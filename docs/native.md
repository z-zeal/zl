# Native Boundary, Compilation, and FFI

- [The boundary rule](#the-boundary-rule)
- [Native compilation (`@native`)](#native-compilation-native)
- [Native bindings (`zl-bind`)](#native-bindings-zl-bind)
- [FFI](#ffi)
- [Native export registry](#native-export-registry)

## The boundary rule

A permanent design rule for the project:

> **C++ is the small, stable native foundation; ZL owns high-level library policy and
> composition.**

Future C++ → ZL migrations must be benchmarked before a performance-sensitive native
implementation is removed. The `benchmarks/` directory holds the gate used for that.

Deliberately native: the VM, OS access, parsing engines (JSON, regex), collection
storage primitives, cryptographic primitives, and low-level numeric operations.

## Native compilation (`@native`)

Selected functions opt into the restricted native compiler with `@native`. The compiler
preserves the normal VM path as a fallback and can emit portable C++:

```text
zl --emit-native output.cpp source.zl
```

> This is the one remaining path that lowers the AST directly into the legacy
> untyped `zl::ir`, and it is frozen. Machine code is produced from verified MIR
> instead: `zl --emit-native-code` / `zl --emit-machine-code`, or
> `zl --backend native` to run with native code generation. See
> [pipeline.md](pipeline.md) and [native-backend.md](native-backend.md).

The current native tier supports:

- Primitive `int`, `double`/`decimal`, and `bool` values
- Initialized primitive locals and assignments
- Integer ranges and boolean branches
- Arithmetic and bitwise expressions
- Bounded constant-loop unrolling
- Safe integer constant folding

Generated modules expose the stable primitive ABI in
`include/zl/compiler/native_abi.hpp`.

`Task<T>`, closures, heap objects, and VM reflection objects do **not** cross the native
ABI. Unsupported native signatures are rejected deterministically; the ordinary VM
remains authoritative for those cases.

Run `scripts/native_gate.sh` (Linux/macOS) or `scripts/native_gate.ps1` (Windows) to
exercise the native compiler, ABI regression tests, generated C++ compilation, and the
benchmark semantic gate.

### IR optimization

The compiler IR has a backend-neutral optimization pass. It removes unreachable blocks
and folds safe primitive integer/boolean constant expressions while leaving
ownership/borrow instructions untouched. MIR native emission runs this pass before
lowering, so optimized IR feeds both native output and future VM/backend consumers.

## Native bindings (`zl-bind`)

The `zl-bind` tool generates controlled native bindings, ZL facades, manifests, and API
documentation from a restricted C/C++ declaration subset. Generated manifests carry
target ABI metadata, and generated C++ contains compile-time ABI guards. Unsupported C++
constructs are rejected rather than guessed.

Class bindings use smart-pointer-backed native ownership and translate C++ exceptions
into deterministic ZL runtime errors. Shared bindings can retain an opaque handle
without exposing C++ pointer types to ZL.

The tool is deliberately conservative: C++ stays a small native foundation, while
high-level library behavior stays in ZL.

## FFI

The ownership-aware FFI foundation provides opaque handles, borrowed buffers and struct
views, callback lifetime contracts, native resource adapters, dynamic library loading,
and validated `@ffi(symbol)` / `@ffi(library, symbol)` metadata.

### Callback quiescence and ownership (stabilization audit, closed 2026-09-20)

The P1-10 question was what stops an *external* thread from calling back into a ZL
callback whose context is going away, and who owns what crosses the boundary on the
way out. The answers are mechanical, and each is pinned:

- A callback value is a token into a process-local registry; it never crosses the
  ABI as a raw pointer (`ZL_NATIVE_CALLBACK`, `include/zl/compiler/native_abi.hpp`).
  Every invocation looks the token up before dispatching - an unknown or evicted id
  is a boundary error, not a call through stale memory.
- Each registry entry couples its invoke target to a **lifetime**: in-flight callers
  hold leases, `close()` refuses new leases and waits for the running ones to drain,
  and the entry is erased only after that. So "quiescence" is not a convention the
  host must remember; a callback cannot fire into an expired context. Pinned by
  `tests/native_ffi_lifetime_adapter_tests.cpp` (ctest `zl-native-ffi-lifetime-tests`:
  open/close transitions, close-waits-for-in-flight, move-only lease RAII,
  lease-on-closed throws, registry coupling, concurrent lease counting).
- Re-entry itself participates in the GC stop protocol like any native wait
  (`VM::BlockingNativeCall`), and a callback that throws restores the caller's
  execution/handler state - the pinned continuation case is
  `examples/advanced/MutexLocks.zl`; `GenericRuntimeChecks.zl` covers async
  callback frames and `share`-factory metadata.
- Outbound ownership is transport-enforced: an export declared to return an owned
  handle must return an id the registry knows (`native_boundary.cpp` rejects an
  unregistered owned return), so the runtime owns what a token refers to on both
  directions.

### No GC-path finalizers for native resources (same audit)

"Blocking finalizers during collection" cannot occur for native resources because
there is no GC-path finalization at all: the `NativeResourceRegistry` is consulted
only by the explicit boundary (`contains` for validation, `consume` when an
OWNED/CONSUMED argument transfers the handle into an export exactly once). The
collector never sweeps handles. The trade is accepted and stated: a handle that is
dropped without a consuming call leaks its resource - leaking is fail-open on
memory, and the alternative (implicit destructors run at collection time) is what
would put arbitrary blocking host code inside the stop-the-world protocol. Borrowed
views have no finalization need: the ABI contract is that the pointer is valid only
for the duration of the synchronous call, so nothing borrow-shaped outlives one to
be finalized.


For plain-data C structs, `zl-bind` emits a typed field-by-field schema: each
`struct` whose members are all scalar (fixed-width integers, `double`/`float`,
`bool`) gets zero-initialized storage behind an opaque handle, one typed
get/set binding pair per field (`<Ns>.<Struct>_get_<field>`,
`<Ns>.<Struct>_set_<field>`), layout queries (`<Ns>.<Struct>_size`,
`<Ns>.<Struct>_offset_<field>`), and a C++ schema table whose `offsetof`/
`sizeof` entries are evaluated by the target compiler under `static_assert`
guards. The generated ZL facade exposes each field as a typed method pair
(`field()` / `set_field(v)`). Structs with pointer, array or non-scalar
fields, or with no fields at all, are refused at generation time rather than
bound opaquely; the raw-buffer FFI path above remains the escape hatch for
layouts this schema does not describe.

## Native export registry

A process-local `NativeExportRegistry` accepts generated export tables, rejects
malformed or duplicate registrations, resolves exports by name, and routes name-based
calls through the `NativeError` boundary. This is the runtime registration bridge for
generated native modules; it avoids embedding raw function pointers in ZL values.
