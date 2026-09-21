# MIR

MIR is ZL's mid-level intermediate representation: a typed, SSA-shaped graph
that sits between semantic analysis and any backend. It is the layer a native
compiler, an optimiser, or a static analyser consumes instead of re-deriving
structure from the AST or from bytecode - and, since the pipeline phase, it is
**the compiler boundary**: language semantics are decided above it and
target-specific code generation happens below it.

```
.zl → Lexer → Parser → ModuleLoader → TypeChecker
                                          │
                                          ▼
                                   typed lowering
                                          │
                                          ▼
                                      MIR  ← the compiler boundary
                                          │
                                          ▼
                                    verifyModule
                                          │
                                          ▼
                                  optimiser (run paths: on)
                                          │
                     ┌────────────────────┴────────────────────┐
                     ▼                                         ▼
          bytecode backend → Chunk → VM            native backend → machine code
```

That diagram is the whole compiler, not a side path: `zl file.zl` builds MIR,
verifies it, and translates it with the bytecode backend. The reference
AST → bytecode compiler still exists, unchanged, behind `ZL_COMPILER=ast`, so
the two paths can be differentially tested against each other. The stages, their
invariants and the backend contract are documented in
[pipeline.md](pipeline.md).

Headers live in `include/zl/mir/`, implementations in `src/mir/`.
`include/zl/mir/mir.hpp` pulls the whole layer in, ordered by dependency.

The VM and its bytecode are untouched and remain the behavioural reference.
Nothing in the language changed to accommodate MIR.

---

## Two IRs, and why

ZL already had `zl::ir` (`include/zl/compiler/ir.hpp`). It is now a legacy layer
with exactly one remaining consumer, the portable-C++ emitter behind
`--emit-native` (the restricted `@native` subset). It is frozen, not grown, and
it is not on any other path: `--emit-machine-code` produces the same `ZLM1`
container from the MIR native backend, and the default compiler no longer lowers
the program into it at all. MIR is a different layer with a different job, and
the two coexist deliberately:

| | `zl::ir` (legacy) | `zl::mir` |
|---|---|---|
| Types | `std::string` names | interned `TypeId`s in a `TypeArena` |
| Form | mutable locals, reassignable results | SSA temps plus explicit slots |
| Terminators | mixed into `instructions` | a distinct type, exactly one per block |
| Successors | a vector separate from the terminator | derived from the terminator |
| Generics / unions / function types | not representable | first-class `TypeKind`s |
| Exceptions | not representable | unwind edges and handler chains |
| Fields, indexing, methods, allocation | absent | dedicated opcodes |

`zl::ir` has not been renamed — "MIR" already appears in its diagnostics, and
renaming it would churn user-visible strings for no gain. When a construct only
the native subset needs has to be added, add it to `zl::mir` and lower `zl::ir`
onto it later; do not grow `zl::ir` further.

---

## Core structures

**`Module`** owns everything: the `TypeArena`, the functions, the statics, the
class layouts, the constant pool, and the entry point. The arena must outlive
every `Function` that references its ids.

**`TypeArena`** interns types structurally, so a type *is* its id. Equal shapes
share an id; different shapes never do. Interning is memoisation, so the
accessors are `const` over mutable storage — which means an arena is **not**
safe to intern into from two threads at once.

**`Function`** holds parameters, slots, blocks, and the metadata a backend
needs: `returnType`, `isAsync`, `isLambda`, `typeParameters`, `ownerClass`,
`hasThisParameter`, and `incomplete`.

**`BasicBlock`** holds value-producing instructions and exactly one
`Terminator`. `Instruction` and `Terminator` are separate types; that separation
is the single most load-bearing decision in the design (see
[Invariants](#invariants)).

**`Operand`** is a typed reference to a value: `{kind, type, index}`, where
`kind` is one of `None`, `Const`, `Temp`, `Param`, `Static`. Every non-`None`
operand carries a `TypeId`, so no consumer ever has to ask "what type is this?"
and re-derive it from context.

---

## Invariants

The verifier enforces all of these. They are the contract a backend may rely on.

### Types

1. **Every value has a type.** Operands, temps, parameters, slots, and
   instruction results all carry a `TypeId`. `kNoType` (0) means "absent", and
   appears only where a value legitimately is (a void call, a bare `return`).
2. **Types are interned.** Two `TypeId`s compare equal exactly when the types are
   structurally equal. A backend may use `==` on ids and never inspect shapes.
3. **Nullability is derived, never stored.** A stored flag would be a copy of
   an invariant and could disagree with the type. It is answered by
   `TypeArena::isNullable(id)`, not by the top-level kind alone: `string` is
   nullable even though the VM holds it inline rather than behind a collector
   handle, and a union is nullable when any member is, so `int|string` admits
   `null` while `int|double` does not.
4. **A generic parameter is a `TypeParam`, not an erasure.** `T` inside
   `class Set<T>` is a real type with a name. Nothing is boxed to `object` to
   make the first implementation simpler.
5. **`name` is set for every nominal type** — `Object`, `Task`, `Shared`,
   `Option`, `Result`, `TypeParam`. Rendering never needed it (the kind supplies
   `"Task"`), but every consumer asking "which class is this?" does.
6. **An array's size is optional.** `array<int>` is dynamic; `array[10]<int>` is
   fixed. Interning must not normalise the former into the latter.
7. **`Option<T>` and `Result<T,E>` are kinds of their own.** A declared
   `Option<int>` is the `Option` kind carrying the `int` payload type id, not an
   `Object` that happens to be named "Option", so a backend can see "a payload
   or nothing" without string-matching class names. The runtime *classes* the
   language constructs — `Some<T>`, `None<T>`, `Ok<T,E>`, `Err<T,E>` — stay
   Object kinds, and relate to the sum kinds through `isOptionType` /
   `isResultType` / `optionPayloadFor` / `resultPartsFor` and the assignability
   rule: `Some<int>` satisfies `Option<int>`, `Some<string>` does not, and the
   relation composes under arguments (`Some<List<int>>` satisfies
   `Option<list<int>>`). Native/resource semantics are not a type kind at all:
   ZL has no separate static native type, so FFI values are typed by their
   declared renders (a `CallNative` result is an ordinary type) and resource
   behaviour is carried by slot/parameter *ownership* (`GC`/`OWNED`/`BORROW`)
   plus the move/borrow/drop dataflow, which the verifier checks.
8. **A dynamic value crosses into typed territory only through `Refine`.**
   Every boundary where semantic analysis left a value `unknown` and the
   destination is a concrete type — a typed local or assignment, a call
   argument, a return, a field/element write — lowers to an explicit
   `refine` (the runtime type assertion), so the graph reads
   `dynamic value → refine → statically typed value` and the verifier *rejects*
   any `unknown` operand that reaches a typed destination without one. This is
   what keeps an invalid type assumption detectable at the exact boundary
   rather than silently trusted everywhere downstream.
9. **Unions are ordinary types everywhere a type can appear.** A function may
   declare `int|string` as its return type; the verifier checks each return
   operand against the union, and callers narrow members back out with
   `type_test`/`refine`. Nothing forces a union-returning function to lose its
   return type on the way into MIR.

### SSA and storage

7. **A temp is defined exactly once.** The verifier collects every definition
   and rejects a second one.
8. **A temp is dominated by its definition.** Uses are checked against the
   dominator tree, not against textual order.
9. **Mutable locals are slots, not temps.** `Load`/`Store` name a `SlotId`. This
   is what keeps SSA and ZL's reassignment semantics from fighting each other.
   A slot is the *memory* form of a value, and MIR is valid with either that
   form or the value form (9a) in use — they mean the same thing, and
   `promoteSlotsToBlockParameters` converts the first into the second where it is
   provably safe.
9a. **A block parameter is the value form of a merge (the phi).** A
   `BasicBlock::parameters` entry is defined once, on entry to its block, and the
   *edge* supplies it: `Terminator::edgeArguments[i]` holds one operand per
   parameter of successor `i`. This is the block-argument spelling of phi
   placement, chosen over a list of `(value, predecessor)` pairs on the use side
   because the merged value then has one definition and its own name.
   `if c { x = 10 } else { x = 20 }` followed by a use of `x` is a block
   parameter at the join taking `10` on one edge and `20` on the other.
9b. **A block parameter is a value like any other.** Value space is
   `ValueId` = parameter | block parameter | temp (`include/zl/mir/value.hpp`),
   so def-use, liveness and constant analysis work over one kind of thing rather
   than three. A use of a block parameter must be dominated by its block, and
   every *normal* predecessor must supply one argument per parameter, of the
   parameter's type. Unwind edges supply none: a catch block is entered by an
   exception, not by a decision about what to pass it.
9c. **The entry block has no parameters** — nothing can hand it a value.
10. **Parameters stay SSA unless the body writes, moves, or borrows them.** A
    read-only parameter is a `Param` operand. Promoting every parameter to a
    slot would force `this` — and every read-only binding — through storage that
    has to be *mutable* to exist at all, contradicting the binding's own
    immutability.
11. **A `let` that is never moved is an SSA value**, bound directly to the
    operand its initialiser produced. Giving it a slot would mean storing into
    storage the language says can never be written.
12. **A borrow has no value of its own.** `Borrow` writes to a slot; it does not
    produce a temp. A borrow that produced a temp could not be identified by the
    ownership flow, so dropping one would be undetectable.

### Control flow

13. **`blocks[0]` is the entry block**, and block ids are 1-based and match
    position. `rebuildEdges()` maintains that.
14. **Every block ends in exactly one terminator.** `TerminatorKind::None` is
    rejected. Value-producing instructions never transfer control, and
    terminators never produce values.
15. **Every block is reachable.** Unreachable blocks are an error by default;
    `VerifierOptions::unreachableBlocksAreErrors` demotes them to warnings for
    hand-written MIR under construction.
16. **Predecessor relationships are consistent.** A block named as a successor
    must list the edge, and vice versa. The verifier checks both directions.
17. **The entry block has no predecessors.**
17a. **Control flow is queryable, not just checkable.** `ControlFlowGraph`
   (`include/zl/mir/analysis.hpp`) answers predecessors/successors (normal and
   unwind, kept apart), reachability, reverse post-order, immediate dominators,
   dominance, dominator-tree children, dominance frontiers and dead blocks.
   Normal and unwind edges stay distinct throughout: an unwind target is
   reachable but is *not* dominated by the block that names it, so mixing them
   would make every catch block look like it dominates the code that catches.
17aa. **Which functions can run is a query over the module, not a guess.**
   `reachableFunctions` (`include/zl/mir/reachability.hpp`) walks the call graph
   from the entry point and returns an over-approximation: every override in a
   virtual receiver's hierarchy, every same-named function when a dispatch site
   names a class this module does not have, the `Shared` methods and collection
   methods the bytecode backend dispatches through instructions that name no
   callee (`include/zl/mir/backend_edges.hpp` - the emitter and the analysis
   share the list), and a referenced static's initializer. It reports
   `complete() == false` when the program reaches the reflection family, because
   a `Method` value enters code the module never named and `Type.methods()`
   reads the function table itself - so the answer is a lower bound there and
   says so. The approximation direction is deliberate: a function it calls
   unreachable really is unreachable, which is what a pass that deletes code
   needs - `eliminate-dead-functions` deletes exactly under `complete()`
   (docs/mir-optimizer.md, "Function removal").
17b. **Data flow is derived, never stored in the IR.** `include/zl/mir/dataflow.hpp`
   provides one definition of "a use" (`forEachValueUse`), def-use chains
   (`DefUseInfo`), liveness over slots and values (`LivenessAnalysis`), a
   value-based constant analysis (`ConstantAnalysis`), a generic monotone solver
   (`solveForward`/`solveBackward`) and named reachability helpers. The analyses
   live apart from the IR so a pass can ask a question without the answer being
   cached in a structure that can go stale.
18. **A branch condition is `bool`.** Where semantic analysis left a condition's
    type unknown — a call through a bare `func` — the lowerer records the
    required type with an explicit `Refine` rather than branching on an untyped
    value.

### Calls and generics

19. **A function is a template, never a copy.** `class Set<T>`'s methods are
    lowered once with `this: Set<T>`. The *call site* records which
    instantiation it selected in `Instruction::typeArguments`, one `TypeId` per
    entry in the callee's `typeParameters`. That is the only place the
    substitution is knowable, so it is where it is stored.
20. **An enum member's static type is the enum.** `Color.RED` is a string at
    runtime, but typed as `string` in MIR every comparison and every call taking
    an enum member would read as a mismatch. `ConstKind::EnumMember` carries the
    enum's name so the type survives.
21. **A bare `func` has no signature.** `FunctionSignature::hasSignature` is
    false, and the verifier defers arity checking to the call boundary.
    Recording it as `func(): void` would invent an arity the source never stated
    and reject calls the type checker accepted.
22. **A call's result may be absent.** Calls and `await` are value-producing
    opcodes with an *optional* result: a void callee and an `await` of
    `Task<void>` define no temp. `OpcodeShape::optionalResult` records this, so
    "does this opcode produce a value?" has one answer in the table rather than
    a special case in the verifier.

### Ownership

23. **Move, borrow, and drop state is tracked per slot** and checked as a
    dataflow problem over the reverse postorder: use after move, use after
    drop, move while borrowed, drop while borrowed (outside the exit-cleanup
    region, see below), double drop ("a resource releases exactly once"),
    borrow of a moved or released owner, and `EndBorrow` without `Borrow` are
    all rejected.
24. **Only owned storage can be moved or storage-released.** Ownership kinds on
    slots and parameters are preserved from the source, not inferred; a
    storage-release `drop` of `gc`/`borrow`/`shared` storage is rejected
    because only owned storage has a lifetime to end.
24b. **A move is final.** The checker rejects assigning to a moved variable,
    so the verifier does not let a store resurrect a moved slot either. (It
    does clear a *release*: the two dead-end states differ, and only the move
    is permanent in the language.)
24c. **Ownership joins are unions, matching the checker.** "A move is valid
    after a join only when every path preserved the source" - the checker's
    own `joinOwnershipStates` (branch and loop exits) unions the moved sets,
    and so does the MIR dataflow. The walk is single-pass, not a fixpoint,
    on purpose: a use at the top of a loop body of a value the same iteration
    later moves is exactly the case the checker's sequential scan also misses,
    and MIR must not reject a program the reference accepts.

### Exceptions

25. **A catch block has no normal predecessors.** It is reachable only along
    unwind edges. A catch block that control can also fall into is invalid. The
    same applies to a cleanup block: a `finally` region entered along an unwind
    edge has no normal predecessor, and a catch or cleanup block names the slot
    its handler binds (the caught value for a catch, the thrown object for a
    finally, which the block rethrows).
26. **Handler order is search order**, innermost last, matching how the chain is
    installed dynamically as blocks are created. A finally handler is a
    catch-all that sits innermost of its `try` (after its catches, so the
    catches get first refusal) and rethrows after the cleanup block runs.

---

## Instruction taxonomy

100 opcodes, grouped by what they do. `opcodeShape(op)` answers, for any
opcode, how many operands it takes, whether it produces a result, whether it
may throw, and whether it has side effects. `opcodeUsesSlot` and `opcodeIsCall`
are two of the shared predicates; `opcodeIsSuspension`, `opcodeIsBlocking`,
`opcodeIsThreadBoundary`, `opcodeIsScopedLock`, and `opcodeIsFfi` classify the
concurrency and native-boundary families. The verifier, the printer, and the
analyses all use them so they cannot disagree about what an opcode means.

- **Arithmetic / bitwise:** `Add Sub Mul Div Mod Pow Neg`, `BitAnd BitOr BitXor
  BitNot Shl Shr Ushr`
- **Comparison:** `Eq Ne Lt Le Gt Ge`
- **Logic:** `Not And Or`. `&&` and `||` are **not** opcodes — they short-circuit
  in ZL, so they lower to branches.
- **Conversion:** `Widen` (the language's implicit `int → double`), `Refine`,
  `TypeTest`, `NullCheck`, `IsNull`
- **Locals:** `Load Store Move Borrow EndBorrow Drop`
- **Aggregate / static access:** `FieldLoad FieldStore IndexLoad IndexStore
  StaticLoad StaticStore`
- **Construction:** `Alloc`, `NewCollection`
- **Calls:** `Call InvokeMethod InvokeSuper InvokeStatic CallIndirect
  CallNative MakeClosure`
- **Async:** `Await TaskCreate`
- **Tasks:** `TaskSpawn TaskBlock TaskIgnore TaskCancel`
- **Threads:** `ThreadStart ThreadJoin ThreadIsAlive`
- **Channels:** `ChannelCreate ChannelSend ChannelReceive ChannelSize
  ChannelSendAsync ChannelReceiveAsync`
- **Scoped locks:** `MutexWithLock RwLockWithRead RwLockWithWrite`
- **Atomics:** `AtomicLoad AtomicStore AtomicAdd`, `Bool`/`Double`/`Ref` lanes
- **Semaphores:** `SemaphoreAcquire SemaphoreRelease SemaphoreAvailable
  SemaphoreSetPermits SemaphoreTryAcquire SemaphoreReleaseMany`
- **Conditions:** `ConditionWait ConditionWaitFor ConditionNotifyOne
  ConditionNotifyAll`
- **Shared state:** `SharedCreate SharedGet SharedSet SharedWithLock`
- **FFI:** `FfiCall HandleBorrow HandleConsume HandleClose CallbackRegister
  CallbackInvoke CallbackClose`
- **Misc:** `Nop RangeInBounds Log`

Terminators: `Return Jump Branch Switch Throw Unreachable`.

Arithmetic typing is delegated to `zl::OperatorRules`, the same table the
existing compiler uses, so MIR cannot disagree with the language about what
`int + string` means (concatenation, producing `string`).

`Refine` and `TypeTest` are the two halves of a runtime type narrowing.
`Refine` asserts and retypes, raising when the value does not match — the typed
form of the VM's `AssertType`. `TypeTest` asks and produces a `bool` — the typed
form of the VM's `MatchType` — because a `match` arm has to survive a "no" and
fall through to the next arm. Its tested type lives in
`Instruction::testedType`, since `resultType` there is always `bool`.

`Refine` is not only for `match`. It is *the* spelling of the dynamic-to-static
boundary: wherever lowering moves a value semantic analysis typed `unknown` into
a concrete destination — a typed local or assignment (`int m = alias`), a call
argument against a declared parameter, a `return` against the function's result
type, a field or element write — it emits `refine` first. A backend lowers that
to a real runtime assertion (the MIR→bytecode backend emits the VM's
`AssertType`), so a wrong assumption fails loudly at the boundary instead of
being trusted. Only an identity refinement (operand type already equals the
asserted type) may be treated as a no-op, because there the MIR checker proved
the fact the assertion would re-check.

---

## Concurrency and the native boundary

Tasks, threads, channels, locks, atomics, shared cells, and the FFI boundary
all lower to dedicated operations — never to opaque `call_native` — so the
middle end can see the runtime contract each one carries: which operations
suspend, which block, which cross a thread boundary, which hold a lock, and
which leave the VM entirely. Every one of these operations is marked as
throwing in `opcodeShape` (each validates its receiver and raises on a
mismatch), so exception edges treat them like the calls they are.

### Suspension

`await` is the only suspension point: it parks the async frame on the
scheduler and resumes later with the payload. Spawning a task, starting a
thread, and the channel `sendAsync`/`receiveAsync` constructors only *create*
work — they do not suspend — and every blocking wait (`task_block`,
`thread_join`, `channel_send`/`channel_receive`, the scoped locks,
`semaphore_acquire`, `condition_wait`/`condition_wait_for`) parks the
*thread*, not the frame. Blocking inside an `async` function therefore strands
the scheduler worker, and the verifier warns at every such site.

`SuspensionLiveness` (`dataflow.hpp`) reports, for each `await`, everything
that must survive the park: every SSA value still to be read plus every slot
still to be loaded, each with its type — and, for slots, its ownership and
borrow source, so a backend can see that a borrow binding itself is part of
the preserved frame. The await's own result is excluded (produced by the
resumption, not preserved across the suspension), as are constants and
statics; both lists are deterministically ordered. The verifier separately
proves every borrow crossing an `await` roots in owned-frame storage
(transitively through borrow-of-borrow chains; a caller-owned borrow parameter
never roots), rejecting the suspension otherwise.

### Thread confinement

`task_spawn` and `thread_start` take a synchronous, zero-argument closure and
run it on another thread. Only `Shared<T>` and the synchronisation primitives
(`Atomic`, `Mutex`, `RwLock`, `Semaphore`, `Channel`, `Condition`) may cross
with it — the same rule the checker's confinement check enforces, mirrored so
hand-built MIR is held to it too. Anything else the closure captures
(including a bare `this`) is rejected; captures the verifier cannot resolve
stay the runtime gate's job, which raises there.

The spawned task's payload is the closure's own return, and the verifier
checks the two agree when the body resolves. An empty-body lambda reports
`nil` rather than `void`, so MIR spells that payload `Task<void>` — the
`Task<nil>` the checker names never appears — matching the closure body's own
void return and the runtime nil the task completes with.

`task_block` waits for a task and yields its payload, and is rejected in
`async` code (which suspends with `await` instead). `task_ignore` detaches
failure propagation; `task_cancel` requests cancellation, which the task
observes at its next suspension point and which the runtime then cascades to
the tasks spawned from that task's body. MIR models the request edge only;
the cascade is runtime state, not a graph edge the module sees.

### Scoped locks and shared state

`mutex_with_lock`, `rwlock_with_read`/`rwlock_with_write`, and
`shared_with_lock` run a synchronous closure with the lock held and produce
the closure's value — including the runtime nil of a void body, which an
`unknown`- or `nil`-typed temp may name (a concretely-typed one contradicts
the body and is rejected). The body itself must neither suspend nor hand work
to another thread: a direct `await`, `task_spawn`, or `thread_start` inside it
is rejected. An *indirect* suspension — the body calls a function that awaits
— is not caught, exactly as the checker does not catch it either; the lock is
still held at runtime, and that gap is documented on the opcode rather than
papered over.

`shared_create` builds a `Shared<T>` cell (`share(x)`), and `shared_get` /
`shared_set` are single synchronised accesses. Each access is atomic; a
read-modify-write *sequence* is not, so a function that reads and writes
without `shared_with_lock` gets one warning naming the pattern. The
`Shared.__get`/`__set`/`__withLock` dunders deliberately stay opaque native
calls: lowering them to the dedicated operations would recurse through the
same methods. `Time.sleepAsync` stays opaque for the opposite reason: it is a
scheduler timer, not a synchronisation primitive.

### Channels, atomics, semaphores, conditions

Channels are untyped and bounded: `channel_create` takes an `int` capacity,
`channel_send`/`channel_receive` block when full/empty, `channel_size` reads
the backlog, and `channel_send_async`/`channel_receive_async` return tasks for
the async side. The send future is `Task<void>`; the receive future is
`Task<unknown>` — the checker still names it `Task<void>`, a known
imprecision the lowering corrects, because the task genuinely carries the
received value. An `await` of a `Task<void>` normally defines no temp, but an
`unknown`-typed one is accepted wherever the checker left the await unnamed
(the `sendAsync` await observes `unknown`, not `void`); the temp names the
runtime nil the backend then pops, which is also what keeps the stack effect
identical to the reference compiler's `Await`+`Pop`.

Atomics lower one lane per representation (`atomic_load`/`store`/`add` for
`int`, `bool` and `double` lanes, `atomic_load_ref`/`store_ref` for objects),
each pinning its own result type. Semaphores spell all six operations
(`acquire`/`release`/`available`/`set_permits`/`try_acquire`/`release_many`)
with `int` counts; conditions spell
`wait`/`wait_for`/`notify_one`/`notify_all`, where the bare `wait` — no
predicate, no timeout — warns at every site, because a notify delivered before
it blocks is lost.

### The FFI boundary

`ffi_call` names a library symbol plus one ABI tag per argument and one for
the result (`NativeAbiTag`: `I64 F64 Bool Handle BufferView StructView
Callback`), preserving the native signature in MIR so a later backend can
marshal without re-deriving it. The verifier checks tags against the ZL
operand types (an `unknown` operand is marshalled by the runtime's own
packing), enforces the ownership contract (primitives carry none; a handle
returns `Owned`; views cross only as borrows and can never be returned), and
rejects a call with no symbol or mismatched tag/ownership counts. Handles live
in `NativeHandle` values managed by
`handle_borrow`/`handle_consume`/`handle_close`; callbacks are
`NativeCallback` tokens behind `callback_register` (synchronous closures only
— the registry invokes them under a lease, with no scheduler to park on) with
arity/result-checked `callback_invoke` and `callback_close`. The MIR→bytecode
backend fails closed on the whole family: FFI has no bytecode spelling, so any
function using it is stubbed rather than mistranslated.

---

## What the builder guarantees

`FunctionBuilder::finish()` establishes two invariants before anything inspects
the function:

- every block ends in exactly one terminator — an unterminated block becomes
  `Unreachable`;
- no block is unreachable — `pruneUnreachableBlocks` removes blocks nothing can
  reach, following **both** normal and unwind edges, renumbers the survivors
  densely, and rewrites every reference to them.

This is why a function that was only *partially* lowered is still structurally
valid MIR. The semantic caveat travels separately, in `Function::incomplete`.

Unwind edges matter here in a way that is easy to get wrong: a block reached
along an unwind edge is ordinary code from there on, so a catch handler's jump
to the join block keeps everything downstream of it reachable. Following only
unwind edges in that traversal makes the join block look dead, prunes it, and
leaves the handler jumping nowhere.

---

## Lowering

`zl::mir::lowerProgram(program, checker)` runs four passes over the flat
`Program` the `ModuleLoader` produces:

0. **Class shapes and layouts** — type parameters, parents, fields, statics.
1. **Declare every function**, so a call can name its target regardless of
   declaration order. Lambdas are discovered in the same pass, because a closure
   body is a real MIR function that other instructions reference by id.
2. **Declare lambda bodies** — flags and capture placeholders only. Capture
   *types* are not knowable yet; declaring them early would mean declaring them
   `unknown` and throwing the information away.
3. **Lower function bodies.**
4. **Lower lambda bodies**, in discovery order, so an outer body fills an inner
   closure's capture types before that closure's parameters are added.

Ordering is real, not incidental: the lambda list is a vector, not the hash map
that indexes it, because a hash map's iteration order would let an inner closure
be lowered before the outer body that types its captures.

Notable choices:

- **Function names** are the checker's own: `ownerClass + "." +
  dispatchSignatureForFunction(...).describe()`. Lookups fall back up the parent
  chain, so inherited methods resolve the way dispatch does.
- **`this` is parameter 0**, with `Function::hasThisParameter` set. It is not a
  hidden side channel. Inside a generic class it is the *self-parameterized*
  form (`Set<T>`, not bare `Set`).
- **Closures are functions with leading capture parameters**, so a closure's own
  arity is `parameters.size() - captures.size()`. Captures are copied into slots
  at body entry. The environment is explicit rather than implicit.
- **`this` is captured by name, like any other local.** Semantic analysis puts
  `"this"` in `captureStorageNames`, and a capture is resolved by looking the
  name up in scope — so the enclosing body binds `this` under its own name as
  well as holding it in the receiver field. Inside the closure the captured slot
  then becomes the body's receiver, because `this` in a body is spelled `ThisExpr`
  and answered from the receiver, not from the scope. Capturing it without that
  second step lowers the closure but rejects every `this` inside it.
- **A closure inside a generic class is generic over the class's parameters.**
  It closes over `this`, whose type mentions them, and an unsubstituted type
  parameter is only legal inside a template. So the closure's MIR function is
  declared as a template over the same parameters its owner class has; without
  that, the captured `this: Box<T>` is a type parameter in a concrete function
  and the verifier rejects it.
- **`coerce()` makes implicit conversions explicit.** Where the result type is
  `double` and an operand is `int`, a `Widen` is emitted, so MIR arithmetic is
  homogeneous and a backend never has to re-derive the promotion rule. Where
  the operand is `unknown` and the target is a concrete type, a `Refine` is
  emitted — the dynamic-to-static boundary of invariant 8. The same rule runs
  at every call with a statically known callee: direct calls, `super` calls,
  constructors, indirect calls through a `func(int): int`-shaped value, and
  native calls (against the catalog's declared parameter names). Field writes,
  static-field writes, collection literals (`[1, 2]` asserts each element
  against the collection's element contract) and record literals assert against
  the declared field/element types. A method dispatch names no static callee, so
  its arguments ride on the VM's own dispatch-time checks, as in the reference
  path.
- **A `data` record literal is an allocation plus a write per field.** A record
  has no constructor, so `Point { x: 10, y: 20 }` must not go looking for one —
  unlike `new C(...)`, it never consults the function table.
- **A collection has two spellings and both must work everywhere.** The
  lowercase keyword forms (`list<T>`, `map<K,V>`, `set<T>`) arrive as the
  List/Map/Set kinds; the capitalised class forms (`List<T>`, `Map<K,V>`,
  `Set<T>`) are real generic classes, so they arrive as an `Object` carrying
  that name with the same arguments. Inside a generic class the class spelling
  is the only one available, since `list<T>` cannot be parameterised by a type
  variable. `isCollectionType` is the single answer to "is this a collection",
  used by both construction and indexing, so a spelling cannot end up
  constructible but not indexable or the reverse. A literal is built as
  `NewCollection` plus one `IndexStore` per element, which is why a set has to
  accept positional indexing even though the language has no positional read for
  one.
- **Unsupported constructs do not crash.** `unsupported()` marks the function
  incomplete, records a note, and returns. The module still verifies.

- **`match` lowers to a chain of two-way branches**, one test block per arm, in
  source order. The subject is evaluated exactly once before the first test, so
  a subject with a side effect cannot run once per arm and a guard that
  reassigns the subject's variable cannot change what a later arm compares
  against. Arm bodies store into a result slot the join block reads back — the
  same join mechanism `try` uses, and the shape `promoteSlotsToBlockParameters`
  rewrites into a block parameter once the arms are simple enough. A type pattern emits
  `TypeTest` and then `Refine`s both the arm's own binding and the subject
  identifier the body keeps spelling, which is what narrows `int|string` to
  `int` inside an `int n =>` arm. The non-exhaustive fallthrough raises
  `Exception("non-exhaustive match")`, as the bytecode reference does; when the
  last arm is a wildcard that block is unreachable and `finish()` prunes it.

### Ownership events

The language's ownership model survives lowering as explicit events, not as
metadata that type checking consumes and discards:

- `move` is `Move` on a slot: the storage is emptied and the value continues
  in the result. The verifier's dataflow makes every later use of that slot an
  error, so a backend or pass cannot silently turn the transfer into a copy.
- `borrow T v = owner` is `Borrow` into a borrow-kind slot; `EndBorrow` ends
  it explicitly. ZL borrows are function-scoped, so lowering does *not* emit
  `EndBorrow` at scope end - the borrow ends when the function does. The
  verifier models this: an owner's release inside the trailing cleanup region
  (drops followed only by the return) cannot conflict with a live borrow,
  because both end at the same point; anywhere else it is an error.
- The end of an owned local's lifetime is `Drop` in its **storage-release
  form**: the slot named, no operands (`drop slot 2('x')`). The value form
  (one operand) releases a parameter's or temp's value. `Drop` is the only
  opcode with two exclusive spellings; both check as the same event.
- Lowering emits the cleanup before **every** return site - each `return`, the
  implicit end of a void body, and each lambda's exit - in reverse
  declaration order, for owned slots that were not moved and for owned
  parameters. This mirrors the reference compiler's `DropVar`-before-`Return`
  exactly; the VM's frame teardown also erases a frame's owned locals on any
  exit (early return, exception), so the two paths release the same storage at
  the same points.
- Slots and parameters with `gc` storage get no drop events: the collector
  owns those values. `shared` storage is a thread-synchronised cell
  (`Shared<T>`), not a reference count - nothing counts a managed box
  anywhere in the runtime - so it likewise carries no release event here.
- Every release event here is a **rooting** release, not a free: `DropVar`
  calls `ExecutionState::dropLocal`, and frame teardown calls
  `dropOwnedLocals`, both of which end a binding's ability to keep a box
  alive. Physical storage returns in the collector's reclaim phase. Eager,
  deterministic reclamation is the subject of
  [memory-domains.md](memory-domains.md), whose `release` is what would first
  hand bytes back.
- Closure captures are always GC values (the checker forbids owned/borrow
  captures), so `MakeClosure` needs no ownership events.

### What is not lowered yet

A method call whose receiver semantic analysis could not resolve to a class is
the one remaining note: it lowers to a diagnostic and an incomplete function
rather than malformed MIR, and a valid program does not reach it (the checker
rejects the unresolved receiver first). Every construct that used to be listed
here now lowers completely:

- `try/finally` lowers to an exception handler with a `Cleanup` target. The
  handler is a catch-all (`isFinally`) that the bytecode backend spells as
  `PushFinallyHandler`; the target block binds the thrown object into a pending
  slot, runs the finally body, and throws the slot again. Normal completion
  runs the same body in a plain block, and `return`/`break`/`continue` inside
  the try region inline the active finalizers innermost-first, exactly where the
  reference compiler does.
- Structural (`data`/list/map/set) match patterns lower to the same two-way
  branch chain as every other arm, nesting per field/element/value: `data`
  destructures fields, `list`/`set` check length then members, `map` checks
  entry presence then values. Positional data patterns (`Pair(v, w)`) have their
  field names resolved to declaration order by the checker before lowering.
- `data` copy-update (`base with { ... }`) lowers to an `alloc` plus per-field
  copies and coerced updates.
- Function references lower to `MakeClosure` with no captures, resolved through
  the class hierarchy so an inherited static reference works.
- Object-typed collection literals build an untyped container of the literal's
  shape instead of bailing out.

What remains is a fidelity question rather than a lowering gap: a bare `func`
parameter carries no signature, so semantic analysis types its body's
expressions UNKNOWN and MIR records that faithfully instead of inventing a type.
Fixing it means inferring lambda parameter types at call sites, in the checker —
not guessing in the lowerer.

---

## Explicit data flow

Lowering emits the *memory* form of ZL's mutable locals — `store` on each path,
`load` where the value is needed — because that is what the language means and it
needs no analysis to be correct. It leaves the merge implicit. `ssa.hpp` turns
that into the value form:

```cpp
SsaPromotionReport promoteSlotsToBlockParameters(Function& function);
```

The algorithm is the classic one, and it is built entirely out of the machinery
in `analysis.hpp`:

1. **Eligibility.** A slot is considered only when nothing but `Load`/`Store`
   touches it (no `Move`, `Drop`, `Borrow`, `EndBorrow` — those are memory
   management, not values), its storage is plain GC, it is not a catch binding,
   every store already carries the slot's declared type, and the function has no
   exception-handler chains at all. The unwind rule is not laziness: an unwind
   edge is a path the dominator tree does not model, so "the value reaching a
   catch block" is not a question this construction can answer. The store-type
   rule is a type-fidelity rule: a `Load` produces the *slot's* type — that is
   the contract the stores were checked against, with dynamic values asserted
   before they were stored — while the value form hands later uses the stored
   operand itself. Promoting a slot whose stores are retyped relative to it
   (`unknown alias = numbers`, where the slot says `unknown` and the stored
   value says `list<int>`) would let uses observe `list<int>` with no refine
   anywhere, silently erasing the declared type a `match` subject still needs.
2. **Definite assignment**, as a forward must-analysis over `solveForward`. A
   read may be rewritten into a value only where the slot is written on *every*
   path reaching it; otherwise the value form would name a value that does not
   exist. (The fact has to be the two-valued "assigned on all paths", not "the
   set of blocks that wrote" — the latter intersects to empty at a join reached
   from two different writers, which is precisely the case being converted.)
3. **Placement** at the iterated dominance frontier of the writing blocks — the
   join points where two definitions meet. A parameter is kept only when its
   block is definitely assigned on entry, i.e. when every incoming edge really
   has something to pass; a loop header whose back edge defines a value but whose
   entry edge does not is therefore declined, not given an unpassable parameter.
4. **Renaming** by a depth-first walk of the dominator tree with explicit
   enter/exit actions, so a block's pushes are popped when the walk leaves its
   subtree and a sibling never sees a value that does not reach it. Each `Load`
   becomes its current value (uses of the load's result are rewritten), each
   `Store` updates it, and every successor's edge argument is filled in as the
   predecessor is visited.

Promotion runs on a **copy** and is committed only if it completes, so a shape
the algorithm cannot handle leaves the function exactly as it was. It is
fail-closed in the same sense the bytecode backend is: declining is always
allowed, a half-rewrite never is.

The bytecode backend consumes both forms. It has no phi instruction available, so
a block parameter is lowered as the memory form of itself: each predecessor
stores the argument into that parameter's own local before transferring, and the
block reads it. Values and slots are interchangeable in meaning, so this is a
faithful translation — and it is what makes `ZL_MIR_PROMOTE=1 zl --mir-vm` a
differential check of the promotion itself.

---

## Verifier

`verifyModule(module)` checks a function in this order, because later stages
assume earlier ones held:

1. `checkSignature` — parameters, return type, `this`, type parameters.
2. `collectDefinitions` — SSA uniqueness and temp→slot provenance.
3. `checkControlFlow` — terminators, block references, predecessor consistency,
   reachability, entry-block shape, and the block-parameter rules:
   one definition per parameter id, no parameters on the entry block, a
   non-void typed parameter, exactly one argument per parameter per normal
   incoming edge with an assignable type, and no arguments on a terminator that
   transfers nowhere. An invalid SSA/data-flow structure is reported as an error
   naming the block and the edge, so a bad merge is a deterministic diagnostic
   rather than a backend surprise.
4. `checkInstructions` — operand counts, operand types, result types, opcode
   shapes, call signatures, field and index access, ownership rules.
5. `checkDominance` — every temp use is dominated by its definition, and every
   block-parameter use is dominated by the block that defines it.
6. `checkOwnershipFlow` — a walk over reverse postorder tracking
   `{moved, dropped, droppedParams, borrows}`. Joins union the dead-end sets
   (dead on any path is dead) and intersect the borrows. Stores clear a
   slot's release record; nothing clears a move. `Drop` understands both
   spellings (slot or value operand), reports the once-only rule on a double
   release, and defers to the language's function-scoped borrows for the
   trailing exit-cleanup region.

The verifier is also the safety net for the construction pass: `--emit-ssa`
re-runs `verifyModule` after promoting and refuses to print MIR that does not
verify.

Module level, it additionally checks for duplicate function names, positional id
consistency, the entry point, and static field layout.

The verifier has its own `assignable(from, to)`. `TypeChecker::isAssignable` is
private, and the two are not identical: MIR's does not model ZL's collection
conversion rules (`Set<T>` against bare `set`, list/set interchangeability when
generics agree). That is a known gap, not an oversight — widening it means
exposing the checker's rules at the right layer rather than duplicating them.

On top of the nominal edges it knows the builtin sums: `Some<int>` is assignable
to `Option<int>`, `Ok<int,string>` to `Result<int,string>`, in either spelling,
with payload compatibility checked pairwise and recursively — so
`Some<List<int>>` satisfies `Option<list<int>>` while `Some<string>` does not
satisfy `Option<int>`. A union target accepts a value assignable to *any*
member, which is what makes returning a `Some<int>` from a function declared
`Option<int>|nil` verify.

The other load-bearing rule is the boundary check: an operand typed `unknown`
may reach a *typed* destination — a slot, field, element or static store, a
direct-call parameter, a return, a block-parameter edge argument — only through
a `refine`. Anything else is rejected with an error naming the boundary, because
a dynamic value that a store silently reclassifies makes every later consumer
trust a type nothing ever checked. `unknown`/`TypeParam` destinations stay
permissive (nothing was assumed), and so does anything the verifier cannot
resolve.

Subtyping is decided from the module's type tables, and it follows **all three**
subtype edges the language has:

| edge | recorded in |
| --- | --- |
| `class` → `class` (`extends`) | `ClassLayout::parent` |
| `class` → `interface` (`implements`) | `ClassLayout::interfaces` |
| `interface` → `interface` (`extends`) | `InterfaceInfo::bases` |

Leaving any one out rejects a legal program rather than merely missing an
optimisation. `Shape masked = new Circle(...)` is the ordinary way to write a
polymorphic local and needs the second edge; `Named n = shape` for `interface
Shape extends Named` needs the third, because the store's value type is an
*interface* and there is no class to hang the relationship on. The search is a
breadth-first walk with a visited set — either hierarchy can contain a diamond —
and a step guard, since both tables are input data rather than a guarantee.

### Interfaces are contracts, not classes

An `interface` declaration gets no `ClassLayout`: it has no fields, no instance
layout and no dispatch row, and putting it in `Module::classes` would make the
backend build reflection metadata and vtable rows for a type that can never be
instantiated. What it gets instead is an `InterfaceInfo`, which records the two
things the rest of the IR actually needs:

- **its bases**, for assignability (above); and
- **its method signatures**, because a call through an interface-typed receiver
  still has to be dispatched. Dispatching from the interface's own declaration
  is the exact answer; inferring it from whichever class happens to implement
  the interface would be a guess, and the interface is the only thing that fixes
  the signature the call must match.

Generics are not currently part of this: interfaces take no type parameters in
this language, so the recorded signatures are concrete types.

Member **visibility** is carried into MIR too (`MemberAccess` on `FieldLayout`
and `Function`). It is not bookkeeping: `Type.fields()` prints each field's
access, so a backend with nothing to consult prints `public` for a `private`
field and the program's output changes. Reflection metadata is program output,
which makes an omitted part of it a miscompile rather than a missing feature.

An **unsubstituted generic parameter is compatible with anything**. Inside a
template, `T` stands for some type that is not knowable at verification time, so
demanding a match would reject correct MIR. Recording the instantiation at the
call site is what makes the check precise where it can be.

---

## Optimisation

MIR can be optimised after lowering. That layer is described in
[`mir-optimizer.md`](mir-optimizer.md); what follows is the summary, because one
of its constraints is a statement about MIR itself rather than about any pass.

**The optimiser is a consumer of MIR, not a part of it.** It is reached from the
command line (`--emit-mir-opt`, `--mir-opt-check`), from the run paths
(`--mir-vm`, `zl file.zl`, `ZL_MIR_OPT=0` turns it off), and nothing in MIR
changed to accommodate it. `Module`,
`Function` and `Instruction` are exactly what they were; the optimiser reads
them, proves things about them, and hands back the same structures with fewer of
them in.

**The safety contract is the constraint MIR had to earn.** An optimisation is
legal only if it preserves observable side effects, evaluation order, overflow
behaviour, ownership, borrowing, drops, exceptions, synchronization, native
resource lifetime, task behaviour and callback behaviour. That list can only be
checked if the IR states the facts the checks need — which is why MIR records
types on every value, effects in an explicit table (`effects.hpp`), exception
edges as CFG edges rather than implied control flow, ownership on slots and
bindings, and every suspension, thread and native-resource operation as its own
opcode. An IR that erased any of those could not be optimised safely at all;
the most it could do is guess.

**Where safety cannot be proven, nothing is done.** Every pass counts what it
declined as well as what it changed. The shipped passes are constant folding,
constant propagation, dead-block elimination, dead-value elimination, simple
algebraic simplification, redundant-conversion removal, branch simplification
and local copy propagation — all of them either deleting work that provably
cannot be observed or replacing a use with something provably equal. None of
them reorders, sinks, hoists, inlines or duplicates.

**Equivalence is checked twice, and the second check is the one that counts.**
`compareModules` compares the two modules structurally and by observable event;
`tools/mir_opt_diff.sh` runs every program in `examples/` both ways and compares
its output and exit status. The static comparison deliberately ignores runtime
checks and local stores, and cannot see values at all — running the program is
what covers those. It is also what found the one miscompile so far (a
dead-looking store in a closure body that is not dead, because ZL captures by
value and a mutated capture persists between calls), which is the argument for
having both rather than trusting the cleverer one.

---

## Textual form

`printModule` produces a readable dump, used by `zl --emit-mir`:

```
func Hello.main()($0 this:Hello): void
  at line 2
  slots:
    s1 $local383:int
    s2 __for_end:int
  b1 (entry):
    store slot 1('$local383'), 0:int  ; line 3
    jump b2  ; line 4
  b2:
    %1:int = load slot 1('$local383')  ; line 5
    %2:bool = gt %1:int, 5:int  ; line 7
    branch %2:bool, b4, b3  ; line 7
  b3:
    log "small":string  ; line 10
    jump b5  ; line 7
  b4:
    log "big":string  ; line 8
    jump b5  ; line 7
  b5:
    return  ; line 2
  edges: b1->b2 b2->b3 b2->b4 b3->b5 b4->b5
```

`$n` is a parameter, `%n` a temp, `^n` a block parameter, `sn` a slot, `bn` a
block. Generic templates are marked `; generic-template`, constructors
`; constructor`, and an unlowered body `; incomplete`.

With block parameters (after `--emit-ssa`, see below), the same function shows
the merge as a value — a parameter on the join block and one argument per
incoming edge:

```
  b2(^1 x$1:int):
    %2:string = add "x=":string, ^1:int
    return
  b3:
    jump b2(20:int)
  b4:
    jump b2(10:int)
```

---

---

## Command line

```bash
zl --emit-mir out.mir program.zl   # write the textual MIR
zl --emit-mir - program.zl         # print it to stdout
zl --emit-ssa out.mir program.zl   # promote locals to block parameters first
zl --emit-ssa - program.zl         # and check what came out
```

`--emit-ssa` asks the lowering stage for the MIR's SSA form
(`promoteSlotsToBlockParameters` over every function) and then verifies before
printing: a promotion that produced invalid MIR is reported rather than emitted.
`ZL_MIR_SSA_VERBOSE=1` adds one line per declined slot saying why. `--mir-vm`
runs the same pass when `ZL_MIR_PROMOTE=1` is set, which is how the backend is
checked to behave identically on both forms - see `tools/mir_promotion_diff.sh`,
which does exactly that comparison across the example corpus (50 identical, 0
differing, none skipped).

The optimiser has its own commands:

```bash
zl --emit-mir-opt out.mir program.zl   # lower, optimise, write the result
zl --emit-mir-opt - program.zl         # print it
zl --mir-opt-check program.zl          # optimise and diff; exit 4 on divergence
zl --mir-vm program.zl                 # run the optimised MIR (the run path)
ZL_MIR_OPT=0 zl --mir-vm program.zl    # ... and run it unoptimised
```

`ZL_MIR_OPT_PASSES` selects the pipeline (`default`, `none`, or a comma-separated
list of pass names), `ZL_MIR_OPT_SNAPSHOT_DIR` writes a before/after snapshot per
changed pass, and `ZL_MIR_OPT_VERBOSE=1` prints the per-pass trace to stderr.
`--mir-opt-check` is the static half; `tools/mir_opt_diff.sh` is the runtime
half, and currently reports 50 of 50 example programs identical.

Exit codes: `0` verified, `2` bad usage, `3` stdlib version mismatch, `4`
verification failed, the optimiser did not trust its result, or the selected
backend refused, `5` the output could not be written, `6` unsupported lowering,
`1` a front-end, environment or compiler-invariant failure. The full table is in
[pipeline.md](pipeline.md#exit-codes). Lowering notes go to stderr; they are not
errors, and a module with notes can still verify.

---

## Tests

- `tests/mir_tests.cpp` (`zl-mir-tests`) — 65 regressions. Builds MIR by hand
  and checks the verifier rejects each class of malformed module. A
  lowering-only test could never reach most of these shapes, because the builder
  refuses to produce them. The concurrency set pins the opcode
  shapes/predicates, the task/thread/channel/lock/atomic/semaphore/condition/
  shared/FFI/callback typing rules (accept and reject sides), the
  void-body-temp compatibility rule, and the bare-wait and unlocked-access
  warnings.
- `tests/mir_lowering_tests.cpp` (`zl-mir-lowering-tests`) — 42 regressions.
  Drives the real pipeline on small programs and asserts on the MIR that comes
  out — including the specific properties an earlier lowerer got wrong, and the
  end-to-end merge (`if/else` writing one variable, then read) coming out as a
  block parameter with one argument per edge, interface widening verifying, and
  member visibility surviving into the module, and every concurrency
  primitive lowering to its dedicated operation (`task_spawn`/`task_block`/
  `task_ignore`/`task_cancel`, `thread_start`/`join`/`is_alive`, the channel
  family, the scoped locks, every atomic lane, semaphores and conditions, and
  `shared_create`/`get`/`set`).
- `tests/mir_ssa_tests.cpp` (`zl-mir-ssa-tests`) — 30 regressions. Covers the
  CFG queries (predecessors/successors, reachability, dominance, dominator tree,
  dominance frontiers, dead blocks), the data-flow layer (def-use, liveness,
  constants), every way a block parameter or edge argument can be malformed, and
  the promotion pass including the slots it must *decline* and the fact that a
  declined promotion leaves the function untouched, per-`await` suspension
  liveness (live temps and slots with types, borrow bindings with ownership
  and source, the owned root crossing with the borrow), and promotion
  composing with a suspension point.
- `tests/mir_ownership_tests.cpp` (`zl-mir-ownership-tests`) — the
  ownership/lifetime suite. Hand-built MIR naming one invalid state each
  (use after drop, double drop, drop after move, drop while borrowed
  mid-block, borrow after drop, storage-release of gc storage, both drop
  spellings at once, a drop naming no storage, drop/move on one path used at
  a join, `end_borrow` without a borrow, double drop of an owned parameter)
  plus the legal shapes that must stay legal (the exit release with a live
  function-scoped borrow, borrow→end_borrow→move), and a pipeline case
  proving a real `owned`/`move`/`borrow` program verifies as emitted.
- `tests/mir_type_tests.cpp` (`zl-mir-type-tests`) — the type-system integration
  suite. The type table (primitives, classes, generics, instantiated generics,
  function types, unions, collections, Option/Result sums, nullability, and the
  nested shapes `List<int>`, `List<string>`, `Map<string,int>`,
  `Map<string,List<int>>`, `Option<List<int>>` surviving as structural ids);
  sum-spelling relations; union return types; the dynamic-to-static boundaries
  emitting `refine` and the backend emitting `AssertType` for them; `match`
  narrowing emitting `type_test`+`refine`; the verifier rejecting an unrefined
  unknown store and a `Some<string>`-for-`Option<int>` return; distinct generic
  overloads keeping distinct dispatch identities; and SSA promotion declining
  to retype a dynamic slot.

- `tests/mir_pass_tests.cpp` (`zl-mir-opt-tests`) — 146 regressions. The
  optimiser's executable specification, written against hand-built MIR: the
  effect table and the rules that discharge `MayThrow`, the evaluator's
  refusals, each pass's rewrite *and* each pass's declinations, the closure
  store rule, pipeline ordering and the registry, rollback of a corrupting
  pass, analysis caching and invalidation, snapshots, and the differential
  validator catching a deleted `log` or a changed signature while accepting a
  real optimisation.
- `tests/mir_opt_pipeline_tests.cpp` (`zl-mir-opt-pipeline-tests`) — 79
  regressions over real ZL programs: lower → verify → optimise → verify → static
  differential → run both versions and compare their output and exit code.
- `tools/mir_opt_diff.sh` — the corpus harness: every program in `examples/`
  run optimised and unoptimised, outputs compared byte for byte.

Lowering is also exercised across `examples/`: every file is lowered and
verified. 56 of the 59 lower and verify completely; the other 3 verify with
notes (8 notes between them). That count is the measure of coverage — and
because `--emit-ssa` re-verifies after promoting, every one of the 59 also
promotes and re-verifies clean.

The 8 remaining notes are all one root cause: a bare `func` parameter.
`applyTwice(func f, int x)` carries no signature, so semantic analysis types a
lambda's parameter as `unknown`, and MIR records `unknown` rather than inventing
a type. That is the honest answer — the language genuinely does not know — and
inventing `int` to make the count look better would be exactly the erasure this
IR exists to avoid.

## Safety analysis and research evaluation

The verifier also exposes stable property categories and JSON diagnostics through
`--safety-check`. Definite initialization, may/must ownership and borrow facts,
explicit native resource identities, and boundary audits are described in
[MIR safety analysis](mir-safety.md), together with a corpus protocol that keeps
source rejection layers separate from malformed-MIR compiler-contract tests.

The measured, end-to-end evaluation of MIR as a compiler boundary — compile-time
stage costs, generated code size, runtime and memory behaviour, static vs runtime
safety, and the reference-vs-MIR and optimised-vs-unoptimised comparisons — lives
in [`research/`](../research/report.md), with the re-runnable corpus and harness.
