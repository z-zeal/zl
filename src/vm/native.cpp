#include "zl/vm/native.hpp"
#include "zl/vm/runtime_fault.hpp"
#include "zl/compiler/native_catalog.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/runtime_thread.hpp"
#include "zl/vm/runtime_task_executor.hpp"
#include "zl/vm/vm.hpp"
#include "zl/vm/value.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/runtime_type_checks.hpp"
#include "zl/regex/regex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <netdb.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#endif
#include <cstdint>

namespace zl {

thread_local VM* g_currentNativeVm = nullptr;

VM* setCurrentNativeVm(VM* vm) {
    VM* previous = g_currentNativeVm;
    g_currentNativeVm = vm;
    return previous;
}

namespace {

const Chunk* nativeChunk() { return g_currentNativeVm ? g_currentNativeVm->activeChunk() : nullptr; }

// --- Math ---

Value mathSqrt(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    if (!(x >= 0.0)) throw std::runtime_error("Math.sqrt: domain error; expected a non-negative value");
    return std::sqrt(x);
}

Value mathAbs(const std::vector<Value>& args) {
    if (auto p = std::get_if<std::int64_t>(&args[0])) {
        if (*p == std::numeric_limits<std::int64_t>::min())
            throw std::runtime_error("Math.abs: integer overflow for INT64_MIN");
        return *p < 0 ? -*p : *p;
    }
    return std::fabs(toDouble(args[0]));
}

Value mathPow(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    const double y = toDouble(args[1]);
    const double result = std::pow(x, y);
    // Like Math.exp: a non-finite result from finite inputs is an error,
    // not a silent inf/NaN. pow() overflows to infinity on huge results
    // and returns NaN on domain errors (a negative base with a
    // non-integer exponent).
    if (std::isinf(result) && std::isfinite(x) && std::isfinite(y))
        throw std::runtime_error("Math.pow: result overflow");
    if (std::isnan(result) && std::isfinite(x) && std::isfinite(y))
        throw std::runtime_error("Math.pow: domain error; expected a non-negative base or an integer exponent");
    return result;
}

Value mathFloor(const std::vector<Value>& args) {
    return std::floor(toDouble(args[0]));
}

Value mathCeil(const std::vector<Value>& args) {
    return std::ceil(toDouble(args[0]));
}

Value mathRound(const std::vector<Value>& args) {
    return std::round(toDouble(args[0]));
}

Value mathMin(const std::vector<Value>& args) {
    bool bothInt = std::holds_alternative<std::int64_t>(args[0]) && std::holds_alternative<std::int64_t>(args[1]);
    if (bothInt) return std::min(std::get<std::int64_t>(args[0]), std::get<std::int64_t>(args[1]));
    return std::min(toDouble(args[0]), toDouble(args[1]));
}

Value mathMax(const std::vector<Value>& args) {
    bool bothInt = std::holds_alternative<std::int64_t>(args[0]) && std::holds_alternative<std::int64_t>(args[1]);
    if (bothInt) return std::max(std::get<std::int64_t>(args[0]), std::get<std::int64_t>(args[1]));
    return std::max(toDouble(args[0]), toDouble(args[1]));
}



Value mathCbrt(const std::vector<Value>& args) {
    return std::cbrt(toDouble(args[0]));
}

Value mathTrunc(const std::vector<Value>& args) {
    return std::trunc(toDouble(args[0]));
}

Value mathSin(const std::vector<Value>& args) { return std::sin(toDouble(args[0])); }
Value mathCos(const std::vector<Value>& args) { return std::cos(toDouble(args[0])); }
Value mathTan(const std::vector<Value>& args) { return std::tan(toDouble(args[0])); }

Value mathAsin(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    if (x < -1.0 || x > 1.0) throw std::runtime_error("Math.asin: domain error; expected [-1, 1]");
    return std::asin(x);
}

Value mathAcos(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    if (x < -1.0 || x > 1.0) throw std::runtime_error("Math.acos: domain error; expected [-1, 1]");
    return std::acos(x);
}

Value mathAtan(const std::vector<Value>& args) { return std::atan(toDouble(args[0])); }

Value mathAtan2(const std::vector<Value>& args) {
    return std::atan2(toDouble(args[0]), toDouble(args[1]));
}

Value mathLog(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    if (!(x > 0.0)) throw std::runtime_error("Math.log: domain error; expected a value > 0");
    return std::log(x);
}

Value mathLog10(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    if (!(x > 0.0)) throw std::runtime_error("Math.log10: domain error; expected a value > 0");
    return std::log10(x);
}

Value mathExp(const std::vector<Value>& args) {
    const double x = toDouble(args[0]);
    const double result = std::exp(x);
    if (std::isinf(result) && std::isfinite(x))
        throw std::runtime_error("Math.exp: result overflow");
    return result;
}




// Random helpers intentionally use a process-local PRNG. They are suitable
// for simulation/game/application randomness, not cryptography.
std::mt19937_64& mathRng() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    return rng;
}

Value mathRandom(const std::vector<Value>& /*args*/) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(mathRng());
}

Value mathRandomInt(const std::vector<Value>& args) {
    const std::int64_t lo = toInt64Strict(args[0]);
    const std::int64_t hi = toInt64Strict(args[1]);
    if (lo > hi) throw std::runtime_error("Math.randomInt: minimum must not exceed maximum");
    std::uniform_int_distribution<std::int64_t> dist(lo, hi);
    return dist(mathRng());
}

Value mathRandomFloat(const std::vector<Value>& args) {
    const double lo = toDouble(args[0]);
    const double hi = toDouble(args[1]);
    if (lo > hi) throw std::runtime_error("Math.randomFloat: minimum must not exceed maximum");
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(mathRng());
}

// --- IO ---
// (log(...) remains the dedicated statement-level print - these are library
// forms that additionally support no-newline output and reading input, which
// a statement-only `log` couldn't do.)

Value ioPrint(const std::vector<Value>& args) {
    std::cout << valueToString(args[0]);
    return Value{};
}

Value ioPrintln(const std::vector<Value>& args) {
    std::cout << valueToString(args[0]) << std::endl;
    return Value{};
}

Value ioReadLine(const std::vector<Value>& /*args*/) {
    std::string line;
    if (!std::getline(std::cin, line)) return Value{}; // EOF -> nil
    return line;
}

Value ioClear(const std::vector<Value>& /*args*/) {
#ifdef _WIN32
    // Enable virtual-terminal processing when the host console supports it,
    // then use the same ANSI sequence as Unix-like terminals. If the handle
    // is redirected or the mode cannot be changed, still emit the sequence;
    // redirected consumers may intentionally interpret it themselves.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && out != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode)) {
            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
                DWORD updated = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(out, updated);
            }
        }
    }
#endif
    std::cout << "\x1b[2J\x1b[H" << std::flush;
    return Value{};
}

// --- Collection (lists, arrays, sets share ListRef; maps use MapRef) ---

ListRef requireList(const Value& v, const char* fnName) {
    if (const auto* p = std::get_if<ListRef>(&v); p && *p) return *p;
    throwTypeError(std::string(fnName) + " expects a list/array/set as its first argument");
}

MapRef requireMap(const Value& v, const char* fnName) {
    if (const auto* p = std::get_if<MapRef>(&v); p && *p) return *p;
    throwTypeError(std::string(fnName) + " expects a map as its first argument");
}

std::size_t requireIndex(const Value& v, std::size_t size, const char* fnName) {
    std::int64_t i = toInt64Strict(v);
    if (i < 0 || static_cast<std::size_t>(i) >= size) {
        throwIndexError(std::string(fnName) + ": index " + std::to_string(i) +
                        " out of bounds (size " + std::to_string(size) + ")");
    }
    return static_cast<std::size_t>(i);
}

std::size_t listLogicalSize(const ListRef& list) {
    return list->items.size() - std::min(list->frontIndex, list->items.size());
}

void normalizeListFront(ListRef list) {
    if (list->frontIndex == 0) return;
    if (list->frontIndex >= list->items.size()) {
        list->items.clear();
        list->frontIndex = 0;
        return;
    }
    if (list->frontIndex * 2 >= list->items.size()) {
        list->items.erase(list->items.begin(),
                          list->items.begin() + static_cast<std::ptrdiff_t>(list->frontIndex));
        list->frontIndex = 0;
    }
}

Value collNewList(const std::vector<Value>&) { return makeEmptyList(); }

Value collPush(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Collection.push");
    RuntimeTypeCheck types(nativeChunk());
    types.listWrite(list, &args[1], listLogicalSize(list) + 1);
    if (list->frontIndex != 0) {
        // A list with a queue front offset is still a valid ListRef, but
        // Collection.push should preserve normal list semantics. Compact the
        // consumed prefix before exposing the new element.
        list->items.erase(list->items.begin(),
                          list->items.begin() + static_cast<std::ptrdiff_t>(list->frontIndex));
        list->frontIndex = 0;
    }
    list->items.push_back(args[1]);
    types.commit();
    return Value{};
}

Value collPop(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Collection.pop");
    RuntimeTypeCheck types(nativeChunk());
    if (listLogicalSize(list) == 0) throw std::runtime_error("Collection.pop: cannot pop from an empty list");
    types.listWrite(list, nullptr, listLogicalSize(list) - 1);
    Value back = std::move(list->items.back());
    list->items.pop_back();
    normalizeListFront(list);
    types.commit();
    return back;
}

Value collGet(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto list = requireList(args[0], "Collection.get");
    const std::size_t size = listLogicalSize(list);
    const std::size_t index = requireIndex(args[1], size, "Collection.get");
    return list->items[list->frontIndex + index];
}

Value collSet(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Collection.set");
    Value replacement = args[2];
    RuntimeTypeCheck types(nativeChunk());
    const std::size_t size = listLogicalSize(list);
    const std::size_t index = requireIndex(args[1], size, "Collection.set");
    types.listWrite(list, &replacement, size);
    std::swap(list->items[list->frontIndex + index], replacement);
    types.commit();
    return Value{};
}

Value collLength(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    if (const auto* p = std::get_if<ListRef>(&args[0]); p && *p) return static_cast<std::int64_t>(listLogicalSize(*p));
    if (const auto* p = std::get_if<MapRef>(&args[0]); p && *p) return static_cast<std::int64_t>((*p)->size());
    if (auto p = std::get_if<std::string>(&args[0])) return static_cast<std::int64_t>(p->size());
    throwTypeError("Collection.length expects a list/array/set, map, or string");
}

// --- Queue / Stack (zl.util) - both are a plain ListRef under the hood,
// same box Collection's own lists use; only the push/pop END differs
// (front for Queue's FIFO order, back for Stack's LIFO order, matching
// Collection.push/pop's existing back-of-list convention). No new Value
// variant, no VM changes - purely a compile-time-gated pair of natives
// (see NativeSignature::homePackage in type_checker.cpp). ---

Value queueNew(const std::vector<Value>&) { return makeEmptyList(); }

Value queueEnqueue(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Queue.enqueue");
    RuntimeTypeCheck types(nativeChunk());
    types.listWrite(list, &args[1], listLogicalSize(list) + 1);
    list->items.push_back(args[1]);
    types.commit();
    return Value{};
}

Value queueDequeue(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Queue.dequeue");
    RuntimeTypeCheck types(nativeChunk());
    if (listLogicalSize(list) == 0) throw std::runtime_error("Queue.dequeue: cannot dequeue from an empty queue");
    types.listWrite(list, nullptr, listLogicalSize(list) - 1);
    Value front = std::move(list->items[list->frontIndex++]);
    normalizeListFront(list);
    types.commit();
    return front;
}

Value queuePeek(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto list = requireList(args[0], "Queue.peek");
    if (listLogicalSize(list) == 0) throw std::runtime_error("Queue.peek: cannot peek an empty queue");
    return list->items[list->frontIndex];
}

Value queueIsEmpty(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    return listLogicalSize(requireList(args[0], "Queue.isEmpty")) == 0;
}

Value queueSize(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    return static_cast<std::int64_t>(listLogicalSize(requireList(args[0], "Queue.size")));
}

Value stackNew(const std::vector<Value>&) { return makeEmptyList(); }

Value stackPush(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Stack.push");
    RuntimeTypeCheck types(nativeChunk());
    types.listWrite(list, &args[1], listLogicalSize(list) + 1);
    normalizeListFront(list);
    list->items.push_back(args[1]);
    types.commit();
    return Value{};
}

Value stackPop(const std::vector<Value>& args) {
    auto list = requireList(args[0], "Stack.pop");
    RuntimeTypeCheck types(nativeChunk());
    if (listLogicalSize(list) == 0) throw std::runtime_error("Stack.pop: cannot pop from an empty stack");
    types.listWrite(list, nullptr, listLogicalSize(list) - 1);
    Value back = std::move(list->items.back());
    list->items.pop_back();
    normalizeListFront(list);
    types.commit();
    return back;
}

Value stackPeek(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto list = requireList(args[0], "Stack.peek");
    if (listLogicalSize(list) == 0) throw std::runtime_error("Stack.peek: cannot peek an empty stack");
    return list->items.back();
}

Value stackIsEmpty(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    return listLogicalSize(requireList(args[0], "Stack.isEmpty")) == 0;
}

Value stackSize(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    return static_cast<std::int64_t>(listLogicalSize(requireList(args[0], "Stack.size")));
}

Value collNewMap(const std::vector<Value>&) { return makeEmptyMap(); }

Value collMapSet(const std::vector<Value>& args) {
    auto map = requireMap(args[0], "Collection.mapSet");
    Value replacement = args[2];
    RuntimeTypeCheck types(nativeChunk());
    types.mapWrite(map, args[1], replacement);
    map->setEntry(args[1], std::move(replacement));
    types.commit();
    return Value{};
}

Value collMapGet(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto map = requireMap(args[0], "Collection.mapGet");
    if (auto found = map->tryGetEntry(args[1])) return std::move(*found);
    throwKeyError("Collection.mapGet: key not found");
}

Value collMapHas(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto map = requireMap(args[0], "Collection.mapHas");
    return map->findEntry(args[1]) != MapBox::kNoEntry;
}

Value collMapRemove(const std::vector<Value>& args) {
    auto map = requireMap(args[0], "Collection.mapRemove");
    RuntimeTypeCheck access(nativeChunk());
    map->removeEntry(args[1]);
    return Value{};
}

Value collMapKeys(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto map = requireMap(args[0], "Collection.mapKeys");
    auto out = std::get<ListRef>(makeEmptyList());
    const auto snapshot = map->snapshotEntries();
    out->items.reserve(snapshot.size());
    for (const auto& entry : snapshot) out->items.push_back(entry.first);
    if (map->storageType) out->storageType = std::make_shared<const NativeContainerType>(NativeContainerType{{map->storageType->arguments[0]}, {}});
    return out;
}

Value collMapValues(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto map = requireMap(args[0], "Collection.mapValues");
    auto out = std::get<ListRef>(makeEmptyList());
    const auto snapshot = map->snapshotEntries();
    out->items.reserve(snapshot.size());
    for (const auto& entry : snapshot) out->items.push_back(entry.second);
    if (map->storageType) out->storageType = std::make_shared<const NativeContainerType>(NativeContainerType{{map->storageType->arguments[1]}, {}});
    return out;
}

Value collNewSet(const std::vector<Value>&) { return makeEmptyList(); } // sets reuse the list representation

Value collSetAdd(const std::vector<Value>& args) {
    auto set = requireList(args[0], "Collection.setAdd");
    RuntimeTypeCheck types(nativeChunk());
    types.listWrite(set, &args[1], listLogicalSize(set));
    for (std::size_t i = set->frontIndex; i < set->items.size(); ++i) {
        if (valuesEqual(set->items[i], args[1])) { types.commit(); return Value{}; }
    }
    types.listWrite(set, nullptr, listLogicalSize(set) + 1);
    set->items.push_back(args[1]);
    types.commit();
    return Value{};
}

Value collSetHas(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto set = requireList(args[0], "Collection.setHas");
    for (std::size_t i = set->frontIndex; i < set->items.size(); ++i) {
        if (valuesEqual(set->items[i], args[1])) return true;
    }
    return false;
}

Value collSetRemove(const std::vector<Value>& args) {
    auto set = requireList(args[0], "Collection.setRemove");
    Value removed;
    RuntimeTypeCheck types(nativeChunk());
    for (std::size_t i = set->frontIndex; i < set->items.size(); ++i) {
        if (valuesEqual(set->items[i], args[1])) {
            types.listWrite(set, nullptr, listLogicalSize(set) - 1);
            removed = std::move(set->items[i]);
            set->items.erase(set->items.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    types.commit();
    return Value{};
}

Value collSetItems(const std::vector<Value>& args) {
    RuntimeTypeCheck access(nativeChunk());
    auto set = requireList(args[0], "Collection.setItems");
    auto out = std::get<ListRef>(makeEmptyList());
    out->items.reserve(set->items.size());
    for (std::size_t i = set->frontIndex; i < set->items.size(); ++i) out->items.push_back(set->items[i]);
    if (set->storageType) out->storageType = std::make_shared<const NativeContainerType>(NativeContainerType{set->storageType->arguments, {}});
    return out;
}

// --- String ---

// UTF-8 scalar helpers used by String.utf8* / Text.*. Ill-formed sequences
// are treated as a single-byte character so indexing never hangs or splits a
// well-formed multi-byte scalar. String.length/charAt remain byte operations.
struct Utf8Char {
    std::string bytes;
    std::uint32_t cp;
    std::size_t next;
};

Utf8Char utf8Next(const std::string& s, std::size_t i) {
    const auto u = static_cast<unsigned char>(s[i]);
    auto one = [&]() { return Utf8Char{s.substr(i, 1), u, i + 1}; };
    if (u < 0x80) return {s.substr(i, 1), u, i + 1};
    int need = 0;
    std::uint32_t cp = 0;
    if ((u & 0xE0) == 0xC0) { need = 2; cp = u & 0x1F; }
    else if ((u & 0xF0) == 0xE0) { need = 3; cp = u & 0x0F; }
    else if ((u & 0xF8) == 0xF0) { need = 4; cp = u & 0x07; }
    else return one();
    if (i + static_cast<std::size_t>(need) > s.size()) return one();
    for (int k = 1; k < need; ++k) {
        const auto c = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((c & 0xC0) != 0x80) return one();
        cp = (cp << 6) | (c & 0x3F);
    }
    const std::uint32_t minCp = need == 2 ? 0x80u : need == 3 ? 0x800u : 0x10000u;
    if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return one();
    return {s.substr(i, static_cast<std::size_t>(need)), cp, i + static_cast<std::size_t>(need)};
}

std::size_t utf8LengthOf(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); i = utf8Next(s, i).next) ++n;
    return n;
}

Utf8Char utf8At(const std::string& s, std::int64_t index, const char* fnName) {
    if (index < 0) {
        throwIndexError(std::string(fnName) + ": index " + std::to_string(index) + " out of bounds");
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        auto ch = utf8Next(s, i);
        if (static_cast<std::int64_t>(n) == index) return ch;
        i = ch.next;
        ++n;
    }
    throwIndexError(std::string(fnName) + ": index " + std::to_string(index) +
                    " out of bounds (size " + std::to_string(n) + ")");
}

std::string utf8Encode(std::uint32_t cp) {
    std::string out;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

const std::string& requireString(const Value& v, const char* fnName) {
    if (auto p = std::get_if<std::string>(&v)) return *p;
    throwTypeError(std::string(fnName) + " expects a string argument");
}

Value strLength(const std::vector<Value>& args) {
    return static_cast<std::int64_t>(requireString(args[0], "String.length").size());
}

Value strUpper(const std::vector<Value>& args) {
    std::string s = requireString(args[0], "String.upper");
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

Value strLower(const std::vector<Value>& args) {
    std::string s = requireString(args[0], "String.lower");
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Value strTrim(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.trim");
    std::size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string("");
    std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

Value strContains(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.contains");
    const std::string& needle = requireString(args[1], "String.contains");
    return s.find(needle) != std::string::npos;
}

Value strStartsWith(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.startsWith");
    const std::string& prefix = requireString(args[1], "String.startsWith");
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

Value strEndsWith(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.endsWith");
    const std::string& suffix = requireString(args[1], "String.endsWith");
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Value strIndexOf(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.indexOf");
    const std::string& needle = requireString(args[1], "String.indexOf");
    auto pos = s.find(needle);
    return static_cast<std::int64_t>(pos == std::string::npos ? -1 : static_cast<std::int64_t>(pos));
}

Value strCharAt(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.charAt");
    std::size_t i = requireIndex(args[1], s.size(), "String.charAt");
    return std::string(1, s[i]);
}

Value strSubstring(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.substring");
    std::int64_t start = toInt64Strict(args[1]);
    std::int64_t end = toInt64Strict(args[2]);
    if (start < 0 || end < start || static_cast<std::size_t>(end) > s.size()) {
        throw std::runtime_error("String.substring: invalid range [" + std::to_string(start) +
                                  ", " + std::to_string(end) + ") for a string of length " +
                                  std::to_string(s.size()));
    }
    return s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
}

Value strReplace(const std::vector<Value>& args) {
    std::string s = requireString(args[0], "String.replace");
    const std::string& from = requireString(args[1], "String.replace");
    const std::string& to = requireString(args[2], "String.replace");
    if (from.empty()) return s;
    std::string result;
    std::size_t pos = 0;
    while (true) {
        std::size_t found = s.find(from, pos);
        if (found == std::string::npos) { result += s.substr(pos); break; }
        result += s.substr(pos, found - pos) + to;
        pos = found + from.size();
    }
    return result;
}

Value strSplit(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.split");
    const std::string& delim = requireString(args[1], "String.split");
    Value result = makeEmptyList();
    auto& items = std::get<ListRef>(result)->items;
    if (delim.empty()) {
        // An empty separator means "every character", and a character is a UTF-8
        // scalar, not a byte: splitting "héllo" on bytes returned six fragments
        // with two mojibake halves of 'é' in them. Same scalar walk as
        // String.utf8CharAt, so the two agree on what a character is.
        for (std::size_t i = 0; i < s.size(); ) {
            const Utf8Char ch = utf8Next(s, i);
            items.emplace_back(ch.bytes);
            i = ch.next;
        }
        return result;
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t found = s.find(delim, pos);
        if (found == std::string::npos) { items.emplace_back(s.substr(pos)); break; }
        items.emplace_back(s.substr(pos, found - pos));
        pos = found + delim.size();
    }
    return result;
}

std::int64_t parseIntStrict(const std::string& s, const char* functionName) {
    try {
        std::size_t pos = 0;
        std::int64_t result = static_cast<std::int64_t>(std::stoll(s, &pos));
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) throw std::invalid_argument("trailing characters");
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(functionName) + ": \"" + s + "\" isn't a valid integer");
    }
}

double parseDoubleStrict(const std::string& s, const char* functionName) {
    try {
        std::size_t pos = 0;
        double result = std::stod(s, &pos);
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) throw std::invalid_argument("trailing characters");
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(functionName) + ": \"" + s + "\" isn't a valid number");
    }
}

Value strToInt(const std::vector<Value>& args) {
    return parseIntStrict(requireString(args[0], "String.toInt"), "String.toInt");
}

Value strToFloat(const std::vector<Value>& args) {
    return parseDoubleStrict(requireString(args[0], "String.toFloat"), "String.toFloat");
}

Value strLastIndexOf(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.lastIndexOf");
    const std::string& needle = requireString(args[1], "String.lastIndexOf");
    const auto pos = s.rfind(needle);
    return pos == std::string::npos ? std::int64_t{-1} : static_cast<std::int64_t>(pos);
}

Value strIndexOfFrom(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.indexOfFrom");
    const std::string& needle = requireString(args[1], "String.indexOfFrom");
    std::int64_t start = toInt64Strict(args[2]);
    if (start < 0) start = 0;
    if (static_cast<std::size_t>(start) > s.size()) return std::int64_t{-1};
    const auto pos = s.find(needle, static_cast<std::size_t>(start));
    return pos == std::string::npos ? std::int64_t{-1} : static_cast<std::int64_t>(pos);
}

Value strTrimStart(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.trimStart");
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    return s.substr(begin);
}

Value strTrimEnd(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.trimEnd");
    std::size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(0, end);
}

namespace {
std::int64_t lexicalCompare(const std::string& a, const std::string& b) {
    const int result = a.compare(b);
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
}
std::string asciiLower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
} // namespace

// -1 / 0 / 1, matching the ordering contract used by List.sort comparators.
Value strCompare(const std::vector<Value>& args) {
    return lexicalCompare(requireString(args[0], "String.compare"), requireString(args[1], "String.compare"));
}

// ASCII case folding only; ZL does not yet carry a Unicode case table.
Value strCompareIgnoreCase(const std::vector<Value>& args) {
    return lexicalCompare(asciiLower(requireString(args[0], "String.compareIgnoreCase")),
                          asciiLower(requireString(args[1], "String.compareIgnoreCase")));
}

// Byte value at `index`; ZL strings are byte strings, so this is the
// honest unit rather than a pretended Unicode code point.
Value strCodePointAt(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.codePointAt");
    const std::size_t index = requireIndex(args[1], s.size(), "String.codePointAt");
    return static_cast<std::int64_t>(static_cast<unsigned char>(s[index]));
}

Value strFromCodePoint(const std::vector<Value>& args) {
    const std::int64_t code = toInt64Strict(args[0]);
    if (code < 0 || code > 255)
        throwIndexError("String.fromCodePoint: value " + std::to_string(code) + " out of the 0..255 byte range");
    return std::string(1, static_cast<char>(code));
}

Value strRepeat(const std::vector<Value>& args) {
    // Named String.repeatText (not String.repeat): `repeat` is the do-while
    // loop keyword and cannot appear after a dot.
    const std::string& s = requireString(args[0], "String.repeatText");
    const std::int64_t count = toInt64Strict(args[1]);
    if (count < 0) throw std::runtime_error("String.repeatText: count must be non-negative");
    if (count == 0 || s.empty()) return std::string("");
    const auto total = static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(s.size());
    if (total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error("String.repeatText: result exceeds the maximum string size");
    std::string out;
    out.reserve(static_cast<std::size_t>(total));
    for (std::int64_t i = 0; i < count; ++i) out.append(s);
    return out;
}

Value strUtf8Length(const std::vector<Value>& args) {
    return static_cast<std::int64_t>(utf8LengthOf(requireString(args[0], "String.utf8Length")));
}

Value strUtf8CharAt(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8CharAt");
    return utf8At(s, toInt64Strict(args[1]), "String.utf8CharAt").bytes;
}

Value strUtf8Substring(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8Substring");
    const std::int64_t start = toInt64Strict(args[1]);
    const std::int64_t end = toInt64Strict(args[2]);
    const auto n = static_cast<std::int64_t>(utf8LengthOf(s));
    if (start < 0 || end < start || end > n) {
        throw std::runtime_error("String.utf8Substring: invalid range [" + std::to_string(start) +
                                  ", " + std::to_string(end) + ") for a string of length " +
                                  std::to_string(n));
    }
    std::string out;
    std::int64_t idx = 0;
    for (std::size_t i = 0; i < s.size() && idx < end; ) {
        auto ch = utf8Next(s, i);
        if (idx >= start) out += ch.bytes;
        i = ch.next;
        ++idx;
    }
    return out;
}

Value strUtf8Reverse(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8Reverse");
    std::vector<std::string> chars;
    for (std::size_t i = 0; i < s.size(); ) {
        auto ch = utf8Next(s, i);
        chars.push_back(std::move(ch.bytes));
        i = ch.next;
    }
    std::string out;
    out.reserve(s.size());
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) out += *it;
    return out;
}

Value strUtf8CodePointAt(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8CodePointAt");
    return static_cast<std::int64_t>(utf8At(s, toInt64Strict(args[1]), "String.utf8CodePointAt").cp);
}

Value strUtf8FromCodePoint(const std::vector<Value>& args) {
    const std::int64_t code = toInt64Strict(args[0]);
    if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        throwIndexError("String.utf8FromCodePoint: value " + std::to_string(code) +
                        " is not a Unicode scalar value");
    }
    return utf8Encode(static_cast<std::uint32_t>(code));
}

Value strUtf8ByteIndex(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8ByteIndex");
    const std::int64_t index = toInt64Strict(args[1]);
    const auto n = static_cast<std::int64_t>(utf8LengthOf(s));
    if (index < 0 || index > n) {
        throwIndexError("String.utf8ByteIndex: index " + std::to_string(index) +
                        " out of bounds (size " + std::to_string(n) + ")");
    }
    if (index == n) return static_cast<std::int64_t>(s.size());
    std::int64_t idx = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        if (idx == index) return static_cast<std::int64_t>(i);
        i = utf8Next(s, i).next;
        ++idx;
    }
    return static_cast<std::int64_t>(s.size());
}

Value strUtf8IndexFromByte(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "String.utf8IndexFromByte");
    const std::int64_t byteIndex = toInt64Strict(args[1]);
    if (byteIndex < 0 || static_cast<std::size_t>(byteIndex) > s.size()) {
        throwIndexError("String.utf8IndexFromByte: byte index " + std::to_string(byteIndex) +
                        " out of bounds (size " + std::to_string(s.size()) + ")");
    }
    const auto target = static_cast<std::size_t>(byteIndex);
    std::int64_t idx = 0;
    for (std::size_t i = 0; i < s.size() && i < target; ) {
        i = utf8Next(s, i).next;
        ++idx;
    }
    return idx;
}


// --- Parsing utilities ---
// Int.parse, Double.parse, Bool.parse - the official ZL way to convert
// strings into typed values. These deliberately throw on invalid input
// (catchable with try/catch) rather than returning nil, since a failed
// parse is almost always a programming error or bad user input that
// should be handled explicitly.

Value intParse(const std::vector<Value>& args) {
    return parseIntStrict(requireString(args[0], "Int.parse"), "Int.parse");
}

Value doubleParse(const std::vector<Value>& args) {
    return parseDoubleStrict(requireString(args[0], "Double.parse"), "Double.parse");
}

Value boolParse(const std::vector<Value>& args) {
    const std::string& s = requireString(args[0], "Bool.parse");
    if (s == "true" || s == "1" || s == "yes") return true;
    if (s == "false" || s == "0" || s == "no") return false;
    throw std::runtime_error("Bool.parse: \"" + s + "\" is not a valid boolean (expected true/false/1/0/yes/no)");
}

// --- FileSystem ---
// Every failure here just throws std::runtime_error - it propagates through
// the VM's per-instruction try/catch exactly like a built-in error (division
// by zero, bad index, ...), so zl's `try`/`catch` handles file errors for
// free with no special-casing needed.

Value fsReadFile(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.readFile");
    std::ifstream file(path, std::ios::binary);
    if (!file) throwIOError("FileSystem.readFile: could not open \"" + path + "\"");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

Value fsWriteFile(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.writeFile");
    const std::string& content = requireString(args[1], "FileSystem.writeFile");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throwIOError("FileSystem.writeFile: could not open \"" + path + "\" for writing");
    file << content;
    return Value{};
}

Value fsAppendFile(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.appendFile");
    const std::string& content = requireString(args[1], "FileSystem.appendFile");
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) throwIOError("FileSystem.appendFile: could not open \"" + path + "\" for writing");
    file << content;
    return Value{};
}

Value fsExists(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.exists");
    return std::filesystem::exists(path);
}

Value fsDeleteFile(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.deleteFile");
    std::error_code ec;
    bool removed = std::filesystem::remove(path, ec);
    if (ec || !removed) throwIOError("FileSystem.deleteFile: could not delete \"" + path + "\"");
    return Value{};
}

namespace {
// Pins a freshly built list as list<string>. Natives whose catalog signature
// declares an element type must pin the same type at runtime, so the static
// declaration and the actual storage contract cannot drift apart.
void pinStringList(const Value& list) {
    std::get<ListRef>(list)->storageType = std::make_shared<const NativeContainerType>(
        NativeContainerType{{TypeName{"string", {}, {}, {}}}, {}});
}
} // namespace

Value fsListDir(const std::vector<Value>& args) {
    const std::string& path = requireString(args[0], "FileSystem.listDir");
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
        throwIOError("FileSystem.listDir: \"" + path + "\" is not a directory");
    }
    Value result = makeEmptyList();
    auto& items = std::get<ListRef>(result)->items;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        items.emplace_back(entry.path().filename().string());
    }
    pinStringList(result);
    return result;
}


// --- FileSystem: path, metadata and directory operations ---------------
// std::filesystem is the portability boundary; every failure surfaces as an
// IOError so ZL code can catch it by type.

namespace {
std::filesystem::path fsPath(const Value& value, const char* fnName) {
    return std::filesystem::path(requireString(value, fnName));
}
} // namespace

Value fsRename(const std::vector<Value>& args) {
    const auto from = fsPath(args[0], "FileSystem.rename");
    const auto to = fsPath(args[1], "FileSystem.rename");
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) throwIOError("FileSystem.rename: " + ec.message());
    return Value{};
}

Value fsCopyFile(const std::vector<Value>& args) {
    const auto from = fsPath(args[0], "FileSystem.copyFile");
    const auto to = fsPath(args[1], "FileSystem.copyFile");
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throwIOError("FileSystem.copyFile: " + ec.message());
    return Value{};
}

Value fsSize(const std::vector<Value>& args) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(fsPath(args[0], "FileSystem.size"), ec);
    if (ec) throwIOError("FileSystem.size: " + ec.message());
    return static_cast<std::int64_t>(size);
}

Value fsIsFile(const std::vector<Value>& args) {
    std::error_code ec;
    return std::filesystem::is_regular_file(fsPath(args[0], "FileSystem.isFile"), ec) && !ec;
}

Value fsIsDirectory(const std::vector<Value>& args) {
    std::error_code ec;
    return std::filesystem::is_directory(fsPath(args[0], "FileSystem.isDirectory"), ec) && !ec;
}

// Creates every missing parent as well; succeeding on an existing directory
// keeps the operation idempotent.
Value fsCreateDir(const std::vector<Value>& args) {
    const auto path = fsPath(args[0], "FileSystem.createDir");
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) throwIOError("FileSystem.createDir: " + ec.message());
    if (!std::filesystem::is_directory(path, ec))
        throwIOError("FileSystem.createDir: \"" + path.string() + "\" is not a directory");
    return Value{};
}

// Recursive removal; returns the number of entries deleted.
Value fsRemoveDir(const std::vector<Value>& args) {
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(fsPath(args[0], "FileSystem.removeDir"), ec);
    if (ec) throwIOError("FileSystem.removeDir: " + ec.message());
    return static_cast<std::int64_t>(removed);
}

// Seconds since the Unix epoch, matching Time.now()'s unit.
Value fsModifiedTime(const std::vector<Value>& args) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(fsPath(args[0], "FileSystem.modifiedTime"), ec);
    if (ec) throwIOError("FileSystem.modifiedTime: " + ec.message());
    const auto since = stamp.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since).count();
    // file_clock's epoch is unspecified; normalize against the system clock.
    const auto fileNow = std::chrono::duration_cast<std::chrono::seconds>(
        std::filesystem::file_time_type::clock::now().time_since_epoch()).count();
    const auto systemNow = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::int64_t>(seconds + (systemNow - fileNow));
}

Value fsAbsolutePath(const std::vector<Value>& args) {
    std::error_code ec;
    const auto path = std::filesystem::absolute(fsPath(args[0], "FileSystem.absolutePath"), ec);
    if (ec) throwIOError("FileSystem.absolutePath: " + ec.message());
    return path.lexically_normal().string();
}

Value fsParentPath(const std::vector<Value>& args) {
    return fsPath(args[0], "FileSystem.parentPath").parent_path().string();
}

Value fsFileName(const std::vector<Value>& args) {
    return fsPath(args[0], "FileSystem.fileName").filename().string();
}

Value fsExtension(const std::vector<Value>& args) {
    return fsPath(args[0], "FileSystem.extension").extension().string();
}

Value fsStem(const std::vector<Value>& args) {
    return fsPath(args[0], "FileSystem.stem").stem().string();
}

Value fsJoinPath(const std::vector<Value>& args) {
    auto base = fsPath(args[0], "FileSystem.joinPath");
    base /= requireString(args[1], "FileSystem.joinPath");
    return base.lexically_normal().string();
}

Value fsCurrentDir(const std::vector<Value>&) {
    std::error_code ec;
    const auto path = std::filesystem::current_path(ec);
    if (ec) throwIOError("FileSystem.currentDir: " + ec.message());
    return path.string();
}

Value fsTempDir(const std::vector<Value>&) {
    std::error_code ec;
    const auto path = std::filesystem::temp_directory_path(ec);
    if (ec) throwIOError("FileSystem.tempDir: " + ec.message());
    return path.string();
}


Value networkResolve(const std::vector<Value>& args) {
    const std::string& host = requireString(args[0], "Network.resolve");
#ifdef _WIN32
    static std::once_flag winsockOnce;
    std::call_once(winsockOnce, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Network.resolve: WSAStartup failed");
        }
    });
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* result = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (rc != 0) {
#ifdef _WIN32
        throw std::runtime_error("Network.resolve: " + host + ": " + std::to_string(WSAGetLastError()));
#else
        throw std::runtime_error("Network.resolve: " + host + ": " + gai_strerror(rc));
#endif
    }
    Value out = makeEmptyList();
    auto& items = std::get<ListRef>(out)->items;
    for (addrinfo* p = result; p; p = p->ai_next) {
        char numeric[NI_MAXHOST]{};
        if (getnameinfo(p->ai_addr, static_cast<socklen_t>(p->ai_addrlen),
                        numeric, sizeof(numeric), nullptr, 0, NI_NUMERICHOST) == 0) {
            std::string value(numeric);
            bool duplicate = false;
            for (const auto& existing : items) {
                if (std::holds_alternative<std::string>(existing) && std::get<std::string>(existing) == value) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) items.emplace_back(std::move(value));
        }
    }
    if (items.empty()) {
        freeaddrinfo(result);
        throw std::runtime_error("Network.resolve: host resolved without a numeric address: " + host);
    }
    std::string first = std::get<std::string>(items.front());
    freeaddrinfo(result);
    return first;
}

namespace {
// Shared resolver core: Network.resolve returns the first address,
// Network.resolveAll returns every distinct numeric address.
ListRef resolveHostAddresses(const std::string& host, const char* fnName) {
#ifdef _WIN32
    static std::once_flag winsockOnce;
    std::call_once(winsockOnce, [&] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error(std::string(fnName) + ": WSAStartup failed");
        }
    });
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* result = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (rc != 0) {
#ifdef _WIN32
        throw std::runtime_error(std::string(fnName) + ": " + host + ": " + std::to_string(WSAGetLastError()));
#else
        throw std::runtime_error(std::string(fnName) + ": " + host + ": " + gai_strerror(rc));
#endif
    }
    Value out = makeEmptyList();
    auto list = std::get<ListRef>(out);
    for (addrinfo* p = result; p; p = p->ai_next) {
        char numeric[NI_MAXHOST]{};
        if (getnameinfo(p->ai_addr, static_cast<socklen_t>(p->ai_addrlen),
                        numeric, sizeof(numeric), nullptr, 0, NI_NUMERICHOST) != 0) continue;
        std::string value(numeric);
        const bool duplicate = std::any_of(list->items.begin(), list->items.end(), [&](const Value& existing) {
            return std::holds_alternative<std::string>(existing) && std::get<std::string>(existing) == value;
        });
        if (!duplicate) list->items.emplace_back(std::move(value));
    }
    freeaddrinfo(result);
    if (list->items.empty())
        throw std::runtime_error(std::string(fnName) + ": host resolved without a numeric address: " + host);
    pinStringList(out);
    return list;
}
} // namespace

Value networkResolveAll(const std::vector<Value>& args) {
    return resolveHostAddresses(requireString(args[0], "Network.resolveAll"), "Network.resolveAll");
}

Value networkHostname(const std::vector<Value>&) {
    char buffer[256]{};
#ifdef _WIN32
    static std::once_flag winsockOnce;
    std::call_once(winsockOnce, [] { WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data); });
#endif
    if (gethostname(buffer, sizeof(buffer) - 1) != 0)
        throw std::runtime_error("Network.hostname: could not read the local host name");
    return std::string(buffer);
}

// Literal-only check (no DNS traffic), covering both IPv4 and IPv6 forms.
Value networkIsValidIp(const std::vector<Value>& args) {
    const std::string& text = requireString(args[0], "Network.isValidIp");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* result = nullptr;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &result) != 0) return false;
    freeaddrinfo(result);
    return true;
}


// --- Time ---

std::tm toLocalTm(std::int64_t epochSeconds) {
    std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &t);   // Windows secure CRT signature: (tm*, const time_t*)
#else
    localtime_r(&t, &result);   // POSIX signature: (const time_t*, tm*)
#endif
    return result;
}

Value timeNow(const std::vector<Value>&) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::int64_t>(secs);
}

Value timeNowMillis(const std::vector<Value>&) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::int64_t>(ms);
}

Value timeSleepAsync(const std::vector<Value>& args) {
    const double milliseconds = toDouble(args[0]);
    if (milliseconds < 0.0) {
        throw std::runtime_error("Time.sleepAsync: duration cannot be negative");
    }

    auto task = std::make_shared<RuntimeTaskState>("void");
    RuntimeTaskExecutor::instance().enqueue([task, milliseconds]() {
        try {
            if (task->status() == TaskStatus::Cancelled) return;
            try {
                task->start();
            } catch (const std::logic_error&) {
                return;
            }

            const auto total = std::chrono::duration<double, std::milli>(milliseconds);
            auto remaining = total;
            constexpr auto quantum = std::chrono::milliseconds(5);
            while (remaining > decltype(remaining)::zero()) {
                if (task->cancellationRequested()) {
                    try { task->cancel(); } catch (const std::logic_error&) {}
                    return;
                }
                const auto slice = std::min(remaining, std::chrono::duration<double, std::milli>(quantum));
                std::this_thread::sleep_for(slice);
                remaining -= slice;
            }

            if (task->cancellationRequested()) {
                try { task->cancel(); } catch (const std::logic_error&) {}
                return;
            }
            try { task->succeed(Value{}); } catch (const std::logic_error&) {}
        } catch (...) {
            try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
        }
    });

    return TaskRef(task);
}


// Values that may cross a thread boundary. `Shared<T>` is the explicit opt-in
// wrapper; the rest are the runtime's own synchronisation primitives, whose
// state is guarded internally, so sharing the handle is the intended use. The
// compile-time check in TypeChecker::validateThreadLambda keeps the same list.
bool isThreadSafeClassName(const std::string& className) {
    return className == "Shared" || className == "Atomic" || className == "Mutex" ||
           className == "RwLock" || className == "Semaphore" || className == "Channel" ||
           className == "Condition";
}

bool isExplicitlySharedValue(const Value& value) {
    const auto* object = std::get_if<ObjectRef>(&value);
    return object && *object && isThreadSafeClassName((*object)->className);
}

bool capturesAreExplicitlyShared(const ClosureBox& closure) {
    for (const auto& [name, value] : closure.captured) {
        (void)name;
        if (!isExplicitlySharedValue(value)) return false;
    }
    return true;
}

template <typename Ptr, typename Factory>
Ptr ensureObjectState(ObjectBox& object, Ptr ObjectBox::*member, Factory&& factory) {
    std::lock_guard<std::mutex> initLock(object.stateInitMutex);
    auto& state = object.*member;
    if (!state) state = factory();
    return state;
}

Value mutexWithLock(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    auto closure = std::get_if<ClosureRef>(&args[1]);
    if (!obj || !*obj || (*obj)->className != "Mutex")
        throw std::runtime_error("Mutex.withLock: expected a Mutex");
    auto mutexState = ensureObjectState(*(*obj), &ObjectBox::mutexState, [] { return std::make_shared<std::mutex>(); });
    if (!closure || !*closure || !(*closure)->chunk || !(*closure)->paramNames.empty())
        throw std::runtime_error("Mutex.withLock: expected a zero-argument func");
    if (!g_currentNativeVm) throw std::runtime_error("Mutex.withLock: no active VM");
    std::unique_lock<std::mutex> lock(*mutexState, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    return g_currentNativeVm->invokeTaskClosure(*closure);
}

Value rwLockWithRead(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    auto closure = std::get_if<ClosureRef>(&args[1]);
    if (!obj || !*obj || (*obj)->className != "RwLock")
        throw std::runtime_error("RwLock.withRead: expected a RwLock");
    auto rwLockState = ensureObjectState(*(*obj), &ObjectBox::rwLockState, [] { return std::make_shared<std::shared_mutex>(); });
    if (!closure || !*closure || !(*closure)->chunk || !(*closure)->paramNames.empty())
        throw std::runtime_error("RwLock.withRead: expected a zero-argument func");
    if (!g_currentNativeVm) throw std::runtime_error("RwLock.withRead: no active VM");
    std::shared_lock<std::shared_mutex> lock(*rwLockState, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    return g_currentNativeVm->invokeTaskClosure(*closure);
}

Value rwLockWithWrite(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    auto closure = std::get_if<ClosureRef>(&args[1]);
    if (!obj || !*obj || (*obj)->className != "RwLock")
        throw std::runtime_error("RwLock.withWrite: expected a RwLock");
    auto rwLockState = ensureObjectState(*(*obj), &ObjectBox::rwLockState, [] { return std::make_shared<std::shared_mutex>(); });
    if (!closure || !*closure || !(*closure)->chunk || !(*closure)->paramNames.empty())
        throw std::runtime_error("RwLock.withWrite: expected a zero-argument func");
    if (!g_currentNativeVm) throw std::runtime_error("RwLock.withWrite: no active VM");
    std::unique_lock<std::shared_mutex> lock(*rwLockState, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    return g_currentNativeVm->invokeTaskClosure(*closure);
}


Value atomicLoad(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.load: expected an Atomic");
    auto atomicState = ensureObjectState(*(*obj), &ObjectBox::atomicState, [] { return std::make_shared<std::atomic<std::int64_t>>(0); });
    return atomicState->load(std::memory_order_seq_cst);
}

Value atomicStore(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.store: expected an Atomic");
    if (!std::holds_alternative<std::int64_t>(args[1]))
        throw std::runtime_error("Atomic.store: expected an int");
    auto atomicState = ensureObjectState(*(*obj), &ObjectBox::atomicState, [] { return std::make_shared<std::atomic<std::int64_t>>(0); });
    atomicState->store(std::get<std::int64_t>(args[1]), std::memory_order_seq_cst);
    return Value{};
}

Value atomicAdd(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.add: expected an Atomic");
    if (!std::holds_alternative<std::int64_t>(args[1]))
        throw std::runtime_error("Atomic.add: expected an int");
    auto atomicState = ensureObjectState(*(*obj), &ObjectBox::atomicState, [] { return std::make_shared<std::atomic<std::int64_t>>(0); });
    return atomicState->fetch_add(std::get<std::int64_t>(args[1]), std::memory_order_seq_cst) + std::get<std::int64_t>(args[1]);
}

Value atomicLoadBool(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.loadBool: expected an Atomic");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicBoolState, [] { return std::make_shared<std::atomic<bool>>(false); });
    return state->load(std::memory_order_seq_cst);
}

Value atomicStoreBool(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.storeBool: expected an Atomic");
    if (!std::holds_alternative<bool>(args[1]))
        throw std::runtime_error("Atomic.storeBool: expected a bool");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicBoolState, [] { return std::make_shared<std::atomic<bool>>(false); });
    state->store(std::get<bool>(args[1]), std::memory_order_seq_cst);
    return Value{};
}

Value atomicLoadDouble(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic") throw std::runtime_error("Atomic.loadDouble: expected an Atomic");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicDoubleState, [] { return std::make_shared<std::atomic<double>>(0.0); });
    return state->load(std::memory_order_seq_cst);
}

Value atomicStoreDouble(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic") throw std::runtime_error("Atomic.storeDouble: expected an Atomic");
    if (!std::holds_alternative<double>(args[1])) throw std::runtime_error("Atomic.storeDouble: expected a double");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicDoubleState, [] { return std::make_shared<std::atomic<double>>(0.0); });
    state->store(std::get<double>(args[1]), std::memory_order_seq_cst);
    return Value{};
}

Value atomicLoadRef(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.loadRef: expected an Atomic");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicRefState, [] { return std::make_shared<AtomicRefState>(); });
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->value;
}

Value atomicStoreRef(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Atomic")
        throw std::runtime_error("Atomic.storeRef: expected an Atomic");
    if (!std::holds_alternative<ObjectRef>(args[1]) && !std::holds_alternative<std::monostate>(args[1]))
        throw std::runtime_error("Atomic.storeRef: expected an object reference or null");
    auto state = ensureObjectState(*(*obj), &ObjectBox::atomicRefState, [] { return std::make_shared<AtomicRefState>(); });
    std::lock_guard<std::mutex> lock(state->mutex);
    state->value = args[1];
    return Value{};
}

Value semaphoreAcquire(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.acquire: expected a Semaphore");
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    VM::BlockingNativeCall blocked(g_currentNativeVm);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&] { return state->permits > 0; });
    --state->permits;
    return Value{};
}

Value semaphoreRelease(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.release: expected a Semaphore");
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->permits;
    }
    state->cv.notify_one();
    return Value{};
}

Value semaphoreAvailable(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.available: expected a Semaphore");
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->permits;
}

// Sets the permit count outright. A Semaphore starts at zero permits, so
// without this every acquire() would block forever and the primitive would be
// unusable as a resource counter.
Value semaphoreSetPermits(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.setPermits: expected a Semaphore");
    const std::int64_t permits = toInt64Strict(args[1]);
    if (permits < 0) throw std::runtime_error("Semaphore.setPermits: permit count cannot be negative");
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->permits = permits;
    }
    // Raising the count can satisfy several waiters at once.
    state->cv.notify_all();
    return Value{};
}

// Non-blocking acquire: takes a permit and returns true, or returns false
// immediately when none is available.
Value semaphoreTryAcquire(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.tryAcquire: expected a Semaphore");
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->permits <= 0) return false;
    --state->permits;
    return true;
}

Value semaphoreReleaseMany(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Semaphore")
        throw std::runtime_error("Semaphore.releaseMany: expected a Semaphore");
    const std::int64_t count = toInt64Strict(args[1]);
    if (count < 0) throw std::runtime_error("Semaphore.releaseMany: count cannot be negative");
    if (count == 0) return Value{};
    auto state = ensureObjectState(*(*obj), &ObjectBox::semaphoreState, [] { return std::make_shared<ObjectBox::SemaphoreState>(); });
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->permits += count;
    }
    state->cv.notify_all();
    return Value{};
}

Value conditionWait(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Condition")
        throw std::runtime_error("Condition.wait: expected a Condition");
    auto state = ensureObjectState(*(*obj), &ObjectBox::conditionState, [] { return std::make_shared<ObjectBox::ConditionState>(); });
    VM::BlockingNativeCall blocked(g_currentNativeVm);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock);
    return Value{};
}

// Bounded wait. Returns true when notified within `seconds`, false on
// timeout. Condition.wait() has no timeout and no predicate, so a notify that
// arrives before the wait begins is missed and the waiter hangs; this gives
// callers a way to bound that exposure.
Value conditionWaitFor(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Condition")
        throw std::runtime_error("Condition.waitFor: expected a Condition");
    const double seconds = toDouble(args[1]);
    if (seconds < 0) throw std::runtime_error("Condition.waitFor: timeout cannot be negative");
    auto state = ensureObjectState(*(*obj), &ObjectBox::conditionState, [] { return std::make_shared<ObjectBox::ConditionState>(); });
    VM::BlockingNativeCall blocked(g_currentNativeVm);
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto duration = std::chrono::duration<double>(seconds);
    return state->cv.wait_for(lock, std::chrono::duration_cast<std::chrono::nanoseconds>(duration)) ==
           std::cv_status::no_timeout;
}

Value conditionNotifyOne(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Condition")
        throw std::runtime_error("Condition.notifyOne: expected a Condition");
    auto state = ensureObjectState(*(*obj), &ObjectBox::conditionState, [] { return std::make_shared<ObjectBox::ConditionState>(); });
    state->cv.notify_one();
    return Value{};
}

Value conditionNotifyAll(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Condition")
        throw std::runtime_error("Condition.notifyAll: expected a Condition");
    auto state = ensureObjectState(*(*obj), &ObjectBox::conditionState, [] { return std::make_shared<ObjectBox::ConditionState>(); });
    state->cv.notify_all();
    return Value{};
}

Value channelCreate(const std::vector<Value>& args) {
    if (!std::holds_alternative<std::int64_t>(args[0]))
        throw std::runtime_error("Channel.create: expected capacity int");
    const auto cap = static_cast<std::int64_t>(std::get<std::int64_t>(args[0]));
    if (cap <= 0) throw std::runtime_error("Channel.create: capacity must be greater than zero");
    auto obj = makeGCObject();
    obj->className = "Channel";
    obj->channelState = std::make_shared<ObjectBox::ChannelState>();
    obj->channelState->capacity = static_cast<std::size_t>(cap);
    return obj;
}

namespace {
// True when a blocking channel operation on THIS thread certainly cannot ever
// be unblocked: no ZL worker thread exists, no CPU-pool task is queued or
// running, no async frame is queued on the scheduler, and the caller is not
// itself a worker (main may still make progress). The only remaining runnable
// entity is the thread about to block, so waiting would be a hang - report it
// as a deadlock instead. Re-evaluated every round of the pump-while-blocked
// loop below: a pending frame that finishes without touching the channel must
// not buy the wait an eternity.
bool channelBlockWouldDeadlock() {
    if (gIsWorkerThread) return false;
    if (gAliveWorkerThreads.load(std::memory_order_acquire) > 0) return false;
    if (g_currentNativeVm && g_currentNativeVm->schedulerPendingCount() > 0) return false;
    return true;
}
} // namespace

Value channelSend(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Channel" || !(*obj)->channelState)
        throw std::runtime_error("Channel.send: expected a Channel");
    auto state = (*obj)->channelState;
    VM* vm = g_currentNativeVm;
    VM::BlockingNativeCall blocked(vm);
    std::unique_lock<std::mutex> lock(state->mutex);
    while (true) {
        TaskRef receiverTask;
        while (!state->pendingReceives.empty()) {
            auto candidate = state->pendingReceives.front().task;
            state->pendingReceives.pop_front();
            if (candidate && !candidate->isTerminal()) {
                receiverTask = std::move(candidate);
                break;
            }
        }
        if (receiverTask) {
            // Completed after releasing the channel mutex: the receiver's
            // continuations may synchronously re-enter this channel.
            Value payload = args[1];
            lock.unlock();
            try {
                receiverTask->start();
                receiverTask->succeed(std::move(payload));
            } catch (const std::logic_error&) {
                // Cancellation won the race after the queue handoff.
            }
            state->cvNotEmpty.notify_one();
            return Value{};
        }
        if (state->items.size() < state->capacity) break;
        if (channelBlockWouldDeadlock()) {
            throw std::runtime_error(
                "Channel.send: deadlock - the channel is full and no other thread or pending task can receive");
        }
        // An async receiver may be queued on this thread's scheduler behind
        // the very call that is about to block. Pump one ready frame - with
        // the channel lock released, since the pumped frame needs it - rather
        // than sleeping through work only this thread can run. When no frame
        // is ready, wait briefly so another thread's receiver can arrive; the
        // condition is re-checked under the lock every round, so a notify
        // that lands mid-pump is observed, never lost.
        lock.unlock();
        const bool pumped = vm && vm->pumpSchedulerOne();
        lock.lock();
        if (!pumped && state->items.size() >= state->capacity) {
            state->cvNotFull.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return state->items.size() < state->capacity || !state->pendingReceives.empty();
            });
        }
    }
    state->items.push_back(args[1]);
    lock.unlock();
    state->cvNotEmpty.notify_one();
    return Value{};
}

Value channelReceive(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Channel" || !(*obj)->channelState)
        throw std::runtime_error("Channel.receive: expected a Channel");
    auto state = (*obj)->channelState;
    VM* vm = g_currentNativeVm;
    VM::BlockingNativeCall blocked(vm);
    std::unique_lock<std::mutex> lock(state->mutex);
    TaskRef senderTask;
    std::optional<Value> handoff;
    while (!handoff && state->items.empty()) {
        while (!handoff && !state->pendingSends.empty()) {
            auto sender = std::move(state->pendingSends.front());
            state->pendingSends.pop_front();
            if (sender.task && !sender.task->isTerminal()) {
                senderTask = std::move(sender.task);
                handoff = std::move(sender.value);
            }
        }
        if (handoff) break;
        if (channelBlockWouldDeadlock()) {
            throw std::runtime_error(
                "Channel.receive: deadlock - the channel is empty and no other thread or pending task can send");
        }
        // An async sender may be queued on this thread's scheduler behind the
        // very call that is about to block. Pump one ready frame - with the
        // channel lock released, since the pumped frame needs it - rather
        // than sleeping through work only this thread can run. When no frame
        // is ready, wait briefly so another thread's sender can arrive; the
        // condition is re-checked under the lock every round, so a notify
        // that lands mid-pump is observed, never lost.
        lock.unlock();
        const bool pumped = vm && vm->pumpSchedulerOne();
        lock.lock();
        if (!pumped && state->items.empty() && !handoff) {
            state->cvNotEmpty.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return !state->items.empty() || !state->pendingSends.empty();
            });
        }
    }
    if (handoff) {
        // Completed after releasing the channel mutex: the sender's
        // continuations may synchronously re-enter this channel.
        lock.unlock();
        try {
            senderTask->start();
            senderTask->succeed(Value{});
        } catch (const std::logic_error&) {
            // Cancellation won the race after the queue handoff.
        }
        state->cvNotFull.notify_one();
        return std::move(*handoff);
    }
    Value value = std::move(state->items.front());
    state->items.pop_front();
    lock.unlock();
    state->cvNotFull.notify_one();
    return value;
}

Value channelSize(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Channel" || !(*obj)->channelState)
        throw std::runtime_error("Channel.size: expected a Channel");
    auto state = (*obj)->channelState;
    std::lock_guard<std::mutex> lock(state->mutex);
    return static_cast<std::int64_t>(state->items.size());
}


Value channelSendAsync(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Channel" || !(*obj)->channelState)
        throw std::runtime_error("Channel.sendAsync: expected a Channel");
    auto task = std::make_shared<RuntimeTaskState>("void", std::vector<Value>{args[0]});
    const Value value = args[1];
    std::shared_ptr<ObjectBox::ChannelState> state = (*obj)->channelState;

    std::optional<Value> delivered;
    TaskRef receiverTask;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // Discard cancelled/terminal receive waiters before matching a sender.
        while (!state->pendingReceives.empty()) {
            auto candidate = state->pendingReceives.front().task;
            state->pendingReceives.pop_front();
            if (candidate && !candidate->isTerminal()) {
                receiverTask = std::move(candidate);
                delivered = value;
                break;
            }
        }
        if (!receiverTask) {
            if (state->items.size() < state->capacity) {
                state->items.push_back(value);
            } else {
                state->pendingSends.push_back({value, task});
                std::weak_ptr<ObjectBox::ChannelState> weakState = state;
                std::weak_ptr<RuntimeTaskState> weakTask = task;
                task->onCancellation([weakState, weakTask]() {
                    auto lockedState = weakState.lock();
                    auto lockedTask = weakTask.lock();
                    if (!lockedState || !lockedTask) return;
                    std::lock_guard<std::mutex> lock(lockedState->mutex);
                    auto it = std::remove_if(lockedState->pendingSends.begin(), lockedState->pendingSends.end(),
                        [&](const auto& pending) { return pending.task == lockedTask; });
                    lockedState->pendingSends.erase(it, lockedState->pendingSends.end());
                });
                state->cvNotEmpty.notify_one();
                return TaskRef(task);
            }
        }
    }

    task->start();
    task->succeed(Value{});
    if (receiverTask && delivered) {
        try {
            receiverTask->start();
            receiverTask->succeed(std::move(*delivered));
        } catch (const std::logic_error&) {
            // The receiver may have been cancelled concurrently between the
            // queue handoff and its terminal transition. Cancellation wins.
        }
    }
    state->cvNotEmpty.notify_one();
    return TaskRef(task);
}

Value channelReceiveAsync(const std::vector<Value>& args) {
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Channel" || !(*obj)->channelState)
        throw std::runtime_error("Channel.receiveAsync: expected a Channel");
    auto task = std::make_shared<RuntimeTaskState>("", std::vector<Value>{args[0]});
    std::shared_ptr<ObjectBox::ChannelState> state = (*obj)->channelState;
    std::optional<Value> immediate;
    TaskRef senderTask;
    bool senderMatched = false;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->items.empty()) {
            immediate = std::move(state->items.front());
            state->items.pop_front();
        } else {
            while (!state->pendingSends.empty()) {
                auto sender = state->pendingSends.front();
                state->pendingSends.pop_front();
                if (sender.task && !sender.task->isTerminal()) {
                    senderTask = std::move(sender.task);
                    immediate = std::move(sender.value);
                    senderMatched = true;
                    break;
                }
            }
            if (!immediate) {
                state->pendingReceives.push_back({task});
                std::weak_ptr<ObjectBox::ChannelState> weakState = state;
                std::weak_ptr<RuntimeTaskState> weakTask = task;
                task->onCancellation([weakState, weakTask]() {
                    auto lockedState = weakState.lock();
                    auto lockedTask = weakTask.lock();
                    if (!lockedState || !lockedTask) return;
                    std::lock_guard<std::mutex> lock(lockedState->mutex);
                    auto it = std::remove_if(lockedState->pendingReceives.begin(), lockedState->pendingReceives.end(),
                        [&](const auto& pending) { return pending.task == lockedTask; });
                    lockedState->pendingReceives.erase(it, lockedState->pendingReceives.end());
                });
                state->cvNotFull.notify_one();
                return TaskRef(task);
            }
        }
    }

    // Never complete the matched sender while holding the channel mutex: its
    // continuations may synchronously re-enter this channel.
    if (senderMatched && senderTask) {
        try {
            senderTask->start();
            senderTask->succeed(Value{});
        } catch (const std::logic_error&) {
            // Cancellation may have won the race after the queue handoff.
        }
    }

    task->start();
    task->succeed(std::move(*immediate));
    state->cvNotFull.notify_one();
    return TaskRef(task);
}

Value taskSpawn(const std::vector<Value>& args) {
    auto closure = std::get_if<ClosureRef>(&args[0]);
    if (!closure || !(*closure)) throw std::runtime_error("Task.spawn: expected a func value");
    const auto& c = **closure;
    if (!c.chunk) throw std::runtime_error("Task.spawn: function value has no executable chunk");
    if (c.functionIndex >= c.chunk->functions.size()) throw std::runtime_error("Task.spawn: invalid function value");
    const auto& fn = c.chunk->functions[c.functionIndex];
    if (fn.isAsync) throw std::runtime_error("Task.spawn: expected a synchronous func");
    if (!c.paramNames.empty()) throw std::runtime_error("Task.spawn: CPU task entry func must take no parameters");
    if (!capturesAreExplicitlyShared(c)) throw std::runtime_error("Task.spawn: captured values must be Shared<T> when crossing CPU workers");

    auto task = std::make_shared<RuntimeTaskState>(c.returnTypeName.empty() ? "void" : c.returnTypeName);
    // A CPU task spawned from a task body joins that task's cancellation
    // cascade (a spawn from an ordinary synchronous frame has no parent).
    if (RuntimeTaskState* parent = gCurrentSpawningTask) parent->addSpawnedChild(task);
    const auto closureCopy = *closure;
    auto root = std::make_shared<ProtectedGCRoot>(closureCopy.get());
    RuntimeTaskExecutor::instance().enqueue([task, closureCopy, root]() mutable {
        std::unique_ptr<VM> workerVm;
        try {
            // Cooperative cancellation: a spawn cancelled before its worker
            // started never runs the closure at all. (A Pending task cancelled
            // outright is already terminal and skipped for the same reason.)
            if (task->isTerminal() || task->cancellationRequested()) {
                if (!task->isTerminal()) {
                    try { task->cancel(); } catch (const std::logic_error&) {}
                }
                return;
            }
            workerVm = std::make_unique<VM>();
            task->start();
            Value result;
            {
                // The closure body may itself spawn tasks; they belong to this
                // task's cancellation cascade, so mark it as the spawning task.
                CurrentSpawningTaskGuard spawnGuard{task.get()};
                result = workerVm->invokeTaskClosure(closureCopy);
            }
            if (task->cancellationRequested()) {
                // Cancelled while running: a synchronous closure has no
                // suspension point to notice at, so its completion settles as
                // cancelled rather than delivering a result nobody will read.
                try { task->cancel(); } catch (const std::logic_error&) {}
            } else {
                task->succeed(std::move(result));
            }
        } catch (...) {
            try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
        }
    });
    return TaskRef(task);
}

Value threadStart(const std::vector<Value>& args) {
    auto closure = std::get_if<ClosureRef>(&args[0]);
    if (!closure || !(*closure)) throw std::runtime_error("Thread.start: expected a func value");
    const auto& c = **closure;
    if (!c.chunk) throw std::runtime_error("Thread.start: function value has no executable chunk");
    if (c.functionIndex >= c.chunk->functions.size()) throw std::runtime_error("Thread.start: invalid function value");
    if (c.chunk->functions[c.functionIndex].isAsync) throw std::runtime_error("Thread.start: async func values must use Task/await instead");
    if (!c.paramNames.empty()) throw std::runtime_error("Thread.start: thread entry func must take no parameters");
    if (!capturesAreExplicitlyShared(c)) throw std::runtime_error("Thread.start: captured values must be Shared<T> when crossing raw threads");
    if (!g_currentNativeVm) throw std::runtime_error("Thread.start: no active VM");

    auto state = std::make_shared<RuntimeThreadState>();
    const auto closureCopy = *closure;
    auto root = std::make_shared<ProtectedGCRoot>(closureCopy.get());
    state->startWith([closureCopy, root]() mutable {
        VM workerVm;
        // An exception escaping here is captured by startWith and rethrown by
        // Thread.join, so a worker failure is reported to whoever joins rather
        // than terminating the process.
        (void)workerVm.invokeTaskClosure(closureCopy);
    });
    return Value{std::move(state)};
}

Value threadJoin(const std::vector<Value>& args) {
    auto thread = std::get_if<ThreadRef>(&args[0]);
    if (!thread || !(*thread)) throw std::runtime_error("Thread.join: expected a Thread value");
    VM::BlockingNativeCall blocked(g_currentNativeVm);
    (*thread)->join();
    return Value{};
}

Value threadIsAlive(const std::vector<Value>& args) {
    auto thread = std::get_if<ThreadRef>(&args[0]);
    if (!thread || !(*thread)) throw std::runtime_error("Thread.isAlive: expected a Thread value");
    return (*thread)->isAlive();
}

Value timeSleep(const std::vector<Value>& args) {
    double seconds = toDouble(args[0]);
    if (seconds > 0.0) {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
    return Value{};
}

Value timeYear(const std::vector<Value>& args)   { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_year + 1900); }
Value timeMonth(const std::vector<Value>& args)  { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_mon + 1); }
Value timeDay(const std::vector<Value>& args)    { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_mday); }
Value timeHour(const std::vector<Value>& args)   { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_hour); }
Value timeMinute(const std::vector<Value>& args) { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_min); }
Value timeSecond(const std::vector<Value>& args) { return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_sec); }

namespace {
// Shared token substitution for Time.format / Time.utcFormat. Deliberately a
// small readable token set rather than raw strftime.
std::string formatCivilTime(const std::tm& parts, std::string pattern) {
    const auto replaceAll = [&](const std::string& token, int value, int width) {
        std::ostringstream oss;
        oss << std::setw(width) << std::setfill('0') << value;
        const std::string text = oss.str();
        std::size_t pos = 0;
        while ((pos = pattern.find(token, pos)) != std::string::npos) {
            pattern.replace(pos, token.size(), text);
            pos += text.size();
        }
    };
    replaceAll("YYYY", parts.tm_year + 1900, 4);
    replaceAll("MM", parts.tm_mon + 1, 2);
    replaceAll("DD", parts.tm_mday, 2);
    replaceAll("HH", parts.tm_hour, 2);
    replaceAll("mm", parts.tm_min, 2);
    replaceAll("ss", parts.tm_sec, 2);
    // P2-2: previously an unrecognised token was left in place, so a strftime
    // pattern such as "%Y-%m-%d" was returned unchanged and printed a wrong date
    // confidently. Now an unknown %-token raises instead of failing silently.
    if (pattern.find('%') != std::string::npos) {
        throw std::runtime_error(
            "Time.format: unknown token '%...' in pattern \"" + pattern +
            "\" - expected YYYY, MM, DD, HH, mm, ss (e.g. \"YYYY-MM-DD\")");
    }
    return pattern;
}

std::tm toUtcTm(std::int64_t epochSeconds) {
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    return result;
}
} // namespace

// Time.format(timestamp, "YYYY-MM-DD HH:mm:ss") in local time.
Value timeFormat(const std::vector<Value>& args) {
    return formatCivilTime(toLocalTm(toInt64Strict(args[0])), requireString(args[1], "Time.format"));
}

// Same pattern language as Time.format, evaluated in UTC.
Value timeUtcFormat(const std::vector<Value>& args) {
    return formatCivilTime(toUtcTm(toInt64Strict(args[0])), requireString(args[1], "Time.utcFormat"));
}

// Monotonic milliseconds: safe for measuring elapsed time because it is
// unaffected by wall-clock adjustments.
Value timeMonotonicMillis(const std::vector<Value>&) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// 0 = Sunday .. 6 = Saturday, matching struct tm.
Value timeDayOfWeek(const std::vector<Value>& args) {
    return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_wday);
}

// 1-based day of the year.
Value timeDayOfYear(const std::vector<Value>& args) {
    return static_cast<std::int64_t>(toLocalTm(toInt64Strict(args[0])).tm_yday + 1);
}

Value timeIsLeapYear(const std::vector<Value>& args) {
    const std::int64_t year = toInt64Strict(args[0]);
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

// Local-time civil parts to epoch seconds. Out-of-range fields normalize the
// same way mktime does, so 2024-01-32 is 2024-02-01.
Value timeFromParts(const std::vector<Value>& args) {
    std::tm parts{};
    parts.tm_year = static_cast<int>(toInt64Strict(args[0])) - 1900;
    parts.tm_mon = static_cast<int>(toInt64Strict(args[1])) - 1;
    parts.tm_mday = static_cast<int>(toInt64Strict(args[2]));
    parts.tm_hour = static_cast<int>(toInt64Strict(args[3]));
    parts.tm_min = static_cast<int>(toInt64Strict(args[4]));
    parts.tm_sec = static_cast<int>(toInt64Strict(args[5]));
    parts.tm_isdst = -1;
    const std::time_t result = std::mktime(&parts);
    if (result == static_cast<std::time_t>(-1))
        throw std::runtime_error("Time.fromParts: the given date/time is not representable");
    return static_cast<std::int64_t>(result);
}


// --- Type reflection -------------------------------------------------------

Value typeName(const std::vector<Value>& args) {
    if (std::holds_alternative<ObjectRef>(args[0])) {
        auto obj = std::get<ObjectRef>(args[0]);
        return (obj && obj->runtimeType) ? Value{obj->runtimeType->name} : Value{};
    }
    if (std::holds_alternative<ListRef>(args[0])) return std::string("list");
    if (std::holds_alternative<MapRef>(args[0])) return std::string("map");
    if (std::holds_alternative<ClosureRef>(args[0])) return std::string("func");
    if (std::holds_alternative<std::int64_t>(args[0])) return std::string("int");
    if (std::holds_alternative<double>(args[0])) return std::string("double");
    if (std::holds_alternative<std::string>(args[0])) return std::string("string");
    if (std::holds_alternative<bool>(args[0])) return std::string("bool");
    return std::string("nil");
}

Value typeKind(const std::vector<Value>& args) {
    const Value& value = args.at(0);
    if (std::holds_alternative<std::monostate>(value)) return std::string("nil");
    if (std::holds_alternative<std::int64_t>(value)) return std::string("int");
    if (std::holds_alternative<double>(value)) return std::string("double");
    if (std::holds_alternative<std::string>(value)) return std::string("string");
    if (std::holds_alternative<bool>(value)) return std::string("bool");
    if (std::holds_alternative<ListRef>(value)) return std::string("list");
    if (std::holds_alternative<MapRef>(value)) return std::string("map");
    if (std::holds_alternative<ClosureRef>(value)) return std::string("function");
    if (std::holds_alternative<TaskRef>(value)) return std::string("task");
    if (std::holds_alternative<ThreadRef>(value)) return std::string("thread");
    if (auto obj = std::get_if<ObjectRef>(&value)) {
        if (!*obj) return std::string("object");
        if ((*obj)->className == "Type") return std::string("type");
        return ((*obj)->runtimeType && (*obj)->runtimeType->isDataType) ? std::string("data") :
               ((*obj)->runtimeType && (*obj)->runtimeType->isEnumType) ? std::string("enum") : std::string("object");
    }
    return std::string("object");
}

Value typeIsData(const std::vector<Value>& args) {
    const Value& value = args.at(0);
    if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj && (*obj)->runtimeType) return (*obj)->runtimeType->isDataType;
    return false;
}

Value typeIsEnum(const std::vector<Value>& args) {
    const Value& value = args.at(0);
    if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj && (*obj)->runtimeType) return (*obj)->runtimeType->isEnumType;
    return false;
}

Value typeEnumMembers(const std::vector<Value>& args) {
    const Value& value = args.at(0);
    auto out = makeGCList();
    if (auto obj = std::get_if<ObjectRef>(&value); obj && *obj && (*obj)->runtimeType) {
        for (const auto& member : (*obj)->runtimeType->enumMembers) out->items.emplace_back(member);
    }
    return ListRef(std::move(out));
}

Value typeFields(const std::vector<Value>& args) {
    if (!std::holds_alternative<ObjectRef>(args[0]))
        throw std::runtime_error("Reflection.fields: expected an object");
    auto obj = std::get<ObjectRef>(args[0]);
    auto out = makeGCList();
    if (obj && obj->runtimeType) for (const auto& field : obj->runtimeType->fields) {
        auto ref = makeGCObject();
        ref->className = "Field";
        objectFieldAccess(*ref, "name") = field.name;
        objectFieldAccess(*ref, "type") = field.typeName;
        objectFieldAccess(*ref, "access") = field.access;
        out->items.emplace_back(ObjectRef(std::move(ref)));
    }
    return out;
}

Value typeMethods(const std::vector<Value>& args) {
    if (!std::holds_alternative<ObjectRef>(args[0]))
        throw std::runtime_error("Reflection.methods: expected an object");
    auto obj = std::get<ObjectRef>(args[0]);
    auto out = makeGCList();
    if (obj && obj->runtimeType) for (const auto& method : obj->runtimeType->methods) {
        auto ref = makeGCObject();
        ref->className = "Method";
        objectFieldAccess(*ref, "name") = method.name;
        objectFieldAccess(*ref, "returnType") = method.returnType;
        objectFieldAccess(*ref, "access") = method.access;
        objectFieldAccess(*ref, "static") = method.isStatic;
        objectFieldAccess(*ref, "async") = method.isAsync;
        auto params = makeGCList();
        for (const auto& type : method.parameterTypes) params->items.emplace_back(type);
        objectFieldAccess(*ref, "parameters") = ListRef(std::move(params));
        out->items.emplace_back(ObjectRef(std::move(ref)));
    }
    return out;
}

Value typeBase(const std::vector<Value>& args) {
    if (!std::holds_alternative<ObjectRef>(args[0]))
        throw std::runtime_error("Reflection.base: expected an object");
    auto obj = std::get<ObjectRef>(args[0]);
    if (!obj || !obj->runtimeType || obj->runtimeType->baseClassName.empty()) return Value{};
    return obj->runtimeType->baseClassName;
}

// --- Reflection wrappers -------------------------------------------------

// Phase 0 measurement surface (memory-domains.md §9/§10): the process-wide
// collector counters as a map. Keys are stable - the allocation benchmark
// (benchmarks/) parses them - and every value is an int:
//   allocations   boxes that entered the registry (fresh + recycled)
//   reused        boxes taken from the recycled-box free list
//   collections   collect() runs so far
//   collectMs     cumulative trace+sweep time, whole milliseconds
//   tracked       boxes live right now
//   peakTracked   high-water mark of the live heap
//   threshold     allocation count that triggers the next collection
Value gcStats(const std::vector<Value>& args) {
    if (!args.empty()) throw std::runtime_error("GC.stats: expected no arguments");
    const auto c = TracingGC::instance().counters();
    auto out = makeGCMap();
    out->setEntry(Value(std::string("allocations")), Value(static_cast<std::int64_t>(c.allocations)));
    out->setEntry(Value(std::string("reused")), Value(static_cast<std::int64_t>(c.reused)));
    out->setEntry(Value(std::string("collections")), Value(static_cast<std::int64_t>(c.collections)));
    out->setEntry(Value(std::string("collectMs")), Value(static_cast<std::int64_t>(c.collectNs / 1000000)));
    out->setEntry(Value(std::string("tracked")), Value(static_cast<std::int64_t>(c.tracked)));
    out->setEntry(Value(std::string("peakTracked")), Value(static_cast<std::int64_t>(c.peakTracked)));
    out->setEntry(Value(std::string("threshold")), Value(static_cast<std::int64_t>(c.threshold)));
    return Value(out);
}

Value sharedShare(const std::vector<Value>& args) {
    if (args.size() != 1) throw std::runtime_error("share: expected one value");
    auto box = makeGCObject();
    // Runtime dispatch uses the canonical Shared class; the compiler retains
    // Shared<T> at the static type level for get()/setValue() typing.
    box->className = "Shared";
    objectFieldAccess(*box, "__value") = args[0];
    return Value{ObjectRef(std::move(box))};
}

Value sharedGet(const std::vector<Value>& args) {
    if (args.size() != 1) throw std::runtime_error("Shared.__get: expected one value");
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Shared")
        throw std::runtime_error("Shared.__get: expected a Shared value");
    auto mutex = ensureObjectState(*(*obj), &ObjectBox::sharedValueMutex, [] { return std::make_shared<std::recursive_mutex>(); });
    std::unique_lock<std::recursive_mutex> lock(*mutex, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    const Value* it = objectFieldLookup(**obj, "__value");
    if (it == nullptr) throw std::runtime_error("Shared.__get: invalid Shared value");
    return *it;
}

Value sharedWithLock(const std::vector<Value>& args) {
    if (args.size() != 2) throw std::runtime_error("Shared.__withLock: expected Shared value and callback");
    auto obj = std::get_if<ObjectRef>(&args[0]);
    auto closure = std::get_if<ClosureRef>(&args[1]);
    if (!obj || !*obj || (*obj)->className != "Shared")
        throw std::runtime_error("Shared.__withLock: expected a Shared value");
    if (!closure || !*closure || !(*closure)->chunk || !(*closure)->paramNames.empty())
        throw std::runtime_error("Shared.__withLock: expected a zero-argument func");
    if (!g_currentNativeVm) throw std::runtime_error("Shared.__withLock: no active VM");
    auto mutex = ensureObjectState(*(*obj), &ObjectBox::sharedValueMutex, [] { return std::make_shared<std::recursive_mutex>(); });
    std::unique_lock<std::recursive_mutex> lock(*mutex, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    return g_currentNativeVm->invokeTaskClosure(*closure);
}

Value sharedSet(const std::vector<Value>& args) {
    if (args.size() != 2) throw std::runtime_error("Shared.__set: expected two values");
    auto obj = std::get_if<ObjectRef>(&args[0]);
    if (!obj || !*obj || (*obj)->className != "Shared")
        throw std::runtime_error("Shared.__set: expected a Shared value");
    auto mutex = ensureObjectState(*(*obj), &ObjectBox::sharedValueMutex, [] { return std::make_shared<std::recursive_mutex>(); });
    std::unique_lock<std::recursive_mutex> lock(*mutex, std::defer_lock);
    {
        VM::BlockingNativeCall blocked(g_currentNativeVm);
        lock.lock();
    }
    if (!nativeChunk()) throw std::runtime_error("Shared.__set: no active VM");
    Value replacement = args[1];
    {
        RuntimeTypeCheck types(nativeChunk());
        types.require(replacement, runtimeFieldType(*nativeChunk(), (*obj)->className, "__value", obj->get()));
        std::swap(objectFieldAccess(**obj, "__value"), replacement);
        types.commit();
    }
    lock.unlock();
    return Value{};
}

Value typeOf(const std::vector<Value>& args) {
    if (args.size() != 1) throw std::runtime_error("Type.of: expected one argument");
    auto box = makeGCObject();
    box->className = "Type";
    objectFieldAccess(*box, "__value") = args[0];
    return ObjectRef(std::move(box));
}


ObjectRef requireTypeObject(const Value& value, const char* name) {
    auto obj = std::get_if<ObjectRef>(&value);
    if (!obj || !(*obj) || (*obj)->className != "Type")
        throw std::runtime_error(std::string(name) + ": expected a Type value");
    return *obj;
}

const Value& reflectedValue(const Value& value, const char* name) {
    auto obj = requireTypeObject(value, name);
    const Value* it = objectFieldLookup(*obj, "__value");
    if (it == nullptr) throw std::runtime_error(std::string(name) + ": invalid Type value");
    return *it;
}

Value reflectionName(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.name");
    return typeName({value});
}

Value reflectionKind(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.kind");
    return typeKind({value});
}

Value reflectionIsData(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.isData");
    return typeIsData({value});
}

Value reflectionIsEnum(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.isEnum");
    return typeIsEnum({value});
}

Value reflectionEnumMembers(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.enumMembers");
    return typeEnumMembers({value});
}

Value reflectionFunction(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.function");
    auto closure = std::get_if<ClosureRef>(&value);
    if (!closure || !(*closure))
        throw std::runtime_error("Reflection.function: expected a function value");
    auto ref = makeGCObject();
    ref->className = "Function";
    objectFieldAccess(*ref, "__closure") = value;
    objectFieldAccess(*ref, "name") = (*closure)->functionName;
    auto params = makeGCList();
    for (const auto& type : (*closure)->parameterTypeNames) params->items.emplace_back(type);
    objectFieldAccess(*ref, "parameters") = ListRef(std::move(params));
    objectFieldAccess(*ref, "returnType") = (*closure)->isAsync
        ? "Task<" + (*closure)->returnTypeName + ">" : (*closure)->returnTypeName;
    objectFieldAccess(*ref, "async") = (*closure)->isAsync;
    objectFieldAccess(*ref, "native") = (*closure)->isNative;
    return ObjectRef(std::move(ref));
}

Value typeCallable(const std::vector<Value>& args) {
    return reflectionFunction(args);
}

Value reflectionFields(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.fields");
    return typeFields({value});
}

Value reflectionMethods(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.methods");
    return typeMethods({value});
}

Value reflectionBase(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.base");
    return typeBase({value});
}

Value reflectionInterfaces(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.interfaces");
    auto obj = std::get_if<ObjectRef>(&value);
    if (!obj || !(*obj) || !(*obj)->runtimeType)
        throw std::runtime_error("Reflection.interfaces: expected an object-backed Type");
    auto out = makeGCList();
    for (const auto& name : (*obj)->runtimeType->interfaces) out->items.emplace_back(name);
    return ListRef(std::move(out));
}

Value reflectionTypeParameters(const std::vector<Value>& args) {
    const Value& value = reflectedValue(args[0], "Reflection.typeParameters");
    auto obj = std::get_if<ObjectRef>(&value);
    if (!obj || !(*obj) || !(*obj)->runtimeType)
        throw std::runtime_error("Reflection.typeParameters: expected an object-backed Type");
    auto out = makeGCList();
    for (const auto& name : (*obj)->runtimeType->typeParameters) out->items.emplace_back(name);
    return ListRef(std::move(out));
}

Value reflectedMember(const Value& value, const char* expectedClass, const char* name) {
    auto obj = std::get_if<ObjectRef>(&value);
    if (!obj || !(*obj) || (*obj)->className != expectedClass)
        throw std::runtime_error(std::string(name) + ": expected a " + expectedClass + " value");
    return value;
}

Value reflectionMemberField(const Value& value, const char* expectedClass, const char* operation, const char* fieldName) {
    auto obj = std::get<ObjectRef>(reflectedMember(value, expectedClass, operation));
    const Value* it = objectFieldLookup(*obj, fieldName);
    if (it == nullptr) throw std::runtime_error(std::string("Reflection.") + operation + ": malformed " + expectedClass + " metadata");
    return *it;
}
Value reflectionFieldName(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Field", "Field.name", "name"); }
Value reflectionFieldType(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Field", "Field.type", "type"); }
Value reflectionFieldAccess(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Field", "Field.access", "access"); }
Value reflectionMethodName(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.name", "name"); }
Value reflectionMethodReturnType(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.returnType", "returnType"); }
Value reflectionMethodAccess(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.access", "access"); }
Value reflectionMethodIsStatic(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.static", "static"); }
Value reflectionMethodIsAsync(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.async", "async"); }
Value reflectionMethodParameters(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Method", "Method.parameters", "parameters"); }
Value reflectionFunctionName(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Function", "Function.name", "name"); }
Value reflectionFunctionParameters(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Function", "Function.parameters", "parameters"); }
Value reflectionFunctionReturnType(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Function", "Function.returnType", "returnType"); }
Value reflectionFunctionIsAsync(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Function", "Function.async", "async"); }
Value reflectionFunctionIsNative(const std::vector<Value>& args) { return reflectionMemberField(args[0], "Function", "Function.native", "native"); }


Value reflectionTypeConstructors(const std::vector<Value>& args) {
    if (args.size() != 1 || !std::holds_alternative<ObjectRef>(args[0])) throw std::runtime_error("Type.constructors: expected a Type value");
    auto typeObj = requireTypeObject(args[0], "Type.constructors");
    const Value* it = objectFieldLookup(*typeObj, "__value");
    if (it == nullptr || !std::holds_alternative<ObjectRef>(*it)) throw std::runtime_error("Type.constructors: invalid Type value");
    auto valueObj = std::get<ObjectRef>(*it);
    auto out = makeGCList();
    if (valueObj && valueObj->runtimeType) {
        for (const auto& ctor : valueObj->runtimeType->constructors) {
            auto ref = makeGCObject();
            ref->className = "Constructor";
            objectFieldAccess(*ref, "__owner") = ctor.ownerClassName;
            objectFieldAccess(*ref, "__functionIndex") = static_cast<std::int64_t>(ctor.functionIndex);
            objectFieldAccess(*ref, "__access") = ctor.access;
            auto params = makeGCList();
            for (const auto& type : ctor.parameterTypes) params->items.emplace_back(type);
            objectFieldAccess(*ref, "parameters") = ListRef(std::move(params));
            out->items.emplace_back(ObjectRef(std::move(ref)));
        }
    }
    return out;
}

Value reflectionField(const std::vector<Value>& args) {
    const Value& typeValue = args[0];
    const Value& fieldNameValue = args[1];
    const std::string& fieldName = requireString(fieldNameValue, "Reflection.field");
    auto typeObj = requireTypeObject(typeValue, "Reflection.field");
    const Value& original = objectFieldRequire(*typeObj, "__value");
    if (!std::holds_alternative<ObjectRef>(original)) throw std::runtime_error("Reflection.field: expected an object-backed Type");
    auto valueObj = std::get<ObjectRef>(original);
    if (!valueObj || !valueObj->runtimeType) throw std::runtime_error("Reflection.field: missing runtime type metadata");
    auto it = std::find_if(valueObj->runtimeType->fields.begin(), valueObj->runtimeType->fields.end(), [&](const auto& f){ return f.name == fieldName; });
    if (it == valueObj->runtimeType->fields.end()) throw std::runtime_error("Reflection.field: field not found: " + fieldName);
    auto ref = makeGCObject(); ref->className = "Field";
    objectFieldAccess(*ref, "name") = it->name; objectFieldAccess(*ref, "type") = it->typeName; objectFieldAccess(*ref, "access") = it->access;
    return ObjectRef(std::move(ref));
}

Value reflectionMethod(const std::vector<Value>& args) {
    const std::string& methodName = requireString(args[1], "Reflection.method");
    auto typeObj = requireTypeObject(args[0], "Reflection.method");
    const Value& original = objectFieldRequire(*typeObj, "__value");
    if (!std::holds_alternative<ObjectRef>(original)) throw std::runtime_error("Reflection.method: expected an object-backed Type");
    auto valueObj = std::get<ObjectRef>(original);
    if (!valueObj || !valueObj->runtimeType) throw std::runtime_error("Reflection.method: missing runtime type metadata");
    const auto matches = std::count_if(valueObj->runtimeType->methods.begin(), valueObj->runtimeType->methods.end(),
                                       [&](const auto& m){ return m.name == methodName; });
    if (matches == 0) throw std::runtime_error("Reflection.method: method not found: " + methodName);
    if (matches > 1) throw std::runtime_error("Reflection.method: method '" + methodName + "' is overloaded; use Type.methods() and select a signature");
    auto it = std::find_if(valueObj->runtimeType->methods.begin(), valueObj->runtimeType->methods.end(), [&](const auto& m){ return m.name == methodName; });
    auto ref = makeGCObject(); ref->className = "Method";
    objectFieldAccess(*ref, "name") = it->name; objectFieldAccess(*ref, "returnType") = it->returnType; objectFieldAccess(*ref, "access") = it->access; objectFieldAccess(*ref, "static") = it->isStatic; objectFieldAccess(*ref, "async") = it->isAsync;
    auto params = makeGCList(); for (const auto& type : it->parameterTypes) params->items.emplace_back(type); objectFieldAccess(*ref, "parameters") = ListRef(std::move(params));
    objectFieldAccess(*ref, "__owner") = it->ownerClassName; objectFieldAccess(*ref, "__functionIndex") = static_cast<std::int64_t>(it->functionIndex);
    return ObjectRef(std::move(ref));
}

Value reflectionConstructor(const std::vector<Value>& args) {
    auto typeObj = requireTypeObject(args[0], "Reflection.constructor");
    if (!std::holds_alternative<std::int64_t>(args[1])) throw std::runtime_error("Reflection.constructor: expected integer index");
    const auto index = static_cast<std::size_t>(std::get<std::int64_t>(args[1]));
    const Value& original = objectFieldRequire(*typeObj, "__value");
    if (!std::holds_alternative<ObjectRef>(original)) throw std::runtime_error("Reflection.constructor: expected an object-backed Type");
    auto valueObj = std::get<ObjectRef>(original);
    if (!valueObj || !valueObj->runtimeType || index >= valueObj->runtimeType->constructors.size()) throw std::runtime_error("Reflection.constructor: index out of range");
    auto ref = makeGCObject(); ref->className = "Constructor";
    const auto& ctor = valueObj->runtimeType->constructors[index];
    objectFieldAccess(*ref, "__owner") = ctor.ownerClassName; objectFieldAccess(*ref, "__functionIndex") = static_cast<std::int64_t>(ctor.functionIndex); objectFieldAccess(*ref, "__access") = ctor.access;
    auto params = makeGCList(); for (const auto& type : ctor.parameterTypes) params->items.emplace_back(type); objectFieldAccess(*ref, "parameters") = ListRef(std::move(params));
    return ObjectRef(std::move(ref));
}

Value reflectionConstructorParameters(const std::vector<Value>& args) {
    auto obj = std::get<ObjectRef>(reflectedMember(args[0], "Constructor", "Constructor.parameters"));
    return objectFieldRequire(*obj, "parameters");
}

// --- Test assertions ------------------------------------------------------

[[noreturn]] void testFailure(const char* name, const std::string& message) {
    throw std::runtime_error(std::string("Test.") + name + " failed: " + message);
}

Value testFail(const std::vector<Value>& args) {
    const std::string message = requireString(args[0], "Test.fail");
    testFailure("fail", message);
}

Value testAssertTrue(const std::vector<Value>& args) {
    if (!isTruthy(args[0])) testFailure("assertTrue", "expected a truthy value");
    return Value{};
}

Value testAssertFalse(const std::vector<Value>& args) {
    if (isTruthy(args[0])) testFailure("assertFalse", "expected a falsy value");
    return Value{};
}

Value testAssertEqual(const std::vector<Value>& args) {
    if (!valuesEqual(args[0], args[1])) {
        testFailure("assertEqual", "expected " + valueToString(args[0]) +
            " == " + valueToString(args[1]));
    }
    return Value{};
}

Value testAssertNotEqual(const std::vector<Value>& args) {
    if (valuesEqual(args[0], args[1])) {
        testFailure("assertNotEqual", "expected " + valueToString(args[0]) +
            " != " + valueToString(args[1]));
    }
    return Value{};
}

Value testAssertNear(const std::vector<Value>& args) {
    const double actual = toDouble(args[0]);
    const double expected = toDouble(args[1]);
    const double epsilon = toDouble(args[2]);
    if (!(epsilon >= 0.0) || std::isnan(actual) || std::isnan(expected) ||
        std::fabs(actual - expected) > epsilon) {
        testFailure("assertNear", "expected " + valueToString(args[0]) +
            " to be within " + valueToString(args[2]) + " of " +
            valueToString(args[1]));
    }
    return Value{};
}



// --- Logging ---
Value logInfo(const std::vector<Value>& args) { std::cout << "[INFO] " << valueToString(args[0]) << "\n"; return Value{}; }
Value logWarn(const std::vector<Value>& args) { std::cout << "[WARN] " << valueToString(args[0]) << "\n"; return Value{}; }
Value logError(const std::vector<Value>& args) { std::cerr << "[ERROR] " << valueToString(args[0]) << "\n"; return Value{}; }

namespace {
// One writer for every level so formatting stays consistent and error-class
// levels remain on stderr.
Value writeLog(const char* level, const Value& value) {
    std::ostream& out = (std::string(level) == "ERROR" || std::string(level) == "FATAL") ? std::cerr : std::cout;
    out << "[" << level << "] " << valueToString(value) << "\n";
    return Value{};
}
} // namespace
Value logDebug(const std::vector<Value>& args) { return writeLog("DEBUG", args[0]); }
Value logTrace(const std::vector<Value>& args) { return writeLog("TRACE", args[0]); }
Value logFatal(const std::vector<Value>& args) { return writeLog("FATAL", args[0]); }
// Log.at(level, value) - an explicit level chosen at runtime.
Value logAt(const std::vector<Value>& args) {
    std::string level = requireString(args[0], "Log.at");
    for (char& c : level) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (level != "TRACE" && level != "DEBUG" && level != "INFO" && level != "WARN" &&
        level != "ERROR" && level != "FATAL")
        throw std::runtime_error("Log.at: unknown level \"" + level + "\" (expected trace/debug/info/warn/error/fatal)");
    return writeLog(level.c_str(), args[1]);
}


// --- Advanced text ---
Value textFormat(const std::vector<Value>& args) {
    std::string out = requireString(args[0], "Text.format");
    auto vals = requireList(args[1], "Text.format");
    for (std::size_t i = 0; i < vals->items.size(); ++i) {
        const std::string token = "{" + std::to_string(i) + "}";
        const std::string value = valueToString(vals->items[i]);
        std::size_t pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) { out.replace(pos, token.size(), value); pos += value.size(); }
    }
    return out;
}

static void collectRegexNames(const std::shared_ptr<zl::regex_engine::Node>& n, std::vector<std::pair<int,std::string>>& out) {
    if (n->kind == zl::regex_engine::NodeKind::NamedGroup && n->captureIndex > 0 && !n->captureName.empty()) out.emplace_back(n->captureIndex, n->captureName);
    for (const auto& c : n->children) collectRegexNames(c, out);
}

static Value makeRegexMatchObject(const std::string& text, const zl::regex_engine::MatchResult& match, const zl::regex_engine::Pattern& pattern) {
    auto ref = makeGCObject();
    ref->className = "RegexMatch";
    objectFieldAccess(*ref, "value") = match.groups.empty() ? text.substr(match.start, match.end - match.start) : match.groups[0];
    objectFieldAccess(*ref, "start") = static_cast<std::int64_t>(match.start);
    objectFieldAccess(*ref, "end") = static_cast<std::int64_t>(match.end);
    auto groups = makeGCList();
    auto matched = makeGCList();
    auto namedGroups = makeGCMap();
    auto namedMatched = makeGCMap();
    for (std::size_t i = 0; i < match.groups.size(); ++i) {
        groups->items.emplace_back(match.groups[i]);
        matched->items.emplace_back(i < match.groupMatched.size() && match.groupMatched[i]);
    }
    std::vector<std::pair<int,std::string>> names;
    collectRegexNames(pattern.root, names);
    for (const auto& [index, name] : names) if (index >= 0 && static_cast<std::size_t>(index) < match.groups.size()) {
        namedGroups->entries.emplace_back(name, match.groups[index]);
        namedMatched->entries.emplace_back(name, index < static_cast<int>(match.groupMatched.size()) && match.groupMatched[index]);
    }
    objectFieldAccess(*ref, "groups") = ListRef(std::move(groups));
    objectFieldAccess(*ref, "matched") = ListRef(std::move(matched));
    objectFieldAccess(*ref, "namedGroups") = MapRef(std::move(namedGroups));
    objectFieldAccess(*ref, "namedMatched") = MapRef(std::move(namedMatched));
    return ObjectRef(std::move(ref));
}
static zl::regex_engine::Pattern loadRegexPattern(const std::vector<Value>& args, std::size_t index, const char* fnName) {
    try { return zl::regex_engine::parse(requireString(args[index], fnName)); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("RegexError: ") + fnName + ": " + e.what()); }
}
Value textRegexMatches(const std::vector<Value>& args) {
    const std::string text = requireString(args[0], "Text.regexMatches"); auto p = loadRegexPattern(args,1,"Text.regexMatches");
    try { return zl::regex_engine::match(p,text,true).matched; } catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexMatches: ")+e.what());}
}
Value textRegexFullMatches(const std::vector<Value>& args) {
    const std::string text = requireString(args[0], "Text.regexFullMatches"); auto p = loadRegexPattern(args,1,"Text.regexFullMatches");
    try { auto r=zl::regex_engine::match(p,text,false); return r.matched && r.start==0 && r.end==text.size(); } catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexFullMatches: ")+e.what());}
}
Value textRegexFindAll(const std::vector<Value>& args) {
    const std::string text = requireString(args[0], "Text.regexFindAll"); auto p = loadRegexPattern(args,1,"Text.regexFindAll");
    try {auto ms=zl::regex_engine::matchAll(p,text);auto out=makeGCList();for(auto&m:ms)out->items.emplace_back(m.groups.empty()?text.substr(m.start,m.end-m.start):m.groups[0]);return ListRef(std::move(out));}catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexFindAll: ")+e.what());}
}
Value textRegexReplace(const std::vector<Value>& args) {
    const std::string text = requireString(args[0], "Text.regexReplace"); const std::string repl=requireString(args[2],"Text.regexReplace"); auto p=loadRegexPattern(args,1,"Text.regexReplace");
    try{return zl::regex_engine::replace(p,text,repl);}catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexReplace: ")+e.what());}
}
Value textRegexFind(const std::vector<Value>& args) {
    const std::string text=requireString(args[0],"Text.regexFind");auto p=loadRegexPattern(args,1,"Text.regexFind");
    try{auto m=zl::regex_engine::match(p,text,true);if(!m.matched)return Value{};return makeRegexMatchObject(text,m,p);}catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexFind: ")+e.what());}
}
Value textRegexFindMatches(const std::vector<Value>& args) {
    const std::string text=requireString(args[0],"Text.regexFindMatches");auto p=loadRegexPattern(args,1,"Text.regexFindMatches");
    try{auto ms=zl::regex_engine::matchAll(p,text);auto out=makeGCList();for(auto&m:ms)out->items.emplace_back(makeRegexMatchObject(text,m,p));return ListRef(std::move(out));}catch(const std::exception&e){throw std::runtime_error(std::string("RegexError: Text.regexFindMatches: ")+e.what());}
}

// --- JSON serialization ---
class JsonParser {
    const std::string& s; std::size_t p = 0;
    int depth = 0;
    static constexpr int kMaxNestingDepth = 500;
    void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    bool take(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error("Serialize.decode: " + m + " at offset " + std::to_string(p)); }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7Fu) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    std::uint32_t hex4() {
        if (p + 4 > s.size()) fail("incomplete unicode escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    Value parseString() {
        if (!take('"')) fail("expected string");
        std::string out;
        while (p < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[p++]);
            if (c == '"') return out;
            if (c < 0x20u) fail("unescaped control character in string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (p >= s.size()) fail("unterminated escape");
            const char e = s[p++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    const std::uint32_t first = hex4();
                    std::uint32_t cp = first;
                    // JSON encodes non-BMP code points as a UTF-16 surrogate pair.
                    if (first >= 0xD800u && first <= 0xDBFFu) {
                        if (p + 2 > s.size() || s[p] != '\\' || s[p + 1] != 'u') {
                            fail("high surrogate must be followed by a low surrogate");
                        }
                        p += 2;
                        const std::uint32_t second = hex4();
                        if (second < 0xDC00u || second > 0xDFFFu) fail("invalid low surrogate");
                        cp = 0x10000u + ((first - 0xD800u) << 10) + (second - 0xDC00u);
                    } else if (first >= 0xDC00u && first <= 0xDFFFu) {
                        fail("unexpected low surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("unsupported escape");
            }
        }
        fail("unterminated string");
    }

    Value parseNumber() {
        ws();
        const std::size_t start = p;
        if (p < s.size() && s[p] == '-') ++p;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) fail("expected number");
        if (s[p] == '0') {
            ++p;
            if (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) fail("leading zero in number");
        } else {
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        }
        bool dbl = false;
        if (p < s.size() && s[p] == '.') {
            dbl = true; ++p;
            if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) fail("expected digit after decimal point");
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            dbl = true; ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) fail("expected digit in exponent");
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        }
        const std::string n = s.substr(start, p - start);
        try {
            return dbl ? Value{std::stod(n)} : Value{static_cast<std::int64_t>(std::stoll(n))};
        } catch (...) { fail("invalid number"); }
    }

    Value parseValue() {
        ws();
        if (p >= s.size()) fail("unexpected end");
        if (s[p] == '"') return parseString();
        if (s[p] == '{' || s[p] == '[') {
            // Arrays and objects recurse; without a cap a hostile payload of
            // 100k nested '[' is a stack overflow (SIGSEGV) rather than a
            // parse error. 500 is far above any real document and shallow
            // enough that recursion stays in the default stack budget.
            if (depth >= kMaxNestingDepth) fail("nesting too deep (limit " + std::to_string(kMaxNestingDepth) + ")");
            ++depth;
            Value v = s[p] == '{' ? parseObject() : parseArray();
            --depth;
            return v;
        }
        if (s.compare(p, 4, "true") == 0) { p += 4; return true; }
        if (s.compare(p, 5, "false") == 0) { p += 5; return false; }
        if (s.compare(p, 4, "null") == 0) { p += 4; return Value{}; }
        return parseNumber();
    }

    Value parseArray() {
        take('['); auto out = makeGCList(); ws();
        if (take(']')) return out;
        for (;;) {
            out->items.push_back(parseValue()); ws();
            if (take(']')) return out;
            if (!take(',')) fail("expected ',' or ']'");
            ws(); if (p < s.size() && s[p] == ']') fail("trailing comma in array");
        }
    }

    Value parseObject() {
        take('{'); auto out = makeGCMap(); ws();
        if (take('}')) return out;
        for (;;) {
            ws(); if (p >= s.size() || s[p] != '"') fail("object key must be string");
            Value k = parseString(); if (!take(':')) fail("expected ':'");
            Value v = parseValue(); out->entries.emplace_back(k, v); ws();
            if (take('}')) return out;
            if (!take(',')) fail("expected ',' or '}'");
            ws(); if (p < s.size() && s[p] == '}') fail("trailing comma in object");
        }
    }
public:
    explicit JsonParser(const std::string& x): s(x) {}
    Value parse() { Value v = parseValue(); ws(); if (p != s.size()) fail("trailing characters"); return v; }
};
std::string jsonEscape(const std::string& s) {
    std::string o = "\"";
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20u) {
                    o += "\\u00";
                    o.push_back(hex[c >> 4]);
                    o.push_back(hex[c & 0x0F]);
                } else {
                    o.push_back(static_cast<char>(c));
                }
        }
    }
    return o + "\"";
}
// Recursive JSON encoder. Unlike the printer (which truncates), encoding is
// fail-closed: a cycle or a subtree past kMaxValueNestingDepth is a clean,
// catchable error, because silently emitting truncated markers would corrupt
// the serialized data. `active` tracks the current root-to-leaf path so
// diamond sharing still encodes every occurrence; `depth` is passed by value
// so siblings never consume each other's budget. Null box handles encode as
// JSON null rather than dereferencing nothing.
std::string toJsonInner(const Value& v, std::unordered_set<const void*>& active, int depth) {
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (auto p = std::get_if<bool>(&v)) return *p ? "true" : "false";
    if (auto p = std::get_if<std::int64_t>(&v)) return std::to_string(*p);
    if (auto p = std::get_if<double>(&v)) {
        if (!std::isfinite(*p)) throw std::runtime_error("Serialize.encode: non-finite number");
        // Same spelling the printer uses: shortest decimal that reads back as
        // the same double. A value must not be serialized with more digits than
        // `log` shows for it.
        return doubleToShortestString(*p);
    }
    if (auto p = std::get_if<std::string>(&v)) return jsonEscape(*p);
    if (auto p = std::get_if<ListRef>(&v)) {
        if (!*p) return "null";
        if (!active.insert(p->get()).second)
            throw std::runtime_error("Serialize.encode: circular reference detected");
        if (depth + 1 > kMaxValueNestingDepth)
            throw std::runtime_error("Serialize.encode: nesting exceeds the maximum depth of " +
                                     std::to_string(kMaxValueNestingDepth));
        std::string o = "[";
        for (std::size_t i = 0; i < (*p)->items.size(); ++i) {
            if (i) o += ",";
            o += toJsonInner((*p)->items[i], active, depth + 1);
        }
        active.erase(p->get());
        return o + "]";
    }
    if (auto p = std::get_if<MapRef>(&v)) {
        if (!*p) return "null";
        if (!active.insert(p->get()).second)
            throw std::runtime_error("Serialize.encode: circular reference detected");
        if (depth + 1 > kMaxValueNestingDepth)
            throw std::runtime_error("Serialize.encode: nesting exceeds the maximum depth of " +
                                     std::to_string(kMaxValueNestingDepth));
        std::string o = "{";
        const auto snapshot = (*p)->snapshotEntries();
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            if (i) o += ",";
            const auto& e = snapshot[i];
            if (!std::holds_alternative<std::string>(e.first))
                throw std::runtime_error("Serialize.encode: object keys must be strings");
            o += jsonEscape(std::get<std::string>(e.first)) + ":" + toJsonInner(e.second, active, depth + 1);
        }
        active.erase(p->get());
        return o + "}";
    }
    if (auto p = std::get_if<ObjectRef>(&v)) {
        if (!*p) return "null";
        if (!active.insert(p->get()).second)
            throw std::runtime_error("Serialize.encode: circular reference detected");
        if (depth + 1 > kMaxValueNestingDepth)
            throw std::runtime_error("Serialize.encode: nesting exceeds the maximum depth of " +
                                     std::to_string(kMaxValueNestingDepth));
        // The generic collection classes are thin wrappers whose only field is
        // the native storage they delegate to. Encoding the wrapper would emit
        // `{"__native":{...}}` - the object's plumbing rather than its data - so
        // encode the payload directly instead.
        const auto& className = (*p)->className;
        if (className == "List" || className == "Map" || className == "Set") {
            const Value* native = objectFieldLookup(**p, "__native");
            if (native != nullptr) {
                std::string encoded = toJsonInner(*native, active, depth + 1);
                active.erase(p->get());
                return encoded;
            }
        }
        std::string o = "{";
        bool first = true;
        // Layout order for declared fields, as in valueToString.
        objectFieldForEach(**p, [&](const std::string& name, const Value& fieldValue) {
            if (!first) o += ",";
            first = false;
            o += jsonEscape(name) + ":" + toJsonInner(fieldValue, active, depth + 1);
        });
        active.erase(p->get());
        return o + "}";
    }
    throw std::runtime_error("Serialize.encode: unsupported value");
}
std::string toJson(const Value& v) {
    std::unordered_set<const void*> active;
    return toJsonInner(v, active, 0);
}
Value serializeEncode(const std::vector<Value>& args){return toJson(args[0]);}
Value serializeDecode(const std::vector<Value>& args){return JsonParser(requireString(args[0],"Serialize.decode")).parse();}
Value serializeAsString(const std::vector<Value>& args){if(!std::holds_alternative<std::string>(args[0]))throw std::runtime_error("Serialize.asString: expected string");return std::get<std::string>(args[0]);}
Value serializeAsInt(const std::vector<Value>& args){if(!std::holds_alternative<std::int64_t>(args[0]))throw std::runtime_error("Serialize.asInt: expected int");return std::get<std::int64_t>(args[0]);}
Value serializeAsDouble(const std::vector<Value>& args){if(std::holds_alternative<std::int64_t>(args[0]))return static_cast<double>(std::get<std::int64_t>(args[0]));if(!std::holds_alternative<double>(args[0]))throw std::runtime_error("Serialize.asDouble: expected number");return std::get<double>(args[0]);}
Value serializeAsBool(const std::vector<Value>& args){if(!std::holds_alternative<bool>(args[0]))throw std::runtime_error("Serialize.asBool: expected bool");return std::get<bool>(args[0]);}

// --- Checksums / hashes ---
std::uint32_t crc32(const std::string& s){std::uint32_t crc=0xFFFFFFFFu;for(unsigned char c:s){crc^=c;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xEDB88320u:0);}return ~crc;}
std::string hex32(std::uint32_t x){std::ostringstream o;o<<std::hex<<std::setw(8)<<std::setfill('0')<<x;return o.str();}
Value cryptoCrc32(const std::vector<Value>& args){return hex32(crc32(requireString(args[0],"Crypto.crc32")));}

// Compact SHA-256 implementation; this is a native primitive, not a ZL
// reimplementation, and exposes only the stable digest API to ZL.
std::string sha256(const std::string& in){
    static const std::uint32_t K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};std::uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};std::vector<std::uint8_t> d(in.begin(),in.end());std::uint64_t bits=(std::uint64_t)d.size()*8;d.push_back(0x80);while(d.size()%64!=56)d.push_back(0);for(int i=7;i>=0;--i)d.push_back((bits>>(i*8))&255);auto R=[](std::uint32_t x,int n){return (x>>n)|(x<<(32-n));};for(std::size_t off=0;off<d.size();off+=64){std::uint32_t w[64]{};for(int i=0;i<16;++i)w[i]=(d[off+i*4]<<24)|(d[off+i*4+1]<<16)|(d[off+i*4+2]<<8)|d[off+i*4+3];for(int i=16;i<64;++i){auto s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3);auto s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}std::uint32_t a=h[0],b=h[1],c=h[2],e=h[4],f=h[5],g=h[6],hh=h[7],dd=h[3];for(int i=0;i<64;++i){auto S1=R(e,6)^R(e,11)^R(e,25);auto ch=(e&f)^((~e)&g);auto t1=hh+S1+ch+K[i]+w[i];auto S0=R(a,2)^R(a,13)^R(a,22);auto maj=(a&b)^(a&c)^(b&c);auto t2=S0+maj;hh=g;g=f;f=e;e=dd+t1;dd=c;c=b;b=a;a=t1+t2;}h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=dd;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;}std::ostringstream o;o<<std::hex<<std::setfill('0');for(auto x:h)o<<std::setw(8)<<x;return o.str();}
Value cryptoSha256(const std::vector<Value>& args){return sha256(requireString(args[0],"Crypto.sha256"));}
Value hashCode(const std::vector<Value>& args){ return valueHashCode(args[0]); }

// Compact SHA-1. Legacy-strength: exposed for interoperability with existing
// protocols, not recommended for new integrity or signature use.
std::string sha1(const std::string& in){
    std::uint32_t h[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    std::vector<std::uint8_t> d(in.begin(),in.end());
    std::uint64_t bits=(std::uint64_t)d.size()*8; d.push_back(0x80);
    while(d.size()%64!=56) d.push_back(0);
    for(int i=7;i>=0;--i) d.push_back((bits>>(i*8))&255);
    auto R=[](std::uint32_t x,int n){return (x<<n)|(x>>(32-n));};
    for(std::size_t off=0;off<d.size();off+=64){
        std::uint32_t w[80]{};
        for(int i=0;i<16;++i) w[i]=(d[off+i*4]<<24)|(d[off+i*4+1]<<16)|(d[off+i*4+2]<<8)|d[off+i*4+3];
        for(int i=16;i<80;++i) w[i]=R(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        std::uint32_t a=h[0],b=h[1],c=h[2],dd=h[3],e=h[4];
        for(int i=0;i<80;++i){
            std::uint32_t f,k;
            if(i<20){f=(b&c)|((~b)&dd);k=0x5A827999;}
            else if(i<40){f=b^c^dd;k=0x6ED9EBA1;}
            else if(i<60){f=(b&c)|(b&dd)|(c&dd);k=0x8F1BBCDC;}
            else {f=b^c^dd;k=0xCA62C1D6;}
            const std::uint32_t t=R(a,5)+f+e+k+w[i];
            e=dd;dd=c;c=R(b,30);b=a;a=t;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=dd;h[4]+=e;
    }
    std::ostringstream o;o<<std::hex<<std::setfill('0');
    for(auto x:h)o<<std::setw(8)<<x;
    return o.str();
}
Value cryptoSha1(const std::vector<Value>& args){return sha1(requireString(args[0],"Crypto.sha1"));}

// Compact MD5. Checksum-strength only; never use it for security decisions.
std::string md5(const std::string& in){
    static const std::uint32_t S[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                                      5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                      4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                                      6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static const std::uint32_t K[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    std::uint32_t h[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    std::vector<std::uint8_t> d(in.begin(),in.end());
    const std::uint64_t bits=(std::uint64_t)d.size()*8;
    d.push_back(0x80);
    while(d.size()%64!=56) d.push_back(0);
    for(int i=0;i<8;++i) d.push_back(static_cast<std::uint8_t>((bits>>(i*8))&255));
    const auto R=[](std::uint32_t x,std::uint32_t n){return (x<<n)|(x>>(32-n));};
    for(std::size_t off=0;off<d.size();off+=64){
        std::uint32_t m[16]{};
        for(int i=0;i<16;++i)
            m[i]=d[off+i*4]|(d[off+i*4+1]<<8)|(d[off+i*4+2]<<16)|(static_cast<std::uint32_t>(d[off+i*4+3])<<24);
        std::uint32_t a=h[0],b=h[1],c=h[2],dd=h[3];
        for(std::uint32_t i=0;i<64;++i){
            std::uint32_t f,g;
            if(i<16){f=(b&c)|((~b)&dd);g=i;}
            else if(i<32){f=(dd&b)|((~dd)&c);g=(5*i+1)%16;}
            else if(i<48){f=b^c^dd;g=(3*i+5)%16;}
            else {f=c^(b|(~dd));g=(7*i)%16;}
            f=f+a+K[i]+m[g];
            a=dd;dd=c;c=b;b=b+R(f,S[i]);
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=dd;
    }
    std::ostringstream o;o<<std::hex<<std::setfill('0');
    for(auto x:h)
        for(int i=0;i<4;++i) o<<std::setw(2)<<static_cast<int>((x>>(i*8))&255);
    return o.str();
}
Value cryptoMd5(const std::vector<Value>& args){return md5(requireString(args[0],"Crypto.md5"));}

// FNV-1a 64-bit: a fast non-cryptographic digest for bucketing and change
// detection, returned as 16 lowercase hex digits.
Value cryptoFnv1a64(const std::vector<Value>& args){
    const std::string& value = requireString(args[0], "Crypto.fnv1a64");
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream o; o<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
    return o.str();
}

Value cryptoHexEncode(const std::vector<Value>& args){
    const std::string& value = requireString(args[0], "Crypto.hexEncode");
    std::ostringstream o; o<<std::hex<<std::setfill('0');
    for (unsigned char c : value) o<<std::setw(2)<<static_cast<int>(c);
    return o.str();
}

Value cryptoHexDecode(const std::vector<Value>& args){
    const std::string& value = requireString(args[0], "Crypto.hexDecode");
    if (value.size() % 2 != 0) throw std::runtime_error("Crypto.hexDecode: odd-length hex input");
    std::string out;
    out.reserve(value.size() / 2);
    const auto digit = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Crypto.hexDecode: invalid hex digit");
    };
    for (std::size_t i = 0; i + 1 < value.size(); i += 2)
        out.push_back(static_cast<char>(digit(value[i]) * 16 + digit(value[i + 1])));
    return out;
}

const char* base64Alphabet(){return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";}

Value cryptoBase64Encode(const std::vector<Value>& args){
    const std::string& value = requireString(args[0], "Crypto.base64Encode");
    const char* alphabet = base64Alphabet();
    std::string out;
    out.reserve(((value.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < value.size(); i += 3) {
        const std::size_t remaining = value.size() - i;
        std::uint32_t block = static_cast<unsigned char>(value[i]) << 16;
        if (remaining > 1) block |= static_cast<unsigned char>(value[i + 1]) << 8;
        if (remaining > 2) block |= static_cast<unsigned char>(value[i + 2]);
        out.push_back(alphabet[(block >> 18) & 63]);
        out.push_back(alphabet[(block >> 12) & 63]);
        out.push_back(remaining > 1 ? alphabet[(block >> 6) & 63] : '=');
        out.push_back(remaining > 2 ? alphabet[block & 63] : '=');
    }
    return out;
}

Value cryptoBase64Decode(const std::vector<Value>& args){
    const std::string& value = requireString(args[0], "Crypto.base64Decode");
    const std::string alphabet = base64Alphabet();
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : value) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const auto pos = alphabet.find(c);
        if (pos == std::string::npos) throw std::runtime_error("Crypto.base64Decode: invalid base64 character");
        buffer = (buffer << 6) | static_cast<std::uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}


// --- System ---

Value sysExit(const std::vector<Value>& args) {
    throw SystemExitException{static_cast<int>(toInt64Strict(args[0]))};
}

Value sysGetEnv(const std::vector<Value>& args) {
    const std::string& name = requireString(args[0], "System.getEnv");
    const char* val = std::getenv(name.c_str());
    if (!val) return Value{}; // nil if not set
    return std::string(val);
}

// Runs `command` through the shell and captures its stdout. Deliberately
// broad (the person explicitly asked for this) - there's no sandboxing here,
// same as Python's os.system or Java's ProcessBuilder.
Value sysExec(const std::vector<Value>& args) {
    const std::string& command = requireString(args[0], "System.exec");
    std::array<char, 256> buffer{};
    std::string result;
    // MSVC declares the pipe-of-a-command functions with a leading underscore;
    // the bare POSIX names are not in its headers.
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) throw std::runtime_error("System.exec: failed to start \"" + command + "\"");
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}


// Runs `command` and returns its exit status instead of its output; the
// child's stdout/stderr stay attached to this process.
Value sysExecStatus(const std::vector<Value>& args) {
    const std::string& command = requireString(args[0], "System.execStatus");
    const int status = std::system(command.c_str());
    if (status == -1) throw std::runtime_error("System.execStatus: failed to start \"" + command + "\"");
#ifdef _WIN32
    return static_cast<std::int64_t>(status);
#else
    if (WIFEXITED(status)) return static_cast<std::int64_t>(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return static_cast<std::int64_t>(128 + WTERMSIG(status));
    return static_cast<std::int64_t>(status);
#endif
}

Value sysSetEnv(const std::vector<Value>& args) {
    const std::string& name = requireString(args[0], "System.setEnv");
    const std::string& value = requireString(args[1], "System.setEnv");
    if (name.empty() || name.find('=') != std::string::npos)
        throw std::runtime_error("System.setEnv: invalid variable name \"" + name + "\"");
#ifdef _WIN32
    if (_putenv_s(name.c_str(), value.c_str()) != 0)
#else
    if (setenv(name.c_str(), value.c_str(), 1) != 0)
#endif
        throw std::runtime_error("System.setEnv: could not set \"" + name + "\"");
    return Value{};
}

Value sysHasEnv(const std::vector<Value>& args) {
    return std::getenv(requireString(args[0], "System.hasEnv").c_str()) != nullptr;
}

// Reads a variable with an explicit default, avoiding a nil round trip.
Value sysEnvOr(const std::vector<Value>& args) {
    const char* value = std::getenv(requireString(args[0], "System.envOr").c_str());
    return value ? std::string(value) : requireString(args[1], "System.envOr");
}

Value sysPlatform(const std::vector<Value>&) {
#if defined(_WIN32)
    return std::string("windows");
#elif defined(__APPLE__)
    return std::string("macos");
#elif defined(__linux__)
    return std::string("linux");
#else
    return std::string("unknown");
#endif
}


Value reflectionFunctionInvoke(const std::vector<Value>& args) {
    if (!g_currentNativeVm) throw std::runtime_error("Reflection.functionInvoke: no active VM");
    if (args.size() != 2) throw std::runtime_error("Reflection.functionInvoke: expected function and args");
    return g_currentNativeVm->invokeReflectiveFunction(args[0], args[1]);
}

Value reflectionMethodInvoke(const std::vector<Value>& args) {
    if (!g_currentNativeVm) throw std::runtime_error("Reflection.methodInvoke: no active VM");
    if (args.size() != 3) throw std::runtime_error("Reflection.methodInvoke: expected method, receiver, args");
    return g_currentNativeVm->invokeReflectiveMethod(args[0], args[1], args[2]);
}

Value reflectionConstructorInvoke(const std::vector<Value>& args) {
    if (!g_currentNativeVm) throw std::runtime_error("Reflection.constructorInvoke: no active VM");
    if (args.size() != 2) throw std::runtime_error("Reflection.constructorInvoke: expected constructor, args");
    return g_currentNativeVm->invokeReflectiveConstructor(args[0], args[1]);
}

std::vector<NativeFunction> buildTable() {
    // Callback bindings carry explicit NativeId values. The catalog supplies
    // language metadata; the table supplies only the VM implementations.
    // Binding no longer depends on the two tables having matching positions.
        static const std::vector<std::pair<NativeId, std::function<Value(const std::vector<Value>&)>>> callbacks = {
        std::pair{NativeId::TYPE_NAME, typeName},
        std::pair{NativeId::TYPE_FIELDS, typeFields},
        std::pair{NativeId::TYPE_METHODS, typeMethods},
        std::pair{NativeId::TYPE_BASE, typeBase},
        std::pair{NativeId::TYPE_CALLABLE, typeCallable},
        std::pair{NativeId::MATH_SQRT, mathSqrt},
        std::pair{NativeId::MATH_ABS, mathAbs},
        std::pair{NativeId::MATH_POW, mathPow},
        std::pair{NativeId::MATH_FLOOR, mathFloor},
        std::pair{NativeId::MATH_CEIL, mathCeil},
        std::pair{NativeId::MATH_ROUND, mathRound},
        std::pair{NativeId::MATH_MIN, mathMin},
        std::pair{NativeId::MATH_MAX, mathMax},
        std::pair{NativeId::MATH_CBRT, mathCbrt},
        std::pair{NativeId::MATH_TRUNC, mathTrunc},
        std::pair{NativeId::MATH_SIN, mathSin},
        std::pair{NativeId::MATH_COS, mathCos},
        std::pair{NativeId::MATH_TAN, mathTan},
        std::pair{NativeId::MATH_ASIN, mathAsin},
        std::pair{NativeId::MATH_ACOS, mathAcos},
        std::pair{NativeId::MATH_ATAN, mathAtan},
        std::pair{NativeId::MATH_ATAN2, mathAtan2},
        std::pair{NativeId::MATH_LOG, mathLog},
        std::pair{NativeId::MATH_LOG10, mathLog10},
        std::pair{NativeId::MATH_EXP, mathExp},
        std::pair{NativeId::MATH_RANDOM, mathRandom},
        std::pair{NativeId::MATH_RANDOMINT, mathRandomInt},
        std::pair{NativeId::MATH_RANDOMFLOAT, mathRandomFloat},
        std::pair{NativeId::IO_PRINT, ioPrint},
        std::pair{NativeId::IO_PRINTLN, ioPrintln},
        std::pair{NativeId::IO_READLINE, ioReadLine},
        std::pair{NativeId::IO_CLEAR, ioClear},
        std::pair{NativeId::COLLECTION_NEWLIST, collNewList},
        std::pair{NativeId::COLLECTION_PUSH, collPush},
        std::pair{NativeId::COLLECTION_POP, collPop},
        std::pair{NativeId::COLLECTION_GET, collGet},
        std::pair{NativeId::COLLECTION_SET, collSet},
        std::pair{NativeId::COLLECTION_LENGTH, collLength},
        std::pair{NativeId::COLLECTION_NEWMAP, collNewMap},
        std::pair{NativeId::COLLECTION_MAPSET, collMapSet},
        std::pair{NativeId::COLLECTION_MAPGET, collMapGet},
        std::pair{NativeId::COLLECTION_MAPHAS, collMapHas},
        std::pair{NativeId::COLLECTION_MAPREMOVE, collMapRemove},
        std::pair{NativeId::COLLECTION_MAPKEYS, collMapKeys},
        std::pair{NativeId::COLLECTION_MAPVALUES, collMapValues},
        std::pair{NativeId::COLLECTION_NEWSET, collNewSet},
        std::pair{NativeId::COLLECTION_SETADD, collSetAdd},
        std::pair{NativeId::COLLECTION_SETHAS, collSetHas},
        std::pair{NativeId::COLLECTION_SETREMOVE, collSetRemove},
        std::pair{NativeId::COLLECTION_SETITEMS, collSetItems},
        std::pair{NativeId::QUEUE_NEWQUEUE, queueNew},
        std::pair{NativeId::QUEUE_ENQUEUE, queueEnqueue},
        std::pair{NativeId::QUEUE_DEQUEUE, queueDequeue},
        std::pair{NativeId::QUEUE_PEEK, queuePeek},
        std::pair{NativeId::QUEUE_ISEMPTY, queueIsEmpty},
        std::pair{NativeId::QUEUE_SIZE, queueSize},
        std::pair{NativeId::STACK_NEWSTACK, stackNew},
        std::pair{NativeId::STACK_PUSH, stackPush},
        std::pair{NativeId::STACK_POP, stackPop},
        std::pair{NativeId::STACK_PEEK, stackPeek},
        std::pair{NativeId::STACK_ISEMPTY, stackIsEmpty},
        std::pair{NativeId::STACK_SIZE, stackSize},
        std::pair{NativeId::STRING_LENGTH, strLength},
        std::pair{NativeId::STRING_UPPER, strUpper},
        std::pair{NativeId::STRING_LOWER, strLower},
        std::pair{NativeId::STRING_TRIM, strTrim},
        std::pair{NativeId::STRING_CONTAINS, strContains},
        std::pair{NativeId::STRING_STARTSWITH, strStartsWith},
        std::pair{NativeId::STRING_ENDSWITH, strEndsWith},
        std::pair{NativeId::STRING_INDEXOF, strIndexOf},
        std::pair{NativeId::STRING_CHARAT, strCharAt},
        std::pair{NativeId::STRING_SUBSTRING, strSubstring},
        std::pair{NativeId::STRING_REPLACE, strReplace},
        std::pair{NativeId::STRING_SPLIT, strSplit},
        std::pair{NativeId::STRING_TOINT, strToInt},
        std::pair{NativeId::STRING_TOFLOAT, strToFloat},
        std::pair{NativeId::FILESYSTEM_READFILE, fsReadFile},
        std::pair{NativeId::FILESYSTEM_WRITEFILE, fsWriteFile},
        std::pair{NativeId::FILESYSTEM_APPENDFILE, fsAppendFile},
        std::pair{NativeId::FILESYSTEM_EXISTS, fsExists},
        std::pair{NativeId::FILESYSTEM_DELETEFILE, fsDeleteFile},
        std::pair{NativeId::FILESYSTEM_LISTDIR, fsListDir},
        std::pair{NativeId::NETWORK_RESOLVE, networkResolve},
        std::pair{NativeId::TIME_NOW, timeNow},
        std::pair{NativeId::TIME_NOWMILLIS, timeNowMillis},
        std::pair{NativeId::TIME_SLEEP, timeSleep},
        std::pair{NativeId::TIME_SLEEP_ASYNC, timeSleepAsync},
        std::pair{NativeId::TIME_YEAR, timeYear},
        std::pair{NativeId::TIME_MONTH, timeMonth},
        std::pair{NativeId::TIME_DAY, timeDay},
        std::pair{NativeId::TIME_HOUR, timeHour},
        std::pair{NativeId::TIME_MINUTE, timeMinute},
        std::pair{NativeId::TIME_SECOND, timeSecond},
        std::pair{NativeId::TIME_FORMAT, timeFormat},
        std::pair{NativeId::THREAD_START, threadStart},
        std::pair{NativeId::THREAD_JOIN, threadJoin},
        std::pair{NativeId::THREAD_ISALIVE, threadIsAlive},
        std::pair{NativeId::TASK_SPAWN, taskSpawn},
        std::pair{NativeId::MUTEX_WITHLOCK, mutexWithLock},
        std::pair{NativeId::RWLOCK_WITHREAD, rwLockWithRead},
        std::pair{NativeId::RWLOCK_WITHWRITE, rwLockWithWrite},
        std::pair{NativeId::ATOMIC_LOAD, atomicLoad},
        std::pair{NativeId::ATOMIC_STORE, atomicStore},
        std::pair{NativeId::ATOMIC_ADD, atomicAdd},
        std::pair{NativeId::ATOMIC_LOAD_BOOL, atomicLoadBool},
        std::pair{NativeId::ATOMIC_STORE_BOOL, atomicStoreBool},
        std::pair{NativeId::ATOMIC_LOAD_DOUBLE, atomicLoadDouble},
        std::pair{NativeId::ATOMIC_STORE_DOUBLE, atomicStoreDouble},
        std::pair{NativeId::ATOMIC_LOAD_REF, atomicLoadRef},
        std::pair{NativeId::ATOMIC_STORE_REF, atomicStoreRef},
        std::pair{NativeId::SEMAPHORE_ACQUIRE, semaphoreAcquire},
        std::pair{NativeId::SEMAPHORE_RELEASE, semaphoreRelease},
        std::pair{NativeId::SEMAPHORE_AVAILABLE, semaphoreAvailable},
        std::pair{NativeId::SEMAPHORE_SETPERMITS, semaphoreSetPermits},
        std::pair{NativeId::SEMAPHORE_TRYACQUIRE, semaphoreTryAcquire},
        std::pair{NativeId::SEMAPHORE_RELEASEMANY, semaphoreReleaseMany},
        std::pair{NativeId::CONDITION_WAIT, conditionWait},
        std::pair{NativeId::CONDITION_WAITFOR, conditionWaitFor},
        std::pair{NativeId::CONDITION_NOTIFYONE, conditionNotifyOne},
        std::pair{NativeId::CONDITION_NOTIFYALL, conditionNotifyAll},
        std::pair{NativeId::CHANNEL_CREATE, channelCreate},
        std::pair{NativeId::CHANNEL_SEND, channelSend},
        std::pair{NativeId::CHANNEL_RECEIVE, channelReceive},
        std::pair{NativeId::CHANNEL_SIZE, channelSize},
        std::pair{NativeId::CHANNEL_SEND_ASYNC, channelSendAsync},
        std::pair{NativeId::CHANNEL_RECEIVE_ASYNC, channelReceiveAsync},
        std::pair{NativeId::TEST_FAIL, testFail},
        std::pair{NativeId::TEST_ASSERTTRUE, testAssertTrue},
        std::pair{NativeId::TEST_ASSERTFALSE, testAssertFalse},
        std::pair{NativeId::TEST_ASSERTEQUAL, testAssertEqual},
        std::pair{NativeId::TEST_ASSERTNOTEQUAL, testAssertNotEqual},
        std::pair{NativeId::TEST_ASSERTNEAR, testAssertNear},
        std::pair{NativeId::LOG_INFO, logInfo},
        std::pair{NativeId::LOG_WARN, logWarn},
        std::pair{NativeId::LOG_ERROR, logError},
        std::pair{NativeId::TEXT_FORMAT, textFormat},
        std::pair{NativeId::TEXT_REGEXMATCHES, textRegexMatches},
        std::pair{NativeId::TEXT_REGEXFULLMATCHES, textRegexFullMatches},
        std::pair{NativeId::TEXT_REGEXFINDALL, textRegexFindAll},
        std::pair{NativeId::TEXT_REGEXREPLACE, textRegexReplace},
        std::pair{NativeId::TEXT_REGEXFIND, textRegexFind},
        std::pair{NativeId::TEXT_REGEXFINDMATCHES, textRegexFindMatches},
        std::pair{NativeId::SERIALIZE_ENCODE, serializeEncode},
        std::pair{NativeId::SERIALIZE_DECODE, serializeDecode},
        std::pair{NativeId::SERIALIZE_ASSTRING, serializeAsString},
        std::pair{NativeId::SERIALIZE_ASINT, serializeAsInt},
        std::pair{NativeId::SERIALIZE_ASDOUBLE, serializeAsDouble},
        std::pair{NativeId::SERIALIZE_ASBOOL, serializeAsBool},
        std::pair{NativeId::CRYPTO_CRC32, cryptoCrc32},
        std::pair{NativeId::CRYPTO_SHA256, cryptoSha256},
        std::pair{NativeId::HASH_CODE, hashCode},
        std::pair{NativeId::SYSTEM_EXIT, sysExit},
        std::pair{NativeId::SYSTEM_GETENV, sysGetEnv},
        std::pair{NativeId::SYSTEM_EXEC, sysExec},
        std::pair{NativeId::INT_PARSE, intParse},
        std::pair{NativeId::DOUBLE_PARSE, doubleParse},
        std::pair{NativeId::BOOL_PARSE, boolParse},
        std::pair{NativeId::GC_STATS, gcStats},
        std::pair{NativeId::SHARED_SHARE, sharedShare},
        std::pair{NativeId::SHARED_GET, std::function<Value(const std::vector<Value>&)>{sharedGet}},
        std::pair{NativeId::SHARED_SET, std::function<Value(const std::vector<Value>&)>{sharedSet}},
        std::pair{NativeId::SHARED_WITHLOCK, std::function<Value(const std::vector<Value>&)>{sharedWithLock}},
        std::pair{NativeId::TYPE_OF, typeOf},
        std::pair{NativeId::TYPE_KIND, typeKind},
        std::pair{NativeId::TYPE_IS_DATA, typeIsData},
        std::pair{NativeId::TYPE_IS_ENUM, typeIsEnum},
        std::pair{NativeId::TYPE_ENUM_MEMBERS, typeEnumMembers},
        std::pair{NativeId::REFLECTION_NAME, reflectionName},
        std::pair{NativeId::REFLECTION_KIND, reflectionKind},
        std::pair{NativeId::REFLECTION_IS_DATA, reflectionIsData},
        std::pair{NativeId::REFLECTION_IS_ENUM, reflectionIsEnum},
        std::pair{NativeId::REFLECTION_ENUM_MEMBERS, reflectionEnumMembers},
        std::pair{NativeId::REFLECTION_FIELDS, reflectionFields},
        std::pair{NativeId::REFLECTION_METHODS, reflectionMethods},
        std::pair{NativeId::REFLECTION_BASE, reflectionBase},
        std::pair{NativeId::REFLECTION_INTERFACES, reflectionInterfaces},
        std::pair{NativeId::REFLECTION_TYPE_PARAMETERS, reflectionTypeParameters},
        std::pair{NativeId::REFLECTION_FIELD_NAME, reflectionFieldName},
        std::pair{NativeId::REFLECTION_FIELD_TYPE, reflectionFieldType},
        std::pair{NativeId::REFLECTION_FIELD_ACCESS, reflectionFieldAccess},
        std::pair{NativeId::REFLECTION_METHOD_NAME, reflectionMethodName},
        std::pair{NativeId::REFLECTION_METHOD_RETURN_TYPE, reflectionMethodReturnType},
        std::pair{NativeId::REFLECTION_METHOD_ACCESS, reflectionMethodAccess},
        std::pair{NativeId::REFLECTION_METHOD_STATIC, reflectionMethodIsStatic},
        std::pair{NativeId::REFLECTION_METHOD_ASYNC, reflectionMethodIsAsync},
        std::pair{NativeId::REFLECTION_METHOD_PARAMETERS, reflectionMethodParameters},
        std::pair{NativeId::REFLECTION_FUNCTION, reflectionFunction},
        std::pair{NativeId::REFLECTION_FUNCTION_NAME, reflectionFunctionName},
        std::pair{NativeId::REFLECTION_FUNCTION_PARAMETERS, reflectionFunctionParameters},
        std::pair{NativeId::REFLECTION_FUNCTION_RETURN_TYPE, reflectionFunctionReturnType},
        std::pair{NativeId::REFLECTION_FUNCTION_ASYNC, reflectionFunctionIsAsync},
        std::pair{NativeId::REFLECTION_FUNCTION_IS_NATIVE, reflectionFunctionIsNative},
        std::pair{NativeId::REFLECTION_FUNCTION_INVOKE, reflectionFunctionInvoke},
        std::pair{NativeId::REFLECTION_TYPE_CONSTRUCTORS, reflectionTypeConstructors},
        std::pair{NativeId::REFLECTION_CONSTRUCTOR_PARAMETERS, reflectionConstructorParameters},
        std::pair{NativeId::REFLECTION_FIELD, reflectionField},
        std::pair{NativeId::REFLECTION_METHOD, reflectionMethod},
        std::pair{NativeId::REFLECTION_CONSTRUCTOR, reflectionConstructor},
        std::pair{NativeId::REFLECTION_METHOD_INVOKE, reflectionMethodInvoke},
        std::pair{NativeId::REFLECTION_CONSTRUCTOR_INVOKE, reflectionConstructorInvoke},
        std::pair{NativeId::FILESYSTEM_RENAME, fsRename},
        std::pair{NativeId::FILESYSTEM_COPYFILE, fsCopyFile},
        std::pair{NativeId::FILESYSTEM_SIZE, fsSize},
        std::pair{NativeId::FILESYSTEM_ISFILE, fsIsFile},
        std::pair{NativeId::FILESYSTEM_ISDIRECTORY, fsIsDirectory},
        std::pair{NativeId::FILESYSTEM_CREATEDIR, fsCreateDir},
        std::pair{NativeId::FILESYSTEM_REMOVEDIR, fsRemoveDir},
        std::pair{NativeId::FILESYSTEM_MODIFIEDTIME, fsModifiedTime},
        std::pair{NativeId::FILESYSTEM_ABSOLUTEPATH, fsAbsolutePath},
        std::pair{NativeId::FILESYSTEM_PARENTPATH, fsParentPath},
        std::pair{NativeId::FILESYSTEM_FILENAME, fsFileName},
        std::pair{NativeId::FILESYSTEM_EXTENSION, fsExtension},
        std::pair{NativeId::FILESYSTEM_STEM, fsStem},
        std::pair{NativeId::FILESYSTEM_JOINPATH, fsJoinPath},
        std::pair{NativeId::FILESYSTEM_CURRENTDIR, fsCurrentDir},
        std::pair{NativeId::FILESYSTEM_TEMPDIR, fsTempDir},
        std::pair{NativeId::NETWORK_RESOLVEALL, networkResolveAll},
        std::pair{NativeId::NETWORK_HOSTNAME, networkHostname},
        std::pair{NativeId::NETWORK_ISVALIDIP, networkIsValidIp},
        std::pair{NativeId::STRING_LASTINDEXOF, strLastIndexOf},
        std::pair{NativeId::STRING_INDEXOFFROM, strIndexOfFrom},
        std::pair{NativeId::STRING_TRIMSTART, strTrimStart},
        std::pair{NativeId::STRING_TRIMEND, strTrimEnd},
        std::pair{NativeId::STRING_COMPARE, strCompare},
        std::pair{NativeId::STRING_COMPAREIGNORECASE, strCompareIgnoreCase},
        std::pair{NativeId::STRING_CODEPOINTAT, strCodePointAt},
        std::pair{NativeId::STRING_FROMCODEPOINT, strFromCodePoint},
        std::pair{NativeId::STRING_REPEAT, strRepeat},
        std::pair{NativeId::STRING_UTF8LENGTH, strUtf8Length},
        std::pair{NativeId::STRING_UTF8CHARAT, strUtf8CharAt},
        std::pair{NativeId::STRING_UTF8SUBSTRING, strUtf8Substring},
        std::pair{NativeId::STRING_UTF8REVERSE, strUtf8Reverse},
        std::pair{NativeId::STRING_UTF8CODEPOINTAT, strUtf8CodePointAt},
        std::pair{NativeId::STRING_UTF8FROMCODEPOINT, strUtf8FromCodePoint},
        std::pair{NativeId::STRING_UTF8BYTEINDEX, strUtf8ByteIndex},
        std::pair{NativeId::STRING_UTF8INDEXFROMBYTE, strUtf8IndexFromByte},
        std::pair{NativeId::TIME_MONOTONICMILLIS, timeMonotonicMillis},
        std::pair{NativeId::TIME_DAYOFWEEK, timeDayOfWeek},
        std::pair{NativeId::TIME_DAYOFYEAR, timeDayOfYear},
        std::pair{NativeId::TIME_ISLEAPYEAR, timeIsLeapYear},
        std::pair{NativeId::TIME_FROMPARTS, timeFromParts},
        std::pair{NativeId::TIME_UTCFORMAT, timeUtcFormat},
        std::pair{NativeId::CRYPTO_SHA1, cryptoSha1},
        std::pair{NativeId::CRYPTO_MD5, cryptoMd5},
        std::pair{NativeId::CRYPTO_FNV1A64, cryptoFnv1a64},
        std::pair{NativeId::CRYPTO_HEXENCODE, cryptoHexEncode},
        std::pair{NativeId::CRYPTO_HEXDECODE, cryptoHexDecode},
        std::pair{NativeId::CRYPTO_BASE64ENCODE, cryptoBase64Encode},
        std::pair{NativeId::CRYPTO_BASE64DECODE, cryptoBase64Decode},
        std::pair{NativeId::LOG_DEBUG, logDebug},
        std::pair{NativeId::LOG_TRACE, logTrace},
        std::pair{NativeId::LOG_FATAL, logFatal},
        std::pair{NativeId::LOG_AT, logAt},
        std::pair{NativeId::SYSTEM_EXECSTATUS, sysExecStatus},
        std::pair{NativeId::SYSTEM_SETENV, sysSetEnv},
        std::pair{NativeId::SYSTEM_HASENV, sysHasEnv},
        std::pair{NativeId::SYSTEM_ENVOR, sysEnvOr},
        std::pair{NativeId::SYSTEM_PLATFORM, sysPlatform},
    };

    const auto& signatures = nativeSignatureTable();
    std::vector<std::pair<NativeId, std::function<Value(const std::vector<Value>&)>>> callbackById;
    callbackById.reserve(callbacks.size());
    for (const auto& [id, callback] : callbacks) {
        const auto duplicate = std::find_if(callbackById.begin(), callbackById.end(), [id](const auto& entry) { return entry.first == id; });
        if (duplicate != callbackById.end()) throw std::logic_error("duplicate native callback id");
        callbackById.emplace_back(id, callback);
    }

    std::vector<NativeFunction> table;
    table.reserve(signatures.size());
    for (const auto& signature : signatures) {
        const auto it = std::find_if(callbackById.begin(), callbackById.end(), [&signature](const auto& entry) { return entry.first == signature.id; });
        if (it == callbackById.end()) {
            throw std::logic_error("native catalog entry has no callback: '" + signature.qualifiedName + "'");
        }
        table.push_back(NativeFunction{signature.id, signature.qualifiedName, it->second});
    }
    if (callbackById.size() != signatures.size()) {
        throw std::logic_error("native callback catalog mismatch");
    }
    return table;
}

} // namespace

std::vector<NativeFunction>& mutableNativeFunctionTable() {
    static std::vector<NativeFunction> table = buildTable();
    return table;
}

NativeFunctionRegistrar::NativeFunctionRegistrar(std::vector<NativeFunction> functions) {
    auto& table = mutableNativeFunctionTable();
    table.insert(table.end(), std::make_move_iterator(functions.begin()), std::make_move_iterator(functions.end()));
}

const std::vector<NativeFunction>& nativeFunctionTable() {
    return mutableNativeFunctionTable();
}

std::optional<std::size_t> findNativeFunction(NativeId id) {
    const auto& table = nativeFunctionTable();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].id == id) return i;
    }
    return std::nullopt;
}

std::optional<std::size_t> findNativeFunction(const std::string& qualifiedName) {
    const auto signature = findNativeSignature(qualifiedName);
    if (!signature) {
        // Not a catalog native: it may be a runtime-registered extension
        // binding (zl-bind output), which is resolved by name alone.
        return findNativeFunctionByName(qualifiedName);
    }
    return findNativeFunction((*signature)->id);
}

std::optional<std::size_t> findNativeFunctionByName(const std::string& qualifiedName) {
    const auto& table = nativeFunctionTable();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].qualifiedName == qualifiedName) return i;
    }
    return std::nullopt;
}

} // namespace zl
