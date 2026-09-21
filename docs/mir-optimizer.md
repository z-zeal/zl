# MIR optimiser

The optimiser is the layer that takes a verified `zl::mir::Module` and returns a
verified module that does the same thing with less work in it.

It is deliberately the *last* thing that was built on top of MIR, and it is
deliberately conservative. MIR's job is to be a contract the backends can trust;
an optimiser that is clever before it is correct would break the one property
the whole layer exists for. So the design rule is stated once and applied
everywhere:

> **Where safety cannot be proven, nothing is done.** Every pass counts what it
> declined as well as what it changed. A skipped optimisation is a correct
> program; a wrong one is a miscompile.

The optimiser is stage 5 of the compiler pipeline (see
[pipeline.md](pipeline.md)), directly after MIR verification and directly before
the selected backend: language semantics are already frozen by the time it runs,
so its only job is to make the module cheaper without changing what it observes.
The run paths turn it on (`ZL_MIR_OPT=0` turns it off); the inspection commands
leave it off unless asked for it by name.

```bash
zl --emit-mir-opt out.mir program.zl   # emit the optimised MIR
zl --mir-opt-check program.zl          # optimise and check it is equivalent
zl --mir-vm program.zl                 # run the optimised MIR (the run path)
ZL_MIR_OPT=0 zl --mir-vm program.zl    # ... and run it unoptimised
tools/mir_opt_diff.sh                  # run the corpus both ways, compare
tools/mir_opt_check_all.sh             # check every example and stdlib module
```

---

## Contents

- [The safety contract](#the-safety-contract)
- [Side-effect classification](#side-effect-classification)
- [The framework](#the-framework)
- [The passes](#the-passes)
- [What is deliberately not done](#what-is-deliberately-not-done)
- [Differential validation](#differential-validation)
- [Command line](#command-line)
- [Tests](#tests)

---

## The safety contract

An optimisation is legal here only if it preserves, exactly:

| Property | Why it is easy to break | How it is held |
| --- | --- | --- |
| observable side effects | deleting a `log`, a call, a store | the effect table (`effects.hpp`) must say the instruction is removable |
| evaluation order | moving or merging work | no pass reorders, sinks or hoists anything; they only delete or substitute |
| overflow behaviour | folding `INT64_MAX + 1` into a wrapped value | `foldInstruction` refuses to fold an operation that would raise |
| ownership | dropping a `move`/`borrow`/`drop` | the ownership family is never removable |
| borrowing | moving a use past an `end_borrow` | ownership events are never moved or removed |
| drops | deleting a deterministic release | `Drop` carries `Ownership` |
| exceptions | deleting an instruction because its result is unused | `MayThrow` must be *discharged* before deletion |
| synchronization | CSE-ing a second `atomic_load` | the sync family is `Synchronization`/`VolatileRead`: never removable, never merged |
| native resource lifetime | removing a `handle_close` | the FFI family carries `NativeResource` |
| task behaviour | dropping an `await`, a spawn, a cancel | `TaskEffect`/`Suspension`/`ThreadBoundary` |
| callback behaviour | deduplicating a callback registration | the callback opcodes carry `NativeResource` and `Calls` |

The two subtlest entries are worth expanding, because they are the ones a
textbook optimiser gets wrong in ZL:

**Overflow is an observable effect.** ZL's integer arithmetic *raises* on
overflow, on division by zero and on an out-of-range shift (`VM::binaryArith`).
So `Add` is classified `Pure | MayThrow`, and an instruction is only deleted
once `MayThrow` has been discharged. That happens in three tiers, each a proof
rather than a guess:

1. every operand is a constant, so the shared evaluator decides — and it
   refuses exactly the operations that raise;
2. a constant makes the failure impossible whatever the other operands are:
   `x + 0` cannot overflow, `x / 1` cannot divide by zero, `x << 3` cannot be
   out of range;
3. the operand *types* exclude the failure: an ordered comparison of two
   numbers cannot raise "comparison operators require numbers".

A dead `x * y` is still not deleted, because it can overflow, and deleting it
would delete the program's error.

**Floating point identities are narrowed to the ones that hold for every
value.** `x + 0.0` is *not* rewritten to `x`, because `(-0.0) + 0.0` is `+0.0`
and the sign of zero is observable. `x * 1.0`, `x / 1.0` and `x - 0.0` are,
because they hold for NaN and both zeros alike.

---

## Side-effect classification

`include/zl/mir/effects.hpp` is the single answer to "what does this
instruction do besides compute?". Every pass asks it; no pass has its own
notion of safe.

| Effect | Meaning | Removable if result unused |
| --- | --- | --- |
| `Pure` | computes a value from its operands | yes |
| `ReadsMemory` | reads a slot, field, element or static | yes |
| `WritesMemory` | writes one | no |
| `Allocation` | creates a new GC value | no |
| `RuntimeCheck` | exists to raise: `refine`, `null_check`, handle validity | no |
| `MayThrow` | may leave along an unwind edge | no (until discharged) |
| `ObservableIO` | visible outside the program (`log`) | no |
| `Synchronization` | locks, atomics, channels, semaphores, conditions, shared | no |
| `VolatileRead` | two reads may differ | no |
| `Calls` | transfers control to another function or native | no |
| `TaskEffect` | creates/awaits/blocks/cancels/ignores a task | no |
| `ThreadBoundary` | carries values to another thread | no |
| `Suspension` | parks the async frame | no |
| `NativeResource` | handle/callback/FFI lifecycle | no |
| `Ownership` | `move`/`borrow`/`end_borrow`/`drop` | no |
| `UnwindBarrier` | a scoped lock or cleanup whose release must run | no |

Only `Pure` and `ReadsMemory` are compatible with deletion, and `MayThrow`
must be discharged first. Everything else stays, even when nothing reads its
result, because each of those instructions is a thing the program is *for*
rather than a step towards one.

Two consequences are worth naming because they surprise people:

* **a static's read is a call.** ZL statics are lazily initialised, so
  `StaticLoad` can run the class's initialiser; it is classified with `Calls`.
* **arithmetic is not `Pure`** — see the previous section.

---

## The framework

### Pass manager

`PassManager` (`include/zl/mir/passes.hpp`) owns an ordered list of passes and
runs them. Two kinds exist: `FunctionPass` (once per function — every
optimisation here but one is one) and `ModulePass` (once per module). One
module pass ships: `eliminate-dead-functions`, which removes functions — the
one rewrite whose correctness argument needs the whole module, because it
counts what *nothing* in the module can reach. It only acts when the
reachability report proves the call graph closed (see "Function removal"
below), and the final whole-module verification is its backstop: a module
pass that could not explain every reference it moved fails the pipeline
rather than reaching a backend.

The pipeline runs to a fixpoint: it repeats until an iteration changes nothing,
or `OptimizationOptions::maxIterations` is reached. It has to repeat, because
the last pass feeds the first — a value that becomes constant late turns into a
branch on a constant that was not constant on the way in.

Passes are created **by name** through `PassRegistry`, so a pipeline can be
written down (`ZL_MIR_OPT_PASSES=fold-constants,propagate-copies`) and the
curated ordering lives in exactly one place.

### Ordering, and why

```text
1. eliminate-dead-functions  module-wide, before anything per-function: a
                             function that cannot run needs no pass run over
                             it, and removing it shrinks what every later
                             pass — and the final verification — walks
2. simplify-branches         decided branches, cheapest first
3. eliminate-dead-blocks     prune what (2) stranded
4. fold-constants            intern the constants; the analysis only reports
                             constants the pool already has
5. propagate-constants       carry them across blocks and through phis
6. simplify-algebraic        identities whose operands only just became 0 or 1
7. remove-redundant-conversions
                             conversions that stopped converting
8. propagate-copies          (6) and (7) leave copies behind; dissolve them
9. eliminate-dead-values     everything above replaces *uses*; this removes
                             the definitions that are now unread
```

The iteration matters for (1) in both directions: later passes can strand a
function that was live on the way in (their leftovers are gone next round), and
a pass that deletes a function's only unpinned `call_indirect` can turn a
declined removal into a licensed one.

### Analysis manager

`FunctionAnalysisManager` owns one result per (analysis, function) and hands
out references, so two passes cannot see two versions of the same fact. The
freshness rule is stated once and is the caller's job:

> a pass that mutates a function calls `analyses.invalidate(function)` before
> it asks for another analysis.

Nothing is silently recomputed — recomputing on every query would defeat the
cache, and guessing would be worse. The report carries `analyses built` /
`analyses reused` so the cache can be seen earning its keep.

### Verification between passes

Any pass that reports a change is followed by `verifyFunction`. This is the
framework's central guard:

* the check runs with `unreachableBlocksAreErrors = false`, because simplifying
  a branch legitimately *creates* unreachable blocks for the pruning pass to
  remove, and failing in between would make the two unable to cooperate;
* if verification fails, the function is **restored from the pre-pass copy**
  and the failure is recorded in the report. A broken rewrite never reaches a
  backend;
* a pass that throws is treated the same way — caught, restored, recorded;
* the final `verifyModule` runs with the caller's own options, unrelaxed.

`OptimizationReport::trustworthy()` is the summary: nothing was rolled back,
and the final verification (if one was asked for) passed.

### Snapshots and before/after inspection

Three levels, all opt-in:

* `OptimizationOptions::keepSnapshots` puts the textual MIR from before and
  after each changed pass into the report (`MirSnapshot`).
* `OptimizationOptions::snapshotDirectory` writes
  `<n>-<pass>-<function>.mir` files, each containing the before, the after, the
  verification result and a line diff.
* `ZL_MIR_OPT_VERBOSE=1` prints the per-pass trace to stderr: one line per pass
  per function, with instruction and block counts, the pass's own note, and any
  rollback.

`diffLines(before, after)` is the shared line diff (bounded LCS) used by the
snapshot files and by anything else that needs to show what changed.

---

## The passes

| Pass | What it does | What it refuses |
| --- | --- | --- |
| `eliminate-dead-functions` | deletes functions the reachability report proves unreachable; renumbers the survivors and rewrites every function id the removal moves (`entryPoint`, static initializers, call targets) | every module whose report is not `complete()` — no entry point, a reachable reflection native, or an unpinned function-value call; and any reference it cannot explain — then the module is untouched |
| `simplify-branches` | a branch or switch on a constant becomes a jump; a branch whose arms agree becomes a jump; a jump through an empty block is threaded | a branch whose arms pass different block arguments; a block with parameters, instructions or a handler chain |
| `eliminate-dead-blocks` | removes blocks no edge can enter, counting exception edges — a catch block is live even though nothing falls through to it | — |
| `fold-constants` | evaluates an instruction whose operands are all constants and rewrites the uses of its result | overflow, division/modulo by zero, out-of-range shifts, mixed operand kinds, a result whose type disagrees |
| `propagate-constants` | replaces a use with the constant every path agrees on, including a block parameter whose incoming edges all hand it the same constant | a constant whose type is not exactly the value's type |
| `simplify-algebraic` | `x + 0`, `x * 1`, `x & ~0`, `x - x`, `x ^ x`, `x / 1`, `-(-x)`, `!(!x)`, self-comparisons | anything that could introduce or remove a raise; `x + 0.0` (sign of zero); `x * 0.0` (NaN, infinity) |
| `remove-redundant-conversions` | `widen` of a double, `refine` to the type a value already has, `null_check` on a non-nullable type, `is_null` of one | a check whose operand type is `unknown` or an unsubstituted `T`, or is nullable |
| `propagate-copies` | store-to-load forwarding, a second load of a slot nothing wrote in between, and a block parameter every incoming edge hands the same value | anything needing a fact from outside the block, or from an unreachable predecessor |
| `eliminate-dead-values` | deletes an instruction whose result nothing reads and whose effect set allows deletion, and a store to a slot nothing reads | anything that can raise; any store in a closure body (below) |

None of them reorders, sinks, hoists, merges or duplicates anything. Every
rewrite is either "replace this use with something provably equal" or "delete
this instruction that provably does nothing that can be seen".

### The closure rule

`eliminate-dead-values` will not remove a store in a function that has
captures. A closure body's slots are the closure's *captured environment*, not
this invocation's private locals: ZL captures by value, and a captured `var`
that a closure mutates persists between calls of that closure. A store that
looks dead ("nothing reads this slot again") is the thing that makes
`makeCounter` count. This was found by the runtime differential on
`examples/intermediate/Closures.zl`, and both the unit and the pipeline
regressions now pin it.

### Function removal

`eliminate-dead-functions` is the only place in the optimiser that deletes a
*function*, and the correctness argument is short because it delegates its one
non-trivial step:

> The pass removes a function only when the module's reachability report is
> `complete()`: an entry point exists, no reachable function touches a native
> of the reflection family, and every reachable call through a function value
> is pinned to a single closure body. Under those conditions the reachable set
> is closed under every way MIR records - or the backend materialises - that
> execution can transfer, so a function outside it has no way to be entered,
> invoked, dispatched to, reflected upon, or handed as a value to code that
> calls it.

Each clause of `complete()` earns its place:

* **Entry point.** Without one the module is a library, and its callers are
  outside the analysis's view; "unreachable" functions of a library are its
  API.
* **Reflection natives — the whole family, not only the invoke forms.**
  `Type.methods()` and its siblings hand the program *the function table
  itself* as data, and the bytecode chunk's runtime-type method metadata is
  built from exactly the module's function list: a program that only ever
  enumerates metadata never calls anything it could not have called before,
  yet its printed output changes when a function is deleted. Metadata a
  program prints *is* program output, so a module that reaches any
  reflection native keeps every function it has. That is the price of the
  boundary being honest rather than plausible, and `nativeIsReflective` in
  the catalog is the one list the analysis and the removal both consult -
  neither can drift from what a native does.
* **Function values.** A `call_indirect` whose target is *proven* — a single
  SSA definition, or a slot with exactly one store — to be one
  `make_closure` body is an ordinary edge: that body is live, and anything
  else can still go. A value the analysis cannot pin means some function the
  caller chose may run, and the module keeps everything it has.
* **Static initializers are kept whether or not anything reads them.** Here
  the pass deliberately removes less than the report licenses: reflection
  can read `Class.field` by name, and reading a lazy static runs its
  initializer, so "nobody references this static" is not "nobody can run
  its function." A function that only a kept initializer calls is kept too:
  the pass closes the keep set over every callee reference found in any kept
  body, whatever opcode carries it, rather than trusting any enumeration of
  edge kinds.
* **The backend's own dispatches.** Three MIR instructions name no callee
  yet translate into virtual calls in the bytecode backend: `shared_get`,
  `shared_set` and `Shared.withLock` dispatch the matching `Shared` method;
  growing a `List`/`Set`/`Map` literal is `push`/`add`/`put` on the
  class-layer collection; and the literal's construction is that class's
  empty constructor. `backend_edges.hpp` is the single source for those
  edges — the backend emits through it and the analysis keeps through it.
  A dispatch site the analysis resolves to no candidate, with the name-based
  fallback empty too, makes the report incomplete: a site whose resolution
  the backend would later *refuse* must not look to a removal pass like an
  absence of edges. (This is the same token rule the backend matches by —
  the bare method name from `simpleName`, not the class-qualified declared
  name; the first version of the analysis compared the two spellings, kept
  nothing, and the corpus differential caught it as a stubbed `main`.)

Removal is licensed by `complete()`; everything after that is bookkeeping the
verifier can check: survivors are compacted in place and renumbered, every
`CallTarget::function`, `Module::entryPoint` and `StaticField::initializer` is
rewritten through the computed old→new map, and any reference the keep set
cannot explain makes the pass commit *nothing* and say so. Deleted functions'
bodies also leave the emitted chunk, which is the entire observable change.

Measured with `--artifact-stats` on the benchmark corpus — every example plus
`benchmarks/AllocationBenchmark.zl`, 54 programs, pass on vs the same default
pipeline minus this pass: instruction bytes 17.5 MB → 3.3 MB (**−81.3%**),
chunk function entries 16,399 → 2,996 (−81.7%), the `AllocationBenchmark` and
native `Benchmark` pair alone −92.9% (785,760 → 55,680 bytes). The
optimisation stage itself costs a fifth of what it did (3.07 s → 0.62 s in
total across the corpus) because most of its work had been spent on functions
that could not run. Programs where nothing is licensed to go are unchanged,
by design: `Reflection.zl` at the reflection gate, `Lambdas.zl` and
`Generics.zl` with everything live.

---

## What is deliberately not done

Stated so the absence is a decision rather than an oversight:

* **no dead *store* elimination across paths.** A store is removed only when
  the slot is read nowhere in the function at all. Removing one whose value is
  read on some other path needs an inter-block reaching-definition proof this
  framework does not claim to have.
* **no function removal from a function pass.** A function-local pass cannot
  see the references that would keep a function alive, so the decision lives
  in one module pass, licensed by one report — see "Function removal" above.
* **no inlining, no loop transformation, no vectorisation, no CSE across
  volatile reads, no global value numbering.** Each needs an analysis this
  layer does not have yet, and each can be added as a pass without touching the
  framework.
* **no reordering of any kind**, which is why evaluation order, exception
  order and synchronization order are preserved by construction rather than by
  argument.
* **no other module-level transform.** One module pass ships, for one decision
  with one proof (above). Inlining, SCC-based specialisation and call-graph
  rewrites each need a comparable argument before they earn the module pass
  slot; none has one yet.

---

## Differential validation

"Optimisation" and "the same program" are two claims, and the verifier only
checks the first — a module can obey every MIR rule and still do something
different. So the optimiser is checked twice.

### Statically: `compareModules`

`include/zl/mir/differential.hpp` compares the unoptimised and optimised
modules and reports any difference it can see:

* **structure** — the optimised module's functions must pair with the
  original's *by name*, in order, with the same signatures; the entry point
  must still name the same function (ids may shift, so it is compared by
  name, and renumbering by `eliminate-dead-functions` is invisible to this
  check exactly as long as survivors keep their identity). The pairing is a
  subsequence test: functions may go missing — whether *that* removal was
  licensed is the reachability report's question, and the pipeline's final
  verification confirms no survivor still names a removed one — but nothing
  may be added, moved or renamed; the same classes, statics and counts;
* **no growth** — optimisation may not make the program bigger;
* **no observable event invented** — the optimised module may delete, never
  create. Unconditional, because it is the one thing that is never legitimate;
* **no observable event lost** — compared against the unoptimised module *with
  the default pipeline run over it*, so that deleting code which cannot run is
  not reported as a divergence while deleting code that can is;
* **no slot write invented** — a slot write may be removed, never added.

The "not lost" rule is measured against a reference built by **the same
pipeline** (`DifferentialOptions::referencePipeline`). Judge a module built by
one pass against a reference built by eight and every event the other seven
would have deleted is reported as a divergence — which would be a statement
about the comparison, not about the module. This is why `--mir-opt-check`
reports equivalence for `ZL_MIR_OPT_PASSES=fold-constants` as readily as for
the default pipeline.

An *observable event* is an instruction whose effect set contains something the
outside world, another thread, or the language's lifetime discipline can see:
output, calls, heap writes, task and thread operations, synchronization,
volatile reads, native-resource events, ownership events, suspension, and
raises. Two families are deliberately excluded, and both are covered by
running the program instead:

* **runtime checks** (`refine`, `null_check`, `field_load`). The optimiser may
  delete one it has proved cannot fail, so counting them would make every
  correct removal look like a divergence.
* **writes to local slots**. They are observable only through a later read of
  that slot, and deciding whether a given store can be observed is the same
  work the optimiser does to remove it — counting them would make the
  comparison either blind or circular.

The comparison also cannot see **values**: rewriting `x + 0` to the wrong value
changes no event at all. That is what the runtime half is for.

### At run time: `tools/mir_opt_diff.sh`

The corpus harness lowers every program under `examples/` twice — once as
lowered, once optimised — translates both to bytecode, executes both, and
compares their output and exit status byte for byte. A program that behaves
differently is a miscompile, and the harness fails rather than reporting a
statistic. The same check runs inside
`tests/mir_opt_pipeline_tests.cpp`, where the VM's output is captured and
compared in-process.

Current state: **52 of 52 example programs identical**, 0 skipped — run after
the module pass joined the default pipeline, so it is also the runtime proof
that function removal changes nothing a program can observe. The files under
an `examples/*/_lib/` directory are importable module sources rather than
runnable programs and are not counted.

### Across the whole corpus: `tools/mir_opt_check_all.sh`

The runtime harness is the strongest check there is and the narrowest, because
it can only cover programs the MIR bytecode backend can execute — and much of
the most interesting code in the language is not in that set. The stdlib is
full of generics, `async`, tasks, locks, atomics and native calls whose
functions the backend currently stubs.

`tools/mir_opt_check_all.sh` needs no backend: for every `.zl` under
`examples/` and `stdlib/` it runs `--mir-opt-check` and reports any file whose
optimised form is not equivalent to its unoptimised one. It is the wide check
where the runtime harness is the deep one, and neither substitutes for the
other — this one cannot see values, and that one cannot reach this one's code.
A file that will not load on its own is skipped and listed, not failed: it says
nothing about the optimiser.

Current state: **76 of 76 equivalent** (50 examples, 26 stdlib modules), 0
divergent, 0 skipped.

---

## Command line

```bash
zl --emit-mir-opt out.mir program.zl   # write the optimised MIR
zl --emit-mir-opt - program.zl         # print it
zl --mir-opt-check program.zl          # optimise and diff; exit 4 on divergence
ZL_MIR_OPT=0 zl --mir-vm program.zl    # ... and run it unoptimised
```

Environment:

| Variable | Effect |
| --- | --- |
| `ZL_MIR_OPT_PASSES` | pipeline spec: `default`, `none`, or a comma-separated list of pass names |
| `ZL_MIR_OPT_SNAPSHOT_DIR` | write one before/after snapshot per changed pass into this directory |
| `ZL_MIR_OPT_VERBOSE` | print the per-pass trace to stderr |
| `ZL_MIR_OPT` | the optimisation stage: on for the run paths, off for the inspection commands; `=0`/`=1` override either way |

Exit codes follow the other MIR commands: `0` success, `2` bad usage, `3`
stdlib version mismatch, `4` verification failed or the module was not
observationally equivalent, `5` output could not be written, `1` front-end
failure.

---

## Tests

- `tests/mir_pass_tests.cpp` (`zl-mir-opt-tests`) — 146 checks, MIR built by
  hand, no front end. The effect table; the discharge rules (an `add` that
  cannot raise becomes removable, one that can does not); the evaluator's
  refusals (overflow, division by zero, `INT64_MIN / -1`, out-of-range shifts,
  mixed kinds); each pass's rewrite and each pass's declinations; the closure
  store rule; pipeline ordering and the registry; rollback of a pass that
  corrupts the MIR; analysis caching and invalidation; snapshots and the diff;
  and the differential validator catching a deleted `log` and a changed
  signature while accepting a real optimisation — and the reference-pipeline
  rule from both sides: a module built by one pass is equivalent against a
  reference built the same way, and divergent against one built by all eight.
- `tests/mir_opt_pipeline_tests.cpp` (`zl-mir-opt-pipeline-tests`) — 79 checks
  over real ZL programs: lower → verify → optimise → verify → static
  differential → **run both versions and compare their output and exit code**.
  The programs cover folding, decided branches, identities on non-constant
  values, loops, strings and static calls, ownership, a closure that mutates a
  capture, a division by zero that must still raise, and a program the
  optimiser has nothing to say about.
- `tools/mir_opt_diff.sh` — the runtime corpus harness described above.
- `tools/mir_opt_check_all.sh` — the static corpus sweep: every example and
  every stdlib module checked with `--mir-opt-check`.

A note on the library entry points: `optimizeModule(module, options)` is the
one-call form; `PassManager::defaultPipeline()` / `namedPipeline(spec, error)`
give explicit control; `compareModules(before, after)` is the static
differential. All three are usable from a backend the same way `--mir-vm` uses
them.
