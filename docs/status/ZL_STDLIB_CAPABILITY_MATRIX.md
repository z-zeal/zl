# ZL Standard Library Capability Matrix

This matrix is generated from the repository's compiler builtin library, native catalog, VM native table, and `stdlib/` source tree.

> **S0 rule:** classify implemented capabilities from ZL source, not by analogy with other languages or by counting `.zl` files.

## Source invariants

- Native catalog entries: **258**
- VM native bindings: **258**
- Compiler embedded builtin classes: **35**
- Visible stdlib `.zl` files: **27**
- Native catalog IDs and VM binding IDs: **exact match**

## Ownership metadata finding

The native catalog type supports parameter and return ownership metadata, and the ABI/runtime enforces ownership tags at the native boundary. The audited native catalog entries do not populate explicit ownership fields, so this matrix records their catalog metadata as `NONE/default` rather than guessing runtime ownership semantics. Ownership-sensitive subsystems therefore require a separate semantic audit before APIs are labeled ownership-complete.

## Native capability domains

| Domain | Count | Source/facade | Status |
|---|---:|---|---|
| `Reflection` | 33 | compiler builtin: Reflection | implemented |
| `String` | 31 | stdlib: zl.text.Text + primitive String surface | implemented |
| `Math` | 23 | compiler builtin: Math | implemented |
| `FileSystem` | 22 | stdlib: zl.fs.FileSystem | implemented |
| `Collection` | 18 | compiler builtin: List/Map/Set | implemented |
| `Time` | 17 | stdlib: zl.time.* | implemented |
| `Type` | 10 | compiler builtin: Type | implemented |
| `Atomic` | 9 | stdlib: zl.lang.Atomic | implemented |
| `Crypto` | 9 | stdlib: zl.crypto.Crypto | implemented |
| `System` | 8 | compiler/runtime system boundary | implemented |
| `Log` | 7 | stdlib: zl.logging.Log | implemented |
| `Text` | 7 | stdlib: zl.text.Text | implemented |
| `Channel` | 6 | stdlib: zl.lang.Channel | implemented |
| `Queue` | 6 | stdlib: zl.util.Queue | implemented |
| `Semaphore` | 6 | stdlib: zl.lang.Semaphore | implemented |
| `Serialize` | 6 | stdlib: zl.serialize.Serialize | implemented |
| `Stack` | 6 | stdlib: zl.util.Stack | implemented |
| `Test` | 6 | stdlib: zl.test.Test | implemented |
| `Condition` | 4 | stdlib: zl.lang.Condition | implemented |
| `IO` | 4 | compiler/runtime IO boundary | implemented |
| `Network` | 4 | stdlib: zl.net.Network | implemented |
| `Shared` | 3 | compiler builtin: Shared | implemented |
| `Thread` | 3 | stdlib: zl.lang.Thread | implemented |
| `RwLock` | 2 | stdlib: zl.lang.RwLock | implemented |
| `Bool` | 1 | compiler/runtime primitive | implemented |
| `Double` | 1 | compiler/runtime primitive | implemented |
| `GC` | 1 | compiler/runtime native surface | implemented |
| `Hash` | 1 | compiler/runtime primitive | implemented |
| `Int` | 1 | compiler/runtime primitive | implemented |
| `Mutex` | 1 | stdlib: zl.lang.Mutex | implemented |
| `Task` | 1 | stdlib: zl.lang.Task | implemented |
| `share` | 1 | compiler/runtime native surface | implemented |

## Compiler embedded builtin classes

| Class | Layer |
|---|---|
| `List<T>` | compiler embedded builtin |
| `Map<K, V>` | compiler embedded builtin |
| `Set<T>` | compiler embedded builtin |
| `Option<T>` | compiler embedded builtin |
| `Some<T> extends Option<T>` | compiler embedded builtin |
| `None<T> extends Option<T>` | compiler embedded builtin |
| `Result<T, E>` | compiler embedded builtin |
| `Ok<T, E> extends Result<T, E>` | compiler embedded builtin |
| `Err<T, E> extends Result<T, E>` | compiler embedded builtin |
| `Shared<T>` | compiler embedded builtin |
| `Exception` | compiler embedded builtin |
| `CancellationException extends Exception` | compiler embedded builtin |
| `RuntimeError extends Exception` | compiler embedded builtin |
| `TypeError extends RuntimeError` | compiler embedded builtin |
| `IndexError extends RuntimeError` | compiler embedded builtin |
| `KeyError extends RuntimeError` | compiler embedded builtin |
| `ArithmeticError extends RuntimeError` | compiler embedded builtin |
| `StackOverflowError extends RuntimeError` | compiler embedded builtin |
| `IOError extends RuntimeError` | compiler embedded builtin |
| `NativeError extends RuntimeError` | compiler embedded builtin |
| `RegexError extends RuntimeError` | compiler embedded builtin |
| `ReflectionError extends Exception` | compiler embedded builtin |
| `InvalidArguments extends ReflectionError` | compiler embedded builtin |
| `AccessViolation extends ReflectionError` | compiler embedded builtin |
| `RegexMatch` | compiler embedded builtin |
| `Regex` | compiler embedded builtin |
| `Math` | compiler embedded builtin |
| `Type` | compiler embedded builtin |
| `Field` | compiler embedded builtin |
| `Method` | compiler embedded builtin |
| `Constructor` | compiler embedded builtin |
| `Function` | compiler embedded builtin |
| `MemoryShape` | compiler embedded builtin |
| `MemorySlot` | compiler embedded builtin |
| `MemoryStats` | compiler embedded builtin |

## Stdlib source modules

| Module | Classes | Methods | Imports |
|---|---|---|---|
| `stdlib/zl/crypto/Crypto.zl` | `Crypto` | `digest256, digest256Hex, hash32, hash64, toHex, fromHex, toBase64, fromBase64, fromHexOr, fromBase64Or, digestsEqual, verifySha256` | `—` |
| `stdlib/zl/fs/FileSystem.zl` | `FileSystem` | `readOr, sizeOr, modifiedTimeOr, removeIfExists, removeDirIfExists, ensureFile, ensureParentDir, readLines, writeLines, appendLine, entries, files, directories, isEmptyDir, withExtension, hasExtension` | `—` |
| `stdlib/zl/lang/Atomic.zl` | `Atomic` | `ofInt, ofBool, ofDouble, increment, decrement, reset, toggle` | `—` |
| `stdlib/zl/lang/Channel.zl` | `Channel` | `bounded, sendBlocking, receiveBlocking, count` | `—` |
| `stdlib/zl/lang/Condition.zl` | `Condition` | `create, waitUntil, waitUntilTimeout, signal, broadcast` | `—` |
| `stdlib/zl/lang/Mutex.zl` | `Mutex` | `create, guard` | `—` |
| `stdlib/zl/lang/RwLock.zl` | `RwLock` | `create, read, write` | `—` |
| `stdlib/zl/lang/Semaphore.zl` | `Semaphore` | `withPermits, withPermit, tryWithPermit, isExhausted` | `—` |
| `stdlib/zl/lang/Task.zl` | `Task` | `—` | `—` |
| `stdlib/zl/lang/Thread.zl` | `Thread` | `startThread, wait, running` | `—` |
| `stdlib/zl/logging/Log.zl` | `Log` | `infoLine, warnLine, errorLine, tagged, infoTagged, warnTagged, errorTagged, when, exception, fields` | `zl.serialize.Serialize` |
| `stdlib/zl/math/Vector2.zl` | `Vector2` | `Vector2, lengthSquared, length, normalize, dot, distanceSquared, distance` | `—` |
| `stdlib/zl/math/Vector3.zl` | `Vector3` | `Vector3, lengthSquared, length, normalize, dot, cross, distanceSquared, distance` | `—` |
| `stdlib/zl/net/Network.zl` | `Network` | `lookup, lookupOr, lookupAllOrEmpty, isResolvable, isIpv4, isIpv6, isLoopback, isPrivate` | `zl.text.Text` |
| `stdlib/zl/serialize/Serialize.zl` | `Serialize` | `stringify, parse, string, integer, number, boolean, stringOr, integerOr, numberOr, booleanOr, stringifyOr, parseOr, isValidJson, isString, isInt, isNumber, isBool, hasField, field, stringField, intField, numberField, boolField, lengthOr, element, stringList, intList, encodeStringList, encodeIntList, encodeStringMap` | `—` |
| `stdlib/zl/test/Runner.zl` | `Runner` | `Runner, run, report` | `—` |
| `stdlib/zl/test/Test.zl` | `Test` | `assertThrows` | `—` |
| `stdlib/zl/text/Text.zl` | `Text` | `length, isEmpty, isNotEmpty, upper, lower, trim, isBlank, repeatText, contains, indexOf, charAt, substring, replace, remove, reverse, toInt, toFloat, toBool, toIntOr, toFloatOr, toBoolOr, startsWith, endsWith, count, padLeft, padRight, join, split, lines, words, chars, trimStart, trimEnd, removePrefix, removeSuffix, ensurePrefix, ensureSuffix, lastIndexOf, indexOfFrom, indicesOf, containsAny, compare, compareIgnoreCase, equalsIgnoreCase, containsIgnoreCase, startsWithIgnoreCase, endsWithIgnoreCase, capitalize, title, slice, take, drop, padCenter, truncate, codePointAt, fromCodePoint, isDigits, isAlpha, formatAll` | `—` |
| `stdlib/zl/time/Calendar.zl` | `Calendar` | `civilToDays, fromDayNumber, dayNumber, addDays, addWeeks, addMonths, addYears, diffDays, startOfDay, endOfDay, isSameDay, daysInMonth, isWeekend, weekdayName, monthName, isoDate, isoTime, isoDateTime, isoUtc` | `—` |
| `stdlib/zl/time/Date.zl` | `Date` | `Date, of, today, year, month, day, dayOfWeek, dayOfYear, weekdayName, monthName, timestamp, dayNumber, format, iso, toString, addDays, subtractDays, addWeeks, subtractWeeks, addMonths, subtractMonths, addYears, subtractYears, startOfMonth, endOfMonth, startOfYear, endOfYear, daysUntil, difference, isWeekend, isWeekday, isLeapYear, isSameMonth, compare, isBefore, isAfter, equals, isBetween` | `zl.time.Calendar, zl.time.Duration` |
| `stdlib/zl/time/DateTime.zl` | `DateTime` | `DateTime, of, now, year, month, day, hour, minute, second, dayOfWeek, dayOfYear, weekdayName, monthName, timestamp, date, timeOfDay, format, iso, isoUtc, toString, addSeconds, addMinutes, addHours, subtractSeconds, subtractMinutes, subtractHours, addDuration, addDays, subtractDays, addWeeks, subtractWeeks, addMonths, subtractMonths, addYears, subtractYears, startOfDay, endOfDay, difference, secondsUntil, minutesUntil, hoursUntil, daysUntil, isWeekend, isSameDay, compare, isBefore, isAfter, equals, isBetween` | `zl.time.Calendar, zl.time.Date, zl.time.TimeOfDay, zl.time.Duration` |
| `stdlib/zl/time/Duration.zl` | `Duration` | `Duration, seconds, minutes, hours, milliseconds, add, subtract, negate, abs, isZero, isNegative, isPositive, compare` | `—` |
| `stdlib/zl/time/Time.zl` | `Time` | `nowDateTime, today, clock, dateTimeAt, dateAt, durationMillis, durationSeconds, durationMinutes, durationHours, durationDays, between, sleepMillis, sleepDuration, elapsedMillisSince, addSeconds, addMinutes, addHours, addDays, addWeeks, addMonths, addYears, dayNumber, diffSeconds, diffMinutes, diffHours, diffDays, compare, isBefore, isAfter, isBetween, min, max, daysInMonth, isWeekend, weekdayName, monthName, startOfDay, endOfDay, isSameDay, isoDate, isoTime, isoDateTime, isoUtc` | `zl.time.Calendar, zl.time.DateTime, zl.time.Date, zl.time.TimeOfDay, zl.time.Duration` |
| `stdlib/zl/time/TimeOfDay.zl` | `TimeOfDay` | `TimeOfDay, of, now, hour, minute, second, totalSeconds, totalMinutes, format, iso, short, toString, addSeconds, addMinutes, addHours, subtractSeconds, subtractMinutes, subtractHours, secondsUntil, difference, isMorning, isAfternoon, isEvening, compare, isBefore, isAfter, equals, isBetween` | `zl.time.Duration` |
| `stdlib/zl/util/PriorityQueue.zl` | `PriorityQueue` | `PriorityQueue, enqueue, enqueueAll, heapify, peek, dequeue, peekOr, dequeueOr, length, isEmpty, isNotEmpty, clear` | `—` |
| `stdlib/zl/util/Queue.zl` | `Queue` | `Queue, enqueue, dequeue, peek, length, isEmpty, isNotEmpty, dequeueOr, peekOr, enqueueAll, clear, drain, items, contains, forEach` | `—` |
| `stdlib/zl/util/Stack.zl` | `Stack` | `Stack, push, pop, peek, length, isEmpty, isNotEmpty, popOr, peekOr, pushAll, clear, drain, items, contains, forEach` | `—` |

## Native capability detail

| Native ID | Qualified name | Domain | Public surface | Status | Ownership metadata |
|---|---|---|---|---|---|
| `TYPE_NAME` | `Type.name` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_FIELDS` | `Type.fields` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_METHODS` | `Type.methods` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_BASE` | `Type.base` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_CALLABLE` | `Type.callable` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_KIND` | `Type.kind` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_IS_DATA` | `Type.isData` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_IS_ENUM` | `Type.isEnum` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_ENUM_MEMBERS` | `Type.enumMembers` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `TYPE_OF` | `Type.of` | `Type` | compiler builtin: Type | implemented | not explicitly declared in native catalog entry; default NONE |
| `SHARED_SHARE` | `share` | `share` | compiler/runtime native surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `SHARED_GET` | `Shared.__get` | `Shared` | compiler builtin: Shared | implemented | not explicitly declared in native catalog entry; default NONE |
| `SHARED_SET` | `Shared.__set` | `Shared` | compiler builtin: Shared | implemented | not explicitly declared in native catalog entry; default NONE |
| `SHARED_WITHLOCK` | `Shared.__withLock` | `Shared` | compiler builtin: Shared | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_NAME` | `Reflection.name` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_KIND` | `Reflection.kind` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_IS_DATA` | `Reflection.isData` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_IS_ENUM` | `Reflection.isEnum` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_ENUM_MEMBERS` | `Reflection.enumMembers` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FIELDS` | `Reflection.fields` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHODS` | `Reflection.methods` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_BASE` | `Reflection.base` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_INTERFACES` | `Reflection.interfaces` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_TYPE_PARAMETERS` | `Reflection.typeParameters` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_TYPE_CONSTRUCTORS` | `Reflection.constructors` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FIELD` | `Reflection.field` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD` | `Reflection.method` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_CONSTRUCTOR` | `Reflection.constructor` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_INVOKE` | `Reflection.methodInvoke` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_CONSTRUCTOR_INVOKE` | `Reflection.constructorInvoke` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FIELD_NAME` | `Reflection.fieldName` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FIELD_TYPE` | `Reflection.fieldType` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FIELD_ACCESS` | `Reflection.fieldAccess` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_NAME` | `Reflection.methodName` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_RETURN_TYPE` | `Reflection.methodReturnType` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_ACCESS` | `Reflection.methodAccess` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_STATIC` | `Reflection.methodIsStatic` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_ASYNC` | `Reflection.methodIsAsync` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_METHOD_PARAMETERS` | `Reflection.methodParameters` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_CONSTRUCTOR_PARAMETERS` | `Reflection.constructorParameters` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION` | `Reflection.function` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_NAME` | `Reflection.functionName` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_PARAMETERS` | `Reflection.functionParameters` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_RETURN_TYPE` | `Reflection.functionReturnType` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_ASYNC` | `Reflection.functionIsAsync` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_IS_NATIVE` | `Reflection.functionIsNative` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `REFLECTION_FUNCTION_INVOKE` | `Reflection.functionInvoke` | `Reflection` | compiler builtin: Reflection | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_SQRT` | `Math.sqrt` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ABS` | `Math.abs` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_POW` | `Math.pow` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_FLOOR` | `Math.floor` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_CEIL` | `Math.ceil` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ROUND` | `Math.round` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_MIN` | `Math.min` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_MAX` | `Math.max` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_CBRT` | `Math.cbrt` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_TRUNC` | `Math.trunc` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_SIN` | `Math.sin` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_COS` | `Math.cos` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_TAN` | `Math.tan` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ASIN` | `Math.asin` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ACOS` | `Math.acos` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ATAN` | `Math.atan` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_ATAN2` | `Math.atan2` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_LOG` | `Math.log` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_LOG10` | `Math.log10` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_EXP` | `Math.exp` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_RANDOM` | `Math.random` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_RANDOMINT` | `Math.randomInt` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `MATH_RANDOMFLOAT` | `Math.randomFloat` | `Math` | compiler builtin: Math | implemented | not explicitly declared in native catalog entry; default NONE |
| `IO_PRINT` | `IO.print` | `IO` | compiler/runtime IO boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `IO_PRINTLN` | `IO.println` | `IO` | compiler/runtime IO boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `IO_READLINE` | `IO.readLine` | `IO` | compiler/runtime IO boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `IO_CLEAR` | `IO.clear` | `IO` | compiler/runtime IO boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_NEWLIST` | `Collection.newList` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_PUSH` | `Collection.push` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_POP` | `Collection.pop` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_GET` | `Collection.get` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_SET` | `Collection.set` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_LENGTH` | `Collection.length` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_NEWMAP` | `Collection.newMap` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPSET` | `Collection.mapSet` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPGET` | `Collection.mapGet` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPHAS` | `Collection.mapHas` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPREMOVE` | `Collection.mapRemove` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPKEYS` | `Collection.mapKeys` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_MAPVALUES` | `Collection.mapValues` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_NEWSET` | `Collection.newSet` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_SETADD` | `Collection.setAdd` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_SETHAS` | `Collection.setHas` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_SETREMOVE` | `Collection.setRemove` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `COLLECTION_SETITEMS` | `Collection.setItems` | `Collection` | compiler builtin: List/Map/Set | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_NEWQUEUE` | `Queue.newQueue` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_ENQUEUE` | `Queue.enqueue` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_DEQUEUE` | `Queue.dequeue` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_PEEK` | `Queue.peek` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_ISEMPTY` | `Queue.isEmpty` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `QUEUE_SIZE` | `Queue.size` | `Queue` | stdlib: zl.util.Queue | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_NEWSTACK` | `Stack.newStack` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_PUSH` | `Stack.push` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_POP` | `Stack.pop` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_PEEK` | `Stack.peek` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_ISEMPTY` | `Stack.isEmpty` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STACK_SIZE` | `Stack.size` | `Stack` | stdlib: zl.util.Stack | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_LENGTH` | `String.length` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UPPER` | `String.upper` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_LOWER` | `String.lower` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_TRIM` | `String.trim` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_CONTAINS` | `String.contains` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_STARTSWITH` | `String.startsWith` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_ENDSWITH` | `String.endsWith` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_INDEXOF` | `String.indexOf` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_CHARAT` | `String.charAt` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_SUBSTRING` | `String.substring` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_REPLACE` | `String.replace` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_SPLIT` | `String.split` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_TOINT` | `String.toInt` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_TOFLOAT` | `String.toFloat` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_LASTINDEXOF` | `String.lastIndexOf` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_INDEXOFFROM` | `String.indexOfFrom` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_TRIMSTART` | `String.trimStart` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_TRIMEND` | `String.trimEnd` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_COMPARE` | `String.compare` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_COMPAREIGNORECASE` | `String.compareIgnoreCase` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_CODEPOINTAT` | `String.codePointAt` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_FROMCODEPOINT` | `String.fromCodePoint` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_REPEAT` | `String.repeatText` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8LENGTH` | `String.utf8Length` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8CHARAT` | `String.utf8CharAt` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8SUBSTRING` | `String.utf8Substring` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8REVERSE` | `String.utf8Reverse` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8CODEPOINTAT` | `String.utf8CodePointAt` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8FROMCODEPOINT` | `String.utf8FromCodePoint` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8BYTEINDEX` | `String.utf8ByteIndex` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `STRING_UTF8INDEXFROMBYTE` | `String.utf8IndexFromByte` | `String` | stdlib: zl.text.Text + primitive String surface | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_READFILE` | `FileSystem.readFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_WRITEFILE` | `FileSystem.writeFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_APPENDFILE` | `FileSystem.appendFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_EXISTS` | `FileSystem.exists` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_DELETEFILE` | `FileSystem.deleteFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_LISTDIR` | `FileSystem.listDir` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_RENAME` | `FileSystem.rename` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_COPYFILE` | `FileSystem.copyFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_SIZE` | `FileSystem.size` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_ISFILE` | `FileSystem.isFile` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_ISDIRECTORY` | `FileSystem.isDirectory` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_CREATEDIR` | `FileSystem.createDir` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_REMOVEDIR` | `FileSystem.removeDir` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_MODIFIEDTIME` | `FileSystem.modifiedTime` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_ABSOLUTEPATH` | `FileSystem.absolutePath` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_PARENTPATH` | `FileSystem.parentPath` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_FILENAME` | `FileSystem.fileName` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_EXTENSION` | `FileSystem.extension` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_STEM` | `FileSystem.stem` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_JOINPATH` | `FileSystem.joinPath` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_CURRENTDIR` | `FileSystem.currentDir` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `FILESYSTEM_TEMPDIR` | `FileSystem.tempDir` | `FileSystem` | stdlib: zl.fs.FileSystem | implemented | not explicitly declared in native catalog entry; default NONE |
| `NETWORK_RESOLVE` | `Network.resolve` | `Network` | stdlib: zl.net.Network | implemented | not explicitly declared in native catalog entry; default NONE |
| `NETWORK_RESOLVEALL` | `Network.resolveAll` | `Network` | stdlib: zl.net.Network | implemented | not explicitly declared in native catalog entry; default NONE |
| `NETWORK_HOSTNAME` | `Network.hostname` | `Network` | stdlib: zl.net.Network | implemented | not explicitly declared in native catalog entry; default NONE |
| `NETWORK_ISVALIDIP` | `Network.isValidIp` | `Network` | stdlib: zl.net.Network | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_NOW` | `Time.now` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_NOWMILLIS` | `Time.nowMillis` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_SLEEP` | `Time.sleep` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_SLEEP_ASYNC` | `Time.sleepAsync` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_YEAR` | `Time.year` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_MONTH` | `Time.month` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_DAY` | `Time.day` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_HOUR` | `Time.hour` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_MINUTE` | `Time.minute` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_SECOND` | `Time.second` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_FORMAT` | `Time.format` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_MONOTONICMILLIS` | `Time.monotonicMillis` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_DAYOFWEEK` | `Time.dayOfWeek` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_DAYOFYEAR` | `Time.dayOfYear` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_ISLEAPYEAR` | `Time.isLeapYear` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_FROMPARTS` | `Time.fromParts` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `TIME_UTCFORMAT` | `Time.utcFormat` | `Time` | stdlib: zl.time.* | implemented | not explicitly declared in native catalog entry; default NONE |
| `THREAD_START` | `Thread.start` | `Thread` | stdlib: zl.lang.Thread | implemented | not explicitly declared in native catalog entry; default NONE |
| `THREAD_JOIN` | `Thread.join` | `Thread` | stdlib: zl.lang.Thread | implemented | not explicitly declared in native catalog entry; default NONE |
| `THREAD_ISALIVE` | `Thread.isAlive` | `Thread` | stdlib: zl.lang.Thread | implemented | not explicitly declared in native catalog entry; default NONE |
| `TASK_SPAWN` | `Task.spawn` | `Task` | stdlib: zl.lang.Task | implemented | not explicitly declared in native catalog entry; default NONE |
| `MUTEX_WITHLOCK` | `Mutex.withLock` | `Mutex` | stdlib: zl.lang.Mutex | implemented | not explicitly declared in native catalog entry; default NONE |
| `RWLOCK_WITHREAD` | `RwLock.withRead` | `RwLock` | stdlib: zl.lang.RwLock | implemented | not explicitly declared in native catalog entry; default NONE |
| `RWLOCK_WITHWRITE` | `RwLock.withWrite` | `RwLock` | stdlib: zl.lang.RwLock | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_LOAD` | `Atomic.load` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_STORE` | `Atomic.store` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_ADD` | `Atomic.add` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_LOAD_BOOL` | `Atomic.loadBool` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_STORE_BOOL` | `Atomic.storeBool` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_LOAD_DOUBLE` | `Atomic.loadDouble` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_STORE_DOUBLE` | `Atomic.storeDouble` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_LOAD_REF` | `Atomic.loadRef` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `ATOMIC_STORE_REF` | `Atomic.storeRef` | `Atomic` | stdlib: zl.lang.Atomic | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_ACQUIRE` | `Semaphore.acquire` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_RELEASE` | `Semaphore.release` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_AVAILABLE` | `Semaphore.available` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_SETPERMITS` | `Semaphore.setPermits` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_TRYACQUIRE` | `Semaphore.tryAcquire` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `SEMAPHORE_RELEASEMANY` | `Semaphore.releaseMany` | `Semaphore` | stdlib: zl.lang.Semaphore | implemented | not explicitly declared in native catalog entry; default NONE |
| `CONDITION_WAIT` | `Condition.wait` | `Condition` | stdlib: zl.lang.Condition | implemented | not explicitly declared in native catalog entry; default NONE |
| `CONDITION_NOTIFYONE` | `Condition.notifyOne` | `Condition` | stdlib: zl.lang.Condition | implemented | not explicitly declared in native catalog entry; default NONE |
| `CONDITION_NOTIFYALL` | `Condition.notifyAll` | `Condition` | stdlib: zl.lang.Condition | implemented | not explicitly declared in native catalog entry; default NONE |
| `CONDITION_WAITFOR` | `Condition.waitFor` | `Condition` | stdlib: zl.lang.Condition | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_CREATE` | `Channel.create` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_SEND` | `Channel.send` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_RECEIVE` | `Channel.receive` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_SIZE` | `Channel.size` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_SEND_ASYNC` | `Channel.sendAsync` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `CHANNEL_RECEIVE_ASYNC` | `Channel.receiveAsync` | `Channel` | stdlib: zl.lang.Channel | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_FAIL` | `Test.fail` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_ASSERTTRUE` | `Test.assertTrue` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_ASSERTFALSE` | `Test.assertFalse` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_ASSERTEQUAL` | `Test.assertEqual` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_ASSERTNOTEQUAL` | `Test.assertNotEqual` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEST_ASSERTNEAR` | `Test.assertNear` | `Test` | stdlib: zl.test.Test | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_INFO` | `Log.info` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_WARN` | `Log.warn` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_ERROR` | `Log.error` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_DEBUG` | `Log.debug` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_TRACE` | `Log.trace` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_FATAL` | `Log.fatal` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `LOG_AT` | `Log.at` | `Log` | stdlib: zl.logging.Log | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_FORMAT` | `Text.format` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXMATCHES` | `Text.regexMatches` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXFULLMATCHES` | `Text.regexFullMatches` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXFINDALL` | `Text.regexFindAll` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXREPLACE` | `Text.regexReplace` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXFIND` | `Text.regexFind` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `TEXT_REGEXFINDMATCHES` | `Text.regexFindMatches` | `Text` | stdlib: zl.text.Text | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_ENCODE` | `Serialize.encode` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_DECODE` | `Serialize.decode` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_ASSTRING` | `Serialize.asString` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_ASINT` | `Serialize.asInt` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_ASDOUBLE` | `Serialize.asDouble` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `SERIALIZE_ASBOOL` | `Serialize.asBool` | `Serialize` | stdlib: zl.serialize.Serialize | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_CRC32` | `Crypto.crc32` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_SHA256` | `Crypto.sha256` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_SHA1` | `Crypto.sha1` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_MD5` | `Crypto.md5` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_FNV1A64` | `Crypto.fnv1a64` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_HEXENCODE` | `Crypto.hexEncode` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_HEXDECODE` | `Crypto.hexDecode` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_BASE64ENCODE` | `Crypto.base64Encode` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `CRYPTO_BASE64DECODE` | `Crypto.base64Decode` | `Crypto` | stdlib: zl.crypto.Crypto | implemented | not explicitly declared in native catalog entry; default NONE |
| `HASH_CODE` | `Hash.code` | `Hash` | compiler/runtime primitive | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_EXIT` | `System.exit` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_GETENV` | `System.getEnv` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_EXEC` | `System.exec` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_EXECSTATUS` | `System.execStatus` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_SETENV` | `System.setEnv` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_HASENV` | `System.hasEnv` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_ENVOR` | `System.envOr` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `SYSTEM_PLATFORM` | `System.platform` | `System` | compiler/runtime system boundary | implemented | not explicitly declared in native catalog entry; default NONE |
| `INT_PARSE` | `Int.parse` | `Int` | compiler/runtime primitive | implemented | not explicitly declared in native catalog entry; default NONE |
| `DOUBLE_PARSE` | `Double.parse` | `Double` | compiler/runtime primitive | implemented | not explicitly declared in native catalog entry; default NONE |
| `BOOL_PARSE` | `Bool.parse` | `Bool` | compiler/runtime primitive | implemented | not explicitly declared in native catalog entry; default NONE |
| `GC_STATS` | `GC.stats` | `GC` | compiler/runtime native surface | implemented | not explicitly declared in native catalog entry; default NONE |
