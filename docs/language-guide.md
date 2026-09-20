# ZL Language Guide

Reference for the language surface. For the standard library see
[stdlib.md](stdlib.md); for imports and dependencies see [packages.md](packages.md).

- [File and class rule](#file-and-class-rule)
- [Values and declarations](#values-and-declarations)
- [String methods](#string-methods)
- [Lambda expressions](#lambda-expressions)
- [Generic collections](#generic-collections)
- [Static methods](#static-methods)
- [Operator overloading](#operator-overloading)
- [Record values (`data`)](#record-values-data)
- [`Option<T>` and `Result<T,E>`](#optiont-and-resultte)
- [Reflection](#reflection)
- [Memory-model direction](#memory-model-direction)

## File and class rule

Each `.zl` file must declare one *primary type* whose name matches the file stem exactly:

```text
TestProgram.zl   -> class TestProgram
type_mismatch.zl -> class type_mismatch
Point.zl         -> data Point
Shape.zl         -> interface Shape
```

A `class`, `data`, `interface`, `enum`, or `memory` declaration satisfies the rule, and other
declarations may share the file with the primary type - they simply are not importable
by their own name, since an import resolves to a path. The reason for the rule, and
what it costs, is written up in
[docs/packages.md](packages.md#one-primary-type-per-file).

This is enforced by the compiler pipeline. Errors are reported in three categories:
`syntax error`, `compile error`, and `runtime error`.

## Values and declarations

- `var` (rebindable) and `let` (single assignment) declarations, with inference or
  explicit types
- Primitives: `int`, `double`, `decimal`, `bool`, `string`
- Union-style values such as `int|string`
- Arrays, lists, maps, and sets via the collection helpers
- Arithmetic, comparison, logical, bitwise, and shift operators
- `if`, `elif`, `else if`, `else`, `for`, `while`, `repeat`, `break`, `continue`
- `func` declarations, methods, constructors, and return values
- `class` and `data` declarations, plus `enum`
- Access modifiers: `public`, `private`, `protected`

**An explicit type comes BEFORE the name, Java-style:** `Animal a = new Dog(...)`,
`int total = 0`. The annotation-after-colon spelling `var x: int` is a syntax error.

**Methods default to `private`.** Omitting a modifier on a method makes it private to
its class, so cross-class calls need an explicit `public` (or `protected` for
subclasses). Static *fields* without a modifier are likewise private.

**Static methods are not inherited-qualified** (unlike static fields, which are —
see below). `Derived.helper()` fails when `helper` is declared on `Base`; call it as
`Base.helper()`.

`enum` members are accessed as `EnumName.MEMBER` and are genuinely statically typed —
a variable or parameter typed as the enum rejects a raw string at compile time.

**Map ordering is a guarantee, not an accident.** `map` is insertion-ordered by
construction: it is a vector-backed association list, not a hash table. New keys append
at the end, re-setting an existing key updates it in place, and removing a key does not
reorder the rest. Lookups by **string key are O(1) expected** (a lazily built hash
index over the vector); lookups by any other key type — `int`, objects, mixed unions —
scan linearly, so building an n-entry non-string-keyed map costs O(n²). The typed
`Map<K,V>` is the same storage with a type-safe face, so prefer string keys at scale
and treat large non-string-keyed maps as small collections.

**`decimal` is binary64, not base-10.** It is a synonym of `double`: 64-bit IEEE-754
floating point, with all the usual binary fraction rounding — `0.1 + 0.2 != 0.3`.
There is no decimal-fixed type yet.

**A `double` prints as the shortest decimal that reads back as the same value.**
`log(3.14)` prints `3.14`, and `log(0.1 + 0.2)` prints `0.30000000000000004` — that
last digit is really there, so nothing is rounded away for appearance.
`String.toFloat` of any printed form returns the identical double, and
`Serialize.encode` uses the same spelling, so a value never has two
representations. Two consequences to expect in output: a whole-valued double
prints without a fraction (`log(1.0)` is `1`, `log(-0.0)` is `-0`), and a
magnitude outside the plain-decimal range prints in scientific form (`1e+21`,
`1e-07`). There is no printf-style precision specifier yet: round with
`Math.round` and align with `Text.padLeft` when a column of numbers has to line
up.

**`string` has no ordering operator.** `<`, `<=`, `>`, `>=` do not work on strings;
only `==` and `!=` do. (Comparison fails at runtime inside `sort` comparators, not at
compile time — compare with an explicit key or `String.compare` instead.)

**Integer arithmetic is checked, except bit operations.** `+`, `-`, `*`, `/`, `%`,
and unary `-` on `int` raise a runtime error on overflow, and `/`/`%` also reject a
zero divisor (including `INT64_MIN / -1`). Division by zero is an error even for
`double`: the language has no infinities or NaN. `double` arithmetic is checked
the same way — `+`, `-`, `*`, `/`, and `^^` raise on overflow, and `^^` and
`Math.pow` also reject a negative base with a fractional exponent, while
`Math.sqrt` rejects a negative argument (underflow still rounds to zero). Bitwise
`&`, `|`, `^`, `~` and the shifts `<<`, `>>`, `>>>` instead wrap modulo 2^64
(two's complement): `>>` is arithmetic (sign-extending), `>>>` is logical
(zero-filling), and a shift count outside `0..63` is a runtime error.

**Dynamic call results are not directly callable.** `f(x)()` — invoking whatever a
call returned — is rejected. Bind the result first: `var g = f(x)` then `g()`.

**Static fields** support inherited qualified access. A derived class resolves an
inherited static field to its declaring class, so reads and writes share the same
backing storage. Access modifiers are enforced against the declaring owner: protected
inherited access works from subclasses, private inherited access is rejected, and
interface-qualified static-field access is rejected because interfaces own no static
storage.

## String methods

A `string` has methods, and every one of them is an existing `String.*` native
primitive: the receiver binds the native's first parameter and the call's arguments
bind the rest, so `s.length()` and `String.length(s)` are the same call and produce
the same bytecode. There is no class behind `string` and no second implementation
behind the method spelling — the compiler resolves the method to the catalog entry
(`src/compiler/type_checker.cpp`, table `kStringMethods`), and both backends emit that
native call.

```zl
var s = "Hello, World"
log(s.length())                     // 12      (bytes, like String.length)
log(s.upper())                      // HELLO, WORLD
log(s.startsWith("Hello"))          // true
log(s.substring(0, 5))              // Hello
log(s.replace("World", "ZL"))       // Hello, ZL
log("42".toInt() + 1)               // 43
log(Collection.get(s.split(", "), 1))   // World
```

The surface, in the order the "no such method" diagnostic lists it:

| Method | Native | Notes |
| --- | --- | --- |
| `length()` | `String.length` | bytes, not characters |
| `charAt(i)`, `substring(start, end)`, `codePointAt(i)` | `String.charAt`, `String.substring`, `String.codePointAt` | byte indexing |
| `upper()`, `lower()`, `trim()`, `trimStart()`, `trimEnd()` | `String.upper`, `String.lower`, `String.trim`, `String.trimStart`, `String.trimEnd` | `upper`/`lower` case ASCII only |
| `contains(s)`, `startsWith(p)`, `endsWith(s)`, `indexOf(s)`, `lastIndexOf(s)`, `indexOfFrom(s, i)`, `compare(o)`, `compareIgnoreCase(o)` | `String.contains`, `String.startsWith`, `String.endsWith`, `String.indexOf`, `String.lastIndexOf`, `String.indexOfFrom`, `String.compare`, `String.compareIgnoreCase` | byte positions; a miss is `-1` |
| `replace(from, to)`, `split(sep)`, `repeatText(n)` | `String.replace`, `String.split`, `String.repeatText` | `split` returns the native `list<string>` storage, exactly as `String.split` does — read it with `Collection.*`, or use `Text.split` for a `List<string>` |
| `utf8Length()`, `utf8CharAt(i)`, `utf8Substring(start, end)`, `utf8Reverse()`, `utf8CodePointAt(i)`, `utf8ByteIndex(i)`, `utf8IndexFromByte(i)` | the matching `String.utf8*` natives | scalar indexing, the boundary `Text.*` wraps |
| `toInt()`, `toFloat()` | `String.toInt`, `String.toFloat` | throw on unparsable input; `Text.toIntOr` and friends are the total forms |

Indexing follows the primitive: `"héllo".length()` is `6` bytes, `"héllo".utf8Length()`
is `5` scalars, and `"héllo".length()` and `String.length("héllo")` never disagree — see
[stdlib.md](stdlib.md#zltext) for where `String.*` ends and `Text.*` begins.

A name outside the surface is a compile-time type error that lists the surface:

```text
type error: type 'string' has no method 'trimLeft' (string methods: charAt, ...)
```

Arity and argument types are checked against the catalog entry, so `s.length(1)` and
`s.contains(3)` are compile errors rather than native contract throws at run time.
`repeat` is a loop keyword, which is why the repeat method keeps the native spelling
`repeatText` (the same reason `Text.repeatText` exists).

An **untyped** lambda parameter (`func(name) => name.length()`) is `UNKNOWN` until a
type is written, so a string method on it is still rejected — annotate the parameter
(`func(string name) => name.length()`).

## Lambda expressions

An anonymous function *value* — assignable to a variable, passable as an argument, or
returnable from a function:

```zl
var square = func(x) => x * x         // expression body - `return` is implied
log(square(5))                        // 25

var greet = func(name) {              // block body - explicit `return` required
    log("Hi " + name)
}
greet("Zeal")

func applyTwice(func f, int x): int { // `func` as a parameter/return type
    return f(f(x))
}
log(applyTwice(func(x) => x * 2, 3))  // 12
```

**Syntax.** `func` — the same keyword as a named declaration, reused in expression
position — followed by a parameter list, an optional `: ReturnType`, then either
`=> expr` (single expression, return implied) or `{ statements }` (block body, explicit
`return` required; falling off the end returns `nil`). Parameters may be untyped (`x`)
or typed (`int x`), using the same `Type Name` order as named functions. `func` alone,
with no parameter list, is also a valid *type* name for a slot holding a function value:
`callback: func`.

**Declared return types.** A lambda may state its result type just like a named
function. The annotation is a checked contract, not documentation — the body's inferred
result must be assignable to it:

```zl
var double = func(int x): int => x * 2          // ok
var isBig  = func(int x): bool => x > 10        // ok
var report = func(int x): void { log(x) }       // ok - no value returned

var wrong  = func(int x): bool => x + 1         // error: body returns 'int'
```

When the annotation is present it overrides whatever the call site expected, so the
lambda's contract stays visible to callers. Omitting it keeps the previous behaviour:
the result type is inferred from the body, and from the expected type at the call site.

**Capture is by value.** A lambda snapshots every variable in scope at the moment it is
*evaluated* (not declared) — a copy, not a live reference:

```zl
var n = 10
var readN = func() => n
n = 999
log(readN())   // 10 - NOT 999
```

Two lambdas created from the same scope, for example on different loop iterations, get
independent snapshots; one closure's execution never affects another's. Within a single
closure, mutating one of its own captured variables persists across repeated calls to
*that same* closure, since a closure privately owns its snapshot from creation — the
classic counter pattern:

```zl
func makeCounter(): func {
    var count = 0
    var inc = func() { count = count + 1
        return count }
    return inc
}
var c = makeCounter()
log(c())  // 1
log(c())  // 2
```

**Function-type signatures.** A `func`-typed slot may carry the whole signature —
`func(int, string): bool` — as a parameter type, a return type, or the type of a
declaration. The signature is a checked contract, not a comment: the callable handed
to it is checked where it is handed over, and a call through the slot is checked
against the argument list.

```zl
func predicate(func(int, string): bool f, int n, string s): bool { return f(n, s) }

predicate(func(int n, string s): bool => n > 0, 3, "abc")   // ok

var tooFew = func(int n): bool => n > 0
predicate(tooFew, 3, "abc")
// error: argument 1 to func 'predicate': expected a func with 2 parameter(s), got 1

func(int, string): bool same = func(int n, string s): bool => n == s.length()
same(3)
// error: func value 'same' expects 2 argument(s), got 1
```

Parameter types and the return type are checked the same way (`incompatible func
parameter type`, `incompatible func return type`), and a `func(...): T` return type
keeps its signature out of the call, so a factory that hands back a callable produces
a value the next call site can check. An `unknown` on either side — an untyped lambda
parameter, say — is accepted rather than guessed at.

Bare `func` is the compatibility spelling and stays dynamic: it carries no signature,
so there is nothing to check statically, and calling it with the wrong number of
arguments is still a runtime error (`VM: function '…' argument count mismatch`). Write
the signature when the callee is going to call the value; leave it bare only where the
signature genuinely is not known.

## Union types and narrowing

A union is a set of alternatives, not the dynamic `unknown` type. Every
alternative must fit a typed assignment, argument or return. Use `match` to
refine it before an operation that only accepts one member:

```zl
static func describe(int|string value): string {
    return match value {
        int _ => "number " + (value + 1)
        string text => String.upper(text)
        null => "missing"
    }
}
```

A type arm refines both its binding and the original subject identifier while
that identifier remains unchanged. The catch-all binding carries the remaining
alternatives. Guards do not establish exhaustive coverage. ZL reference types,
including `string`, remain nullable: cover `null` or use a wildcard as well.
The two built-in sums are the exception - their cases are the subject's whole
value set, so matching every case is exhaustive without a `null` arm; see
[`Option<T>` and `Result<T,E>`](#optiont-and-resultte). Enums and primitive
numbers/bools are not nullable.

The subject is evaluated once. If a guard reassigns it, subsequent patterns still
inspect the original snapshot; use the pattern binding to read that snapshot.
Reassignment invalidates read refinements, and does not change the variable's
original declaration or constness. Mutable closure captures and loop back edges
cannot keep a refinement that their writes may invalidate.

Locals, loop counters, catch variables and pattern bindings have distinct lexical
storage, even when they reuse a spelling. Closures capture the selected bindings
by value. Typed reassignment is checked before storing a dynamic value, so a
rejected write leaves the old slot intact (RHS side effects are not rolled back).
The same expected-type handling supports reassigned lambdas and typed literals.

The focused regression can be run with:

```sh
python3 tests/type_boundaries.py /path/to/zl
```

## Heap write contracts

Native `list<T>`, `map<K,V>`, `set<T>` and `array[N]<T>` values carry contracts
on their shared storage, not just on the variable that first names them. Typed
initialization (including inferred locals), calls, returns, fields and successful
type patterns establish those contracts. An erased alias cannot bypass them:

```zl
list<int> numbers = [1, 2]
unknown alias = numbers
Collection.push(alias, "wrong")   // runtime error; numbers is unchanged
```

Nested containers are checked and constrained as one transaction. If a type
check fails, it does not leave partially constrained children behind. New children
inserted later acquire the required contract too. Containers are invariant:
`list<int>` cannot become `list<double>` through an erased alias, while an `int`
can still be stored in a fresh `list<double>`. Bare/`unknown` views do not erase an
existing contract. A native container's first successful typed view establishes
its storage contract; use a fresh container when a different contract is needed.
Fixed-array aliases cannot grow or shrink the array through list, queue, stack
or set operations. These checks do not replace the explicit synchronization
required for shared mutable program state.

Instance/data/static field writes enforce their complete declarations, including
inherited generic parameters. Class and data fields cannot redeclare an inherited
name: the object layout has one storage slot per field name.

Native return inference also preserves element types: `String.split` returns
`list<string>`, `Collection.get/pop` return the source element type, and map
keys/values and set copies preserve the relevant type argument. The rules live in
the native signature catalog rather than in library-specific compiler branches.

## Generic collections

`List<T>`, `Map<K,V>`, and `Set<T>` are real generic classes, not native tags.
`push`/`pop`/`get`/`put`/`length` and `add`/`has`/`remove` are genuinely compiled
methods, so a wrong element, key, or value type is a compile-time error:

```zl
var nums = new List<int>()
nums.push(1)
nums.push(2)
nums.push("oops")   // compile error: no overload of 'push' matches (int), got (string)

int first = nums.get(0)    // get()'s return type narrows to int for THIS instantiation
nums.put(0, 99)            // index-assignment - see the naming note below

var ages = new Map<string, int>()
ages.put("ada", 36)
log(ages.get("ada"))       // 36
log(ages.has("grace"))     // false
ages.remove("ada")

var tags = new Set<string>()
tags.add("x")
tags.add("x")              // no-op, sets don't duplicate
log(tags.length())         // 1
```

These are always available; no `import` is needed. They are backed by the same native
list/map/set storage that the lowercase `list<T>`/`map<K,V>`/`set<T>` type annotations
use — only the compile-time type surface is new, layered on the same generic-class
machinery that user-defined generics (`class Box<T>`) already use. A mismatched type is
caught by ordinary overload resolution, with the same error-message quality as any other
method call.

Printing a collection prints its data, not its wrapper: `log(nums)` on the list above
prints `[1, 2]`, a `Map` prints `{"ada": 36}`, and strings inside a collection are quoted
(`["a", "b"]`). The same spelling comes out of concatenation (`"" + nums`) and
`Serialize.encode`, so a value has one form through every door.

### Generic methods

A method may declare its own type parameters, independently of any class-level ones:

```zl
class Helpers {
    public static func firstOf<T>(List<T> items, T fallback): T {
        if (items.length() == 0) { return fallback }
        return items.get(0)
    }
}

var nums = new List<int>()
nums.push(41)
int first = Helpers.firstOf<int>(nums, 0)   // 41
```

Type arguments at the call site are required — there is no inference from the
arguments. A method type parameter may not reuse a name already bound as a class
type parameter (`class Box<T> { func identity<T>(...) }` is a compile error).

**Naming note.** The index/key-assignment method is `put`, not `set`, because `set` is a
type spelling (`set<T>`) and a *declared member name* is still an identifier position.
`list`, `set` and `map` may name a variable, field, parameter, loop variable, catch
variable or lambda parameter - see [Naming and the collection keywords](#naming-and-the-collection-keywords) -
but a method or function *name* may not be one of them, so `set` stays a
keyword-collision workaround rather than a design preference.

### Algorithms

`List<T>` owns the high-level collection algorithms in ZL while C++ provides only the
storage primitives:

```zl
var values = new List<int>()
values.push(4)
values.push(1)
values.push(3)

var doubled = values.transform(func(x) => x * 2)
var total = values.reduce(func(a, b) => a + b, 0)
values.sort(func(a, b) => a < b)

log(values.contains(2))
var evens = values.filter(func(x) => x % 2 == 0)
log(values.any(func(x) => x > 3))
```

Available: `contains`, `indexOf`, `count`, `any`, `all`, `filter`, `forEach`,
`transform`, `reduce`, `sort`, and `reversed`. These are implemented in ZL against the
existing `Collection.*` native storage boundary rather than as algorithm-specific C++.

`count(item)` is the odd one out: unlike `any`/`all`/`filter` it takes a value, not a
predicate, and returns how many times that value occurs.

### Typed collection literals

The expected declared type participates in literal checking and construction. Lists
support `[...]`; map and set literals use `{...}`:

```zl
list<int> nums = [1, 2, 3]
map<string, int> ages = {"ada": 36, "alan": 41}
set<string> names = {"ada", "alan", "ada"}

List<int> genericNums = [4, 5, 6]
Map<string, int> genericAges = {"ada": 36}
Set<int> genericSet = [1, 2, 2]
```

Every element, key, and value is checked against the expected generic type. Typed
`List`/`Map`/`Set` literals construct normal generic ZL collection objects; lowercase
`list`/`map`/`set` annotations keep their native representation.

#### Empty literals

An empty literal holds nothing that could contradict a declaration, so the declaration
decides the container - including for maps, which have no entries to spell:

```zl
map<string, int> ages = {}          // an empty native map
Map<string, int> genericAges = {}   // an empty generic Map
set<int> tags = {}
List<int> values = []
```

The literal still needs something to declare it, and in *argument* position that is the
parameter type: the call's own candidates say what the position wants, and an empty
literal — which holds nothing that could contradict them — is inferred against it.

```zl
static func countEntries(map<string, int> m): int { return Collection.length(m) }

log(countEntries({}))               // 0 - the parameter declares the literal

map<string, int> empty = {}         // the bound spelling is the same value
log(countEntries(empty))            // 0
```

Only an *empty* literal takes the expectation, and only where every arity-matching
candidate agrees on the container: with overloads for both `map<K,V>` and `list<T>`,
`{}` has no single answer and the call is reported as unmatched rather than guessed at.

With no declaration to go by, the spelling decides: `[]` infers an empty `list` and
`{}` an empty `set`, so `var tags = {}` followed by `set<int> declared = tags` type
checks (and `Collection.setAdd(tags, x)` twice holds one element).

A **non-empty** `{...}` literal with no declared type is still a `list`, not a set:
that spelling is also how a variadic argument list is written, where deduplication
would be wrong.

```zl
log(Text.format("{0} + {1} = {2}", {"1", "1", "2"}))   // 1 + 1 = 2, duplicates kept
```

## Static methods

Classes may declare `static func` members, called through the class name with no `this`
receiver:

```zl
class Helpers {
    public static func clamp(double x): double {
        if (x < 0.0) { return 0.0 }
        return x
    }
}

log(Helpers.clamp(-2.0))
```

Using `this` from a static function is a compile-time error. The standard library uses
static methods to move high-level policy into ZL without replacing the small native
runtime primitives underneath.

## Operator overloading

Classes define operators with the `operator` declaration. Operators use normal method
dispatch and overload resolution, and are public by default but may be marked `private`
or `protected`:

```zl
class Number {
    public int value

    func Number(int value) {
        this.value = value
    }

    operator +(Number other): Number {
        return new Number(this.value + other.value)
    }

    operator -(): Number {
        return new Number(-this.value)
    }
}
```

Arithmetic, comparison, bitwise, shift, and unary `+`/`-`/`!`/`~` declarations are
supported. Built-in operators remain the fallback for primitives; object operands use a
declared operator when one exists. Overloads are class-aware, so different object
parameter types coexist, while generic operator parameters keep shared runtime dispatch
with concrete compile-time checking. Numeric mixed-type overloads, operator visibility,
inheritance, and interface operator contracts are supported. Unsupported or ambiguous
object operators are compile-time errors.

## Annotations

The compiler knows five annotation names, and each one targets a declaration kind:

| Annotation | Allowed on | Meaning |
| --- | --- | --- |
| `@Deprecated` | classes, funcs (constructors included) | uses warn on stderr unless suppressed |
| `@SuppressWarnings(deprecation)` | classes, funcs | silence the deprecation warnings underneath |
| `@Override` | methods (not constructors) | checked against the parent class / implemented interfaces: a mismatch is an error, an unannotated override is a warning |
| `@ffi(library, symbol)` / `@ffi(symbol)` | funcs | the body is a foreign-symbol binding, not ZL |
| `@native` | funcs | the body is looked up in the native catalog instead of compiled |

Anything else is an error, not a silent no-op: an unknown name is a parse error at the
`@` itself, and a known name on a target it does not annotate (say `@Override` on a
class or a constructor) is a compile error naming the annotations that are allowed
there. The registry is
`include/zl/parser/annotation_rules.hpp` — names are checked in the parser, targets in
the type checker — and a new annotation means a row there, a consumer, and a fixture.
Annotations on fields are rejected.

## Memory declarations (`memory`)

A `memory` declaration writes a storage-domain *policy* — a contract the compiler, not
user code, calls:

```zl
memory FrameArena {
    private int resets = 0

    public func acquire(MemoryShape shape): Option<MemorySlot>   // required
    public func release(MemorySlot slot): void                   // required
    public func reset(): void                                    // optional
    public func exhausted(MemoryShape shape): void               // optional
    public func onCollect(MemoryStats stats): void               // optional
}
```

The two required methods and the three optional ones are validated exactly — present,
public, with those signatures — at compile time; the optional ones may be absent, but a
mis-spelled one is an error, not a default. State (fields) and helpers must be private:
the public surface of a domain is exactly its contract. A domain cannot be constructed
(`new FrameArena()` refuses until allocation with a domain lands), cannot be extended,
and declares no constructor or static member. Nothing consumes a declaration yet, so
none of it reaches the bytecode — the contract is checked now so the runtime that
honours it later arrives against fixtures, not against hope. The plan it belongs to,
and what each later phase adds, is
[memory-domains.md](memory-domains.md).

## Record values (`data`)

A `data` declaration defines a named record with typed public fields:

```zl
data Point {
    x: int
    y: int
}

var a = Point { x: 10, y: 20 }
var b = Point { x: 10, y: 20 }

log(a == b) // true
```

Record literals must provide every declared field exactly once; unknown, missing, or
duplicate fields are compile-time errors. Records of the same `data` type compare
structurally, including nested records, with numeric fields using normal ZL numeric
equality.

Records are value-oriented and immutable after construction. They may declare public
instance `func` methods — including `toString()` — which use ordinary dispatch and
reflection infrastructure, but they may not reassign their own fields. Record
inheritance and destructuring remain roadmap work.

## `Option<T>` and `Result<T,E>`

Two built-in generic sum types, available with no `import`. They model absence and
failure as values rather than as exceptions you might forget to handle.

```zl
var present = new Some<int>(42)
var absent  = new None<int>()

present.isSome()                              // true
present.unwrap()                              // 42 - throws on None
absent.unwrapOr(-1)                           // -1
absent.unwrapOrElse(func() => expensive())    // lazy fallback
present.isSomeAnd(func(v) => v > 40)          // true
```

`Result<T,E>` is the two-parameter sibling: a success value of type `T`, or an error of
type `E`, for when the failure carries information the caller needs.

```zl
var ok  = new Ok<int, string>(42)
var err = new Err<int, string>("division by zero")

ok.unwrap()          // 42
err.unwrapErr()      // "division by zero"
err.unwrapOr(-1)     // -1
err.isErrAnd(func(e) => e == "division by zero")
```

Full member list: `Option` has `isSome`, `isNone`, `unwrap`, `unwrapOr`, `unwrapOrElse`,
`expect`, `contains`, `isSomeAnd`. `Result` adds `isOk`, `isErr`, `unwrapErr`,
`unwrapErrOr`, and `isOkAnd`/`isErrAnd`.

A function may declare either as its return type or a parameter type, and a
subclass instantiation widens to its parameterized parent - `Some<int>` satisfies
`Option<int>`, while `Some<string>` does not:

```zl
static func find(int n): Option<int> {
    if (n > 0) { return new Some<int>(n) }
    return new None<int>()
}

static func label(Option<int> o): string {
    return match o { Some v => "found " + v.value  None => "empty" }
}
```

### Matching a sum

A case pattern names the case and binds the case object - `Some v` binds a
`Some<T>` whose payload is `v.value`, `Err e` binds an `Err<T,E>` whose error is
`e.error`. The arguments come from the subject, so the case name alone is
enough; spelling them out (`Ok<int,string> v`) is the same program:

```zl
static func report(Result<int, string> r): string {
    return match r {
        Ok v when v.value > 0 => "positive"
        Ok v => "ok " + v.value
        Err e => "err " + e.error
    }
}
```

The two cases are the whole value set, so covering both is exhaustive with no
wildcard and no `null` arm, and a missing case is a compile error naming it
(`non-exhaustive match: uncovered Err<int,string>`). Inside an arm the subject
identifier is refined to the matched case, so `v.value` type-checks.

Two consequences are worth knowing:

- **`None` and `Err` name their case in pattern position**, they do not bind a
  variable called `None` or `Err`; a binding is written as `Some name`,
  `Ok name`. A variable really named `None` is still readable in expression
  position - only a pattern reads it as the case.
- **Nil is not a case.** A `null` subject matches no case and reaches the
  lowering's unmatched path, which raises a catchable
  `Exception("non-exhaustive match")`. A `null` arm (or a wildcard) is allowed
  and is reachable even after every case is covered, but it is never required.

The case set is closed only while it is exactly the declared cases. A program
that extends a sum (`class Mine extends Result<int,string>`) reopens it, and
then a `Mine` value is neither case, so the plain pair is reported as
non-exhaustive again - see `tests/zl/invalid/type_errors/OpenSumMatch.zl`.

See `examples/advanced/OptionType.zl` and `examples/advanced/ResultType.zl`.

### Naming and the collection keywords

`list`, `set` and `map` are type spellings, and the lexer has no context, so
they are read as names wherever no type can appear:

```zl
var list = [1, 2]              // a variable called list
var map = 3
Collection.push(list, 4)       // ... used as an argument
list = [5, 6]                  // ... and assigned
for set in 0..3 { }            // a loop variable
try { ... } catch Exception list { }   // a catch variable
var f = func(map) => map + 1   // a lambda parameter

list<int> order = [1, 2]       // and the type readings are untouched
map<string, int> counts = {}
set<int> unique = {}
```

Statement position resolves the one real ambiguity by lookahead: a statement is
a typed declaration only when a complete type annotation is followed by a name,
so `list<int> xs = []` declares while `list = []`, `list.push(1)` and
`list.length()` use the variable. The same rule applies to a name in a typed
declaration - `int list = 3` declares an int called `list`. A statement whose
token sequence spells both readings (`list < n > x`) is read as the declaration,
exactly as it already is for a user-defined generic type. Function and method
*names* remain identifiers, which is why the collection API still spells
`put` rather than `set`.

## Reflection

A minimal `Type` API for runtime inspection:

```zl
Type.name(value)       // runtime type/class name
Type.base(object)      // direct base class name, or nil for a root class
```

`Type.fields(object)` and `Type.methods(object)` return the *effective* members,
including inherited ones — but not as plain strings: each element is a `Field` or
`Method` object. Both return a native `list`, so walk them with the
`Collection.*` primitives and name the element type before calling methods on it:

```zl
list<Field> fields = t.fields()
for i in 0..Collection.length(fields) {
    Field f = Collection.get(fields, i)
    log("field " + f.name() + ": " + f.type() + " (" + f.access() + ")")
}

list<Method> methods = t.methods()
for i in 0..Collection.length(methods) {
    Method m = Collection.get(methods, i)
    log("method " + m.name() + " -> " + m.returnType())
}
```

`Field` exposes `name()`, `type()`, and `access()`; `Method` exposes `name()`,
`returnType()`, `access()`, `isStatic()`, `isAsync()`, and `parameters()`. The
instance form `Type.of(value)` returns the `Type` object itself (`name()`,
`kind()`, `isData()`, `fields()`, `methods()`). String-comparing an element is a
mistake — compare `f.name()` instead.

Reflection metadata is intentionally small. Generic type arguments, annotations,
and writable reflection are reserved for future phases.

See `examples/intermediate/Reflection.zl` for a complete program.

## Runtime lifetimes and GC roots

Active executions, queued/suspended async invocations and reachable closures keep
their programs reachable. The collector follows program constants, current
static values and cached initialization failures at trace time. A parked VM does
not keep a stale copy of a static slot, and an unreachable static/closure cycle
is collectible rather than permanently pinned.

A thrown ZL exception retains its payload while C++ unwinds. A failure cached in
a Task or static field instead stores a traced edge and an owned diagnostic;
rethrowing it establishes a new in-flight root. Destructors do not read reclaimed
exception objects. Failures of an async `main` propagate to the entry point.

Collection separates tracing from destruction. Unreachable allocations leave the
registry while mutators are stopped, but their native owners are destroyed after
reactivation. Implicit `Thread` joins also wait at a stable VM instruction
boundary, not inside a vector/map update or frame unwind. Explicit `Thread.join`
remains synchronous. Values being returned or caught stay rooted while these
waits run. Pending native channel tasks retain their operation owner until a
terminal transition.

C++ embedders participate explicitly: `GCRoots` contains value snapshots and
borrowed program roots whose owners must outlive the root lease. `TracingGC::collect`
returns a move-only `Collection`; reclaim it only after the stop-the-world phase
has ended. The coordinator performs this ordering for VM collections.

The focused `gc_lifetime_tests.cpp` target checks program/static/closure cycles,
exception lifetimes, pending operation roots and blocking reclamation across
consecutive collections. `StaticMembers.zl` exercises the corresponding language
paths under allocation pressure.

## Memory-model direction

The planned memory model combines ownership with tracing GC. Unannotated managed and
reference values default to GC-managed, thread-confined semantics. `shared` is explicit
rather than the default.

Two pieces of that model exist today, and neither is a memory-management lever yet.
A typed declaration may name its storage contract in place of a bare type —
`owned Token a = new Token("alpha")`, `borrow Token view = b`, and the same modifier on
a parameter (`func consume(owned Token t)`), on reference-like types only. `move` transfers
an `owned` binding; the contract is checked in the type checker and again over the MIR
control-flow graph. What an `owned` binding gives up at `Drop` is its ability to *keep a
box alive* — the frame forgets it — and not its memory, which the collector returns on its
own schedule. The first surface of the plan that closes that gap also exists now: a
[`memory` declaration](#memory-declarations-memory) whose contract the compiler
validates (and nothing consumes yet). The direction itself, phase by phase with
measured baselines, is [memory-domains.md](memory-domains.md); the checked contract
itself is specified in [mir.md](mir.md#ownership).

Thread confinement is enforced at compile time: a closure passed to `Thread.start` or
`Task.spawn` may only capture values that are safe to carry across the boundary. The
accepted set is `Shared<T>` plus the runtime's own synchronisation primitives —
`Atomic`, `Mutex`, `RwLock`, `Semaphore`, `Channel` and `Condition` — since each of them
guards its state internally. Capturing anything else is a compile error:

```zl
var total = 0
Thread.start(func() { total += 1 })
// compile error: Thread.start cannot capture 'total' across a thread boundary;
//                use Atomic or Mutex for mutable shared state

var counter = new Atomic()
Thread.start(func() { Atomic.add(counter, 1) })   // fine
```

Wrapping a value in `Shared<T>` makes the capture legal, not automatically safe. Each
cell operation is individually synchronised, but a read-modify-write across two of them
is not: `get()` and `setValue()` can interleave with another thread's pair and lose the
update. Four threads doing 2000 unsynchronised increments of one `Shared<int>` land
around 3000 of the expected 8000
(`tests/zl/valid/concurrency_regressions/SharedLostUpdateMeasurement.zl` measures it on
every run, and fails if the number ever becomes exact — that would mean `Shared<T>`
became synchronised and these docs are stale).

Three ways to make the same increment correct:

```zl
import zl.lang.Atomic
import zl.lang.Mutex

var counter = new Shared<int>(0)     // or: var counter = share(0)

counter.withLock(func(): int {       // the cell's own lock
    var next = counter.get() + 1
    counter.setValue(next)
    return next
})

var atomic = new Atomic()            // an atomic slot, for counters
Atomic.add(atomic, 1)

var m = new Mutex()                  // a lock, for a larger critical section
Mutex.withLock(m, func() { counter.setValue(counter.get() + 1) })
```

All three land on exactly 8000 in that fixture. Use `withLock` when the state is the
cell's own, `Atomic` for a single slot, and `Mutex`/`RwLock` when several values have
to move together.
