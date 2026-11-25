# The native frame GC map

The native tier can now *carry* a managed reference: a parameter, a local, a
block parameter, a return value, a call argument or a call result moves a
reference through native code without the collector ever needing to know. That
is the first half of P1-6's remaining work, and it is safe for a precise
reason: **the tier cannot allocate and cannot call the runtime, so no
collection can run while a native frame is live.**

This document records the native root model, the frame metadata and
conservative call-site maps already exported, and the remaining steps to make
them live at runtime. It argues from the code as it is, the way
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
| **Used** | arithmetic, comparison, equality, field access, method calls, `new`, constants — everything that would need the runtime or the object layout |

Carrying is in (2026-11-22). Using is refused by name, per function, with the
MIR opcode and the source line — `selectBinary` and `Neg` refuse references
explicitly, and everything else falls through to the default arm of
`selectInstruction`.

That boundary is the load-bearing fact for the phases below: a reference the
tier can carry but not use cannot reach a collector, because nothing it holds
can trigger one.

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

The safepoints are the instructions where a collection could be requested. In
this tier that is exactly the call sites (`lirOpcodeIsCall` says so: a call
is "a safepoint, a clobber of every caller-saved register, and the only place
a GC map is required today"). If allocation ever enters the tier, every
allocation point becomes a safepoint too, and a backward edge may need one so
a long loop can be interrupted.

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
| Driver | `src/native/exec.cpp` | maps and calls the subset; refuses reference signatures |

So the raw material and advisory maps are in place. What is still missing is
making allocation a real safepoint and having the native driver participate
and publish the reported roots.

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

### Phase 4 — make allocation real, which makes the map mandatory

The map is advisory until the tier can trigger a collection. The smallest
honest step is a `CallRuntime` that allocates — a runtime helper taking a
size and returning a reference — which turns the call site into a real
safepoint and makes "the tier cannot allocate" false.

**Gate:** a native function that allocates in a loop, run under a driver that
publishes roots, with a fixture that forces collection pressure and asserts
the object allocated before the call is still alive after it. Without the
map, this is the test that fails by construction.

### Phase 5 — the driver publishes roots

`NativeExecutable` registers a safepoint participant for the duration of a
call and, at each safepoint inside the native frame, publishes the references
the GC map reports. The driver must also be able to *supply* references,
which the CLI cannot do: `--run-native` has `--int64` and `--double`, and a
managed reference cannot be invented from the command line. The backend tests
can (they already call entries with raw pointers), so the driver's
participation belongs in the test harness, not the CLI.

**Gate:** the Phase 4 fixture run through the real driver, with the safepoint
participant registered; a counter in the test's runtime helper asserts the
collector saw the native frame's roots.

### Phase 6 — objects, fields, and the rest

With references carried, allocation real and roots published, field access and
object layout stop being theoretical. This is the phase that makes the native
tier a candidate backend rather than a demonstration, and it is where
`docs/memory-domains.md` and the ownership work start to matter.

**Gate:** a named program with a heap object runs end-to-end through
`--backend native` with a measured wall time against the VM — P1-6's original
done-when.

## Risks

1. **A map that is a subset of the live set is a use-after-free.** The
   conservative-first ordering exists so that the first map can only
   over-report. Never ship a liveness pass without the superset test.
2. **The frame map and the bytes drifting apart.** Phase 2's gate exists
   because `layoutFrame` is the single source of truth and nothing currently
   checks the consumer reads it.
3. **Publishing roots the collector cannot use.** `GCRoots` holds `Value`s;
   a native frame holds a raw pointer. The translation is a `Value` that
   borrows the object, and its lifetime must be the call, not the frame's.
4. **Conservative roots pin the whole frame.** Acceptable for correctness, but
   it can change collection behaviour on the corpus; measure before and after
   Phase 3, the way [research/report.md](research/report.md) measures
   everything else.

## Testing map

| Phase | Gate | Harness |
| --- | --- | --- |
| 1 | reference carry round-trips; a use is refused | `tests/native_backend_tests.cpp` |
| 2 | exported offsets match the emitted displacements; all slots and spills are represented | `testEmittedFrameMap` in `tests/native_backend_tests.cpp` |
| 3 | conservative roots cover all reference locations; two call sites are keyed by opcode offset | `testEmittedFrameMap` in `tests/native_backend_tests.cpp` |
| 4 | allocation forces collection; the prior object survives | `tests/native_backend_tests.cpp` + a runtime helper |
| 5 | the driver's participant publishes the roots | same, through `NativeExecutable` |
| 6 | a named program runs native end-to-end, measured | `tests/native_exec_parity.py`, `research/report.md` |

Every phase also carries the standing gates: `ctest -C Release`, the
regression fixtures, the examples byte-compared, and the native gate
(`scripts/native_gate.sh`).
