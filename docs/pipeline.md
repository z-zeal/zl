# The compiler pipeline

MIR is ZL's compiler boundary. This document describes the pipeline that puts it
there: the stages, what each one guarantees to the next, what a backend is
allowed to assume, and how the compiler can change its backend without changing
what a ZL program means.

```text
ZL source
  │
  ├─ 1. load              lexer → parser → ModuleLoader        → Program
  ├─ 2. semantic analysis TypeChecker                          → recorded types
  ├─ 3. typed lowering    AST + recorded types                 → MIR
  ├─ 4. MIR verification  verifyModule                          → the contract
  ├─ 5. MIR optimisation  PassManager (run paths: on by default) → MIR, still verified
  └─ 6. code generation   bytecode backend | native backend    → Chunk | native IR + code
                                    │                                    │
                                    ▼                                    ▼
                              stack VM (executes)              machine code (AOT artifact)
```

Implementation: `include/zl/compiler/pipeline.hpp`, `src/compiler/pipeline.cpp`.
Every command in `src/main.cpp` is that diagram with different stage options -
they no longer each hand-roll "load, check, lower, verify, ...", which is what
made the pipeline a convention rather than a component.

- [Why the boundary is MIR](#why-the-boundary-is-mir)
- [Stages and their invariants](#stages-and-their-invariants)
- [Backends](#backends)
- [Choosing a backend](#choosing-a-backend)
- [Compatibility with the VM](#compatibility-with-the-vm)
- [Command reference](#command-reference)
- [Exit codes](#exit-codes)
- [What moved out of the backends](#what-moved-out-of-the-backends)
- [Checks](#checks)

## Why the boundary is MIR

A compiler has exactly one place where language semantics stop and
target-specific representation begins. Everything above that line answers "what
does this program mean"; everything below answers "what do these bytes do". If
the line is in two places, the two answers drift, and a program's meaning
depends on which backend compiled it - which is the one thing a compiler must
never do.

For ZL the line is MIR:

| Question | Answered by | Recorded in |
| --- | --- | --- |
| What does this expression mean? Type, overload, visibility, mutability, ownership, capture, dispatch target | the AST + `TypeChecker` | MIR operands, instructions and function metadata |
| What shape does control flow have? | typed lowering | blocks, terminators, edge arguments, handler chains |
| How is it executed? | a backend | `zl::Chunk` or native IR → machine code |

Three rules follow, and they are the rules a change to this compiler is
expected to keep:

1. **A backend reads MIR and nothing else.** No AST, no type checker, no
   re-derivation of "what did this expression mean". If a backend needs a fact,
   the fix is to put the fact in MIR.
2. **The lowerer does no inference of its own.** Every type it writes comes from
   `TypeChecker::expressionType` - the same record the checker used to accept
   the program. A second opinion about types is a second type system.
3. **A fact a backend needs but MIR lacks is a MIR bug**, not a backend
   workaround. The verifier (`docs/mir.md`) is what makes rule 1 safe: a backend
   may assume MIR's invariants because something checked them, and code
   generation re-checks immediately before it consumes the module.

The rules are checked mechanically, not just written down:
`tools/boundary_lint.sh` reads the tree and fails if a backend includes the AST,
the parser, the type checker, the module loader, the pipeline, or the legacy IR
- directly, *or transitively*, through a permitted header that later starts
reaching one (the check walks each backend file's include graph to a fixpoint
and names the chain that reached the forbidden header); if the lowerer touches
the checker except through `expressionType`; if anything outside the pipeline
constructs a `ModuleLoader` or a `TypeChecker` - scanned across the whole
production source tree, in every spelling (qualified, unqualified, `new`,
`make_unique`, aliases, line-broken), with the pipeline driver as the single
explicit allowlist entry; or if the legacy `zl::ir` gains a second consumer.
It is registered with CTest as `boundary-lint`, with the lint's own
regressions (`boundary-lint-regressions`) proving each rule still fails on the
broken trees it exists for, so a violation fails the build rather than
surviving review.

The include graph is kept honest structurally as well as by lint: `zl/common/`
is audited to include nothing frontend, the shared operator table
(`zl::OperatorRules`) is keyed by `Operator` rather than by a lexer token, and
the frontend-facing lowering seam (`zl/mir/lowering.hpp`) is the only MIR
header that includes the AST or the checker - so a backend that pulled in the
front end would do so visibly, not through a header that looked safe.

## Stages and their invariants

`zl::pipeline::Stage` is the ordered list below; the pipeline refuses to run a
stage out of turn (`ErrorKind::Invariant`), because each stage's postcondition is
the next stage's precondition. `--pipeline-report` prints the invariants
verbatim; `zl::pipeline::stageInvariant` is the single source of truth for them.

| # | Stage | Runs | Invariant (what the next stage may assume) |
| --- | --- | --- | --- |
| 1 | load | always | the program is the complete merged translation unit: every declaration of the entry file and of every transitively imported file, exactly once |
| 2 | semantic analysis | always | the program is a well-formed ZL program, and the checker's recorded expression types are the only source of type truth downstream |
| 3 | typed lowering | always | every source function is present as MIR; a function that cannot be represented is marked incomplete rather than guessed at |
| 4 | MIR verification | always | the module satisfies every MIR invariant (docs/mir.md), which is exactly what entitles a backend to assume them without re-checking |
| 5 | MIR optimisation | run paths by default (`ZL_MIR_OPT=0` off); inspect commands only when asked | the module still verifies and still observes the same events; a pass that breaks either is rolled back and the stage fails |
| 6 | code generation | unless the command stops at MIR | the selected backend produced its artifact from this exact verified module, and re-derived no language semantics from the AST |

Details worth knowing:

- **Stage 3 is where the MIR *form* is chosen.** `promoteSlots` (`ZL_MIR_PROMOTE=1`)
  turns mutable locals into block parameters, and it happens inside stage 3 so
  that stage 4 verifies exactly the form the backend will receive. Both forms
  mean the same thing, and every backend accepts either.
- **The optimiser is a stage with a switch, and the switch is off only when you
  say so.** The run paths (`zl file.zl`, `--mir-vm`, `--backend native`,
  `--run-native`, `--pipeline-report`) optimise by default, because the stage is what makes the
  MIR boundary worth having; `ZL_MIR_OPT=0` turns it off. The inspection
  commands (`--emit-mir`, `--emit-ssa`, `--emit-native-ir`, `--safety-check`)
  leave it off unless asked, because their output is a snapshot of lowering
  rather than of the run path. Its passes are proven observationally equivalent
  by differential harnesses (see [Checks](#checks)), which is how the default
  was earned: a harness can only measure the stage by running the program both
  ways, and it would not be trusted if it were never on.
- **Stage 6 re-verifies.** With `verifyBeforeCodegen` on (the default), the
  module is verified again immediately before a backend sees it. A module mutated
  between stages 4 and 6 - by a pass, by a future stage, by a bug - is caught at
  the boundary instead of by a backend noticing something odd later. With `verify`
  disabled and this off, code generation *refuses*: there is no path from a source
  file to a backend that skips verification.
- **A skipped stage is still recorded.** The ledger says `-- MIR optimization ok
  (skipped) skipped (opt-in: ...)` rather than omitting the line, so "the
  optimiser did not run" is visible instead of being an absence.
- **Failures are data.** Each stage records a `Failure { kind, stage, message,
  detail }`; the kind selects the message label and the exit code, so the CLI
  reports a MIR failure as a MIR failure and never charges a step it did not
  take. A stage that fails closes its own ledger line (`FAIL <message>`), so a
  report shows where the pipeline stopped instead of stopping between two
  lines.

## Backends

A backend's entire job is: **translate a verified `zl::mir::Module` into a
target-specific execution format, and refuse rather than guess.** Two are
implemented.

| | bytecode | native |
| --- | --- | --- |
| Entry point | `compileModuleToBytecode` (`include/zl/mir/vm_backend.hpp`) | `compileMirToNative` (`include/zl/native/pipeline.hpp`) |
| Produces | `zl::Chunk` for the stack VM | native IR, then x86-64 machine code |
| Coverage | the whole language | a deliberately small, named subset |
| Refusal | a function that cannot be translated is *stubbed*: its entry points at a stub that raises if reached, and the reason is carried out to the caller | a function is rejected *by name and reason*; the ledger accounts for every function in the module |
| Used by | `zl file.zl`, `--mir-vm`, `--emit-mir*` | `--emit-native-ir`, `--emit-native-code`, `--emit-machine-code`, `--run-native`, `--backend native` |
| Executed by | the VM (`zl::VM::run`) | the VM for whole programs; `--run-native` executes compiled functions directly (`docs/native-backend.md`) |

Neither backend is privileged by the language. `Backend` is an enumerator plus
an arm in `Pipeline::generate`; a third backend would be another enumerator, and
nothing above the boundary would move.

### What a backend may assume

Everything in `docs/mir.md`, checked by the verifier: types on every value,
interned type ids, SSA temps defined once and dominating their uses, exactly one
terminator per block, edge arguments matching block parameters, exception edges
distinct from normal edges, ownership/lifetime events explicit, and every
dynamic-to-static boundary passed through `refine`. A backend that has to ask
the AST "is this really an int here?" has found a hole in MIR, not a shortcut.

### What a backend must not do

- Read the AST, the type checker, or the source file.
- Invent a type, a dispatch target, a field offset, or an ownership event that
  MIR did not state.
- Silently skip an operation it does not implement. Both backends fail closed:
  the bytecode backend raises at runtime if a stubbed function is reached, and
  the native backend refuses the function and says why.

### What a program can run

`zl::mir::reachableFunctions` (`include/zl/mir/reachability.hpp`) answers which
functions a program can enter, from the same verified MIR the backends get. It is
an over-approximation on purpose, and the direction is the point:

- virtual dispatch keeps every override in the receiver's hierarchy, and every
  implementer when the receiver's type is an interface;
- a dispatch site naming a class or method the module does not have keeps every
  function with that method name, rather than reading an absence of evidence as
  evidence of absence;
- a static field's initializer is kept when the field is referenced, because
  statics initialise lazily, on first access.

So a function it calls unreachable really is unreachable, which is what makes the
answer usable for anything that would *remove* code. There are two cases where it
refuses to be exact, and in both `ReachabilityReport::complete()` is false,
because the reachable set is then a *lower bound* and not removal-safe:

- **Reflection**: the whole reflection family, not just its invoke forms.
  `Reflection.methodInvoke` (and the constructor and function forms) calls
  whatever a `Method`/`Function`/`Constructor` value describes, so a program
  that reaches one has no closed call graph; and `Type.methods()` and the
  other enumeration natives *read the function table itself*, which the
  emitted chunk renders into runtime type metadata - so their output is built
  from the very list a removal would edit, and metadata a program prints is
  program output. A module that reaches any reflection native keeps every
  function it has. `dynamicEntryReason` names the native that opened the
  graph. Which natives those are is `nativeIsReflective` in the catalog -
  the catalog owns what a native does, so a caller cannot drift from it with
  a hand-written list.
- **Unresolved function values**: `call_indirect` (and `Task.spawn`,
  `Thread.start`, the `withLock` family) executes a value, not a named target.
  The analysis proves the edge to a single body only from SSA structure - a
  single definition, possibly through a slot with exactly one store. A value it
  cannot pin (a function-valued parameter, a block parameter, a static, a slot
  with zero or multiple stores) may hold any function the caller chose, so the
  edge is recorded as unresolved instead of dropped or guessed:
  `unresolvedCalls` is set, the report counts every such site, and
  `unresolvedCallReason` names the first. A later function-value analysis can
  shrink the unresolved set by proving more values to be single closures; it
  cannot change what `complete()` means - it is true only when every possible
  execution edge relevant to the module has been statically accounted for.

The analysis is a question, not a transform; the transform is
`eliminate-dead-functions` in the MIR optimiser, which deletes only under the
report's completeness proof (docs/mir-optimizer.md, "Function removal").
`--backend native` still reports the answer rather than acting on it - the
numbers below are a one-line hello-world, which really does enter exactly one
function:

```text
native: 5 function(s) compiled, 283 left to the VM
  reachable: 1 of 288 function(s) can run, from HelloWorld.main()
```

- because the ledger's totals are about the whole module and most of a module is
  stdlib the program never calls. `--pipeline-report` prints the same line.

## Choosing a backend

```bash
zl program.zl                        # the shipped default: bytecode backend
zl --backend bytecode program.zl     # explicit
zl --backend native program.zl       # native code generation, VM execution
ZL_BACKEND=native zl program.zl      # same, without touching the command line
zl --pipeline-report program.zl      # the stage ledger and the MIR digest
```

An unknown backend name is a usage error (exit 2), never a silent fallback:
compiling with something other than what was asked for is exactly the class of
surprise this boundary exists to remove.

**The guarantee.** Selecting a backend is a code-generation decision and nothing
else. The pipeline builds the MIR module without consulting the backend, so both
backends are handed byte-identical input; `--pipeline-report` compiles the
program once per backend and prints both digests, refusing to continue if they
differ:

```text
backend parity
  bytecode: 54a1e0962d58f173
  native:   54a1e0962d58f173
  identical: the backend choice is a code-generation decision only
```

and `tools/backend_diff.sh` runs the whole example corpus three ways - reference
compiler, bytecode backend, native backend - and compares what each run printed
and exited with. It is currently 51 of 51 identical.

## Compatibility with the VM

The VM is the behavioural reference for the language, and this phase does not
change it or the bytecode it executes. Three mechanisms keep that compatibility
honest:

1. **The execution driver.** `Execution::Vm` runs the bytecode translation of the
   *same verified MIR* whatever backend generated code, because no native
   execution driver exists yet. `--backend native` therefore reports
   `execution: VM (the native tier is a code generator; mixed-mode native
   execution is not implemented yet)` and runs the program the same way. When a
   mixed-mode runtime lands, it changes the driver, not the language.
2. **The reference path.** The original AST → bytecode compiler is kept,
   unchanged, behind `ZL_COMPILER=ast` (or `--reference-compiler`). It is the
   other half of every backend differential harness. It shares stages 1-2 with
   the pipeline and diverges at code generation, which is precisely the
   difference under measurement.
3. **Fail-closed refusals.** Where MIR is richer than a backend can express, the
   function is refused by name, never mis-translated. A stubbed function raises
   loudly if it is ever reached; a native refusal is listed in the ledger with
   its reason. Both are reported on the run path, not buried in a log.

## Command reference

| Command | Stages | Notes |
| --- | --- | --- |
| `zl file.zl` | 1-6, execute | default path; optimiser on, bytecode backend |
| `zl --backend native file.zl` | 1-6, execute | native code generated for the subset; VM executes |
| `zl --backend native --strict-native f.zl` | 1-6 | fails unless *every* function compiled natively |
| `zl --reference-compiler file.zl` / `ZL_COMPILER=ast` | 1-2 + reference compiler | the differential escape hatch, not the pipeline |
| `zl --check file.zl` | 1-2 | stops after semantic analysis; nothing is generated |
| `zl --pipeline-report file.zl` | 1-6, both backends | ledger, invariants, MIR digest parity; requires a program (a valid `main`) - a library is refused with the entry-point diagnostic, because the report describes a runnable program's pipeline |
| `zl --mir-vm file.zl` | 1-6, execute | the same pipeline as the default path, with the ledger on |
| `zl --emit-mir <out\|-> f.zl` | 1-4 | the lowered module, before optimisation |
| `zl --emit-ssa <out\|-> f.zl` | 1-4 | the same module in its SSA form; re-verified after promotion |
| `zl --emit-mir-opt <out\|-> f.zl` | 1-5 | after optimisation |
| `zl --mir-opt-check f.zl` | 1-5 | optimise + `compareModules`; exit 4 on divergence |
| `zl --emit-native-ir <out\|-> f.zl` | 1-4 + selection | native IR and the per-function ledger |
| `zl --emit-native-code <out\|-> f.zl` | 1-6 | native IR and machine code |
| `zl --emit-machine-code out.zlm f.zl` | 1-6 | the `ZLM1` container, now written from MIR |
| `zl --safety-check f.zl` | 1-4 | JSON report; never generates code, never executes |
| `zl --emit-native out.cpp f.zl` | legacy | the last AST → `zl::ir` consumer; see below |
| `zl --parse-only f.zl` | lexer + parser | syntax only |

`--strict-native` (`ZL_NATIVE_STRICT=1`) turns `--backend native` into an
all-or-nothing check: the pipeline fails rather than report a function it left to
the VM. It is off by default because the native subset is deliberately partial, so
even a hello-world has stdlib functions outside it; it is what a program written
*to* the subset uses to keep it that way.

Environment switches: `ZL_BACKEND`, `ZL_MIR_OPT`, `ZL_MIR_OPT_PASSES`,
`ZL_MIR_OPT_CHECK`, `ZL_MIR_OPT_SNAPSHOT_DIR`, `ZL_MIR_OPT_VERBOSE`,
`ZL_MIR_PROMOTE`, `ZL_MIR_SSA_VERBOSE`, `ZL_NATIVE_STRICT`, `ZL_PIPELINE_VERBOSE`,
`ZL_COMPILER`, plus the module search roots `ZL_EXTRA_ROOTS`, `ZL_STDLIB_ROOT`,
`ZL_HOME`.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | front end, environment or compiler-invariant failure (syntax, module, type, internal) |
| 2 | usage error: an unknown command, flag, or backend name |
| 3 | stdlib version mismatch: the bundled stdlib was built for a different runtime |
| 4 | MIR verification failed, the optimiser produced something it could not trust, or the selected backend refused |
| 5 | the requested output file could not be written |
| 6 | unsupported: lowering did not complete (partial MIR is not a certificate) |
| 7 | reserved for the safety report's unclassified/unloadable cases |

`--safety-check` keeps its own mapping on top of these, because a research
harness has to distinguish "rejected by the parser" from "rejected by the MIR
contract" in the JSON it emits; see `docs/mir-safety.md`.

## What moved out of the backends

This phase moved semantic lowering *out* of the places that duplicated it:

- **`Compiler` (AST → bytecode)** no longer lowers the whole program into the
  legacy `zl::ir` and throws the result away on every compile. That call was a
  second semantic lowering with no consumer; deleting it also deletes the
  default path's last reason to know `zl::ir` exists.
- **`src/main.cpp`** no longer re-implements the front end eight times. Every
  command is now a set of stage options; stdlib-version checking, root assembly,
  optimiser configuration, the differential check and failure reporting live in
  one place.
- **`--emit-machine-code`** no longer goes AST → `zl::ir` → x86-64. It writes the
  same `ZLM1` container from the MIR native backend, so the last CLI consumer of
  the legacy machine-code path is gone.
- **`--mir-vm`, `--emit-mir`, `--emit-ssa`, `--emit-mir-opt`, `--mir-opt-check`,
  `--emit-native-ir/-code`, `--safety-check`, `--check`** all share stages 1-4
  with the shipped run path instead of each driving `ModuleLoader`, `TypeChecker`,
  `lowerProgram` and `verifyModule` by hand.

What remains, and why:

- **`zl::ir` + `src/compiler/ir_lowering.cpp` + `--emit-native`.** The portable
  C++ emitter for the restricted `@native` subset still lowers the AST directly.
  It is a single, quarantined consumer: it is not grown, it is not on the default
  path, and it prints a note saying so. Porting that emitter to consume MIR is a
  self-contained follow-up; until then the honest statement is "one legacy path
  remains", not "MIR is used everywhere".
- **`zl::machine::emitX64`** stays as a frozen encoder. It no longer has a CLI
  consumer.

## Checks

Every claim above is executed by a test or a harness, and the count changes are
reported rather than described as "passing".

| Check | What it proves |
| --- | --- |
| `zl-pipeline-tests` (225 checks) | stage order and the ledger; code generation refuses an unverified or mutated module; backend selection leaves MIR byte-identical and behaviour unchanged (reference / bytecode / native); the opt-in stages are invisible when on and recorded when off; failure kinds map to exit codes |
| `zl-mir-tests`, `zl-mir-ssa-tests`, `zl-mir-ownership-tests`, `zl-mir-type-tests` | the MIR contract itself: malformed modules are rejected, legal ones accepted |
| `zl-mir-lowering-tests` | stages 1-4 over real ZL programs produce the MIR the language means |
| `zl-mir-opt-tests`, `zl-mir-opt-pipeline-tests` | the optimiser, per pass and end to end, including the runtime differential |
| `zl-native-backend-tests` | the native backend executes emitted machine code and agrees with the VM |
| `tools/mir_backend_diff.sh` | reference AST → bytecode vs the MIR pipeline, on stdout and exit status: **51 of 51 identical** |
| `tools/backend_diff.sh` | reference vs bytecode backend vs native backend, plus MIR-digest parity per program: **51 of 51 identical** |
| `tools/mir_opt_diff.sh` | optimised vs unoptimised, run both ways: **51 of 51 identical** |
| `tools/mir_promotion_diff.sh` | memory form vs value form, run both ways: **51 of 51 identical** |
| `tools/mir_opt_check_all.sh` | optimised vs unoptimised *statically*, over `examples/` and `stdlib/` |
| `tools/boundary_lint.sh` (CTest `boundary-lint`, `boundary-lint-regressions`) | rules 1-4 above: no backend sees the AST (directly or through the include graph), the lowerer only reads recorded types, the front end is constructed in exactly one place (scanned across the production tree), the legacy IR keeps one consumer |
| `tools/native_demo.sh` | one program through source → MIR → native IR → disassembly, calling the emitted code and diffing against the VM |
