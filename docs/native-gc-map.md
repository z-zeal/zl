# The native frame GC map

The native tier can carry managed references through parameters, locals, call
arguments and results. Calls now pass through a driver bridge that registers
each active native frame, polls the safepoint coordinator, and publishes typed
roots from emitted frame maps. Explicitly bound `CallRuntime` helpers can
allocate, and a forced-collection regression proves a native-only root keeps
such an object alive. Non-generic data records and simple non-inherited classes
with trivial constructors use that bridge for allocation and typed primitive or
nested-object fields; class primitive fields receive zero defaults. Field
initializers, nontrivial constructors, inheritance, and dynamic method behavior
remain outside the subset.

This document records the native root model, frame metadata, conservative
call-site maps, the live runtime bridge, and the remaining steps to make
language-level objects useful. It argues from the code as it is, the way
[memory-domains.md](memory-domains.md) does.

- [The rule the tier already keeps](#the-rule-the-tier-already-keeps)
- [What a GC map is](#what-a-gc-map-is)
- [What exists today](#what-exists-today)
- [Phases](#phases)
- [Risks](#risks)
- [Testing map](#testing-map)

## The rule the tier already keeps

`ValueClass::Reference` is deliberately coarser than the ZL type lattice: a
`string`, a `list<int>`, a `Point` and an unknown type are all the same
question for the machine — *where does this live*, and the answer is "a
pointer-width slot that the collector must be able to find".

Selection keeps that distinction honest in one direction only:

| | |
| --- | --- |
| **Carried** | a parameter, a local slot, a block parameter, a return value, a call argument, a call result — moved, stored, loaded, returned |
| **Used** | arithmetic, comparison, equality, general class fields/methods, `new`, constants — non-generic data records and simple non-inherited classes with trivial constructors support typed fields |

Carrying is in (2026-11-22). Using remains narrow and refused by name, per
function, with the MIR opcode and source line: arithmetic/equality refuse
references explicitly, while allocation and the supported field operations
are handled through the managed-object bridge.

That boundary is the load-bearing fact for the phases below: reference carry
alone does not create an allocation operation. The data-record and simple-class
subsets allocate and access fields through runtime helpers, so allocation and
native root publication remain one coupled capability; otherwise a later
safepoint could reclaim an object still held by a native frame.

## What a GC map is

A GC map answers one question at one moment: **at this instruction, which
places in this frame hold a reference the collector must trace?**

Two pieces of information are needed.

1. **The frame map** — which frame offsets can hold references. `layoutFrame()`
   assigns every `FrameSlot` (marked by `FrameSlotInfo::isReference`) and every
   vreg spill (marked by `VRegInfo::cls`) a negative offset from `rbp`.
   `EmittedFunction::frameLocations` exports those IDs, offsets and reference
   bits with the emitted code.

2. **The liveness** — which of those reference locations are *live* at a given
   instruction. The current conservative maps report every reference location
   at every call. A dead slot may hold a stale pointer; tracing it is safe for
   correctness but pins objects and grows the root set. A slot that is live and
   *not* reported is a collection bug: the object is reclaimed while the frame
   still points at it.

Safepoints are instructions where a collection rendezvous can happen. In this
tier every `Call` and `CallRuntime` is a safepoint (`lirOpcodeIsCall` says a
call is "a safepoint, a clobber of every caller-saved register, and the only
place a GC map is required today"). Explicitly bound runtime allocation is
covered by this protocol: allocation may request collection, and the next
native call publishes the now-spilled result before rendezvous. If allocation
is later emitted inline, that instruction must itself become a safepoint; a
backward edge may also need one so a long loop can be interrupted.

The driver's half is `GCSafepointCoordinator` (`include/zl/vm/gc_safepoint.hpp`):
a participant publishes a `GCRoots` snapshot when it parks. A native frame
must register as a participant for the duration of the call and publish the
references its GC map says are live at the safepoint it is parked at.

## What exists today

| Piece | Where | State |
| --- | --- | --- |
| Reference value class | `include/zl/native/target.hpp` | exists, documented as "must be reported to the collector at any safepoint it is live across" |
| Slot flagged as a reference | `FrameSlotInfo::isReference` (`include/zl/native/lir.hpp`), set in `newSlot` (`src/native/select.cpp`) | exists, printed as `gc-root` by `printLirFunction` |
| "A call is the only place a GC map is required today" | `lirOpcodeIsCall` (`include/zl/native/lir.hpp`) | exists |
| Frame layout | `FunctionEmitter::layoutFrame` (`src/native/emit.cpp`) | exported as `EmittedFunction::frameLocations`, including LIR slots and vreg spills |
| Conservative call-site maps | `EmittedFunction::safepoints` | exported by CALL opcode byte offset; includes every reference location |
| Safepoint coordinator | `src/vm/gc_safepoint.cpp` | exists, used by the VM |
| Driver/runtime bridge | `src/native/exec.cpp`, `src/vm/native_gc_bridge.cpp` | registers a participant per call, publishes native roots at safepoints, binds object alloc/field helpers, and supports typed borrowed reference entry |

The first allocation-and-publication path is live as one bundled milestone.
The ordering is intentional: runtime allocation is not accepted independently
of root publication, because each newly allocated reference must survive a
collection at a later native safepoint. Non-generic data records and simple
classes now exercise this path. Remaining work is field-initializer expressions,
nontrivial constructors, inheritance layout, dynamic method dispatch, and
mixed-mode execution.

## Phases

Each phase has an exit gate. A phase is not done when the code compiles; it is
done when the gate's test fails on the old behaviour and passes on the new.

### Phase 1 — carry a reference · **landed 2026-11-22**

A reference may be a parameter, local, block parameter, return value, call
argument or call result. Every *use* is still refused.

**Gate:** `testReferencePassThrough` in `tests/native_backend_tests.cpp` —
`identity`, `viaSlot` and `wrap` compile, emit, are mapped executable and
round-trip a pointer unchanged; `same` (reference equality) is refused with a
reason. Before the change, the first three were refused.

### Phase 2 — export the frame map · **landed 2026-11-22**

`EmittedFunction` exports `frameLocations`: every LIR `FrameSlot` and every
vreg spill has its ID, negative `rbp` displacement, and reference bit, alongside
the existing total frame size. The emitter also initializes reference-bearing
locations to null in the prologue; this makes it safe for a conservative map to
include a spill before its first definition.

**Gate:** `testEmittedFrameMap` reads a reference slot's offset from the exported
layout and verifies the emitted `Load` and `Store` use that exact displacement.
It also checks the reference bits and frame bounds for every LIR slot and vreg
spill, and verifies a reference spill offset appears in emitted memory access.

### Phase 3 — compute the GC map at call sites · **landed 2026-11-22**

Each emitted call now has a `SafepointMap`, keyed by the byte offset of the CALL
opcode. The initial map is deliberately conservative: it reports every
reference-bearing frame location, including stale values and null-initialized
spill slots. This is sound but can pin objects; liveness filtering remains a
future optimization after measurement.

**Gate:** `testEmittedFrameMap` compiles a function with two calls and asserts
that each map points to its own emitted CALL opcode and relocation, and includes
every reference-bearing location. The reference slot Load/Store displacement
test ensures the metadata maps to the frame the bytes actually use.

### Phases 4 + 5 — runtime allocation and root publication · **landed 2026-11-22**

These two phases are one acceptance unit, not independently shippable steps:
allocation can create a managed pointer before the native frame has returned,
so a later collection must see that pointer in a published root map. The driver
binds explicitly supplied `CallRuntime` symbols (unknown runtime symbols still
fail closed). Emitted `Call` and `CallRuntime` instructions are wrapped by an
absolute-address enter/leave bridge. `NativeExecutable` registers a coordinator
participant for the call, tracks the active native frame chain, and polls
before each target call. When collection is pending, the bridge reads the active
safepoint maps, resolves non-null raw addresses against the GC's
managed-allocation registry, and publishes typed `Value` roots. Reference entry
is available through the borrowed-pointer API; the CLI still refuses to invent
a managed pointer from `--int64` or `--double` arguments.

**Gate:** `testNativeSafepointRootPublication` binds a runtime allocator that
returns a new object into a native spill, then binds a pressure helper that
allocates past the GC threshold. The next native call polls and collects while
the only root to the object is the native frame; the test verifies the object
survives and is returned unchanged.

### Phase 6a — non-generic data records · **landed 2026-11-22**

Data literals allocate GC-managed `ObjectBox` instances through the internal
runtime bridge. Field loads and stores support `int`, `bool`, `double`, and
nested non-generic data records. Class/field names travel through the LIR's
runtime-string table; helpers validate object addresses against the managed
allocation registry and store fields through the VM's tracing-aware object
field accessors. Constructors, field defaults for ordinary classes, inheritance
layout and dynamic dispatch remain refused.

**Gate:** `testNativeDataRecordFields` compiles real ZL source, executes
integer/bool/double field accesses and nested records, and forces collection
between writing and reading a nested field while the parent record remains in
the native frame. The maps are deliberately conservative, so this checks the
integrated path rather than claiming liveness precision.

### Phase 6b — simple class fields · **landed 2026-11-22**

Non-generic classes with no inheritance and an empty/trivial constructor can
allocate through the same bridge. Primitive and nested supported-object fields
use the typed field helpers; primitive defaults match the VM (`0`, `0.0`, and
`false`). The generated default stores are themselves safepointed calls, so the
new object stays rooted while its layout is initialized. Field initializers,
nontrivial constructors, inheritance, and methods remain refused.

**Gate:** `testNativePlainClassFields` verifies primitive defaults, nested class
fields, and collection while the child is reachable through the parent. It
complements `testNativeDataRecordFields` rather than replacing its data-literal
coverage.

### Phase 6c — complete class execution and mixed mode · **open**

Field-initializer expressions, nontrivial constructors, inheritance layout,
method dispatch, and full-program native execution remain. This is where
`docs/memory-domains.md` and ownership work start to matter.

**Gate:** a named program with heap objects and collections runs end-to-end
through `--backend native` with a measured wall time against the VM — P1-6's
original done-when. The supported object slices above are progress toward, not a
replacement for, that acceptance criterion.

## Risks

1. **A map that is a subset of the live set is a use-after-free.** The
   conservative-first ordering exists so that the first map can only
   over-report. Never ship a liveness pass without the superset test.
2. **The frame map and the bytes drifting apart.** `layoutFrame` is the single
   source of truth; `testEmittedFrameMap` checks exported offsets against the
   actual encoded loads and stores.
3. **Unknown native pointers must not become roots.** Root publication resolves
   addresses against the GC's live allocation registry and ignores untracked
   pointers rather than guessing their box type.
4. **Conservative roots pin the whole frame.** Safe but potentially costly:
   stale values remain roots until the frame exits. Measure collection
   frequency and retained-object counts before adding liveness filtering.
5. **A long call-free native loop delays rendezvous.** The bridge polls at calls;
   if native loops gain allocation or become long-running, back-edge polls will
   be needed so another participant's collection cannot wait for function exit.

## Testing map

| Phase | Gate | Harness |
| --- | --- | --- |
| 1 | reference carry round-trips; a use is refused | `tests/native_backend_tests.cpp` |
| 2 | exported offsets match the emitted displacements; all slots and spills are represented | `testEmittedFrameMap` in `tests/native_backend_tests.cpp` |
| 3 | conservative roots cover all reference locations; two call sites are keyed by opcode offset | `testEmittedFrameMap` in `tests/native_backend_tests.cpp` |
| 4 + 5 | a runtime-allocated object survives forced collection with its only root in native spills | `testNativeSafepointRootPublication` in `tests/native_backend_tests.cpp` |
| 6a | data-record fields execute; nested records survive forced collection | `testNativeDataRecordFields` in `tests/native_backend_tests.cpp` |
| 6b | a named program runs native end-to-end, measured | `tests/native_exec_parity.py`, `research/report.md` |

Every phase also carries the standing gates: `ctest -C Release`, the
regression fixtures, the examples byte-compared, and the native gate
(`scripts/native_gate.sh`).
