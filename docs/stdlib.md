# ZL Standard Library

The standard library is deliberately hybrid: public APIs live in ZL packages, while the
VM, OS access, parsing engines, and storage primitives remain small native primitives.
See [native.md](native.md) for the boundary rule.

- [`zl.lang` (auto-imported)](#zllang-auto-imported)
- [Generic collections](#generic-collections)
- [Math](#math)
- [`zl.test`](#zltest)
- [`zl.logging`](#zllogging)
- [`zl.text`](#zltext)
- [Regular expressions](#regular-expressions)
- [`zl.serialize`](#zlserialize)
- [`zl.time`](#zltime)
- [`zl.crypto`](#zlcrypto)
- [Concurrency](#concurrency)

## `zl.lang` (auto-imported)

`zl.lang` is available everywhere with no `import` statement. It contains everything
that existed as a flat native before packages did: `Math`, `String`, `IO`, `Collection`,
`FileSystem`, `Time`, `System`, and `Int`/`Double`/`Bool` parsing.

Everything else requires an explicit `import` — see [packages.md](packages.md).

Beyond `zl.lang`, the bundled packages include typed queue, stack, and priority queue
collections (`zl.util.Queue`, `zl.util.Stack`, `zl.util.PriorityQueue`), text and time helpers,
task/channel/thread facades,
serialization, filesystem access, hashing/encoding, logging, and a small portable DNS
network facade.

## Generic collections

`List<T>`, `Map<K,V>`, `Set<T>`, `Queue<T>`, `Stack<T>` and `PriorityQueue<T>` are ZL
classes over native or list-backed storage primitives.
Their methods are compiled once; each invocation carries the receiver's concrete
type bindings. Typed literals and collections built by methods such as `transform`,
`filter`, `reversed`, `keys`, and `values` retain those bindings, including nested
instantiations and empty results.

```zl
List<int> numbers = [1, 2, 3]
List<int> doubled = numbers.transform(func(x) => x * 2)
Map<string,int> ages = {"ada": 36}
List<string> names = ages.keys()
```

`PriorityQueue<T>` is a comparator-driven binary heap. The comparator returns true
when its first argument should be nearer the top: `a < b` gives a min-heap and
`a > b` gives a max-heap. `enqueue` and `dequeue` are O(log n), `peek` is O(1),
and `heapify(List<T>)` builds a heap in O(n). Equal-priority values are not stable.

```zl
import zl.util.PriorityQueue

var next = new PriorityQueue<int>(func(int a, int b) => a < b)
next.enqueue(8)
next.enqueue(3)
log(next.peek()) // 3
```

The returned collections can be passed to typed parameters normally; there is no
need to keep them in untyped locals. Runtime argument, return, and explicit-local
checks also apply when values arrive through callbacks or reflection. Closures
retain their lexical type bindings after the creating method returns, including
across async suspension. A scalar `int` may still widen to `double`; different
mutable collection instantiations are not interchangeable.

The lowercase native `list`/`map` storage values are distinct from these generic
class instances. Their heap contracts also survive erased aliases and protect
nested writes; fixed-array views prevent resizing through native aliases.
`String.split` returns `list<string>`, and native element reads and key/value
projections retain their type arguments. See `examples/intermediate/GenericRuntimeChecks.zl`
and `tests/type_boundaries.py` for typed factories and rejected dynamic writes.

The generic wrappers declare their backing storage as `list<T>`, `map<K,V>` and
`set<T>`; backing primitives do not bypass the public element contract.

## Math

Constants are accessed without parentheses:

```zl
log(Math.PI)
log(Math.E)
log(Math.TAU)
```

Available operations:

- `sqrt`, `abs`, `pow`, `floor`, `ceil`, `round`, `min`, `max`
- `clamp`, `sign`, `cbrt`, `trunc`
- `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
- `log`, `log10`, `exp`
- `lerp`, `degrees`, `radians`
- `random`, `randomInt(min, max)`, `randomFloat(min, max)`

Domain errors are reported for `asin`/`acos` outside `[-1, 1]`, for `log`/`log10` on
non-positive inputs, for `sqrt` on a negative input, and for `pow` with a negative
base and a fractional exponent. `Math.exp` and `Math.pow` report finite-input
overflow. `Math.clamp`, `Math.randomInt`, and `Math.randomFloat` reject a minimum
greater than the maximum. The random functions use a process-local pseudorandom
generator and are **not** cryptographically secure.

High-level helpers (`clamp`, `sign`, `lerp`, `degrees`, `radians`) are ZL-owned static
methods; genuinely primitive operations (`sqrt`, `sin`, `cbrt`, random generation) stay
native. The public `Math.*` API is stable across that boundary.

## `zl.test`

```zl
import zl.test.Test
import zl.test.Runner

class ExampleTests {
    func main(): void {
        var tester = new Test()
        tester.assertThrows(func() { throw new Exception("expected") })

        var runner = new Runner("example")
        runner.run("truth", func() { Test.assertTrue(true) })
        runner.run("math", func() { Test.assertEqual(2 + 2, 4) })
        Test.assertEqual(runner.report(), 0)
    }
}
```

Assertions: `Test.assertTrue`, `Test.assertFalse`, `Test.assertEqual`,
`Test.assertNotEqual`, `Test.assertNear(actual, expected, epsilon)`, `Test.fail`,
and `Test.assertThrows`.

`Runner` provides ZL-native grouping, pass/fail accounting, and reporting.

## `zl.logging`

```zl
import zl.logging.Log

Log.info("server started")
Log.warn("cache miss")
Log.error("request failed")
```

The package is `zl.logging` rather than `zl.log` because `log` is a reserved keyword.

## `zl.text`

```zl
import zl.text.Text

Text.upper("zl")
Text.startsWith("hello", "he")
Text.endsWith("hello", "lo")
Text.join(new List<string>(), ",")

Text.format("hello {0}", {"ZL"})
Text.regexMatches("abc123", "[0-9]+")
Text.regexFindAll("a12 b34", "[0-9]+")
Text.regexReplace("a1b2", "[0-9]", "X")
```

Convenience and composition methods are ZL-owned. `String.*` remains the low-level
native string boundary; the regex and formatting engines stay native.

`Text.*` is the single spelling for the regex operations — `regexMatches`,
`regexFullMatches`, `regexFindAll`, `regexFindMatches`, `regexFind` and
`regexReplace`. Earlier releases also exposed four of these as `String.regex*`
aliases bound to the very same callbacks; those duplicates have been removed so
each operation has exactly one name.

`Text.*` character indexing (`length`, `charAt`, `substring`, `reverse`,
`codePointAt`, `fromCodePoint`, and search helpers that return offsets) is by
Unicode scalar value. `Text.length("héllo")` is `5`; `Text.charAt("héllo", 1)`
is `"é"`; `Text.reverse` keeps multi-byte scalars intact. `String.*` remains
the byte-string primitive boundary, so `String.length("héllo")` is `6`.
`upper` and `lower` only case ASCII. See `examples/REVIEW.md` (O18) and
`tests/zl/valid/language_hardening_tests/Utf8Text.zl`.

The one character-oriented `String.*` primitive is the empty-separator split:
`String.split(s, "")` means "every character", and a character is a scalar, so
`String.split("héllo", "")` returns five one-scalar pieces (`String.utf8CharAt`
walks the same boundaries). `Text.split(value, "")` forwards to it and returns
them as a `List<string>`. A non-empty separator is an ordinary byte-string
search, and an ill-formed byte still comes back on its own rather than hanging
the walk.

`string` also has methods, and they *are* the `String.*` primitives with the
receiver bound as the first argument: `s.length()` is `String.length(s)`,
`s.startsWith(p)` is `String.startsWith(s, p)`, and so on for the whole surface.
The method spelling therefore stays the byte boundary — `s.length()` counts bytes
and `s.substring` indexes bytes, while the `utf8*` methods are the scalar-indexed
ones listed above. `Text.*` remains the ZL-owned convenience layer on top (it is
where `toIntOr`, `padLeft`, `join`, the regex helpers and the `List<string>` return
of `split` live). See
[language-guide.md](language-guide.md#string-methods) for the full method table.

## Regular expressions

Two surfaces sit over the same native engine.

`Text.regex*` is the shortcut form, and `Regex` is the pattern object:

```zl
import zl.text.Text

Text.regexMatches("abc123", "[0-9]+")     // true - matches ANYWHERE
Text.regexFindAll("a12 b34", "[0-9]+")
Text.regexReplace("a1b2", "[0-9]", "X")

var digits = new Regex("[0-9]+")
digits.matches("12345")                   // true  - anchored, whole string
digits.matches("abc123")                  // false
var found = new Regex("([a-z]+)=([0-9]+)").find("width=800")
found.group(1)                            // "width"
```

Note the difference: `Text.regexMatches` searches, while `Regex.matches` is anchored.

### The fluent builder

`Regex` also builds patterns, so metacharacters do not have to be escaped by hand.
Every step returns the same `Regex`, and `source()` shows what has been assembled:

```zl
var pin = new Regex("")
pin.startOfLine().digit().exact(4).endOfLine()
pin.source()          // ^\d\d\d\d$
pin.matches("1234")   // true
```

Builders: `then` (literal, escaped), `raw`, `digit`, `wordCharacter`,
`unicodeProperty`, `any`, `startOfLine`, `endOfLine`, `alternate`, `named`,
`backreference`, `atomic`, and the lookahead/lookbehind forms.

Quantifiers rewrite the element added immediately before them: `exact(n)`,
`range(min, max)`, `oneOrMore`, `zeroOrMore`, `optional`, and the lazy and
possessive variants. On an empty builder they throw rather than silently produce a
broken pattern, so `.oneOrMore()` with nothing before it is an error.

`startOfLine`/`endOfLine` simply append their anchor, so they only make sense as the
first or last step in a chain.

The builder is available with no `import`. Worked examples:
`examples/advanced/RegexBasics.zl` and `examples/advanced/RegexBuilder.zl`.

## `zl.serialize`

```zl
import zl.serialize.Serialize

var json = Serialize.stringify({"name": "ZL", "version": 15})
var value = Serialize.parse(json)
var answer = Serialize.number(42)
```

`encode`/`decode` and the low-level typed conversion primitives remain available behind
the facade. The JSON parser and encoder are native, and support null, booleans, numbers,
strings, arrays/lists, and string-keyed objects/maps.

## `zl.time`

Provides `Date`, `DateTime`, `TimeOfDay`, and `Duration` value objects over the native
clock primitives. Higher-level operations — `Time.nowDateTime()`, `Time.today()`,
`Date.addDays()`, `DateTime.addHours()`, `DateTime.difference()` — are implemented in ZL.

`Time.format(timestamp, pattern)` renders a timestamp using ZL's own tokens, **not**
strftime:

| Token | Meaning |
| --- | --- |
| `YYYY` | 4-digit year |
| `MM` | 2-digit month |
| `DD` | 2-digit day |
| `HH` | 2-digit hour (24h) |
| `mm` | 2-digit minute |
| `ss` | 2-digit second |

```zl
Time.format(Time.now(), "YYYY-MM-DD HH:mm:ss")   // 2026-09-06 16:32:44
```

An unrecognised `%-`token (e.g. strftime `"%Y-%m-%d"`) now raises an error
instead of being returned unchanged, so a wrong date cannot be printed
confidently. Valid tokens are `YYYY`, `MM`, `DD`, `HH`, `mm`, `ss`.

## `zl.crypto`

```zl
import zl.crypto.Crypto

Crypto.sha256("abc")
Crypto.crc32("123456789")
```

SHA-256 and CRC32 are native primitives; ZL owns the public package API. Cryptographic
primitives are deliberately kept native for correctness and performance.

## Concurrency

The runtime supports native asynchronous operations that return `Task<T>` and complete
from worker threads. The reference operation is `Time.sleepAsync(milliseconds)`, which
can be awaited from an `async func` without blocking the VM scheduler.

`Shared<T>` is an explicit generic wrapper. Construct it with `new Shared<T>(value)` and
access it with `get()` / `setValue()`, or construct a cell with `share(value)`.
CPU-worker and raw-thread captures must use `Shared` or the supported synchronization
handles; ordinary mutable captures are rejected. Individual cell operations are
synchronized, but read-modify-write sequences need `withLock` or an atomic operation.

Explicit `Thread.join` blocks cooperatively. Dropping the last thread handle retains
its implicit-join behavior, with the native wait deferred to a stable VM boundary so
GC can continue. Cached task failures keep their managed exception as a traced edge;
an in-flight rethrow roots the payload during unwinding. Async entry-point failures
are reported rather than silently returning success.
