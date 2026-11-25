// Native backend regressions: MIR -> native IR -> machine code -> *executed*.
//
// The whole pipeline is exercised end to end, and the last step is the point:
// the emitted bytes are mapped executable and called through a function
// pointer, then compared against the value ZL's own semantics say the program
// computes. A backend test that only inspects the IR proves the backend agrees
// with itself; running the code is what proves it agrees with the language.
//
// MIR is built through ModuleBuilder here rather than by lowering ZL source,
// for the same reason mir_tests.cpp does: it pins the backend's contract with
// *MIR* independently of whatever the front end currently produces. The
// end-to-end direction (real ZL through the real lowerer) is covered by the
// last group of cases.

#include "zl/mir/builder.hpp"
#include "zl/mir/lowering.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/compiler/module_loader.hpp"
#include "zl/compiler/type_checker.hpp"
#include "zl/native/exec.hpp"
#include "zl/native/pipeline.hpp"
#include "zl/vm/gc.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// Executing the emitted code needs a host the backend can target *and* a
// platform that permits it. Neither half is satisfied on macOS.
//
// The backend emits x86-64 only (SysV and Win64), while the CI macOS runners
// are arm64, so there is nothing runnable to jump to. And macOS forbids this
// pattern even on matching hardware: executable memory must be mapped with
// MAP_JIT and toggled with pthread_jit_write_protect_np on arm64, and the
// hardened runtime rejects mprotect() to PROT_EXEC on anonymous memory
// elsewhere. This test maps with plain mmap() and then mprotect()s to
// PROT_EXEC, so on macOS the jump faults and CTest reports
// "***Exception: SegFault" - which reads as a crash in the test rather than
// as an unsupported platform.
//
// tests/machine_code_tests.cpp already required __x86_64__ before checking
// for a POSIX host; the execution guards now agree.
#if defined(__unix__) && !defined(__APPLE__) && defined(__x86_64__)
#include <sys/mman.h>
#include <sys/wait.h>
// fork() and _exit() live in <unistd.h>. On glibc, <sys/wait.h> pulls it in
// transitively, so the omission is invisible on Linux; on macOS it is not,
// and the build fails with "no member named 'fork' in the global namespace".
// Included explicitly rather than relying on that transitive path.
#include <unistd.h>
#include <cerrno>
#define ZL_NATIVE_CAN_EXECUTE 1
#else
#define ZL_NATIVE_CAN_EXECUTE 0
#endif

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native backend regression: " << message << '\n';
    ++failures;
}

using namespace zl;
using namespace zl::mir;
using namespace zl::native;

// --- executable memory -----------------------------------------------------
//
// One page per function is wasteful and exactly right for a test: it keeps
// every entry point at a known address so the relocation of a direct call is
// a real rel32 patch rather than an accident of layout.
#if ZL_NATIVE_CAN_EXECUTE
class ExecutableBuffer {
public:
    explicit ExecutableBuffer(const std::vector<EmittedFunction>& functions) {
        std::size_t total = 0;
        for (const auto& fn : functions) total += (fn.code.size() + 4095) / 4096 * 4096;
        if (total == 0) total = 4096;
        base_ = static_cast<std::uint8_t*>(
            ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        size_ = total;
        if (base_ == MAP_FAILED) { base_ = nullptr; return; }

        std::size_t offset = 0;
        for (const auto& fn : functions) {
            std::memcpy(base_ + offset, fn.code.data(), fn.code.size());
            entries_.push_back({fn.name, base_ + offset});
            offset += (fn.code.size() + 4095) / 4096 * 4096;
        }
        // Bind direct calls now that every entry point has an address.
        offset = 0;
        for (const auto& fn : functions) {
            for (const auto& reloc : fn.relocations) {
                std::uintptr_t targetAddress = reinterpret_cast<std::uintptr_t>(entry(reloc.symbol));
                if (targetAddress == 0 && reloc.symbol == "__zl_native_safepoint_enter")
                    targetAddress = detail::safepointEnterAddress();
                if (targetAddress == 0 && reloc.symbol == "__zl_native_safepoint_leave")
                    targetAddress = detail::safepointLeaveAddress();
                if (targetAddress == 0) { ok_ = false; continue; }
                std::uint8_t* site = base_ + offset + reloc.offset;
                if (reloc.kind == EmittedFunction::Relocation::Kind::Absolute64) {
                    const std::uint64_t address = targetAddress;
                    std::memcpy(site, &address, sizeof(address));
                } else {
                    const auto distance = static_cast<std::int64_t>(targetAddress) -
                        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site) + 4);
                    if (distance < INT32_MIN || distance > INT32_MAX) { ok_ = false; continue; }
                    const std::int32_t delta = static_cast<std::int32_t>(distance);
                    std::memcpy(site, &delta, sizeof(delta));
                }
            }
            offset += (fn.code.size() + 4095) / 4096 * 4096;
        }
        ok_ = ok_ && ::mprotect(base_, size_, PROT_READ | PROT_EXEC) == 0;
    }

    ~ExecutableBuffer() { if (base_ != nullptr) ::munmap(base_, size_); }
    ExecutableBuffer(const ExecutableBuffer&) = delete;
    ExecutableBuffer& operator=(const ExecutableBuffer&) = delete;

    [[nodiscard]] bool ok() const { return ok_ && base_ != nullptr; }

    [[nodiscard]] std::uint8_t* entry(const std::string& name) const {
        for (const auto& e : entries_)
            if (e.first == name) return e.second;
        return nullptr;
    }

private:
    std::uint8_t* base_{nullptr};
    std::size_t size_{0};
    bool ok_{true};
    std::vector<std::pair<std::string, std::uint8_t*>> entries_;
};
#endif

// --- MIR fixtures ----------------------------------------------------------

struct Built {
    Module module;
};

// Compiles a module and returns the pipeline result, asserting it verified.
//
// Selection and encoding are properties of a *target*, not of the host this
// process happens to run on. The only encoder is x86-64 SysV; using
// `hostTarget()` here would make every Windows host (described, no encoder)
// and every arm64 host (no description) fail tests that are about the subset
// rather than about jumping to the bytes. Execution cases still consult
// `hostTarget().encoderAvailable()` before mapping the result executable.
PipelineResult compile(const Module& module) {
    const auto report = verifyModule(module);
    require(report.ok(), "fixture MIR must verify:\n" + report.describe());
    return compileMirToNative(module, x64SysVTarget());
}

bool rejected(const PipelineResult& result, const std::string& function) {
    for (const auto& r : result.vmFunctions)
        if (r.function == function) return true;
    return false;
}

bool lowered(const PipelineResult& result, const std::string& function) {
    for (const auto& n : result.nativeFunctions)
        if (n == function) return true;
    return false;
}

// --- the cases -------------------------------------------------------------

// Target description. The calling convention is data, and the tests read it as
// data: nothing here spells a register number.
void testTargetDescription() {
    const auto& sysv = x64SysVTarget();
    require(sysv.cc.intArgRegs.size() == 6, "System V passes six integer arguments in registers");
    require(sysv.cc.floatArgRegs.size() == 8, "System V passes eight float arguments in registers");
    require(sysv.cc.shadowSpace == 32, "System V x86-64 mandates 32 bytes of shadow space (PSABI 3.2.2)");
    require(sysv.cc.stackAlignment == 16, "System V requires 16-byte stack alignment at a call");
    require(sysv.encoderAvailable(), "the System V encoder exists");

    const auto& win = x64WindowsTarget();
    require(win.cc.intArgRegs.size() == 4, "Win64 passes four arguments in registers");
    require(win.cc.shadowSpace == 32, "Win64 mandates 32 bytes of shadow space");
    require(!win.encoderAvailable(),
            "Win64 has no encoder yet and must say so rather than emitting SysV bytes");

    // A caller-saved register must never also be callee-saved: the whole
    // point of the split is that exactly one side is responsible.
    for (const auto& caller : sysv.cc.callerSaved)
        for (const auto& callee : sysv.cc.calleeSaved)
            require(!(caller == callee), std::string("register ") + caller.name +
                                             " cannot be both caller- and callee-saved");
    // The allocator must never be handed a register the encoder uses as
    // scratch, or a computation would clobber a live value.
    for (const auto& alloc : sysv.allocatableInt)
        for (const auto& scratch : sysv.cc.intScratch)
            require(!(alloc == scratch),
                    std::string("allocatable register ") + alloc.name + " overlaps encoder scratch");
}

void testValueClasses() {
    ModuleBuilder builder("classes");
    const auto& types = builder.types();
    Module& m = builder.mutableModule();
    require(classifyType(m, types.intType()) == ValueClass::Integer, "int is an integer class value");
    require(classifyType(m, types.boolType()) == ValueClass::Integer, "bool is an integer class value");
    require(classifyType(m, types.doubleType()) == ValueClass::Float, "double is a float class value");
    require(classifyType(m, types.voidType()) == ValueClass::Void, "void has no value class");
    require(classifyType(m, types.stringType()) == ValueClass::Reference, "string is a reference");
    require(classifyType(m, types.listType(types.intType())) == ValueClass::Reference,
            "collections are references");
    require(classifyType(m, types.objectType("Point")) == ValueClass::Reference, "objects are references");
    // Fails closed: a type the mapping does not understand must not become an
    // integer, because an untracked reference in a register is undiagnosable.
    require(classifyType(m, types.unknownType()) == ValueClass::Reference,
            "unknown types classify conservatively as references");
}

// int add(int a, int b) { return a + b }
Module buildIntArith() {
    ModuleBuilder builder("arith");
    const auto intType = builder.types().intType();
    auto fn = builder.addFunction("add");
    const auto a = fn.addParameter("a", intType);
    const auto b = fn.addParameter("b", intType);
    fn.setReturnType(intType);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    const auto sum = fn.emitBinary(Opcode::Add, fn.parameterOperand(a), fn.parameterOperand(b), intType);
    fn.emitReturn(Operand::temp(sum, intType));
    return builder.take();
}

// double scale(double x) { return x * 2.5 - 1.0 }
Module buildFloatArith() {
    ModuleBuilder builder("farith");
    const auto d = builder.types().doubleType();
    const auto k25 = builder.constantDouble(2.5);
    const auto k1 = builder.constantDouble(1.0);
    auto fn = builder.addFunction("scale");
    const auto x = fn.addParameter("x", d);
    fn.setReturnType(d);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    const auto product = fn.emitBinary(Opcode::Mul, fn.parameterOperand(x), Operand::constant(k25, d), d);
    const auto diff = fn.emitBinary(Opcode::Sub, Operand::temp(product, d), Operand::constant(k1, d), d);
    fn.emitReturn(Operand::temp(diff, d));
    return builder.take();
}

// int classify(int n) { if (n < 0) return -1 else return 1 }
Module buildBranch() {
    ModuleBuilder builder("branch");
    const auto intType = builder.types().intType();
    const auto boolType = builder.types().boolType();
    const auto zero = builder.constantInt(0);
    const auto minusOne = builder.constantInt(-1);
    const auto one = builder.constantInt(1);
    auto fn = builder.addFunction("classify");
    const auto n = fn.addParameter("n", intType);
    fn.setReturnType(intType);
    const auto entry = fn.addBlock();
    const auto negative = fn.addBlock();
    const auto positive = fn.addBlock();
    fn.setCurrentBlock(entry);
    const auto cmp = fn.emitBinary(Opcode::Lt, fn.parameterOperand(n), Operand::constant(zero, intType), boolType);
    fn.emitBranch(Operand::temp(cmp, boolType), negative, positive);
    fn.setCurrentBlock(negative);
    fn.emitReturn(Operand::constant(minusOne, intType));
    fn.setCurrentBlock(positive);
    fn.emitReturn(Operand::constant(one, intType));
    fn.finish();
    return builder.take();
}

// int sumTo(int n) { var acc = 0; var i = 1; while (i <= n) { acc = acc + i; i = i + 1 }; return acc }
// A loop through mutable slots: the memory form of SSA the backend must handle.
Module buildLoop() {
    ModuleBuilder builder("loop");
    const auto intType = builder.types().intType();
    const auto boolType = builder.types().boolType();
    const auto zero = builder.constantInt(0);
    const auto one = builder.constantInt(1);
    auto fn = builder.addFunction("sumTo");
    const auto n = fn.addParameter("n", intType);
    fn.setReturnType(intType);
    const auto acc = fn.addSlot("acc", intType);
    const auto i = fn.addSlot("i", intType);

    const auto entry = fn.addBlock();
    const auto header = fn.addBlock();
    const auto body = fn.addBlock();
    const auto exit = fn.addBlock();

    fn.setCurrentBlock(entry);
    fn.emitStore(acc, Operand::constant(zero, intType));
    fn.emitStore(i, Operand::constant(one, intType));
    fn.emitJump(header);

    fn.setCurrentBlock(header);
    const auto iv = fn.emitLoad(i);
    const auto cond = fn.emitBinary(Opcode::Le, Operand::temp(iv, intType), fn.parameterOperand(n), boolType);
    fn.emitBranch(Operand::temp(cond, boolType), body, exit);

    fn.setCurrentBlock(body);
    const auto accv = fn.emitLoad(acc);
    const auto iv2 = fn.emitLoad(i);
    const auto next = fn.emitBinary(Opcode::Add, Operand::temp(accv, intType), Operand::temp(iv2, intType), intType);
    fn.emitStore(acc, Operand::temp(next, intType));
    const auto inc = fn.emitBinary(Opcode::Add, Operand::temp(iv2, intType), Operand::constant(one, intType), intType);
    fn.emitStore(i, Operand::temp(inc, intType));
    fn.emitJump(header);

    fn.setCurrentBlock(exit);
    const auto out = fn.emitLoad(acc);
    fn.emitReturn(Operand::temp(out, intType));
    fn.finish();
    return builder.take();
}

// int callee(int x) { return x * 3 }
// int caller(int x) { return callee(x) + 1 }
Module buildCall() {
    ModuleBuilder builder("call");
    const auto intType = builder.types().intType();
    const auto three = builder.constantInt(3);
    const auto one = builder.constantInt(1);

    const FunctionId calleeId = [&] {
        auto fn = builder.addFunction("callee");
        const auto x = fn.addParameter("x", intType);
        fn.setReturnType(intType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto product = fn.emitBinary(Opcode::Mul, fn.parameterOperand(x), Operand::constant(three, intType), intType);
        fn.emitReturn(Operand::temp(product, intType));
        return fn.function().id;
    }();

    auto caller = builder.addFunction("caller");
    const auto x = caller.addParameter("x", intType);
    caller.setReturnType(intType);
    const auto entry = caller.addBlock();
    caller.setCurrentBlock(entry);
    const auto call = caller.emitCall(calleeId, {caller.parameterOperand(x)}, intType);
    const auto sum = caller.emitBinary(Opcode::Add, Operand::temp(call, intType), Operand::constant(one, intType), intType);
    caller.emitReturn(Operand::temp(sum, intType));
    return builder.take();
}

// string greet() { return "hi" } - outside the subset, must be refused by name.
Module buildStringFunction() {
    ModuleBuilder builder("strings");
    const auto stringType = builder.types().stringType();
    const auto hi = builder.constantString("hi");
    auto fn = builder.addFunction("greet");
    fn.setReturnType(stringType);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    fn.emitReturn(Operand::constant(hi, stringType));
    return builder.take();
}

// string identity(string s) { return s } - a reference moved straight through.
// Nothing is done *to* the reference; it is only carried. That is the whole of
// this increment: a managed pointer the tier can hold and hand back without
// the collector ever needing to see it, because the tier cannot allocate and
// cannot call into the runtime.
Module buildRefIdentity() {
    ModuleBuilder builder("refidentity");
    const auto stringType = builder.types().stringType();
    auto fn = builder.addFunction("identity");
    const auto s = fn.addParameter("s", stringType);
    fn.setReturnType(stringType);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    fn.emitReturn(fn.parameterOperand(s));
    return builder.take();
}

// string viaSlot(string s) { var t = s; return t } - a reference through a
// frame slot, which is where a future GC map will have to look.
Module buildRefSlot() {
    ModuleBuilder builder("refslot");
    const auto stringType = builder.types().stringType();
    auto fn = builder.addFunction("viaSlot");
    const auto s = fn.addParameter("s", stringType);
    fn.setReturnType(stringType);
    const auto t = fn.addSlot("t", stringType);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    fn.emitStore(t, fn.parameterOperand(s));
    const auto loaded = fn.emitLoad(t);
    fn.emitReturn(Operand::temp(loaded, stringType));
    return builder.take();
}

// string wrap(string s) { return identity(s) } - a reference passed through a
// native call to another native function.
Module buildRefCaller() {
    ModuleBuilder builder("refcaller");
    const auto stringType = builder.types().stringType();
    const FunctionId calleeId = [&] {
        auto fn = builder.addFunction("identity");
        const auto s = fn.addParameter("s", stringType);
        fn.setReturnType(stringType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        fn.emitReturn(fn.parameterOperand(s));
        return fn.function().id;
    }();
    auto caller = builder.addFunction("wrap");
    const auto s = caller.addParameter("s", stringType);
    caller.setReturnType(stringType);
    const auto entry = caller.addBlock();
    caller.setCurrentBlock(entry);
    const auto first = caller.emitCall(calleeId, {caller.parameterOperand(s)}, stringType);
    const auto second = caller.emitCall(calleeId, {Operand::temp(first, stringType)}, stringType);
    caller.emitReturn(Operand::temp(second, stringType));
    return builder.take();
}

// bool same(string a, string b) { return a == b } - a reference *use*, not a
// move. Equality on a managed value is runtime work (it may walk a collection),
// so it stays outside the subset even once carrying a reference is allowed.
Module buildRefComparison() {
    ModuleBuilder builder("refcmp");
    const auto stringType = builder.types().stringType();
    const auto boolType = builder.types().boolType();
    auto fn = builder.addFunction("same");
    const auto a = fn.addParameter("a", stringType);
    const auto b = fn.addParameter("b", stringType);
    fn.setReturnType(boolType);
    const auto entry = fn.addBlock();
    fn.setCurrentBlock(entry);
    const auto eq = fn.emitBinary(Opcode::Eq, fn.parameterOperand(a), fn.parameterOperand(b), boolType);
    fn.emitReturn(Operand::temp(eq, boolType));
    return builder.take();
}

void testSelectionSubset() {
    {
        auto result = compile(buildIntArith());
        require(result.ok(), "integer arithmetic compiles: " + result.error);
        require(lowered(result, "add"), "add is in the native subset");
        require(result.lir.functions.size() == 1, "one native function");
        if (result.lir.functions.empty()) return;
        const auto& fn = result.lir.functions.front();
        require(fn.returnClass == ValueClass::Integer, "add returns an integer-class value");
        require(fn.parameterClasses.size() == 2, "add takes two parameters");
        require(fn.vregs[fn.parameters[0]].fixed.has_value(),
                "a parameter is constrained to its ABI register at selection time");
    }
    {
        auto result = compile(buildStringFunction());
        require(result.ok(), "a module with an unsupported function is not an error");
        require(rejected(result, "greet"), "a string-returning function is refused, not guessed at");
        require(result.lir.functions.empty(), "nothing was lowered for it");
        require(!result.vmFunctions.empty() && !result.vmFunctions.front().reason.empty(),
                "the refusal states a reason");
    }
    {
        // A caller of a refused function must itself be refused: there is no
        // native body to call.
        ModuleBuilder builder("mixed");
        const auto intType = builder.types().intType();
        const auto stringType = builder.types().stringType();
        const FunctionId refusedId = [&] {
            auto fn = builder.addFunction("stringy");
            fn.setReturnType(stringType);
            const auto entry = fn.addBlock();
            fn.setCurrentBlock(entry);
            fn.emitReturn(Operand::constant(builder.constantString("x"), stringType));
            return fn.function().id;
        }();
        (void)refusedId;
        const FunctionId ok = [&] {
            auto fn = builder.addFunction("plain");
            fn.setReturnType(intType);
            const auto entry = fn.addBlock();
            fn.setCurrentBlock(entry);
            fn.emitReturn(Operand::constant(builder.constantInt(7), intType));
            return fn.function().id;
        }();
        auto caller = builder.addFunction("usesPlain");
        caller.setReturnType(intType);
        const auto entry = caller.addBlock();
        caller.setCurrentBlock(entry);
        const auto call = caller.emitCall(ok, {}, intType);
        caller.emitReturn(Operand::temp(call, intType));

        auto result = compile(builder.take());
        require(lowered(result, "plain") && lowered(result, "usesPlain"),
                "a call to a natively-compiled function stays native");
        require(rejected(result, "stringy"), "the unsupported function is still refused");
    }
}

void testUnverifiedMirIsRefused() {
    // A module the verifier rejects must not be compiled: the backend's
    // licence to assume the invariants is only sound if something checks.
    Module broken = buildIntArith();
    broken.functions[0].blocks[0].terminator.kind = TerminatorKind::None;
    auto result = compileMirToNative(broken, x64SysVTarget());
    require(!result.ok(), "the native backend refuses MIR that does not verify");
    require(result.error.find("did not verify") != std::string::npos,
            "the refusal says the MIR did not verify");
}

// A described target without an encoder (Win64) is select-only: the subset
// still runs, and no bytes are produced. Turning that into a hard error made
// every Windows host fail this suite even though refusal-to-emit is the
// documented, correct outcome.
void testNoEncoderIsSelectOnly() {
    auto result = compileMirToNative(buildIntArith(), x64WindowsTarget());
    require(result.ok(),
            "a described target without an encoder is not a pipeline error: " + result.error);
    require(lowered(result, "add"), "Win64 can still select the integer subset");
    require(result.code.empty(), "Win64 produces no bytes until it has an encoder");
}

#if ZL_NATIVE_CAN_EXECUTE
template <typename Fn>
Fn* entryPoint(const ExecutableBuffer& buffer, const std::string& name) {
    return reinterpret_cast<Fn*>(buffer.entry(name));
}

void testExecution() {
    if (!hostTarget().encoderAvailable()) {
        std::cerr << "note: host has no native encoder; execution cases skipped\n";
        return;
    }
    {
        auto result = compile(buildIntArith());
        require(result.ok() && !result.code.empty(), "integer arithmetic emitted code");
        ExecutableBuffer buffer(result.code);
        require(buffer.ok(), "executable mapping succeeded");
        if (buffer.ok()) {
            auto* add = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "add");
            require(add != nullptr && add(2, 40) == 42, "add(2, 40) == 42");
            require(add(-5, 5) == 0, "add(-5, 5) == 0");
        }
    }
    {
        auto result = compile(buildFloatArith());
        require(result.ok() && !result.code.empty(), "float arithmetic emitted code: " + result.error);
        ExecutableBuffer buffer(result.code);
        if (buffer.ok()) {
            auto* scale = entryPoint<double(double)>(buffer, "scale");
            require(scale != nullptr && std::fabs(scale(4.0) - 9.0) < 1e-12, "scale(4.0) == 9.0");
            require(std::fabs(scale(0.0) + 1.0) < 1e-12, "scale(0.0) == -1.0");
        }
    }
    {
        auto result = compile(buildBranch());
        require(result.ok() && !result.code.empty(), "branching emitted code: " + result.error);
        ExecutableBuffer buffer(result.code);
        if (buffer.ok()) {
            auto* classify = entryPoint<std::int64_t(std::int64_t)>(buffer, "classify");
            require(classify != nullptr && classify(-9) == -1, "classify(-9) == -1");
            require(classify(0) == 1, "classify(0) == 1");
            require(classify(17) == 1, "classify(17) == 1");
        }
    }
    {
        auto result = compile(buildLoop());
        require(result.ok() && !result.code.empty(), "the loop emitted code: " + result.error);
        ExecutableBuffer buffer(result.code);
        if (buffer.ok()) {
            auto* sumTo = entryPoint<std::int64_t(std::int64_t)>(buffer, "sumTo");
            require(sumTo != nullptr && sumTo(10) == 55, "sumTo(10) == 55");
            require(sumTo(0) == 0, "sumTo(0) == 0 (loop body never runs)");
            require(sumTo(100) == 5050, "sumTo(100) == 5050");
        }
    }
    {
        auto result = compile(buildCall());
        require(result.ok() && result.code.size() == 2, "both functions emitted code: " + result.error);
        ExecutableBuffer buffer(result.code);
        if (buffer.ok()) {
            auto* caller = entryPoint<std::int64_t(std::int64_t)>(buffer, "caller");
            require(caller != nullptr && caller(5) == 16, "caller(5) == callee(5) + 1 == 16");
        }
    }
}

// The subset boundary for references: carrying one is in, using one is out.
// The three "in" shapes are a parameter, a frame slot, and a call argument;
// the "out" shape is an operation that would need the runtime (equality on a
// managed value). Each is asserted on its own so a regression cannot hide
// behind a neighbour.
void testReferencePassThrough() {
    {
        auto result = compile(buildRefIdentity());
        require(result.ok(), "a reference parameter and result compile: " + result.error);
        require(lowered(result, "identity"), "identity is in the native subset");
        require(result.lir.functions.size() == 1, "one native function");
        if (!result.lir.functions.empty()) {
            const auto& fn = result.lir.functions.front();
            require(fn.parameterClasses.size() == 1 &&
                        fn.parameterClasses[0] == ValueClass::Reference,
                    "the parameter is classified as a reference, not an integer");
            require(fn.returnClass == ValueClass::Reference,
                    "the result is classified as a reference");
        }
    }
    {
        auto result = compile(buildRefSlot());
        require(result.ok(), "a reference through a frame slot compiles: " + result.error);
        require(lowered(result, "viaSlot"), "viaSlot is in the native subset");
        if (!result.lir.functions.empty()) {
            const auto& fn = result.lir.functions.front();
            bool refSlot = false;
            for (const auto& slot : fn.slots)
                if (slot.isReference) refSlot = true;
            require(refSlot, "the reference slot is marked for the frame's GC map");
        }
    }
    {
        auto result = compile(buildRefCaller());
        require(result.ok(), "a reference passed through a call compiles: " + result.error);
        require(lowered(result, "identity") && lowered(result, "wrap"),
                "both the callee and its caller are native");
    }
    {
        auto result = compile(buildRefComparison());
        require(result.ok(), "a module with an unsupported reference use is not an error");
        require(rejected(result, "same"),
                "a reference *use* (equality) is still refused, not guessed at");
        require(!result.vmFunctions.empty() && !result.vmFunctions.front().reason.empty(),
                "the refusal names its reason");
    }
    if (hostTarget().encoderAvailable()) {
        const char* payload = "payload";
        {
            auto result = compile(buildRefIdentity());
            require(result.ok() && !result.code.empty(), "identity emitted code: " + result.error);
            ExecutableBuffer buffer(result.code);
            require(buffer.ok(), "identity mapped executable");
            if (buffer.ok()) {
                auto* identity = entryPoint<const char*(const char*)>(buffer, "identity");
                require(identity != nullptr && identity(payload) == payload,
                        "a reference round-trips through native code unchanged");
            }
        }
        {
            auto result = compile(buildRefSlot());
            require(result.ok() && !result.code.empty(), "viaSlot emitted code: " + result.error);
            ExecutableBuffer buffer(result.code);
            require(buffer.ok(), "viaSlot mapped executable");
            if (buffer.ok()) {
                auto* viaSlot = entryPoint<const char*(const char*)>(buffer, "viaSlot");
                require(viaSlot != nullptr && viaSlot(payload) == payload,
                        "a reference stored and reloaded in a frame slot round-trips");
            }
        }
        {
            auto result = compile(buildRefCaller());
            require(result.ok() && !result.code.empty(), "wrap emitted code: " + result.error);
            ExecutableBuffer buffer(result.code);
            require(buffer.ok(), "wrap mapped executable");
            if (buffer.ok()) {
                auto* wrap = entryPoint<const char*(const char*)>(buffer, "wrap");
                require(wrap != nullptr && wrap(payload) == payload,
                        "a reference passed through a native call round-trips");
            }
        }
    }
}

// Comparisons and division, each checked against the value the language means
// rather than against the encoding the backend happened to pick.
void testArithmeticEdges() {
    if (!hostTarget().encoderAvailable()) return;

    ModuleBuilder builder("edges");
    const auto intType = builder.types().intType();
    const auto boolType = builder.types().boolType();
    const auto d = builder.types().doubleType();

    auto addBinary = [&](const std::string& name, Opcode op, std::uint32_t operandType,
                         std::uint32_t resultType) {
        auto fn = builder.addFunction(name);
        const auto a = fn.addParameter("a", operandType);
        const auto b = fn.addParameter("b", operandType);
        fn.setReturnType(resultType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto r = fn.emitBinary(op, fn.parameterOperand(a), fn.parameterOperand(b), resultType);
        fn.emitReturn(Operand::temp(r, resultType));
    };
    addBinary("idiv", Opcode::Div, intType, intType);
    addBinary("imod", Opcode::Mod, intType, intType);
    addBinary("ilt", Opcode::Lt, intType, boolType);
    addBinary("ige", Opcode::Ge, intType, boolType);
    addBinary("flt", Opcode::Lt, d, boolType);
    addBinary("feq", Opcode::Eq, d, boolType);
    addBinary("fne", Opcode::Ne, d, boolType);

    auto result = compile(builder.take());
    require(result.ok() && result.code.size() == 7, "the arithmetic edge module compiled: " + result.error);
    ExecutableBuffer buffer(result.code);
    if (!buffer.ok()) return;

    auto* idiv = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "idiv");
    auto* imod = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "imod");
    require(idiv(17, 5) == 3, "17 / 5 == 3 (truncating)");
    require(idiv(-17, 5) == -3, "-17 / 5 == -3 (truncates toward zero)");
    require(imod(17, 5) == 2, "17 % 5 == 2");
    require(imod(-17, 5) == -2, "-17 % 5 == -2 (sign follows the dividend)");

    auto* ilt = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "ilt");
    auto* ige = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "ige");
    require(ilt(1, 2) == 1 && ilt(2, 1) == 0 && ilt(2, 2) == 0, "signed < is correct");
    require(ige(2, 2) == 1 && ige(1, 2) == 0, "signed >= is correct");
    require(ilt(-1, 1) == 1, "signed < treats negatives as less than positives");

    // NaN is the case an unsigned-flag comparison gets wrong, which is why it
    // is tested rather than assumed.
    auto* flt = entryPoint<std::int64_t(double, double)>(buffer, "flt");
    auto* feq = entryPoint<std::int64_t(double, double)>(buffer, "feq");
    auto* fne = entryPoint<std::int64_t(double, double)>(buffer, "fne");
    const double nan = std::nan("");
    require(flt(1.0, 2.0) == 1 && flt(2.0, 1.0) == 0, "double < is correct");
    require(flt(nan, 1.0) == 0 && flt(1.0, nan) == 0, "an unordered comparison is false");
    require(feq(1.0, 1.0) == 1 && feq(nan, nan) == 0, "NaN is not equal to itself");
    require(fne(nan, nan) == 1, "NaN is unequal to itself");
    require(feq(0.0, -0.0) == 1, "positive and negative zero compare equal");
}

// --- arithmetic faults: the emitted code must trap, never wrap --------------
//
// The language is fail-closed: the VM throws a catchable ArithmeticError when
// a result is unrepresentable (int overflow, INT64_MIN / -1, a shift count
// outside 0..63, a zero float divisor, an infinity or NaN). The native tier
// has no unwind tables, so its contract for the same fault is a trap - the
// emitted code branches to `ud2`, which terminates the process with SIGILL.
// Each fault is executed in a forked child, so the trap (the expected
// outcome) is an assertion instead of a test-suite death. The healthy
// neighbours are run in the parent: they must not fire.

// Runs `child` in a forked process. Returns the terminating signal when the
// child is killed (SIGILL from `ud2`), the exit code when it exits normally,
// or -1 when the wait itself failed.
int trapOrExitCode(std::function<void()> child) {
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        child();
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFSIGNALED(status)) return WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// Fails with the observed outcome when `observed` is not SIGILL.
void requireTrap(int observed, const std::string& what) {
    const std::string detail = observed == SIGILL ? ""
        : observed < 0 ? " [wait failed]"
        : observed <= 128 ? " [exited cleanly with code " + std::to_string(observed) + "]"
                          : " [killed by signal " + std::to_string(observed) + "]";
    require(observed == SIGILL, what + detail);
}

void testArithmeticTraps() {
    if (!hostTarget().encoderAvailable()) return;

    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

    ModuleBuilder builder("traps");
    const auto intType = builder.types().intType();
    const auto d = builder.types().doubleType();

    auto addBinary = [&](const std::string& name, Opcode op, std::uint32_t operandType,
                         std::uint32_t resultType) {
        auto fn = builder.addFunction(name);
        const auto a = fn.addParameter("a", operandType);
        const auto b = fn.addParameter("b", operandType);
        fn.setReturnType(resultType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto r = fn.emitBinary(op, fn.parameterOperand(a), fn.parameterOperand(b), resultType);
        fn.emitReturn(Operand::temp(r, resultType));
    };
    auto addUnary = [&](const std::string& name, Opcode op, std::uint32_t operandType,
                        std::uint32_t resultType) {
        auto fn = builder.addFunction(name);
        const auto a = fn.addParameter("a", operandType);
        fn.setReturnType(resultType);
        const auto entry = fn.addBlock();
        fn.setCurrentBlock(entry);
        const auto r = fn.emitUnary(op, fn.parameterOperand(a), resultType);
        fn.emitReturn(Operand::temp(r, resultType));
    };

    addBinary("tadd", Opcode::Add, intType, intType);
    addBinary("tsub", Opcode::Sub, intType, intType);
    addBinary("tmul", Opcode::Mul, intType, intType);
    addBinary("tdiv", Opcode::Div, intType, intType);
    addBinary("tmod", Opcode::Mod, intType, intType);
    addBinary("tshl", Opcode::Shl, intType, intType);
    addBinary("tshr", Opcode::Shr, intType, intType);
    addBinary("tushr", Opcode::Ushr, intType, intType);
    addUnary("tneg", Opcode::Neg, intType, intType);
    addBinary("tfadd", Opcode::Add, d, d);
    addBinary("tfmul", Opcode::Mul, d, d);
    addBinary("tfdiv", Opcode::Div, d, d);
    addUnary("tfneg", Opcode::Neg, d, d);

    auto result = compile(builder.take());
    require(result.ok() && result.code.size() == 13, "the trap module compiled: " + result.error);
    ExecutableBuffer buffer(result.code);
    if (!buffer.ok()) return;

    auto* tadd = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tadd");
    auto* tsub = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tsub");
    auto* tmul = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tmul");
    auto* tdiv = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tdiv");
    auto* tmod = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tmod");
    auto* tshl = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tshl");
    auto* tshr = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tshr");
    auto* tushr = entryPoint<std::int64_t(std::int64_t, std::int64_t)>(buffer, "tushr");
    auto* tneg = entryPoint<std::int64_t(std::int64_t)>(buffer, "tneg");
    auto* tfadd = entryPoint<double(double, double)>(buffer, "tfadd");
    auto* tfmul = entryPoint<double(double, double)>(buffer, "tfmul");
    auto* tfdiv = entryPoint<double(double, double)>(buffer, "tfdiv");
    auto* tfneg = entryPoint<double(double)>(buffer, "tfneg");

    // The healthy neighbours: the checks must not fire on representable
    // results, and the boundary values the checks exist for still compute.
    require(tadd(1, 2) == 3, "1 + 2 == 3");
    require(tsub(1, 2) == -1, "1 - 2 == -1");
    require(tmul(-2, 3) == -6, "-2 * 3 == -6");
    require(tdiv(17, 5) == 3, "17 / 5 == 3");
    require(tdiv(kMin, 1) == kMin, "INT64_MIN / 1 == INT64_MIN (no overflow)");
    require(tmod(-17, 5) == -2, "-17 % 5 == -2");
    require(tmod(kMin, 2) == 0, "INT64_MIN % 2 == 0");
    require(tshl(1, 63) == kMin, "1 << 63 == INT64_MIN (count 63 is in range)");
    require(tshr(kMin, 63) == -1, "INT64_MIN >> 63 == -1 (arithmetic)");
    require(tushr(-1, 63) == 1, "-1 >>> 63 == 1 (logical: only bit 0 survives)");
    require(tneg(kMax) == -9223372036854775807, "negation of INT64_MAX is representable");
    require(tfadd(1.5, 2.25) == 3.75, "1.5 + 2.25 == 3.75");
    require(tfmul(2.0, 0.5) == 1.0, "2.0 * 0.5 == 1.0");
    require(tfdiv(1.0, 4.0) == 0.25, "1.0 / 4.0 == 0.25");
    require(tfneg(-3.5) == 3.5, "negation of a finite double is fine");
    // The VM defines INT64_MIN % -1 as 0, so the emitted code computes it
    // instead of trapping - this is the one fault case with a value.
    require(tmod(kMin, -1) == 0, "INT64_MIN % -1 == 0 (the VM's definition), computed, not trapped");

    // And the faults trap - the same way the VM reports them as catchable
    // ArithmeticErrors, except this tier has no unwind tables.
    requireTrap(trapOrExitCode([=] { (void)tadd(kMax, 1); }),
                "INT64_MAX + 1 traps (VM: integer overflow in addition; x86 would wrap to INT64_MIN)");
    requireTrap(trapOrExitCode([=] { (void)tsub(kMin, 1); }),
                "INT64_MIN - 1 traps (VM: integer overflow in subtraction)");
    requireTrap(trapOrExitCode([=] { (void)tmul(kMax, 2); }),
                "INT64_MAX * 2 traps (VM: integer overflow in multiplication)");
    requireTrap(trapOrExitCode([=] { (void)tneg(kMin); }),
                "negation of INT64_MIN traps (VM: integer overflow in negation)");
    requireTrap(trapOrExitCode([=] { (void)tdiv(kMin, -1); }),
                "INT64_MIN / -1 traps (VM: integer overflow in division; idiv would raise #DE)");
    requireTrap(trapOrExitCode([=] { (void)tdiv(7, 0); }),
                "zero divisor traps (the tier's pre-existing contract)");
    requireTrap(trapOrExitCode([=] { (void)tshl(1, 64); }),
                "shift count 64 traps (x86 would mask it to 0 and compute x << 0)");
    requireTrap(trapOrExitCode([=] { (void)tshl(1, -1); }),
                "negative shift count traps (VM: shift count must be in 0..63)");
    requireTrap(trapOrExitCode([=] { (void)tshr(1, 64); }),
                "arithmetic shift count 64 traps");
    requireTrap(trapOrExitCode([=] { (void)tushr(1, -1); }),
                "logical shift count -1 traps");
    requireTrap(trapOrExitCode([=] { (void)tfdiv(1.0, 0.0); }),
                "1.0 / 0.0 traps (VM: division by zero; IEEE would emit +infinity)");
    requireTrap(trapOrExitCode([=] { (void)tfdiv(0.0, 0.0); }),
                "0.0 / 0.0 traps (NaN is not a value the language has)");
    requireTrap(trapOrExitCode([=] { (void)tfadd(1e308, 1e308); }),
                "float addition overflow to infinity traps (VM: floating-point overflow)");
    requireTrap(trapOrExitCode([=] { (void)tfmul(1e200, 1e200); }),
                "float multiplication overflow to infinity traps");
    requireTrap(trapOrExitCode([=] { (void)tfneg(std::numeric_limits<double>::infinity()); }),
                "negating a non-finite input traps - no non-finite value may exist in the tier");
}

extern "C" void* nativeGcAllocate() {
    ObjectRef value = makeGCObject();
    value->className = "native-frame-root-survived";
    return value.get();
}

extern "C" void nativeGcPressure() {
    auto& gc = TracingGC::instance();
    const std::size_t allocations = gc.counters().threshold + 1;
    for (std::size_t i = 0; i < allocations; ++i) (void)makeGCObject();
}

LirFunction nativeReferenceIdentity() {
    const auto& arg = x64SysVTarget().cc.intArgRegs[0];
    LirFunction fn;
    fn.name = "nativeIdentity";
    fn.parameters = {1};
    fn.parameterClasses = {ValueClass::Reference};
    fn.returnClass = ValueClass::Reference;
    fn.vregs.push_back(VRegInfo{});
    VRegInfo parameter;
    parameter.id = 1;
    parameter.cls = ValueClass::Reference;
    parameter.name = "value";
    parameter.fixed = arg;
    fn.vregs.push_back(parameter);
    LirBlock entry;
    entry.id = 1;
    entry.label = "entry";
    LirInstruction ret;
    ret.opcode = LirOpcode::Return;
    ret.operands = {LirOperand::reg(1)};
    entry.instructions.push_back(ret);
    fn.blocks.push_back(entry);
    fn.entry = 1;
    return fn;
}

LirModule nativeGcRootProbeModule() {
    LirFunction wrapper;
    wrapper.name = "nativeRootProbe";
    wrapper.returnClass = ValueClass::Reference;
    wrapper.vregs.push_back(VRegInfo{});
    VRegInfo allocated;
    allocated.id = 1;
    allocated.cls = ValueClass::Reference;
    allocated.name = "allocated";
    wrapper.vregs.push_back(allocated);
    VRegInfo result;
    result.id = 2;
    result.cls = ValueClass::Reference;
    result.name = "identityResult";
    wrapper.vregs.push_back(result);

    LirBlock entry;
    entry.id = 1;
    entry.label = "entry";
    LirInstruction allocate;
    allocate.opcode = LirOpcode::CallRuntime;
    allocate.result = 1;
    allocate.resultClass = ValueClass::Reference;
    allocate.callee = "test.gc.allocate";
    entry.instructions.push_back(allocate);
    LirInstruction pressure;
    pressure.opcode = LirOpcode::CallRuntime;
    pressure.callee = "test.gc.pressure";
    entry.instructions.push_back(pressure);
    LirInstruction call;
    call.opcode = LirOpcode::Call;
    call.result = 2;
    call.resultClass = ValueClass::Reference;
    call.operands = {LirOperand::reg(1)};
    call.argClasses = {ValueClass::Reference};
    call.callee = "nativeIdentity";
    entry.instructions.push_back(call);
    LirInstruction ret;
    ret.opcode = LirOpcode::Return;
    ret.operands = {LirOperand::reg(2)};
    entry.instructions.push_back(ret);
    wrapper.blocks.push_back(entry);
    wrapper.entry = 1;

    LirModule module;
    module.name = "native-gc-root-probe";
    module.functions.push_back(std::move(wrapper));
    module.functions.push_back(nativeReferenceIdentity());
    return module;
}

void testNativeSafepointRootPublication() {
    LirModule module = nativeGcRootProbeModule();
    const auto emitted = emitModule(module, x64SysVTarget());
    require(emitted.success, "GC root probe emits: " + emitted.error);
    if (!emitted.success) return;

    NativeRuntimeBindings bindings;
    bindings["test.gc.allocate"] = reinterpret_cast<std::uintptr_t>(&nativeGcAllocate);
    bindings["test.gc.pressure"] = reinterpret_cast<std::uintptr_t>(&nativeGcPressure);
    NativeExecutable executable(emitted.functions, module, bindings);
    require(executable.ok(), "GC root probe maps and binds: " + executable.error());
    if (!executable.ok()) return;

    std::string error;
    const std::size_t entry = executable.resolve("nativeRootProbe", error);
    require(entry != NativeExecutable::kNoEntry, "GC root probe entry resolves: " + error);
    if (entry == NativeExecutable::kNoEntry) return;

    auto& gc = TracingGC::instance();
    const auto before = gc.counters();
    std::uintptr_t returned = 0;
    const bool called = executable.callReference(entry, {}, returned, error);
    const auto after = gc.counters();
    require(called, "native reference call and collection succeed: " + error);
    require(returned != 0, "native runtime allocator returns a managed object");
    require(after.collections > before.collections,
            "allocation pressure causes a collection at the next native call site");
    ObjectRef keepAlive(reinterpret_cast<ObjectBox*>(returned));
    require(keepAlive->className == "native-frame-root-survived",
            "the collector preserved a runtime-allocated object rooted only in the native frame map");

    auto cleanup = gc.collect(GCRoots{});
    cleanup.reclaim();
}
#endif

// The exported frame layout must agree with the displacement encoded for
// each LIR frame slot. The reference fixture produces a concrete slot read
// and write, so checking both directions pins the metadata to the machine code.
bool hasRbpAccess(const std::vector<std::uint8_t>& code, std::uint8_t opcode,
                  std::int32_t displacement) {
    for (std::size_t i = 0; i + 7 <= code.size(); ++i) {
        if ((code[i] != 0x48 && code[i] != 0x4c) || code[i + 1] != opcode) continue;
        const std::uint8_t modrm = code[i + 2];
        if ((modrm & 0xc7) != 0x85) continue; // [rbp + disp32]
        std::int32_t encoded = 0;
        std::memcpy(&encoded, code.data() + i + 3, sizeof(encoded));
        if (encoded == displacement) return true;
    }
    return false;
}

void testEmittedFrameMap() {
    auto result = compile(buildRefSlot());
    require(result.ok(), "the reference-slot fixture compiles: " + result.error);
    if (!result.ok() || result.lir.functions.empty() || result.code.empty()) return;

    const auto& lir = result.lir.functions.front();
    const auto& emitted = result.code.front();
    require(emitted.name == lir.name, "emitted frame map belongs to the matching function");
    require(emitted.frameLocations.size() == (lir.slots.size() - 1) + (lir.vregs.size() - 1),
            "every LIR slot and vreg spill is exported exactly once");

    bool checkedReference = false;
    for (std::size_t i = 1; i < lir.slots.size(); ++i) {
        const auto& sourceSlot = lir.slots[i];
        const EmittedFrameLocation* layout = nullptr;
        for (const auto& candidate : emitted.frameLocations)
            if (candidate.kind == EmittedFrameLocationKind::FrameSlot &&
                candidate.id == sourceSlot.id) layout = &candidate;
        require(layout != nullptr, "frame map contains slot " + std::to_string(sourceSlot.id));
        if (layout == nullptr) continue;
        require(layout->isReference == sourceSlot.isReference,
                "frame-map reference bit matches LIR slot " + std::to_string(sourceSlot.id));
        require(layout->offset < 0 && layout->offset >= -static_cast<std::int32_t>(emitted.frameSize),
                "frame slot offset is inside the emitted frame");
        if (!sourceSlot.isReference) continue;
        checkedReference = true;
        require(hasRbpAccess(emitted.code, 0x89, layout->offset),
                "reference slot Store uses its exported frame offset");
        require(hasRbpAccess(emitted.code, 0x8b, layout->offset),
                "reference slot Load uses its exported frame offset");
    }
    require(checkedReference, "fixture has a reference frame slot to map");

    bool checkedReferenceSpill = false;
    for (std::size_t i = 1; i < lir.vregs.size(); ++i) {
        const auto& sourceVReg = lir.vregs[i];
        const EmittedFrameLocation* layout = nullptr;
        for (const auto& candidate : emitted.frameLocations)
            if (candidate.kind == EmittedFrameLocationKind::VirtualRegister &&
                candidate.id == sourceVReg.id) layout = &candidate;
        require(layout != nullptr, "frame map contains vreg spill " + std::to_string(sourceVReg.id));
        if (layout == nullptr) continue;
        const bool isReference = sourceVReg.cls == ValueClass::Reference;
        require(layout->isReference == isReference,
                "frame-map reference bit matches vreg " + std::to_string(sourceVReg.id));
        require(layout->offset < 0 && layout->offset >= -static_cast<std::int32_t>(emitted.frameSize),
                "vreg spill offset is inside the emitted frame");
        if (!isReference) continue;
        checkedReferenceSpill = true;
        require(hasRbpAccess(emitted.code, 0x89, layout->offset) ||
                    hasRbpAccess(emitted.code, 0x8b, layout->offset),
                "reference vreg spill offset appears in emitted memory accesses");
    }
    require(checkedReferenceSpill, "fixture has reference vreg spills to map");

    auto calls = compile(buildRefCaller());
    require(calls.ok(), "the multi-call fixture compiles: " + calls.error);
    if (!calls.ok()) return;
    const EmittedFunction* wrap = nullptr;
    for (const auto& function : calls.code)
        if (function.name == "wrap") wrap = &function;
    require(wrap != nullptr, "the call-map fixture emits wrap");
    if (wrap == nullptr) return;

    std::vector<std::int32_t> expectedRoots;
    for (const auto& location : wrap->frameLocations)
        if (location.isReference) expectedRoots.push_back(location.offset);
    require(!expectedRoots.empty(), "the call-map fixture spills managed references");
    require(wrap->safepoints.size() == 2, "both calls have distinct safepoint maps");

    for (std::size_t i = 0; i < wrap->safepoints.size(); ++i) {
        const auto& map = wrap->safepoints[i];
        require(map.callOffset < wrap->code.size() && wrap->code[map.callOffset] == 0xe8,
                "safepoint is keyed by the byte offset of its CALL opcode");
        if (i > 0)
            require(wrap->safepoints[i - 1].callOffset != map.callOffset,
                    "each call site has its own offset key");
        bool relocationMatches = false;
        for (const auto& relocation : wrap->relocations)
            if (relocation.offset == map.callOffset + 1) relocationMatches = true;
        require(relocationMatches, "call-site map offset agrees with its relocation");
        require(map.referenceOffsets.size() == expectedRoots.size(),
                "conservative map reports every reference-bearing frame location");
        for (const auto root : expectedRoots) {
            bool found = false;
            for (const auto offset : map.referenceOffsets)
                if (offset == root) found = true;
            require(found, "conservative map retains every possible managed root");
        }
    }
}

// --- the real front end ----------------------------------------------------
//
// The pipeline test that matters: real ZL source, the real lowerer, the real
// verifier, then the native backend. It proves the backend consumes what the
// language actually produces, and that unsupported programs degrade to a
// refusal instead of a crash.
Module lowerSource(const std::string& name, const std::string& source, bool& ok) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("zl_native_" + name);
    fs::create_directories(directory);
    const fs::path file = directory / (name + ".zl");
    {
        std::ofstream out(file);
        out << source;
    }
    zl::ModuleLoader loader(file, std::vector<fs::path>{});
    auto program = loader.load();
    zl::TypeChecker checker;
    checker.check(*program, /*requireMain=*/false);
    auto lowered = lowerProgram(*program, checker);
    const auto report = verifyModule(lowered.module);
    ok = report.ok();
    if (!ok) std::cerr << "lowering did not verify:\n" << report.describe();
    return std::move(lowered.module);
}

void testEndToEnd() {
    bool ok = false;
    Module module = lowerSource("Calc", R"(
class Calc {
    static func triple(int n): int {
        return n * 3
    }
    static func mix(double a, double b): double {
        return a * b + 1.5
    }
    static func biggest(int a, int b): int {
        if (a > b) {
            return a
        }
        return b
    }
}
)", ok);
    require(ok, "the ZL fixture lowers to verified MIR");
    if (!ok) return;

    auto result = compileMirToNative(module, x64SysVTarget());
    require(result.ok(), "the native backend ran over real lowered MIR: " + result.error);
    require(!result.nativeFunctions.empty(),
            "at least one real ZL function reached the native tier; got:\n" + result.describe());
    // Whatever it could not take must be named, never silently dropped.
    require(result.nativeFunctions.size() + result.vmFunctions.size() == module.functions.size(),
            "every function is accounted for as either native or VM");
}

void testUnsupportedProgramDegrades() {
    bool ok = false;
    Module module = lowerSource("Greeter", R"(
class Greeter {
    string name = "world"
    func greet(): string {
        return "hello " + this.name
    }
}
)", ok);
    require(ok, "the object fixture lowers to verified MIR");
    if (!ok) return;

    auto result = compileMirToNative(module, x64SysVTarget());
    require(result.ok(), "an unsupported program is a partial result, not an error");
    require(result.nativeFunctions.empty() || !result.vmFunctions.empty(),
            "string/object work is left to the VM");
    for (const auto& rejection : result.vmFunctions)
        require(!rejection.reason.empty(),
                "every refusal names its reason (" + rejection.function + ")");
}

} // namespace

int main() {
    testTargetDescription();
    testValueClasses();
    testSelectionSubset();
    testEmittedFrameMap();
#if ZL_NATIVE_CAN_EXECUTE
    // Defined in the guarded region above with its execution fixtures, so the
    // call has to be guarded too: unguarded, a platform without executable
    // memory (macOS) failed to build with "use of undeclared identifier".
    testReferencePassThrough();
    testNativeSafepointRootPublication();
#endif
    testUnverifiedMirIsRefused();
    testNoEncoderIsSelectOnly();
#if ZL_NATIVE_CAN_EXECUTE
    testExecution();
    testArithmeticEdges();
    testArithmeticTraps();
#else
    std::cerr << "note: executable memory unavailable on this platform; execution cases skipped\n";
#endif
    testEndToEnd();
    testUnsupportedProgramDegrades();

    if (failures != 0) {
        std::cerr << failures << " native backend regression(s)\n";
        return 1;
    }
    std::cout << "native backend tests passed\n";
    return 0;
}
