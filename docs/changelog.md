# Development Checkpoints

Dated progress notes, newest first. These were previously appended to `README.md`.

## 2026-09-19 - Phase 1 (memory domains): annotation rules and the `memory` contract skeleton

Phase 1 of [`docs/memory-domains.md`](memory-domains.md) is in: annotations
have rules, and a `memory` declaration parses and is contract-checked - with
no consumer yet, exactly as the phase planned. The phase's two exit criteria
are both compile errors with locations now: a `memory` block missing
`release` is refused at the declaration, and an unknown `@` name is refused
at the `@`.

**Annotations: a table instead of silence.** `parseAnnotations` accepted any
identifier, so `@memroy(Pool)` compiled and did nothing - a misspelt
annotation was indistinguishable from an absent one, and nothing stopped a
fourth annotation consumer from being another `if`. The fix is one registry,
[`include/zl/parser/annotation_rules.hpp`](../include/zl/parser/annotation_rules.hpp),
with the five names that have consumers today (`Deprecated`,
`SuppressWarnings`, `Override`, `ffi`, `native`), each with the declaration
kinds it may annotate. The two halves are enforced where the context lives:
the *name* is checked in the parser at the `@` itself (the earliest point a
location is known - `syntax error: unknown annotation '@memroy' at line N -
known annotations are: ...`), and the *target* is checked in the type checker
where the declaration kind is fully known (`@Override` on a class or on a
constructor is an error naming what is allowed there; `@Deprecated` and
`@SuppressWarnings` remain constructor-legal because they target any func).
A field still takes no annotation, unchanged.

**`memory Name { ... }`: the contract skeleton.** `memory` is a contextual
keyword at file scope (identifier uses are untouched - `var memory = 5`
still parses), the body takes exactly the member forms a domain can use
(fields, funcs; statics, asyncs, operators, nested declarations and
annotations on members are each a specific error), and
`TypeChecker::registerMemoryDeclaration` validates the contract from
`docs/memory-domains.md §4.1` against the written signatures: `acquire`
and `release` required, `reset`/`exhausted`/`onCollect` optional-but-
then-exact, each public, state and helpers private, no constructor, no
`extends` of a domain, and `new FrameArena()` fails closed with a message
naming the phase that removes the refusal - the same convention as the
native backend's unimplemented region opcodes. Three inert builtin types
(`MemoryShape`, `MemorySlot`, `MemoryStats`) make the signatures resolvable;
nothing accepts a value of them, so a fabricated one is unobservable.
Nothing emits the declaration: no layout, no dispatch, no reflection, no
`--emit-mir` output - registration builds a checking shape and the checker
is the only reader.

Phase 0's one open gate item closes with this phase: §9 required each
phase's §10 numbers to land in `research/report.md`, and they now have -
`research/report.md` §2.8 carries the allocation baseline (throughput,
collect share, peak RSS, before/after) with the same reading §10.1 gives it.

Gates: the new fixtures are `tests/zl/valid/memory_tests/MemoryContract/`
(a full valid contract, imported as a module) and
`tests/zl/valid/language_hardening_tests/AnnotationRules/` (every known
annotation on a legal target), plus one invalid fixture per rule:
`tests/zl/invalid/memory_tests/` (11 - missing release, wrong signature,
private contract method, duplicate contract method, public helper, public
field, constructor, static member, construction refused, extends refused,
malformed declaration) and `UnknownAnnotation` /
`AnnotationTarget{Class,Constructor}`. 42/42 ctest, 88/88 regression
fixtures, 52/52 examples, boundary-lint, backend diff and the native gate
all green on the same build.

## 2026-09-19 - One primary type per file in the test corpus

The regression fixtures had drifted against the rule
[`docs/packages.md`](packages.md#one-primary-type-per-file) documents: eight
fixtures inlined their helper types into the case's own file, so the corpus
that is supposed to pin the language's module rules never exercised the
one-primary-type-per-file layout it ships with. Each of them is now a
per-case directory - entry file plus one helper module per type, imported by
name, the layout `tests/zl/valid/import_tests/ImportCase/` already used -
and the runner discovers them unchanged (a case is a directory containing a
`main`-declaring `.zl`):

- `valid/ownership_tests/OwnedBorrowShared` (Ticket out)
- `invalid/ownership_tests/UseAfterMove`, `MoveWhileBorrowed` (Ticket out)
- `valid/language_hardening_tests/StringMethods` (StringMethodBox,
  StringMethodTag out), `IncompleteStubRefusal` (Box out),
  `GenericNameCollision` (E out - the collision with builtin `Result`'s
  `E` type parameter survives the import intact, same diagnostic)
- `invalid/type_errors/OpenSumMatch` (Mine out - the imported subclass
  still reopens the sum and the match still reports
  `uncovered Result<int,string>`)
- `valid/core_tests/AllFeatures` (Point, Animal, Dog, Box out; Dog imports
  Animal itself, and `scripts/native_gate.sh`/`.ps1` point at the new
  entry path)

Two fixtures keep multiple types in one file on purpose, both because the
multi-type file *is* the thing under test:
`invalid/semantic_errors/DuplicateClass` (two same-name classes in one
compilation unit is the diagnostic) and
`valid/import_tests/ImportCase/ImportHelper` (a helper travelling with its
primary import is the documented behaviour being exercised). Every moved
case reproduces its pre-move exit status and diagnostic; every unmoved gate
was rerun (88/88 fixtures, 52/52 examples, 42/42 ctest).

## 2026-09-19 - Memory domains: a plan that fits the runtime (design, no behaviour change)

The direction for letting a ZL program choose - and eventually write - its own
automatic memory strategy now lives in
[`docs/memory-domains.md`](memory-domains.md). It replaces an earlier draft, and
most of the replacing came from the code disagreeing with it:

- the draft's `MemoryDomain::allocate(size)` has nothing to intercept. Managed
  allocation here is `make_unique<ObjectBox>` into one global registry, and a
  box's payload is RAII (`std::string`, `vector<Value>`, `unordered_map`), so a
  chunk reset could not free it - a bump arena would leak exactly the memory it
  came to reclaim. The contract is typed instead (`acquire(MemoryShape)` /
  `release(MemorySlot)`), modelled on the native-resource registry ZL already
  has and verifies.
- the ownership half of the abstraction is already built: `gc`/`owned`/`borrow`/
  `shared` are parsed, checked and carried through MIR to the runtime. So the
  plan adds an *orthogonal* storage-domain id rather than the fused
  `Arena<'a, T>` the draft proposed, which would have re-opened every ownership
  rule in the checker, the verifier and both backends.
- `shared` is the thread-synchronised cell, not a reference count, and no
  managed box is counted anywhere - so the draft's `shared Object` is out, and
  `docs/mir.md`'s "reference-counted elsewhere" is corrected.
- `with` is the data-update operator, `@name` annotations have no allowlist (an
  unknown one compiles and is ignored), and `unsafe` does not exist. Domains
  need none of the three: they never receive an address, and reclamation happens
  only where the domain's policy and the compiler's liveness proof agree - which
  is what turns a lying domain from a memory-safety bug into a performance bug.
- escape checking is scoped to what the checker can prove: it extends the region
  paths `borrowRegionForExpr`/`regionOverlaps` already compute to `FieldStore`,
  `StaticStore` and captures, instead of promising the heap alias analysis
  `docs/mir-safety.md` says the project does not have.

Two doc gaps the plan exposed, and which this change closes: `docs/language-guide.md`
never documented the `owned`/`borrow` declarations at all, and said nothing about
what `Drop` does - which is to end a binding's rootedness, not to free anything.
`TASKS.md` gains **P2-8** for that second half: the lifetime language is checked
end to end with no eager reclamation behind it, which is the real reason a memory
domain would be worth its complexity. Phase 0 of the plan is deliberately the
runtime's own easy wins - flat `ObjectBox` fields, recycling boxes from the
collector's deferred vector, a measured baseline - gated so that if those beat an
arena, no syntax is added.

No compiler, runtime, stdlib or test change: documentation only, so the build,
ctest, examples and the differential harnesses are untouched by definition.

## 2026-09-18 - An empty literal says what kind of collection it is (P2-10)

```zl
static func take(map<string, int> m): int { return Collection.length(m) }
static func take(list<int> l): int { return Collection.length(l) }

take({})
// was: error [mir.type-flow]: in Demo.main(): block b1[2]: invoke_static of
//      'Demo.take(list)' argument 0 ('l') passes set<unknown> but the
//      parameter is list<int>
// now: no overload of 'take' matches the given argument types; candidates are:
//        - Demo.take(map)
//        - Demo.take(list)
```

Found while closing P2-9, and pre-existing: an empty `{}` inferred a bare `SET`
with no class name at all. `isAssignable` reads an unparameterized collection as
an *untyped slot* - the deliberate rule that lets a bare `list` field hold
anything - so the literal was more permissive than the parameterized
`set<int>` spelling of the same value, and overload resolution was happy to hand
it to a `list<int>` parameter. The MIR lowerer types the identical literal
`set<unknown>` (`lowering.cpp` builds `setType(unknownType())` for it), so the
module was rejected one stage later, in a message about a block and an
instruction.

The checker now spells the element type it already knew:

```cpp
if (node->bracketSyntax) return InferredType(ZlType::LIST, "list<unknown>");
node->targetCollectionKind = "set";
return InferredType(ZlType::SET, "set<unknown>");
```

`genericCollectionCompatible` compares the container base first, so `set<unknown>`
against `list<int>` is now a mismatch at resolution, while `set<unknown>` against
`set<int>` still is not - `unknown` arguments stay compatible, which is what
`var tags = {}` followed by `set<int> declared = tags` relies on. A bare `list`
or `set` parameter (no class name on the other side) is unaffected, so the
untyped-slot rule still does its job where it was meant to.

This is the inference every empty literal goes through, so it is not a narrow
change: where the candidates *agree* on a container, P2-9's expectation declares
the literal first and none of this is reached (`onlyList({})` against a single
`list<int>` overload still compiles and returns 0).

Gates: `tests/zl/invalid/type_errors/EmptyLiteralContainerMismatch.zl` pins the
message; the full gate was rerun - 42 ctest, 51 examples byte-compared across the
reference/bytecode/native paths, every regression fixture, and all five
differential harnesses.

## 2026-09-18 - A `func`-typed slot carries its signature, and the call site checks it (P1-2)

```zl
static func predicate(func(int, string): bool f, int n, string s): bool { return f(n, s) }

var tooFew = func(int n): bool => n > 0
predicate(tooFew, 3, "abc")
// was: runtime error: type assertion failed for argument 1:
//      expected func(int,string):bool, got func
// now: compile error: argument 1 to func 'predicate':
//      expected a func with 2 parameter(s), got 1
```

The signature was already parseable and already checked in two places - a
`func(int, string): bool` declaration rejected a one-parameter lambda
(`validateFunctionTypeAssignment`), and a call *through* such a slot checked its
arity. What was missing was the hand-over, and it was missing unevenly: the
implicit-self, method-call and `super` paths each carried their own copy of the
argument check, while the qualified static path (`Class.method(...)`) carried none
at all. So the same program was a compile error or a runtime error depending on
how the callee was spelled.

The three copies are now one `TypeChecker::validateFunctionArguments`, called from
all four paths, comparing arity, parameter types and return type against the
declared signature. A static method whose return type is `func(int): int` also
carries the signature out of the call now (`returnFunction*` was read on the other
paths only), so the value a factory hands back is itself a checked callable rather
than an opaque `func`.

Two cases are deliberately still unchecked, and both are the bare-`func`
compatibility escape hatch the task required to keep working: a parameter declared
bare `func` has no signature to check against, and an argument that carries no
signature of its own - a bare `func` value forwarded from elsewhere - has nothing
to compare. `InferredArguments::functionHasSignature` is new and is what says so;
before, "no signature" and "zero parameters" were the same empty vector, and a
forwarded bare `func` was refused for having the wrong arity. The runtime type
assertion still covers both.

Gates: `tests/zl/valid/language_hardening_tests/FuncSignatures.zl` (a signed
parameter, a signed slot called directly, a signed instance-method parameter, a
signed return called through a variable, and bare `func` taking any callable)
under MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and `--backend native`;
`tests/zl/invalid/type_errors/FuncSignatureArity.zl`, `FuncSignatureParamType.zl`
and `FuncSignatureReturnType.zl` pin the three diagnostics. The README limitation
is deleted and [`docs/language-guide.md`](language-guide.md#lambda-expressions)
replaces its "Known gap" paragraph with the contract.

## 2026-09-18 - The `Shared<T>` lost-update measurement is a regression, and the docs stop implying safety (P1-8)

The acceptance was "either confinement is checked at compile time or `share()`
exists and the docs stop implying safety". Reading the code showed both halves
already existed and only the writing was wrong: `share()` is a native
(`NativeId::SHARED_SHARE`, with a checker arm that instantiates `Shared<T>` from
the argument's type), and the capture rule
(`capturedValueCrossesThreadBoundary`) has rejected a non-`Shared` mutable
capture across `Thread.start`/`Task.spawn` since before this change. The README
nonetheless still said "The top-level `share()` helper and compile-time
confinement checks are pending."

What was left, and is now done:

- **The README bullet says what is and is not guaranteed.** `Shared<T>` makes a
  capture legal, not safe; each cell operation is synchronised, a
  `get()`/`setValue()` read-modify-write across the pair is not.
- **The language guide names three fixes instead of two.** `withLock` - the cell's
  own lock, and the one that fits a counter - was missing from the list that only
  offered `Atomic` and `Mutex`.
- **`examples/advanced/SharedState.zl` demonstrates the safe form** next to the
  unsynchronised pair it warns about, instead of warning and moving on.
- **The 2490 measurement fails loudly instead of silently.**
  `tests/zl/valid/concurrency_regressions/SharedLostUpdateMeasurement.zl` runs
  four threads x 2000 unsynchronised `setValue(get() + 1)` on one `Shared<int>`,
  alongside the same work under `withLock` and under `Atomic`, and asserts all
  three: `Atomic` exact, `withLock` exact, and the unsynchronised counter *below*
  8000. The third assertion is the point - if it ever fails, `Shared<T>` became
  synchronised and the four documents that say otherwise are stale, and the
  failure message says exactly that. Measured on a 2-core box: plain 2771-3670 of
  8000, both guarded counters exactly 8000, on all four configs.

No compiler or runtime behaviour changed here; this is the documentation and the
regression the task asked for. The capture check remains what it always was - a
check on *what* a closure carries, not on the arithmetic inside it.

## 2026-09-18 - An empty literal in argument position takes the parameter's type (P2-9)

```zl
static func countEntries(map<string, int> m): int { return Collection.length(m) }

countEntries({})      // was: no overload of 'countEntries' matches the given
                      //      argument types; candidates are: - P29.countEntries(map)
                      // now: 0
```

The empty-literal fix above (A9) read the container kind from the *declaration*,
and in argument position there is no declaration yet - arguments are inferred
before overload resolution picks a parameter to offer. This is the other half of
that plumbing, and it is a reusable step rather than a special case:

- `TypeChecker::argumentExpectations` reads one expectation per argument position
  from the call's own arity-matching candidates. A position is usable only when
  every such candidate describes it as the *same* parameterized container
  (`map<string,int>`, `list<int>`, `Set<int>` - the element types live in the
  name, which is what `inferCollectionLiteralExpected` checks against).
- `inferArguments` takes those expectations and re-infers an **empty** literal
  through the existing `inferExpected`, which is the same route a typed
  declaration uses - so `expressionTypes_` and `targetCollectionKind` are set
  exactly as they are there, and all four backends read the result the same way.
- It is wired into all six argument-inferring call paths: qualified static,
  implicit self, method call, `new`, `super(...)`, and a call through a func
  value (which has one signature rather than a candidate set, so it builds its
  expectations from the slot's own parameter types).

Two limits, both deliberate. A **non-empty** `{1, 2}` never takes an expectation:
that spelling is also how a variadic argument list is written, and re-reading
`Text.format("{0}", {1})` as a set would be wrong - the fixture pins it. And where
the arity-matching candidates disagree about the container (`f(map<K,V>)` and
`f(list<T>)` together), `{}` has no single answer, so the position is left alone
and resolution reports the mismatch rather than guessing.

Gates: `tests/zl/valid/core_tests/EmptyCollectionLiterals.zl` now covers the
argument-position form directly - static, instance method, generic `Map`, a `{}`
reaching a `list<int>` parameter, a non-empty `{1, 2, 3}` still a list - under
MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and `--backend native`;
[`docs/language-guide.md`](language-guide.md#empty-literals) drops the "bind it
first" workaround.

## 2026-09-18 - `match` destructures `Option`/`Result`, and a value-less `match` is a statement (P1-1, P0)

```zl
static func report(Result<int, string> r): string {
    return match r { Ok v => "ok " + v.value  Err e => "err " + e.error }
}

match n { 1 => log("one")  _ => log("other") }   // no value: a statement
```

`match` could not pattern-match the two built-in sums at all, although the three
exhaustiveness rules the language wanted for them existed as unreachable code.
`Ok v` was rejected by the resolver (`generic type 'Ok' requires 2 type
argument(s)`) and `Ok<int, string> v` passed the pattern test and then failed the
exhaustiveness subtraction with `uncovered Result<int,string>` - no case pattern
could ever cover a member that was the parent class. Both are fixed:

- **A built-in sum subject is read as its case set.** `TypeChecker::builtinSumCases`
  returns `Some<T>|None<T>` for `Option<T>` and `Ok<T,E>|Err<T,E>` for
  `Result<T,E>` (verified against `SemanticModel::directSubclasses`, which is new),
  and `inferMatchExpr` seeds `remaining` with the cases instead of the parent. A
  case pattern erases its case, so covering both is exhaustive and a missing case
  is reported by name (`non-exhaustive match: uncovered Err<int,string>`).
- **Omitted pattern arguments are filled from the subject.**
  `TypeChecker::fillTypePatternArgs` walks the case's parent chain and inverts
  `Ok<T,E> extends Result<T,E>` into concrete arguments, writing them back into
  the arm's annotation - the same write-back positional data patterns already get,
  so the checker, the AST compiler and the MIR lowerer all see one spelled-out
  type. `Ok v` and `Ok<int, string> v` compile to the same program.
- **`None` and `Err` name their case in pattern position** instead of binding a
  variable called `None` (a `Variable` pattern whose spelling is one of the
  subject's cases becomes that case pattern). Expression position is unaffected.
- **Nil is not a case.** The implicit nil member is not added for a sum, so no
  `null` arm is required; a nil subject matches no arm and hits the lowering's
  unmatched path, which raises the catchable `Exception("non-exhaustive
  match")`. A `null` arm or a wildcard is allowed and reachable, never required -
  the arm-loop's "subject is already covered" check exempts them.
- **A pattern naming the sum itself covers its cases** when the arguments agree
  (`TypeChecker::typePatternCovers`): `match r { Result<int,string> whole => … }`
  is exhaustive, and a case arm after it is unreachable. This is the same shape
  the runtime's `reflectiveObjectMatches` accepts, so the static and dynamic
  readings cannot disagree.
- **`isAssignable` is untouched.** The widening REVIEW.md O21 warned against is
  not what this needed - the pattern test already passed; only coverage did not.
- **A reopened hierarchy stays open.** `class Mine extends Result<int,string>`
  makes `directSubclasses` more than the two cases, so the plain pair stops being
  exhaustive for that program (pinned by an invalid fixture).
- **Separately: a `match` whose arms produce no value was refused by MIR.** Every
  arm being a `log`/assignment made the match void-typed, and the lowering still
  declared a void result slot, stored the nil constant into it and loaded it back:
  six `mir.type-flow` contract violations for a program the AST backend ran. The
  slot, the stores and the load now exist only when the match has a result
  (`kNoSlot`, new in `include/zl/mir/value.hpp`).

Gates: `tests/zl/valid/language_hardening_tests/OptionResultMatch.zl` (both
spellings, both sums, guards, a `null` arm, a catch-all, a nil subject raising
catchably and a value-less `match`) under MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0`
and `--backend native`, wired into `ctest` as `sum-match-parity` via
`tests/sum_match_parity.py`; `tests/zl/invalid/type_errors/IncompleteSumMatch.zl`
and `OpenSumMatch.zl` pin the two diagnostics; `examples/advanced/OptionType.zl`
and `ResultType.zl` now demonstrate the case patterns in place of
`if (r.isOk())`. See [docs/language-guide.md](language-guide.md#matching-a-sum).

## 2026-09-18 - `list`, `set` and `map` are names where no type can appear (P1-4)

```zl
var list = [1, 2]              // was: syntax error: Expected variable name -- got "list"
Collection.push(list, 3)
list = [4, 5]
for set in 0..3 { }

list<int> order = [1, 2]       // the type readings are untouched
map<string, int> counts = {}
set<int> unique = {}
```

The lexer runs without context, so `list`/`set`/`map` arrived as type keywords and
were reserved everywhere: `var list = new List<int>()` was refused with no
escaping hatch. They are now contextual names, the way `shared` already was:

- `Parser::checkName` / `expectName` accept the three spellings in every name
  position - `var`/`let`, the name of a typed declaration, function/lambda
  parameters, fields, loop variables and catch variables.
- `Parser::looksLikeTypedDeclStart` decides statement position by lookahead: a
  statement is a typed declaration only when a complete type annotation is
  followed by a name, so `list<int> xs = []` declares while `list = []`,
  `list.push(1)` and `list.length()` use the variable. The same rule now accepts
  `int list = 3`, where the *name* is the keyword.
- `ExpressionParser::parsePrimary` reads the three spellings as identifiers (no
  type can appear in expression position), so a variable called `list` can be
  passed, assigned, indexed and used as a method receiver.

Function and method *names* remain identifiers, so the `Map.put` naming note
stands. A token sequence that spells both readings (`list < n > x`) is read as
the declaration, exactly as it already is for a user-defined generic type.

Gates: `tests/zl/valid/language_hardening_tests/ContextualNames.zl` covers the
type readings, variables, fields, typed declarations, parameters, loop variables,
catch variables, lambda parameters and statement-position method calls, under
MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and `--backend native`. See
[docs/language-guide.md](language-guide.md#naming-and-the-collection-keywords).

## 2026-09-18 - `string` has methods, and they are the `String.*` natives (P1-3)

```zl
var s = "Hello, World"
log(s.length())           // 12  - the same call as String.length(s)
log(s.startsWith("He"))   // true
log("42".toInt() + 1)     // 43
```

`string` was the last builtin type with no method surface: `s.length()` failed with
`cannot call method 'length' on value of type string` while `List`, `Map`, `Set`,
`Option` and `Result` were all method-based, and the only way to reach the primitives
was the `String.length(s)` free-function form. The fix adds no class and no second
implementation - a method call resolves to the native catalog entry, with the receiver
bound as the native's first parameter:

- `TypeChecker::inferMethodCall` looks the method up in `kStringMethods`
  (`src/compiler/type_checker.cpp`), validates arity and argument types against the
  catalog signature, and records the resolved name and id on the node
  (`MethodCallExpr::isStringMethod` / `nativeMethodName` / `nativeMethodId`).
- The reference AST compiler emits `CallNative` for that entry; MIR lowering emits the
  same `emitCallNative` with the receiver first. `s.length()` and `String.length(s)`
  are the same instruction, and `tests/pipeline_tests.cpp` asserts exactly that.
- The surface is the 29 `String.*` primitives that take a string first: `length`,
  `charAt`, `substring`, `contains`, `startsWith`, `endsWith`, `indexOf`,
  `lastIndexOf`, `indexOfFrom`, `replace`, `split`, `trim`/`trimStart`/`trimEnd`,
  `upper`/`lower`, `compare`/`compareIgnoreCase`, `codePointAt`, `repeatText`,
  `toInt`, `toFloat`, and the seven `utf8*` scalar-indexed methods. Semantics are the
  primitives' own: indexing stays the byte boundary
  (`"héllo".length()` is 6) and `split` returns the native `list<string>` storage, so
  `Collection.*` reads it back or `Text.split` wraps it into a `List<string>`.
- `startsWith`/`endsWith` had no native to map onto, so they became primitives in
  `src/vm/native.cpp` (a byte prefix/suffix compare) and `Text.startsWith`/`endsWith`
  now delegate to them instead of carrying a second ZL copy.
- A miss is a compile-time error that teaches the surface - "type 'string' has no
  method 'trimLeft' (string methods: charAt, …)" - and `s.length(1)` /
  `s.contains(3)` are type errors rather than native contract throws at run time.

`repeat` is a lexer keyword, so the repeat method keeps the native spelling
`repeatText` (the same reason `Text.repeatText` exists). An untyped lambda parameter is
still `UNKNOWN` until annotated - that is P1-2's bare `func` signature, not this
surface - so `func(name) => name.length()` needs `func(string name)`.

Gates: `tests/zl/valid/language_hardening_tests/StringMethods.zl` checks every method
against its qualified spelling under MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and
`--backend native`; `tests/zl/invalid/type_errors/StringMethodUnknown.zl` pins the
diagnostic; `testStringMethodsAreTheStringNatives` in `tests/pipeline_tests.cpp`
compiles both spellings of one program, requires identical output, and asserts in the
emitted bytecode that the method spelling is a `CallNative` of the mapped native with
no method dispatch in the body. See
[docs/language-guide.md](language-guide.md#string-methods).

## 2026-09-18 - A lambda inside a lambda body no longer donates its return type (P0-3)

```zl
static func callIt(func f): int { return f() }

var body = func() {
    var n = callIt(func() { return 1 })
    if (n == 1) { log("yes") }
}
body()          // was: printed "yes" forever; now: prints it once and returns
```

The default (MIR) pipeline ran that body as an endless loop, and the reference AST
compiler reported `type assertion failed for return: expected int, got void`. The
cause was in the checker, and it stayed hidden unless one lambda appeared inside
another's body: `inferLambdaExpr` infers a block body's return type from an
accumulator (`lastFunctionReturnType_`, `lastFunctionReturnClassName_`,
`lastFunctionHadReturn_`) that every `return` statement writes into, and a *nested*
lambda left its own result in that accumulator on its way out. The enclosing lambda
then read `int` as its inferred return - a non-void signature for a body containing
no `return` at all. MIR lowered the body's exit block to `unreachable`, since nothing
may legally reach the end of a non-void function, and the VM looped on it.

`currentReturnType_`, `currentFunctionName_` and friends were already saved and
restored around the body check for precisely this reason; the four accumulator fields
now join them, so a lambda's return inference is private in both directions - it
neither inherits the enclosing function's returns nor hands its own back.
`--emit-mir` on the reproducer shows `func ...$lambda0(): void` with a `return` in its
exit block where it previously showed `: int` with `unreachable`.

Pinned by `tests/zl/valid/language_hardening_tests/NestedLambdaReturn.zl` - nesting
three deep with a different return type at each level, a lambda built inside a loop,
the enclosing lambda's own `return` winning over the nested one's, and the same nest
inside a worker thread. It passes under MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and
`--backend native`, and it did not terminate at all before the fix. That is also how
the bug was found: the `Condition` fixture below hung on a waiter closure whose
predicate was a lambda.

## 2026-09-18 - `Condition` is exercised from a worker thread (P2-4)

`tests/zl/valid/concurrency_regressions/ConditionWorkerSignal.zl` closes the last
untested claim in that directory: a `Condition` waited on inside a spawned thread and
signalled from a different one. Three shapes, all with the waiting done by a worker:

- twenty rounds of the documented safe pattern (`Condition.waitUntil` over an `Atomic`
  flag, signalled from a second thread), each round asserting that the waiter woke on
  the published state - a hand-off that works only on the first attempt is a race, not
  a feature;
- the bare primitive: a worker blocks in `Condition.wait` and another thread's
  `notifyAll` wakes it. A notification delivered before the waiter blocks is lost by
  design (`stdlib/zl/lang/Condition.zl` says so), so the signaller keeps notifying
  until the waiter reports itself awake, bounded at 500 attempts - which turns a broken
  wait into a failed check rather than a hung fixture;
- `Condition.waitFor(gate, 0.2)` with nobody notifying, asserting it returns `false`:
  the bounded wait has to be bounded from a worker too.

`examples/basics/Conditions.zl` turned out to be about `elif`, and
`tests/runtime_sync_tests.cpp` pokes the primitive's C++ state directly, so neither
covered this.

## 2026-09-18 - The one-type-per-file rule, and the reason for it (P2-6)

[`docs/packages.md`](packages.md) gains a section stating the rule and why it holds.
The short version: an import names a *type* and is resolved by a path computation
alone (`io.github.test.Helpers` -> `io/github/test/Helpers.zl`) - no file is opened and
there is no index of which type lives where - so the stem match is what lets one name
be both the file to load and the type it delivers. The dotted name is also the module's
identity: diamond imports, circular-import detection and the `class 'X' is defined in
both ... and ...` provenance error all key on it, so one file is one node in that graph.

The rule is also narrower than every document claimed. It is one *primary type* per
file, and a `data`, `interface` or `enum` declaration satisfies it just as a `class`
does; helper declarations may share the file, in any order, and travel with the primary
import (`import app.Lib` makes `Lib`'s helpers visible too) - they simply cannot be
imported by their own name, because there is no file of that name to resolve to.
[README.md](../README.md) and [`docs/language-guide.md`](language-guide.md) said "a
class" and "one class per file"; both now say what the loader
(`src/compiler/module_loader.cpp`) actually enforces, and point at the rationale.

## 2026-09-18 - The native-resource stress suite's flake was its own assertions (P2-8)

The suite failed 7 runs in 40 solo (and 12 in 40 on a busier box), which TASKS.md
recorded as "~7% of runs ... prints `all native resource stress regressions passed` and
then exits non-zero ... a static-destruction or thread-teardown race". It never printed
the pass line on a failing run, and nothing happened after `main`: three assertions in
`tests/native_resource_stress_tests.cpp` were wrong about what a correct race looks
like.

- **A borrow that goes invalid is the lifetime tracking working.** `borrow()` can hand
  back a view that is already invalid, because the owner was consumed and destroyed in
  the window between the registry lookup and the check - that is exactly what
  `NativeResourceBorrow`'s `weak_ptr` lifetime is for. The test counted it as `sawDangling`
  ("a valid borrow with the wrong handle is a lie") and incremented no counter, so both
  of its assertions failed together. Invalidated borrows are now their own outcome, and
  the dangling check still means what it says.
- **The hammer ran before the pool was drained.** Leaving the claim loop only means
  every token has been *claimed*; another worker can still be between its claim and its
  consume. A worker that ran ahead consumed that token itself - uncounted, since the
  hammer only tallies rejections - so "every token was consumed exactly once" lost one
  and the rejection count gained one. The hammer now waits for the drain it assumes
  (with a deadline, so a genuinely lost token still fails loudly instead of hanging).
- **Two 5 ms sleeps assumed the invokers would be scheduled inside them.** On a loaded
  two-core box they were not, `ran` was still 0 when `close()` landed, and the suite
  reported "invocations ran before close()" - a complaint about the runtime that was
  really a complaint about the scheduler. Both phases now wait for the fact they assert
  on: close after the first successful invocation, stop after the first refusal.

Gates: 500 consecutive solo runs and 50 `ctest -j2` runs clean, plus 300 runs with two
cores pinned busy (before the fix: 3 failures in 6 loaded runs). A red line in this
suite is evidence about your change again.

## 2026-09-18 - An empty `{}` is a set, and a map-typed slot accepts one (A9)

Two symptoms, one cause - an empty literal was forced through the same shape checks as
a populated one:

```zl
map<string, int> ages = {}    // was: type error: map literal requires key:value entries
Map<string, int> generic = {} // was: type error: Map literal requires key:value entries

var tags = {}                 // inferred list<unknown>, so:
set<int> declared = tags      // was: [mir.type-flow] store of list<unknown> into set<int>
```

An empty literal holds nothing that could contradict a declaration, so in
`inferCollectionLiteralExpected` the declaration now decides the container for all
three native spellings and all three generic ones. With no declaration to go by, the
spelling decides: `[]` infers an empty `list` and `{}` an empty `set`, matching
[`docs/language-guide.md`](language-guide.md)'s own rule that lists use `[...]` and
sets and maps use `{...}`. The reference compiler needed one line to agree - an empty
literal recorded as `"map"` fell through to `Collection.newList` and tripped the
declaration's runtime assertion.

A **non-empty** `{1, 2}` with no declared type stays a `list`. That spelling is also how
a variadic argument list is written (`Text.format("{0} + {1}", {"1", "1"})`), where set
de-duplication would silently eat arguments; the fixture asserts exactly that.

One case stays open and is now written down rather than discovered twice: an empty
literal in *argument* position has no declaration to lean on, so `countEntries({})`
against a `map<string,int>` parameter is still a compile error - arguments are inferred
before overload resolution. Bind it first (`map<string,int> empty = {}`), which the
fixture does; propagating the expectation into call arguments is tracked as P2-9 and
belongs with P1-1/A1.

Gates: `tests/zl/valid/core_tests/EmptyCollectionLiterals.zl` (both native and generic
slots, both spellings, the inferred-then-declared flow, a call boundary, and the
duplicate-keeping variadic case) under MIR, `ZL_COMPILER=ast`, `ZL_MIR_OPT=0` and
`--backend native`; [`docs/language-guide.md`](language-guide.md#empty-literals).

## 2026-09-18 - `String.split(s, "")` splits characters, not bytes (A10)

`String.split("héllo", "")` returned six fragments - `h`, then the two halves of `é` as
mojibake, then `l`, `l`, `o` - because the empty-separator path iterated `char`s. An
empty separator means "every character", and a character in this codebase is a UTF-8
scalar, so it now walks the same boundaries as `String.utf8CharAt` (the scalar helpers
moved above the `String.*` block in `src/vm/native.cpp` so one implementation serves
both). `Text.split` forwards to it and inherits the behaviour; ill-formed bytes still
come back one at a time rather than hanging the walk.

Gates: the split cases in `tests/zl/valid/language_hardening_tests/Utf8Text.zl` (a
two-byte scalar, a four-byte one, and split-then-join round-tripping the original
string) under MIR, `ZL_COMPILER=ast` and `ZL_MIR_OPT=0`;
[`docs/stdlib.md`](stdlib.md#zltext).

## 2026-09-18 - The reference compiler stops emitting returns nothing can reach (A6)

Every function body and every block-body lambda was followed by an implicit
`push nil; return`, including a body that already ended in `return expr` or `throw` - a
`Return` does not fall through, so those bytes were unreachable, on every function in
the program and the whole standard library. `Compiler::compile` and
`compileLambdaExpr` now emit the tail only where control flow can actually reach the end
of the body, decided by a deliberately conservative `canFallThrough`: a `return`, a
`throw`, a block whose last statement cannot fall through, or an `if`/`else` whose arms
all cannot. Anything else - a loop a `break` might escape, a `finally` that might
swallow a throw, any statement kind not listed - keeps its tail, because dropping one
the VM needs is worse than a few dead bytes.

Gates: `testUnreachableReturnTailIsNotEmitted` in `tests/pipeline_tests.cpp` counts
returns per function over the code the compiler emitted for it (a named function's body
runs to the next entry address, a lambda's to its `MakeClosure`), pins the conservative
direction too - an `if` with no `else` and a body with no return keep the implicit one -
and checks the fixture still prints the same six values. Against the old behaviour it
fails 4 of its checks, so it is not vacuous.

## 2026-09-18 - `boundary-lint-regressions` stopped failing on its own output

`ctest -j2` went red on this suite twice in two runs, each time announcing

```text
FAIL: new-expression construction: diagnostic does not name src/main.cpp:3
    boundary-lint: rule 3: ... src/main.cpp:3: heap construction of the front end: ...
```

that is, a missing diagnostic, printed underneath the complaint. The check was
`printf '%s\n' "$output" | grep -q "$file:$line:"` under `set -o pipefail`: `grep -q`
exits at the first match, `printf` then dies of SIGPIPE, and pipefail reports the
*pipeline* as failed. Measured in isolation: 4 false negatives in 200 trials with two
cores busy, 0 in 200 idle - which is why it looked like a flake of the thing under test
rather than of the test.

Both sites in `tests/boundary_lint_tests.sh` now use a `case` substring match with no
subprocess to race with, and the same shape in `tools/boundary_lint.sh`'s ignore-pattern
check - where a false negative means an ignore silently not ignoring, i.e. a phantom
violation - is a here-string. Gates: 15 runs of the suite with two cores pinned busy,
0 failures; `ctest -j2` green 3 of 3 (was red 2 of 2).

## 2026-09-18 - `zl-runtime-sync-tests` failed whenever the machine was quiet

`ctest -C Release` - the gate [TASKS.md](../TASKS.md) documents, with no `-j` - failed
this suite 199 runs in 200 on an idle two-core box, and passed 30 in 30 with both cores
busy. `testReadWriteLockState` starts two writers and four readers and then asserts
`reads > 0` ("readers actually ran"), but nothing ever made the readers run: on two cores
the writers own both from start to finish, the readers are not scheduled until
`writersDone` is already set, and they leave on their first check. Under load the writers
are preempted, the readers get a core, and the suite passes - so the only person who saw
this failure was a developer running the documented gate on a small machine, and CI,
which runs the suite in parallel, never did.

The writers now wait until every reader has taken the lock at least once before their
first write, which makes "readers actually ran" a fact the test establishes rather than
one it hopes for; the wait cannot deadlock, because a reader never waits on a writer.
Readers also yield between reads: without that, four spinning readers starve the writers'
exclusive lock and the same test takes four seconds instead of a tenth of one. Measured
after the fix: 0 failures in 100 solo runs and 0 in 25 with both cores pinned busy, at
0.095 s per run.

## 2026-09-17 - A block-body lambda with an untyped parameter compiles on both pipelines (P0-1)

```zl
static func twice(func f): int { return f(3) }
log(twice(func(x) { return x * 2 }))    // was: refused outright, now: 6
```

The default (MIR) pipeline rejected the whole program with
`[mir.return]: block b1 returns a value from a void function`, and the
reference AST compiler died at runtime with an argument-count mismatch - while
the arrow form `func(x) => x * 2` worked. The cause was return-type inference
for the block body: a lambda's inferred return stayed UNKNOWN when every
`return` in the block had an UNKNOWN type, and UNKNOWN then collapsed to
nil/void. The checker now tracks that a return was *seen* even when its type is
UNKNOWN (`lastFunctionHadReturn_` in `src/compiler/type_checker.cpp`), so a bare-`func` block body keeps the UNKNOWN return the arrow form already
gets, instead of dropping to void while the body still emits `return x * 2`.

Both spellings are pinned side by side - block and arrow, bare `func` callee
and `func(int): int` callee - by
`tests/zl/valid/language_hardening_tests/LambdaBlockBody.zl`, which prints `6`
from every form and is exercised by `scripts/run_regressions.sh`; the
reproducer above prints `6` under MIR, the reference AST compiler, unoptimized
MIR, and `--backend native`.

## 2026-09-17 - `log` prints a collection's data, not its storage (P2-1)

`log(l)` on a `List<double>` printed `List{__native: [3.14, 1]}` - the wrapper's
plumbing rather than the payload. `Serialize.encode` had already learned to
look through the `List`/`Map`/`Set` wrappers (REVIEW.md O11); the printer now
does the same: `formatValueInner` (`src/vm/value.cpp`) unwraps the `__native`
field of the three generic collections and formats the payload directly, so
`log(l)` prints `[3.14, 1]`, `log(m)` prints `{"pi": 3.14}`, and a value keeps
one spelling through `log`, concatenation, `Text.format`, and the JSON
encoder. The look-through preserves the existing totality guards: a collection
that (indirectly) contains itself still prints a `<cyclic>` marker instead of
overflowing the stack.

No example printed a bare collection, so no expected output changed. The
behaviour is pinned by `tests/zl/valid/core_tests/CollectionPrinting.zl`
(payloads, empty collections, nesting, string quoting, cycles), documented in
`docs/language-guide.md` (Generic collections), and REVIEW.md O11 records the
printer half next to the encoder half.

## 2026-09-17 - `Time.format` rejects a token it does not recognise (P2-2)

`Time.format(0, "%Y-%m-%d")` returned the pattern unchanged - the tokens are
ZL's own (`YYYY`, `MM`, `DD`, `HH`, `mm`, `ss`), and an unrecognised one was
left in place silently, which was the easiest way to print a wrong date
confidently. `formatCivilTime` (`src/vm/native.cpp`, shared by `Time.format`
and `Time.utcFormat`) now raises on any unknown `%`-token, naming the expected
token set.

`docs/stdlib.md` documents the raise, `examples/advanced/TimeLib.zl`
demonstrates the rejection next to the correct tokens (expected output
updated), REVIEW.md O10 is closed, and
`tests/zl/valid/language_hardening_tests/TimeFormatTokens.zl` pins the token
set - every token through the deterministic `Time.utcFormat`, a full pattern on
the epoch and a leap day, and the raise on strftime-shaped patterns for both
entry points.

## 2026-09-17 - One voice for the empty list (P2-3)

`List.first()` on an empty list threw `List.first called on empty list` (ZL,
naming the declared type), while `List.pop()` delegated to the storage
primitive and surfaced `Collection.pop: cannot pop from an empty list` (native,
naming the primitive). `List.pop` (`src/compiler/builtin_library.cpp`) now
guards emptiness in ZL exactly like `first()`/`last()` already did, so every
wrapper method names the type the user declared and the operation they asked
for. The raw `Collection.pop` primitive keeps its own name when called
directly - which is then what the user asked for.

`tests/zl/valid/core_tests/EmptyListErrors.zl` pins both messages (and
`List.last`, and the direct primitive call, and the `firstOr`/`lastOr`
fallbacks, and that a successful pop still round-trips). REVIEW.md's note
under O18 is updated.

## 2026-09-17 - The legacy IR folder spells a folded double exactly (P0-2)

`rewriteInstructionConstants` wrote a folded double back into the instruction's
`symbol` with `std::to_string` - six fixed decimals - and `parseConst` read it
back with `stod`, so any constant needing more precision was silently rounded
on its way through the optimizer. Latent (the only consumer is the legacy
`--emit-native` tier, which refuses double-returning `@native` functions), but
one feature away from a miscompile. All double-writing sites in
`src/compiler/ir_optimizer.cpp` now use the runtime's
`zl::doubleToShortestString` - the language's one spelling for a double - and
the integer-only identity folds keep `std::to_string` on `std::int64_t`, which
is exact.

`tests/ir_tests.cpp` gained `testOptimizerFoldsDoublesExactly`: `0.1 + 0.2`
folds to a `Const` whose symbol is the full 17-digit `0.30000000000000004`,
unary minus keeps every digit (`-0.30000000000000004`), and both spellings
parse back to bit-identical doubles.

## 2026-09-17 - Stale claims deleted, and the fixed-array gap re-verified (P1-5, S1-S3)

Four write-ups recorded failures that no longer reproduce, and each one cost
the next reader a full reproduction:

- REVIEW.md O19 (`INT64_MIN` cannot be written as a literal) now records the
  fix instead of teaching the `(0 - INT64_MAX) - 1` workaround.
- The block-bodied-lambda claim in `research/report.md` §2.7 now points at the
  real remaining gap (the untyped block-body parameter, P0-1) instead of the
  already-working typed form.
- `research/corpus/limitations/FixedArrayNative.zl` and
  `docs/status/mir-safety-evaluation.md` recorded that passing a fixed array
  through `Collection.*` lost the element type (`array[5]<int>` downgraded to
  `list<unknown>`) and the verifier rejected it. It does not:
  `isCollectionType` (`src/mir/type.cpp`) classifies `TypeKind::Array` as a
  collection, the fixture verifies clean and runs on both pipelines (re-run
  today: `len=5`, `last=5`), and the status document marks the two affected
  workflows as fixed rather than deleting them - the baseline's other findings
  stand.

All four TASKS.md entries are deleted; nothing here changed behaviour, so the
gates that cover the underlying fixes are the ones named above.

## 2026-09-17 - Doubles print as the shortest decimal that reads back (F9)

`log(3.14)` printed `3.1400000000000001`. The arithmetic was never wrong - the
printer was. `valueToString` formatted every double with 17 significant digits
(`std::numeric_limits<double>::max_digits10`), which round-trips but is the
*longest* decimal that does; the examples then documented that spelling as
"the shortest decimal that reads back as the same binary double", which is
exactly backwards. It is the first double a beginner prints (`examples/REVIEW.md`,
F9).

**One formatter, and it is the shortest one.** `doubleToShortestString`
(`src/vm/value.cpp`) spells a finite double as the shortest digit string that
parses back to the same bits: `log(3.14)` is `3.14`, `Math.PI` is
`3.141592653589793`, and `0.1 + 0.2` is still `0.30000000000000004`, because
that digit really is there. Nothing was traded away - `String.toFloat` of any
printed form returns the identical double, and `Serialize.decode` of any
encoded one returns it too.

The digits come from `std::to_chars` in scientific form where the toolchain's
`<charconv>` supports floating point, and from a precision-ascending `strtod`
round-trip loop where it does not (GCC 10, older libc++). The *style* - fixed
versus scientific - is decided in this file rather than left to `to_chars`,
whose choice is not pinned by the standard and differs between libraries:
libstdc++ writes `123456789012345680` for a value another library writes as
`1.2345678901234568e+17`. Since `examples/run_all.sh` compares bytes on three
platforms, the spelling of a program's output cannot be allowed to depend on
the host it was built on. The rule is printf `%g`'s / Python's `repr`: fixed
while the decimal point sits in `-3..17`, scientific outside it. Both digit
sources were compared over 1.5M values - random bit patterns, denormals,
`DBL_MAX`, powers of ten - for exact round-trip, identical digit strings, and
that style rule; the committed checks are the unit test in
`tests/runtime_type_tests.cpp` and the ZL fixture below.

Behaviour that did *not* change is worth stating, because it looks like it
should have. A whole-valued double already printed without a fraction (17
significant digits of `1.0` is `1`), so `log(1.0)` is still `1` and `-0.0` is
still `-0` - the assertions `LiteralFoldParity.zl` makes about zero are
untouched. The fixed/scientific cutoffs also match what `%g` at 17 digits did,
so `1e+21`, `10000000000000000` and `0.0001` are byte-identical to before.
What changes is only ever digits that were not needed: `3.1400000000000001`
becomes `3.14`, `9.9999999999999995e-08` becomes `1e-07`,
`1.0000000000000001e-05` becomes `1e-05`, and `123456789012345678.0` keeps its
`1.2345678901234568e+17`.

**The JSON encoder carried its own `setprecision(17)`.** `Serialize.encode`
serialized a double with more digits than `log` showed for the same value, so a
value had two spellings depending on which door it left through. It now calls
the same formatter; `{"pi":3.1400000000000001}` is `{"pi":3.14}`, and decoding
still round-trips.

Pinned by `tests/zl/valid/language_hardening_tests/DoubleFormatting.zl`, which
is self-checking and runs under all four configurations (MIR, reference AST,
unoptimized MIR, `--backend native`) through `tests/double_format_parity.py` -
ctest `double-format-parity`, mirroring `backend-fold-parity`. Expected output
in `DataTypes.zl` and `MathLib.zl` is updated, as are `examples/README.md`,
`docs/language-guide.md`, and the F9 verdict in `examples/REVIEW.md`.

## 2026-09-17 - Windows and macOS CTest: the leftover failures are real bugs

The first CI run recorded three leftover CTest failures rather than skipping
them. They were host-target mistakes, not "the suite does not run there".

**A host with no encoder is select-only, not a pipeline error.**
`compileMirToNative` always called `emitModule` after a successful selection.
Win64 is described and has no encoder, so every Windows host failed with
`no machine-code encoder for target 'x86_64-pc-windows-msvc'` even though
`PipelineOptions.selectOnly` already documented that outcome. Emit is now
skipped when `encoderAvailable()` is false; the native IR is the result, and
SysV bytes are not produced under a Windows name. `zl-native-backend-tests`
compiles fixtures against `x64SysVTarget()` (the one encoder) instead of
`hostTarget()`, and pins the Win64 select-only path with
`testNoEncoderIsSelectOnly`. An unknown host (macOS arm64) no longer walks
off `lir.functions.front()` after `require()` continues: that was the 0.02s
SegFault. The compiler pipeline cross-compiles Unknown hosts to SysV so
`--backend native` stays a real backend rather than a hard error on every
non-x86 machine.

**An async frame cannot resume twice, even when `std::function` move is a copy.**
libc++'s small-object `std::function` may leave the source callable after a
move, so `AsyncFrame::resume` taking `std::move(resume_)` made the second
resume run the continuation again instead of throwing. It now `swap`s the
continuation out, which empties the member on every library. That was the
macOS-only `zl-runtime-scheduler-tests` failure at 0.03s.

**The hardening VM is a shared owner, not a stack object.** `VM` is
`enable_shared_from_this`; the suite stack-allocated one and then
dereferenced `optional<Chunk>` after a successful compile. It now
`make_shared`s both the VM and the chunk (the entry point Await actually
needs) and quotes the boundary-lint bash path so Git's `Program Files` bash
on Windows is not split by `cmd.exe`.

## 2026-09-16 - Generic methods land, and the CI gate is run for the first time

**Method-level type parameters close O16.** `static func firstOf<T>(List<T>
items, T fallback): T` used to be a syntax error (`Expected '(' after func name
-- got "<"`), which is why `Generics.zl` wrote one helper per element type: only
generic *classes* existed. The parser now accepts type parameters on function
declarations and explicit type arguments at the call site; the checker
instantiates them for both static and instance calls, rejects a method type
parameter that collides with a class type parameter, and the bindings are
threaded through bytecode, MIR and the VM. Static `Call` is the acceptance
path. Covered by `GenericMethods.zl` plus type-mismatch and name-collision
fixtures.

**The documented regression command now works when it is run as documented.**
`scripts/run_regressions.sh` moved to `scripts/` before it looked at its first
argument, so the invocation in `docs/development.md` and in the CI workflow -
`bash scripts/run_regressions.sh build/zl_language all` - always failed with
`error: 'build/zl_language' is not an executable file`: a relative path was
being resolved against `scripts/`, where no `build/` exists. The compiler path
is now resolved against the calling directory before the script changes its
own. Worth noting *why* this survived review: every previous check passed an
absolute path, so the command was never once executed in the form it is
documented and run in CI.

**The two parallel review branches are one line of history again.** One branch
carried the native-tier and CI work, the other the module-root, UTF-8 `Text`,
git-URL-allowlist, O2-lambda and generic-method fixes; they shared a merge base
and conflicted in exactly one file, `.github/workflows/ci.yml`, which both had
added independently. Resolved by keeping the full three-platform matrix and
adopting the sibling's `-C Release` (required by the multi-config Visual Studio
generator on Windows, where `ctest` would otherwise look in the wrong
configuration directory), its explicit `-DBUILD_TESTING=ON`, and a per-test
`--timeout`.

**The cross-platform promise was unverified, and the first CI run found it
broken in six places.** macOS and Windows had never been built by CI, and both
failed. `fork` and `_exit` were called without including `<unistd.h>`, which
glibc's `<sys/wait.h>` supplies transitively and libc++ does not. `NOMINMAX`
was undefined, so the `min` and `max` macros in `windows.h` turned
`std::numeric_limits<T>::max()` and `std::min`/`std::max` into syntax errors at
the call site rather than at the macro. `std::filesystem::path::preferred_separator`
was appended to a `std::string`, but it is `wchar_t` on Windows. `popen` and
`pclose` were called unguarded where MSVC declares only `_popen`/`_pclose`. And
a closure held in a local `auto` inside another lambda - whose return type is
itself being deduced - is not typed in time by MSVC, so the first call to it is
parsed as a function-style cast; that table is now filled by a named function.
Every one of these is invisible to g++, which is precisely why they survived:
the suite that could have caught them did not exist until now.

Diagnosing them required a change to the workflow as well. The build is teed to
a log and its errors are re-emitted as `::error::` annotations, because the log
archive is not reachable from every environment and annotations are. Two of the
six fixes were wrong on the first attempt and CI said so, which is the argument
for having the run in the first place.

**Known remaining, and not caused by this work.** macOS and Windows now build
for the first time and each runs 37 of the 40 CTest cases green. Three macOS
cases and two Windows cases still fail, and they are recorded here rather than
skipped or hidden. `zl-native-backend-tests` faults on macOS; the backend emits
x86-64 while those runners are arm64, and macOS requires `MAP_JIT` for
executable memory, so execution is now guarded out - yet it still faults at
startup, which means something outside the execution path is also involved and
is not yet identified. `zl-runtime-hardening-vm-tests` segfaults on both
platforms and `zl-runtime-scheduler-tests` fails on macOS only. None of the
three is touched by this branch: they were failing before there was a CI run to
see them, and the honest description of the cross-platform promise is that it
was never tested rather than that it held.

**The CI gate was executed end to end for the first time.** The workflow had
never run - it was assembled from documented commands because no `cmake` was
available here. With one installed, the Linux job's every step is now verified
against this tree: configure, `zl-tests` (36 targets), CTest 40/40, the
regression corpus 49/49 with the package-manager cases included, all five
differential harnesses (51/51 backend and MIR paths, 77 optimiser cases), and
the native gate. Every step passes.

## 2026-09-15 - The native tier honours the arithmetic contract, and the SysV shadow space is real

**The emitted code is fail-closed like the VM.** An external harsh review of
the toolchain found that the native backend's *emitted* machine code was silent
where the language is fail-closed: `INT64_MAX + 1` wrapped to `INT64_MIN`,
`INT64_MIN / -1` reached `idiv` and raised `#DE`, shift counts were masked mod
64 (`1 << 64` computed `1 << 0`), and float overflow produced a quiet infinity.
None of it is reachable in executed programs yet (the VM is the only execution
driver), and the differential harnesses cannot see it because all three of
their arms run on the VM - so it existed only in the bytes, and the one suite
that executes bytes had no test for any of it. The emitter now handles each
case the way the tier's existing zero-divisor guard does: branch to a trap
(`ud2` → SIGILL) instead of executing the faulting instruction, with the one
exception the language defines - `INT64_MIN % -1` computes `0`. A non-finite
double can no longer exist in native code: `fdiv` traps on a zero divisor, and
every float operation traps when its exponent field comes out all ones.
`zl-native-backend-tests` gained `testArithmeticTraps`, which executes all
fifteen fault cases in forked children (a trap is an assertion, not a
test-suite death) and the healthy boundary values in the parent.

**The System V model no longer denies the shadow space.** `x64SysVTarget`
recorded `shadowSpace = 0` and a test asserted "System V has no shadow space";
PSABI 3.2.2 mandates 32 bytes, so both conventions now agree, the field's
comment says what it means, and every function that calls reserves it in its
frame. Not observable today - emitted callees never write the shadow, and the
tier emits no runtime calls or stack arguments yet - but it is what a future
`CallRuntime` callee from the C world needs, and a calling frame short of the
shadow would hand that callee the caller's saved `rbp`.

**The checks now run on every push.** Until now every claim in this
repository - 44 regressions, the three-way backend parity, the optimiser and
promotion differentials, the boundary rules, the safety pipeline - was true
when a maintainer ran the tools locally, and that was it. `.github/workflows/
ci.yml` runs the same commands on every push and pull request: the Linux job
takes the full gate (CTest plus the regression corpus, all five differential
harnesses, and the native compiler gate), and macOS and Windows build and run
CTest so the portability claim is verified rather than assumed. Nothing in the
workflow is CI-only, and every gate already exited non-zero on a mismatch - a
green line is the claim, not a hope.

## 2026-09-12 - The boundary is enforced, and the compiler knows what runs

The MIR boundary had rules and a component; this phase makes the rules fail the
build when they break, and gives the compiler one question it could not answer
before: which functions a program can actually enter.

**The boundary rules are now a check.** `tools/boundary_lint.sh` (CTest
`boundary-lint`) reads the tree and fails on a broken rule: a backend that
includes the AST, the parser, the type checker, the module loader, the pipeline or
the legacy IR; a typed lowerer that touches the checker except through
`expressionType`; a `ModuleLoader` or `TypeChecker` constructed anywhere but the
pipeline; a fifth consumer of `zl::ir`. It found the last hand-driven front end
on its first run - `--emit-native` was still building its own roots, stdlib
version check and type checker - which is exactly the drift the rules exist to
prevent, so that path now runs pipeline stages 1-2 like every other command and
keeps only its legacy emitter.

**`--emit-native` is no longer the exception.** It is still the one consumer of
the untyped `zl::ir` (that is what "frozen, not grown" means), but its front end
is the pipeline's, so it inherits the shared roots, the stdlib-version check and
the failure/exit-code mapping instead of its own copies. The MIR-based tiers
(`--emit-native-ir`, `--emit-native-code`, `--emit-machine-code`) are unaffected.

**The compiler can now say what a program can run.** `zl::mir::reachability`
answers it from the verified module: an over-approximation that keeps every
override in the receiver's hierarchy, keeps every same-named function when a
dispatch site names a class the module does not have, keeps a referenced static's
initializer (statics initialise lazily), and refuses to answer at all when the
program reaches reflection's invoke family, because then the call graph is not
closed. The direction of the approximation is the point: a function it calls
unreachable really is unreachable, which is what a future dead-code pass needs.
`--backend native` and `--pipeline-report` print it, so `5 function(s) compiled,
283 left to the VM` is now followed by `1 of 288 function(s) can run` - the
totals are about the module, and most of a module is stdlib the program never
calls. Nothing deletes code yet; this is the analysis, adoption is separate.

**`--strict-native` (`ZL_NATIVE_STRICT=1`)** turns `--backend native` into an
all-or-nothing check for programs written to the native subset: it fails with the
refusal reasons instead of reporting a partial tier.

**`Function::simpleName` keeps its signature on purpose, and now says so.**
Writing the dispatch test surfaced the trap: `simpleName` is the rendered name
(`"speak()"`, `"add(int,int)"`), because two overloads of one name have to stay
distinguishable, while a call site names the bare method. Every caller that
matched one against the other had to slice the string itself.
`Function::declaredName()` is that slice, named once, and the field's comment now
describes what it holds instead of calling it the "unqualified source name".

**Smaller fixes, each measured.** A failed stage now closes its own ledger line,
so `--pipeline-report` shows where the pipeline stopped rather than stopping
between two lines. A misspelled option is a usage error (exit 2) instead of being
read as an entry file, which used to report `could not open file:
--emit-mirr`. The structural digest now hashes callee identity and the
constant/static/class/interface pools, so two modules that differ only in which
of two identical functions a call names no longer digest the same.

**Checks.** `zl-pipeline-tests` is 225 checks, adding the native ledger contract
(every function accounted for exactly once, every refusal carrying a reason),
strict-native's refusal and that it does not affect the bytecode backend, the
dispatch/reachability cases, reflection refusing to close the graph, and a
library having no answer. `tools/boundary_lint.sh` is new and in CTest. All five
differential harnesses still pass on the corpus (50 of 50 identical each, 76
equivalent for the static check), and every MIR, lowering, optimiser, ownership,
type and native target still passes.

## 2026-09-11 — MIR becomes the compiler boundary

MIR stopped being a side pipeline. The compiler is now one staged pipeline -
source → lexer/parser → semantic analysis → typed lowering → MIR → verification
→ optimisation → selected backend - and every command in `zl_language` is that
pipeline with different stage options. Documented in
[`docs/pipeline.md`](pipeline.md).

**The pipeline is a component, not a convention.** `zl::pipeline::Pipeline`
(`include/zl/compiler/pipeline.hpp`, `src/compiler/pipeline.cpp`) owns six named
stages with stated invariants, records what each one did, and refuses to run one
out of turn: a stage's postcondition is the next stage's precondition. Skipped
stages are recorded as skipped rather than omitted. Failures are data
(`Failure { kind, stage, message, detail }`) and the kind selects both the
reported label and the exit code, so a MIR failure is never reported as a
lowering failure and vice versa.

**No backend is ever handed unverified MIR.** Stage 4 runs `verifyModule`; stage
6 verifies again immediately before a backend consumes the module, so a module
mutated between the two is caught at the boundary rather than by a backend
noticing something odd later. Turning stage 4 off without that re-check makes
code generation *refuse* - there is no path from a source file to a backend that
skips verification. `compileMirToNative` keeps its own re-check as well.

**Backend selection is a code-generation decision only.** `--backend
bytecode|native` (or `ZL_BACKEND`) picks the generator; both are handed the same
module, which `zl --pipeline-report` proves mechanically by compiling the program
once per backend and comparing the structural digests of the MIR boundary. The
reference AST → bytecode compiler is kept unchanged behind `ZL_COMPILER=ast`, and
`tools/backend_diff.sh` is the new gate: every example compiled and run three
ways (reference, bytecode backend, native backend) must print identical bytes and
exit identically, currently 50 of 50.

**The default path now goes through MIR.** `zl file.zl` lowers to MIR, verifies
it, optimises it (`ZL_MIR_OPT=0` to disable) and translates it with the bytecode
backend. The VM is untouched and remains the behavioural reference, and it stays
the execution driver: `--backend native` generates machine code for the subset
the native tier can prove and reports `execution: VM`, because mixed-mode native
execution does not exist yet. `tools/mir_backend_diff.sh` compares the reference
path against the pipeline on the whole corpus (50 identical, 0 gaps).

**Duplicated lowering removed.** `Compiler` no longer lowers every program into
the legacy `zl::ir` and discards the result; `--emit-machine-code` now writes the
same `ZLM1` container from the MIR native backend instead of going AST → `zl::ir`
→ x86-64; and the eight hand-rolled copies of "load, check, lower, verify" in
`main.cpp` are one set of stage options. `zl::ir` is left with a single
quarantined consumer, the portable-C++ emitter behind `--emit-native`, which
prints a note saying so.

**New checks.** `zl-pipeline-tests` (188 checks) owns the composition: stage
order, the unverified/modified-module refusals, backend parity in both MIR and
behaviour, the opt-in stages being invisible when on and visible when skipped, and
the exit-code table. `tools/backend_diff.sh` adds cross-process backend parity
per program. All existing MIR, lowering, optimiser, ownership, type and native
targets still pass, as do the four pre-existing differential harnesses (50 of 50
each).

## 2026-09-11 — MIR optimiser framework

The framework first, the transformations second, and the proof that they are
safe running underneath both. Everything here is opt-in and additive: the
bytecode compiler and the VM are untouched and remain the behavioural reference.
Documented in [`docs/mir-optimizer.md`](mir-optimizer.md).

**Framework.** `PassManager` owns an ordered list of passes created by name from
a registry, so a pipeline can be written down (`ZL_MIR_OPT_PASSES=fold-constants,
propagate-copies`) and the curated ordering lives in one place. It runs to a
fixpoint because the passes feed each other — the last one makes something
constant that the first one can then branch on. `FunctionAnalysisManager` owns
one result per (analysis, function) and hands out references, with freshness
stated as the caller's duty: a pass that mutates calls `invalidate()`. Any pass
that reports a change is followed by `verifyFunction`, run with
`unreachableBlocksAreErrors = false` so that branch simplification may strand
blocks for the pruning pass to remove; if verification fails the function is
**restored from its pre-pass copy** and the failure recorded, so a broken
rewrite never reaches a backend. Three levels of before/after inspection: in-
report snapshots, a `ZL_MIR_OPT_SNAPSHOT_DIR` file per changed pass with a
bounded line diff, and `ZL_MIR_OPT_VERBOSE=1` for the per-pass trace.

**Safety, stated once.** `effects.hpp` is the single answer to "what does this
instruction do besides compute" — 17 kinds, with only `Pure` and `ReadsMemory`
compatible with deletion, and `MayThrow` required to be *discharged* first.
Discharge happens in three tiers (all-constant operands so the evaluator
decides; a constant that makes the failure impossible whatever the rest are;
operand types that exclude it), which is what makes `x + 0` removable while
`x * y` is not: ZL raises on overflow, and deleting a possible runtime error is
a behaviour change. The floating-point identities are narrowed the same way —
`x * 1.0` yes, `x + 0.0` no, because `(-0.0) + 0.0` is `+0.0` and the sign of
zero is observable.

**Passes.** Constant folding, constant propagation, dead-block elimination,
dead-value elimination, algebraic simplification, redundant-conversion removal,
branch simplification and local copy propagation. None of them reorders, sinks,
hoists, inlines or duplicates: every rewrite is either replacing a use with
something provably equal or deleting work that provably cannot be observed.
Deliberately absent, and documented as such: no function removal (reflection
reaches functions by name), no inter-procedural transform, no dead-store
elimination across paths.

**Differential validation, twice.** `compareModules` compares the modules
structurally and by observable event — events are never *invented*
(unconditionally), and never *lost* (measured against the unoptimised module
with the default pipeline run over it, so removing code that cannot run is not
reported as a divergence while removing code that can is). It deliberately does
not count runtime checks (the optimiser may delete one it has proved cannot
fail) or local stores (deciding whether one is observable is the same work the
optimiser does to remove it), and it cannot see values at all — so
`tools/mir_opt_diff.sh` runs every program in `examples/` both ways and compares
its output and exit status byte for byte. Currently **50 of 50 identical, none
skipped**. `tools/mir_opt_check_all.sh` is the wide sibling: it needs no backend,
so it also covers the 26 stdlib modules - the generics, `async`, task, lock and
FFI code the backend stubs today - and is currently **76 of 76 equivalent**.

**A miscompile the runtime half caught, and the static half could not.**
`examples/intermediate/Closures.zl` printed `1 1 1` where it should print
`1 2 3`. Dead-value elimination had removed a store to a slot nothing in the
function read again — but that function is a closure body, and a closure body's
slots are the closure's *captured environment*: ZL captures by value, and a
captured `var` that a closure mutates persists between calls, which is the whole
point of `makeCounter`. Nothing changed a single observable event, so no amount
of comparing event lists would have noticed. `eliminate-dead-values` now declines
every store in a function with captures (how the runtime spells capture
persistence is a backend contract the pass does not restate), and both suites
pin it.

The "no event lost" rule is measured against a reference built by the *same*
pipeline (`DifferentialOptions::referencePipeline`), so a pipeline named with
`ZL_MIR_OPT_PASSES` is judged against itself rather than against the default
one - measuring a module built by one pass against a reference built by eight
reports every event the other seven would have deleted.

Regressions: `tests/mir_pass_tests.cpp` (`zl-mir-opt-tests`), 146 checks of
hand-built MIR with no front end; `tests/mir_opt_pipeline_tests.cpp`
(`zl-mir-opt-pipeline-tests`), 79 checks that lower, optimise, verify, compare
statically and then *run both versions* and compare their output. Command line:
`--emit-mir-opt <out|->`, `--mir-opt-check`, and `ZL_MIR_OPT=1 --mir-vm`.

## 2026-09-11 — MIR lowering: structural match, copy-update, and try/finally

The last lowering gaps close. `try/finally`, structural match patterns, `data`
copy-update, function references, and object-typed collection literals now lower
into valid MIR that executes identically through `--mir-vm`; the bytecode
compiler/VM stays the semantic reference, and every case below was checked
against it with a fixture in addition to the 50-example differential corpus.

**try/finally.** `ExceptionHandler` gains an `isFinally` flag. A finally handler
is a catch-all placed innermost in its try's chain (after the catches, so the
catches get first refusal), whose target is a `Cleanup` block that binds the
thrown object into a pending slot, runs the finally body, and throws the slot
again. Normal completion runs the same body in a plain block, and
`return`/`break`/`continue` inline the active finalizers innermost-first, mirroring
the reference compiler's `activeFinallyBlocks_`. The bytecode backend spells a
finally handler as `PushFinallyHandler` and binds the pending slot exactly like a
catch binding. Two backend bugs surfaced and were fixed along the way:
`handlerGroups`' union-find dereferenced `end()` once a component's root was
reached (latent until a try had two handlers — two catches, or a catch plus a
finally), and `PushFinallyHandler`'s patched target never got the per-function
base offset (latent until the handler was actually reached).

**Structural match.** `lowerMatchPattern` walks one arm's pattern recursively —
`data` destructures fields, `list`/`set` check length then members, `map` checks
entry presence then values — and every sub-pattern branches to the same
fall-through the top-level arm chain uses. Positional data patterns
(`Pair(v, w)`) failed in the checker first (`unknown data field 'Pair.'`), so
`TypeChecker` now writes declaration-order field names back into the arm before
the bytecode compiler or the lowerer sees it.

**data copy-update.** `base with { ... }` lowers to `alloc` plus per-field
copies (walking the parent chain, as the verifier does) and coerced updates.

**A promotion bug the new shapes exposed.** `promoteSlotsToBlockParameters`
profiled every store at instruction index 0 instead of its real index, so the
"every store already carries the slot's declared type" eligibility check read
the block's first instruction. It still passed because the old match shape put a
`refine` at index 0 of each arm body, which incidentally declined the arm's
slots and left the function with nothing to promote; the structural-match shape
moved the refine to its own block, the decline disappeared, and a dynamic
(`unknown`) slot got promoted and silently retyped to `list<int>`. Profiling now
records the real instruction index, so the eligibility check reads the store it
is deciding about and the dynamic slot stays in memory form.

## 2026-09-10 — MIR backend: the differential corpus is the whole example set

The last 7 known-gap examples — Generics, Closures, CollectionAlgorithms,
Exceptions, GenericRuntimeChecks, Lambdas, StaticMembers — now run identically
on `--mir-vm` and the reference path, so `tools/mir_backend_diff.sh` enforces
the whole example tree — 50 runnable programs across `basics/`, `intermediate/`
and `advanced/` — with an empty `KNOWN_GAPS`, and `tools/mir_promotion_diff.sh`
reports all 50 identical with and without promotion and 0 skipped (the `_lib`
module sources are excluded from its listing instead of counted as skips). A
tree-wide `--mir-vm` run reports zero stub warnings: no example reaches the
fail-closed path. The earlier "24 matched / 7 gaps" checkpoint's gaps are
gone. Semantics stay the reference's: every fix below removes a place where
the MIR path disagreed with it.

**Closures end to end.** `emitCallIndirect` walked its arguments with
`i + 1 < size` over `operands[1..]`, silently dropping the last (or only)
argument of every indirect call — the "VM stack underflow" behind
Closures/Lambdas. Lowering coerced `unknown` operands inside the dynamic
binary-result path even when the operator table had already classified the
operation, emitting a runtime `AssertType` the reference never performs;
operands of unknown type are now exempt there (with the verifier's binary
checks early-returning on unknown), which is what let untyped lambdas compose.

**Static members.** A `Thread.start(...)` result nobody reads died at the
wrong time: `defineTemp` now emits `Pop` for never-read temps
(`collectReadTemps` scans operands, terminator values and edge arguments), so
a discarded value's lifetime ends at the pop point exactly like the reference's.

**Generics.** `refineArguments` skipped the receiver slot in the argument
positions but indexed parameters from 0, pairing the first real argument of
every constructor call with the receiver's own `this: C<T>` type — a generic
constructor's unknown argument was then "refined" to the class type, an
assertion the language never makes (`new Box<int>(transform(...))` asserted
`Box<int>` against an int). Skip counts now apply to both sides.

**Runtime checks.** `Task.block/ignore/cancel` are runtime task operations,
not methods (the stdlib `Task` is a marker class), so the backend now emits the
dedicated opcodes instead of failing to resolve a dispatch slot; `await`
translates to the VM's `Await`. The verifier accepts `object` parameters from
any non-nil value (the language's dynamic annotation) and walks generic
instantiations through their base layout (`List<string>` → `object`).

**The reflection layer is only as honest as the metadata.** Three registrations
lied, and GenericRuntimeChecks caught each: functions were registered with
empty `parameterTypeNames` (an empty entry is a mismatch, not a skip — every
reflective call broke), with bare class names for compound types (an assert
against `Box` rejects a `Box<int>`), and with no `returnTypeName` (a callable
whose own return type is unregistered fails every higher-order call).
`ClassReflectionInfo.baseTypeName` now carries the full extends clause
(`Base<B>`, not `Base`), so `Proj<A,B> extends Base<B>` threads its bindings
through to the base's parameters. `share()` tags its box through CallNative's
factory-type operand, matching the reference.

**Class-layer collections.** A class-spelled literal (`List<int> s = [5]`) used
to lower to a raw native list — an untyped value where the language promised a
real object. Class-layer `NewCollection` now builds the object the reference
builds (`NewObject` tagged with the rendered instantiation + the empty
constructor), literal filling goes through the class's own `push`/`add`/`put`
(the typed boundary the language runs), and index reads on class-layer maps go
through `GetIndex` like every other `c[k]`.

**Regression coverage.** All 5 C++ suites pass (MIR, SSA, lowering, types,
ownership 16/16); `examples/run_all.sh` stays 50/50.

## 2026-09-10 — MIR: ownership and lifetime as events, not metadata

ZL's ownership model now survives lowering. The MIR keeps each storage's
contract (`gc`/`owned`/`borrow`/`shared` on slots and parameters) and
represents the lifetime events as instructions: `move` empties a slot and
continues the value; `borrow`/`end_borrow` bracket a function-scoped view;
`drop` releases a resource in two exclusive spellings — a value operand, or
the **storage-release form** (slot named, no operands) that lowering emits
for the end of an owned local's lifetime. Documented in
[`docs/mir.md`](mir.md); regression suite in `tests/mir_ownership_tests.cpp`
(`zl-mir-ownership-tests`).

**Lowering.** Every return site — each `return`, the implicit end of a void
body, each lambda's exit — emits reverse-order releases for owned slots that
were not moved and for owned parameters, mirroring the reference compiler's
`DropVar`-before-`Return` placement. Slots whose value was `move`d are
skipped: the reference clears the local and the frame teardown releases the
rest, and the MIR path does the same. GC slots get no events; closure
captures are always GC values, so `MakeClosure` carries none either.

**Verification.** `checkOwnershipFlow` now tracks
`{moved, dropped, droppedParams, borrows}` across the CFG with union joins —
the same rule the type checker's `joinOwnershipStates` uses, so MIR rejects
exactly the programs the checker does, no stricter and no looser at branch
and loop joins. The invalid states it catches: use after move, use after
drop, double drop ("a resource releases exactly once"), drop after move,
drop while borrowed outside the exit-cleanup region (a ZL borrow is
function-scoped and ends with the function, so the trailing release cannot
conflict with it), borrow of a moved or released owner, storage-release of
non-owned storage, and `end_borrow` without a borrow. A move is final: the
checker rejects assigning to a moved variable, so no store resurrects one
here either.

**Backend.** The bytecode backend translates the events to the reference
runtime's own opcodes — `MoveVar`, `DropVar`, and value rebinding for borrows
(the runtime has no aliasing; lifetimes are the verifier's job) — and
registers owned locals in `ownedLocalNames` so the VM's frame teardown
releases them on early return and exception exactly as the reference path
does. Differential runs (`tools/mir_backend_diff.sh`) stay at 24 matched /
7 known gaps; examples stay 50/50.

## 2026-09-10 — MIR: the ZL type system, end to end

MIR now carries the language's *type semantics*, not just its shapes, and the
boundaries where a dynamic value becomes a typed one are explicit instructions
instead of silent retypes. Documented in [`docs/mir.md`](mir.md); regression
suite in `tests/mir_type_tests.cpp` (`zl-mir-type-tests`).

**Types.** `Option<T>` and `Result<T,E>` are first-class kinds carrying their
payload/[ok, error] type ids; `Some`/`None`/`Ok`/`Err` keep their class spellings
and relate to the sums by a verified assignability rule (`Some<int>` satisfies
`Option<int>`, `Some<string>` does not, and the relation composes under
arguments). Unions are ordinary types everywhere — a function may declare
`int|string` as its return type, which the verifier previously rejected outright.
Nested generics survive as structural ids, so `Map<string,List<int>>` and
`Option<List<int>>` keep their arguments as type ids, not rendered strings.
Native/resource semantics stay where the language actually has them: FFI values
are typed by their declared renders and resource behaviour by slot/parameter
ownership with the move/borrow/drop dataflow.

**Boundaries.** A dynamic (`unknown`) value crossing into typed territory — a
typed local or assignment, a call argument, a return, a field/element write,
a `match` arm — now lowers to an explicit `refine` (the runtime type
assertion), and the verifier rejects any `unknown` operand that reaches a typed
destination without one, so an invalid type assumption fails at the boundary
instead of being trusted downstream. The MIR→bytecode backend translates
`refine` to a real `AssertType`, `type_test` to `MatchType`, and index reads
through `GetIndex`, which fixed `--mir-vm` on every program that indexes a
class-spelled collection (`names[0]` on a `List<string>`). SSA promotion
declines to promote a slot whose stores are retyped relative to the slot's
declared type, so promotion can no longer erase a declared `unknown` (or any
declared contract) out from under a `match` subject.

**Compiler type resolution.** Dispatch signatures erased every concrete generic
instantiation to a bare `object`, so `label(Option<int>)` and `label(List<int>)`
collided in every function table keyed by the rendered name — the second
declaration silently replaced the first and calls dispatched to the wrong body,
caught (when at all) by a runtime assertion. A `GENERIC_OBJECT` parameter now
keeps the generic class's erased base name (`label(Option)`, `label(List)`):
still one body per declaration, never per instantiation, but no declaration can
shadow another.

**Verification:** `zl-mir-type-tests` (new), the three existing MIR suites, the
50-example corpus, `type_boundaries.py`, the LSP/test-runner Python suites, and
differential `--mir-vm` runs against the reference path all pass.

## 2026-09-09 — MIR: a typed mid-level IR with a verifier

ZL now has a real mid-level intermediate representation, in `zl::mir`
(`include/zl/mir/`, `src/mir/`). It is reached with `zl --emit-mir <out|->
<file.zl>` and is documented in [`docs/mir.md`](mir.md).

**Why.** The compiler had one internal graph, `zl::ir`, and it could not carry
what a backend needs: types were `std::string` names, terminators were mixed in
with value-producing instructions, successors were duplicated separately from the
terminator that implied them, and generics, unions, function types, exceptions,
field access and indexing had no representation at all. Rather than retrofit
that, MIR is a new layer beside it. `zl::ir` is unchanged and still feeds
`--emit-native` and `--emit-machine-code`; the bytecode compiler and the VM are
untouched, and no language semantics changed.

**What it is.** Types are interned structural values, so a type *is* its id and
`TypeId` equality is type equality. Nullability is derived from the kind rather
than stored. Generic parameters, unions and function signatures are first-class
kinds — nothing was erased to `object` to make a first version tractable.
Functions are SSA over temps, with mutable locals as explicit slots and
`Load`/`Store`; parameters stay SSA unless the body writes, moves or borrows
them. `Instruction` and `Terminator` are separate types with exactly one
terminator per block, which is the invariant everything else about the CFG rests
on. Exceptions are unwind edges with handler chains; ownership is a dataflow
problem the verifier solves over reverse postorder.

**A generic function is a template, not a copy.** `class Set<T>`'s methods are
lowered once with `this: Set<T>`, and the call site records which instantiation
it selected. That keeps one source of truth instead of a function per
instantiation, and it is what makes "passes `Shared<int>` but the parameter is
`Shared<T>`" checkable rather than a false mismatch.

**Verification.** `verifyModule` enforces 26 documented invariants — SSA
uniqueness, dominance, block reachability, predecessor consistency, operand and
result typing, call signatures, field and index legality, ownership flow, and
the shape of exception edges. `FunctionBuilder::finish()` guarantees two of them
structurally, so a function that was only partly lowered is still valid MIR and
carries its caveat in `Function::incomplete` instead of as a broken graph.

**Lowering** covers expressions, statements, classes and inheritance, generics,
closures including nested ones and ones that capture `this`, `List`/`Map`/`Set`
calls, `match`, `data` record literals, annotated collection literals,
try/catch, async and await, and `Shared<T>`. Unsupported constructs produce a
note and an incomplete function, never malformed MIR. Across `examples/`, 54 of
57 files lower and verify completely and the rest verify with notes; none fail.

A closure that captures `this` needed two things, not one. `this` is captured by
name — semantic analysis puts `"this"` in `captureStorageNames` — so the
enclosing body has to bind it under that name for the capture to resolve; and
inside the closure the captured slot has to become the body's receiver, because
`this` there is spelled `ThisExpr` and answered from the receiver rather than
from the scope. Such a closure written inside `class Box<T>` also closes over
`this: Box<T>`, and an unsubstituted type parameter is only legal inside a
template, so the closure's MIR function is declared generic over its owner
class's parameters.

`match` lowers to a chain of two-way branches, one test block per arm in source
order, with the subject evaluated exactly once before the first test — so a
subject with a side effect cannot run once per arm, and a guard that reassigns
the subject's variable cannot change what a later arm compares against. Arm
bodies store into a result slot the join block reads back, since this IR has no
phi. A type pattern emits the new `TypeTest` and then `Refine`s both the arm's
binding and the subject identifier the body keeps spelling, which is what
narrows `int|string` to `int` inside an `int n =>` arm.

`TypeTest` was the gap that blocked it: MIR could *assert* a type (`Refine`, the
typed form of the VM's `AssertType`) but not *ask* about one, and a match arm has
to survive a "no" and fall through rather than raise. It is the typed form of the
VM's `MatchType`.

**Compiler fixes made along the way**, each at the layer that owned the problem
rather than worked around in MIR: `zlTypeName` moved next to its declaration out
of `type_checker.cpp`; the builtin `Math.PI`/`E`/`TAU` table unified into one
place instead of three copies; `zl::forEachChild` added to the AST so passes
scanning a body have one exhaustive answer to "what is inside this node" —
hand-rolled walkers were silently missing anything nested inside a call
argument; `array[N]` sizing made optional so a dynamic `array<int>` is not
recorded as `array[0]<int>`; and nullability made a question for
`TypeArena::isNullable` rather than for the top-level kind alone. The last was a
real defect: `string` is nullable in ZL even though the VM holds it inline rather
than behind a collector handle, and a union is nullable when any member is — so
`int|string` admits `null` while `int|double` does not. Reading only the kind
called every union non-nullable and rejected the `null` arm of a match over one.

Two more surfaced once `match` and record literals were lowering. `TypeChecker`
records each expression's type for the backend seam in one funnel, `inferExpr` —
but a collection literal written under an annotation (`list<int> xs = [1, 2]`)
took a special-case branch in `inferExpected` that bypassed it, so the checker
proved the type and then never told anyone, and every backend saw UNKNOWN. And
the verifier's `checkIndexAccess` handled lists and arrays but not maps, so it
rejected `index_store` on a `map<K,V>` even though the instruction taxonomy has
always said index access covers maps. Both are the kind of gap that only shows up
once a second consumer starts asking.

The same check turned out to have a third blind spot, found by writing a program
the example corpus does not contain: `set<int> s = [1, 2, 3]`. The VM runs it
fine, but MIR rejected it — a literal is built as `new_collection` plus one
`index_store` per element, and the index check covered lists, arrays and maps
while a set fell through to "indexed access requires a List<T>". Indexing also
recognised the `List<T>` class spelling but not `Set<T>` or `Map<K,V>`, and
`new_collection` recognised none of them, so a spelling could be indexable but
not constructible. `isCollectionType` is now the single answer to "is this a
collection" and both construction and indexing ask it, which is what stops the
two spellings drifting apart again.

One more came from the last two notes in the corpus. `share(x)` is a native with
no namespace and the catalog keys it by its bare name, but the call lowerer only
consulted the catalog for qualified names, so a bare `share` fell through to the
implicit self-call path and reported `call to 'C.()' which was not lowered` —
the empty dispatch being the giveaway that no method had ever been resolved. The
bytecode compiler special-cases `share` by name; the lowerer now asks the
catalog by the bare name instead, which is the same test without the
hardcoding. That cleared the note and, with it, a downstream one: the value
`share` produced had been unresolved, so a later method call on it could not
name a class either.

**Tests.** `tests/mir_tests.cpp` (`zl-mir-tests`) builds MIR by hand and checks
the verifier rejects each class of malformed module — 47 cases.
`tests/mir_lowering_tests.cpp` (`zl-mir-lowering-tests`) drives the real
pipeline end to end and asserts on the specific properties an earlier lowerer
got wrong — 29 cases.

## 2026-09-08 — Thread failures are catchable; `shared` usable as an identifier

Adversarial probing of the runtime found three defects.

**An uncaught exception on a thread killed the process.** `Thread.start`'s
worker wrapper caught everything and called `std::terminate()`, on the premise
that "an exception escaping a Thread is process-fatal by definition". A ZL
program could therefore be aborted by a worker with no way to observe or handle
the failure - the output was a bare `terminate called after throwing an
instance of 'zl::ZlThrownException'`. Threads now capture the failure in a
`StoredException`, exactly as tasks already did, and `Thread.join` rethrows it
in the joining thread. Worker failures are catchable by their real type,
including runtime faults such as an out-of-range index, and a program that
never joins gets an ordinary diagnostic and a nonzero exit instead of an abort.

**Stored exception payloads could be collected.** `StoredException` reported
its managed payload through `appendGCRoots`, which works for holders the
collector traces (tasks, static fields) but not for threads, which it does not
trace. Under allocation pressure a worker's exception object was collected
before the join and the message degraded to `ZL exception`. `StoredException`
now pins its payload with a `ProtectedGCRoot` for its own lifetime, so the
payload survives regardless of who holds it. Verified with twenty concurrently
failing workers and heavy churn between throw and join: all twenty messages
arrive intact.

**`shared` was unusable as a variable name.** It is an ownership modifier, but
the parser already accepted it as an identifier in declarations
(`var shared = ...`). Any later use failed: `shared.push(1)` reported
`Expected a type name`. The statement parser now treats `shared` as a modifier
only when a type actually follows it, and the expression parser accepts it as
an identifier, so `shared int x` and `var shared = ...; shared.push(1)` both
work.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes. Also verified during this pass: GC churn over 140,000 objects, 5,000-
deep recursion, typed exceptions propagating through 500 frames with intact
stack traces, recoverable `StackOverflowError`, typed failures surviving task
and awaited-chain boundaries, and 8 threads x 1000 atomic increments summing
exactly.

## 2026-09-08 — Correct calendar arithmetic; Date/DateTime/TimeOfDay rebuilt

Probing the time facades across a daylight-saving boundary exposed two real
correctness bugs, not just missing features.

**Adding a day shifted the clock.** `addDays` added a fixed 86400 seconds, so
noon on 2024-03-09 plus one day came back as **1 PM** on 2024-03-10 in any zone
observing a spring-forward transition. `addWeeks` inherited the same defect.

**`endOfDay` could return the wrong date entirely.** It was `startOfDay +
86399`, but a DST day is 23 or 25 hours long, so on 2024-03-10 in New York it
produced `2024-03-11 00:59:59` — the following day.

Both are fixed by doing calendar arithmetic on civil day numbers (Howard
Hinnant's `days_from_civil`) and rebuilding the timestamp through
`Time.fromParts`, rather than by manipulating elapsed seconds. `endOfDay` is
now derived from the next day's midnight, which is correct for every day
length. `diffDays` counts calendar days to stay consistent with `addDays`.

**New `zl.time.Calendar`.** The pure civil-calendar math lives in a leaf module
that depends only on the native clock primitives, so `Time` and the value
classes can share it without an import cycle (`Time` already imported `Date`).
`Time` now delegates to it instead of carrying its own copy.

**The two arithmetic families are now named apart.** `addSeconds`/`addMinutes`/
`addHours` are elapsed time and intentionally shift the wall clock across a
transition; `addDays`/`addWeeks`/`addMonths`/`addYears` are calendar time and
preserve the local time of day. `DateTime` exposes both, documented, so
`dt.addDays(1)` and `dt.addHours(24)` correctly differ across a DST boundary.

**Value classes rebuilt.** `Date` gained construction from parts, week/month/
year arithmetic, period boundaries (`startOfMonth`, `endOfYear`, …),
`daysUntil`, weekday/month names, and day-based comparison so two timestamps on
the same date compare equal. `DateTime` gained both arithmetic families,
`date()`/`timeOfDay()` views, difference helpers and instant-based comparison.
`TimeOfDay` gained validated `of(h, m, s)` construction, within-day wrapping
arithmetic, part-of-day predicates and difference helpers.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes. A 40,000-day civil round trip is exact; leap-year rules (1900, 2000,
2024), year boundaries, negative offsets and pre-epoch dates all verified; and
a sweep over every day of four transition months found zero drift in
America/New_York, Europe/London, Australia/Sydney, Asia/Manila and
Pacific/Chatham.

## 2026-09-08 — Concurrency primitives made usable

Adversarial probing of the synchronization primitives found two that could not
be used correctly from ZL at all.

**Semaphore was unusable.** A `Semaphore` starts with zero permits and nothing
exposed a way to set them, so any `acquire()` blocked forever by construction.
Added `Semaphore.setPermits`, a non-blocking `Semaphore.tryAcquire`, and
`Semaphore.releaseMany`. The facade adds `withPermits(n)` construction and
`withPermit`/`tryWithPermit` scope helpers that release the permit through a
`finally`, so an exception in the body cannot leak it. Verified with six
threads contending over two permits: observed concurrency never exceeded two.

**Condition could hang indefinitely.** `Condition.wait` has neither a predicate
nor a timeout, so a notification delivered before a waiter blocks is lost and
that waiter sleeps forever. Added `Condition.waitFor(seconds)`, which returns
whether it was notified before the deadline, and facade helpers `waitUntil` /
`waitUntilTimeout` that re-test a predicate around bounded waits and therefore
cannot miss a wakeup permanently. The lost-wakeup hazard of the bare `wait` is
now documented at the primitive rather than left to be discovered.

The previously empty `Atomic`, `Mutex`, `RwLock`, `Condition` and `Semaphore`
facade classes now carry real content: construction with an initial value
(`Atomic.ofInt/ofBool/ofDouble`, `Semaphore.withPermits`), counter helpers, and
scope helpers. `Atomic.toggle` is explicitly documented as not atomic as a
whole. Atomics were verified genuinely thread-safe: eight threads each adding
1000 produced exactly 8000.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; catalog and runtime bindings remain in exact correspondence (246 each).

## 2026-09-08 — Lambda return-type annotations

Lambdas may now declare a return type: `func(int x): int => x * 2`. Previously
the parser rejected the annotation outright ("Expected '=>' or '{' after a
lambda's parameter list"), so a lambda's contract could only be inferred, and
typed parameters combined with an explicit result were unwritable.

The annotation is enforced rather than decorative: the body's inferred result
must be assignable to the declared type, and a mismatch is a compile error
naming both types. When present the declaration overrides the call site's
expectation, so a lambda's stated contract remains visible to callers. Block
bodies, expression bodies, `void` lambdas and untyped parameters are all
supported; omitting the annotation preserves the previous inference behaviour.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; positive cases (typed/untyped params, block and expression bodies,
`void`) and negative cases (`bool` body returning `int`, `int` body returning
`bool`, `string` body returning `int`) all behave correctly.

## 2026-09-08 — Library layer reconciliation

Follow-up to the stdlib expansion, focused on making the three library layers
(compiler builtins, native catalog bindings, public facades) agree.

**One name per operation.** `String.regexMatches`, `regexFullMatches`,
`regexFindAll`, `regexReplace`, `regexFind` and `regexFindMatches` were aliases
bound to exactly the same callbacks as the `Text.*` spellings, and `Text` was
missing the last two. The `String.regex*` duplicates are removed, `Text` gained
`regexFind` and `regexFindMatches`, and the builtin `Regex` builder now
delegates to `Text.*`. `Text.*` was already the documented and used spelling;
`String.regex*` had no callers.

**Honest container return types.** `FileSystem.listDir` and
`Network.resolveAll` declared a bare `list` while always returning strings.
Both now declare `list<string>`, and the runtime pins the same element type, so
the static declaration and the storage contract cannot drift. Wrong-typed
writes are rejected at compile time, or at runtime when they arrive through an
`unknown`. The remaining element-agnostic returns (`Collection.newList`,
`newMap`, `newSet`, `Queue.newQueue`, `Stack.newStack`) are correctly untyped.

**Typed Queue and Stack.** `zl.util.Queue` and `zl.util.Stack` were empty
marker classes, leaving FIFO/LIFO as raw untyped lists while every other
collection was a typed generic. They are now real `Queue<T>` and `Stack<T>`
classes with the same conventions as `List`/`Map`/`Set`: element typing,
`length`/`isEmpty`/`isNotEmpty`, safe `dequeueOr`/`popOr`/`peekOr` fallbacks,
`enqueueAll`/`pushAll`, `clear`, `drain`, `items`, `contains` and `forEach`.
The native `Queue.*`/`Stack.*` functions remain the storage boundary and stay
callable directly; because qualified-namespace calls resolve against the native
table first, the methods are genuine delegations rather than self-recursion.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; regex, queue/stack, filesystem, text, time and serialization probes
re-verified. Native catalog and runtime bindings remain in exact
correspondence (242 each).

## 2026-09-08 — Typed runtime exceptions, stdlib expansion, generic-resolution fixes

### Runtime failure behaviour

Runtime failures raised inside the VM are now real ZL exception objects and are
catchable by type. A new `ZlRuntimeFault` carries the intended class from the
throw site; the interpreter loop converts it at the failing frame, preserving
the message and stack trace, and rethrows it as an ordinary thrown exception.
Plain `std::runtime_error` from VM internals becomes `RuntimeError` rather than
escaping uncatchably.

The exception hierarchy is now `Exception` → `RuntimeError` →
`{TypeError, IndexError, KeyError, ArithmeticError, StackOverflowError,
IOError, NativeError, RegexError}`. Arithmetic faults, index and key errors,
type-assertion failures, filesystem failures and the call-depth limit all raise
their specific class, and a native that chooses a class keeps it instead of
being flattened into `NativeError`.

### Compiler and type system

- Nested generic declarations such as `List<List<int>> x = ...` parse; the type
  lookahead splits fused `>>` and `>>>` closers.
- A generic body may name its own uninstantiated form (`Map<K,V>` inside
  `class Map<K,V>`) in parameters, returns and locals. `this` types as the
  self-parameterized form, and access control treats the template and that form
  as the same declaring class.
- Fixed a declaration-order bug where a generic instantiation appearing only in
  another class's signature (very often `List<string>` returned from an
  imported class) resolved to a key with no registered shape and appeared to
  have no methods. Self-reference deferral is now restricted to generic bodies,
  and all instantiations are rebuilt once every class shape exists.

### Standard library

Collections gained substantial query, search, bulk-edit and set-algebra
coverage: `List` added 25 operations including `find`, `findIndex`,
`lastIndexOf`, `distinct`, `takeWhile`, `dropWhile`, `removeWhere`,
`removeRange`, `sorted`, `binarySearch`, `minBy`, `maxBy`, `fold` and
`equalsList`; `Map` added `getOrPut`, `putAll`, `copy`, `keyOf`, `filterKeys`,
`removeWhere` and friends; `Set` added `symmetricDifference`, `retainAll`,
`isSupersetOf`, `isDisjointFrom`, `filter` and `copy`.

New native primitives back the systems-facing libraries: 17 filesystem
operations (path decomposition, metadata, directory create/remove, copy,
rename), process/environment access (`execStatus`, `setEnv`, `hasEnv`,
`envOr`, `platform`), string search and comparison (`lastIndexOf`,
`indexOfFrom`, `trimStart`, `trimEnd`, `compare`, `compareIgnoreCase`),
calendar and monotonic time (`fromParts`, `dayOfWeek`, `isLeapYear`,
`utcFormat`, `monotonicMillis`), hashing and encoding (`sha1`, `md5`,
`fnv1a64`, hex and base64 codecs), DNS (`resolveAll`, `hostname`,
`isValidIp`) and the `trace`/`debug`/`fatal`/`at` log levels.

The `zl.fs`, `zl.text`, `zl.time`, `zl.serialize`, `zl.net`, `zl.crypto` and
`zl.logging` facades were rewritten on top of those primitives. They add
composition, safe fallbacks and honest units — calendar-correct date
arithmetic, `List<string>`-returning text splitting, typed JSON field
extraction — without re-wrapping any native that qualified-name resolution
already provides. `Crypto` documents the real strength of each digest and
still invents no cryptography.

**Validation:** fresh build; 49/49 examples pass; `tests/type_boundaries.py`
passes; typed-exception, collection, filesystem, text, time, serialization,
crypto and network probes all verified against known-good values.

## 2026-09-04 — Phase 17: native runtime integration and optimization groundwork

Completed the next two Phase 17 objectives.

A process-local `NativeExportRegistry` now accepts generated export tables, rejects
malformed and duplicate registrations, resolves exports by name, and routes name-based
calls through the existing `NativeError` boundary. This provides the runtime
registration bridge required by generated native modules without embedding raw function
pointers in ZL values.

The compiler IR also gained a backend-neutral optimization pass. It removes unreachable
blocks and folds safe primitive integer/boolean constant expressions while leaving
ownership/borrow instructions untouched. MIR native emission runs this pass before
lowering, so optimized IR feeds both native output and future VM/backend consumers.

**Validation:** `zl-ir-tests`, `zl-native-runtime-registry-tests`, and
`zl-native-compiler-tests` pass. The emitted MIR-native constant-folding path is
covered, as is direct C++ compilation of the generated native source.

## 2026-09-04 — Phase 16 completion

The ownership-aware FFI foundation is complete: opaque handles, borrowed buffers and
struct views, callback lifetime contracts, native resource adapters, dynamic library
loading, and validated `@ffi(symbol)` / `@ffi(library, symbol)` metadata are implemented
and covered by focused tests.

Typed field-by-field C struct schemas remain a later ABI extension.

## Phase 17 — binding safety

`zl-bind` class bindings use smart-pointer-backed native ownership and translate C++
exceptions into deterministic ZL runtime errors. Shared bindings can retain an opaque
handle without exposing C++ pointer types to ZL.

## Phase 16 — native boundary

Established the permanent design rule that C++ is the small, stable native foundation
while ZL owns high-level library policy and composition, together with the current
native inventory and the benchmark gate.

## Phase 15 — standard library expansion

Added user-facing packages for testing, logging, text processing, serialization, time,
and hashing. Public APIs live in ZL packages while low-level operations remain small
native primitives. See [stdlib.md](stdlib.md).

## Phase 11 — standard library and package ecosystem

Phase 11 is complete. The standard library includes reference queue/stack utilities,
text/time helpers, task/channel/thread facades, serialization, filesystem, and a small
portable DNS network facade. `zlpkg` supports version-checked path dependencies and
records resolved package versions in `zlpkg.lock`.

## 2026-08-30 — Phase 9 continuation: inherited static fields

Static fields now support inherited qualified access. A derived class resolves an
inherited static field to its declaring class, so reads and writes use the same backing
storage. Access modifiers are enforced against the declaring owner; protected inherited
access works from subclasses and private inherited access is rejected.
Interface-qualified static-field access is explicitly rejected because interfaces do not
own static field storage.

**Verification:** inherited public/protected access, shared storage, interface/class
coexistence, private inherited rejection, and interface-qualified rejection were
exercised. Existing lazy and re-entrant static behavior remains intact; the concurrent
static fixture passes under a 20-second direct run.

## 2026-08-30 — Phase 8: record methods

The Phase 8 record-method slice is implemented. `data` declarations support public
instance `func` methods, record fields are immutable after construction, and record
methods use the ordinary dispatch and reflection infrastructure. Focused valid and
invalid regressions cover the slice.

## Phase 10 — native compilation

Selected functions can opt into the restricted native compiler with `@native`, with the
VM path preserved as a fallback and MIR-native portable C++ emission via
`zl --emit-native`. See [native.md](native.md).
