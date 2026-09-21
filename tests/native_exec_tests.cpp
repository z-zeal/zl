// Native execution driver regressions: `zl::native::NativeExecutable` - the
// loader `--run-native` runs on. Two other files cover neighbouring ground and
// neither one subsumes this: `zl-native-backend-tests` proves the emitted
// bytes are correct through its own in-test buffer, and the
// `native-exec-parity` ctest proves the CLI end-to-end against the VM. What
// lives here is the driver's *own* contract: the arena mapping and the
// inter-function relocation binding it must perform for real (a program whose
// native caller calls a native callee is exactly the shape only this loader,
// not the per-function test buffer, has to make work); name resolution with
// bare tokens, exact names, ambiguity, and the miss list; the shape classifier
// that decides int64 / double / refuse; and the refusals themselves, which
// must be decided from the signature *before* anything runs.
#include "zl/mir/builder.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/native/exec.hpp"
#include "zl/native/pipeline.hpp"

#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) && !defined(__APPLE__) && defined(__x86_64__)
#define ZL_EXEC_TESTS_CAN_EXECUTE 1
#endif

namespace {

using namespace zl;
using namespace zl::mir;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native exec driver regression: " << message << '\n';
    ++failures;
}

zl::native::PipelineResult compile(const Module& module) {
    const auto report = verifyModule(module);
    require(report.ok(), "fixture MIR must verify:\n" + report.describe());
    return zl::native::compileMirToNative(module, zl::native::x64SysVTarget());
}

// int Mod.double(int x) { return x * 2 }   and the caller below it.
Module buildCrossCall() {
    ModuleBuilder builder("cross");
    const auto intType = builder.types().intType();
    const auto two = builder.constantInt(2);

    const FunctionId dblId = [&] {
        auto fn = builder.addFunction("Mod.double(int)");
        const auto x = fn.addParameter("x", intType);
        fn.setReturnType(intType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto product = fn.emitBinary(Opcode::Mul, fn.parameterOperand(x),
                                           Operand::constant(two, intType), intType);
        fn.emitReturn(Operand::temp(product, intType));
        return fn.function().id;
    }();

    auto caller = builder.addFunction("Mod.twice(int)");
    const auto x = caller.addParameter("x", intType);
    caller.setReturnType(intType);
    const auto entry = caller.addBlock();
    caller.setCurrentBlock(entry);
    const auto first = caller.emitCall(dblId, {caller.parameterOperand(x)}, intType);
    const auto second = caller.emitCall(dblId, {caller.parameterOperand(x)}, intType);
    const auto sum = caller.emitBinary(Opcode::Add, Operand::temp(first, intType),
                                       Operand::temp(second, intType), intType);
    caller.emitReturn(Operand::temp(sum, intType));
    return builder.take();
}

// double Mod.scale(double x) { return x * 2.5 - 1.0 }
// int Mod.mix(int a, double b) { return a }   <- mixed register file
Module buildShapes() {
    ModuleBuilder builder("shapes");
    const auto intType = builder.types().intType();
    const auto doubleType = builder.types().doubleType();
    const auto k25 = builder.constantDouble(2.5);
    const auto k1 = builder.constantDouble(1.0);

    {
        auto fn = builder.addFunction("Mod.scale(double)");
        const auto x = fn.addParameter("x", doubleType);
        fn.setReturnType(doubleType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto scaled = fn.emitBinary(Opcode::Mul, fn.parameterOperand(x),
                                          Operand::constant(k25, doubleType), doubleType);
        const auto shifted = fn.emitBinary(Opcode::Sub, Operand::temp(scaled, doubleType),
                                           Operand::constant(k1, doubleType), doubleType);
        fn.emitReturn(Operand::temp(shifted, doubleType));
    }
    {
        auto fn = builder.addFunction("Mod.mix(int,double)");
        const auto a = fn.addParameter("a", intType);
        const auto b = fn.addParameter("b", doubleType);
        (void)b; // present to make the signature mixed; the body returns `a`
        fn.setReturnType(intType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        fn.emitReturn(fn.parameterOperand(a));
    }
    return builder.take();
}

void testResolve() {
    const auto result = compile(buildCrossCall());
    require(result.code.size() == 2, "both cross-calling functions are in the subset");
    zl::native::NativeExecutable exec(result.code, result.lir);
#if defined(ZL_EXEC_TESTS_CAN_EXECUTE)
    require(exec.ok(), "the driver maps the module executable: " + exec.error());
    std::string error;
    require(exec.resolve("Mod.twice(int)", error) != zl::native::NativeExecutable::kNoEntry,
            "an exact name resolves");
    require(exec.resolve("twice", error) != zl::native::NativeExecutable::kNoEntry,
            "a bare method token resolves");
    require(exec.resolve("nope", error) == zl::native::NativeExecutable::kNoEntry &&
                error.find("no native function matches") != std::string::npos &&
                error.find("Mod.double(int)") != std::string::npos,
            "a miss names the compiled set");
    // A second module exercises resolution with a different name set: the
    // bare-token path must work wherever the exact-name path does.
    const auto both = compile(buildShapes());
    zl::native::NativeExecutable execBoth(both.code, both.lir);
    require(execBoth.ok(), "the shapes module maps: " + execBoth.error());
    std::string ambiguous;
    require(execBoth.resolve("scale", ambiguous) != zl::native::NativeExecutable::kNoEntry,
            "bare token 'scale' resolves in the shapes module");
#else
    require(!exec.ok() && exec.error().find("unsupported platform") != std::string::npos,
            "off x86-64 Linux the driver says so rather than pretending: " + exec.error());
#endif
}

void testCrossFunctionCall() {
#if !defined(ZL_EXEC_TESTS_CAN_EXECUTE)
    require(true, "execution skipped off x86-64 Linux");
    return;
#else
    const auto result = compile(buildCrossCall());
    zl::native::NativeExecutable exec(result.code, result.lir);
    require(exec.ok(), "driver maps the cross-call module");
    std::string error;
    const std::size_t twice = exec.resolve("twice", error);
    require(twice != zl::native::NativeExecutable::kNoEntry, "twice resolves");
    std::int64_t value = 0;
    require(exec.callInt64(twice, {21}, value, error) && value == 84,
            "twice(21) == 4 through a real bound direct call, got " + std::to_string(value));
    require(exec.callInt64(twice, {}, value, error) == false &&
                error.find("takes 1 argument(s), got 0") != std::string::npos,
            "wrong arity is refused by count, not by crash: " + error);
#endif
}

void testShapes() {
#if !defined(ZL_EXEC_TESTS_CAN_EXECUTE)
    require(true, "execution skipped off x86-64 Linux");
    return;
#else
    const auto result = compile(buildShapes());
    require(result.code.size() == 2, "scale and mix are both emittable");
    zl::native::NativeExecutable exec(result.code, result.lir);
    require(exec.ok(), "shapes module maps: " + exec.error());
    std::string error;
    const std::size_t scale = exec.resolve("scale", error);
    const std::size_t mix = exec.resolve("mix", error);
    std::string reason;
    require(exec.shape(scale, reason) == zl::native::NativeExecutable::Shape::kDouble,
            "all-double signature classifies as kDouble");
    require(exec.shape(mix, reason) == zl::native::NativeExecutable::Shape::kUnsupported &&
                reason.find("all-integer or all-double") != std::string::npos,
            "the mixed signature is refused with its shape printed: " + reason);
    double value = 0.0;
    require(exec.callDouble(scale, {2.0}, value, error) && value == 4.0,
            "scale(2.0) == 2.0*2.5-1.0 exactly, got " + std::to_string(value));
    std::int64_t misuse = 0;
    require(exec.callInt64(scale, {2}, misuse, error) == false &&
                error.find("all-integer") != std::string::npos,
            "callInt64 refuses the double entry by name: " + error);
    require(exec.callDouble(mix, {1.0}, value, error) == false,
            "callDouble refuses the mixed entry");
#endif
}

} // namespace

int main() {
    testResolve();
    testCrossFunctionCall();
    testShapes();
#if !defined(ZL_EXEC_TESTS_CAN_EXECUTE)
    std::cerr << "note: executable memory unavailable on this platform; execution cases skipped\n";
#endif
    if (failures != 0) {
        std::cerr << failures << " native exec driver regression(s)\n";
        return 1;
    }
    std::cout << "native exec driver tests passed\n";
    return 0;
}
