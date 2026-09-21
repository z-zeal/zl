#include "zl/vm/vm.hpp"
#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"
#include "zl/vm/runtime_type_checks.hpp"
#include "zl/vm/runtime_fault.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <memory>
#include <cstdlib>

#include "zl/vm/native.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/common/type_name.hpp"
#include <algorithm>
#include <unordered_set>

namespace zl {
namespace {

RuntimeTypeBindings methodTypeArgBindings(const Chunk& chunk, std::size_t operand,
                                          const std::vector<std::string>& typeParameters) {
    RuntimeTypeBindings extra;
    if (operand == 0 || typeParameters.empty()) return extra;
    if (operand > chunk.names.size()) return extra;
    const std::string& joined = chunk.names[operand - 1];
    std::vector<std::string> names;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= joined.size(); ++i) {
        if (i == joined.size() || joined[i] == ';') {
            names.push_back(joined.substr(start, i - start));
            start = i + 1;
        }
    }
    const std::size_t n = std::min(names.size(), typeParameters.size());
    for (std::size_t i = 0; i < n; ++i) extra[typeParameters[i]] = names[i];
    return extra;
}

bool typeNameMentionsUnbound(const TypeName& name, const std::vector<std::string>& params,
                             const RuntimeTypeBindings& bindings) {
    if (std::find(params.begin(), params.end(), name.name) != params.end() &&
        bindings.find(name.name) == bindings.end()) {
        return true;
    }
    for (const auto& arg : name.args) {
        if (typeNameMentionsUnbound(arg, params, bindings)) return true;
    }
    for (const auto& member : name.unionMembers) {
        if (typeNameMentionsUnbound(member, params, bindings)) return true;
    }
    return false;
}

std::string formatStackTrace(const ExecutionState* state, const char* fallback) {
    std::string trace;
    if (state) {
        for (const auto& name : state->callStackNames()) {
            if (!trace.empty()) trace += "\n";
            trace += "at " + name;
        }
    }
    return trace.empty() ? std::string(fallback) : trace;
}

// Doubles are fail-closed like ints: a non-finite result is a catchable
// arithmetic error, never a silent inf/NaN - the language has neither
// value. Overflow of + - * / on finite inputs can only produce an infinity
// (never NaN); pow() can additionally return NaN on domain errors, e.g. a
// negative base with a fractional exponent.
double checkedDoubleResult(double result, const char* operation) {
    if (std::isinf(result))
        throwArithmeticError(std::string("floating-point overflow in ") + operation);
    if (std::isnan(result))
        throwArithmeticError(std::string("invalid floating-point result in ") + operation);
    return result;
}

// Build a real ZL exception object of `className`, so `catch IndexError e`
// and friends work against failures raised inside the VM or a native.
ObjectRef makeRuntimeExceptionObject(const std::string& className, const std::string& message,
                                     const Chunk* chunk, const ExecutionState* state,
                                     const char* traceFallback) {
    // Fall back to the always-present base class when a program's chunk does
    // not carry metadata for the specific class (older/partial chunks).
    std::string resolved = className;
    if (chunk && !chunk->classReflection.count(resolved)) {
        resolved = chunk->classReflection.count("RuntimeError") ? "RuntimeError" : "Exception";
    }
    Value objectValue = makeEmptyObject(resolved);
    auto object = std::get<ObjectRef>(objectValue);
    // The flat field layout resolves names through the box's own runtimeType,
    // so the type must be attached BEFORE the declared fields are written -
    // otherwise message/stackTrace would land in extraFields and the
    // in-layout read would see nil (the ObjectBox comment in value.hpp).
    if (chunk) {
        auto it = chunk->classReflection.find(resolved);
        if (it != chunk->classReflection.end()) object->runtimeType = it->second.runtimeType;
    }
    objectFieldAccess(*object, "message") = message;
    objectFieldAccess(*object, "stackTrace") = formatStackTrace(state, traceFallback);
    return object;
}

[[noreturn]] void throwRuntimeFault(const ZlRuntimeFault& fault, const Chunk* chunk,
                                    const ExecutionState* state) {
    throw ZlThrownException(makeRuntimeExceptionObject(fault.className(), fault.what(), chunk, state, "at <runtime>"));
}

// CancellationException for the async invocation paths. The flat field layout
// resolves "message" through the box's runtime class, so the type is attached
// before the first write (see makeRuntimeExceptionObject).
ObjectRef makeCancellationException(const Chunk& chunk) {
    Value object = makeEmptyObject("CancellationException");
    auto ex = std::get<ObjectRef>(object);
    const auto it = chunk.classReflection.find("CancellationException");
    if (it != chunk.classReflection.end()) ex->runtimeType = it->second.runtimeType;
    objectFieldAccess(*ex, "message") = std::string("task was cancelled");
    return ex;
}

// A plain std::runtime_error escaping the interpreter core is still a genuine
// ZL-level failure; surface it as `RuntimeError` rather than losing the
// program's ability to catch and describe it.
[[noreturn]] void throwInterpreterError(const std::exception& e, const Chunk* chunk,
                                        const ExecutionState* state) {
    throw ZlThrownException(makeRuntimeExceptionObject("RuntimeError", e.what(), chunk, state, "at <runtime>"));
}

[[noreturn]] void throwNativeError(const std::exception& e,
                                   const Chunk& chunk,
                                   const ExecutionState& state) {
    const std::string rawMessage = e.what();
    const std::string regexPrefix = "RegexError: ";
    const bool isRegexError = rawMessage.rfind(regexPrefix, 0) == 0;
    const std::string className = isRegexError ? "RegexError" : "NativeError";
    const std::string message = isRegexError ? rawMessage.substr(regexPrefix.size()) : rawMessage;
    throw ZlThrownException(makeRuntimeExceptionObject(className, message, &chunk, &state, "at <native>"));
}

[[noreturn]] void throwReflectionException(const std::string& message,
                                            const Chunk* chunk,
                                            const ExecutionState* state) {
    std::string className = "ReflectionError";
    std::string clean = message;
    const std::string invalid = "ReflectionError.InvalidArguments: ";
    const std::string access = "ReflectionError.AccessViolation: ";
    if (clean.rfind(invalid, 0) == 0) {
        className = "InvalidArguments";
        clean.erase(0, invalid.size());
    } else if (clean.rfind(access, 0) == 0) {
        className = "AccessViolation";
        clean.erase(0, access.size());
    } else if (clean.rfind("ReflectionError: ", 0) == 0) {
        clean.erase(0, std::string("ReflectionError: ").size());
    }
    throw ZlThrownException(makeRuntimeExceptionObject(className, clean, chunk, state, "at <reflection>"));
}

} // namespace


VM::VM(std::shared_ptr<RuntimeScheduler> sharedScheduler)
    : ownedScheduler_(sharedScheduler ? nullptr : std::make_shared<RuntimeScheduler>()),
      scheduler_(sharedScheduler ? std::move(sharedScheduler) : ownedScheduler_),
      gcParticipantId_(GCSafepointCoordinator::instance().registerParticipant()) {
    if (const char* env = std::getenv("ZL_MAX_CALL_DEPTH")) {
        try {
            const unsigned long long parsed = std::stoull(env);
            if (parsed > 0 && parsed <= static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
                state_ = ExecutionState(static_cast<std::size_t>(parsed));
            }
        } catch (...) {
            // Invalid configuration falls back to the safe default.
        }
    }
}

VM::~VM() {
    if (gcParticipantId_ != 0) {
        GCSafepointCoordinator::instance().unregisterParticipant(gcParticipantId_);
        gcParticipantId_ = 0;
    }
}



namespace {
bool isNumeric(const Value& v) { return isNumericValue(v); } // local alias, keeps call sites below unchanged

bool isSubclass(const Chunk& chunk, const std::string& child, const std::string& parent) {
    if (child == parent) return true;
    auto it = chunk.classReflection.find(child);
    std::size_t guard = 0;
    while (it != chunk.classReflection.end() && !it->second.baseClassName.empty() && guard++ < 1024) {
        if (it->second.baseClassName == parent) return true;
        it = chunk.classReflection.find(it->second.baseClassName);
    }
    return false;
}
} // namespace

void VM::pushNativeRoots(const std::vector<Value>& roots) {
    nativeRootFrames_.push_back(roots);
}

void VM::popNativeRoots() {
    if (!nativeRootFrames_.empty()) nativeRootFrames_.pop_back();
}

void VM::appendNativeRoots(std::vector<Value>& roots) const {
    for (const auto& frame : nativeRootFrames_) {
        roots.insert(roots.end(), frame.begin(), frame.end());
    }
}

ExecutionState::CallFrame VM::makeCallFrame(const Chunk& chunk, const FunctionInfo& fn,
                                              const std::vector<Value>& args,
                                              const std::optional<Value>& receiver,
                                              const ClosureRef& closure,
                                              RuntimeTypeBindings extraBindings) const {
    if (args.size() != fn.paramNames.size()) {
        throw std::runtime_error("VM: function '" + fn.name + "' argument count mismatch");
    }
    ExecutionState::CallFrame frame;
    frame.functionName = fn.name;
    frame.ownerClassName = fn.ownerClassName;
    frame.ownedLocalNames = fn.ownedLocalNames;
    if (closure) {
        frame.locals = closure->captured;
        frame.activeClosure = closure;
        frame.typeBindings = closure->typeBindings;
    } else if (receiver) {
        frame.locals["this"] = *receiver;
        if (const auto* object = std::get_if<ObjectRef>(&*receiver); object && *object) {
            frame.typeBindings = receiverTypeBindings(**object, chunk, fn.ownerClassName);
        }
    } else if (state_.inFunction() && state_.currentFrame().ownerClassName == fn.ownerClassName) {
        frame.typeBindings = state_.typeBindings();
    }
    for (const auto& [k, v] : extraBindings) frame.typeBindings[k] = v;
    frame.typeParameters = fn.typeParameters;
    frame.returnTypeName = substituteTypeParams(fn.returnTypeName, frame.typeBindings);
    RuntimeTypeCheck types(&chunk);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string expected = i < fn.parameterTypeNames.size()
            ? substituteTypeParams(fn.parameterTypeNames[i], frame.typeBindings) : "unknown";
        if (!expected.empty() && !types.check(args[i], expected)) {
            throwTypeError("type assertion failed for argument " + std::to_string(i + 1) +
                           ": expected " + expected + ", got " + runtimeValueTypeName(args[i]));
        }
        frame.locals[fn.paramNames[i]] = args[i];
    }
    types.commit();
    return frame;
}

VM::ProgramScope::ProgramScope(VM& vm, const Chunk& chunk, std::shared_ptr<const Chunk> owner)
    : vm_(vm), previous_(vm.activeChunk_) {
    if (!owner && !vm_.activeChunkOwners_.empty() && vm_.activeChunk_ == &chunk &&
        vm_.activeChunkOwners_.back()) {
        // Nested scope over the same chunk object (run() -> execute(), or a
        // re-entrant native callback): the enclosing scope's owner is exactly
        // this chunk's owner.
        owner = vm_.activeChunkOwners_.back();
    }
    vm_.activePrograms_.push_back(&chunk);
    vm_.activeChunkOwners_.push_back(std::move(owner));
    vm_.activeChunk_ = &chunk;
    previousJoins_ = DeferredThreadJoins::bind(&vm_.threadJoins_);
}

VM::ProgramScope::~ProgramScope() {
    DeferredThreadJoins::bind(previousJoins_);
    vm_.activeChunk_ = previous_;
    vm_.activePrograms_.pop_back();
    vm_.activeChunkOwners_.pop_back();
}

std::shared_ptr<const Chunk> VM::shareActiveChunk() const {
    if (!activeChunkOwners_.empty() && activeChunkOwners_.back() &&
        activeChunkOwners_.back().get() == activeChunk_) {
        return activeChunkOwners_.back();
    }
    // No shared owner was handed to this run (bare-reference entry point):
    // keep the old behavior and hand out a copy.
    return std::make_shared<Chunk>(*activeChunk_);
}

GCRoots VM::gcRoots() const {
    GCRoots roots;
    state_.appendGCRoots(roots.values);
    appendNativeRoots(roots.values);
    roots.programs = activePrograms_;
    if (entryTask_) roots.values.emplace_back(entryTask_);
    if (asyncInvocation_) {
        asyncInvocation_->frame.appendGCRoots(roots.values);
        if (asyncInvocation_->chunk) roots.programs.push_back(asyncInvocation_->chunk.get());
        if (asyncInvocation_->task) roots.values.emplace_back(asyncInvocation_->task);
        if (asyncInvocation_->awaitedTask) roots.values.emplace_back(asyncInvocation_->awaitedTask);
    }
    appendExceptionRoots(pendingResumeException_, roots.values);
    return roots;
}

void VM::drainThreadJoins(bool bounded) {
    if (threadJoins_.empty()) return;
    BlockingNativeCall blocked(this);
    if (bounded) threadJoins_.drainBounded();
    else threadJoins_.drain();
}

void VM::beginBlockingNativeCall() {
    GCSafepointCoordinator::instance().beginBlockingNative(gcParticipantId_, gcRoots());
}

void VM::endBlockingNativeCall() const {
    if (gcParticipantId_ != 0)
        GCSafepointCoordinator::instance().endBlockingNative(gcParticipantId_);
}

Value VM::binaryArith(OpCode op, const Value& a, const Value& b) const {
    // `+` doubles as string concatenation if EITHER side is a string -
    // convenient for building log() messages like "Hello, " + name.
    if (op == OpCode::Add && (std::holds_alternative<std::string>(a) || std::holds_alternative<std::string>(b))) {
        return valueToString(a) + valueToString(b);
    }

    if (!isNumeric(a) || !isNumeric(b)) {
        throw std::runtime_error("arithmetic operators require numbers (or strings, for +)");
    }

    // Integer power stays integral when both operands are integers and the
    // non-negative exponent produces a representable int64 result. Negative
    // or fractional exponents retain the historical floating-point behavior.
    if (op == OpCode::Pow) {
        const bool bothInts = std::holds_alternative<std::int64_t>(a) &&
                              std::holds_alternative<std::int64_t>(b);
        if (bothInts) {
            const auto base = std::get<std::int64_t>(a);
            const auto exponent = std::get<std::int64_t>(b);
            if (exponent >= 0) {
                std::int64_t result = 1;
                std::int64_t factor = base;
                std::uint64_t n = static_cast<std::uint64_t>(exponent);
                auto mulChecked = [](std::int64_t x, std::int64_t y, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
                    return !__builtin_mul_overflow(x, y, &out);
#else
                    if (x == 0 || y == 0) { out = 0; return true; }
                    if (x == -1) { if (y == std::numeric_limits<std::int64_t>::min()) return false; out = -y; return true; }
                    if (y == -1) { if (x == std::numeric_limits<std::int64_t>::min()) return false; out = -x; return true; }
                    if (x > 0) {
                        if (y > 0) { if (x > std::numeric_limits<std::int64_t>::max() / y) return false; }
                        else { if (y < std::numeric_limits<std::int64_t>::min() / x) return false; }
                    } else {
                        if (y > 0) { if (x < std::numeric_limits<std::int64_t>::min() / y) return false; }
                        else { if (x != 0 && y < std::numeric_limits<std::int64_t>::max() / x) return false; }
                    }
                    out = x * y; return true;
#endif
                };
                bool exact = true;
                while (n != 0) {
                    if (n & 1u) {
                        std::int64_t next = 0;
                        if (!mulChecked(result, factor, next)) { exact = false; break; }
                        result = next;
                    }
                    n >>= 1u;
                    if (n != 0) {
                        std::int64_t next = 0;
                        if (!mulChecked(factor, factor, next)) { exact = false; break; }
                        factor = next;
                    }
                }
                if (exact) return result;
                throwArithmeticError("integer overflow in exponentiation");
            }
            throw std::runtime_error("negative integer exponent requires floating-point result");
        }
        return checkedDoubleResult(std::pow(toDouble(a), toDouble(b)), "exponentiation");
    }

    // Whole-number math stays whole-number; if either side is a decimal, both promote to double.
    bool bothInts = std::holds_alternative<std::int64_t>(a) && std::holds_alternative<std::int64_t>(b);
    if (bothInts) {
        std::int64_t x = std::get<std::int64_t>(a);
        std::int64_t y = std::get<std::int64_t>(b);
        switch (op) {
            case OpCode::Add: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_add_overflow(x, y, &result))
                    throwArithmeticError("integer overflow in addition");
#else
                if ((y > 0 && x > std::numeric_limits<std::int64_t>::max() - y) ||
                    (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y))
                    throwArithmeticError("integer overflow in addition");
                result = x + y;
#endif
                return result;
            }
            case OpCode::Sub: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_sub_overflow(x, y, &result))
                    throwArithmeticError("integer overflow in subtraction");
#else
                if ((y < 0 && x > std::numeric_limits<std::int64_t>::max() + y) ||
                    (y > 0 && x < std::numeric_limits<std::int64_t>::min() + y))
                    throwArithmeticError("integer overflow in subtraction");
                result = x - y;
#endif
                return result;
            }
            case OpCode::Mul: {
                std::int64_t result;
#if defined(__GNUC__) || defined(__clang__)
                if (__builtin_mul_overflow(x, y, &result))
                    throwArithmeticError("integer overflow in multiplication");
#else
                if (x != 0 && y != 0) {
                    if ((x == -1 && y == std::numeric_limits<std::int64_t>::min()) ||
                        (y == -1 && x == std::numeric_limits<std::int64_t>::min()))
                        throwArithmeticError("integer overflow in multiplication");
                    if (x > 0) {
                        if (y > 0 && x > std::numeric_limits<std::int64_t>::max() / y) throwArithmeticError("integer overflow in multiplication");
                        if (y < 0 && y < std::numeric_limits<std::int64_t>::min() / x) throwArithmeticError("integer overflow in multiplication");
                    } else {
                        if (y > 0 && x < std::numeric_limits<std::int64_t>::min() / y) throwArithmeticError("integer overflow in multiplication");
                        if (y < 0 && x < std::numeric_limits<std::int64_t>::max() / y) throwArithmeticError("integer overflow in multiplication");
                    }
                }
                result = x * y;
#endif
                return result;
            }
            case OpCode::Div:
                if (y == 0) throwArithmeticError("division by zero");
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    throwArithmeticError("integer overflow in division");
                return x / y;
            case OpCode::Mod:
                if (y == 0) throwArithmeticError("modulo by zero");
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    return std::int64_t{0};
                return x % y;
            default: break;
        }
    }

    double x = toDouble(a);
    double y = toDouble(b);
    switch (op) {
        case OpCode::Add: return checkedDoubleResult(x + y, "addition");
        case OpCode::Sub: return checkedDoubleResult(x - y, "subtraction");
        case OpCode::Mul: return checkedDoubleResult(x * y, "multiplication");
        case OpCode::Div:
            if (y == 0.0) throwArithmeticError("division by zero");
            return checkedDoubleResult(x / y, "division");
        case OpCode::Mod:
            if (y == 0.0) throwArithmeticError("modulo by zero");
            return std::fmod(x, y);
        default:
            throw std::runtime_error("VM: not an arithmetic opcode");
    }
}

Value VM::binaryBitwise(OpCode op, const Value& a, const Value& b) const {
    const std::int64_t x = toInt64Strict(a);
    const std::int64_t y = toInt64Strict(b);
    switch (op) {
        case OpCode::BitAnd: return x & y;
        case OpCode::BitOr:  return x | y;
        case OpCode::BitXor: return x ^ y;
        case OpCode::Shl: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << shift);
        }
        case OpCode::Shr: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            const std::uint64_t ux = static_cast<std::uint64_t>(x);
            std::uint64_t shifted = ux >> shift;
            if (x < 0 && shift != 0) shifted |= (~std::uint64_t{0}) << (64 - shift);
            return static_cast<std::int64_t>(shifted);
        }
        case OpCode::Ushr: {
            if (y < 0 || y >= 64) throw std::runtime_error("shift count must be in the range 0..63");
            const auto shift = static_cast<unsigned>(y);
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(x) >> shift);
        }
        default:
            throw std::runtime_error("VM: not a bitwise opcode");
    }
}

Value VM::binaryCompare(OpCode op, const Value& a, const Value& b) const {
    if (op == OpCode::Eq || op == OpCode::Neq) {
        bool equal = valuesEqual(a, b);
        return op == OpCode::Eq ? equal : !equal;
    }

    // <, >, <=, >= only make sense for numbers here
    if (!isNumeric(a) || !isNumeric(b)) {
        throw std::runtime_error("comparison operators require numbers");
    }
    const int cmp = compareNumericValues(a, b);
    if (cmp == 2) return false; // NaN: all ordered comparisons are false
    switch (op) {
        case OpCode::Lt:  return cmp < 0;
        case OpCode::Gt:  return cmp > 0;
        case OpCode::Lte: return cmp <= 0;
        case OpCode::Gte: return cmp >= 0;
        default:
            throw std::runtime_error("VM: not a comparison opcode");
    }
}

Value VM::binaryLogical(OpCode op, const Value& a, const Value& b) const {
    // NOTE: the compiler no longer emits OpCode::And/Or - it compiles &&/||
    // with JumpIfFalse/Jump so the right operand is only evaluated when it
    // actually needs to be (see Compiler::compileBinary). This func and
    // the And/Or opcodes are kept only as a defensive fallback for any other
    // bytecode producer; the VM should never hit them from compiled ZL source.
    bool x = isTruthy(a);
    bool y = isTruthy(b);
    return op == OpCode::And ? (x && y) : (x || y);
}

Value VM::unary(OpCode op, const Value& a) const {
    switch (op) {
        case OpCode::Neg:
            if (auto p = std::get_if<std::int64_t>(&a)) {
                if (*p == std::numeric_limits<std::int64_t>::min())
                    throwArithmeticError("integer overflow in negation");
                return -*p;
            }
            if (auto p = std::get_if<double>(&a)) return -*p;
            throw std::runtime_error("unary '-' requires a number");
        case OpCode::Not:
            return !isTruthy(a);
        case OpCode::BitNot:
            return ~toInt64Strict(a);
        default:
            throw std::runtime_error("VM: not a unary opcode");
    }
}

bool VM::rangeContinue(const Value& current, const Value& end, const Value& step) const {
    if (!isNumeric(current) || !isNumeric(end) || !isNumeric(step)) {
        throw std::runtime_error("for-loop range/step must be numbers");
    }
    if (std::holds_alternative<std::int64_t>(current) &&
        std::holds_alternative<std::int64_t>(end) &&
        std::holds_alternative<std::int64_t>(step)) {
        const auto s = std::get<std::int64_t>(step);
        const auto c = std::get<std::int64_t>(current);
        const auto e = std::get<std::int64_t>(end);
        if (s > 0) return c < e;
        if (s < 0) return c > e;
        throw std::runtime_error("for-loop step cannot be 0 (the loop would never end)");
    }
    double s = toDouble(step);
    if (s > 0.0) return toDouble(current) < toDouble(end);
    if (s < 0.0) return toDouble(current) > toDouble(end);
    throw std::runtime_error("for-loop step cannot be 0 (the loop would never end)");
}

void VM::pumpSchedulerUntilTerminal(const TaskRef& task) {
    // The scheduler is cooperative: pump ready async frames while the task is
    // pending so synchronous code cannot deadlock by blocking the very
    // scheduler that owns the task. If another executor later completes the
    // task externally (a native async op finishing on a worker thread enqueues
    // our continuation), keep pumping rather than sleeping indefinitely while
    // owning the scheduler that must resume it.
    pushNativeRoots({task});
    struct TaskRootGuard {
        VM* vm;
        ~TaskRootGuard() { vm->popNativeRoots(); }
    } taskRootGuard{this};
    BlockingNativeCall blocked(this);
    while (!task->isTerminal()) {
        if (scheduler_->runOne()) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int VM::run(const Chunk& chunk, const std::vector<std::string>& programArgs) {
    return runImpl(chunk, programArgs, {});
}

int VM::run(std::shared_ptr<const Chunk> chunk, const std::vector<std::string>& programArgs) {
    // Bind the reference before moving ownership into runImpl: argument
    // evaluation order is unspecified, so `*chunk` after std::move(chunk)
    // could dereference a null shared_ptr.
    const Chunk& chunkRef = *chunk;
    return runImpl(chunkRef, programArgs, std::move(chunk));
}

int VM::runImpl(const Chunk& chunk, const std::vector<std::string>& programArgs,
                std::shared_ptr<const Chunk> owner) {
    ProgramScope program(*this, chunk, std::move(owner));
    const ExecuteStatus result = execute(chunk, 0, false, programArgs, nullptr);
    if (result == ExecuteStatus::Suspended) {
        throw std::runtime_error("VM: top-level execution unexpectedly suspended");
    }
    if (entryTask_ && !entryTask_->isTerminal()) {
        // `main` is an async func and suspended on an await. Draining only the
        // frames that are ready right now is not enough: a native async
        // operation completes on a worker thread and enqueues our continuation
        // later. Pump the scheduler until the entry task settles so the rest of
        // main actually runs.
        pumpSchedulerUntilTerminal(entryTask_);
    }
    {
        BlockingNativeCall blocked(this);
        scheduler_->runUntilIdle();
    }
    // The process-wide heap must never be collected from just this VM's roots
    // while other VMs/threads are still running.
    GCSafepointCoordinator::instance().poll(gcParticipantId_, [this] { return gcRoots(); });
    // Final teardown: wait for dropped workers, but never forever - a worker
    // blocked on a channel nobody will serve is abandoned with a diagnostic
    // rather than hanging the process at exit.
    drainThreadJoins(true);
    // An async entry task is an entry-point result, not an ignored background
    // task. Propagate failure/cancellation with a freshly pinned exception.
    if (entryTask_) (void)entryTask_->observe();
    return 0;
}

VM::ExecuteStatus VM::execute(const Chunk& chunk, std::size_t startIp, bool stopAtReturn,
                const std::vector<std::string>& programArgs, Value* returnValue,
                std::shared_ptr<const Chunk> owner) {
    // Re-entry budget (see nestedExecuteDepth_): nested callbacks otherwise
    // overflow the C++ stack thousands of levels below the 100k ZL-frame cap.
    // RAII-scoped decrement: execute() has many exits (returns and throws).
    if (++nestedExecuteDepth_ > kMaxNestedExecuteDepth) {
        --nestedExecuteDepth_;
        throwStackOverflowError("stack overflow: maximum nested-execution depth (" +
                                std::to_string(kMaxNestedExecuteDepth) + ") exceeded");
    }
    struct NestedExecuteGuard {
        std::size_t& depth;
        ~NestedExecuteGuard() { --depth; }
    };
    NestedExecuteGuard nestedGuard{nestedExecuteDepth_};
    // Re-entrant native callbacks must retain both the callee's program and
    // their caller's program; neither is an independent global GC pin.
    ProgramScope program(*this, chunk, std::move(owner));
    const std::size_t initialCallDepth = state_.callDepth();
    std::size_t ip = startIp;

    std::size_t instructionsSinceSafePoint = 0;
    while (true) {
      try {
        drainThreadJoins();
        if (++instructionsSinceSafePoint >= 128 || TracingGC::instance().shouldCollect()) {
            // The snapshot is provided lazily: building it walks the whole
            // call stack, and doing that at every safepoint made deep
            // recursion O(n^2). poll() invokes the provider only when a
            // collection rendezvous actually needs these roots.
            GCSafepointCoordinator::instance().poll(gcParticipantId_, [this] { return gcRoots(); });
            instructionsSinceSafePoint = 0;
        }
        if (ip >= chunk.code.size()) throw std::runtime_error("VM: instruction pointer out of bounds");
        const Instruction& instr = chunk.code[ip];

        if (pendingResumeException_) {
            auto error = pendingResumeException_;
            pendingResumeException_ = nullptr;
            std::rethrow_exception(error);
        }

        switch (instr.op) {
            case OpCode::PushConst:
                if (instr.operand >= chunk.constants.size())
                    throw std::runtime_error("VM: constant index out of bounds");
                state_.push(chunk.constants[instr.operand]);
                ip++;
                break;

            case OpCode::PushProgramArgs: {
                Value list = makeEmptyList();
                auto& items = std::get<ListRef>(list)->items;
                for (const auto& arg : programArgs) items.emplace_back(arg);
                state_.push(list);
                ip++;
                break;
            }

            case OpCode::Pop:
                state_.pop();
                ip++;
                break;

            case OpCode::Dup:
                state_.push(state_.top());
                ip++;
                break;

            case OpCode::DefineVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                Value v = state_.pop();
                state_.setVariable(name, v);
                ip++;
                break;
            }

            case OpCode::LoadVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                state_.push(state_.loadVariable(name));
                ip++;
                break;
            }

            case OpCode::MoveVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                Value v = state_.loadVariable(name);
                state_.setVariable(name, Value{});
                state_.push(std::move(v));
                ip++;
                break;
            }

            case OpCode::DropVar: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: name index out of bounds");
                const std::string& name = chunk.names[instr.operand];
                state_.dropLocal(name);
                ip++;
                break;
            }

            case OpCode::Add: case OpCode::Sub: case OpCode::Mul:
            case OpCode::Div: case OpCode::Mod: case OpCode::Pow: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryArith(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::BitAnd: case OpCode::BitOr: case OpCode::BitXor:
            case OpCode::Shl: case OpCode::Shr: case OpCode::Ushr: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryBitwise(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::Eq: case OpCode::Neq: case OpCode::Lt:
            case OpCode::Gt: case OpCode::Lte: case OpCode::Gte: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryCompare(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::And: case OpCode::Or: {
                Value b = state_.pop();
                Value a = state_.pop();
                state_.push(binaryLogical(instr.op, a, b));
                ip++;
                break;
            }

            case OpCode::Neg: case OpCode::Not: case OpCode::BitNot: {
                Value a = state_.pop();
                state_.push(unary(instr.op, a));
                ip++;
                break;
            }

            case OpCode::Jump:
                ip = instr.operand; // no ip++ - we ARE the jump
                break;

            case OpCode::JumpIfFalse: {
                Value cond = state_.pop();
                ip = isTruthy(cond) ? ip + 1 : instr.operand;
                break;
            }

            case OpCode::RangeContinue: {
                Value step = state_.pop();
                Value end = state_.pop();
                Value current = state_.pop();
                state_.push(rangeContinue(current, end, step));
                ip++;
                break;
            }

            case OpCode::CallNative: {
                const auto& natives = nativeFunctionTable();
                if (instr.operand >= natives.size()) throw std::runtime_error("VM: native func index out of bounds");
                const NativeFunction& native = natives[instr.operand];
                const std::size_t arity = native.arity();
                std::vector<Value> args(arity);
                for (std::size_t i = 0; i < arity; ++i) {
                    args[arity - 1 - i] = state_.pop();
                }
                pushNativeRoots(args);
                struct NativeRootGuard {
                    VM* vm;
                    VM* previous;
                    ~NativeRootGuard() {
                        setCurrentNativeVm(previous);
                        vm->popNativeRoots();
                    }
                } nativeRootGuard{this, setCurrentNativeVm(this)};
                try {
                    Value result = native.fn(args);
                    if (instr.operand2 != 0) {
                        if (instr.operand2 > chunk.names.size())
                            throw std::runtime_error("VM: native factory type index out of bounds");
                        initializeObjectType(result,
                            substituteTypeParams(chunk.names[instr.operand2 - 1], state_.typeBindings()), chunk);
                    }
                    const auto signature = findNativeSignature(native.qualifiedName);
                    if (signature && !(*signature)->returnClassName.empty()) {
                        const auto bindings = nativeTypeBindings(**signature, args.empty() ? "unknown" : runtimeValueTypeName(args.front()));
                        RuntimeTypeCheck types(&chunk);
                        types.require(result, substituteTypeParams((*signature)->returnClassName, bindings));
                        types.commit();
                    }
                    state_.push(result);
                } catch (const ZlThrownException&) {
                    throw;
                } catch (const SystemExitException&) {
                    throw;
                } catch (const ZlRuntimeFault&) {
                    // The native already chose its ZL exception class; do not
                    // flatten it into a generic NativeError.
                    throw;
                } catch (const std::exception& e) {
                    const std::string message = e.what();
                    if (message.rfind("Reflection.", 0) == 0) {
                        throwReflectionException("ReflectionError: " + message, &chunk, &state_);
                    }
                    throwNativeError(e, chunk, state_);
                }
                ip++;
                break;
            }

            case OpCode::TaskBlock: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.block() expects a Task value");
                }
                // Pump the cooperative scheduler until the task settles; once
                // terminal, observe() collects its result (and safely waits on
                // the task condition variable if an external executor is still
                // finalising it).
                pumpSchedulerUntilTerminal(*taskRef);
                state_.push((*taskRef)->observe());
                ++ip;
                break;
            }

            case OpCode::TaskIgnore: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.ignore() expects a Task value");
                }
                (*taskRef)->ignore();
                state_.push(Value{});
                ++ip;
                break;
            }

            case OpCode::MatchType: {
                Value expectedValue = state_.pop();
                Value value = state_.pop();
                auto name = std::get_if<std::string>(&expectedValue);
                RuntimeTypeCheck types(&chunk);
                const bool matches = name && types.check(value, substituteTypeParams(*name, state_.typeBindings()), RuntimeTypeCheck::Mode::Pattern);
                if (matches) types.commit();
                state_.push(matches);
                ++ip;
                break;
            }

            case OpCode::AssertType: {
                Value value = state_.pop();
                if (instr.operand >= chunk.names.size()) {
                    throw std::runtime_error("VM: AssertType name index out of bounds");
                }
                const std::string& rawExpected = chunk.names[instr.operand];
                if (state_.inFunction()) {
                    const auto& frame = state_.currentFrame();
                    if (!frame.typeParameters.empty() &&
                        typeNameMentionsUnbound(parseTypeName(rawExpected), frame.typeParameters,
                                                frame.typeBindings)) {
                        state_.push(value);
                        ++ip;
                        break;
                    }
                }
                const std::string expected = substituteTypeParams(rawExpected, state_.typeBindings());
                RuntimeTypeCheck types(&chunk);
                if (!types.check(value, expected)) {
                    throwTypeError("type assertion failed: expected " + expected + ", got " + runtimeValueTypeName(value));
                }
                types.commit();
                state_.push(value);
                ++ip;
                break;
            }

            case OpCode::TaskCancel: {
                Value value = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&value);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: Task.cancel() expects a Task value");
                }
                const TaskStatus status = (*taskRef)->status();
                if (status == TaskStatus::Pending) {
                    (*taskRef)->cancel();
                } else if (status == TaskStatus::Running) {
                    (*taskRef)->requestCancellation();
                }
                state_.push(Value{});
                ++ip;
                break;
            }

            case OpCode::Await: {
                Value awaited = state_.pop();
                auto* taskRef = std::get_if<TaskRef>(&awaited);
                if (!taskRef || !(*taskRef)) {
                    throw std::runtime_error("VM: await expects a Task value");
                }
                if (asyncInvocation_ && asyncInvocation_->task && asyncInvocation_->task->cancellationRequested()) {
                    throw ZlThrownException(makeCancellationException(chunk));
                }
                const TaskStatus status = (*taskRef)->status();
                if (status == TaskStatus::Succeeded || status == TaskStatus::Failed || status == TaskStatus::Cancelled) {
                    state_.push((*taskRef)->observe());
                    ++ip;
                    break;
                }
                if (!state_.inFunction()) {
                    throw std::runtime_error("VM: await used outside a function");
                }
                if (!asyncInvocation_ || !asyncInvocation_->task) {
                    throw std::runtime_error("VM: await used outside an async invocation");
                }
                auto awaitedTask = *taskRef;
                if (asyncInvocation_->task.get() == awaitedTask.get()) {
                    throw std::runtime_error("VM: async func cannot await its own Task");
                }
                asyncInvocation_->resumeIp = ip + 1;
                asyncInvocation_->awaitedTask = awaitedTask;
                auto vmSelf = shared_from_this();
                asyncInvocation_->task->onCancellation([vmSelf]() {
                    vmSelf->scheduler_->enqueue(std::make_shared<AsyncFrame>([vmSelf]() {
                        vmSelf->resumeAsyncInvocation();
                    }));
                });
                awaitedTask->then([vmSelf, awaitedTask]() {
                    vmSelf->scheduler_->enqueue(std::make_shared<AsyncFrame>([vmSelf, awaitedTask]() {
                        // Resume owns reactivation and restores the awaited value
                        // only after the collector has released this parked VM.
                        if (vmSelf->asyncInvocation_ && vmSelf->asyncInvocation_->awaitedTask == awaitedTask)
                            vmSelf->resumeAsyncInvocation();
                    }));
                });
                return ExecuteStatus::Suspended;
            }

            case OpCode::Call: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: func index out of bounds");
                const FunctionInfo& fn = chunk.functions[instr.operand];
                std::vector<Value> args(fn.paramNames.size());
                for (std::size_t i = args.size(); i > 0; --i) args[i - 1] = state_.pop();
                std::optional<Value> receiver;
                if (!fn.isStatic) {
                    if (const Value* self = state_.findLocal("this")) receiver = *self;
                }
                auto extra = methodTypeArgBindings(chunk, instr.operand2, fn.typeParameters);
                auto frame = makeCallFrame(chunk, fn, args, receiver, {}, std::move(extra));
                if (fn.isAsync) {
                    auto task = scheduleAsyncInvocation(shareActiveChunk(), instr.operand, std::move(frame));
                    if (!state_.inFunction() && !entryTask_) entryTask_ = task;
                    state_.push(task);
                    ++ip;
                    break;
                }
                frame.returnIp = ip + 1;
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::MakeClosure: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: closure func index out of bounds");
                const FunctionInfo& fn = chunk.functions[instr.operand];
                auto box = makeGCClosure();
                box->functionName = fn.name;
                box->paramNames = fn.paramNames;
                if (fn.capturesEvaluationScope) box->typeBindings = state_.typeBindings();
                for (const auto& type : fn.parameterTypeNames)
                    box->parameterTypeNames.push_back(substituteTypeParams(type, box->typeBindings));
                box->returnTypeName = substituteTypeParams(fn.returnTypeName, box->typeBindings);
                box->isAsync = fn.isAsync;
                box->isNative = fn.isNative;
                box->entryAddress = fn.entryAddress;
                box->functionIndex = instr.operand;
                // Share the program chunk instead of copying it per closure:
                // a deep copy here made every lambda allocation O(program
                // size), ~350us for the stdlib-sized chunk.
                box->chunk = shareActiveChunk();
                const auto scope = state_.snapshotScope();
                if (!fn.capturesEvaluationScope) {
                    // Named function values have no lexical environment.
                } else {
                    // captureNames is computed for every lambda by
                    // TypeChecker::analyzeLambdaCaptures, so an empty list means
                    // the body references nothing - capture nothing. Snapshotting
                    // the whole scope instead made a closure that used no
                    // variables carry every local in the enclosing function,
                    // which then failed the Shared<T> check on Thread.start even
                    // for locals it never touched.
                    for (const auto& name : fn.captureNames) {
                        auto it = scope.find(name);
                        if (it != scope.end()) box->captured.emplace(name, it->second);
                    }
                }
                state_.push(Value{std::move(box)});
                ip++;
                break;
            }

            case OpCode::CallValue: {
                const std::size_t argCount = instr.operand2;
                Value callee = state_.pop();
                auto* closureRef = std::get_if<ClosureRef>(&callee);
                if (!closureRef || !*closureRef || !(*closureRef)->chunk ||
                    (*closureRef)->functionIndex >= (*closureRef)->chunk->functions.size()) {
                    throw std::runtime_error("VM: attempted to call an invalid func value");
                }
                const ClosureBox& closure = **closureRef;
                const auto& fn = closure.chunk->functions[closure.functionIndex];
                std::vector<Value> args(argCount);
                for (std::size_t i = argCount; i > 0; --i) args[i - 1] = state_.pop();
                auto frame = makeCallFrame(*closure.chunk, fn, args, std::nullopt, *closureRef);
                if (fn.isAsync) {
                    auto task = scheduleAsyncInvocation(closure.chunk, closure.functionIndex, std::move(frame));
                    state_.push(task);
                    ++ip;
                    break;
                }
                frame.returnIp = ip + 1;
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::NewObject: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: invalid class-name index");
                const std::string& className = chunk.names[instr.operand];
                Value obj = makeEmptyObject(className);
                if (instr.operand3 > chunk.names.size())
                    throw std::runtime_error("VM: object type index out of bounds");
                const std::string typeName = instr.operand3 != 0
                    ? substituteTypeParams(chunk.names[instr.operand3 - 1], state_.typeBindings()) : className;
                initializeObjectType(obj, typeName, chunk);
                state_.push(std::move(obj));
                ip++;
                break;
            }

            case OpCode::CopyObject: {
                Value source = state_.pop();
                if (!std::holds_alternative<ObjectRef>(source) || !std::get<ObjectRef>(source)) {
                    throw std::runtime_error("CopyObject expected object");
                }
                ObjectRef src = std::get<ObjectRef>(source);
                auto copy = makeGCObject();
                copy->genericTypeName = src->genericTypeName;
                copy->className = src->className;
                copy->runtimeType = src->runtimeType;
                copy->fields = src->fields;
                copy->extraFields = src->extraFields;
                state_.push(ObjectRef(copy));
                ip++;
                break;
            }
            case OpCode::GetStaticField: {
                if (instr.operand >= chunk.names.size() || instr.operand2 >= chunk.names.size())
                    throw std::runtime_error("VM: invalid static field metadata index");
                const std::string key = chunk.names[instr.operand] + "." + chunk.names[instr.operand2];
                auto metaIt = chunk.staticFields.find(key);
                if (metaIt == chunk.staticFields.end()) throw std::runtime_error("VM: unknown static field '" + key + "'");
                const auto& meta = metaIt->second;
                std::shared_ptr<StaticFieldState> state;
                {
                    std::lock_guard<std::mutex> mapLock(chunk.staticStorage->mapMutex);
                    auto& slot = chunk.staticStorage->fields[key];
                    if (!slot) {
                        slot = std::make_shared<StaticFieldState>();
                        slot->initializerFunction = meta.initializerFunction;
                    }
                    state = slot;
                }
                for (;;) {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    if (state->status == StaticFieldState::Status::Initialized) {
                        state_.push(state->value);
                        break;
                    }
                    if (state->status == StaticFieldState::Status::Failed) {
                        state->failure.rethrow();
                    }
                    if (state->status == StaticFieldState::Status::Initializing) {
                        if (state->ownerThread == std::this_thread::get_id()) {
                            throw std::runtime_error("StaticInitializationError: re-entrant initialization of '" + key + "'");
                        }
                        {
                            BlockingNativeCall blocked(this);
                            state->cv.wait(lock, [&] { return state->status != StaticFieldState::Status::Initializing; });
                        }
                        continue;
                    }
                    state->status = StaticFieldState::Status::Initializing;
                    state->ownerThread = std::this_thread::get_id();
                    lock.unlock();
                    try {
                        Value result = invokeFunction(chunk, state->initializerFunction, {}, std::nullopt);
                        lock.lock();
                        state->value = result;
                        state->status = StaticFieldState::Status::Initialized;
                        state->ownerThread = {};
                        lock.unlock();
                        state->cv.notify_all();
                        state_.push(std::move(result));
                    } catch (...) {
                        auto failure = std::current_exception();
                        lock.lock();
                        state->failure = StoredException(failure);
                        state->status = StaticFieldState::Status::Failed;
                        state->ownerThread = {};
                        lock.unlock();
                        state->cv.notify_all();
                        std::rethrow_exception(failure);
                    }
                    break;
                }
                ip++;
                break;
            }

            case OpCode::SetStaticField: {
                if (instr.operand >= chunk.names.size() || instr.operand2 >= chunk.names.size())
                    throw std::runtime_error("VM: invalid static field metadata index");
                const std::string key = chunk.names[instr.operand] + "." + chunk.names[instr.operand2];
                auto value = state_.top(); // retain the pending write in roots across native waits
                auto metaIt = chunk.staticFields.find(key);
                if (metaIt == chunk.staticFields.end()) throw std::runtime_error("VM: unknown static field '" + key + "'");
                std::shared_ptr<StaticFieldState> state;
                {
                    std::lock_guard<std::mutex> mapLock(chunk.staticStorage->mapMutex);
                    auto& slot = chunk.staticStorage->fields[key];
                    if (!slot) { slot = std::make_shared<StaticFieldState>(); slot->initializerFunction = metaIt->second.initializerFunction; }
                    state = slot;
                }
                std::unique_lock<std::mutex> lock(state->mutex);
                if (state->status == StaticFieldState::Status::Initializing && state->ownerThread != std::this_thread::get_id()) {
                    BlockingNativeCall blocked(this);
                    state->cv.wait(lock, [&] { return state->status != StaticFieldState::Status::Initializing; });
                }
                Value replacement = value;
                {
                    RuntimeTypeCheck types(&chunk);
                    types.require(value, runtimeFieldType(chunk, chunk.names[instr.operand], chunk.names[instr.operand2]));
                    std::swap(state->value, replacement);
                    types.commit();
                }
                state->status = StaticFieldState::Status::Initialized;
                state->failure = {};
                state->ownerThread = {};
                lock.unlock();
                state->cv.notify_all();
                state_.pop();
                state_.push(std::move(value));
                ip++;
                break;
            }

            case OpCode::GetIndex: {
                RuntimeTypeCheck access(&chunk);
                Value indexValue = state_.pop();
                Value object = state_.pop();
                ListRef list;
                if (const auto* listRef = std::get_if<ListRef>(&object)) {
                    list = *listRef;
                } else if (const auto* objRef = std::get_if<ObjectRef>(&object); objRef && *objRef) {
                    if (const Value* nativeField = objectFieldLookup(**objRef, "__native")) {
                        if (const auto* nativeList = std::get_if<ListRef>(nativeField)) list = *nativeList;
                    }
                }
                if (!list) {
                    throw std::runtime_error("VM: indexed access requires a list");
                }
                if (!std::holds_alternative<std::int64_t>(indexValue)) throw std::runtime_error("VM: list index must be an int");
                const auto index = std::get<std::int64_t>(indexValue);
                const std::size_t logicalSize = list->items.size() - std::min(list->frontIndex, list->items.size());
                if (index < 0 || static_cast<std::size_t>(index) >= logicalSize) throw std::runtime_error("VM: list index out of bounds");
                state_.push(list->items[list->frontIndex + static_cast<std::size_t>(index)]);
                ++ip;
                break;
            }

            case OpCode::GetField: {
                RuntimeTypeCheck access(&chunk);
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: field-name index out of bounds");
                const std::string& fieldName = chunk.names[instr.operand];
                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot read field '" + fieldName + "' of non-object (or nil)");
                }
                
                // Flat layout: the name resolves through the box's runtime
                // class once, and the slot is a vector index from then on.
                // A name with no value (declared but never materialised, or
                // never written at all) keeps its historical nil default.
                const Value* found = objectFieldLookup(**objRef, fieldName);
                state_.push(found ? *found : Value{});
                ip++;
                break;
            }

            case OpCode::SetField: {
                if (instr.operand >= chunk.names.size())
                    throw std::runtime_error("VM: field-name index out of bounds");
                const std::string& fieldName = chunk.names[instr.operand];
                Value value = state_.pop();
                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot write field '" + fieldName + "' of non-object (or nil)");
                }
                
                Value replacement = value; // displaced resources die after the leaf type lock
                {
                    RuntimeTypeCheck types(&chunk);
                    types.require(value, runtimeFieldType(chunk, (*objRef)->className, fieldName, objRef->get()));
                    std::swap(objectFieldAccess(**objRef, fieldName), replacement);
                    types.commit();
                }
                state_.push(value); // assignment leaves the value on the stack
                ip++;
                break;
            }

            case OpCode::InvokeMethod: {
                const std::size_t methodSlotIndex = instr.operand;
                const std::size_t argCount = instr.operand2;

                std::vector<Value> args(argCount);
                for (std::size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = state_.pop();
                }

                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot invoke method on non-object (or nil)");
                }

                auto vtableIt = chunk.classVTables.find((*objRef)->className);
                if (vtableIt == chunk.classVTables.end() || methodSlotIndex >= vtableIt->second.size()) {
                    throw std::runtime_error("VM: no method slot " + std::to_string(methodSlotIndex) +
                                             " for class '" + (*objRef)->className + "'");
                }

                const std::size_t functionIndex = vtableIt->second[methodSlotIndex];
                if (functionIndex == Chunk::INVALID_FUNCTION_INDEX || functionIndex >= chunk.functions.size()) {
                    throw std::runtime_error("VM: class '" + (*objRef)->className +
                                             "' does not implement method slot " + std::to_string(methodSlotIndex));
                }

                const FunctionInfo& fn = chunk.functions[functionIndex];
                if (fn.paramNames.size() != argCount) {
                    throw std::runtime_error(
                        "VM: method '" + fn.name + "' expects " + std::to_string(fn.paramNames.size()) +
                        " argument(s), got " + std::to_string(argCount));
                }
                auto extra = methodTypeArgBindings(chunk, instr.operand3, fn.typeParameters);
                auto frame = makeCallFrame(chunk, fn, args, object, {}, std::move(extra));
                if (fn.isAsync) {
                    auto task = scheduleAsyncInvocation(shareActiveChunk(), functionIndex, std::move(frame));
                    state_.push(task);
                    ++ip;
                    break;
                }
                frame.returnIp = ip + 1;
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::InvokeSuper: {
                if (instr.operand >= chunk.functions.size())
                    throw std::runtime_error("VM: super func index out of bounds");
                const std::size_t argCount = instr.operand2;
                const FunctionInfo& fn = chunk.functions[instr.operand];

                std::vector<Value> args(argCount);
                for (std::size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = state_.pop();
                }

                Value object = state_.pop();
                auto* objRef = std::get_if<ObjectRef>(&object);
                if (!objRef || !(*objRef)) {
                    throw std::runtime_error("VM: cannot call super func on non-object (or nil)");
                }
                if (fn.paramNames.size() != argCount) {
                    throw std::runtime_error(
                        "VM: super func '" + fn.name + "' expects " + std::to_string(fn.paramNames.size()) +
                        " argument(s), got " + std::to_string(argCount));
                }

                auto extra = methodTypeArgBindings(chunk, instr.operand3, fn.typeParameters);
                auto frame = makeCallFrame(chunk, fn, args, object, {}, std::move(extra));
                if (fn.isAsync) {
                    auto task = scheduleAsyncInvocation(shareActiveChunk(), instr.operand, std::move(frame));
                    state_.push(task);
                    ++ip;
                    break;
                }
                frame.returnIp = ip + 1;
                state_.enterFrame(std::move(frame));
                ip = fn.entryAddress;
                break;
            }

            case OpCode::Return: {
                Value resultValue = state_.pop();
                if (!state_.inFunction()) {
                    throw std::runtime_error("'return' used outside of a func");
                }
                const auto& expected = state_.currentFrame().returnTypeName;
                {
                    RuntimeTypeCheck types(&chunk);
                    if (!expected.empty() && !types.check(resultValue, expected)) {
                        throwTypeError("type assertion failed for return: expected " + expected +
                                       ", got " + runtimeValueTypeName(resultValue));
                    }
                    types.commit();
                }
                ExecutionState::CallFrame finishedFrame = state_.leaveFrame();
                if (stopAtReturn && state_.callDepth() < initialCallDepth) {
                    if (returnValue) *returnValue = resultValue;
                    return ExecuteStatus::Completed;
                }
                // A handler installed inside the func that just returned

                // can no longer legally catch exceptions from its caller.
                state_.discardHandlersForFinishedFrames();
                // Write this call's (possibly mutated) locals back into the
                // closure it belongs to, so a closure's own mutations to its
                // captured variables persist the next time IT is called -
                // see CallFrame::activeClosure's comment.
                if (finishedFrame.activeClosure) {
                    finishedFrame.activeClosure->captured = std::move(finishedFrame.locals);
                }
                finishedFrame.dropOwnedLocals();
                state_.push(resultValue);
                ip = finishedFrame.returnIp;
                break;
            }

            case OpCode::PushHandler: {
                std::string catchClassName;
                if (instr.operand2 != 0) {
                    const std::size_t nameIndex = instr.operand2 - 1;
                    if (nameIndex >= chunk.names.size()) throw std::runtime_error("VM: catch type name index out of bounds");
                    catchClassName = chunk.names[nameIndex];
                }
                state_.pushHandler(instr.operand, std::move(catchClassName), false, instr.operand3);
                ip++;
                break;
            }

            case OpCode::PushFinallyHandler:
                state_.pushHandler(instr.operand, {}, true);
                ip++;
                break;

            case OpCode::PopHandler:
                if (state_.hasHandler()) state_.popHandler();
                ip++;
                break;

            case OpCode::Throw: {
                Value thrown = state_.pop();
                if (!std::holds_alternative<ObjectRef>(thrown) || !std::get<ObjectRef>(thrown)) {
                    throw std::runtime_error("throw requires an Exception-derived object");
                }
                auto ex = std::get<ObjectRef>(std::move(thrown));
                if (objectFieldLookup(*ex, "stackTrace") != nullptr || ex->className == "Exception" || isSubclass(chunk, ex->className, "Exception")) {
                    std::string trace;
                    for (const auto& name : state_.callStackNames()) {
                        if (!trace.empty()) trace += "\n";
                        trace += "at " + name;
                    }
                    if (trace.empty()) trace = "at <runtime>";
                    objectFieldAccess(*ex, "stackTrace") = trace;
                }
                throw ZlThrownException(std::move(ex));
            }

            case OpCode::Log:
                std::cout << valueToString(state_.pop()) << "\n";
                ip++;
                break;

            case OpCode::Halt:
                return ExecuteStatus::Completed;
        }
      } catch (const ZlThrownException& e) {
          // execute() is re-entrant: a nested run (a closure driven from a
          // native, started at initialCallDepth) must only consume handlers
          // pushed within THAT nested run. Handlers owned by the caller frame
          // (callStackSize < initialCallDepth) belong to the outer run; letting
          // the nested run catch on them resumes the caller's catchIp against
          // the closure's chunk and re-runs the caller (the exception-in-
          // withLock double-continuation bug). If no in-scope handler matches,
          // rethrow so the outer run handles it in its own chunk.
          if (!dispatchThrownException(chunk, e.value(), initialCallDepth, ip)) throw;
      } catch (const ZlRuntimeFault& fault) {
          // A native/VM failure that names its own ZL exception class. It is
          // converted here, not at the throw site, so the handler search and
          // the reported stack trace see the frame that actually failed.
          auto object = makeRuntimeExceptionObject(fault.className(), fault.what(), &chunk, &state_, "at <runtime>");
          if (!dispatchThrownException(chunk, object, initialCallDepth, ip))
              throw ZlThrownException(std::move(object));
      } catch (const std::runtime_error& e) {
          // Any other interpreter-level failure is still a real ZL error:
          // expose it as `RuntimeError` so programs can catch it by type and
          // read its message and stack trace.
          auto object = makeRuntimeExceptionObject("RuntimeError", e.what(), &chunk, &state_, "at <runtime>");
          if (!dispatchThrownException(chunk, object, initialCallDepth, ip))
              throw ZlThrownException(std::move(object));
      }
    }
}

// Finds the innermost in-scope handler for `thrown`, unwinds to it and moves
// `ip` to its catch block. Returns false when this (possibly nested) run owns
// no matching handler, in which case the caller must propagate.
bool VM::dispatchThrownException(const Chunk& chunk, const ObjectRef& thrown,
                                 std::size_t initialCallDepth, std::size_t& ip) {
    if (!thrown) return false;
    bool matched = false;
    ExecutionState::Handler h{};
    while (state_.hasHandler() && state_.topHandlerCallDepth() >= initialCallDepth) {
        h = state_.popHandler();
        if (h.catchClassName.empty() ||
            thrown->className == h.catchClassName ||
            (chunk.classReflection.count(thrown->className) &&
                isSubclass(chunk, thrown->className, h.catchClassName))) {
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    state_.unwindTo(h);
    state_.removeHandlerGroup(h.groupId);
    if (h.rethrowAfterHandler || !h.catchClassName.empty()) {
        state_.push(Value(thrown));
    } else {
        // An untyped `catch e` binds the failure message, preserving the
        // historical contract for both thrown objects and runtime faults.
        const Value* message = objectFieldLookup(*thrown, "message");
        state_.push(message ? *message : Value(valueToString(Value(thrown))));
    }
    ip = h.catchIp;
    return true;
}

TaskRef VM::scheduleAsyncInvocation(std::shared_ptr<const Chunk> chunk, std::size_t functionIndex,
                                    ExecutionState::CallFrame frame) {
    auto task = std::make_shared<RuntimeTaskState>(frame.returnTypeName);
    // Spawn relation: a task created while another task's body runs belongs
    // to it, and that task's cancellation cascades here. The thread-local
    // covers bodies running on foreign threads (a Task.spawn closure that
    // calls an async func); the invocation's own task covers every other
    // call made from inside an async body. A synchronous frame outside any
    // task has no parent to record, and gets none.
    if (RuntimeTaskState* parent = gCurrentSpawningTask) parent->addSpawnedChild(task);
    else if (asyncInvocation_ && asyncInvocation_->task) asyncInvocation_->task->addSpawnedChild(task);
    auto child = std::make_shared<VM>(scheduler_);
    frame.returnIp = chunk->code.size();
    child->asyncInvocation_ = AsyncInvocation{std::move(chunk), functionIndex, std::move(frame), task, 0, false, {}};
    // A queued/suspended VM owns roots but is not a running mutator.
    child->beginBlockingNativeCall();
    scheduler_->enqueue(std::make_shared<AsyncFrame>([child]() { child->resumeAsyncInvocation(); }));
    return task;
}

void VM::resumeAsyncInvocation() {
    if (!asyncInvocation_) return;
    endBlockingNativeCall();
    struct SuspendGuard {
        VM* vm;
        ~SuspendGuard() { vm->beginBlockingNativeCall(); }
    } suspendGuard{this};
    try {
        if (asyncInvocation_->awaitedTask && asyncInvocation_->awaitedTask->isTerminal()) {
            try {
                state_.push(asyncInvocation_->awaitedTask->observe());
            } catch (...) {
                pendingResumeException_ = std::current_exception();
            }
            asyncInvocation_->awaitedTask.reset();
        }
        if (!asyncInvocation_->started) {
            const TaskStatus taskStatus = asyncInvocation_->task->status();
            if (taskStatus == TaskStatus::Cancelled) {
                asyncInvocation_.reset();
                return;
            }
            if (taskStatus != TaskStatus::Pending) {
                throw std::logic_error("VM: invalid initial async task state");
            }
            asyncInvocation_->task->start();
            state_.enterFrame(std::move(asyncInvocation_->frame));
            asyncInvocation_->resumeIp = asyncInvocation_->chunk->functions[asyncInvocation_->functionIndex].entryAddress;
            asyncInvocation_->started = true;
        }
        if (asyncInvocation_->task->cancellationRequested()) {
            pendingResumeException_ = std::make_exception_ptr(ZlThrownException(makeCancellationException(*asyncInvocation_->chunk)));
        }
        Value result;
        ExecuteStatus status;
        // Mark this thread as running the task's body for the duration of the
        // step, so a task this body spawns (an async call, a Task.spawn) is
        // recorded as its child and takes part in its cancellation cascade.
        {
            CurrentSpawningTaskGuard spawnGuard{asyncInvocation_->task.get()};
            // Pass the invocation's chunk owner along so closures and nested async
            // calls inside the async body share it instead of copying per closure.
            status = execute(*asyncInvocation_->chunk, asyncInvocation_->resumeIp, true, {},
                             &result, asyncInvocation_->chunk);
        }
        if (status == ExecuteStatus::Completed) {
            auto task = asyncInvocation_->task;
            asyncInvocation_.reset();
            task->succeed(std::move(result));
        }
    } catch (const ZlThrownException& e) {
        auto task = asyncInvocation_->task;
        asyncInvocation_.reset();
        // Cancellation/terminal completion may win a race after the VM began
        // resuming this frame. Never turn that stale completion into a second
        // terminal transition (and never leak its logic_error to the scheduler).
        if (task->isTerminal()) return;
        if (e.value() && e.value()->className == "CancellationException") {
            try { task->cancel(); } catch (const std::logic_error&) {}
        } else {
            try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
        }
    } catch (...) {
        auto task = asyncInvocation_->task;
        asyncInvocation_.reset();
        if (task->isTerminal()) return;
        try { task->fail(std::current_exception()); } catch (const std::logic_error&) {}
    }
}


Value VM::invokeTaskClosure(const ClosureRef& closure) {
    if (!closure || !closure->chunk || closure->functionIndex >= closure->chunk->functions.size())
        throw std::runtime_error("VM: invalid task closure");
    if (closure->isAsync) throw std::runtime_error("VM: task closure must be synchronous");
    // The closure's own chunk pointer is the shared owner, so closures made
    // inside the thread/async body share the chunk rather than copying it.
    return invokeFunction(*closure->chunk, closure->functionIndex, {}, std::nullopt, closure,
                          closure->chunk);
}

Value VM::invokeReflectiveFunction(const Value& functionValue, const Value& argsList) {
    auto functionObj = std::get_if<ObjectRef>(&functionValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!functionObj || !(*functionObj) || (*functionObj)->className != "Function" || !list || !(*list))
        throwReflectionException("ReflectionError.InvalidArguments: malformed function invocation", activeChunk_, &state_);
    const Value* closureField = objectFieldLookup(**functionObj, "__closure");
    if (closureField == nullptr) throwReflectionException("ReflectionError: invalid function handle", activeChunk_, &state_);
    auto closure = std::get_if<ClosureRef>(closureField);
    if (!closure || !(*closure) || !(*closure)->chunk)
        throwReflectionException("ReflectionError: invalid function closure", activeChunk_, &state_);
    if ((*closure)->functionIndex == Chunk::INVALID_FUNCTION_INDEX ||
        (*closure)->functionIndex >= (*closure)->chunk->functions.size())
        throwReflectionException("ReflectionError: invalid function index", activeChunk_, &state_);
    const auto& fn = (*closure)->chunk->functions[(*closure)->functionIndex];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string expected = i < (*closure)->parameterTypeNames.size()
            ? (*closure)->parameterTypeNames[i] : "unknown";
        if (!runtimeAssignableToType(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    if (fn.isAsync) {
        auto frame = makeCallFrame(*(*closure)->chunk, fn, values, std::nullopt, *closure);
        return scheduleAsyncInvocation((*closure)->chunk, (*closure)->functionIndex, std::move(frame));
    }
    return invokeFunction(*(*closure)->chunk, (*closure)->functionIndex, values, std::nullopt, *closure,
                          (*closure)->chunk);
}

Value VM::invokeReflectiveMethod(const Value& methodValue, const Value& receiver, const Value& argsList) {
    auto methodObj = std::get_if<ObjectRef>(&methodValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!methodObj || !*methodObj || (*methodObj)->className != "Method" || !list || !*list)
        throwReflectionException("ReflectionError.InvalidArguments: malformed method invocation", activeChunk_, &state_);
    const Value* fit = objectFieldLookup(**methodObj, "__functionIndex");
    const Value* ait = objectFieldLookup(**methodObj, "access");
    const Value* sit = objectFieldLookup(**methodObj, "static");
    if (fit == nullptr || !std::holds_alternative<std::int64_t>(*fit))
        throwReflectionException("ReflectionError: invalid method handle", activeChunk_, &state_);
    const auto access = ait != nullptr && std::holds_alternative<std::string>(*ait) ? std::get<std::string>(*ait) : "public";
    if (access != "public") throwReflectionException("ReflectionError.AccessViolation: method is not public", activeChunk_, &state_);
    const bool isStatic = sit != nullptr && std::holds_alternative<bool>(*sit) && std::get<bool>(*sit);
    if (!isStatic && (!std::holds_alternative<ObjectRef>(receiver) || !std::get<ObjectRef>(receiver)))
        throwReflectionException("ReflectionError.InvalidArguments: instance method requires object receiver", activeChunk_, &state_);
    const auto index = static_cast<std::size_t>(std::get<std::int64_t>(*fit));
    if (!activeChunk_ || index == Chunk::INVALID_FUNCTION_INDEX || index >= activeChunk_->functions.size())
        throwReflectionException("ReflectionError: invalid method function index", activeChunk_, &state_);
    const auto& fn = activeChunk_->functions[index];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    const auto bindings = !isStatic
        ? receiverTypeBindings(*std::get<ObjectRef>(receiver), *activeChunk_, fn.ownerClassName)
        : RuntimeTypeBindings{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string expected = i < fn.parameterTypeNames.size()
            ? substituteTypeParams(fn.parameterTypeNames[i], bindings) : "unknown";
        if (!runtimeAssignableToType(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    if (fn.isAsync) {
        auto frame = makeCallFrame(*activeChunk_, fn, values, isStatic ? std::nullopt : std::optional<Value>(receiver));
        return scheduleAsyncInvocation(shareActiveChunk(), index, std::move(frame));
    }
    return invokeFunction(*activeChunk_, index, values, isStatic ? std::nullopt : std::optional<Value>(receiver));
}

Value VM::invokeReflectiveConstructor(const Value& constructorValue, const Value& argsList) {
    auto ctorObj = std::get_if<ObjectRef>(&constructorValue);
    auto list = std::get_if<ListRef>(&argsList);
    if (!ctorObj || !*ctorObj || (*ctorObj)->className != "Constructor" || !list || !*list)
        throwReflectionException("ReflectionError.InvalidArguments: malformed constructor invocation", activeChunk_, &state_);
    const Value* fit = objectFieldLookup(**ctorObj, "__functionIndex");
    const Value* oit = objectFieldLookup(**ctorObj, "__owner");
    const Value* ait = objectFieldLookup(**ctorObj, "__access");
    if (fit == nullptr || !std::holds_alternative<std::int64_t>(*fit) || oit == nullptr || !std::holds_alternative<std::string>(*oit))
        throwReflectionException("ReflectionError: invalid constructor handle", activeChunk_, &state_);
    const auto access = ait != nullptr && std::holds_alternative<std::string>(*ait) ? std::get<std::string>(*ait) : "public";
    if (access != "public") throwReflectionException("ReflectionError.AccessViolation: constructor is not public", activeChunk_, &state_);
    const auto index = static_cast<std::size_t>(std::get<std::int64_t>(*fit));
    if (!activeChunk_ || index == Chunk::INVALID_FUNCTION_INDEX || index >= activeChunk_->functions.size())
        throwReflectionException("ReflectionError: invalid constructor function index", activeChunk_, &state_);
    const auto& fn = activeChunk_->functions[index];
    const auto& values = (*list)->items;
    if (fn.paramNames.size() != values.size())
        throwReflectionException("ReflectionError.InvalidArguments: expected " + std::to_string(fn.paramNames.size()) + " argument(s), got " + std::to_string(values.size()), activeChunk_, &state_);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::string expected = i < fn.parameterTypeNames.size() ? fn.parameterTypeNames[i] : "unknown";
        if (!runtimeAssignableToType(values[i], expected, activeChunk_))
            throwReflectionException("ReflectionError.InvalidArguments: argument type mismatch at index " + std::to_string(i), activeChunk_, &state_);
    }
    Value object = makeEmptyObject(std::get<std::string>(*oit));
    auto obj = std::get<ObjectRef>(object);
    auto meta = activeChunk_->classReflection.find(obj->className);
    if (meta != activeChunk_->classReflection.end()) obj->runtimeType = meta->second.runtimeType;
    (void)invokeFunction(*activeChunk_, index, values, object);
    return object;
}

Value VM::invokeFunction(const Chunk& chunk, std::size_t functionIndex,
                         const std::vector<Value>& args, const std::optional<Value>& receiver,
                         const ClosureRef& closure, std::shared_ptr<const Chunk> owner) {
    if (functionIndex >= chunk.functions.size()) {
        throw std::runtime_error("VM: function index out of bounds");
    }
    const FunctionInfo& fn = chunk.functions[functionIndex];
    auto frame = makeCallFrame(chunk, fn, args, receiver, closure);
    frame.returnIp = chunk.code.size();
    const std::size_t callerFrames = state_.callDepth();
    const std::size_t callerStackSize = state_.valueStackSize();
    state_.enterFrame(std::move(frame));
    Value result;
    try {
        const ExecuteStatus status = execute(chunk, fn.entryAddress, true, {}, &result, std::move(owner));
        if (status != ExecuteStatus::Completed) throw std::runtime_error("VM: function execution suspended unexpectedly");
    } catch (...) {
        state_.restoreToDepth(callerStackSize, callerFrames);
        throw;
    }
    return result;
}

} // namespace zl
