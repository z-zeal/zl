# Review: what writing the examples turned up

Writing 46 runnable examples means running every code path a new user runs, in
the order they run it. That surfaced four bugs worth fixing outright - one of
them severe enough to break the language's headline feature - plus a set of
smaller ones that are recorded but left alone.

Everything below was reproduced against a build of this tree. Each entry says
what it is, how to see it, and whether it is fixed here or still open.

## Native-tier arithmetic contract — 2026-09-15

A harsh external review of the toolchain (the kind this file exists to
encourage) found one MAJOR: the native backend's *emitted* code violated the
language's fail-closed arithmetic contract in four places — int overflow
wrapped, `INT64_MIN / -1` raised `#DE`, shift counts were masked mod 64, and
float overflow emitted a quiet infinity. It was latent (the VM is the only
execution driver, and the `ZLM1` artifact has no consumer) and invisible to
the differential harnesses (all three arms execute on the VM), which is why it
survived: only `zl-native-backend-tests` executes emitted bytes, and it had no
overflow/shift/float-fault case. Fixed at the emitter — the tier's existing
zero-divisor trap convention, extended (`INT64_MIN % -1` still computes `0`,
because the VM defines it) — and covered by fifteen executed-byte trap cases
plus their healthy boundary neighbours, run in forked children. The SysV
shadow-space model (`shadowSpace = 0`, asserted by a test against PSABI 3.2.2's
32) was corrected in the same pass, with the reservation wired into the frame
layout for calling functions.

## Stabilization update — 2026-09-08

The findings below include the original reproductions. Subsequent hardening has
closed **O4/F8, O5, and O6**:

- GC now parks actual native waits (including lock acquisition), retains parked
  roots until reactivation, and prevents resumption during a running collection.
  Nested lock callbacks participate normally instead of postponing GC. A
  controlled-collector regression rejects the former early-resumption protocol.
- Native-driven callbacks restore the caller's execution/handler state on throw;
  `MutexLocks.zl` checks one continuation and lock reuse after an exception.
- Generic invocation frames and escaping closures carry lexical type bindings.
  Method-built and literal collections retain their identity; declared parent
  type arguments are substituted instead of copied by position. Argument and
  return checks share the ordinary VM boundaries rather than skipping generic
  signatures. `GenericRuntimeChecks.zl` also covers async callbacks and native
  `share` factory metadata, including legitimate widening/base-class assignments.

A subsequent compiler batch closes the local-reassignment and union-narrowing
gaps. Unions now have a distinct semantic kind, preserve their alternatives
through aliases/calls/generic instantiation, and use coverage-based match checking.
Read refinements retain the original write contract and constness; guard writes,
loop back edges and mutable captures invalidate unsafe assumptions. Unique local
storage names fix shadowing across blocks, patterns, loops and catch clauses.
The capture walk now binds callback-local declarations/counters instead of
mistaking them for outer thread captures. `match` also no longer leaks its subject
onto the surrounding value stack. Compiler and VM share type-name parsing and
substitution rather than decoding different grammars.

The heap-contract batch now checks stock native list/map/set/queue/stack writes
against storage-owned contracts, including aliases, nested children and fixed
array lengths. Type checks plan their changes and commit only after the whole
boundary succeeds. Field writes use complete declaration metadata and the
receiver's inherited generic bindings. This exposed and corrected the built-in
containers' former hard-coded `int` backing declarations, a template-parent field
substitution bug, and inherited field-name collisions. Native signatures now
describe element/key/value relationships; `String.split` returns `list<string>`
and native reads/projections retain their arguments.

The lifetime batch closes the static-root gap with program graph tracing: constants,
current static values and cached failures survive collection through active/suspended
VMs and reachable closures. It does not globally pin static stores, so unreachable
static/closure/failure cycles are reclaimed. A direct ASan reproduction previously
freed a static list still in use; the strengthened `StaticMembers.zl` now preserves
both values and cached failures across pressure.

Reclamation occurs after tracing, outside collector/coordinator locks. Implicit thread
joins run at stable VM boundaries instead of inside container/frame destruction.
In-flight exceptions root their object; cached failures use traced storage and native
diagnostics instead of permanent exception pins. Async entry failures propagate, and
pending channel tasks retain their operation owner. `gc_lifetime_tests.cpp` controls
retirement so another collection must complete while a retired thread is joined;
`type_boundaries.py` also checks async failure propagation.

The stabilization gate that remained after that batch - external FFI callback
quiescence/ownership, channel cancellation/progress interactions, blocking
native-resource finalizers, and the exception/Shared audit - closed on
2026-09-20, each item at a fixture or a written argument:

- **FFI callback quiescence/ownership**: the registry couples every callback to a
  drain-on-close lifetime, external re-entry looks the token up before dispatch, and
  owned returns must be registered handles - mechanics, fixtures and the accepted
  trade are written up in [docs/native.md](../docs/native.md#callback-quiescence-and-ownership-stabilization-audit-closed-2026-09-20).
- **Channel cancellation/progress**: a cancelled waiter is removed from its queue,
  every rendezvous path skips terminal tasks, and an unmatchable blocking operation
  reports the deadlock error instead of waiting - re-checked each pump round. Pinned
  by `tests/zl/valid/concurrency_regressions/ChannelCancelledWaiters.zl` (both
  directions, deterministic, one thread); the sync/async cross-match this interacts
  with is `ChannelSyncAsyncMix.zl`.
- **Blocking native-resource finalizers**: they do not exist - the collector never
  finalizes handles; release is explicit boundary consumption only, and the leak that
  buys is argued in [docs/native.md](../docs/native.md#no-gc-path-finalizers-for-native-resources-same-audit).
- **Exception/Shared audit**: `Shared<T>.withLock` releases on throw and stays usable
  - second throw included - now pinned by
  `tests/zl/valid/concurrency_regressions/SharedLockThrowRelease.zl`, the cell-variant
  counterpart to the `MutexLocks.zl` pin from the O5 fix; the lost-update guidance
  (O20) stays measured-and-documented rather than "fixed", because `withLock` exactness
  under threads is `SharedLostUpdateMeasurement.zl`.

Library expansion and the large type-checker split remain deferred; native lowering
progress is tracked under P1-6/PF-3.

## Fixed in this branch

### 1. Every generic in the language was unusable at runtime

**This was the big one.** Any method or constructor with a type-parameter-typed
argument threw on the first call:

```text
var nums = new List<int>()
nums.push(4)
-> runtime error: type assertion failed for argument 1: expected T, got int
```

It was not specific to `List`. The same failure hit `Map.put`, `Set.add`,
`Shared`, `Some`/`None`, `Ok`/`Err`, and any user-defined `class Box<T>`.
`List<int>` and `Map<K,V>` are the documented centrepiece of the language
(`docs/language-guide.md`, "Generic collections" and "Algorithms"), so the
central examples in the reference did not run at all.

**Cause.** The runtime type assertions added in `fe91b12` take their expected
type from `FunctionInfo::parameterTypeNames` and `returnTypeName`, which
`Compiler::compile` filled from `describeTypeAnnotation(...)` - the *source*
annotation. For a generic method that is the bare type parameter, so the VM
compared an `int` argument against the literal name `"T"`. The guard at
`src/vm/vm.cpp:1101` skips `""` and `"unknown"` but not an unsubstituted type
parameter.

**Fix.** `src/compiler/compiler.cpp` now erases type parameters before they
reach an assertion, at all four sites that produce one (pass-1 parameter names,
pass-1 return type, pass-2 `currentReturnTypeName_`, and the shared helper).
Erasure is token-aware, so it covers the bare case (`T`) and the nested ones
(`List<T>`, `func():T`) - the latter mattered because `Shared.withLock` and
`List.extend` take exactly those.

Verified: `examples/basics/Lists.zl`, `Maps.zl`, `Sets.zl`,
`intermediate/Generics.zl`, `advanced/OptionType.zl`, `ResultType.zl`,
`SharedState.zl` all run.

### 2. A block-body lambda was checked against the enclosing function's return type

The counter pattern documented in `docs/language-guide.md` failed on first call:

```zl
static func makeCounter(): func {
    var count = 0
    var inc = func() { count = count + 1
        return count }
    return inc
}
var c = makeCounter()
log(c())          // -> type assertion failed: expected func, got int
```

`Compiler::compileLambdaExpr` never saved or restored `currentReturnTypeName_`,
so an explicit `return` inside the lambda emitted `AssertType("func")` - the
*factory's* return type. Expression-body lambdas were unaffected because they
emit `Return` directly, which is why this looked intermittent.

**Fix.** `compileLambdaExpr` scopes `currentReturnTypeName_` to the lambda's own
inferred return type. `examples/intermediate/Closures.zl` now prints `1 2 3`.

### 3. `await` at the top level was a heap-use-after-free

`async func main` that awaited never resumed, and under AddressSanitizer it was
a write-after-free, reproduced 3/3:

```text
ERROR: AddressSanitizer: heap-use-after-free ... thread T1
  #6 zl::RuntimeScheduler::enqueue(...)   src/vm/runtime_scheduler.cpp:28
  #7 operator()                           src/vm/vm.cpp:1059
  #12 zl::RuntimeTaskState::succeed(...)  src/vm/runtime_task.cpp:86
  #13 operator()                          src/vm/native.cpp:778   (timeSleepAsync)
freed by thread T0 here:
  #7 zl::RuntimeScheduler::~RuntimeScheduler()
  #8 zl::VM::~VM()                        src/vm/vm.cpp:104
  #9 main                                 src/main.cpp:341
```

Two separate causes:

- **Lifetime.** `VM` held its scheduler by value and handed child VMs a raw
  pointer to it. A child created for an async invocation is kept alive by the
  task continuation and so outlives the root VM; when a worker then called
  `enqueue`, the scheduler was already freed. `RuntimeScheduler` is now owned by
  `shared_ptr` and co-owned by every VM that can enqueue into it.
- **Nothing drove the scheduler.** `VM::run` called `runUntilIdle()`, which only
  drains frames that are ready *right now*. A native async op completes on a
  worker thread later, so `main` returned and the process exited before the
  continuation was ever enqueued - which is why the failure looked like a silent
  exit on a normal build and only showed as a crash under ASan. `VM::run` now
  remembers the Task produced by a top-level async call and pumps until it
  settles, the same way `TaskBlock` does.

Verified: `async func main` with nested awaits prints its full output 3/3 and is
clean under ASan. Covered by `examples/advanced/AsyncTasks.zl`.

Separately, `docs` lists `block()`/`ignore()` as pending. `block()` works, and
`examples/advanced/TaskBlocking.zl` documents it as the way to collect a task
from a synchronous `main`.

### 4. Enum values could not be passed to a function typed with the enum

```zl
enum Color { Red, Green }
static func describe(Color c): string { ... }
describe(Color.Red)
-> runtime error: type assertion failed for argument 1: expected Color, got string
```

An enum member is stored as its name string, so the runtime type name is
`"string"` while the declared type is `"Color"`. The compile-time check is
correct and still rejects a raw string; only the runtime assertion was wrong.

**Fix.** `runtimeAssignableToType` in `src/vm/vm.cpp` now consults
`chunk.classReflection[...].isEnumType` and validates against `enumMembers`.
Verified by `examples/intermediate/Enums.zl`.

### 5. A thread closure that referenced nothing captured the entire scope

```zl
var n = 5                                    // never used by the closure
var t = Thread.start(func() { log("hi") })
-> runtime error: Thread.start: captured values must be Shared<T> when crossing
   raw threads
```

The compile-time check used the accurate reference list, but `MakeClosure` had a
fallback that snapshotted the whole scope whenever `captureNames` was empty.
Since `TypeChecker::analyzeLambdaCaptures` populates that list for *every*
lambda, empty genuinely means "references nothing" - so the fallback captured
every local in the enclosing function, including the handle of a thread you had
already started. That is why a second `Thread.start` never worked. The fallback
is gone.

### 6. Capture analysis missed the callee of a call

Removing that fallback exposed a second bug: `collectThreadRefs` walked a
`CallExpr`'s arguments but not its callee, because `CallExpr` stores the callee
as a *name* rather than a child node. So in

```zl
static func compose(func f, func g): func {
    return func(x) => f(g(x))
}
```

neither `f` nor `g` was recorded as a capture. The whole-scope snapshot had been
hiding it; with accurate capture the lambda body failed at runtime with
`undefined variable 'g'`. The callee is now included. `Lambdas.zl` covers it.

### 7. `Option<int>` and `Result<T,E>` could not be a declared type

```zl
static func find(): Option<int> { return new Some<int>(5) }
-> runtime error: type assertion failed: expected Option<int>, got Some
```

`reflectiveObjectMatches` deliberately preserves concrete instantiated identity,
so `Base<int>` stays distinguishable from `Base<string>`. The rule was too
strict: it also rejected the ordinary `Some<int>` -> `Option<int>` widening that
`class Some<T> extends Option<T>` exists to provide.

It now accepts that widening when the object's own instantiation carries the
same type arguments as the expected type, so `Some<int>` satisfies `Option<int>`
while `Some<string>` still does not. Mismatched instantiations were already
caught at compile time - `GB<string>` into a `GB<int>` parameter is rejected
before the runtime check runs - and both directions were re-verified.

`OptionType.zl` and `ResultType.zl` now use the idiomatic shape: a function that
returns an Option, passed to a function that takes one.

### 8. A closure nested inside a closure could not see the outer closure's captures

```zl
var n = 7
var outer = func() {
    var inner = func() { return n }
    return inner()
}
log("got " + outer())
-> runtime error: undefined variable 'n'
```

This one is a regression that fix 5 introduced, and it only showed up after fix
6. Both fixes made `captureNames` more accurate, and accuracy removed the
accidental safety net: `MakeClosure` used to snapshot the *entire* enclosing
scope whenever `captureNames` was empty, so an outer lambda carried every local
in `main` whether it named them or not, and the inner lambda could find `n`
among them. Once the outer lambda's capture list became accurate it stopped
carrying `n`, and the inner lambda had nothing to capture it from.

**Cause.** `collectThreadRefs` (`src/compiler/type_checker.cpp`) walks a lambda
body to build its capture list, but its `switch` had no `LambdaExpr` case, so it
hit `default: break` and never descended into a nested lambda. Names referenced
only inside the inner lambda were therefore never recorded as captures of the
outer one.

**Fix.** `collectThreadRefs` now recurses into a nested `LambdaExpr`, threading
the inner lambda's own parameter names through the shadowing set so an inner
parameter is not recorded as an outer capture.

Verified against upstream `fe91b12` built side by side: the snippet above prints
`got 7` on both. `MutexLocks.zl` and `Channels.zl` exercise the threaded form.

### 9. Nested generic type arguments could not be written

```zl
var grid = new List<List<int>>()
-> syntax error: Expected '>' to close type argument list -- got ">>"
```

The same happened for `Map<string, List<int>>`, `Box<List<int>>`, and anything
else that closed two type argument lists in a row. Adding a space -
`new List<List<int> >` - parsed fine, which is what gave the cause away.

**Cause.** The lexer runs without context, so the trailing `>>` becomes a single
`SHR` token, and all ten places in the parser that close a type construct did a
plain `expect(TokenType::GT, ...)`. This is the classic C++ `>>` problem.

**Fix.** `Parser::expectTypeAngleClose` (`src/parser/parser.cpp`) accepts `GT`,
or splits a fused `SHR`/`USHR` back into the `>` characters it was made of,
consuming exactly one and leaving the rest in the stream for the enclosing list.
All ten call sites in `parser.cpp` and `expression_parser.cpp` now go through it.

Shift expressions are untouched, because they are never parsed by these paths -
`32 >> 2` is `8`, `64 >> 2 >> 1` is `8`, `256 >>> 4` is `16`.
`intermediate/NestedGenerics.zl` covers both halves.

### 10. Any zero-argument method named `get` was treated as a `Shared` payload read

```zl
class Holder<T> {
    public T item
    public func get(): T { return this.item }
}
var h = new Holder<List<int>>(new List<int>())
log("len " + h.get().length())
-> compile error: protected Shared payload access requires Shared.withLock on 'h'
```

`h` is not a `Shared` and there is no payload to protect. Renaming the method to
`fetch` or `value` made the identical program compile, which pinned it to the
name.

**Cause.** `sharedPayloadOwnerForExpr` recognised a payload read purely by shape
- a zero-argument method called `get` - and then resolved its receiver through
`resolveSharedCellAlias`, which answers for *any* identifier by returning it
unchanged. So every `x.get()` counted as a protected `Shared` cell.

Fixing that alone made the guard fire on nothing at all, which exposed a second,
opposite bug: a cell built directly by `new Shared<T>(...)` was never registered.
Registration only happened for `var alias = existingCell`, because
`resolveSharedCellAlias` returns `nullopt` for a `NewExpr`. The guard therefore
only ever worked by accident, on copies.

**Fix.** Three changes in `src/compiler/type_checker.cpp`:

- `trackedSharedCellForExpr` only treats `x.get()` as a payload read when `x` is
  actually a tracked cell.
- `var c = new Shared<T>(...)` now registers `c` as its own canonical cell.
- `resolveSharedCellAlias` treats a self-edge as canonical rather than as a
  cycle, so the registration above resolves instead of failing the loop guard.

Verified both directions. Still rejected, as intended:

| Guarded | |
| --- | --- |
| `c.get().length()` on a direct cell | error |
| `alias.get().length()` on a copy | error |
| `var items = c.get()` then `items.length()` | error |
| `var items = c.get()` then `items[0]` | error |

Allowed: `c.get() + 1` on a `Shared<int>` (no member access), any payload access
inside `withLock`, a plain class's `get()`, and `Map.get(key)` (takes an
argument, so it was never matched).

### 11. `Task<T>` lost its type argument when read out of a `List` by index

```zl
var ts = new List<Task<int>>()
ts.push(inst.work(3))
log("v " + ts[0].block())
-> compile error: type error: type 'Task' has no method 'block'

log("v " + ts.get(0).block())   // the same thing, and this one worked
```

So the two equivalent reads of the same element disagreed, which made it
impossible to fan tasks out into a collection and collect the results - the
natural way to write concurrent code.

**Cause.** `TypeChecker::inferIndexAccess` derived the element type from the
`List<...>` class name with a hardcoded list that only covered the four
primitives (`int`, `double`, `string`, `bool`) and sent everything else to
`ZlType::OBJECT`. `Task` is its own type kind, not an object whose class name
happens to be `"Task<int>"`, so `block` could not be resolved on it. The other
element-type mappers in the same file already handled `Task`, `func`, `nil`,
`list`, `map`, `set` and `array`; this one had drifted.

**Fix.** `inferIndexAccess` now uses the same canonical mapping. A subtlety the
first attempt got wrong: the mapping matches on the *base* name (`list`, not
`List`), so a class element must keep its full instantiation as its class name -
otherwise `List<List<int>>` indexed once became plain `List` and the next index
failed with "indexed access requires a List<T>". `intermediate/NestedGenerics.zl`
caught that immediately.

Verified: `List<Task<int>>[i].block()`, `List<Task<string>>[i].block()`, a 50-task
fan-out summed through indices (`sum 1225`), and no change to `List<int>`,
`List<List<int>>` or `List<Shared<int>>`.

### 12. A non-exhaustive `match` on an int, double or string compiled, then mistyped itself

```zl
var n = 3
var m = match n { 1 => "one" }
log("m = " + m)          // m is the int 3, not a string
```

No arm matches, so the match falls through and evaluates to the *subject* - which
has a different type from every arm. With no declared type the value just leaks
out as an `int`; declaring one turns it into a runtime failure instead:

```zl
string m = match n { 1 => "one" }
-> runtime error: type assertion failed: expected string, got int
```

The exhaustiveness checker already rejected incomplete matches for union, `bool`,
`enum`, `Option` and `Result` subjects (`type_checker.cpp:3360-3390`). It simply
had no rule for the plain scalar subjects, which is where the open value space
makes a list of literals provably insufficient.

**Fix.** An `int`, `double` or `string` subject now requires a wildcard or an
irrefutable variable pattern. An irrefutable variable pattern already counted as
a wildcard through `rootCatchAll`, so `v => ...` still works as the catch-all.

Verified: int, double and string subjects without a catch-all are all rejected at
compile time; with a wildcard, with a variable pattern, and the existing `bool`
and `enum` forms all still compile and run. `intermediate/MatchExpressions.zl`
covers the accepted shapes, including a `when` guard.

## Still open

Ranked by how quickly a new user hits them. Entries marked **(fixed)** no longer
reproduce; they are kept here because the workaround is still baked into an
example or a doc, and the context explains why.

### O2 - A `func`-typed lambda parameter cannot be called **(fixed)**

```zl
var apply = func(f) => f(21)
-> compile error: type error (line 3): undefined func 'f'
```

The same thing written as a *named* function works fine, so the gap is specific
to lambda parameters:

```zl
static func namedCall(func f): int { return f(21) }
namedCall(func(x) => x * 2)      // 42
```

Verified to be independent of the capture fixes above - it used to reproduce
with and without them. This was the "`func`-typed slots don't carry full
signature types" limitation showing up as an untyped lambda parameter being
looked up as a named function.

**Fix.** `inferCall` treats an UNKNOWN callee as a value call, so
`func(f) => f(21)` invokes the parameter rather than looking `f` up as a
named function. The MIR verifier matches that: `call_indirect` through
Unknown/TypeParam is deferred to the runtime instead of being rejected as
"not callable". Covered by
`tests/zl/valid/language_hardening_tests/LambdaFuncParam.zl`.

### O3 - `Atomic`, `Mutex` and `Channel` could not be captured by a thread **(fixed)**

The rejection message said "use Atomic or Mutex for mutable shared state", but
the capture check whitelisted `Shared<T>` only, so it rejected the very things it
recommended:

```zl
var a = new Atomic()
var t = Thread.start(func() { Atomic.add(a, 1) })
-> compile error: Thread.start cannot capture 'a' across a thread boundary;
   use Atomic or Mutex for mutable shared state
```

The whitelist existed in three places that had drifted: `isExplicitlySharedValue`
in `src/vm/native.cpp`, `TypeChecker::validateThreadLambda`, and a further inline
`Shared`-only test at each of the two call sites in `type_checker.cpp`
(`Task.spawn` and `Thread.start`). Fixing only the first two let `Atomic` and
`Channel` through while `Mutex` was still rejected by the third with a different
message. All three now share one list - `Shared<T>`, `Atomic`, `Mutex`, `RwLock`,
`Semaphore`, `Channel`, `Condition` - kept in step between
`isThreadSafeClassName` in `native.cpp` and
`capturedValueCrossesThreadBoundary` in `type_checker.cpp`.

Each was then verified end to end from a worker thread:

| Captured value | Verified |
| --- | --- |
| `Atomic` | `atomic total 2000`, 3/3 runs (`Atomics.zl`) |
| `Channel` | `received from-worker` (`Channels.zl`) |
| `Semaphore` | `available 2` / `count 2` |
| `Mutex` | `guarded total 100`, 5/5 runs at 2x50 (`MutexLocks.zl`) |
| `RwLock` | `read 1000` under `withWrite`/`withRead` |

`Condition` is on the list but has not been exercised from a worker thread -
unchecked.

`Atomics.zl`, `Channels.zl` and `MutexLocks.zl` no longer need the
`new Shared<Atomic>(...)` / `new Shared<Channel>(...)` handle wrappers they used
as a workaround.

Also worth noting: `stdlib/zl/lang/Atomic.zl` and `Mutex.zl` are empty class
bodies. Their entire API is native statics, so there is no instance surface at
all.

### O4 - Locked critical sections deadlock under contention **(fixed)**

2 x 50 locked increments completes and prints `total=100`. 2 x 100 hangs
forever with no output, 4/4 runs. Both threads are joined by `main`, so it
presents as a hang rather than a crash.

It is not specific to `Shared<T>`. `Mutex.withLock` deadlocks at the same
threshold (2 x 100 hangs, exit 124) while `Shared.withLock` at 2 x 50 is stable
5/5, so the fault is in the locked-closure path they share, not in either
wrapper.

### O5 - An exception escaping `Mutex.withLock` corrupts the continuation **(fixed)**

```zl
try {
    Mutex.withLock(m, func() => throwSomething())
} catch Exception e { log("caught") }
```

prints the catch, then runs the rest of `main` **twice**, then dies with
`'return' used outside of a func`. A plain `1 / 0` in the same position escapes
the `try` entirely. The lock's value is in the closure, so keep bodies
non-throwing until this is fixed.

### O6 - A generic collection loses its instantiated identity **(fixed)**

A list built inside a generic method - the result of `transform`, `filter`, or
`reversed` - is erased to `List`, so it cannot be passed to a `List<int>`
parameter:

```zl
static func total(List<int> xs): int { ... }
total(nums)                          // fine
total(nums.transform(func(x) => x * 2))
-> runtime error: type assertion failed for argument 1: expected List<int>, got List
```

This one is *not* fixed by the widening above, and the reason is worth stating:
a list built inside a generic method body does not know its own instantiation.
`transform` runs as `List<T>`, so the object it constructs records `T` as its
type argument, not `int` - there is nothing to compare against `List<int>`.
Fixing it means threading the concrete instantiation through generic method
bodies, which is a real piece of work rather than a check to relax.

`CollectionAlgorithms.zl` reads such lists through their own methods instead of
passing them on.

The same erasure also breaks the **typed collection literals** documented in
`docs/language-guide.md`:

```zl
List<int> nums = [4, 5, 6]
-> runtime error: type assertion failed: expected List<int>, got List

List<string> names = ["Ada", "Grace"]
-> runtime error: type assertion failed: expected List<string>, got List
```

The literal builds an erased `List`, which the typed declaration then asserts
against `List<int>`. The lowercase native form is unaffected and works:

```zl
list<int> nums = [1, 2, 3]                 // fine
var nums = new List<int>()                 // fine
```

This is why the README's headline snippet did not run, and why `Arrays.zl` and
`Lists.zl` use `new List<T>()` plus `push` rather than a typed literal.

### O8 - A static method taking an interface-typed parameter does not compile

```zl
static func describe(Shape s): string { return s.name() }
-> runtime error: Compiler: unknown library func 'R3.describe'
```

An interface-typed *variable* works, so `Interfaces.zl` uses one.

Re-confirmed on a minimal case: `static func describe(Shape s): double { return
s.area() }` with `Circle implements Shape`. Calling `describe` directly on the
instance works (`new Circle(2.0).area()` is fine); only the static entry point
fails.

The failure is late - it is a `Compiler:` error at emit time, not a type error.
`Compiler::compileCall` resolves `Namespace.callee` by scanning
`chunk_.functions` for a static whose `dispatchSignature` equals the call site's
`node->resolvedDispatch`, and only falls through to the native table if nothing
matches (`src/compiler/compiler.cpp:1336-1351`). So the two signatures disagree
for an interface-typed parameter, the static is never found, and the lookup ends
up asking the native table for `R3.describe`. Which side of the comparison is
wrong is not yet pinned down.

### O9 - `Time.nowMillis()` recursed until the stack ran out (fixed)

`stdlib/zl/time/Time.zl` declared
`public static func nowMillis(): int { return Time.nowMillis() }`, which shadowed
the native of the same name and called itself:

```text
runtime error: stack overflow: maximum call depth (100000) exceeded
```

The wrapper was redundant - the native is already reachable as `Time.nowMillis` -
so it has been deleted and the call now resolves to the native.

### O10 - `Time.format` uses its own tokens, and nothing documents them (fixed)

I originally recorded this as "format does not expand its specifiers". That was
wrong, and worth keeping as a caution: `Time.format(now, "%Y-%m-%d")` returns the
format string unchanged because it recognises `YYYY`, `MM`, `DD`, `HH`, `mm`,
`ss` - not strftime. Given the right tokens it works:

```zl
Time.format(now, "YYYY-MM-DD HH:mm:ss")   // 2026-09-06 16:32:44
```

The real problem was that no document mentioned the token set, so `%Y` was the
obvious thing to reach for - and it failed silently, an unrecognised token left
in place rather than reported. Both halves are closed now: `Time.format` is
documented in `docs/stdlib.md` (token table included), and an unrecognised
`%`-token raises instead of passing through, so a wrong date can no longer be
printed confidently. `TimeLib.zl` uses the correct tokens and demonstrates the
rejection; `tests/zl/valid/language_hardening_tests/TimeFormatTokens.zl` pins
both the token set and the raise.

### O11 - `Serialize.stringify` on a generic `Map` encoded internal storage (fixed)

```zl
var m = new Map<string, string>()
m.put("a", "b")
Serialize.stringify(m)      // was: {"__native":{"a":"b"}}
```

The generic collection classes are thin wrappers whose only field is the native
storage they delegate to, and the encoder walked object fields blindly. It now
looks through `List`/`Map`/`Set` wrappers and encodes the payload, so both
collection spellings produce the same JSON. See `JsonLib.zl`.

The printer had the same blindness until later: `log(m)` printed
`Map{__native: {...}}` - the plumbing rather than the data. `formatValueInner`
(`src/vm/value.cpp`) now reuses the same look-through, so `log(l)` prints
`[3.14, 1]` and `log(m)` prints `{"pi": 3.14}`, one spelling through every
door. Pinned by `tests/zl/valid/core_tests/CollectionPrinting.zl`.

### O12 - `Math.sqrt(-1.0)` returns `nan` instead of throwing (fixed here)

`docs/stdlib.md` says domain errors are reported. They are for `log`/`log10`,
`asin`/`acos`, and out-of-order `clamp`/`random` ranges - but `sqrt` returns
`nan`. `MathLib.zl` shows both behaviours.

Fixed: `sqrt` now throws on a negative argument, `pow` throws on domain errors
and finite-input overflow, and double `+ - * / ^^` raise on overflow - the
language no longer produces `nan`/infinities anywhere. `MathLib.zl` and
`docs/stdlib.md` show the new contract, locked in by the `DoubleArithmetic`
regression fixture.

### O13 - The README's headline example did not run (fixed here)

```zl
var names = ["Ada", "Grace", "Alan"]
names.forEach(func(n) => log("Hello, " + n))
-> compile error: cannot call method 'forEach' on value of type list
```

A list literal infers the native `list`, which has no methods; `forEach` lives on
the generic `List<T>`. Annotating does not rescue it either, because of the typed
literal problem in O6 - `List<string> names = [...]` fails at runtime.

The README snippet now uses `new List<string>()` with `push`, which runs, and the
stale `examples/Hello.zl` path in the same file now points at
`examples/basics/HelloWorld.zl`.

### O14 - `docs/language-guide.md` shows a semicolon that is not valid syntax **(fixed)**

The closures section used to contain `func() { count = count + 1; return count }`.
`;` is not accepted anywhere - not as a separator, not even as a trailing
terminator:

```text
var a = 1;
-> syntax error: Expected expression -- got ";" at line 3
```

**Fix.** The language-guide snippet no longer uses a semicolon.

### O15 - `docs/language-guide.md` implies `count` is a predicate

`count` is listed among the lambda-taking algorithms. It is
`count(T item)` and returns the number of occurrences of a value; the predicate
versions are `any`, `all`, and `filter`.

### O16 - Generic methods are not supported **(fixed)**

`static func firstOf<T>(List<T> items, T fallback): T` used to be a syntax error
(`Expected '(' after func name -- got "<"`). Method-level type parameters now
parse, type-check, and run; call sites write the type arguments explicitly
(`Helpers.firstOf<int>(nums, 0)`). Covered by
`tests/zl/valid/language_hardening_tests/GenericMethods.zl`.

### O17 - The legacy regression corpus is missing **(fixed)**

`CMakeLists.txt` used to declare 15+ test executables whose sources were not in
the tree, so `cmake -S . -B build` failed at the generate step:

```text
CMake Error at CMakeLists.txt:259 (add_executable):
  No SOURCES given to target: zl-native-struct-tests
```

The examples were therefore built by compiling `src/**/*.cpp` directly with the
same flags the `zl_language` target uses, and the project's own regression suite
could not be run.

**Fix.** Required C++ tests are registered with `zl_add_required_test`, which
fails configuration if a source is missing rather than silently skipping.
`.github/workflows/ci.yml` configures the tree, builds `zl-tests`, and runs
`ctest` on every push and pull request.

### O18 - String operations count and index bytes, not characters **(fixed)**

Every `String.*` operation still works on UTF-8 bytes: `String.length` is a
plain `.size()`. `Text.*` used to do the same, so `Text.length("héllo")` was
`6`, `Text.charAt` could return half of `"é"`, and `Text.reverse` emitted
invalid UTF-8 (`é` as `\303\251` came out as `\251\303`).

```zl
Text.length("héllo")            // was 6, now 5
Text.charAt("héllo", 1)         // was the byte 0xC3, now "é"
Text.reverse("héllo")           // was invalid UTF-8, now "olléh"
```

**Fix.** `Text.length` / `charAt` / `substring` / `reverse` / `codePointAt` /
`fromCodePoint` and the offset-returning search helpers index Unicode scalar
values via `String.utf8*` natives. `String.*` remains the byte-string
primitive. `upper` / `lower` still only case ASCII. Covered by
`tests/zl/valid/language_hardening_tests/Utf8Text.zl`; see `docs/stdlib.md`.

Two smaller notes found in the same pass:

- `string` has no methods at all — **fixed**. `s.length()` used to fail with
  `cannot call method 'length' on value of type string`, leaving the
  `Text.length(s)` free-function form as the only option while every collection
  type was method-based. `string` now has a method surface, and each method *is*
  an existing `String.*` native with the receiver bound as its first argument,
  so nothing is implemented twice. `startsWith`/`endsWith` became native
  primitives (byte prefix/suffix tests) and `Text.startsWith`/`endsWith`
  delegate to them instead of carrying a second ZL copy. Pinned by
  `tests/zl/valid/language_hardening_tests/StringMethods.zl` (every method
  checked against its qualified spelling) plus
  `tests/zl/invalid/type_errors/StringMethodUnknown.zl` and
  `testStringMethodsAreTheStringNatives` in `tests/pipeline_tests.cpp`; see
  `docs/language-guide.md#string-methods`.
- `List.pop()` reported `Collection.pop: cannot pop from an empty list` while
  `List.first()` reported `List.first called on empty list` - two voices for one
  condition (fixed: `List.pop` now guards emptiness in ZL and names itself; the
  raw `Collection.pop` primitive keeps its own name when called directly, and
  `tests/zl/valid/core_tests/EmptyListErrors.zl` pins both messages).

### O19 - `INT64_MIN` cannot be written as a literal — FIXED

Previously `var min = -9223372036854775808` errored `integer literal is out of range`.
Fixed: the parser folds the exact spelling `-9223372036854775808` into one negative
literal (`src/parser/expression_parser.cpp:102`), and one past it in either direction
is still rejected. Pinned by `tests/zl/valid/language_hardening_tests/Int64Min.zl`
and `tests/zl/invalid/type_errors/IntLiteralOutOfRange.zl`.

```zl
var min = -9223372036854775808  // now compiles and prints -9223372036854775808
var max = 9223372036854775807   // fine
```

### O20 - `Shared<T>` really does lose updates; measured

`SharedState.zl` warns that `Shared<T>` makes a capture *legal* but not *safe*.
That is not a hypothetical. Two threads doing 2000 unsynchronised read-modify-write
increments each, run alongside two threads using `Atomic` for the same work:

```text
plain  (expect <= 4000)  2490     <- 1510 updates lost
atomic (expect 4000)     4000     <- exact
```

So the guidance in that example is load-bearing, and `Atomic` is correct under
real contention rather than merely in the small counts the examples use. Recorded
here as evidence, not as a defect.

### O21 - `match` cannot pattern-match on `Option` or `Result` at all

The exhaustiveness checker has explicit rules for exactly these subjects -
"Option subject requires both Some and None or a wildcard", "Result subject
requires both Ok and Err or a wildcard" - so matching on them is clearly
intended. But no pattern spelling is accepted:

```zl
var r = new Ok<int, string>(7)
match r { Ok v => ...  Err e => ... }
-> type error: generic type 'Ok' requires 2 type argument(s), for example 'Ok<...>'

match r { Ok<int, string> v => ...  Err<int, string> e => ... }
-> type error: match type pattern does not match subject type
```

The first form is rejected before it gets anywhere; the second is rejected by the
type-pattern compatibility test at `type_checker.cpp:3044`, which routes through
`isAssignable`. `Ok<T,E> extends Result<T,E>`, so that check needs to accept an
instantiated subclass against its parameterized parent - the same widening that
fix 7 added on the runtime side in `reflectiveObjectMatches`, but for the static
assignability model.

`enum` subjects work fine, so the pattern machinery itself is sound.

The practical consequence is that those two exhaustiveness rules are unreachable
dead code, and `ResultType.zl` / `OptionType.zl` use `if (r.isOk())` rather than
`match` - which reads as a style choice but is in fact the only option.

Not fixed: widening `isAssignable` for generic instantiations touches every
assignment in the language, which is well beyond what the finding justifies on its
own.

## Verdict on the previously reported F6-F9

| | Claim | Verdict |
| --- | --- | --- |
| F6 | `await Time.sleepAsync(...)` is a heap-use-after-free, crashes every run | **Confirmed** as a UAF - ASan trace above, 3/3 - and now **fixed**. The wording needs one correction: a normal build did not reliably segfault; it hung or exited silently, and the crash only showed under ASan. |
| F7 | `Thread.start` unusable inline; an unused `var n = 5` in scope breaks it | **Confirmed**, with a refinement: it only bit closures that reference *no* variable, because those captured the whole scope. A closure that referenced a `Shared` was fine even with unused locals present. Now **fixed** (fix 5). |
| F8 | `withLock` deadlocks; 2x50 fine, 2x100 hangs | **Confirmed** - `total=100` at 2x50 (5/5), 2x100 hangs (exit 124). One correction to the original report: the API is the instance method `counter.withLock(func() { ... })` (`builtin_library.cpp:663`), not a qualified `Shared.withLock(counter, f)` call - the latter does not compile. `Mutex.withLock` hangs at the same threshold, so this is the locked-closure path, not `Shared` (see O4). |
| F9 | `log(3.14)` prints `3.1400000000000001` | **Confirmed**, and now **fixed**. Cosmetic, but it is the first double a beginner prints - and the diagnosis in the examples was wrong: 17 digits is the *longest* decimal that round-trips, not the shortest. |

F6 and F8 were not reachable at all before fix 1: `Shared<int>` could not be
constructed, so no thread or lock example could even start. Both were re-verified
after the fix. F6 and F7 have since been fixed (fixes 3 and 5), and F8 was closed
by the later safepoint work described above.

F9 was the printer, not the arithmetic. `valueToString` formatted every double
with `std::setprecision(std::numeric_limits<double>::max_digits10)` - 17
significant digits, which is round-trip safe but is the *longest* spelling that
reads back as the same binary double. It is now the shortest one
(`doubleToShortestString`, `src/vm/value.cpp`), so `log(3.14)` prints `3.14`,
`Math.PI` prints `3.141592653589793`, and `0.1 + 0.2` still prints
`0.30000000000000004` because that digit really is there. Nothing was lost:
`String.toFloat` of the printed form returns the identical double. The JSON
encoder now shares the one formatter instead of carrying its own
`setprecision(17)`, so a value cannot be serialized with more digits than `log`
shows for it. Pinned by `tests/zl/valid/language_hardening_tests/DoubleFormatting.zl`
under all four backends (`tests/double_format_parity.py`, ctest
`double-format-parity`); `DataTypes.zl` and `MathLib.zl` expected output updated.

## Three features that existed but were documented nowhere

`Option<T>`, `Result<T,E>`, and the fluent `Regex` builder are all defined in
`src/compiler/builtin_library.cpp` and appear in no file under `docs/`. They now
have examples: `OptionType.zl`, `ResultType.zl`, `RegexBuilder.zl`.

The builder is worth calling out - `new Regex("").digit().exact(4)` producing
`^\d\d\d\d$` is the kind of thing people would use if they knew it existed, and
`source()` makes it self-documenting.

One small oddity while reading it: `None<T>.unwrapOrElse` carries two stacked
`@Override` annotations. It compiles and behaves correctly, so this is cosmetic.
