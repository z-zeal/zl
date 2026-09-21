// MIR data-model, CFG analysis, and verifier regressions.
//
// These tests build MIR directly rather than lowering it, because the point is
// to prove the verifier rejects each specific class of malformed MIR. A
// lowering-only test could never reach them: the builder refuses to produce
// most of these shapes.
#include "zl/mir/analysis.hpp"
#include "zl/mir/builder.hpp"
#include "zl/mir/printer.hpp"
#include "zl/mir/verifier.hpp"
#include "zl/mir/reachability.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "mir regression: " << message << '\n';
    ++failures;
}

// True when the report contains an error whose message includes `needle`.
bool hasError(const zl::mir::VerificationReport& report, const std::string& needle) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity != zl::mir::DiagnosticSeverity::Error) continue;
        if (diagnostic.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string reportText(const zl::mir::VerificationReport& report) {
    return report.describe();
}

// True when the report contains a warning whose message includes `needle`.
bool hasWarning(const zl::mir::VerificationReport& report, const std::string& needle) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity != zl::mir::DiagnosticSeverity::Warning) continue;
        if (diagnostic.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

using namespace zl::mir;

// Constants are built field-by-field rather than by aggregate initialisation,
// so adding a payload member to `Constant` does not silently shift the meaning
// of every positional literal in this file.
Constant intConstant(std::int64_t value) {
    Constant c;
    c.kind = ConstKind::Int;
    c.intValue = value;
    return c;
}

Constant boolConstant(bool value) {
    Constant c;
    c.kind = ConstKind::Bool;
    c.boolValue = value;
    return c;
}

Constant stringConstant(std::string value) {
    Constant c;
    c.kind = ConstKind::String;
    c.stringValue = std::move(value);
    return c;
}

Constant nilConstant() {
    Constant c;
    c.kind = ConstKind::Nil;
    return c;
}

// ---------------------------------------------------------------------------
// Constant interning
// ---------------------------------------------------------------------------

Constant doubleConstant(double value) {
    Constant c;
    c.kind = ConstKind::Double;
    c.doubleValue = value;
    return c;
}

void testConstantInterning() {
    Module module;
    // Dedup: equal values share one slot, distinct values do not.
    const ConstId one = module.internConstant(intConstant(1));
    require(module.internConstant(intConstant(1)) == one, "equal ints did not share a constant slot");
    const ConstId two = module.internConstant(intConstant(2));
    require(two != one, "distinct ints shared a constant slot");
    require(module.internConstant(stringConstant("1")) != one, "int 1 and string \"1\" shared a slot");
    require(module.internConstant(doubleConstant(1.0)) != one, "int 1 and double 1.0 shared a slot");
    require(module.internConstant(boolConstant(true)) != one, "int 1 and bool true shared a slot");
    require(module.internConstant(nilConstant()) != one, "int 1 and nil shared a slot");
    // IEEE == treats -0.0 and +0.0 as equal, but they are observably different
    // (sign of 1.0/x, printed form). Pool identity is bitwise, matching
    // Constant::operator==, so they must not share a slot.
    require(module.internConstant(doubleConstant(-0.0)) != module.internConstant(doubleConstant(0.0)),
            "-0.0 and +0.0 shared a constant slot");
    // Ids are dense 1-based positions into Module::constants.
    require(module.constants.size() == module.constantIndex.size(), "constant index drifted from the pool");
    for (const auto& entry : module.constantIndex) {
        require(entry.second >= 1 && entry.second <= module.constants.size(), "constant index held a stale id");
        require(module.constants[entry.second - 1] == entry.first, "constant index pointed at the wrong slot");
    }

    // Scale: interning used to scan the pool linearly per literal, so a
    // large literal (a 200k-element list interns one int per index) compiled
    // in O(n^2) and effectively hung. 100k distinct values plus 100k
    // re-interns must stay far below any quadratic budget.
    Module big;
    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < 100000; ++i) {
        const ConstId id = big.internConstant(intConstant(i));
        require(id == static_cast<ConstId>(i + 1), "distinct constants did not get dense ids");
    }
    for (std::int64_t i = 0; i < 100000; ++i) {
        require(big.internConstant(intConstant(i)) == static_cast<ConstId>(i + 1),
                "re-interned constant missed its slot");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed < std::chrono::seconds(25), "constant interning regressed to superlinear time");
}

// ---------------------------------------------------------------------------
// Type arena
// ---------------------------------------------------------------------------

void testTypeArena() {
    TypeArena arena;
    const TypeId a = arena.listType(arena.intType());
    const TypeId b = arena.listType(arena.intType());
    require(a == b, "interning did not unify identical list types");
    require(a != arena.listType(arena.doubleType()), "distinct element types shared a type id");

    // Union identity must not depend on the order members were written in.
    const TypeId unionAB = arena.unionType({arena.intType(), arena.stringType()});
    const TypeId unionBA = arena.unionType({arena.stringType(), arena.intType()});
    require(unionAB == unionBA, "union type identity depended on member order");

    // A one-member union is that member.
    require(arena.unionType({arena.intType()}) == arena.intType(), "single-member union was not collapsed");

    require(arena.render(arena.mapType(arena.stringType(), arena.intType())) == "map<string,int>",
            "map rendering was wrong: " + arena.render(arena.mapType(arena.stringType(), arena.intType())));
    require(arena.render(arena.objectType("Box", {arena.intType()})) == "Box<int>",
            "generic object rendering was wrong");
    require(arena.render(arena.arrayType(arena.intType(), 10)) == "array[10]<int>",
            "fixed array rendering was wrong");

    FunctionSignature signature;
    signature.parameterTypes = {arena.intType(), arena.stringType()};
    signature.returnType = arena.boolType();
    require(arena.render(arena.functionType(signature)) == "func(int,string):bool",
            "function type rendering was wrong: " + arena.render(arena.functionType(signature)));

    require(arena.find(0) == nullptr, "type id 0 must not resolve");
    require(arena.find(9999) == nullptr, "out-of-range type id resolved");
    std::cout << "mir type arena: PASS\n";
}

// ---------------------------------------------------------------------------
// A small well-formed module reused by several tests
// ---------------------------------------------------------------------------

// Builds:  func Calc.sum(int,int): int { return a + b }
//          func Calc.main(): void { log(sum(1,2)) }
Module buildValidModule() {
    ModuleBuilder builder("test");
    TypeArena& types = builder.types();

    {
        FunctionBuilder fb = builder.addFunction("Calc.sum(int,int)");
        fb.setReturnType(types.intType());
        const ParamId a = fb.addParameter("a", types.intType());
        const ParamId b = fb.addParameter("b", types.intType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const TempId sum = fb.emitBinary(Opcode::Add, fb.parameterOperand(a), fb.parameterOperand(b),
                                         types.intType());
        fb.emitReturn(Operand::temp(sum, types.intType()));
        fb.finish();
    }

    FunctionId mainId = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Calc.main()");
        fb.setReturnType(types.voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const FunctionId sum = builder.findFunction("Calc.sum(int,int)");
        const Operand one = Operand::constant(builder.constantInt(1), types.intType());
        const Operand two = Operand::constant(builder.constantInt(2), types.intType());
        const TempId result = fb.emitCall(sum, {one, two}, types.intType());
        fb.emitLog(Operand::temp(result, types.intType()));
        fb.emitReturn();
        fb.finish();
        mainId = fb.function().id;
    }
    builder.setEntryPoint(mainId);
    return builder.take();
}

void testValidModuleVerifies() {
    Module module = buildValidModule();
    const auto report = verifyModule(module);
    require(report.ok(), "a well-formed module was rejected:\n" + reportText(report));

    // The entry point and function identity must survive the round trip.
    require(module.entryPoint != kNoFunction, "entry point was lost");
    require(module.functions.size() == 2, "expected two lowered functions");

    // Edges were materialised by finish(); a single-block function has none.
    require(module.functions[0].edges.empty(), "a straight-line function reported control-flow edges");

    const std::string text = printModule(module);
    require(text.find("func Calc.sum(int,int)") != std::string::npos, "printer dropped the function header");
    require(text.find("add") != std::string::npos, "printer dropped the add instruction");
    std::cout << "mir valid module: PASS\n";
}

// ---------------------------------------------------------------------------
// Control-flow graph analysis
// ---------------------------------------------------------------------------

// Builds a diamond with a loop back-edge so dominance is non-trivial:
//   b1 -> b2/b3 ; b2 -> b4 ; b3 -> b4 ; b4 -> b2 (loop) and b4 -> b5
Module buildCfgModule() {
    ModuleBuilder builder("cfg");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Cfg.walk(int)");
    fb.setReturnType(types.voidType());
    const ParamId n = fb.addParameter("n", types.intType());

    const BlockId b1 = fb.addBlock();
    const BlockId b2 = fb.addBlock();
    const BlockId b3 = fb.addBlock();
    const BlockId b4 = fb.addBlock();
    const BlockId b5 = fb.addBlock();

    fb.setCurrentBlock(b1);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());
    const Operand limit = Operand::constant(builder.constantInt(10), types.intType());
    const TempId counter = fb.emitBinary(Opcode::Add, fb.parameterOperand(n), zero, types.intType());
    const SlotId slot = fb.addSlot("i", types.intType());
    fb.emitStore(slot, Operand::temp(counter, types.intType()));
    fb.emitBranch(Operand::constant(builder.constantBool(true), types.boolType()), b2, b3);

    fb.setCurrentBlock(b2);
    // A value defined only on this arm, so a use of it from b3 is a genuine
    // dominance violation rather than a legal one.
    const TempId armOnly = fb.emitBinary(Opcode::Add, fb.parameterOperand(n), zero, types.intType());
    const SlotId armSlot = fb.addSlot("arm", types.intType());
    fb.emitStore(armSlot, Operand::temp(armOnly, types.intType()));
    fb.emitJump(b4);

    fb.setCurrentBlock(b3);
    fb.emitJump(b4);

    fb.setCurrentBlock(b4);
    const Operand current = Operand::temp(fb.emitLoad(slot), types.intType());
    const TempId keepGoing = fb.emitBinary(Opcode::Lt, current, limit, types.boolType());
    fb.emitBranch(Operand::temp(keepGoing, types.boolType()), b2, b5);

    fb.setCurrentBlock(b5);
    fb.emitReturn();
    fb.finish();
    return builder.take();
}

void testControlFlowAnalysis() {
    Module module = buildCfgModule();
    const auto report = verifyModule(module);
    require(report.ok(), "the CFG fixture was rejected:\n" + reportText(report));

    const Function& function = module.functions.front();
    ControlFlowGraph cfg(function);
    require(cfg.blockCount() == 5, "expected five blocks");
    require(cfg.successors(1).size() == 2, "the entry block did not have two successors");
    require(cfg.predecessors(4).size() == 2, "the join block did not have two predecessors");
    require(cfg.isReachable(5), "the exit block was reported unreachable");

    // Reverse post-order starts at the entry and visits every reachable block.
    require(cfg.reversePostOrder().size() == 5, "reverse post-order did not cover the function");
    require(cfg.reversePostOrder().front() == 1, "reverse post-order did not start at the entry");

    // b1 dominates everything; b2 does not dominate b3 (they are siblings).
    require(cfg.dominates(1, 4), "the entry block did not dominate the join");
    require(cfg.dominates(1, 5), "the entry block did not dominate the exit");
    require(!cfg.dominates(2, 3), "a branch arm was reported to dominate its sibling");
    require(!cfg.dominates(4, 1), "a loop body was reported to dominate the entry");
    require(cfg.dominates(4, 4), "a block must dominate itself");

    // The materialised edge list must match the terminators.
    require(function.edges.size() == 6, "unexpected edge count");
    std::cout << "mir control-flow analysis: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: malformed control flow
// ---------------------------------------------------------------------------

void testRejectsMissingBlockReference() {
    Module module = buildValidModule();
    // Point the call's block at a block that does not exist by replacing the
    // terminator with a jump to a nonexistent id.
    module.functions[0].blocks[0].terminator = Terminator{};
    module.functions[0].blocks[0].terminator.kind = TerminatorKind::Jump;
    module.functions[0].blocks[0].terminator.target = 99;
    const auto report = verifyModule(module);
    require(!report.ok(), "a jump to a nonexistent block was accepted");
    require(hasError(report, "does not exist"), "the missing-block error was not reported:\n" + reportText(report));
    std::cout << "mir verifier missing block: PASS\n";
}

void testRejectsMissingTerminator() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].terminator = Terminator{};
    const auto report = verifyModule(module);
    require(!report.ok(), "a block with no terminator was accepted");
    require(hasError(report, "no terminator"), "the missing-terminator error was not reported:\n" +
                                                    reportText(report));
    std::cout << "mir verifier missing terminator: PASS\n";
}

void testRejectsUnreachableBlock() {
    Module module = buildCfgModule();
    // Detach b3 so nothing reaches it.
    module.functions[0].blocks[0].terminator.elseBlock = 2;
    module.functions[0].rebuildEdges();
    const auto report = verifyModule(module);
    require(!report.ok(), "an unreachable block was accepted");
    require(hasError(report, "unreachable"), "the unreachable-block error was not reported:\n" +
                                                 reportText(report));

    // The same module is only a warning when the option is relaxed.
    VerifierOptions options;
    options.unreachableBlocksAreErrors = false;
    const auto relaxed = verifyModule(module, options);
    require(relaxed.ok(), "an unreachable block was still an error with the option relaxed:\n" +
                              reportText(relaxed));
    require(relaxed.warningCount() > 0, "the relaxed run recorded no warning");
    std::cout << "mir verifier unreachable block: PASS\n";
}

void testRejectsInconsistentPredecessors() {
    Module module = buildCfgModule();
    // Drop an edge without changing any terminator.
    module.functions[0].edges.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a stale control-flow edge list was accepted");
    require(hasError(report, "edge list does not match"),
            "the edge-consistency error was not reported:\n" + reportText(report));
    std::cout << "mir verifier edge consistency: PASS\n";
}

void testRejectsDominanceViolation() {
    Module module = buildCfgModule();
    Function& function = module.functions[0];
    // b2 and b3 are the two arms of the entry branch: neither dominates the
    // other. A temp defined in b2 and used in b3 is therefore invalid.
    TempId armOnly = kNoTemp;
    TypeId armType = 0;
    for (const auto& instruction : function.blocks[1].instructions) {
        if (instruction.opcode == Opcode::Add) { armOnly = instruction.result; armType = instruction.resultType; }
    }
    require(armOnly != kNoTemp, "the fixture did not define an arm-local value");

    Instruction bad;
    bad.opcode = Opcode::Log;
    bad.operands = {Operand::temp(armOnly, armType)};
    function.blocks[2].instructions.insert(function.blocks[2].instructions.begin(), bad);
    function.rebuildEdges();

    const auto report = verifyModule(module);
    require(!report.ok(), "a use outside the definition's dominator tree was accepted");
    require(hasError(report, "does not dominate"),
            "the dominance error was not reported:\n" + reportText(report));

    // Sanity check on the same fixture: a value defined in b1 IS available in
    // b3, because the entry dominates every reachable block.
    Module okModule = buildCfgModule();
    TempId entryValue = kNoTemp;
    TypeId entryType = 0;
    for (const auto& instruction : okModule.functions[0].blocks[0].instructions) {
        if (instruction.opcode == Opcode::Add) { entryValue = instruction.result; entryType = instruction.resultType; }
    }
    Instruction legal;
    legal.opcode = Opcode::Log;
    legal.operands = {Operand::temp(entryValue, entryType)};
    okModule.functions[0].blocks[2].instructions.insert(okModule.functions[0].blocks[2].instructions.begin(), legal);
    okModule.functions[0].rebuildEdges();
    const auto okReport = verifyModule(okModule);
    require(okReport.ok(), "a dominating use was rejected:\n" + reportText(okReport));
    std::cout << "mir verifier dominance: PASS\n";
}

void testRejectsForwardUse() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    // Move the add after the return by appending a second use before it.
    Instruction& add = function.blocks[0].instructions[0];
    const TempId result = add.result;
    const TypeId type = add.resultType;
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), add);
    // Now the first copy defines it and the original (second) redefines it.
    const auto report = verifyModule(module);
    require(!report.ok(), "a duplicated temp definition was accepted");
    require(hasError(report, "defined more than once"),
            "the duplicate-definition error was not reported:\n" + reportText(report));
    (void)result;
    (void)type;
    std::cout << "mir verifier ssa uniqueness: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: operands and types
// ---------------------------------------------------------------------------

void testRejectsUndefinedTemp() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands[0] = Operand::temp(77, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a reference to an undefined temp was accepted");
    require(hasError(report, "never defined"), "the undefined-temp error was not reported:\n" +
                                                   reportText(report));
    std::cout << "mir verifier undefined temp: PASS\n";
}

void testRejectsWrongOperandType() {
    Module module = buildValidModule();
    // `int - string` is not a ZL operation. (`int + string` is: concatenation.)
    const TypeId stringType = module.types.stringType();
    const ConstId text = module.internConstant(stringConstant("x"));
    module.functions[0].blocks[0].instructions[0].opcode = Opcode::Sub;
    module.functions[0].blocks[0].instructions[0].operands[1] = Operand::constant(text, stringType);
    const auto report = verifyModule(module);
    require(!report.ok(), "int - string was accepted");
    require(hasError(report, "sub on int and string"),
            "the operator typing error was not reported:\n" + reportText(report));
    std::cout << "mir verifier operand types: PASS\n";
}

void testRejectsWrongResultType() {
    Module module = buildValidModule();
    // Claim the add produces a bool.
    module.functions[0].blocks[0].instructions[0].resultType = module.types.boolType();
    const auto report = verifyModule(module);
    require(!report.ok(), "an add with a bool result was accepted");
    require(hasError(report, "must produce int"), "the result-type error was not reported:\n" +
                                                      reportText(report));
    std::cout << "mir verifier result type: PASS\n";
}

void testRejectsBadConstant() {
    Module module = buildValidModule();
    // An int operand pointing at a string constant.
    const ConstId text = module.internConstant(stringConstant("x"));
    module.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::constant(text, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a string constant typed as int was accepted");
    require(hasError(report, "constant"), "the constant-kind error was not reported:\n" + reportText(report));
    std::cout << "mir verifier constant kind: PASS\n";
}

void testRejectsBadParameterReference() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::param(9, module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "an out-of-range parameter reference was accepted");
    require(hasError(report, "unknown parameter"),
            "the parameter error was not reported:\n" + reportText(report));
    std::cout << "mir verifier parameter reference: PASS\n";
}

void testRejectsWrongOperandCount() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].instructions[0].operands.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a binary operator with one operand was accepted");
    require(hasError(report, "requires 2 operand"),
            "the operand-count error was not reported:\n" + reportText(report));
    std::cout << "mir verifier operand count: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: terminators, returns, signatures
// ---------------------------------------------------------------------------

void testRejectsInvalidReturn() {
    Module module = buildValidModule();
    // A void function that returns a value.
    Function& main = module.functions[1];
    main.blocks[0].terminator.value = Operand::constant(module.internConstant(
        intConstant(7)), module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a value returned from a void function was accepted");
    require(hasError(report, "returns a value from a void function"),
            "the void-return error was not reported:\n" + reportText(report));
    std::cout << "mir verifier void return: PASS\n";
}

void testRejectsMissingReturnValue() {
    Module module = buildValidModule();
    module.functions[0].blocks[0].terminator.value = Operand::none();
    const auto report = verifyModule(module);
    require(!report.ok(), "a bare return from an int function was accepted");
    require(hasError(report, "returns no value"), "the missing-return-value error was not reported:\n" +
                                                      reportText(report));
    std::cout << "mir verifier missing return value: PASS\n";
}

void testRejectsReturnTypeMismatch() {
    Module module = buildValidModule();
    // Return a bool where the signature says int.
    module.functions[0].blocks[0].terminator.value = Operand::constant(
        module.internConstant(boolConstant(true)), module.types.boolType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a bool returned from an int function was accepted");
    require(hasError(report, "but the function returns int"),
            "the return-type error was not reported:\n" + reportText(report));
    std::cout << "mir verifier return type mismatch: PASS\n";
}

void testRejectsMalformedSignature() {
    Module module = buildValidModule();
    // A void parameter is not a thing.
    module.functions[0].parameters[0].type = module.types.voidType();
    const auto report = verifyModule(module);
    require(!report.ok(), "a void parameter was accepted");
    require(hasError(report, "cannot have type void"),
            "the malformed-signature error was not reported:\n" + reportText(report));
    std::cout << "mir verifier malformed signature: PASS\n";
}

void testRejectsNonBoolBranch() {
    Module module = buildCfgModule();
    module.functions[0].blocks[0].terminator.value =
        Operand::constant(module.internConstant(intConstant(1)),
                          module.types.intType());
    const auto report = verifyModule(module);
    require(!report.ok(), "a branch on an int was accepted");
    require(hasError(report, "must be bool"), "the branch-condition error was not reported:\n" +
                                                  reportText(report));
    std::cout << "mir verifier branch condition: PASS\n";
}

void testRejectsEntryWithPredecessor() {
    Module module = buildCfgModule();
    // Make the exit block jump back to the entry.
    module.functions[0].blocks[4].terminator = Terminator{};
    module.functions[0].blocks[4].terminator.kind = TerminatorKind::Jump;
    module.functions[0].blocks[4].terminator.target = 1;
    module.functions[0].rebuildEdges();
    const auto report = verifyModule(module);
    require(!report.ok(), "an entry block with a predecessor was accepted");
    require(hasError(report, "entry block"), "the entry-predecessor error was not reported:\n" +
                                                 reportText(report));
    std::cout << "mir verifier entry predecessor: PASS\n";
}

void testRejectsCallArityMismatch() {
    Module module = buildValidModule();
    // Call the two-parameter sum with one argument.
    Instruction& call = module.functions[1].blocks[0].instructions[0];
    call.operands.pop_back();
    call.target.argumentTypes.pop_back();
    const auto report = verifyModule(module);
    require(!report.ok(), "a call with the wrong arity was accepted");
    require(hasError(report, "but it takes 2"), "the call-arity error was not reported:\n" +
                                                    reportText(report));
    std::cout << "mir verifier call arity: PASS\n";
}

void testRejectsCallArgumentType() {
    Module module = buildValidModule();
    Instruction& call = module.functions[1].blocks[0].instructions[0];
    const ConstId text = module.internConstant(stringConstant("x"));
    call.operands[1] = Operand::constant(text, module.types.stringType());
    call.target.argumentTypes[1] = module.types.stringType();
    const auto report = verifyModule(module);
    require(!report.ok(), "a string passed to an int parameter was accepted");
    require(hasError(report, "but the parameter is int"),
            "the call-argument-type error was not reported:\n" + reportText(report));
    std::cout << "mir verifier call argument type: PASS\n";
}

// ---------------------------------------------------------------------------
// Verifier: impossible instruction combinations and ownership
// ---------------------------------------------------------------------------

void testRejectsAwaitOutsideAsync() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    Instruction await;
    await.opcode = Opcode::Await;
    await.result = 50;
    await.resultType = module.types.intType();
    await.operands = {Operand::constant(module.internConstant(nilConstant()),
                                        module.types.taskType(module.types.intType()))};
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), await);
    const auto report = verifyModule(module);
    require(!report.ok(), "an await in a non-async function was accepted");
    require(hasError(report, "await in a non-async function"),
            "the await error was not reported:\n" + reportText(report));
    std::cout << "mir verifier await outside async: PASS\n";
}

void testRejectsStoreToImmutableSlot() {
    ModuleBuilder builder("immutable");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Immut.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId slot = fb.addSlot("constant", types.intType(), /*isMutable=*/false);
    fb.emitStore(slot, Operand::constant(builder.constantInt(1), types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a store into an immutable slot was accepted");
    require(hasError(report, "immutable"), "the immutability error was not reported:\n" + reportText(report));
    std::cout << "mir verifier immutable slot: PASS\n";
}

void testRejectsMoveOfNonOwnedSlot() {
    ModuleBuilder builder("move");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Move.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // A GC slot cannot be moved; only an owned one can.
    const SlotId slot = fb.addSlot("value", types.listType(types.intType()), true, zl::OwnershipKind::GC);
    (void)fb.emitMove(slot);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a move from a GC slot was accepted");
    require(hasError(report, "only an owned slot can be moved"),
            "the move-ownership error was not reported:\n" + reportText(report));
    std::cout << "mir verifier move of non-owned: PASS\n";
}

void testRejectsUseAfterMove() {
    ModuleBuilder builder("aftermove");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("AfterMove.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId slot = fb.addSlot("buffer", types.listType(types.intType()), true, zl::OwnershipKind::OWNED);
    fb.emitStore(slot, Operand::temp(fb.emitNewCollection(types.listType(types.intType())),
                                     types.listType(types.intType())));
    const TempId moved = fb.emitMove(slot);
    fb.emitDrop(Operand::temp(moved, types.listType(types.intType())));
    // Reading the slot after the move is the error.
    (void)fb.emitLoad(slot);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a read after a move was accepted");
    require(hasError(report, "after it was moved"),
            "the use-after-move error was not reported:\n" + reportText(report));
    std::cout << "mir verifier use after move: PASS\n";
}

void testRejectsMoveWhileBorrowed() {
    ModuleBuilder builder("borrowed");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("Borrowed.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId owner = fb.addSlot("owner", listType, true, zl::OwnershipKind::OWNED);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitStore(owner, Operand::temp(fb.emitNewCollection(listType), listType));
    const TempId ownerValue = fb.emitLoad(owner);
    fb.emitBorrow(borrow, Operand::temp(ownerValue, listType));
    // Moving the owner while the borrow is live is the error.
    (void)fb.emitMove(owner);
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a move of a borrowed local was accepted");
    require(hasError(report, "while it is borrowed"),
            "the borrow-conflict error was not reported:\n" + reportText(report));
    std::cout << "mir verifier move while borrowed: PASS\n";
}

void testRejectsDropOfBorrow() {
    ModuleBuilder builder("dropborrow");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("DropBorrow.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId owner = fb.addSlot("owner", listType, true, zl::OwnershipKind::OWNED);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitStore(owner, Operand::temp(fb.emitNewCollection(listType), listType));
    const TempId ownerValue = fb.emitLoad(owner);
    fb.emitBorrow(borrow, Operand::temp(ownerValue, listType));
    const TempId borrowed = fb.emitLoad(borrow);
    // Dropping a borrowed value instead of ending the borrow is the error.
    fb.emitDrop(Operand::temp(borrowed, listType));
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "dropping a borrowed value was accepted");
    require(hasError(report, "end the borrow instead"),
            "the drop-of-borrow error was not reported:\n" + reportText(report));
    std::cout << "mir verifier drop of borrow: PASS\n";
}

void testRejectsEndBorrowWithoutBorrow() {
    ModuleBuilder builder("endborrow");
    TypeArena& types = builder.types();
    const TypeId listType = types.listType(types.intType());
    FunctionBuilder fb = builder.addFunction("EndBorrow.run()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const SlotId borrow = fb.addSlot("view", listType, true, zl::OwnershipKind::BORROW);
    fb.emitEndBorrow(borrow);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "an end_borrow with no active borrow was accepted");
    require(hasError(report, "does not hold an active borrow"),
            "the end_borrow error was not reported:\n" + reportText(report));
    std::cout << "mir verifier end borrow: PASS\n";
}

void testRejectsNullCheckOnNonNullable() {
    Module module = buildValidModule();
    Function& function = module.functions[0];
    Instruction check;
    check.opcode = Opcode::NullCheck;
    check.result = 60;
    check.resultType = module.types.intType();
    check.operands = {Operand::constant(module.internConstant(intConstant(1)),
                                        module.types.intType())};
    function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), check);
    const auto report = verifyModule(module);
    require(!report.ok(), "a null check on an int was accepted");
    require(hasError(report, "cannot be nil"), "the null-check error was not reported:\n" + reportText(report));
    std::cout << "mir verifier null check: PASS\n";
}

void testRejectsUndeclaredTypeParamUse() {
    ModuleBuilder builder("typeparam");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.id(T)");
    // Not marked as a generic template, so T is out of scope.
    const TypeId param = types.typeParam("T");
    fb.setReturnType(param);
    const ParamId value = fb.addParameter("value", param);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn(fb.parameterOperand(value));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(!report.ok(), "a generic parameter outside a template was accepted");
    require(hasError(report, "not a generic template"),
            "the type-parameter scope error was not reported:\n" + reportText(report));
    std::cout << "mir verifier type parameter scope: PASS\n";
}

void testAcceptsDeclaredTypeParam() {
    ModuleBuilder builder("typeparam");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.id(T)");
    fb.setGenericTemplate({"T"});
    const TypeId param = types.typeParam("T");
    fb.setReturnType(param);
    const ParamId value = fb.addParameter("value", param);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    fb.emitReturn(fb.parameterOperand(value));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "a declared generic parameter was rejected:\n" + reportText(report));
    std::cout << "mir verifier type parameter declared: PASS\n";
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void testExceptionEdges() {
    ModuleBuilder builder("exceptions");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Throws.run()");
    fb.setReturnType(types.voidType());

    const BlockId entry = fb.addBlock();
    const BlockId catchBlock = fb.addBlock(BlockKind::Catch);
    const BlockId after = fb.addBlock();
    const SlotId binding = fb.addSlot("e", types.stringType(), false);
    fb.function().slots[0].isCatchBinding = true;

    fb.setCurrentBlock(entry);
    fb.pushExceptionHandler(0, catchBlock, binding);
    fb.emitJump(after);

    fb.setCurrentBlock(catchBlock);
    fb.emitLog(Operand::temp(fb.emitLoad(binding), types.stringType()));
    fb.emitJump(after);

    fb.setCurrentBlock(after);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "a valid try/catch shape was rejected:\n" + reportText(report));

    const Function& function = module.functions.front();
    ControlFlowGraph cfg(function);
    require(cfg.predecessors(catchBlock).empty(), "a catch block had a normal predecessor");
    require(cfg.unwindPredecessors(catchBlock).size() == 1, "the catch block had no unwind predecessor");
    require(cfg.reachableWithUnwind().count(catchBlock) != 0, "the catch block was not unwind-reachable");

    // A catch block that is also reachable by falling through is invalid.
    Module broken = module;
    broken.functions[0].blocks[0].terminator.kind = TerminatorKind::Jump;
    broken.functions[0].blocks[0].terminator.target = catchBlock;
    broken.functions[0].rebuildEdges();
    const auto brokenReport = verifyModule(broken);
    require(!brokenReport.ok(), "a catch block reachable by normal flow was accepted");
    require(hasError(brokenReport, "normal control flow"),
            "the catch-reachability error was not reported:\n" + reportText(brokenReport));
    std::cout << "mir verifier exception edges: PASS\n";
}

// ---------------------------------------------------------------------------
// Regressions from lowering real ZL programs
//
// Each of these is a shape the example corpus produced and an earlier verifier
// or type layer got wrong. They are kept as MIR-level tests so the fix stays
// pinned even when the lowerer changes.
// ---------------------------------------------------------------------------

void testInheritedFieldAccess() {
    ModuleBuilder builder("inherit");
    TypeArena& types = builder.types();

    ClassLayout& animal = builder.addClassLayout("Animal");
    FieldLayout name;
    name.name = "name";
    name.type = types.stringType();
    animal.fields.push_back(name);

    // `Dog extends Animal` declares no fields of its own.
    ClassLayout& dog = builder.addClassLayout("Dog");
    dog.parent = "Animal";

    FunctionBuilder fb = builder.addFunction("Dog.fetch()");
    fb.setReturnType(types.stringType());
    const ParamId self = fb.addParameter("this", types.objectType("Dog"));
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `this.name` - declared on the parent, not on Dog.
    const TempId loaded = fb.emitFieldLoad(fb.parameterOperand(self), "name", types.stringType());
    fb.emitReturn(Operand::temp(loaded, types.stringType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "reading an inherited field was rejected:\n" + reportText(report));

    // A field nothing in the chain declares is still an error.
    module.functions[0].blocks[0].instructions[0].name = "collar";
    const auto missing = verifyModule(module);
    require(!missing.ok(), "a field no class declares was accepted");
    require(hasError(missing, "do not declare"),
            "the inherited-field error was not reported:\n" + reportText(missing));
    std::cout << "mir verifier inherited fields: PASS\n";
}

void testListObjectIndexing() {
    ModuleBuilder builder("index");
    TypeArena& types = builder.types();

    // `List<T>` is a real class, so an indexable base arrives as an Object
    // named "List" rather than the `list` keyword kind.
    const TypeId listOfInt = types.objectType("List", {types.intType()});

    FunctionBuilder fb = builder.addFunction("Indexer.first(List<int>)");
    fb.setReturnType(types.intType());
    const ParamId xs = fb.addParameter("xs", listOfInt);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const Operand zero = Operand::constant(builder.constantInt(0), types.intType());
    const TempId element = fb.emitIndexLoad(fb.parameterOperand(xs), zero, types.intType());
    fb.emitReturn(Operand::temp(element, types.intType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "indexing a List<int> was rejected:\n" + reportText(report));

    // Only a List is indexable in ZL - `Box<int>[0]` is not a thing. New types
    // come from the module's arena: `builder.take()` moved the builder's one out.
    const TypeId boxOfInt = module.types.objectType("Box", {module.types.intType()});
    module.functions[0].parameters[0].type = boxOfInt;
    module.functions[0].blocks[0].instructions[0].operands[0] = Operand::param(0, boxOfInt);
    const auto notAList = verifyModule(module);
    require(!notAList.ok(), "indexing a non-List object was accepted");
    require(hasError(notAList, "requires a List"),
            "the indexability error was not reported:\n" + reportText(notAList));
    std::cout << "mir verifier list indexing: PASS\n";
}

void testEnumMemberConstant() {
    ModuleBuilder builder("enums");
    TypeArena& types = builder.types();
    const TypeId color = types.objectType("Color");

    ClassLayout& enumeration = builder.addClassLayout("Color");
    enumeration.isEnum = true;
    enumeration.enumMembers = {"RED", "GREEN"};

    FunctionBuilder fb = builder.addFunction("Enums.isRed(Color)");
    fb.setReturnType(types.boolType());
    const ParamId c = fb.addParameter("c", color);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `Color.RED`: a string at runtime, but statically a Color.
    const ConstId redConst = builder.constantEnumMember("Color", "RED");
    const Operand red = Operand::constant(redConst, color);
    const TempId same = fb.emitBinary(Opcode::Eq, fb.parameterOperand(c), red, types.boolType());
    fb.emitReturn(Operand::temp(same, types.boolType()));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "comparing an enum member to its own enum was rejected:\n" + reportText(report));

    // Typing the same constant as string is what used to happen, and it must not
    // come back: the comparison would then be Color against string.
    module.functions[0].blocks[0].instructions[0].operands[1] = Operand::constant(redConst, types.stringType());
    const auto asString = verifyModule(module);
    require(!asString.ok(), "an enum member typed as string was accepted");
    std::cout << "mir verifier enum members: PASS\n";
}

void testGenericCallInstantiation() {
    ModuleBuilder builder("generic");
    TypeArena& types = builder.types();
    const TypeId payload = types.typeParam("T");
    const TypeId sharedT = types.sharedType(payload);

    // A generic template: `Shared<T>.set(T)`. MIR never duplicates it per
    // instantiation, so the call site records which T it selected.
    FunctionId setter = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Shared.set(T)");
        fb.function().typeParameters = {"T"};
        fb.setReturnType(types.voidType());
        const ParamId self = fb.addParameter("this", sharedT);
        (void)fb.addParameter("value", payload);
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        fb.emitFieldStore(fb.parameterOperand(self), "__value", Operand::param(1, payload));
        fb.emitReturn();
        fb.finish();
        setter = fb.function().id;
    }

    const TypeId sharedInt = types.sharedType(types.intType());
    FunctionBuilder fb = builder.addFunction("Use.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId instance = fb.emitAlloc("Shared", {types.intType()}, sharedInt);
    const Operand value = Operand::constant(builder.constantInt(1), types.intType());
    const TempId callTemp = fb.emitCall(setter, {Operand::temp(instance, sharedInt), value}, 0, {},
                                        {types.intType()});
    (void)callTemp;
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(),
            "a call into a generic template with its instantiation recorded was rejected:\n" +
                reportText(report));

    // What the recorded instantiation buys is precision. With T bound to int, a
    // string argument is a real mismatch and must be caught.
    Module wrong = module;
    const ConstId text = wrong.internConstant(stringConstant("x"));
    wrong.functions[1].blocks[0].instructions[1].operands[1] =
        Operand::constant(text, wrong.types.stringType());
    wrong.functions[1].blocks[0].instructions[1].target.argumentTypes = {sharedInt,
                                                                         wrong.types.stringType()};
    const auto mismatch = verifyModule(wrong);
    require(!mismatch.ok(), "a string passed for T:=int was accepted");
    require(hasError(mismatch, "but the parameter is int"),
            "the substituted-parameter error was not reported:\n" + reportText(mismatch));

    // Without the instantiation the parameter stays Shared<T>/T, and an
    // unsubstituted generic parameter is deliberately compatible with anything -
    // the verifier cannot know what T will be. So the same call is accepted, and
    // only less precisely checked.
    Module uninstantiated = module;
    const ConstId uninstantiatedText = uninstantiated.internConstant(stringConstant("x"));
    uninstantiated.functions[1].blocks[0].instructions[1].typeArguments.clear();
    uninstantiated.functions[1].blocks[0].instructions[1].operands[1] =
        Operand::constant(uninstantiatedText, uninstantiated.types.stringType());
    uninstantiated.functions[1].blocks[0].instructions[1].target.argumentTypes = {
        sharedInt, uninstantiated.types.stringType()};
    const auto permissive = verifyModule(uninstantiated);
    require(permissive.ok(),
            "an uninstantiated generic call was rejected, but T is unknowable there:\n" +
                reportText(permissive));
    std::cout << "mir verifier generic instantiation: PASS\n";
}

void testSharedIsReferenceType() {
    ModuleBuilder builder("shared");
    TypeArena& types = builder.types();
    const TypeId sharedInt = types.sharedType(types.intType());

    ClassLayout& layout = builder.addClassLayout("Shared");
    layout.typeParameters = {"T"};
    FieldLayout value;
    value.name = "__value";
    value.type = types.typeParam("T");
    layout.fields.push_back(value);

    FunctionBuilder fb = builder.addFunction("Shared.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // Construction of a Shared<T> allocates a reference, not a plain object.
    const TempId instance = fb.emitAlloc("Shared", {types.intType()}, sharedInt);
    const Operand receiver = Operand::temp(instance, sharedInt);
    fb.emitFieldStore(receiver, "__value", Operand::constant(builder.constantInt(0), types.intType()));
    // Shared<T> has methods of its own, so it is a legal dispatch receiver.
    (void)fb.emitInvokeMethod(receiver, "Shared", "withLock", {}, 0);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "Shared<T> as alloc result, field base, and receiver was rejected:\n" +
                             reportText(report));
    std::cout << "mir verifier shared reference type: PASS\n";
}

void testAwaitOfVoidTaskDefinesNoTemp() {
    ModuleBuilder builder("awaitvoid");
    TypeArena& types = builder.types();

    FunctionBuilder fb = builder.addFunction("Tasks.run()");
    fb.setAsync(true);
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    // `TaskCreate` from nil gives a Task<void>; awaiting it yields nothing, so
    // there is no temp - even though `await` is a value-producing opcode.
    const TypeId voidTask = types.taskType(types.voidType());
    const TempId task = fb.emitTaskCreate(Operand::constant(builder.constantNil(), types.nilType()), voidTask);
    (void)fb.emitAwait(Operand::temp(task, voidTask), 0);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    require(opcodeShape(Opcode::Await).optionalResult,
            "await is not marked as having an optional result");
    const auto report = verifyModule(module);
    require(report.ok(), "awaiting a Task<void> was rejected:\n" + reportText(report));

    // Awaiting a Task<int> still has to define a temp.
    const TypeId intTask = types.taskType(types.intType());
    module.functions[0].blocks[0].instructions[0].resultType = intTask;
    module.functions[0].blocks[0].instructions[1].operands[0] = Operand::temp(1, intTask);
    const auto noTemp = verifyModule(module);
    require(!noTemp.ok(), "awaiting a Task<int> with no temp was accepted");
    std::cout << "mir verifier await result: PASS\n";
}

void testMapIndexAccess() {
    ModuleBuilder builder("mapindex");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Maps.put()");
    const TypeId mapType = types.mapType(types.stringType(), types.intType());
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId map = fb.emitNewCollection(mapType);
    const Operand collection = Operand::temp(map, mapType);
    fb.emitIndexStore(collection,
                      Operand::constant(builder.constantString("a"), types.stringType()),
                      Operand::constant(builder.constantInt(1), types.intType()));
    const TempId read = fb.emitIndexLoad(collection,
                                         Operand::constant(builder.constantString("a"), types.stringType()),
                                         types.intType());
    (void)read;
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "indexing a map by its key was rejected:\n" + reportText(report));

    // A map is keyed, not positioned, so an int where the key type is string is
    // a genuine mismatch - the same rule a list applies to its index.
    Module wrongKey = module;
    wrongKey.functions[0].blocks[0].instructions[1].operands[1] =
        Operand::constant(wrongKey.internConstant(intConstant(0)), wrongKey.types.intType());
    const auto badKey = verifyModule(wrongKey);
    require(!badKey.ok(), "indexing a map<string,int> with an int key was accepted");
    require(hasError(badKey, "the key must be string"),
            "the key-type error was not reported:\n" + reportText(badKey));

    // And the value read back has to be the map's value type.
    Module wrongValue = module;
    wrongValue.functions[0].blocks[0].instructions[2].resultType = wrongValue.types.stringType();
    const auto badValue = verifyModule(wrongValue);
    require(!badValue.ok(), "reading a string out of a map<string,int> was accepted");
    std::cout << "mir verifier map indexing: PASS\n";
}

void testSetIndexing() {
    // A set literal is built by storing each element in turn, so `set<T>` has to
    // accept positional index access even though the language has no positional
    // read for one. This check once covered List/Array/Map and fell through to
    // the "requires a List<T>" error for a set, so `set<int> s = [1, 2, 3]` -
    // which the VM runs fine - produced MIR that failed to verify.
    ModuleBuilder builder("setindex");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Sets.build()");
    const TypeId setOfInt = types.setType(types.intType());
    fb.setReturnType(setOfInt);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId set = fb.emitNewCollection(setOfInt);
    const Operand collection = Operand::temp(set, setOfInt);
    fb.emitIndexStore(collection,
                      Operand::constant(builder.constantInt(0), types.intType()),
                      Operand::constant(builder.constantInt(7), types.intType()));
    fb.emitReturn(collection);
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "building a set<int> by position was rejected:\n" + reportText(report));

    // Positional, not keyed: a string where an index belongs is a mismatch, the
    // same rule a list applies.
    Module wrongIndex = module;
    wrongIndex.functions[0].blocks[0].instructions[1].operands[1] = Operand::constant(
        wrongIndex.internConstant(stringConstant("a")), wrongIndex.types.stringType());
    const auto badIndex = verifyModule(wrongIndex);
    require(!badIndex.ok(), "storing into a set<int> with a string index was accepted");
    require(hasError(badIndex, "the index must be int"),
            "the index-type error was not reported:\n" + reportText(badIndex));

    // And the element written has to be the set's element type.
    Module wrongValue = module;
    wrongValue.functions[0].blocks[0].instructions[1].operands[2] = Operand::constant(
        wrongValue.internConstant(stringConstant("a")), wrongValue.types.stringType());
    const auto badValue = verifyModule(wrongValue);
    require(!badValue.ok(), "storing a string into a set<int> was accepted");
    std::cout << "mir verifier set indexing: PASS\n";
}

// `List<T>` and `Map<K,V>` spelled as classes are the same collections as the
// lowercase keyword forms - and inside a generic class the class spelling is the
// only one available. Construction has to accept both, or a spelling can be
// indexable but not constructible.
void testCollectionClassFormsAreConstructible() {
    ModuleBuilder builder("collections");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Generic.singleton(T)");
    const TypeId typeParam = types.typeParam("T");
    const TypeId listOfT = types.objectType("List", {typeParam});
    fb.setGenericTemplate({"T"});
    fb.setReturnType(listOfT);
    const ParamId item = fb.addParameter("item", typeParam);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId list = fb.emitNewCollection(listOfT);
    fb.emitIndexStore(Operand::temp(list, listOfT),
                      Operand::constant(builder.constantInt(0), types.intType()),
                      fb.parameterOperand(item));
    fb.emitReturn(Operand::temp(list, listOfT));
    fb.finish();
    Module module = builder.take();

    const auto report = verifyModule(module);
    require(report.ok(), "building a List<T> was rejected:\n" + reportText(report));

    // A non-collection class is still not constructible as one.
    Module notACollection = module;
    const TypeId boxOfT = notACollection.types.objectType("Box", {notACollection.types.typeParam("T")});
    notACollection.functions[0].blocks[0].instructions[0].resultType = boxOfT;
    const auto bad = verifyModule(notACollection);
    require(!bad.ok(), "new_collection of a Box<T> was accepted");
    require(hasError(bad, "must produce list/set/map"),
            "the construction error was not reported:\n" + reportText(bad));
    std::cout << "mir verifier collection class forms: PASS\n";
}

void testTypeTest() {
    ModuleBuilder builder("typetest");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Narrow.check(int|string)");
    const TypeId unionType = types.unionType({types.intType(), types.stringType()});
    fb.setReturnType(types.boolType());
    const ParamId value = fb.addParameter("value", unionType);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TempId isInt = fb.emitTypeTest(fb.parameterOperand(value), types.intType());
    fb.emitReturn(Operand::temp(isInt, types.boolType()));
    fb.finish();
    Module module = builder.take();

    require(opcodeShape(Opcode::TypeTest).producesResult, "type_test is not marked as producing a result");
    require(!opcodeShape(Opcode::TypeTest).mayThrow,
            "type_test is marked as raising; a match arm has to survive a 'no'");
    const auto report = verifyModule(module);
    require(report.ok(), "a runtime type test was rejected:\n" + reportText(report));

    // A test that answers anything but bool cannot drive a branch.
    Module notBool = module;
    notBool.functions[0].blocks[0].instructions[0].resultType = notBool.types.intType();
    const auto wrongResult = verifyModule(notBool);
    require(!wrongResult.ok(), "a type_test producing int was accepted");
    require(hasError(wrongResult, "must produce bool"),
            "the result-type error was not reported:\n" + reportText(wrongResult));

    // The tested type lives in its own field, so clearing it must be caught
    // rather than read as "test against nothing".
    Module untested = module;
    untested.functions[0].blocks[0].instructions[0].testedType = kNoType;
    const auto noType = verifyModule(untested);
    require(!noType.ok(), "a type_test with no tested type was accepted");
    require(hasError(noType, "no tested type"),
            "the missing-type error was not reported:\n" + reportText(noType));

    // Nothing can be tested against void.
    Module againstVoid = module;
    againstVoid.functions[0].blocks[0].instructions[0].testedType = againstVoid.types.voidType();
    const auto voidTest = verifyModule(againstVoid);
    require(!voidTest.ok(), "a type_test against void was accepted");
    require(hasError(voidTest, "cannot test against void"),
            "the void error was not reported:\n" + reportText(voidTest));

    // Testing against an unresolved type parameter proves nothing at all.
    Module againstParam = module;
    againstParam.functions[0].blocks[0].instructions[0].testedType =
        againstParam.types.typeParam("T");
    const auto paramTest = verifyModule(againstParam);
    require(!paramTest.ok(), "a type_test against a type parameter was accepted");
    require(hasError(paramTest, "unresolved type parameter"),
            "the type-parameter error was not reported:\n" + reportText(paramTest));

    // A test the static types already rule out is not malformed - it is dead -
    // so it warns rather than failing.
    Module unrelated = module;
    unrelated.functions[0].blocks[0].instructions[0].testedType = unrelated.types.boolType();
    const auto unrelatedReport = verifyModule(unrelated);
    require(unrelatedReport.ok(), "a type_test against an unrelated type was rejected:\n" +
                                      reportText(unrelatedReport));
    bool warned = false;
    for (const auto& diagnostic : unrelatedReport.diagnostics)
        if (diagnostic.severity == DiagnosticSeverity::Warning &&
            diagnostic.message.find("can never match") != std::string::npos)
            warned = true;
    require(warned, "a type_test that can never match produced no warning");
    std::cout << "mir verifier type test: PASS\n";
}

void testUnionNullabilityFollowsItsMembers() {
    TypeArena arena;
    // `string` is a value type at runtime but the language lets it be null, so
    // it has to count as nullable - and a union is nullable when any member is.
    // Testing only the top-level kind would call `int|string` non-nullable and
    // reject every `null` arm of a match over it.
    require(arena.isNullable(arena.stringType()), "string was not treated as nullable");
    require(!arena.isNullable(arena.intType()), "int was treated as nullable");
    require(!arena.isNullable(arena.boolType()), "bool was treated as nullable");
    require(arena.isNullable(arena.unionType({arena.intType(), arena.stringType()})),
            "int|string was not treated as nullable");
    require(!arena.isNullable(arena.unionType({arena.intType(), arena.doubleType()})),
            "int|double was treated as nullable");
    std::cout << "mir union nullability: PASS\n";
}

void testNominalTypesCarryTheirName() {
    TypeArena arena;
    // `name` is the declaring name of a nominal type. Rendering never needed it
    // (the kind supplies "Task"/"Shared"), but every consumer asking "which
    // class is this?" does - a method call on such a receiver came back with no
    // class name at all when it was missing.
    require(arena.find(arena.taskType(arena.intType()))->name == "Task", "Task<T> has no name");
    require(arena.find(arena.sharedType(arena.intType()))->name == "Shared", "Shared<T> has no name");
    require(arena.render(arena.taskType(arena.intType())) == "Task<int>", "Task<int> rendered wrong");
    require(arena.render(arena.sharedType(arena.intType())) == "Shared<int>", "Shared<int> rendered wrong");
    std::cout << "mir nominal type names: PASS\n";
}

void testArraySizeIsOptional() {
    TypeArena arena;
    // A dynamic `array<int>` must not become `array[0]<int>`: the source never
    // stated a length, and inventing one changes the type.
    require(arena.render(arena.arrayType(arena.intType(), std::nullopt)) == "array<int>",
            "a dynamic array rendered as a fixed-size one");
    require(arena.render(arena.arrayType(arena.intType(), 10)) == "array[10]<int>",
            "a fixed-size array rendered wrong");
    require(arena.arrayType(arena.intType(), std::nullopt) != arena.arrayType(arena.intType(), 10),
            "a dynamic array and array[10] interned to the same type");
    std::cout << "mir array sizes: PASS\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Concurrency and native-boundary operations
// ---------------------------------------------------------------------------

// Declares a zero-argument closure body returning `returnType`. A capture is
// recorded as both a capture and the leading parameter, the way lowering
// represents captures, so make_closure's arity and capture checks see the
// same shape a real closure has.
FunctionId declareClosureBody(ModuleBuilder& builder, const std::string& name, TypeId returnType,
                              bool isAsync = false, TypeId captureType = 0,
                              const std::string& captureName = "cap", bool captureUsesThis = false) {
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction(name);
    fb.setLambda(true);
    fb.setAsync(isAsync);
    fb.setReturnType(returnType);
    if (captureType != 0) {
        (void)fb.addParameter(captureName, captureType);
        fb.addCapture(captureName, captureType, captureUsesThis);
    }
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    if (returnType == types.intType()) {
        fb.emitReturn(Operand::constant(builder.constantInt(0), types.intType()));
    } else if (returnType == types.boolType()) {
        fb.emitReturn(Operand::constant(builder.constantBool(true), types.boolType()));
    } else {
        fb.emitReturn();
    }
    const FunctionId id = fb.function().id;
    fb.finish();
    return id;
}

[[nodiscard]] TypeId syncFuncType(TypeArena& types, TypeId returnType) {
    FunctionSignature signature;
    signature.returnType = returnType;
    return types.functionType(signature);
}

void testConcurrencyOpcodeShapes() {
    // Suspension, blocking, and thread-boundary predicates drive scheduling
    // and code-motion decisions, so their exact membership is pinned here.
    require(opcodeIsSuspension(Opcode::Await), "await is not a suspension point");
    require(!opcodeIsSuspension(Opcode::TaskBlock), "task_block suspends; it blocks");
    require(!opcodeIsSuspension(Opcode::ChannelReceiveAsync), "channel_receive_async suspends; it only creates");

    require(opcodeIsBlocking(Opcode::TaskBlock), "task_block is not blocking");
    require(opcodeIsBlocking(Opcode::ThreadJoin), "thread_join is not blocking");
    require(opcodeIsBlocking(Opcode::ChannelSend), "channel_send is not blocking");
    require(opcodeIsBlocking(Opcode::ChannelReceive), "channel_receive is not blocking");
    require(opcodeIsBlocking(Opcode::MutexWithLock), "mutex_with_lock is not blocking");
    require(opcodeIsBlocking(Opcode::SemaphoreAcquire), "semaphore_acquire is not blocking");
    require(opcodeIsBlocking(Opcode::ConditionWait), "condition_wait is not blocking");
    require(opcodeIsBlocking(Opcode::ConditionWaitFor), "condition_wait_for is not blocking");
    require(!opcodeIsBlocking(Opcode::Await), "await blocks; it suspends");
    require(!opcodeIsBlocking(Opcode::ChannelSendAsync), "channel_send_async blocks; it only creates");
    require(!opcodeIsBlocking(Opcode::AtomicLoad), "atomic_load blocks; it is lock-free");

    require(opcodeIsThreadBoundary(Opcode::TaskSpawn), "task_spawn is not a thread boundary");
    require(opcodeIsThreadBoundary(Opcode::ThreadStart), "thread_start is not a thread boundary");
    require(!opcodeIsThreadBoundary(Opcode::Await), "await crosses threads; it stays on the scheduler");

    require(opcodeIsScopedLock(Opcode::MutexWithLock), "mutex_with_lock is not a scoped lock");
    require(opcodeIsScopedLock(Opcode::RwLockWithRead), "rwlock_with_read is not a scoped lock");
    require(opcodeIsScopedLock(Opcode::RwLockWithWrite), "rwlock_with_write is not a scoped lock");
    require(opcodeIsScopedLock(Opcode::SharedWithLock), "shared_with_lock is not a scoped lock");
    require(!opcodeIsScopedLock(Opcode::SharedGet), "shared_get is a scoped lock; it is one access");

    require(opcodeIsFfi(Opcode::FfiCall), "ffi_call is not FFI");
    require(opcodeIsFfi(Opcode::HandleBorrow), "handle_borrow is not FFI");
    require(opcodeIsFfi(Opcode::HandleConsume), "handle_consume is not FFI");
    require(opcodeIsFfi(Opcode::HandleClose), "handle_close is not FFI");
    require(opcodeIsFfi(Opcode::CallbackRegister), "callback_register is not FFI");
    require(opcodeIsFfi(Opcode::CallbackInvoke), "callback_invoke is not FFI");
    require(!opcodeIsFfi(Opcode::CallNative), "call_native is FFI; it is a VM builtin");

    // Fixed-result primitives record their own type, so the builder needs no
    // result argument and the verifier pins the spelling.
    require(opcodeShape(Opcode::AtomicLoad).operandCount == 1, "atomic_load takes no atomic operand");
    require(opcodeShape(Opcode::TaskSpawn).operandCount == 1, "task_spawn takes no closure operand");
    require(opcodeShape(Opcode::MutexWithLock).operandCount == 2, "mutex_with_lock is not binary");
    require(opcodeShape(Opcode::ChannelReceiveAsync).optionalResult == false,
            "channel_receive_async has an optional result; it always produces its task");
    require(opcodeShape(Opcode::Await).optionalResult, "await lost its optional result");

    TypeArena types;
    require(types.render(types.taskType(types.intType())) == "Task<int>", "Task rendering was wrong");
    require(types.render(types.threadType()) == "Thread", "Thread rendering was wrong");
    require(types.render(types.channelType(types.unknownType())) == "Channel", "Channel rendering was wrong");
    require(types.render(types.mutexType()) == "Mutex", "Mutex rendering was wrong");
    require(types.render(types.rwLockType()) == "RwLock", "RwLock rendering was wrong");
    require(types.render(types.atomicType()) == "Atomic", "Atomic rendering was wrong");
    require(types.render(types.semaphoreType()) == "Semaphore", "Semaphore rendering was wrong");
    require(types.render(types.conditionType()) == "Condition", "Condition rendering was wrong");
    require(types.render(types.sharedType(types.stringType())) == "Shared<string>",
            "Shared rendering was wrong");
    std::cout << "mir concurrency opcode shapes: PASS\n";
}

void testTaskSpawnAcceptsShareableClosure() {
    ModuleBuilder builder("spawnok");
    TypeArena& types = builder.types();
    const TypeId sharedInt = types.sharedType(types.intType());
    const FunctionId body = declareClosureBody(builder, "Spawn.body()", types.intType(), false, sharedInt);
    const FunctionId voidBody = declareClosureBody(builder, "Spawn.voidBody()", types.voidType());

    FunctionBuilder fb = builder.addFunction("Spawn.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId cell = fb.addParameter("cell", sharedInt);
    const TypeId funcType = syncFuncType(types, types.intType());
    const TempId closure =
        fb.emitMakeClosure(body, {fb.parameterOperand(cell)}, funcType);
    (void)fb.emitTaskSpawn(Operand::temp(closure, funcType), types.taskType(types.intType()));
    const TypeId voidFuncType = syncFuncType(types, types.voidType());
    const TempId voidClosure = fb.emitMakeClosure(voidBody, {}, voidFuncType);
    (void)fb.emitTaskSpawn(Operand::temp(voidClosure, voidFuncType), types.taskType(types.voidType()));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(report.ok(), "task_spawn of a shareable closure was rejected:\n" + reportText(report));
    std::cout << "mir verifier task spawn accepts: PASS\n";
}

void testTaskSpawnRejectsAsyncClosure() {
    ModuleBuilder builder("spawnasync");
    TypeArena& types = builder.types();
    const FunctionId body = declareClosureBody(builder, "Spawn.body()", types.intType(), /*isAsync=*/true);

    FunctionBuilder fb = builder.addFunction("Spawn.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    FunctionSignature signature;
    signature.returnType = types.intType();
    signature.isAsync = true;
    const TypeId funcType = types.functionType(signature);
    const TempId closure = fb.emitMakeClosure(body, {}, funcType);
    (void)fb.emitTaskSpawn(Operand::temp(closure, funcType), types.taskType(types.intType()));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(!report.ok(), "task_spawn of an async closure was accepted");
    require(hasError(report, "must be synchronous"), "the async-closure error was not reported:\n" +
                                                        reportText(report));
    std::cout << "mir verifier task spawn rejects async: PASS\n";
}

void testTaskSpawnRejectsUnshareableCapture() {
    ModuleBuilder builder("spawncapture");
    TypeArena& types = builder.types();
    const FunctionId body =
        declareClosureBody(builder, "Spawn.body()", types.voidType(), false, types.intType());

    FunctionBuilder fb = builder.addFunction("Spawn.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId count = fb.addParameter("count", types.intType());
    const TypeId funcType = syncFuncType(types, types.voidType());
    const TempId closure =
        fb.emitMakeClosure(body, {fb.parameterOperand(count)}, funcType);
    (void)fb.emitTaskSpawn(Operand::temp(closure, funcType), types.taskType(types.voidType()));
    fb.emitReturn();
    fb.finish();

    const auto report = verifyModule(builder.take());
    require(!report.ok(), "task_spawn capturing an int was accepted");
    require(hasError(report, "may cross a thread boundary"),
            "the confinement error was not reported:\n" + reportText(report));
    std::cout << "mir verifier task spawn rejects capture: PASS\n";
}

void testTaskSpawnRejectsNonTaskResult() {
    ModuleBuilder builder("spawnresult");
    TypeArena& types = builder.types();
    const FunctionId body = declareClosureBody(builder, "Spawn.body()", types.intType());

    FunctionBuilder fb = builder.addFunction("Spawn.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId funcType = syncFuncType(types, types.intType());
    const TempId closure = fb.emitMakeClosure(body, {}, funcType);
    (void)fb.emitTaskSpawn(Operand::temp(closure, funcType), types.intType());
    fb.emitReturn();
    fb.finish();

    Module module = builder.take();
    const auto report = verifyModule(module);
    require(!report.ok(), "task_spawn producing int was accepted");
    require(hasError(report, "must produce Task<T>"), "the non-task error was not reported:\n" +
                                                          reportText(report));

    // A payload mismatch is the same class of error through a different rule.
    module.functions[1].blocks[0].instructions[1].resultType = module.types.taskType(types.stringType());
    const auto mismatchReport = verifyModule(module);
    require(!mismatchReport.ok(), "task_spawn with a mismatched payload was accepted");
    require(hasError(mismatchReport, "produces Task<string>"),
            "the payload-mismatch error was not reported:\n" + reportText(mismatchReport));
    std::cout << "mir verifier task spawn result: PASS\n";
}

void testTaskBlockRules() {
    ModuleBuilder builder("blockok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Block.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId intTask = types.taskType(types.intType());
    const TypeId voidTask = types.taskType(types.voidType());
    const ParamId work = fb.addParameter("work", intTask);
    const ParamId done = fb.addParameter("done", voidTask);
    (void)fb.emitTaskBlock(fb.parameterOperand(work), types.intType());
    (void)fb.emitTaskBlock(fb.parameterOperand(done), 0);
    (void)fb.emitTaskBlock(fb.parameterOperand(done), types.nilType());
    fb.emitReturn();
    fb.finish();
    const auto report = verifyModule(builder.take());
    require(report.ok(), "well-formed task_block was rejected:\n" + reportText(report));

    // Blocking in async code is await's job.
    ModuleBuilder asyncBuilder("blockasync");
    TypeArena& atypes = asyncBuilder.types();
    FunctionBuilder afb = asyncBuilder.addFunction("Block.main()");
    afb.setAsync(true);
    afb.setReturnType(atypes.voidType());
    const BlockId aentry = afb.addBlock();
    afb.setCurrentBlock(aentry);
    const ParamId awork = afb.addParameter("work", atypes.taskType(atypes.intType()));
    (void)afb.emitTaskBlock(afb.parameterOperand(awork), atypes.intType());
    afb.emitReturn();
    afb.finish();
    const auto asyncReport = verifyModule(asyncBuilder.take());
    require(!asyncReport.ok(), "task_block in an async function was accepted");
    require(hasError(asyncReport, "await the task instead"),
            "the async-block error was not reported:\n" + reportText(asyncReport));

    // Blocking on something that is not a task.
    ModuleBuilder badBuilder("blockbad");
    TypeArena& btypes = badBuilder.types();
    FunctionBuilder bfb = badBuilder.addFunction("Block.main()");
    bfb.setReturnType(btypes.voidType());
    const BlockId bentry = bfb.addBlock();
    bfb.setCurrentBlock(bentry);
    const ParamId count = bfb.addParameter("count", btypes.intType());
    (void)bfb.emitTaskBlock(bfb.parameterOperand(count), btypes.intType());
    bfb.emitReturn();
    bfb.finish();
    const auto badReport = verifyModule(badBuilder.take());
    require(!badReport.ok(), "task_block of an int was accepted");
    require(hasError(badReport, "expects a Task"), "the non-task error was not reported:\n" +
                                                      reportText(badReport));
    std::cout << "mir verifier task block: PASS\n";
}

void testTaskCancelIgnoreRules() {
    ModuleBuilder builder("cancelok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Cancel.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId work = fb.addParameter("work", types.taskType(types.intType()));
    fb.emitTaskCancel(fb.parameterOperand(work));
    fb.emitTaskIgnore(fb.parameterOperand(work));
    fb.emitReturn();
    fb.finish();
    const auto report = verifyModule(builder.take());
    require(report.ok(), "well-formed task_cancel/task_ignore was rejected:\n" + reportText(report));

    ModuleBuilder badBuilder("cancelbad");
    TypeArena& btypes = badBuilder.types();
    FunctionBuilder bfb = badBuilder.addFunction("Cancel.main()");
    bfb.setReturnType(btypes.voidType());
    const BlockId bentry = bfb.addBlock();
    bfb.setCurrentBlock(bentry);
    const ParamId count = bfb.addParameter("count", btypes.intType());
    bfb.emitTaskCancel(bfb.parameterOperand(count));
    bfb.emitReturn();
    bfb.finish();
    const auto badReport = verifyModule(badBuilder.take());
    require(!badReport.ok(), "task_cancel of an int was accepted");
    require(hasError(badReport, "expects a Task"), "the non-task error was not reported:\n" +
                                                      reportText(badReport));
    std::cout << "mir verifier task cancel ignore: PASS\n";
}

void testThreadRules() {
    ModuleBuilder builder("threadok");
    TypeArena& types = builder.types();
    const FunctionId body = declareClosureBody(builder, "Worker.body()", types.intType());

    FunctionBuilder fb = builder.addFunction("Worker.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId funcType = syncFuncType(types, types.intType());
    const TempId closure = fb.emitMakeClosure(body, {}, funcType);
    const TempId thread =
        fb.emitThreadStart(Operand::temp(closure, funcType), types.threadType());
    fb.emitThreadJoin(Operand::temp(thread, types.threadType()));
    (void)fb.emitThreadIsAlive(Operand::temp(thread, types.threadType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed thread start/join/is_alive was rejected:\n" + reportText(report));

    // thread_is_alive always answers bool.
    Module mismatch = module;
    for (auto& instruction : mismatch.functions[1].blocks[0].instructions) {
        if (instruction.opcode == Opcode::ThreadIsAlive) instruction.resultType = mismatch.types.intType();
    }
    const auto mismatchReport = verifyModule(mismatch);
    require(!mismatchReport.ok(), "thread_is_alive producing int was accepted");
    require(hasError(mismatchReport, "must produce bool"),
            "the result-type error was not reported:\n" + reportText(mismatchReport));

    // Joining something that is not a thread.
    ModuleBuilder badBuilder("threadbad");
    TypeArena& btypes = badBuilder.types();
    FunctionBuilder bfb = badBuilder.addFunction("Worker.main()");
    bfb.setReturnType(btypes.voidType());
    const BlockId bentry = bfb.addBlock();
    bfb.setCurrentBlock(bentry);
    const ParamId count = bfb.addParameter("count", btypes.intType());
    bfb.emitThreadJoin(bfb.parameterOperand(count));
    bfb.emitReturn();
    bfb.finish();
    const auto badReport = verifyModule(badBuilder.take());
    require(!badReport.ok(), "thread_join of an int was accepted");
    require(hasError(badReport, "expects a Thread"), "the non-thread error was not reported:\n" +
                                                        reportText(badReport));
    std::cout << "mir verifier threads: PASS\n";
}

void testChannelRules() {
    ModuleBuilder builder("chanok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Chan.main()");
    fb.setReturnType(types.voidType());
    fb.setAsync(true);
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId channel = types.channelType(types.unknownType());
    const TempId handle = fb.emitChannelCreate(
        Operand::constant(builder.constantInt(4), types.intType()), channel);
    (void)fb.emitChannelSize(Operand::temp(handle, channel));
    fb.emitChannelSend(Operand::temp(handle, channel),
                       Operand::constant(builder.constantInt(1), types.intType()));
    (void)fb.emitChannelReceive(Operand::temp(handle, channel), types.unknownType());
    (void)fb.emitChannelSendAsync(Operand::temp(handle, channel),
                                  Operand::constant(builder.constantInt(2), types.intType()),
                                  types.taskType(types.voidType()));
    const TempId pending =
        fb.emitChannelReceiveAsync(Operand::temp(handle, channel), types.taskType(types.unknownType()));
    (void)fb.emitAwait(Operand::temp(pending, types.taskType(types.unknownType())), types.unknownType());
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed channel operations were rejected:\n" + reportText(report));

    // A string capacity is not a capacity.
    Module badCapacity = module;
    badCapacity.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::constant(0, badCapacity.types.stringType());
    const auto capacityReport = verifyModule(badCapacity);
    require(!capacityReport.ok(), "channel_create with a string capacity was accepted");
    require(hasError(capacityReport, "must be int"), "the capacity error was not reported:\n" +
                                                        reportText(capacityReport));

    // Receiving into a non-task future.
    Module badFuture = module;
    for (auto& instruction : badFuture.functions[0].blocks[0].instructions) {
        if (instruction.opcode == Opcode::ChannelReceiveAsync) instruction.resultType = badFuture.types.intType();
    }
    const auto futureReport = verifyModule(badFuture);
    require(!futureReport.ok(), "channel_receive_async producing int was accepted");
    require(hasError(futureReport, "must produce a Task"), "the future error was not reported:\n" +
                                                               reportText(futureReport));
    std::cout << "mir verifier channels: PASS\n";
}

void testScopedLockRules() {
    ModuleBuilder builder("lockok");
    TypeArena& types = builder.types();
    const FunctionId valueBody = declareClosureBody(builder, "Locks.value()", types.intType());
    const FunctionId voidBody = declareClosureBody(builder, "Locks.done()", types.voidType());

    FunctionBuilder fb = builder.addFunction("Locks.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId mutex = fb.addParameter("mutex", types.mutexType());
    const ParamId lock = fb.addParameter("lock", types.rwLockType());
    const ParamId cell = fb.addParameter("cell", types.sharedType(types.intType()));
    const TypeId valueFunc = syncFuncType(types, types.intType());
    const TypeId voidFunc = syncFuncType(types, types.voidType());
    const TempId valueClosure = fb.emitMakeClosure(valueBody, {}, valueFunc);
    const TempId voidClosure = fb.emitMakeClosure(voidBody, {}, voidFunc);
    (void)fb.emitMutexWithLock(fb.parameterOperand(mutex), Operand::temp(valueClosure, valueFunc),
                               types.intType());
    (void)fb.emitMutexWithLock(fb.parameterOperand(mutex), Operand::temp(voidClosure, voidFunc), 0);
    (void)fb.emitMutexWithLock(fb.parameterOperand(mutex), Operand::temp(voidClosure, voidFunc),
                               types.nilType());
    (void)fb.emitRwLockWithRead(fb.parameterOperand(lock), Operand::temp(valueClosure, valueFunc),
                                types.intType());
    (void)fb.emitRwLockWithWrite(fb.parameterOperand(lock), Operand::temp(voidClosure, voidFunc), 0);
    (void)fb.emitSharedWithLock(fb.parameterOperand(cell), Operand::temp(valueClosure, valueFunc),
                                types.intType());
    fb.emitReturn();
    fb.finish();
    const auto report = verifyModule(builder.take());
    require(report.ok(), "well-formed scoped locks were rejected:\n" + reportText(report));

    // A concretely-typed temp contradicts a void body.
    ModuleBuilder badBuilder("lockbad");
    TypeArena& btypes = badBuilder.types();
    const FunctionId badVoid = declareClosureBody(badBuilder, "Locks.done()", btypes.voidType());
    FunctionBuilder bfb = badBuilder.addFunction("Locks.main()");
    bfb.setReturnType(btypes.voidType());
    const BlockId bentry = bfb.addBlock();
    bfb.setCurrentBlock(bentry);
    const ParamId bmutex = bfb.addParameter("mutex", btypes.mutexType());
    const TypeId bvoidFunc = syncFuncType(btypes, btypes.voidType());
    const TempId bvoidClosure = bfb.emitMakeClosure(badVoid, {}, bvoidFunc);
    (void)bfb.emitMutexWithLock(bfb.parameterOperand(bmutex), Operand::temp(bvoidClosure, bvoidFunc),
                                btypes.intType());
    bfb.emitReturn();
    bfb.finish();
    const auto badReport = verifyModule(badBuilder.take());
    require(!badReport.ok(), "mutex_with_lock of a void closure with an int temp was accepted");
    require(hasError(badReport, "must not define a temp"),
            "the void-closure error was not reported:\n" + reportText(badReport));

    // A body that awaits suspends with the lock held.
    ModuleBuilder waitBuilder("lockwait");
    TypeArena& wtypes = waitBuilder.types();
    FunctionBuilder wfb = waitBuilder.addFunction("Locks.body()");
    wfb.setLambda(true);
    wfb.setAsync(true);
    wfb.setReturnType(wtypes.voidType());
    const BlockId wentry = wfb.addBlock();
    wfb.setCurrentBlock(wentry);
    const TypeId wtask = wtypes.taskType(wtypes.voidType());
    const TempId wpending =
        wfb.emitTaskCreate(Operand::constant(waitBuilder.constantNil(), wtypes.nilType()), wtask);
    (void)wfb.emitAwait(Operand::temp(wpending, wtask), 0);
    wfb.emitReturn();
    const FunctionId waiter = wfb.function().id;
    wfb.finish();
    FunctionBuilder wmain = waitBuilder.addFunction("Locks.main()");
    wmain.setReturnType(wtypes.voidType());
    const BlockId wmentry = wmain.addBlock();
    wmain.setCurrentBlock(wmentry);
    const ParamId wmutex = wmain.addParameter("mutex", wtypes.mutexType());
    FunctionSignature wsignature;
    wsignature.returnType = wtypes.voidType();
    wsignature.isAsync = true;
    const TypeId wfunc = wtypes.functionType(wsignature);
    const TempId wclosure = wmain.emitMakeClosure(waiter, {}, wfunc);
    (void)wmain.emitMutexWithLock(wmain.parameterOperand(wmutex), Operand::temp(wclosure, wfunc), 0);
    wmain.emitReturn();
    wmain.finish();
    const auto waitReport = verifyModule(waitBuilder.take());
    require(!waitReport.ok(), "mutex_with_lock of an awaiting closure was accepted");
    require(hasError(waitReport, "must be synchronous"),
            "the suspension-under-lock error was not reported:\n" + reportText(waitReport));
    std::cout << "mir verifier scoped locks: PASS\n";
}

void testAtomicRules() {
    ModuleBuilder builder("atomicok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Atomic.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId cell = fb.addParameter("cell", types.atomicType());
    const Operand atomic = fb.parameterOperand(cell);
    (void)fb.emitAtomicLoad(atomic);
    fb.emitAtomicStore(atomic, Operand::constant(builder.constantInt(1), types.intType()));
    (void)fb.emitAtomicAdd(atomic, Operand::constant(builder.constantInt(2), types.intType()));
    (void)fb.emitAtomicLoadBool(atomic);
    fb.emitAtomicStoreBool(atomic, Operand::constant(builder.constantBool(true), types.boolType()));
    (void)fb.emitAtomicLoadDouble(atomic);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed atomic operations were rejected:\n" + reportText(report));

    // A bool stored into the int lane.
    Module badStore = module;
    badStore.functions[0].blocks[0].instructions[1].operands[1] =
        Operand::constant(0, badStore.types.boolType());
    const auto storeReport = verifyModule(badStore);
    require(!storeReport.ok(), "atomic_store of a bool was accepted");
    require(hasError(storeReport, "must be int"), "the lane error was not reported:\n" +
                                                     reportText(storeReport));

    // A reference loaded as an int.
    ModuleBuilder refBuilder("atomicref");
    TypeArena& rtypes = refBuilder.types();
    FunctionBuilder rfb = refBuilder.addFunction("Atomic.main()");
    rfb.setReturnType(rtypes.voidType());
    const BlockId rentry = rfb.addBlock();
    rfb.setCurrentBlock(rentry);
    const ParamId rcell = rfb.addParameter("cell", rtypes.atomicType());
    (void)rfb.emitAtomicLoadRef(rfb.parameterOperand(rcell), rtypes.intType());
    rfb.emitReturn();
    rfb.finish();
    const auto refReport = verifyModule(refBuilder.take());
    require(!refReport.ok(), "atomic_load_ref producing int was accepted");
    require(hasError(refReport, "must produce a reference type"),
            "the reference error was not reported:\n" + reportText(refReport));
    std::cout << "mir verifier atomics: PASS\n";
}

void testSemaphoreRules() {
    ModuleBuilder builder("semok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Sem.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId gate = fb.addParameter("gate", types.semaphoreType());
    const Operand sem = fb.parameterOperand(gate);
    fb.emitSemaphoreAcquire(sem);
    fb.emitSemaphoreRelease(sem);
    (void)fb.emitSemaphoreAvailable(sem);
    fb.emitSemaphoreSetPermits(sem, Operand::constant(builder.constantInt(3), types.intType()));
    (void)fb.emitSemaphoreTryAcquire(sem);
    fb.emitSemaphoreReleaseMany(sem, Operand::constant(builder.constantInt(2), types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed semaphore operations were rejected:\n" + reportText(report));

    // Permits are counted in ints.
    Module badCount = module;
    badCount.functions[0].blocks[0].instructions[3].operands[1] =
        Operand::constant(0, badCount.types.boolType());
    const auto countReport = verifyModule(badCount);
    require(!countReport.ok(), "semaphore_set_permits with a bool count was accepted");
    require(hasError(countReport, "must be int"), "the count error was not reported:\n" +
                                                     reportText(countReport));
    std::cout << "mir verifier semaphores: PASS\n";
}

void testConditionRules() {
    ModuleBuilder builder("condok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Cond.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId gate = fb.addParameter("gate", types.conditionType());
    const Operand cond = fb.parameterOperand(gate);
    fb.emitConditionWait(cond);
    (void)fb.emitConditionWaitFor(cond, Operand::constant(builder.constantInt(5), types.intType()));
    fb.emitConditionNotifyOne(cond);
    fb.emitConditionNotifyAll(cond);
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed condition operations were rejected:\n" + reportText(report));
    require(hasWarning(report, "no predicate and no timeout"),
            "the bare-wait hazard produced no warning:\n" + reportText(report));

    // A timeout measured in strings.
    Module badTimeout = module;
    badTimeout.functions[0].blocks[0].instructions[1].operands[1] =
        Operand::constant(0, badTimeout.types.stringType());
    const auto timeoutReport = verifyModule(badTimeout);
    require(!timeoutReport.ok(), "condition_wait_for with a string timeout was accepted");
    require(hasError(timeoutReport, "must be numeric"), "the timeout error was not reported:\n" +
                                                           reportText(timeoutReport));
    std::cout << "mir verifier conditions: PASS\n";
}

void testSharedAccessRules() {
    ModuleBuilder builder("sharedok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Shared.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId sharedInt = types.sharedType(types.intType());
    const TempId cell = fb.emitSharedCreate(
        Operand::constant(builder.constantInt(0), types.intType()), sharedInt);
    (void)fb.emitSharedGet(Operand::temp(cell, sharedInt), types.intType());
    fb.emitSharedSet(Operand::temp(cell, sharedInt),
                     Operand::constant(builder.constantInt(1), types.intType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed shared access was rejected:\n" + reportText(report));
    require(hasWarning(report, "not atomic"),
            "unlocked get/set produced no atomicity warning:\n" + reportText(report));

    // A string stored into a Shared<int>.
    Module badStore = module;
    badStore.functions[0].blocks[0].instructions[2].operands[1] =
        Operand::constant(0, badStore.types.stringType());
    const auto storeReport = verifyModule(badStore);
    require(!storeReport.ok(), "shared_set of a string into Shared<int> was accepted");
    require(hasError(storeReport, "shared_set stores"), "the payload error was not reported:\n" +
                                                           reportText(storeReport));

    // A cell read out as the wrong type.
    Module badGet = module;
    badGet.functions[0].blocks[0].instructions[1].resultType = badGet.types.stringType();
    const auto getReport = verifyModule(badGet);
    require(!getReport.ok(), "shared_get of Shared<int> as string was accepted");
    require(hasError(getReport, "but the temp is typed"),
            "the result error was not reported:\n" + reportText(getReport));
    std::cout << "mir verifier shared access: PASS\n";
}

void testFfiCallRules() {
    ModuleBuilder builder("ffiok");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Ffi.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const ParamId handle = fb.addParameter("handle", types.nativeHandleType());
    (void)fb.emitFfiCall("lib", "add", {Operand::constant(builder.constantInt(1), types.intType())},
                         {NativeAbiTag::I64}, NativeAbiTag::I64, types.intType());
    (void)fb.emitFfiCall("lib", "open", {}, {}, NativeAbiTag::Handle, types.nativeHandleType(),
                         {}, NativeOwnership::Owned);
    (void)fb.emitHandleBorrow(fb.parameterOperand(handle), types.nativeHandleType());
    // consume transfers the handle: only the resulting value may be closed.
    const auto transferred = fb.emitHandleConsume(fb.parameterOperand(handle), types.nativeHandleType());
    fb.emitHandleClose(Operand::temp(transferred, types.nativeHandleType()));
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "well-formed FFI calls were rejected:\n" + reportText(report));

    // A handle smuggled back as a borrow.
    Module badOwnership = module;
    badOwnership.functions[0].blocks[0].instructions[1].target.ffiReturnOwnership =
        NativeOwnership::Borrowed;
    const auto ownershipReport = verifyModule(badOwnership);
    require(!ownershipReport.ok(), "an ffi_call returning a borrowed handle was accepted");
    require(hasError(ownershipReport, "only an Owned handle may be returned"),
            "the ownership error was not reported:\n" + reportText(ownershipReport));

    // A view returned across the boundary.
    Module badView = module;
    badView.functions[0].blocks[0].instructions[0].target.ffiReturnTag = NativeAbiTag::BufferView;
    const auto viewReport = verifyModule(badView);
    require(!viewReport.ok(), "an ffi_call returning a view was accepted");
    require(hasError(viewReport, "cannot be returned"), "the view error was not reported:\n" +
                                                           reportText(viewReport));

    // A bool marshalled as an integer.
    Module badParam = module;
    badParam.functions[0].blocks[0].instructions[0].operands[0] =
        Operand::constant(0, badParam.types.boolType());
    const auto paramReport = verifyModule(badParam);
    require(!paramReport.ok(), "an ffi_call with a mistagged argument was accepted");
    require(hasError(paramReport, "but the ABI tag is"), "the tag error was not reported:\n" +
                                                             reportText(paramReport));
    std::cout << "mir verifier ffi calls: PASS\n";
}

void testCallbackRules() {
    ModuleBuilder builder("cbok");
    TypeArena& types = builder.types();
    const FunctionId body = declareClosureBody(builder, "Cb.handler()", types.voidType());

    FunctionSignature signature;
    signature.parameterTypes = {types.intType()};
    signature.returnType = types.voidType();
    const TypeId token = types.nativeCallbackType(signature);

    FunctionBuilder fb = builder.addFunction("Cb.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId funcType = syncFuncType(types, types.voidType());
    const TempId closure = fb.emitMakeClosure(body, {}, funcType);
    const TempId callback = fb.emitCallbackRegister(Operand::temp(closure, funcType), token);
    (void)fb.emitCallbackInvoke(Operand::temp(callback, token),
                                {Operand::constant(builder.constantInt(7), types.intType())}, 0);
    fb.emitCallbackClose(Operand::temp(callback, token));
    fb.emitReturn();
    fb.finish();
    const auto report = verifyModule(builder.take());
    require(report.ok(), "well-formed callback registration was rejected:\n" + reportText(report));

    // An async handler has no scheduler to park on.
    ModuleBuilder asyncBuilder("cbbad");
    TypeArena& atypes = asyncBuilder.types();
    const FunctionId asyncBody =
        declareClosureBody(asyncBuilder, "Cb.handler()", atypes.voidType(), /*isAsync=*/true);
    FunctionBuilder afb = asyncBuilder.addFunction("Cb.main()");
    afb.setReturnType(atypes.voidType());
    const BlockId aentry = afb.addBlock();
    afb.setCurrentBlock(aentry);
    FunctionSignature asignature;
    asignature.returnType = atypes.voidType();
    asignature.isAsync = true;
    const TypeId afunc = atypes.functionType(asignature);
    const TempId aclosure = afb.emitMakeClosure(asyncBody, {}, afunc);
    (void)afb.emitCallbackRegister(Operand::temp(aclosure, afunc), atypes.nativeCallbackType());
    afb.emitReturn();
    afb.finish();
    const auto asyncReport = verifyModule(asyncBuilder.take());
    require(!asyncReport.ok(), "callback_register of an async closure was accepted");
    require(hasError(asyncReport, "must be synchronous"),
            "the async-callback error was not reported:\n" + reportText(asyncReport));
    std::cout << "mir verifier callbacks: PASS\n";
}

void testBorrowOfTaskRejects() {
    ModuleBuilder builder("borrowtask");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Tasks.main()");
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    (void)fb.addSlot("work", types.taskType(types.intType()), true, zl::OwnershipKind::BORROW);
    fb.emitReturn();
    fb.finish();
    const auto report = verifyModule(builder.take());
    require(!report.ok(), "a borrow slot of Task type was accepted");
    require(hasError(report, "borrow is not supported for task handles"),
            "the task-borrow error was not reported:\n" + reportText(report));
    std::cout << "mir verifier task borrow: PASS\n";
}


void testAwaitOfVoidTaskAcceptsUnknownTemp() {
    // The checker leaves some void-awaits unnamed (an await of a
    // Channel.sendAsync task observes unknown), while the runtime still
    // resumes with nil. An unknown-typed temp names that nil honestly; only
    // a concretely-typed temp contradicts the void payload.
    ModuleBuilder builder("awaitunknown");
    TypeArena& types = builder.types();
    FunctionBuilder fb = builder.addFunction("Tasks.run()");
    fb.setAsync(true);
    fb.setReturnType(types.voidType());
    const BlockId entry = fb.addBlock();
    fb.setCurrentBlock(entry);
    const TypeId voidTask = types.taskType(types.voidType());
    const ParamId pending = fb.addParameter("pending", voidTask);
    (void)fb.emitAwait(fb.parameterOperand(pending), types.unknownType());
    fb.emitReturn();
    fb.finish();
    Module module = builder.take();
    const auto report = verifyModule(module);
    require(report.ok(), "awaiting a Task<void> into an unknown temp was rejected:\n" +
                             reportText(report));

    module.functions[0].blocks[0].instructions[0].resultType = module.types.intType();
    const auto concrete = verifyModule(module);
    require(!concrete.ok(), "awaiting a Task<void> into an int temp was accepted");
    require(hasError(concrete, "must not define a temp"),
            "the concrete-temp error was not reported:\n" + reportText(concrete));
    std::cout << "mir verifier await void temp: PASS\n";
}


// ---------------------------------------------------------------------------
// Reachability: function-value execution edges
// ---------------------------------------------------------------------------
//
// These modules are built by hand rather than lowered, because the point is
// to prove each *opcode* that executes a function value is classified: a
// proven body is a closed edge, an unprovable value opens the graph, and
// nothing is silently dropped. The pipeline tests cover the same
// classification from ZL source.

FunctionId findFunctionByName(const Module& module, const std::string& name) {
    for (const auto& function : module.functions)
        if (function.name == name) return function.id;
    return kNoFunction;
}

// Builds:  Work.run(int): int  { return a + 1 }
//          Work.main(): void   { %c = make_closure run
//                                 task_spawn %c
//                                 shared_with_lock(cell, %c)
//                                 thread_start %c }
// Every one of those executes the same provably single closure body.
Module buildClosureValueModule() {
    ModuleBuilder builder("work");
    TypeArena& types = builder.types();

    FunctionSignature closureSignature;
    closureSignature.parameterTypes = {types.intType()};
    closureSignature.returnType = types.intType();
    const TypeId closureType = types.functionType(closureSignature);

    FunctionId runId = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Work.run(int)");
        fb.setReturnType(types.intType());
        const ParamId a = fb.addParameter("a", types.intType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const TempId plus =
            fb.emitBinary(Opcode::Add, fb.parameterOperand(a),
                          Operand::constant(builder.constantInt(1), types.intType()), types.intType());
        fb.emitReturn(Operand::temp(plus, types.intType()));
        fb.finish();
        runId = fb.function().id;
    }

    // `shared_with_lock` reaches the `Shared` class's `withLock` method - the
    // bytecode backend materialises that dispatch even though the MIR opcode
    // names no callee - so, exactly as in a real lowered module, the class
    // must own the method here. Without it the site is a dispatch nothing can
    // serve, and the report is (correctly) incomplete.
    {
        FunctionBuilder fb = builder.addFunction("Shared.withLock(object)");
        fb.setReturnType(types.voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        fb.emitReturn();
        fb.finish();
    }

    FunctionId mainId = kNoFunction;
    {
        FunctionBuilder fb = builder.addFunction("Work.main()");
        fb.setReturnType(types.voidType());
        const ParamId cell = fb.addParameter("cell", types.objectType("Shared"));
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const TempId closure = fb.emitMakeClosure(runId, {}, closureType);
        (void)fb.emitTaskSpawn(Operand::temp(closure, closureType), types.taskType(types.intType()));
        (void)fb.emitSharedWithLock(fb.parameterOperand(cell), Operand::temp(closure, closureType), 0);
        (void)fb.emitThreadStart(Operand::temp(closure, closureType), types.objectType("Thread"));
        fb.emitReturn();
        fb.finish();
        mainId = fb.function().id;
    }
    builder.setEntryPoint(mainId);
    return builder.take();
}

void testReachabilityProvesClosureValueEdges() {
    const Module module = buildClosureValueModule();
    const ReachabilityReport report = reachableFunctions(module);

    require(report.hasEntryPoint, "the module has an entry point");
    require(report.complete(),
            "spawned/locked closures pinned to one body are closed edges");
    require(!report.unresolvedCalls, "so no function-value edge was left open");
    require(report.contains(findFunctionByName(module, "Work.run(int)")),
            "the closure body is reachable through every execute site");
}

// The closure is a parameter: the caller chose it, so Task.spawn,
// Thread.start, and the withLock family each open the graph.
Module buildUnprovenClosureValueModule() {
    ModuleBuilder builder("work2");
    TypeArena& types = builder.types();

    FunctionSignature closureSignature;
    closureSignature.parameterTypes = {types.intType()};
    closureSignature.returnType = types.intType();
    const TypeId closureType = types.functionType(closureSignature);

    {
        FunctionBuilder fb = builder.addFunction("Work2.spawnIt(func)");
        fb.setReturnType(types.voidType());
        const ParamId f = fb.addParameter("f", closureType);
        const ParamId g = fb.addParameter("g", closureType);
        const ParamId cell = fb.addParameter("cell", types.objectType("Shared"));
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        (void)fb.emitTaskSpawn(fb.parameterOperand(f), types.taskType(types.intType()));
        (void)fb.emitThreadStart(fb.parameterOperand(f), types.objectType("Thread"));
        (void)fb.emitMutexWithLock(fb.parameterOperand(cell), fb.parameterOperand(f), 0);
        // A multi-store slot: the value can be either parameter.
        const SlotId slot = fb.addSlot("s", closureType);
        fb.emitStore(slot, fb.parameterOperand(f));
        fb.emitStore(slot, fb.parameterOperand(g));
        const TempId loaded = fb.emitLoad(slot);
        (void)fb.emitCallIndirect(Operand::temp(loaded, closureType), {}, types.intType());
        fb.emitReturn();
        fb.finish();
    }
    builder.setEntryPoint(builder.findFunction("Work2.spawnIt(func)"));
    return builder.take();
}

void testReachabilityUnprovenClosureValuesOpenTheGraph() {
    const Module module = buildUnprovenClosureValueModule();
    const ReachabilityReport report = reachableFunctions(module);

    require(report.hasEntryPoint, "the module has an entry point");
    require(report.unresolvedCalls,
            "a parameter-held function value has no statically known target");
    // task_spawn, thread_start, mutex_with_lock, and the call through the
    // multi-store slot: four open edges.
    require(report.unresolvedCallCount == 4,
            "every unprovable execute site is counted (got " +
                std::to_string(report.unresolvedCallCount) + ")");
    require(!report.complete(),
            "the reachable set is a lower bound, not a removal guarantee");
    require(!report.unresolvedCallReason.empty(), "and the report says where it is open");
}

// A slot with zero stores: reading it before any write is not provable, and
// the edge must stay open rather than be answered from an absence.
void testReachabilitySlotWithoutStoresStaysOpen() {
    ModuleBuilder builder("work3");
    TypeArena& types = builder.types();

    FunctionSignature closureSignature;
    closureSignature.parameterTypes = {types.intType()};
    closureSignature.returnType = types.intType();
    const TypeId closureType = types.functionType(closureSignature);

    {
        FunctionBuilder fb = builder.addFunction("Work3.readIt()");
        fb.setReturnType(types.voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        const SlotId slot = fb.addSlot("s", closureType);
        const TempId loaded = fb.emitLoad(slot);
        (void)fb.emitCallIndirect(Operand::temp(loaded, closureType), {}, types.intType());
        fb.emitReturn();
        fb.finish();
    }
    builder.setEntryPoint(builder.findFunction("Work3.readIt()"));
    const Module module = builder.take();
    const ReachabilityReport report = reachableFunctions(module);
    require(report.unresolvedCalls,
            "a slot with no store cannot be pinned to a body");
    require(!report.complete(), "so the graph stays open, not guessed");
}


// ---------------------------------------------------------------------------
// Reachability: dispatch resolution at hierarchy scale
// ---------------------------------------------------------------------------
//
// The hierarchy the dispatch resolver runs over is built once per analysis:
// subtypes are precomputed, and each (receiver, method) pair is resolved once
// and cached for the run. This test builds a wide, deep hierarchy with many
// dispatch sites all sharing a few (receiver, method) pairs, and checks the
// resolution stays exact: every override in the receiver's hierarchy is kept,
// every interface implementor is kept, and nothing outside the hierarchy is.

// Builds:
//   * a chain of 250 classes: Base <- C01 <- C02 ... <- C249, each with m();
//   * an interface chain I0 <- I1 <- I2, implemented by Impl, which alone
//     defines m2();
//   * a Loner class with a same-named m() and no relationship to Base;
//   * main, which calls ten dispatcher functions, each holding 20 invoke
//     sites on Base.m and two on I0.m2 - 200 dispatch sites that all resolve
//     to the same two (receiver, method) pairs.
Module buildLargeHierarchyModule() {
    constexpr std::size_t kChainLength = 249;
    constexpr std::size_t kDispatchers = 10;
    constexpr std::size_t kSitesPerDispatcher = 20;

    ModuleBuilder builder("hierarchy");
    TypeArena& types = builder.types();
    const TypeId receiverType = types.objectType("Base");

    auto addMethod = [&](const std::string& className, const std::string& methodName) {
        FunctionBuilder fb = builder.addFunction(className + "." + methodName + "()");
        fb.setReturnType(types.stringType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        fb.emitReturn(Operand::constant(builder.constantString(className), types.stringType()));
        fb.finish();
    };

    for (std::size_t i = 0; i < kChainLength; ++i) {
        const std::string name = "C" + std::to_string(i + 1);
        ClassLayout& layout = builder.addClassLayout(name);
        layout.parent = (i == 0) ? "Base" : "C" + std::to_string(i);
        addMethod(name, "m");
    }
    (void)builder.addClassLayout("Base");
    addMethod("Base", "m");

    (void)builder.addInterface("I0");
    InterfaceInfo& i1 = builder.addInterface("I1");
    i1.bases = {"I0"};
    InterfaceInfo& i2 = builder.addInterface("I2");
    i2.bases = {"I1"};
    ClassLayout& impl = builder.addClassLayout("Impl");
    impl.interfaces = {"I2"};
    addMethod("Impl", "m2");

    ClassLayout& loner = builder.addClassLayout("Loner");
    (void)loner;
    addMethod("Loner", "m");

    FunctionId mainId = kNoFunction;
    for (std::size_t d = 0; d < kDispatchers; ++d) {
        const std::string name = "Dispatcher" + std::to_string(d) + ".dispatch()";
        FunctionBuilder fb = builder.addFunction(name);
        fb.setReturnType(types.voidType());
        const ParamId receiver = fb.addParameter("r", receiverType);
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        for (std::size_t s = 0; s < kSitesPerDispatcher; ++s) {
            (void)fb.emitInvokeMethod(fb.parameterOperand(receiver), "Base", "m", {}, types.stringType());
        }
        (void)fb.emitInvokeMethod(fb.parameterOperand(receiver), "I0", "m2", {}, types.stringType());
        fb.emitReturn();
        fb.finish();
        if (d == 0) mainId = fb.function().id; // not the entry; main calls all of them
    }

    {
        FunctionBuilder fb = builder.addFunction("Harness.main()");
        fb.setReturnType(types.voidType());
        const BlockId entry = fb.addBlock();
        fb.setCurrentBlock(entry);
        for (std::size_t d = 0; d < kDispatchers; ++d) {
            const FunctionId dispatcher =
                builder.findFunction("Dispatcher" + std::to_string(d) + ".dispatch()");
            (void)fb.emitCall(dispatcher, {}, 0);
        }
        fb.emitReturn();
        fb.finish();
        mainId = fb.function().id;
    }
    builder.setEntryPoint(mainId);
    return builder.take();
}

void testReachabilityDispatchScalesWithTheHierarchy() {
    const Module module = buildLargeHierarchyModule();
    const ReachabilityReport report = reachableFunctions(module);

    require(report.hasEntryPoint, "the module has an entry point");
    require(report.complete(), "pure virtual dispatch is a closed (over-approximated) edge");

    // Every override in the receiver's hierarchy is kept, including the
    // dead subclasses no path constructs - the over-approximation promise.
    for (std::size_t i = 0; i < 249; ++i) {
        const std::string id = "C" + std::to_string(i + 1) + ".m()";
        const FunctionId found = [module, id]() {
            for (const auto& function : module.functions)
                if (function.name == id) return function.id;
            return kNoFunction;
        }();
        require(report.contains(found), "override " + id + " of the called method is reachable");
    }
    const FunctionId base = [&]() {
        for (const auto& function : module.functions)
            if (function.name == "Base.m()") return function.id;
        return kNoFunction;
    }();
    require(report.contains(base), "the base implementation is reachable");

    // The interface chain reaches its implementor's method...
    const FunctionId m2 = [&]() {
        for (const auto& function : module.functions)
            if (function.name == "Impl.m2()") return function.id;
        return kNoFunction;
    }();
    require(report.contains(m2), "an implementor behind an interface chain is reachable");

    // ...and nothing outside the receiver's hierarchy is kept by name alone.
    const FunctionId loner = [&]() {
        for (const auto& function : module.functions)
            if (function.name == "Loner.m()") return function.id;
        return kNoFunction;
    }();
    require(!report.contains(loner),
            "a same-named method outside the receiver's hierarchy is not reachable");

    // The caches belong to the analysis run: a second pass over the same
    // module answers identically, and the module itself was not touched.
    const ReachabilityReport again = reachableFunctions(module);
    require(again.functions == report.functions,
            "re-running the analysis gives the same reachable set (no leaked state)");
    require(again.unresolvedCalls == report.unresolvedCalls,
            "and the same openness");
}

int main() {
    testConstantInterning();
    testTypeArena();
    testValidModuleVerifies();
    testControlFlowAnalysis();

    testRejectsMissingBlockReference();
    testRejectsMissingTerminator();
    testRejectsUnreachableBlock();
    testRejectsInconsistentPredecessors();
    testRejectsDominanceViolation();
    testRejectsForwardUse();

    testRejectsUndefinedTemp();
    testRejectsWrongOperandType();
    testRejectsWrongResultType();
    testRejectsBadConstant();
    testRejectsBadParameterReference();
    testRejectsWrongOperandCount();

    testRejectsInvalidReturn();
    testRejectsMissingReturnValue();
    testRejectsReturnTypeMismatch();
    testRejectsMalformedSignature();
    testRejectsNonBoolBranch();
    testRejectsEntryWithPredecessor();
    testRejectsCallArityMismatch();
    testRejectsCallArgumentType();

    testRejectsAwaitOutsideAsync();
    testRejectsStoreToImmutableSlot();
    testRejectsMoveOfNonOwnedSlot();
    testRejectsUseAfterMove();
    testRejectsMoveWhileBorrowed();
    testRejectsDropOfBorrow();
    testRejectsEndBorrowWithoutBorrow();
    testRejectsNullCheckOnNonNullable();
    testRejectsUndeclaredTypeParamUse();
    testAcceptsDeclaredTypeParam();

    testExceptionEdges();

    testInheritedFieldAccess();
    testListObjectIndexing();
    testEnumMemberConstant();
    testGenericCallInstantiation();
    testSharedIsReferenceType();
    testAwaitOfVoidTaskDefinesNoTemp();
    testMapIndexAccess();
    testSetIndexing();
    testCollectionClassFormsAreConstructible();
    testTypeTest();
    testUnionNullabilityFollowsItsMembers();
    testNominalTypesCarryTheirName();
    testArraySizeIsOptional();

    testConcurrencyOpcodeShapes();
    testTaskSpawnAcceptsShareableClosure();
    testTaskSpawnRejectsAsyncClosure();
    testTaskSpawnRejectsUnshareableCapture();
    testTaskSpawnRejectsNonTaskResult();
    testTaskBlockRules();
    testTaskCancelIgnoreRules();
    testThreadRules();
    testChannelRules();
    testScopedLockRules();
    testAtomicRules();
    testSemaphoreRules();
    testConditionRules();
    testSharedAccessRules();
    testFfiCallRules();
    testCallbackRules();
    testBorrowOfTaskRejects();
    testAwaitOfVoidTaskAcceptsUnknownTemp();

    testReachabilityProvesClosureValueEdges();
    testReachabilityUnprovenClosureValuesOpenTheGraph();
    testReachabilitySlotWithoutStoresStaysOpen();
    testReachabilityDispatchScalesWithTheHierarchy();

    if (failures != 0) {
        std::cerr << failures << " MIR regression(s) failed\n";
        return 1;
    }
    std::cout << "all MIR regressions passed\n";
    return 0;
}
