# Memory domains: user-defined automatic memory management

Status: **Phases 0–1 implemented; Phases 2–7 design.** This is the direction
for letting ZL programmers write their own automatic memory strategies beside
the collector. It replaces the draft "Custom Automatic Memory Management
Plan"; the
[corrections that came out of checking that draft against this tree](#what-changed-from-the-draft)
are recorded at the bottom, each one against the code that made it necessary.

What exists so far: the [Phase 0 baseline](#101-phase-0-baseline-measured-2026-09-19)
(flat object fields, box recycling, the allocation benchmark and its numbers),
and the [Phase 1 annotation rules + `memory` contract
skeleton](#55-prerequisite-annotations-need-a-rule-before-they-carry-semantics)
(landed 2026-09-19: unknown-annotation error, per-annotation targets, and a
`memory` declaration whose contract the compiler validates with no consumer
yet). Everything below that says "will" or "lands with Phase N" is still
design.

Related: [language-guide — memory-model direction](language-guide.md#memory-model-direction),
[mir.md — ownership](mir.md), [mir-safety.md](mir-safety.md),
[native.md — the FFI ownership boundary](native.md).

## 1. The rule

> ZL allocates automatically. A memory domain chooses **when** an allocation
> may die; it never gets to choose **what** it is, and it never sees a byte or a
> `Value`.

Everything below follows from that. A domain is a reclamation *policy* over
opaque slots, and the compiler is the only caller of its contract. That is what
makes a user-written domain safe to run inside the runtime: safety comes from
**where the contract is invoked**, not from trusting the code inside it.

## 2. Two axes, deliberately kept separate

ZL already has one half of this, and the design only works if the two stay
apart:

| Axis | Answers | Where it lives today |
| --- | --- | --- |
| **Lifetime contract** | who may read/`move`/drop this binding, and for how long | `OwnershipKind{gc, owned, borrow, shared}` in `include/zl/common/ownership.hpp`, parsed at declaration and parameter positions (`src/parser/parser.cpp`), validated by `TypeChecker::validateOwnership`, carried on `mir::Slot::ownership`, `mir::Parameter`, `mir::FieldLayout` and `RuntimeFieldInfo`, and checked as dataflow by `TypeChecker::joinOwnershipStates` and `FunctionVerifier::checkOwnershipFlow` (opcodes `Move`/`Borrow`/`EndBorrow`/`Drop`) |
| **Storage domain** *(new)* | where the allocation came from, and what makes it collectible | nothing yet — `TracingGC` is the only answer, reached through the `makeGC*` gateway |

The contract axis is *not* replaced and *not* merged into the domain axis. A
value can be `owned` **and** live in an arena; a `borrow` can view a
GC-managed object. Representing that as one type constructor (`Arena<'a, T>`,
as the draft proposed) would re-open every ownership rule in the checker, the
MIR verifier, and both backends. Keeping them separate means each rule stays
where it is already proved, and the new one is additive.

`SafetyProperty` already has `Ownership`, `Borrow`, `Move` and
`ResourceLifetime`; the new checks report under a new `MemoryDomain` member of
that enum (`include/zl/mir/verifier.hpp`), classified in `src/mir/safety.cpp`, so
existing tooling that keys on property ids keeps working.

## 3. What exists, so it is not rebuilt

| Existing thing | File | Why it matters here |
| --- | --- | --- |
| Four GC box kinds, one global registry | `src/vm/gc.cpp` (`makeGCList/Map/Object/Closure`), `ListBox`/`MapBox`/`ObjectBox`/`ClosureBox` in `include/zl/vm/value.hpp` | This is the *only* managed allocation surface. A domain allocates a **box of a kind**, not `n` bytes |
| Deferred destruction | `TracingGC::Collection` — trace detaches, `reclaim()` destroys after mutators resume | A domain's bulk release must land in the same deferred phase, or a destructor that joins a thread runs during a stop-the-world trace |
| Counted pinning | `TracingGC::protect/unprotect`, `ProtectedGCRoot` | Already the answer to "keep this alive while code outside an `ExecutionState` uses it" — a domain reuses it rather than adding a root kind |
| Safepoints + collection trigger | `GCSafepointCoordinator`, `instructionsSinceSafePoint >= 128` in `src/vm/vm.cpp`, `allocationThreshold_{128}` in `include/zl/vm/gc.hpp` | Domain `collect()` hooks are driven from here; a domain cannot start a collection |
| Deterministic non-GC resources | `include/zl/vm/native_resource.hpp`, MIR `HandleBorrow`/`HandleConsume`/`HandleClose`, `CallbackRegister/Invoke/Close`, `tests/native_resource_*` | **This is ZL's working precedent for a user-visible memory lifetime contract**: registry-owned, opaque token, exactly-once release, borrow invalidated when the owner dies, verified in MIR. A memory domain is this, generalised |
| Region paths for borrows | `TypeChecker::borrowRegionForExpr`, `borrowRootOwner`, `regionOverlaps`, `OwnershipFlowState` (with `joinMapsByAgreement` joins) | The escape machinery already exists for slot-rooted paths (`owner.field.g`). Domains extend its roots to region handles; they do not need a new analysis |
| Scope-exit execution | `finally` bodies inlined innermost-first at `return`/`break`/`continue` (`docs/mir.md`) | Region exit needs exactly this, for every exit path including unwinding. Do not build a second one |
| Two-path release agreement | lowering emits `Drop` before each return site, and the VM's `dropOwnedLocals()` (`include/zl/vm/execution_state.hpp`) erases the frame's owned locals — "the two paths release the same storage at the same points" (`docs/mir.md`) | The model for §4.3's agreement rule, and the proof that double release is already a solved problem shape |

## 4. The model

### 4.0 Why this is worth doing at all

`owned` today ends a binding's *rootedness*, nothing more: `DropVar` calls
`ExecutionState::dropLocal` and frame teardown calls `dropOwnedLocals`, both of
which erase the local from the frame, and the physical box is freed later by
whichever collection happens to notice it. So ZL has a complete, checked
lifetime language with **no eager reclamation behind it** — `owned` is
currently a promise about aliasing, not about memory. A domain's `release` is
the first thing in the runtime that would actually hand storage back. That is
the gap this document exists to close, and it is also why Phase 0 measures the
collector first: if eager release can be had by improving `reclaim()` and the
free list, most of the benefit needs no new syntax at all.

### 4.1 A domain is a policy over slots

`allocate(size)` and `release(block)` do not fit this runtime: `Value` holds
non-owning `GcRef` pointers, and each box is a C++ object whose payload is
`std::string`, `std::vector<Value>` and `std::unordered_map<std::string, Value>`.
A bump pointer cannot own those, and freeing a chunk without running each box's
destructor leaks its payload — which is exactly the work a region was supposed
to save. So the contract is typed and lifecycle-shaped:

```zl
memory FrameArena {
    // Required. Called by the compiler's `Alloc` for one box. `shape` names the
    // kind and the class, never a size; returning None asks `exhausted` what to do.
    public func acquire(MemoryShape shape): Option<MemorySlot>

    // Required. Called when a live owner count can no longer exist: region exit,
    // last use of an `owned` binding, or `release` on the handle.
    public func release(MemorySlot slot): void

    // Optional. Bulk release at region exit. Default: release each live slot.
    public func reset(): void

    // Optional. Called after acquire returns None. Default: raise `RuntimeError`,
    // the class the runtime already falls back to for unclassified failures.
    public func exhausted(MemoryShape shape): void

    // Optional. Observes the global trace so a policy can keep its own counters.
    // It may not free anything from here; that happens in the reclaim phase.
    public func onCollect(MemoryStats stats): void
}
```

`MemorySlot` is an opaque registry token, the same shape as a
`NativeHandleRef`: `{domainId, index, generation}`. `MemoryShape` is a
`data` record `{kind: BoxKind, className: string, fieldCount: int}`. Neither
exposes an address, a size, or a `Value`, and there is no arithmetic on either,
so a domain cannot form a pointer, cannot fabricate a slot, and cannot touch an
allocation it was not handed.

Because the domain never holds a `Value`, **a domain cannot keep an object
alive and cannot make one die early on its own.** Liveness stays with the
collector and the compiler's proof; the domain decides *storage policy*.

### 4.2 Reclamation happens on agreement, not on instruction

> A slot is destroyed when **both** the domain's policy says retire **and** the
> compiler's liveness proof says no owner can read it.

Either side alone is insufficient (a domain may be wrong; a proof may be
conservative and keep storage longer than needed), and the intersection is safe
in both directions: no use-after-release, and no unbounded leak beyond what the
domain's own capacity rules allow. This is deliberately the same shape as the
existing `Drop`-before-return / frame-teardown pair in `docs/mir.md`: two paths,
one agreed point.

Consequences worth stating in the language guide later:

- A domain that lies and retires early gets **nothing** — the proof vetoes it.
- A domain that never retires leaks its own arena, bounded and observable
  through `onCollect`/`exhausted`, and never corrupts the heap.
- So a hostile or buggy domain is a *performance* bug, not a memory-safety bug.
  That is the property that lets this be a safe-language feature at all.

### 4.3 Where the metadata goes

- `mir::ClassLayout` gains `MemoryDomainId domain{DomainGC}` and
  `mir::FieldLayout` gains `bool domainLocal{false}` (this field's value may
  only hold same-domain or owned storage). Interned ids, allocated in the
  module, printed by `printModule`/`printInstruction` (`include/zl/mir/printer.hpp`), preserved by every pass; a pass that
  rewrites an allocation must copy the id, and the verifier rejects a `Class`
  entry whose domain is not registered.
- `RuntimeTypeInfo` (`include/zl/vm/runtime_type.hpp`) gains the same id, next
  to the `OwnershipKind` it already carries per field, so runtime
  `new`/`CopyObject` dispatch can consult it without the compiler.
- MIR types stay domain-free. Domain is a property of the *class* and of the
  *allocation site*, not of the type identity — otherwise `Box<T>` in two
  domains becomes two types and the generic instantiation cache in
  `include/zl/compiler/generic_instantiation.hpp` starts keying on it.

## 5. Language surface

### 5.1 Declaration

`memory` opens a declaration exactly like `class`, with the contract above
validated as a compile error when a required method is missing, mis-typed, or
private. **This part is implemented (Phase 1, 2026-09-19):** the declaration
parses contextually, `TypeChecker::registerMemoryDeclaration` validates the
contract above against the exact signatures (acquire/release required;
reset/exhausted/onCollect optional-but-then-exact; each public; fields and
helpers private), and nothing emits the declaration - no layout, no dispatch,
no reflection, and `new` of a domain is refused naming the phase that removes
the refusal. One fixture per rule under `tests/zl/invalid/memory_tests/`; the
valid counterpart is `tests/zl/valid/memory_tests/MemoryContract/`. Both words parse **contextually**, and that is a decision rather than a detail.
The lexer has no context, which is why `shared`/`list`/`set`/`map` are keywords
that the parser still accepts as names (`isNameToken`, `Parser::checkName`,
`looksLikeTypedDeclStart` in `src/parser/parser.cpp`) and why
`looksLikeTypedDeclStart` disambiguates `shared int x` from `shared.push(1)` by
peeking at the next token. A `region` statement is recognised the same way -
`region` at statement start followed by a name and `{` - so existing programs
that hold a variable called `memory` or `region` keep compiling, and no token
needs to be added to the table at all.

A `memory` block may declare fields (its counters, its capacity) and private
helpers. It may **not** contain: `unsafe`-style pointer operations (there is no
raw-memory surface in ZL to contain), calls to `Thread.start`/`Task.spawn`,
`await`, or allocation into *itself*. Each is a specific error, not a general
"not allowed here".

### 5.2 Built-in domains

| Domain | Spelling | Policy | Notes |
| --- | --- | --- | --- |
| `gc` | *(implicit default)* | tracing, stop-the-world | unchanged; the migration path is `default → TracingGC`, exactly as the draft's §14 proposed |
| `arena` | `var r = new Arena()` (a `zl.memory` class, not a modifier) | release all at `region r { }` exit | the first non-GC domain; the one that earns the abstraction |
| `pool` | `var p = new Pool<Particle>(10000)` | recycle fixed-shape slots on capacity pressure | blocked on §9 Phase 0 — see the measurement gate |
| `rc` | *(user-written, later)* | counters kept by the policy object | **not a built-in.** `shared` is taken (§5.6), and nothing here needs `Retain`/`Release` opcodes: proof-driven release already gives the eager-free behaviour a counter would. If someone still wants counters — for a foreign object with its own attach/detach protocol — they write it as a `memory` block in Phase 6, which is the point of the feature |

### 5.3 Region blocks

```zl
import zl.memory.Arena

func build(Graph g): void {
    var scratch = new Arena()
    region scratch {
        var node = new Node()           // `Node` declares no domain, so the region's applies
        var tex  = new Texture() in gc  // explicit site override; Texture is long-lived
        g.add(node)                     // error R1: region value stored into a GC object
        g.add(promote(node))            // ok: an explicit copy, into g's domain
    }                                   // exit: reset(), destruction deferred to reclaim
}
```

`region <handle> { … }` is a **statement block whose body is inlined**, not a
closure and not a lambda:

- Not a closure: the checker forbids `owned`/`borrow` captures, and closure
  captures are always GC values (`docs/mir.md`). A lambda-bodied region could
  neither move an owned value in nor return a region value out — which is the
  entire use case.
- Exit runs on *every* path by reusing the `finally` finalizer-inlining pass
  (`return`, `break`, `continue`, throw, falling off the end), so there is one
  scope-exit mechanism in the compiler, not two.
- `region` nests; each level's `ExitRegion` runs innermost-first, matching the
  existing finalizer order.
- The draft's `with r { … }` is unavailable: `with` is the data-update
  operator (`point with { x: 1 }` — `DataUpdateExpr`,
  `src/parser/expression_parser.cpp`). Reusing it would make `expr with { }` and
  `region-of` ambiguous at statement start for no benefit.

`promote(x)` needs no new keyword: it follows the `share(...)` precedent - a
native-catalog entry (`src/compiler/native_catalog.cpp`, `SHARED_SHARE`) with a
matching special case in the checker and in `Compiler::compileCall`, not a lexer
token. `release(r)` - an early manual reset of the handle - would work the same
way, and is the escape hatch for the one case a block cannot express: a region
whose lifetime is not a syntactic scope.

### 5.4 Attachment and precedence

Type-level attachment uses the annotation that already parses onto
`ClassDecl::annotations` (`src/parser/parser.cpp`, `parseAnnotations`) but has
no consumer there yet: three annotation names are consumed today, each by the
code that cares about it - `"native"` (`src/compiler/compiler.cpp`), `"ffi"`
(`src/compiler/native_ffi_declarations.cpp`) and `"Override"`
(`TypeChecker::checkOverrideAnnotation`) - and a class-level annotation is read
by nobody at all.

```zl
@memory(ParticlePool)
class Particle { ... }
```

That annotation is only allowed to take a domain *name*, and it is only
meaningful with a registered domain behind it — see §5.5 first.

Resolution is total and never inferred:

```text
type-level @memory(...)  (final for that class, inherited by subclasses unless re-declared)
      else allocation-site `new T() in <domain>`
      else innermost enclosing `region` whose handle's domain accepts T
      else gc
```

Two rules make this deterministic where the draft was ambiguous: **a
type-level domain is final** (a scope can never re-route a class that declared
one), and **a region applies only to classes that declared none**. A `new T()
in D` where `T` declares `@memory(E)`, `D ≠ E`, is an error, not a precedence
contest.

### 5.5 Prerequisite: annotations need a rule before they carry semantics

**Landed 2026-09-19** - this section is the record of what exists, not a
request. The three problems it set out to fix, and where each fix lives:

- `parseAnnotations` accepted any identifier, so an unrecognised name was
  silently ignored - `@memroy(Pool)` compiled. Now the *name* is checked at
  the `@` itself against the one table of known annotations
  (`include/zl/parser/annotation_rules.hpp`), and an unknown name is a parse
  error listing the known set
  (`tests/zl/invalid/syntax_errors/UnknownAnnotation`).
- Each consumer matched a name ad hoc. That table is now the registry: five
  names, each with its allowed targets and a one-line note on what consumes
  it (`Deprecated`, `SuppressWarnings`, `Override`, `ffi`, `native`). Adding
  an annotation means adding a row, a consumer, and a fixture.
- Legal targets differed per declaration kind and the difference was written
  nowhere. Now the *target* is checked checker-side
  (`TypeChecker::validateAnnotationTargets`), where the declaration kind is
  fully known: a file-scope class takes only `TYPE_DECL` annotations; a
  constructor takes only `FUNCTION` ones, so `@Override` on a constructor is
  an error (`tests/zl/invalid/semantic_errors/AnnotationTargetConstructor`);
  a method takes `FUNCTION` and `METHOD`. Fields take no annotations (the
  parser's existing rejection).

The Phase 1 exit criterion this satisfies: an unknown `@` name is an error.
What this deliberately does *not* do is give any annotation a new meaning -
`@memory(...)` attachment (§5.4) is Phase 2 and is deliberately absent from
the table, because a name in the table with no consumer is exactly the
silently-ignored case this section exists to prevent. Until attachment lands
there is nothing to attach with, and no consumer to attach it to.

### 5.6 Words already in use

| Word | Current meaning | Use for domains |
| --- | --- | --- |
| `shared` / `Shared<T>` | thread-synchronised cell (`SharedCreate/Get/Set/WithLock`). As a *slot modifier* it is inert in lowering - `src/compiler/ir_lowering.cpp` picks `DefineLocal` on both sides of the ownership test - and maps to `BORROWED` at the native boundary (`src/compiler/native_compiler.cpp`) | **no.** The draft's `shared Object` = reference counting would redefine a keyword with a different contract, and every doc, example and the `SharedLostUpdateMeasurement` fixture would go stale. RC is also not worth it: boxes are already traced, and cycle handling is a project of its own. Note this leaves `shared` on a slot as spelling-without-consequence - worth its own small decision (implement it, remove it, or document it as "declares intent") |
| `with` | data-update operator | **no** (§5.3) |
| `owned`, `borrow`, `gc` | lifetime contract on storage | unchanged, orthogonal (§2) |
| `move` | move expression | unchanged; a move out of a region is rejected by R1, not by `move` |
| `memory`, `region`, `promote` | free identifiers | **yes**, contextual keywords |
| `unsafe` | **does not exist** (no token; the `UNSAFE` in `type_checker.cpp` is an unrelated cycle-analysis state) | not in v1. See §11: a domain needs no unsafe, because it never sees bytes |

## 6. Static rules

Six rules, all checkable from per-flow state the checker and the MIR verifier
already carry. Each is a `MemoryDomain` diagnostic with a note and a help line,
in the existing `error/note/help` style (`docs/language-guide.md` shows the
tone at the thread-capture message).

- **R1 — confinement.** A value in domain `D` may be stored only into: storage
  of a box whose `ClassLayout::domain == D`; an `owned` slot; a `MemorySlot`
  token field. Never into a GC object's field, a static, a closure capture, or
  a `Shared<T>` cell. This is the region-path rule extended to `FieldStore`,
  `StaticStore` and `MakeClosure` operands.
- **R2 — scope.** A region value may not be returned, thrown, stored in a
  field of a longer-lived box, or used after the region's exit point.
  `borrowRootOwner` already computes the root; a region-rooted borrow simply
  becomes an ordinary overlap conflict.
- **R3 — explicit escape only via `promote`.** `promote(x)` deep-copies into a
  destination domain and is the only legal way to lengthen a lifetime.
  Promotability is a per-class property computed from `FieldLayout`: a class
  holding a `NativeHandle`, `NativeBuffer`/`NativeStruct` view,
  `NativeCallback`, `Thread`, `Channel`, or any sync primitive
  (`Mutex/RwLock/Atomic/Semaphore/Condition`) is **not** promotable, and
  `promote` on it is a compile error naming the offending field. A class
  holding a closure is promotable only if every capture is; the existing
  `collectLambdaCaptureRefs` walk (`include/zl/compiler/thread_capture.hpp`) is
  what decides that, and it is exhaustive over `NodeKind` by construction, so a
  new node kind cannot silently fall out of it.
- **R4 — thread confinement.** A domain handle and its slots belong to one
  `ExecutionState`. Cross-thread transfer is not expressible: region values are
  not in the accepted capture set for `Thread.start`/`Task.spawn`, so R4 needs
  no new check — it is the existing one, with a clearer message when the
  rejected capture was a region value.
- **R5 — domain kinds.** Domains apply to the four box kinds only. Primitives,
  `string`, `bool`, and `Option`/`Result` payloads are not domain values;
  `validateOwnership` already requires a reference-like type, and the domain
  rule reuses that predicate (`isCollectableKind`).
- **R6 — reentrancy.** A domain's contract may not allocate in its own domain,
  may not collect, and may not be called while mutators are running except in
  the reclaim phase. Enforced by a runtime guard on the domain object plus
  "contract methods may not contain `new` of a class whose domain is
  `MemoryDomainId` of the enclosing declaration", checked statically.

## 7. MIR changes

Cheaper than the draft's seven new opcodes: **three new opcodes, one operand
field, one effect bit, one safety property.**

| Change | Detail |
| --- | --- |
| `Alloc` gains `MemoryDomainId domain` | `include/zl/mir/instruction.hpp`, `src/mir/instruction.cpp` (shape table), `src/mir/lowering.cpp` (reads `ClassLayout::domain` or the site override) |
| `EnterRegion(handle)` / `ExitRegion(handle)` | new; `ExitRegion` is emitted by lowering at every inlined block exit, exactly where finalizers already go |
| `Promote(src, dstDomainId)` | new; produces the copied value and is the only opcode that may cross domains |
| `Drop` unchanged | a region slot's release is `ExitRegion`'s job; `Drop` keeps its existing two spellings and its existing verifier meaning |
| Effects | `Allocation`, `Ownership`, `NativeResource` already exist in `include/zl/mir/effects.hpp`. New: `DomainLifecycle` (1 bit; `EffectSet` is `uint32` with 16 used). `EnterRegion`/`ExitRegion`/`Promote` are all **non-removable** (`MayThrow`-class effects), so the safety contract in `docs/mir-optimizer.md` holds without a new removal rule |
| Dataflow | `include/zl/mir/dataflow.hpp`'s ownership state records which slots are region-rooted; `FunctionVerifier::checkOwnershipFlow` rejects `FieldStore`/`StaticStore` of a region-rooted value into non-domain storage (R1) and any use after `ExitRegion` (R2) |
| `SafetyProperty::MemoryDomain` | added to `include/zl/mir/verifier.hpp`, reported in `src/mir/safety.cpp`, added to `docs/mir-safety.md`'s table with an honest "not covered" column: no interprocedural alias analysis, no leak-freedom proof |

Budget note for whoever schedules this: **one new opcode in this repo is about
nine files** — `instruction.hpp` (enum + comment), `instruction.cpp`
(`opcodeName` + shape table), `effects.cpp`, `verifier.cpp`, `dataflow.cpp`,
`lowering.cpp`, `reachability.cpp`, `vm_backend.cpp`, `builder.{hpp,cpp}` — plus
the printer, `docs/mir.md`, the differential harness, and the native backend's
fail-closed list. Three opcodes is therefore ~28 touchpoints for the MIR layer
alone. The draft's `Alloc/Dealloc/Retain/Release/EnterRegion/ExitRegion/
Promote/MemoryFence` was ~60+.

## 8. Runtime changes

- `MemoryDomainRegistry` (`src/vm/memory_domain.cpp`): interned
  `MemoryDomainId` → a `DomainVTable` of four/five function pointers that call
  into ZL closures through the same guarded path `CallbackInvoke` already uses,
  plus a `BoxPool`-shaped storage policy behind a small internal interface.
  The interface, not the registry, is what `makeGC*` calls.
- The `makeGC*` gateway becomes the single dispatch point:
  `makeGCObject(classId)` consults the domain, defaulting to `TracingGC`. No
  new allocation entry point is allowed; `tools/boundary_lint.sh` gets a rule
  for it (a file other than `gc.cpp`/`memory_domain.cpp` calling
  `std::make_unique<ListBox|MapBox|ObjectBox|ClosureBox>` fails), so the
  gateway stays a gateway.
- Deferred destruction: a domain's retire list is appended to the
  `Collection`-shaped deferred vector, so a destructor that joins a thread or
  unregisters a VM never runs during the stop-the-world phase.
- GC integration: domain boxes are **still traced while live**. A region box is
  in the registry, reachable from the frame, and simply dies earlier — that is
  the whole interoperability story, and the reason no write barrier is needed.
  `onCollect` exists for counters, not for liveness.
- Per-`ExecutionState` domains, next to `StaticRuntimeStorage`, with
  `protect/unprotect` pinning for anything an embedder must hold.

## 9. Phases

Each phase states the gate that keeps the repo honest: examples stay 50/50,
`tools/mir_backend_diff.sh` keeps its 24 matched / 7 known gaps,
`tools/boundary_lint.sh` passes, all C++ suites pass, and the numbers in §10
are written into `research/report.md`.

| # | Phase | Contents | Exit criteria |
| --- | --- | --- | --- |
| 0 | **No language surface.** Make the current model pay, then measure it | (a) `ObjectBox::fields` → flat `std::vector<Value>` + per-class `name → index`, since a `std::string` key per field read is the actual per-object cost; (b) recycle freed boxes from the existing `Collection::retired_` vector instead of destroying them; (c) growth policy on `allocationThreshold_`; (d) a real allocation benchmark in `benchmarks/`; (e) give `gc/owned/borrow/shared` their first `.zl` fixture pair and an example (the doc corrections to `mir.md` and `language-guide.md` already landed) | a measured baseline table: `new` throughput, bytes/object, and % of run time in `collect`, before and after. **Gate:** if a pool or arena cannot beat (a)+(b)+(c) on the benchmark corpus, domains stay a design and the numbers say why |
| 1 | Annotation rules + contract skeleton | unknown-annotation error, per-annotation target allowlist; `memory` declaration parses and validates the contract with no consumer yet | a `memory` block with a missing `release` is a compile error with a location; an unknown `@` name is an error |
| 2 | `MemoryDomainId` metadata | `ClassLayout`/`FieldLayout`/`RuntimeTypeInfo` fields, `Alloc.domain`, printer, `default → TracingGC` migration layer, verifier rejects unregistered ids | all existing tests pass with zero behaviour change; a `@memory`-annotated class round-trips through `--mir` output |
| 3 | `arena` built-in + `region` blocks | registry, box factory, `EnterRegion`/`ExitRegion` + finalizer reuse, R1/R2/R5/R6, diagnostics | arena allocation works; region cleanup is automatic on all five exit paths; escaping a region value into a field, a static, a capture, or a `Shared<T>` is a compile error with a help line |
| 4 | `promote` | promotability from `FieldLayout`, graph copy on the `CopyObject`/`data` path, non-promotable diagnostics, R3 | short-lived values become long-lived only through a named operation; the `NativeHandle` case is rejected with the field named |
| 5 | `pool` | fixed-shape slot recycling, capacity, `exhausted`, per-shape stats | exhaustion is defined, testable, and benchmarked against Phase 0's free list — not against an unimproved GC |
| 6 | **User-written domains** (the point of the feature) | `memory` declarations callable as policies: contract dispatch, reentrancy guard, capability list of what a contract body may call, sample custom domain + a `debug` domain that asserts the agreement rule | a ZL-defined domain allocates and reclaims through the same machinery as `arena`; a domain that lies about liveness cannot cause a use-after-release (fixture: adversarial domain, expected result "proof vetoes") |
| 7 | Optimisation + stabilisation | `retain`-style redundancy elimination where a pass can prove no observable transition, region-entry merging, `MemoryDomain` in the safety tables, `zl.memory` stdlib, freeze diagnostics wording | documented in the language guide, `docs/mir.md`, `docs/mir-safety.md`; every rule R1–R6 has a `.zl` fixture pair (accept + reject) |

Phase 6 is deliberately last. A programmer-written domain is only sound once
Phases 3–4 have proved the agreement rule against the runtime's own domains.

Progress: Phase 0 is measured and closed (§10.1, 2026-09-19). Phase 1 is
landed (2026-09-19): the §5.5 annotation rules and the §5.1 contract
skeleton - unknown-annotation error, per-annotation targets, `memory` parsing
and contract validation, with no consumer. Phases 2–7 remain design.

## 10. Benchmarks (Phase 0's gate)

Measure, in the style of `research/report.md` — numbers, machine, dates:

```text
allocation:  new Class() in a loop, N ∈ {1e5, 1e6}, fields ∈ {1, 8}
release:     region exit vs GC-triggered sweep
peak memory  RSS per run of the same program under gc / arena / pool
GC pressure  collect() invocations and total trace time, per benchmark
interference a long-lived graph that is traced with N region-resident objects inside
```

The point is not that a pool beats a collector; it is that the *language*
feature has to beat the *implementation* improvements from Phase 0, because
otherwise the complexity belongs in the runtime, not in the source language.

### 10.1 Phase 0 baseline, measured 2026-09-19

Workload: `benchmarks/AllocationBenchmark.zl`, driven one process per
configuration by `benchmarks/run_allocation_benchmark.sh` (per-child peak RSS
via `wait4`). Two runs per configuration, averaged. Machine: 2-core x86-64,
3.9 GB RAM, Linux, g++ 12, `-O2` (`Release`), single-threaded VM.

- *keep*: allocate N objects (N ∈ {1e5, 1e6}) and retain them all in a `List`;
  the live heap grows to N, so peak RSS is the memory cost of N kept objects.
- *churn*: allocate 1e6 objects in 1024-object batches and drop each batch;
  the live heap stays ~1 KB, so the whole allocate→trace→sweep→recycle cycle
  is what is timed.

"Before" is `3f1a737` (pre-Phase 0) plus the measurement counters and
`GC.stats` only — no flat fields, no free list. "After" is this branch:
(a) flat `ObjectBox::fields` + per-class `name → index`, (b) recycled-box free
lists in `Collection::reclaim()`, (c) the existing `allocationThreshold_`
growth policy (present in the baseline; verified, not added).

| workload | alloc/s before → after | Δ | collect share before → after | peak RSS before → after | Δ |
| --- | --- | --- | --- | --- | --- |
| one-field, keep 1e5 | 133.4k → 135.4k | +2% | 1.1% → 1.1% | 80.9 MB → 66.5 MB | −16% |
| one-field, keep 1e6 | 129.5k → 120.8k | −7% | 4.3% → 4.2% | 682.3 MB → 559.3 MB | −18% |
| one-field, churn 1e6 | 146.8k → 141.5k | −4% | 1.4% → 1.8% | 18.4 MB → 18.3 MB | −3% |
| eight-field, keep 1e5 | 53.6k → 55.3k | +3% | 1.1% → 0.7% | 153.8 MB → 98.0 MB | −36% |
| eight-field, keep 1e6 | 53.8k → 55.2k | +2% | 4.2% → 2.8% | 1410.5 MB → 874.4 MB | −38% |
| eight-field, churn 1e6 | 56.8k → 55.2k | −3% | 0.9% → 0.9% | 19.0 MB → 17.8 MB | −6% |

Collections per run are unchanged on both sides (4 / 7 for keep, 920 for
churn); the threshold grows to ~2× the live heap, which is why keep 1e6 costs
only 7 collections. Peak RSS per kept object (Δ between keep 1e5 and keep
1e6): **one-field 703 B → 574 B (−18%), eight-field 1464 B → 905 B (−38%)**.
In churn, the free list sees 1,001,377 of 999,424 measured-phase hits
(`reused` from `GC.stats()`); the small surplus is the 16,384-box warm-up
pool drained during the first measured batches. Raw rows:
`benchmarks/results/allocation_{before,after}{,2}.json`.

Reading the table:

- **The memory win is (a), the flat fields.** The per-object
  `std::unordered_map<std::string, Value>` and its string keys are the
  130–560 B that keep-RSS loses per object, and it is the same walk that makes
  the eight-field keep collect share drop 4.2% → 2.8% (tracing a vector, not
  a hash table).
- **Throughput is neutral.** Every Δ is inside this machine's two-run
  variance (keep 1e5 swings ±25% wall time between runs); the one −7%
  (one-field keep 1e6) does not repeat run-over-run. Nothing here buys
  allocation *speed*.
- **Recycling (b) is not a throughput win on this corpus either.** Churn
  throughput is flat (−4% / −3%) even though the pool hit rate is ~100%:
  the per-allocation cost is the registry `push_back`, the refcount and the
  safepoint budget, not the `make_unique`/destructor of the box. What (b)
  does is remove the per-box heap traffic, which shows up only as the −0.4 pp
  extra collect share churn pays for pool maintenance.
- **(c) was already there.** The growth policy ships in the baseline; the
  table measures with it on both sides, which is what makes the "before"
  row fair.

**Gate verdict.** (a)+(b)+(c) buys −16…−38% memory per kept object and a
cheaper sweep of wide objects, at neutral throughput. What the numbers do
not touch is exactly what domains exist for: `owned` still frees nothing
until the collector's schedule notices (P2-8's original complaint), and the
4–7% collect share of keep 1e6 is the price of 1e6 live objects — a
region-scoped graph pays that on every exit, eager or not, until a domain
reclaims at scope exit. A Phase 5 `pool` must beat a 100%-hit free list plus
a growth threshold on *this* corpus; this table is the bar, and on the
corpus as written, recycling alone cannot claim the throughput win — the
arena's eager release, not the pool, is the part with a number to chase.

## 11. What this deliberately does not do

- No raw bytes, pointer arithmetic, or `unsafe`. A domain that cannot see an
  address cannot create a wild pointer, so the feature needs no unsafe boundary
  to be sound — and adding `unsafe` as a language feature is a separate design,
  not a sub-task of this one.
- No `free`/`delete` in user code, no manual refcount ops, no "attach this
  object to that allocator". §1.
- No RC domain in this plan (§5.6).
- No second collector over the same graph. Domain boxes are traced boxes with a
  shorter life; that is the only interoperation model here.
- No native/AOT codegen for domains in v1. `TASKS.md` P1-6 records that the
  native backend has no GC maps, no safepoints, and no refs/objects in its
  subset; retain/release or region cleanup in native codegen needs precisely
  those. `EnterRegion`/`ExitRegion`/`Promote` therefore **fail closed** in
  `--backend native`, the same convention `FfiCall` uses on the VM path.
- No platform allocator domains (Windows `HeapAlloc`/`VirtualAlloc`, Android
  native heap, mmap regions). Those live behind the FFI boundary's registry
  contract once `unsafe` exists; §8's registry is the seam, and nothing in the
  language core needs to know they exist.
- No multiple domains per object, no domain inference, no domain in the type
  spelling of a generic argument.

## 12. Risks, ranked

| Risk | Why it is a risk here | Mitigation in this design |
| --- | --- | --- |
| `fields` refactor (Phase 0a) touches everything | `ObjectBox::fields` is read by the VM, natives, reflection, serialization and `ClosureBox::captured` shares the shape | do it first, alone, with no language change, so the diff is reviewable and the benchmark proves it |
| Region values in a long-lived graph | R1 is the whole safety story; a missed store path is a dangling read | enumerate store targets from the verifier's shape table (every opcode with a slot/field destination) rather than from the lowering code; `boundary_lint` can't help, but a "no opcode writes storage without a domain rule" verifier test can |
| Contract dispatch cost | one VM call per `acquire`/`release` on a path that is currently one `make_unique` | measured in Phase 0; if it dominates, the fix is a MIR inlining pass for contract methods, not a language change |
| Stop-the-world interaction | a domain that mutates during trace would be a data race with the collector | contract methods are invoked at safepoints or in the reclaim phase only, never inside `Marker::trace` |
| Complexity in the language | ten domains on day one makes ZL harder to learn, which the draft's §25.3 correctly worried about | `gc` stays the silent default; `arena` then `pool`; `memory` blocks are an advanced surface with a stdlib-quality example set |

## 13. Testing map

The draft's categories were right; here is where they live.

- Parser: `tests/` lexer/parser suites — `memory` declaration, contextual
  `region`, `new T() in D`, malformed contract.
  (Phase 1, 2026-09-19: the `memory` declaration and malformed-contract
  parser fixtures exist - `tests/zl/invalid/memory_tests/MemoryStaticMember`,
  `MalformedMemoryDecl`; the source-level ownership fixtures are
  `tests/zl/valid/ownership_tests/` and `tests/zl/invalid/ownership_tests/`.
  `region` and `new T() in D` arrive with Phase 3.)
- Type checker: `tests/zl/invalid/type_errors/`, `.../semantic_errors/` and
  (for the domain contract) `.../memory_tests/`
  `.zl` fixtures, one per rule, each the minimum number of declarations needed
  to fail that rule and nothing earlier. (Phase 1, 2026-09-19:
  `memory_tests/` holds one fixture per contract rule; the annotation
  registry's rejections live in `syntax_errors/UnknownAnnotation` and
  `semantic_errors/AnnotationTarget{Class,Constructor}`.) Baseline to fix alongside this: the
  existing ownership surface has **no `.zl` fixture in any gate and no example**
  - `owned`/`borrow`/`move` are tested only at MIR level
  (`tests/mir_ownership_tests.cpp`), and the one `.zl` program that uses them,
  `research/corpus/Ownership.zl`, is referenced by no build, test or script. So
  Phase 0 adds that fixture pair first: everything downstream is checked with
  the language's own tests, and today there are none to extend.
  (Done 2026-09-19: `tests/zl/valid/ownership_tests/OwnedBorrowShared.zl`
  covers `gc`/`owned`/`borrow`/`shared` in one verified program; the rejects
  `tests/zl/invalid/ownership_tests/UseAfterMove.zl` and
  `MoveWhileBorrowed.zl` pin the two dataflow diagnostics; the example is
  `examples/advanced/OwnershipBasics.zl` in the byte-compared gate.)
- MIR: `tests/mir_region_tests.cpp` (new `zl-mir-region-tests` target, declared
  explicitly in `CMakeLists.txt` beside `zl-mir-ownership-tests`), asserting
  `Alloc{domain}`/`EnterRegion`/`ExitRegion`/`Promote` ordering, and that no
  pass may delete or reorder them.
- Runtime: extend `tests/gc_lifetime_tests.cpp` and `tests/gc_safepoint_tests.cpp`
  with region-exit-during-collection, deferred destruction of domain boxes,
  adversarial-domain veto, and `native_resource_tests`-style exhaustion cases.
- Concurrency: `tests/zl/valid/concurrency_regressions/` gets the R4 pair (a
  region value rejected at a thread boundary, with the improved message).
- Differential: `tools/mir_backend_diff.sh` must stay 24/7; a domain program is
  a *new* known gap until the native backend fails closed deliberately.
- Examples: `examples/advanced/MemoryArenas.zl` + a `zl.memory` example with a
  hand-written domain, both in the 50/50 verified-output gate.

## What changed from the draft

Every line here is a draft claim that the code contradicted.

| Draft | This document |
| --- | --- |
| §14 "refactor the GC behind a `MemoryDomain` abstraction" as new work | Phases 1–2 already exist as the `OwnershipKind` axis, plumbed parser → checker → MIR → runtime, with verifier rules. The plan now adds the *orthogonal* axis instead of rebuilding the one that's there |
| §4 `MemoryDomain::allocate(size)/release(block)` | §4.1 typed `acquire(MemoryShape)/release(MemorySlot)` — because allocation here is `make_unique<ObjectBox>` in a global registry, and boxes own RAII payloads that a chunk reset cannot free |
| §5.3 "`shared Object`" = reference counting with `Retain/Release` | cut. `shared` means the thread-synchronised cell; nothing refcounts boxes. §5.6 |
| §9/§12.2 general escape analysis | R1–R3 storage restrictions on top of the region paths the checker already computes (`borrowRegionForExpr`, `regionOverlaps`); `docs/mir-safety.md` says outright there is no heap alias analysis, and this plan does not pretend otherwise |
| §5.4 `with FrameArena { … }` | `region` block, inlined — `with` is `DataUpdateExpr`, and a closure body can't hold `owned`/`borrow` values |
| §7 `@memory(...)` attachment works | needs the annotation allowlist + unknown-annotation error first (Phase 1); class annotations parse but nothing consumes them, and annotations on fields are rejected |
| §16 `unsafe { }` as an existing boundary | no such token; and §11 shows domains don't need one, since they never receive an address |
| §6 inventing a custom-domain contract | §4.1 generalises the *existing* handle/callback registry (`native_resource.hpp`, `HandleBorrow/Consume/Close`), which already implements opaque token + exactly-once release + borrow-invalidated-on-close |
| nothing about threads | §6 R4, §8 (per-`ExecutionState`, counted pinning, safepoint-only contract calls, deferred destruction) |
| §11 seven new MIR ops | §7 three ops + one operand field, with the ~9-file-per-opcode touchpoint cost stated |
| §21 benchmark list with no baseline | §10 + Phase 0's gate: domains must beat the runtime's own easy wins or they don't enter the language |
| §12.1 `explicit > scope > type > default` precedence (conflicts with its own §25.1) | §5.4: type-level is final; scope applies only to classes that declared none; contradictory combination is an error |
| §15 platform allocator domains as a phase | §11 non-goal until `unsafe` exists as a feature |
| inheriting `docs/mir.md`'s "`shared` storage is reference-counted elsewhere" | that claim is false against the runtime (nothing counts a managed box; `DropVar` → `dropLocal` only erases the local). Corrected in `docs/mir.md`, and `docs/language-guide.md` now says what `owned`/`borrow` actually buy you - both in this change |
| no documentation of the existing `gc`/`owned`/`borrow`/`shared` modifiers | `docs/language-guide.md` "Memory-model direction" describes them for the first time, instead of this plan being the only place they appear |
