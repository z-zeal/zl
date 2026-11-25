#include "zl/vm/value.hpp"
#include "zl/vm/gc.hpp"

#include <stdexcept>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace zl {

Value makeEmptyList() {
    return makeGCList();
}

Value makeEmptyMap() {
    return makeGCMap();
}

Value makeEmptyObject(const std::string& className) {
    auto box = makeGCObject();
    box->className = className;
    return box;
}

namespace {

std::string escapeRecordString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 2);
    for (unsigned char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
                    out += hex.str();
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// Shared recursive formatter. Top-level values print strings raw (so
// log("hi") prints hi, not "hi"), but elements NESTED inside a list/map print
// strings quoted (so log({"a", "b"}) prints ["a", "b"], not [a, b] - otherwise
// you can't tell a list of strings from a list of identifiers-gone-wrong).
//
// The value graph can be deep (built in a loop) or cyclic (a list holding
// itself), and printing is exactly where users meet such values - so the
// traversal is total: ancestor cycles print as `<cyclic>` and subtrees past
// kMaxValueNestingDepth print as `<...>`, never a stack overflow. `active`
// holds only the current root-to-leaf path (insert on entry, erase on
// exit), so diamond sharing still prints every occurrence in full; `depth`
// is passed by value so siblings never consume each other's budget.
std::string formatValueInner(const Value& v, bool quoteStrings,
                             std::unordered_set<const void*>& active, int depth) {
    return std::visit([quoteStrings, &active, depth](const auto& held) -> std::string {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            // `nil` is what the *runtime* type name uses (typeKind/typeName), but
            // it is not a literal the language accepts - `string s = nil` does not
            // compile. Printing a null value as `nil` therefore showed users text
            // they cannot write back; the source spelling is `null`.
            return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            return held ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(held);
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(held)) {
                if (std::isnan(held)) return "nan";
                return held < 0 ? "-inf" : "inf";
            }
            return doubleToShortestString(held);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return quoteStrings ? ("\"" + held + "\"") : held;
        } else if constexpr (std::is_same_v<T, ListRef>) {
            if (!held) return "[]";
            if (!active.insert(held.get()).second) return "[<cyclic>]";
            if (depth + 1 > kMaxValueNestingDepth) { active.erase(held.get()); return "[<...>]"; }
            std::string s = "[";
            for (std::size_t i = 0; i < held->items.size(); ++i) {
                if (i) s += ", ";
                s += formatValueInner(held->items[i], true, active, depth + 1);
            }
            active.erase(held.get());
            return s + "]";
        } else if constexpr (std::is_same_v<T, MapRef>) {
            if (!held) return "{}";
            if (!active.insert(held.get()).second) return "{<cyclic>}";
            if (depth + 1 > kMaxValueNestingDepth) { active.erase(held.get()); return "{<...>}"; }
            std::string s = "{";
            const auto snapshot = held->snapshotEntries();
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                if (i) s += ", ";
                s += formatValueInner(snapshot[i].first, true, active, depth + 1) + ": " +
                     formatValueInner(snapshot[i].second, true, active, depth + 1);
            }
            active.erase(held.get());
            return s + "}";
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (!held) return "nil";
            if (!active.insert(held.get()).second) return held->className + " { <cyclic> }";
            if (depth + 1 > kMaxValueNestingDepth) { active.erase(held.get()); return held->className + " { <...> }"; }
            // Generic collections are thin wrappers whose only field is the native storage.
            // Printing the wrapper would emit `List{__native: [...]}` – the plumbing rather
            // than the data – so print the payload directly, mirroring Serialize.encode's
            // look-through (P2-1 fix).
            if (held->className == "List" || held->className == "Map" || held->className == "Set") {
                const Value* nativeField = objectFieldLookup(*held, "__native");
                if (nativeField != nullptr) {
                    std::string inner = formatValueInner(*nativeField, true, active, depth + 1);
                    active.erase(held.get());
                    return inner;
                }
            }
            std::string s = held->className + " { ";
            bool first = true;
            if (held->runtimeType && held->runtimeType->isDataType) {
                // Layout order (the flat slot order), which is also the
                // declaration order both pipelines record. Static fields have
                // no instance slot and are skipped, as before.
                const auto& layout = *held->runtimeType;
                for (const auto& field : layout.fields) {
                    if (field.isStatic) continue;
                    const Value* value = objectFieldLookup(*held, field.name);
                    if (value == nullptr) continue;
                    if (!first) s += ", ";
                    s += field.name + ": ";
                    if (std::holds_alternative<std::string>(*value)) {
                        s += "\"" + escapeRecordString(std::get<std::string>(*value)) + "\"";
                    } else {
                        s += formatValueInner(*value, true, active, depth + 1);
                    }
                    first = false;
                }
            } else {
                s = held->className + "{";
                // Declared fields print in layout order (deterministic; the
                // old per-object map had no guaranteed order), then any
                // out-of-layout names the box carries.
                objectFieldForEach(*held, [&](const std::string& k, const Value& fieldValue) {
                    if (!first) s += ", ";
                    s += k + ": " + formatValueInner(fieldValue, true, active, depth + 1);
                    first = false;
                });
            }
            active.erase(held.get());
            return s + "}";
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            return "<func>";
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            return held ? "<task>" : "nil";
        } else if constexpr (std::is_same_v<T, ThreadRef>) {
            return held ? "<thread>" : "nil";
        } else if constexpr (std::is_same_v<T, NativeHandleRef>) {
            return held.valid() ? "<native-handle>" : "nil";
        } else if constexpr (std::is_same_v<T, NativeBufferView>) {
            return held.valid() ? "<native-buffer>" : "nil";
        } else {
            return held.valid() ? "<native-callback>" : "nil";
        }
    }, v);
}

// The shortest decimal digits that read back as `value`, plus its decimal
// exponent: value == 0.<digits> x 10^(exponent + 1), with no trailing zero in
// `digits` (so 100 is digits "1", exponent 2). Returns false if no exact
// spelling was produced, which the caller treats as "keep the long form".
//
// Two sources, in order of trust:
//
//   * `std::to_chars` in scientific form, where the toolchain's <charconv>
//     supports floating point (GCC 11+, libc++ 14+, MSVC 19.14+). Every
//     conforming implementation must produce the shortest representation that
//     parses back to the same value, with ties to even, so the digits are the
//     standard's and not the host's. Only the scientific form is asked for:
//     to_chars also picks between fixed and scientific, and *that* choice is
//     not pinned by the standard - libstdc++ renders 1.2345678901234568e17 as
//     `123456789012345680` where other libraries write `1.2345678901234568e+17`.
//     Letting it decide would make a program's printed output depend on the
//     host it was built on, and examples/run_all.sh compares bytes on three
//     platforms. The style is this file's decision (see below).
//
//   * otherwise, ask printf for p significant digits and check with strtod
//     whether that spelling parses back to the same bits, ascending from p = 1.
//     17 always succeeds (max_digits10), so the loop terminates with an exact
//     shortest-length spelling. Both sources were compared over 1.5M values
//     (random bit patterns, denormals, DBL_MAX, powers of ten) and agreed on
//     every one; on a host whose printf or strtod is not correctly rounded the
//     fallback could differ in the last digit of a 17-digit value, but never
//     in information - every spelling it returns has been read back with
//     strtod and compared bit for bit before it is accepted.
bool shortestDigits(double value, std::string& digits, int& exponent) {
    const double magnitude = std::fabs(value); // the sign is the caller's business
    char buf[64]; // "-d.ddddddddddddddddde-308" fits in 26
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto result = std::to_chars(buf, buf + sizeof(buf), magnitude, std::chars_format::scientific);
    if (result.ec == std::errc{}) {
        const std::string text(buf, result.ptr);
        const std::size_t e = text.find('e');
        if (e != std::string::npos) {
            std::string mantissa = text.substr(0, e);
            mantissa.erase(std::remove(mantissa.begin(), mantissa.end(), '.'), mantissa.end());
            const int parsedExponent = static_cast<int>(std::strtol(text.c_str() + e + 1, nullptr, 10));
            // Trust but verify: the digits must read back as the same double.
            if (std::strtod(text.c_str(), nullptr) == magnitude) {
                digits = mantissa;
                exponent = parsedExponent;
                return true;
            }
        }
    }
#endif
    for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10; ++precision) {
        const int written = std::snprintf(buf, sizeof(buf), "%.*e", precision - 1, magnitude);
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buf)) return false;
        if (std::strtod(buf, nullptr) != magnitude) continue;
        const char* mantissaEnd = std::strchr(buf, 'e');
        if (!mantissaEnd) return false;
        digits.assign(buf, static_cast<std::size_t>(mantissaEnd - buf));
        digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
        exponent = static_cast<int>(std::strtol(mantissaEnd + 1, nullptr, 10));
        return true;
    }
    return false;
}

} // namespace

// The language's ONE decimal spelling for a finite double: the shortest digit
// string that reads back as the same binary value, so 3.14 prints as `3.14`
// and 0.1 + 0.2 prints as `0.30000000000000004` - the digits that are really
// there, and not one more. Everything that renders a double goes through here:
// log(), string concatenation, Text.format, list/map/record members,
// Serialize.encode, and error messages that quote a value.
//
// Printing 17 significant digits instead (std::numeric_limits<double>::
// max_digits10) round-trips too, but it is the *longest* such spelling rather
// than the shortest, which is why the first double a beginner prints used to
// come back as `3.1400000000000001` (examples/REVIEW.md, F9).
//
// Fixed vs scientific is decided here, on the decimal point position, the way
// Python's repr and printf's %g do it: fixed while the point sits in -3..17
// (`0.0001`, `3.14`, `10000000000000000`), scientific outside it (`1e-05`,
// `1e+17`). A whole-valued double therefore prints without a fraction - which
// is what the 17-digit form already did for `1.0`, so `log(1.0)` is `1` and
// `log(-0.0)` is `-0`, unchanged. There is no precision specifier; round with
// Math.round and pad with Text.padLeft when a column has to line up.
std::string doubleToShortestString(double value) {
    if (value == 0.0) {
        // Keep the sign of a negative zero: -0.0 is a distinct value, and
        // LiteralFoldParity.zl pins that it prints "-0".
        return std::signbit(value) ? "-0" : "0";
    }

    std::string digits;
    int exponent = 0; // value == 0.<digits> x 10^(exponent + 1), i.e. floor(log10(|value|))
    if (!shortestDigits(value, digits, exponent)) {
        // Not reachable: precision 17 round-trips every double by definition of
        // max_digits10. Keep the long spelling rather than lose information.
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        return oss.str();
    }

    // Shortest means shortest: a trailing zero only existed to pad the exponent.
    const std::size_t lastSignificant = digits.find_last_not_of('0');
    digits.erase(lastSignificant == std::string::npos ? 1 : lastSignificant + 1);

    // Position of the decimal point relative to the digits: 123.4 has point 3,
    // 0.001234 has point -2.
    const int point = exponent + 1;
    std::string out;

    if (point > -4 && point <= 17) {
        if (point <= 0) {
            out = "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
        } else if (static_cast<std::size_t>(point) >= digits.size()) {
            out = digits + std::string(static_cast<std::size_t>(point) - digits.size(), '0');
        } else {
            out = digits.substr(0, static_cast<std::size_t>(point)) + "." +
                  digits.substr(static_cast<std::size_t>(point));
        }
    } else {
        out = digits.size() > 1 ? digits.substr(0, 1) + "." + digits.substr(1) : digits;
        const int scientificExponent = point - 1;
        out += "e";
        out += scientificExponent < 0 ? "-" : "+";
        const unsigned magnitude = static_cast<unsigned>(std::abs(scientificExponent));
        if (magnitude < 10) out += "0"; // two exponent digits, as printf and to_chars do
        out += std::to_string(magnitude);
    }

    return value < 0.0 ? "-" + out : out;
}

std::string valueToString(const Value& v) {
    std::unordered_set<const void*> active;
    return formatValueInner(v, false, active, 0);
}

bool isTruthy(const Value& v) {
    return std::visit([](const auto& held) -> bool {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return false;
        } else if constexpr (std::is_same_v<T, bool>) {
            return held;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return held != 0;
        } else if constexpr (std::is_same_v<T, double>) {
            return held != 0.0;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return !held.empty();
        } else if constexpr (std::is_same_v<T, ListRef>) {
            return held && !held->items.empty();
        } else if constexpr (std::is_same_v<T, MapRef>) {
            return held && held->size() != 0;
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            return held != nullptr; // non-nil object is truthy
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            return held != nullptr; // non-nil func value is truthy
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            return held != nullptr;
        } else if constexpr (std::is_same_v<T, ThreadRef>) {
            return held != nullptr;
        } else if constexpr (std::is_same_v<T, NativeHandleRef>) {
            return held.valid();
        } else if constexpr (std::is_same_v<T, NativeBufferView>) {
            return held.valid();
        } else {
            return held.valid();
        }
    }, v);
}

bool isNumericValue(const Value& v) {
    return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v);
}

double toDouble(const Value& v) {
    if (auto p = std::get_if<std::int64_t>(&v)) return static_cast<double>(*p);
    if (auto p = std::get_if<double>(&v)) return *p;
    throw std::runtime_error("expected a number");
}

std::int64_t toInt64Strict(const Value& v) {
    if (auto p = std::get_if<std::int64_t>(&v)) return *p;
    throw std::runtime_error("expected a whole number, got a decimal or non-number");
}

namespace {
int compareIntAndDouble(std::int64_t i, double d) {
    if (std::isnan(d)) return 2;
    if (d > 0.0) {
        constexpr double int64MaxExclusive = 9223372036854775808.0; // 2^63
        if (d >= int64MaxExclusive) return -1;

        const double lower = std::floor(d);
        const auto lowerInt = static_cast<std::int64_t>(lower);
        if (i < lowerInt) return -1;
        if (i > lowerInt) return 1;
        return lower == d ? 0 : -1;
    }

    if (d < 0.0) {
        constexpr double int64Min = -9223372036854775808.0; // -2^63
        if (d < int64Min) return 1;

        const double upper = std::ceil(d);
        const auto upperInt = static_cast<std::int64_t>(upper);
        if (i < upperInt) return -1;
        if (i > upperInt) return 1;
        return upper == d ? 0 : 1;
    }

    return i == 0 ? 0 : (i < 0 ? -1 : 1);
}

int compareNumbers(const Value& a, const Value& b) {
    if (auto ai = std::get_if<std::int64_t>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b)) return (*ai > *bi) - (*ai < *bi);
        return compareIntAndDouble(*ai, std::get<double>(b));
    }
    if (auto ad = std::get_if<double>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b)) return -compareIntAndDouble(*bi, *ad);
        const double bd = std::get<double>(b);
        return (*ad > bd) - (*ad < bd);
    }
    throw std::runtime_error("expected numeric values");
}
} // namespace

int compareNumericValues(const Value& a, const Value& b) {
    if (!isNumericValue(a) || !isNumericValue(b))
        throw std::runtime_error("expected numeric values");
    return compareNumbers(a, b);
}

namespace {
struct ObjectPairHash {
    std::size_t operator()(const std::pair<const ObjectBox*, const ObjectBox*>& p) const noexcept {
        auto h1 = std::hash<const ObjectBox*>{}(p.first);
        auto h2 = std::hash<const ObjectBox*>{}(p.second);
        return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b9) + (h1 << 6) + (h1 >> 2));
    }
};

bool recordValuesEqual(const ObjectRef& a, const ObjectRef& b,
                       std::unordered_set<std::pair<const ObjectBox*, const ObjectBox*>, ObjectPairHash>& seen,
                       int depth) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (!a->runtimeType || !b->runtimeType || !a->runtimeType->isDataType || !b->runtimeType->isDataType || a->className != b->className) return false;
    // Cycles are cut off by `seen`, but depth is not: a 100k-long record
    // chain compared with == is a stack overflow without this budget. Like
    // Python's RecursionError on deep comparison, this is a clean error the
    // program can catch, not a crash.
    if (depth > kMaxValueNestingDepth) {
        throw std::runtime_error("structural comparison nesting exceeds the maximum depth of " +
                                 std::to_string(kMaxValueNestingDepth));
    }

    const auto key = std::make_pair(a.get(), b.get());
    if (!seen.insert(key).second) return true;

    const std::size_t aCount = a->fields.size() + a->extraFields.size();
    const std::size_t bCount = b->fields.size() + b->extraFields.size();
    if (aCount != bCount) return false;
    // Name-based, like the old map scan: two same-named records from
    // different modules can carry different layouts, so the pair is compared
    // by field name and each side is resolved through its own layout.
    auto compareField = [&](const std::string& name, const Value& av) {
        const Value* bv = objectFieldLookup(*b, name);
        if (bv == nullptr) return false;
        if (isNumericValue(av) && isNumericValue(*bv)) {
            // Use the public numeric equality rule here too, so nested record
            // fields preserve special-value semantics such as NaN != NaN.
            return valuesEqual(av, *bv);
        }
        if (std::holds_alternative<ObjectRef>(av) && std::holds_alternative<ObjectRef>(*bv)) {
            return recordValuesEqual(std::get<ObjectRef>(av), std::get<ObjectRef>(*bv), seen, depth + 1);
        }
        return valuesEqual(av, *bv);
    };
    if (a->runtimeType) {
        const std::size_t count = std::min(a->fields.size(), a->runtimeType->instanceFieldCount);
        for (std::size_t i = 0; i < count; ++i) {
            if (!compareField(a->runtimeType->instanceFieldNames[i], a->fields[i])) return false;
        }
    }
    for (const auto& [name, av] : a->extraFields) {
        if (!compareField(name, av)) return false;
    }
    return true;
}
} // namespace

namespace {
std::size_t hashCombineValue(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6) + (seed >> 2));
}

std::size_t valueHashImpl(const Value& v, std::unordered_set<const ObjectBox*>& seen, int depth);

std::size_t recordHash(const ObjectRef& obj, std::unordered_set<const ObjectBox*>& seen, int depth) {
    if (!obj) return 0;
    if (!seen.insert(obj.get()).second) return std::hash<const ObjectBox*>{}(obj.get());
    // Same budget as structural comparison: hashing a 100k-deep record
    // chain must be a catchable error, not a stack overflow.
    if (depth > kMaxValueNestingDepth) {
        throw std::runtime_error("structural hash nesting exceeds the maximum depth of " +
                                 std::to_string(kMaxValueNestingDepth));
    }
    std::size_t seed = std::hash<std::string>{}(obj->className);
    if (obj->runtimeType && obj->runtimeType->isDataType) {
        // Layout order, as before - static fields have no instance slot.
        for (const auto& field : obj->runtimeType->fields) {
            if (field.isStatic) continue;
            const Value* value = objectFieldLookup(*obj, field.name);
            if (value == nullptr) continue;
            seed = hashCombineValue(seed, std::hash<std::string>{}(field.name));
            seed = hashCombineValue(seed, valueHashImpl(*value, seen, depth + 1));
        }
    }
    return seed;
}

std::size_t valueHashImpl(const Value& v, std::unordered_set<const ObjectBox*>& seen, int depth) {
    return std::visit([&](const auto& held) -> std::size_t {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) return 0x11;
        else if constexpr (std::is_same_v<T, bool>) return hashCombineValue(0x12, std::hash<bool>{}(held));
        else if constexpr (std::is_same_v<T, std::int64_t>) return hashCombineValue(0x13, std::hash<std::int64_t>{}(held));
        else if constexpr (std::is_same_v<T, double>) {
            if (std::isnan(held)) return 0x14;
            if (held == 0.0) return hashCombineValue(0x13, std::hash<std::int64_t>{}(0));
            constexpr double minExactInt = -9223372036854775808.0;
            constexpr double maxExactIntExclusive = 9223372036854775808.0;
            if (held >= minExactInt && held < maxExactIntExclusive && std::floor(held) == held) {
                return hashCombineValue(0x13, std::hash<std::int64_t>{}(static_cast<std::int64_t>(held)));
            }
            return hashCombineValue(0x14, std::hash<double>{}(held));
        } else if constexpr (std::is_same_v<T, std::string>) return hashCombineValue(0x15, std::hash<std::string>{}(held));
        else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (held && held->runtimeType && held->runtimeType->isDataType) return hashCombineValue(0x18, recordHash(held, seen, depth));
            return hashCombineValue(0x18, std::hash<const ObjectBox*>{}(held.get()));
        } else if constexpr (std::is_same_v<T, ListRef>) return hashCombineValue(0x16, std::hash<const ListBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, MapRef>) return hashCombineValue(0x17, std::hash<const MapBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, ClosureRef>) return hashCombineValue(0x19, std::hash<const ClosureBox*>{}(held.get()));
        else if constexpr (std::is_same_v<T, TaskRef>) return hashCombineValue(0x1a, std::hash<const void*>{}(held.get()));
        else if constexpr (std::is_same_v<T, ThreadRef>) return hashCombineValue(0x1b, std::hash<const void*>{}(held.get()));
        else if constexpr (std::is_same_v<T, NativeHandleRef>) return hashCombineValue(0x1c, std::hash<std::uint64_t>{}(held.id));
        else if constexpr (std::is_same_v<T, NativeBufferView>) return hashCombineValue(0x1d, hashCombineValue(std::hash<const void*>{}(held.data()), std::hash<std::size_t>{}(held.size())));
        else if constexpr (std::is_same_v<T, NativeStructView>) return hashCombineValue(0x1e, hashCombineValue(std::hash<const void*>{}(held.data()), hashCombineValue(std::hash<std::size_t>{}(held.size()), std::hash<std::size_t>{}(held.alignment()))));
        else return hashCombineValue(0x1f, std::hash<std::uint64_t>{}(held.id));
    }, v);
}
} // namespace

std::int64_t valueHashCode(const Value& v) {
    std::unordered_set<const ObjectBox*> seen;
    return static_cast<std::int64_t>(valueHashImpl(v, seen, 0) & 0x7fffffffffffffffULL);
}

bool valuesEqual(const Value& a, const Value& b) {
    if (isNumericValue(a) && isNumericValue(b)) {
        if (const auto* ad = std::get_if<double>(&a); ad && std::isnan(*ad)) return false;
        if (const auto* bd = std::get_if<double>(&b); bd && std::isnan(*bd)) return false;
        return compareNumbers(a, b) == 0;
    }
    if (std::holds_alternative<ThreadRef>(a) && std::holds_alternative<ThreadRef>(b)) {
        return std::get<ThreadRef>(a) == std::get<ThreadRef>(b);
    }
    if (std::holds_alternative<NativeHandleRef>(a) && std::holds_alternative<NativeHandleRef>(b)) return std::get<NativeHandleRef>(a) == std::get<NativeHandleRef>(b);
    if (std::holds_alternative<NativeBufferView>(a) && std::holds_alternative<NativeBufferView>(b)) return std::get<NativeBufferView>(a) == std::get<NativeBufferView>(b);
    if (std::holds_alternative<NativeStructView>(a) && std::holds_alternative<NativeStructView>(b)) return std::get<NativeStructView>(a).data() == std::get<NativeStructView>(b).data() && std::get<NativeStructView>(a).size() == std::get<NativeStructView>(b).size() && std::get<NativeStructView>(a).alignment() == std::get<NativeStructView>(b).alignment();
    if (std::holds_alternative<NativeCallbackRef>(a) && std::holds_alternative<NativeCallbackRef>(b)) return std::get<NativeCallbackRef>(a) == std::get<NativeCallbackRef>(b);
    if (std::holds_alternative<ObjectRef>(a) && std::holds_alternative<ObjectRef>(b)) {
        const auto& ao = std::get<ObjectRef>(a);
        const auto& bo = std::get<ObjectRef>(b);
        if (ao && bo && ao->runtimeType && bo->runtimeType && ao->runtimeType->isDataType && bo->runtimeType->isDataType) {
            std::unordered_set<std::pair<const ObjectBox*, const ObjectBox*>, ObjectPairHash> seen;
            return recordValuesEqual(ao, bo, seen, 0);
        }
        return ao == bo;
    }
    if (a.index() == b.index()) return a == b; // string==string, bool==bool, ListRef/MapRef compare by identity
    return false;
}

void MapBox::noteAppendedKeyLocked(const Value& key) const {
    const std::size_t position = entries.size() - 1;
    if (const auto* stringKey = std::get_if<std::string>(&key); stringKey && stringKeyIndexValid) {
        stringKeyIndex.emplace(*stringKey, position);
    }
    if (isNumericValue(key) && numericKeyIndexValid) {
        numericKeyIndex.emplace(valueHashCode(key), position);
    }
}

std::size_t MapBox::findEntry(const Value& key) const {
    std::lock_guard<std::mutex> lock(mutex);
    return findEntryLocked(key);
}

std::optional<Value> MapBox::tryGetEntry(const Value& key) const {
    std::lock_guard<std::mutex> lock(mutex);
    const std::size_t pos = findEntryLocked(key);
    if (pos == kNoEntry) return std::nullopt;
    return entries[pos].second;
}

void MapBox::setEntry(const Value& key, Value value) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::size_t pos = findEntryLocked(key);
    if (pos != kNoEntry) {
        // In-place value update: keys keep their positions, so the lookup
        // index stays valid.
        entries[pos].second = std::move(value);
        return;
    }
    entries.emplace_back(key, std::move(value));
    noteAppendedKeyLocked(key);
}

bool MapBox::removeEntry(const Value& key) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::size_t pos = findEntryLocked(key);
    if (pos == kNoEntry) return false;
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(pos));
    invalidateKeyIndexLocked();
    return true;
}

std::vector<std::pair<Value, Value>> MapBox::snapshotEntries() const {
    std::lock_guard<std::mutex> lock(mutex);
    return entries;
}

std::size_t MapBox::findEntryLocked(const Value& key) const {
    // Numeric equality crosses int/double representations. valueHashCode is
    // canonical for equal numeric values, but hash collisions (and NaN's
    // deliberate non-equality) still require valuesEqual confirmation.
    if (isNumericValue(key) && entries.size() >= 8) {
        if (!numericKeyIndexValid) {
            numericKeyIndex.clear();
            numericKeyIndex.reserve(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (isNumericValue(entries[i].first)) {
                    numericKeyIndex.emplace(valueHashCode(entries[i].first), i);
                }
            }
            numericKeyIndexValid = true;
        }
        const auto [begin, end] = numericKeyIndex.equal_range(valueHashCode(key));
        for (auto it = begin; it != end; ++it) {
            if (it->second < entries.size() && valuesEqual(entries[it->second].first, key)) {
                return it->second;
            }
        }
        return kNoEntry;
    }

    // String keys can never equal a non-string key (see the comment on
    // stringKeyIndex), so they may use the hash index once the map is big
    // enough for it to pay for itself.
    if (const auto* needle = std::get_if<std::string>(&key); needle && entries.size() >= 8) {
        if (!stringKeyIndexValid) {
            stringKeyIndex.clear();
            stringKeyIndex.reserve(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto* entryKey = std::get_if<std::string>(&entries[i].first);
                if (!entryKey) continue; // non-string keys cannot match a string lookup
                // emplace keeps the first position for a duplicate key; duplicates
                // cannot arise through Collection.mapSet, this is just defensive.
                stringKeyIndex.emplace(*entryKey, i);
            }
            stringKeyIndexValid = true;
        }
        const auto it = stringKeyIndex.find(*needle);
        if (it == stringKeyIndex.end()) return kNoEntry;
        // Defensive consistency check: the cache is only advisory.
        if (it->second < entries.size() && valuesEqual(entries[it->second].first, key)) {
            return it->second;
        }
        return kNoEntry;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (valuesEqual(entries[i].first, key)) return i;
    }
    return kNoEntry;
}

} // namespace zl
