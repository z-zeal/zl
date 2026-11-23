#include "zl/compiler/compiler.hpp"
#include "zl/compiler/semantic_types.hpp"
#include "zl/parser/type_annotation.hpp"
#include "zl/vm/gc.hpp"
#include <functional>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <optional>
#include <cstdint>

namespace zl {
namespace {

std::size_t typeArgNameOperand(Chunk& chunk, const std::vector<std::string>& names) {
    if (names.empty()) return 0;
    std::string joined = names.front();
    for (std::size_t i = 1; i < names.size(); ++i) {
        joined += ';';
        joined += names[i];
    }
    return chunk.addName(joined) + 1;
}

// Can control flow leave `node` the ordinary way - by running off its end?
// Every function body gets an implicit `return nil` for the case where it can,
// so a body that provably cannot needs no tail: emitting one anyway appends
// instructions the VM can never reach (a `return` or `throw` does not fall
// through). Deliberately conservative - only shapes whose every exit is
// visible in the AST are recognised, so a `break` that escapes a loop, a
// `finally` that swallows a throw, or any statement kind not listed here all
// keep the implicit tail. Getting this wrong in the other direction would drop
// a return the VM needs; this way it costs at most a few dead bytes.
bool canFallThrough(const AstNode* node) {
    if (node == nullptr) return true;
    switch (node->kind) {
        case NodeKind::ReturnStmt:
        case NodeKind::ThrowStmt:
            return false;
        case NodeKind::BlockStmt: {
            const auto* block = static_cast<const BlockStmt*>(node);
            return block->statements.empty() ||
                   canFallThrough(block->statements.back().get());
        }
        case NodeKind::IfStmt: {
            const auto* ifStmt = static_cast<const IfStmt*>(node);
            // An `if` without an `else` always has a path that skips it.
            if (!ifStmt->elseBody) return true;
            for (const auto& branch : ifStmt->branches) {
                if (canFallThrough(branch.body.get())) return true;
            }
            return canFallThrough(ifStmt->elseBody.get());
        }
        default:
            return true;
    }
}

} // namespace

std::size_t Compiler::emit(OpCode op, std::size_t operand, std::size_t line, std::size_t operand2, std::size_t operand3) {
    chunk_.code.push_back(Instruction{op, operand, line, operand2, operand3});
    return chunk_.code.size() - 1;
}

void Compiler::patchJump(std::size_t jumpInstructionIndex) {
    chunk_.code[jumpInstructionIndex].operand = chunk_.code.size();
}

DispatchSignature Compiler::dispatchSignature(const FunctionDecl& func,
                                               const std::vector<std::string>& genericTypeParams) {
    return dispatchSignatureForFunction(func, genericTypeParams);
}

std::size_t Compiler::methodSlot(const DispatchSignature& dispatchKey) const {
    auto it = methodSlots_.find(dispatchKey);
    if (it == methodSlots_.end()) {
        throw std::runtime_error("Compiler: no dispatch slot for method '" + dispatchKey.describe() + "'");
    }
    return it->second;
}

std::size_t Compiler::resolveFunctionIndex(const std::string& className,
                                           const DispatchSignature& signature) const {
    std::string cur = className;
    while (!cur.empty()) {
        auto it = functionIndices_.find(FunctionKey{cur, signature});
        if (it != functionIndices_.end()) return it->second;

        auto parentIt = classParents_.find(cur);
        cur = (parentIt != classParents_.end()) ? parentIt->second : std::string();
    }

    throw std::runtime_error("Compiler: func " + signature.describe() +
                             " is not registered in class '" + className + "' or its ancestors");
}

Chunk Compiler::compile(const Program& program) {
    // No IR lowering happens here. This compiler is the *reference* AST ->
    // bytecode path, kept for differential comparison against the MIR pipeline
    // (`ZL_COMPILER=ast`), and it used to lower the whole program into the
    // legacy `zl::ir` and discard the result on every compile. That was a second
    // semantic lowering with no consumer: MIR is the compiler boundary now, and
    // the only remaining legacy consumer is the portable-C++ emitter behind
    // `--emit-native`. Deleting the call also deletes the last reason for the
    // default path to know `zl::ir` exists.
    chunk_ = Chunk{};
    classParents_.clear();
    methodSlots_.clear();
    functionIndices_.clear();
    loopStack_.clear();
    activeHandlerDepth_ = 0;
    activeOwnedLocalNames_.clear();

    CompilationPlan plan = buildCompilationPlan(program);
    const auto& allFunctions = plan.allFunctions;
    const auto* mainFn = plan.mainFunction;
    const auto& classTypeParams = plan.classTypeParams;
    classParents_ = plan.classParents;

    chunk_.classReflection = plan.classReflection;

    // Runtime signature names keep a generic method's own type parameters as
    // their tokens (e.g. `List<T>.push(T item)` records parameter type "T").
    // The static type checker already enforces types inside a generic body;
    // the VM substitutes the receiver's concrete instantiation (T->string for a
    // List<string>) before asserting at the call boundary, so generic writes
    // are enforced even where the element type was statically erased. We render
    // from the annotation AST (describeTypeAnnotation), which keeps concrete
    // types even if their spelling coincides with a parameter elsewhere.
    auto runtimeTypeName = [](const TypeAnnotation& type) {
        return describeTypeAnnotation(type);
    };

    // --- pass 1: register every func's signature up front, so calls can ---
    // --- resolve regardless of declaration order (forward references work) ---
    // Qualified as "<ownerClassName>.<functionName>" - not just the bare
    // name - because imported classes can legitimately share method names.
    // InvokeMethod selects the exact compiled dispatch slot from the
    // receiver's runtime class vtable.
    for (const auto* fn : allFunctions) {
        std::vector<std::string> paramNames;
        std::vector<std::string> parameterTypeNames;
        paramNames.reserve(fn->params.size());
        parameterTypeNames.reserve(fn->params.size());
        for (const auto& p : fn->params) {
            paramNames.push_back(p.storageName);
            parameterTypeNames.push_back(runtimeTypeName(p.type));
        }
        const DispatchSignature signature = dispatchSignature(*fn, classTypeParams.at(fn->ownerClassName));
        std::string qualifiedName = fn->ownerClassName + "." + signature.describe();
        FunctionInfo info{std::move(qualifiedName), std::move(paramNames), std::move(parameterTypeNames),
                          ((fn->returnType.name.empty() && fn->returnType.unionOf.empty()) ? "void" : runtimeTypeName(fn->returnType)), 0, fn->isStatic, fn->isAsync};
        info.typeParameters = fn->typeParams;
        info.ownerClassName = fn->ownerClassName;
        info.isNative = std::any_of(fn->annotations.begin(), fn->annotations.end(), [](const Annotation& a) { return a.name == "native"; });
        info.dispatchSignature = signature;
        info.capturesEvaluationScope = false;
        const std::size_t functionIndex = chunk_.functions.size();
        chunk_.functions.push_back(std::move(info));
        functionIndices_[FunctionKey{fn->ownerClassName, chunk_.functions.back().dispatchSignature}] = functionIndex;
    }

    // Register static-field initializer functions before compiling any bodies.
    // They are zero-argument internal functions and execute lazily on first access.
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        for (const auto& member : cls->members) {
            if (member->kind != NodeKind::VarDecl) continue;
            const auto* field = static_cast<const VarDecl*>(member.get());
            if (!field->isStatic) continue;
            FunctionInfo info;
            info.name = cls->name + ".<static-init:" + field->name + ">";
            info.returnTypeName = field->hasExplicitType ? describeTypeAnnotation(field->type) : "unknown";
            info.ownerClassName = cls->name;
            info.isStatic = true;
            info.isAsync = false;
            info.capturesEvaluationScope = false;
            const std::size_t functionIndex = chunk_.functions.size();
            chunk_.functions.push_back(std::move(info));
            chunk_.staticFields[cls->name + "." + field->name] = StaticFieldInfo{cls->name, field->name, functionIndex};
        }
    }

    // Bind reflection metadata to stable compiled function indices.
    for (auto& [className, info] : chunk_.classReflection) {
        for (auto& method : info.methods) {
            if (!method.ownerClassName.empty()) {
                DispatchSignature sig{method.name, {}};
                // Reflection metadata carries the exact dispatch signature produced from the AST.
                // Fall back to legacy parameter-name reconstruction only for old metadata.
                if (!method.dispatchSignature.empty()) {
                    sig = DispatchSignature{method.name, {}};
                    // Find the exact registered function by matching its fully described signature.
                    const std::string expected = method.name + method.dispatchSignature;
                    bool matched = false;
                    for (const auto& [key, index] : functionIndices_) {
                        if (key.ownerClassName == method.ownerClassName && key.signature.describe() == expected) {
                            method.functionIndex = index;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) throw std::runtime_error("Compiler: reflection method '" + method.ownerClassName + "." + expected + "' is not registered");
                } else {
                    throw std::runtime_error("Compiler: reflection method '" + method.ownerClassName + "." + method.name + "' has no dispatch signature");
                }
            }
        }
        for (auto& ctor : info.constructors) {
            if (!ctor.ownerClassName.empty()) {
                DispatchSignature sig{ctor.ownerClassName, {}};
                if (!ctor.dispatchSignature.empty()) {
                    const std::string expected = ctor.ownerClassName + ctor.dispatchSignature;
                    bool matched = false;
                    for (const auto& [key, index] : functionIndices_) {
                        if (key.ownerClassName == ctor.ownerClassName && key.signature.name == ctor.ownerClassName && key.signature.describe() == expected) {
                            ctor.functionIndex = index;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) throw std::runtime_error("Compiler: reflection constructor '" + expected + "' is not registered");
                } else {
                    throw std::runtime_error("Compiler: reflection constructor '" + ctor.ownerClassName + "' has no dispatch signature");
                }
            }
        }
        if (info.runtimeType) {
            auto runtimeType = std::make_shared<RuntimeTypeInfo>(*info.runtimeType);
            runtimeType->methods = info.methods;
            runtimeType->constructors = info.constructors;
            info.runtimeType = std::move(runtimeType);
        }
    }

    // Build compile-time virtual-dispatch slots and per-class vtables.
    DispatchTableSet dispatchTables =
        buildDispatchTables(allFunctions, classTypeParams, classParents_);
    methodSlots_ = std::move(dispatchTables.methodSlots);
    chunk_.classVTables = std::move(dispatchTables.classVTables);

    // Skip over every func body on the way to the program's actual start;
    // patched once we know where "the actual start" (the Call to main) is.
    std::size_t skipJump = emit(OpCode::Jump, 0, 0);

    // --- pass 2: compile each func's body, now that entryAddress can be recorded ---
    for (std::size_t i = 0; i < allFunctions.size(); ++i) {
        const FunctionDecl* fn = allFunctions[i];
        chunk_.functions[i].entryAddress = chunk_.code.size();
        currentClassName_ = fn->ownerClassName;
        auto parentIt = classParents_.find(currentClassName_);
        currentParentClassName_ = (parentIt != classParents_.end()) ? parentIt->second : std::string();
        activeOwnedLocalNames_.clear();
        for (const auto& param : fn->params) {
            if (param.ownership == OwnershipKind::OWNED &&
                std::find(activeOwnedLocalNames_.begin(), activeOwnedLocalNames_.end(), param.storageName) == activeOwnedLocalNames_.end()) {
                activeOwnedLocalNames_.push_back(param.storageName);
            }
        }

        if (fn->isConstructor && !currentParentClassName_.empty()) {
            // Java-style: a constructor of a class that `extends` something
            // implicitly calls its parent's no-arg constructor first, UNLESS
            // it already calls `super(...)` explicitly as its own first
            // statement (TypeChecker::checkFunctionDecl validated that if a
            // super(...) call exists anywhere, it's exactly there).
            const auto* block = static_cast<const BlockStmt*>(fn->body.get());
            bool hasExplicitSuperCall =
                !block->statements.empty() && block->statements[0]->kind == NodeKind::ExprStmt &&
                static_cast<const ExprStmt*>(block->statements[0].get())->expression &&
                static_cast<const ExprStmt*>(block->statements[0].get())->expression->kind ==
                    NodeKind::SuperCallExpr;

            if (!hasExplicitSuperCall) {
                std::size_t thisIdx = chunk_.addName("this");
                emit(OpCode::LoadVar, thisIdx, fn->line);
                // Always targets the parent's NO-ARG constructor overload
                // (an implicit super() call passes zero arguments, by
                // definition) - TypeChecker::checkFunctionDecl already
                // validated one exists before letting this class compile.
                const DispatchSignature ctorSignature{currentParentClassName_, {}};
                emit(OpCode::InvokeSuper, resolveFunctionIndex(currentParentClassName_, ctorSignature), fn->line, 0);
                emit(OpCode::Pop, 0, fn->line); // discard the (nil) return value
            }
        }

        // Instance field initializers run before the constructor body, always -
        // including when the class declares a constructor of its own, which is
        // the only way they are distinguishable from a default the allocator
        // already produced. SetField is an assignment form ((object, value) ->
        // (value)), so the value it leaves behind is popped straight back off.
        if (fn->isConstructor) {
            for (const auto& decl : program.declarations) {
                if (decl->kind != NodeKind::ClassDecl) continue;
                const auto* cls = static_cast<const ClassDecl*>(decl.get());
                if (cls->name != fn->ownerClassName) continue;
                for (const auto& member : cls->members) {
                    if (member->kind != NodeKind::VarDecl) continue;
                    const auto* field = static_cast<const VarDecl*>(member.get());
                    if (field->isStatic) continue;
                    // A field declared without an initializer is left alone: the
                    // allocator has already installed its type's zero, and
                    // writing nil into a field with a concrete declared type
                    // would assert a lie.
                    if (!field->initializer) continue;
                    emit(OpCode::LoadVar, chunk_.addName("this"), field->line);
                    compileExpression(field->initializer.get());
                    emit(OpCode::SetField, chunk_.addName(field->name), field->line);
                    emit(OpCode::Pop, 0, field->line);
                }
                break;
            }
        }

        compileStatement(fn->body.get());
        chunk_.functions[i].ownedLocalNames = activeOwnedLocalNames_;

        // Implicit `return` (as nil) if the func falls off the end of its body.
        // A body that cannot fall through already returned (or threw), and that
        // return emitted the same owned local cleanup, so the tail would be
        // unreachable bytes.
        if (canFallThrough(fn->body.get())) {
            std::size_t nilIdx = chunk_.addConstant(Value{});
            emit(OpCode::PushConst, nilIdx, fn->line);
            emitOwnedLocalCleanup(fn->line);
            emit(OpCode::Return, 0, fn->line);
        }
    }

    // Compile static initializers after ordinary functions so they may call any
    // declared static/instance methods. Their bodies are lazy and never execute here.
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        for (const auto& member : cls->members) {
            if (member->kind != NodeKind::VarDecl) continue;
            const auto* field = static_cast<const VarDecl*>(member.get());
            if (!field->isStatic) continue;
            auto metaIt = chunk_.staticFields.find(cls->name + "." + field->name);
            if (metaIt == chunk_.staticFields.end()) throw std::runtime_error("Compiler: missing static field metadata");
            const std::size_t functionIndex = metaIt->second.initializerFunction;
            if (functionIndex >= chunk_.functions.size()) throw std::runtime_error("Compiler: invalid static initializer index");
            chunk_.functions[functionIndex].entryAddress = chunk_.code.size();
            currentClassName_ = cls->name;
            currentParentClassName_.clear();
            if (field->initializer) compileExpression(field->initializer.get());
            else emit(OpCode::PushConst, chunk_.addConstant(Value{}), field->line);
            emit(OpCode::Return, 0, field->line);
        }
    }

    patchJump(skipJump);

    // Program entry: optionally feed main(args: list<string>) real CLI args,
    // call main(), discard its (void) return value, stop.
    if (mainFn->params.size() > 1) {
        throw std::runtime_error(
            "Compiler: main() can take at most one parameter (args: list<string>), got " +
            std::to_string(mainFn->params.size()));
    }
    if (mainFn->params.size() == 1) {
        emit(OpCode::PushProgramArgs, 0, mainFn->line);
    }
    auto mainIt = std::find(allFunctions.begin(), allFunctions.end(), mainFn);
    std::size_t mainIndex = static_cast<std::size_t>(mainIt - allFunctions.begin());
    emit(OpCode::Call, mainIndex, mainFn->line);
    emit(OpCode::Pop, 0, mainFn->line);
    emit(OpCode::Halt);

    return std::move(chunk_);
}

// ---------- statements ----------

void Compiler::compileStatement(const AstNode* node) {
    switch (node->kind) {
        case NodeKind::VarDecl:   compileVarDecl(static_cast<const VarDecl*>(node)); return;
        case NodeKind::LogStmt:   compileLogStmt(static_cast<const LogStmt*>(node)); return;
        case NodeKind::LogExpr: {
            const auto* n = static_cast<const LogExpr*>(node);
            compileExpression(n->argument.get());
            emit(OpCode::Log, 0, n->line);
            const std::size_t nilIdx = chunk_.addConstant(Value{});
            emit(OpCode::PushConst, nilIdx, n->line);
            return;
        }
        case NodeKind::IfStmt:    compileIfStmt(static_cast<const IfStmt*>(node)); return;
        case NodeKind::ReturnStmt: compileReturnStmt(static_cast<const ReturnStmt*>(node)); return;
        case NodeKind::ForStmt:    compileForStmt(static_cast<const ForStmt*>(node)); return;
        case NodeKind::WhileStmt:  compileWhileStmt(static_cast<const WhileStmt*>(node)); return;
        case NodeKind::RepeatStmt: compileRepeatStmt(static_cast<const RepeatStmt*>(node)); return;
        case NodeKind::BreakStmt:  compileBreakStmt(static_cast<const BreakStmt*>(node)); return;
        case NodeKind::ContinueStmt: compileContinueStmt(static_cast<const ContinueStmt*>(node)); return;
        case NodeKind::TryStmt:    compileTryStmt(static_cast<const TryStmt*>(node)); return;
        case NodeKind::ThrowStmt:  compileThrowStmt(static_cast<const ThrowStmt*>(node)); return;
        case NodeKind::BlockStmt: compileBlock(static_cast<const BlockStmt*>(node)); return;
        case NodeKind::ExprStmt:  compileExprStmt(static_cast<const ExprStmt*>(node)); return;
        default:
            throw std::runtime_error("Compiler: node is not a statement");
    }
}

void Compiler::compileBlock(const BlockStmt* node) {
    for (const auto& stmt : node->statements) compileStatement(stmt.get());
}

void Compiler::compileVarDecl(const VarDecl* node) {
    // Explicitly typed locals are dynamic boundaries too. The VM resolves
    // generic annotations using the current invocation's lexical type bindings.
    if (node->initializer) {
        compileExpression(node->initializer.get());
        if (!node->assertedTypeName.empty() && node->assertedTypeName != "unknown") {
            emit(OpCode::AssertType, chunk_.addName(node->assertedTypeName), node->line);
        }
    } else {
        std::size_t nilIdx = chunk_.addConstant(Value{});
        emit(OpCode::PushConst, nilIdx, node->line);
    }
    std::size_t nameIdx = chunk_.addName(node->storageName);
    emit(OpCode::DefineVar, nameIdx, node->line);
    if (node->ownership == OwnershipKind::OWNED &&
        std::find(activeOwnedLocalNames_.begin(), activeOwnedLocalNames_.end(), node->storageName) == activeOwnedLocalNames_.end()) {
        activeOwnedLocalNames_.push_back(node->storageName);
    }
}

void Compiler::emitOwnedLocalCleanup(std::size_t line) {
    for (auto it = activeOwnedLocalNames_.rbegin(); it != activeOwnedLocalNames_.rend(); ++it) {
        emit(OpCode::DropVar, chunk_.addName(*it), line);
    }
}

void Compiler::compileLogStmt(const LogStmt* node) {
    compileExpression(node->argument.get());
    emit(OpCode::Log, 0, node->line);
}

void Compiler::compileExprStmt(const ExprStmt* node) {
    compileExpression(node->expression.get());
    emit(OpCode::Pop, 0, node->line);
}

void Compiler::compileReturnStmt(const ReturnStmt* node) {
    if (node->value) {
        compileExpression(node->value.get());
    } else {
        std::size_t nilIdx = chunk_.addConstant(Value{});
        emit(OpCode::PushConst, nilIdx, node->line);
    }
    // Return performs lexical cleanup before leaving the function. Finalizers
    // are emitted inner-to-outer. compileActiveFinallyCleanup temporarily
    // removes the finalizer currently being emitted from the active stack,
    // so a return/break/continue inside that finalizer cannot recursively
    // emit itself forever.
    for (std::size_t i = activeFinallyBlocks_.size(); i > 0; --i) {
        compileActiveFinallyCleanup(i - 1);
    }
    emitOwnedLocalCleanup(node->line);
    emit(OpCode::Return, 0, node->line);
}

void Compiler::compileIfStmt(const IfStmt* node) {
    std::vector<std::size_t> jumpsToEnd;

    for (const auto& branch : node->branches) {
        compileExpression(branch.condition.get());
        std::size_t jumpToNextBranch = emit(OpCode::JumpIfFalse, 0, node->line);

        compileStatement(branch.body.get());

        jumpsToEnd.push_back(emit(OpCode::Jump, 0, node->line));
        patchJump(jumpToNextBranch);
    }

    if (node->elseBody) {
        compileStatement(node->elseBody.get());
    }

    for (std::size_t idx : jumpsToEnd) patchJump(idx);
}

// for i in start..end step s { body }
//
//     [compile start]              ; push start
//     DefineVar i
//     [compile end]                ; push end
//     DefineVar __for_end_N
//     [compile step]               ; push step
//     DefineVar __for_step_N
// loopStart:
//     LoadVar i ; LoadVar __for_end_N ; LoadVar __for_step_N
//     RangeContinue
//     JumpIfFalse -> loopEnd
//     [compile body]
// incrementLabel:                  <- continue jumps land here
//     LoadVar i ; LoadVar __for_step_N ; Add
//     DefineVar i
//     Jump -> loopStart
// loopEnd:                         <- break jumps land here
void Compiler::compileForStmt(const ForStmt* node) {
    std::size_t id = loopCounter_++;
    std::string endName = "__for_end_" + std::to_string(id);
    std::string stepName = "__for_step_" + std::to_string(id);

    compileExpression(node->start.get());
    emit(OpCode::DefineVar, chunk_.addName(node->storageName), node->line);
    compileExpression(node->end.get());
    emit(OpCode::DefineVar, chunk_.addName(endName), node->line);
    compileExpression(node->step.get());
    emit(OpCode::DefineVar, chunk_.addName(stepName), node->line);

    std::size_t loopStart = chunk_.code.size();
    emit(OpCode::LoadVar, chunk_.addName(node->storageName), node->line);
    emit(OpCode::LoadVar, chunk_.addName(endName), node->line);
    emit(OpCode::LoadVar, chunk_.addName(stepName), node->line);
    emit(OpCode::RangeContinue, 0, node->line);
    std::size_t exitJump = emit(OpCode::JumpIfFalse, 0, node->line);

    loopStack_.push_back(LoopContext{});
    loopStack_.back().handlerDepth = activeHandlerDepth_;
    loopStack_.back().finallyDepth = activeFinallyBlocks_.size();
    compileStatement(node->body.get());

    std::size_t incrementLabel = chunk_.code.size();
    emit(OpCode::LoadVar, chunk_.addName(node->storageName), node->line);
    emit(OpCode::LoadVar, chunk_.addName(stepName), node->line);
    emit(OpCode::Add, 0, node->line);
    emit(OpCode::DefineVar, chunk_.addName(node->storageName), node->line);
    emit(OpCode::Jump, loopStart, node->line);

    std::size_t loopEnd = chunk_.code.size();
    patchJump(exitJump);

    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();
    for (std::size_t idx : ctx.breakJumps) chunk_.code[idx].operand = loopEnd;
    for (std::size_t idx : ctx.continueJumps) chunk_.code[idx].operand = incrementLabel;
}

// while cond { body }
void Compiler::compileWhileStmt(const WhileStmt* node) {
    std::size_t loopStart = chunk_.code.size();
    compileExpression(node->condition.get());
    std::size_t exitJump = emit(OpCode::JumpIfFalse, 0, node->line);

    loopStack_.push_back(LoopContext{});
    loopStack_.back().handlerDepth = activeHandlerDepth_;
    loopStack_.back().finallyDepth = activeFinallyBlocks_.size();
    compileStatement(node->body.get());
    emit(OpCode::Jump, loopStart, node->line);

    std::size_t loopEnd = chunk_.code.size();
    patchJump(exitJump);

    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();
    for (std::size_t idx : ctx.breakJumps) chunk_.code[idx].operand = loopEnd;
    // 'continue' re-checks the condition, same as jumping straight back to loopStart.
    for (std::size_t idx : ctx.continueJumps) chunk_.code[idx].operand = loopStart;
}

// repeat { body } while cond  - body always runs at least once
void Compiler::compileRepeatStmt(const RepeatStmt* node) {
    std::size_t bodyStart = chunk_.code.size();

    loopStack_.push_back(LoopContext{});
    loopStack_.back().handlerDepth = activeHandlerDepth_;
    loopStack_.back().finallyDepth = activeFinallyBlocks_.size();
    compileStatement(node->body.get());

    std::size_t conditionCheck = chunk_.code.size();
    compileExpression(node->condition.get());
    // JumpIfFalse falls through (exits) when false; when true it needs to jump
    // BACK to bodyStart - JumpIfFalse alone can't express "jump on true", so
    // invert with Not first and reuse the same opcode for "jump if NOT false".
    emit(OpCode::Not, 0, node->line);
    emit(OpCode::JumpIfFalse, bodyStart, node->line);

    std::size_t loopEnd = chunk_.code.size();

    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();
    for (std::size_t idx : ctx.breakJumps) chunk_.code[idx].operand = loopEnd;
    // 'continue' skips the rest of the body and goes straight to the condition check.
    for (std::size_t idx : ctx.continueJumps) chunk_.code[idx].operand = conditionCheck;
}

void Compiler::compileBreakStmt(const BreakStmt* node) {
    if (loopStack_.empty()) throw std::runtime_error("Compiler: 'break' used outside of a loop");
    const LoopContext& loop = loopStack_.back();
    // A break exits the current lexical region but lands after the loop. Run
    // finally blocks introduced inside that loop before removing their handlers.
    for (std::size_t i = activeFinallyBlocks_.size(); i > loop.finallyDepth; --i) {
        compileActiveFinallyCleanup(i - 1);
    }
    const std::size_t cleanupCount = activeHandlerDepth_ - loop.handlerDepth;
    for (std::size_t i = 0; i < cleanupCount; ++i) emit(OpCode::PopHandler, 0, node->line);
    loopStack_.back().breakJumps.push_back(emit(OpCode::Jump, 0, node->line));
}

void Compiler::compileContinueStmt(const ContinueStmt* node) {
    if (loopStack_.empty()) throw std::runtime_error("Compiler: 'continue' used outside of a loop");
    const LoopContext& loop = loopStack_.back();
    // Continue skips the remainder of the current iteration, so all finally
    // blocks introduced after loop entry must run before jumping to the loop's
    // continue target.
    for (std::size_t i = activeFinallyBlocks_.size(); i > loop.finallyDepth; --i) {
        compileActiveFinallyCleanup(i - 1);
    }
    const std::size_t cleanupCount = activeHandlerDepth_ - loop.handlerDepth;
    for (std::size_t i = 0; i < cleanupCount; ++i) emit(OpCode::PopHandler, 0, node->line);
    loopStack_.back().continueJumps.push_back(emit(OpCode::Jump, 0, node->line));
}

void Compiler::compileActiveFinallyCleanup(std::size_t firstIndex) {
    if (firstIndex >= activeFinallyBlocks_.size()) {
        throw std::logic_error("Compiler: invalid finally cleanup index");
    }
    const auto block = activeFinallyBlocks_[firstIndex];
    const auto saved = std::move(activeFinallyBlocks_);
    activeFinallyBlocks_.assign(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(firstIndex));
    compileBlock(block);
    activeFinallyBlocks_ = saved;
}

// try { tryBlock } catch [ExceptionType] e { catchBlock } ... finally { finallyBlock }
void Compiler::compileTryStmt(const TryStmt* node) {
    const bool hasCatch = !node->catches.empty();
    const bool hasFinally = node->finallyBlock != nullptr;

    std::size_t finallyHandler = 0;
    if (hasFinally) {
        finallyHandler = emit(OpCode::PushFinallyHandler, 0, node->line);
        ++activeHandlerDepth_;
    }

    const std::size_t catchGroup = hasCatch ? nextHandlerGroupId_++ : 0;
    std::vector<std::size_t> catchHandlers;
    catchHandlers.reserve(node->catches.size());
    // Push the catches in reverse order so the first (most-specific) clause
    // is at the top of the runtime handler stack.
    for (auto it = node->catches.rbegin(); it != node->catches.rend(); ++it) {
        std::size_t catchTypeNameIdx = 0;
        if (it->type) catchTypeNameIdx = chunk_.addName(it->type->name) + 1;
        catchHandlers.push_back(emit(OpCode::PushHandler, 0, node->line, catchTypeNameIdx, catchGroup));
        ++activeHandlerDepth_;
    }

    if (hasFinally) activeFinallyBlocks_.push_back(static_cast<const BlockStmt*>(node->finallyBlock.get()));
    compileStatement(node->tryBlock.get());
    if (hasFinally) activeFinallyBlocks_.pop_back();

    for (std::size_t i = 0; i < node->catches.size(); ++i) {
        --activeHandlerDepth_;
        emit(OpCode::PopHandler, 0, node->line);
    }
    if (hasFinally) {
        compileStatement(node->finallyBlock.get());
        --activeHandlerDepth_;
        emit(OpCode::PopHandler, 0, node->line);
    }
    const std::size_t jumpAfterNormal = emit(OpCode::Jump, 0, node->line);

    std::vector<std::size_t> jumpAfterCatch;
    jumpAfterCatch.reserve(node->catches.size());
    for (std::size_t i = 0; i < node->catches.size(); ++i) {
        const std::size_t catchIndex = node->catches.size() - 1 - i;
        const std::size_t handlerIndex = catchHandlers[i];
        chunk_.code[handlerIndex].operand = chunk_.code.size();

        const CatchClause& clause = node->catches[catchIndex];
        emit(OpCode::DefineVar, chunk_.addName(clause.storageName), clause.line ? clause.line : node->line);
        if (hasFinally) activeFinallyBlocks_.push_back(static_cast<const BlockStmt*>(node->finallyBlock.get()));
        compileStatement(clause.block.get());
        if (hasFinally) activeFinallyBlocks_.pop_back();
        if (hasFinally) {
            compileStatement(node->finallyBlock.get());
            emit(OpCode::PopHandler, 0, node->line);
        }
        jumpAfterCatch.push_back(emit(OpCode::Jump, 0, node->line));
    }

    if (hasFinally) {
        chunk_.code[finallyHandler].operand = chunk_.code.size();
        compileStatement(node->finallyBlock.get());
        emit(OpCode::Throw, 0, node->line);
    }

    patchJump(jumpAfterNormal);
    for (std::size_t jump : jumpAfterCatch) patchJump(jump);
}

void Compiler::compileThrowStmt(const ThrowStmt* node) {
    compileExpression(node->value.get());
    emit(OpCode::Throw, 0, node->line);
}

// ---------- expressions ----------

void Compiler::compileExpression(const AstNode* node) {
    switch (node->kind) {
        case NodeKind::Literal:    compileLiteral(static_cast<const Literal*>(node)); return;
        case NodeKind::Identifier: compileIdentifier(static_cast<const Identifier*>(node)); return;
        case NodeKind::UnaryExpr:  compileUnary(static_cast<const UnaryExpr*>(node)); return;
        case NodeKind::AwaitExpr:   compileAwait(static_cast<const AwaitExpr*>(node)); return;
        case NodeKind::BinaryExpr: compileBinary(static_cast<const BinaryExpr*>(node)); return;
        case NodeKind::CallExpr:   compileCall(static_cast<const CallExpr*>(node)); return;
        case NodeKind::AssignExpr: compileAssign(static_cast<const AssignExpr*>(node)); return;
        case NodeKind::MoveExpr: compileMove(static_cast<const MoveExpr*>(node)); return;
        case NodeKind::CollectionLiteral: compileCollectionLiteral(static_cast<const CollectionLiteral*>(node)); return;
        case NodeKind::NewExpr: compileNewExpr(static_cast<const NewExpr*>(node)); return;
        case NodeKind::DataLiteralExpr: compileDataLiteralExpr(static_cast<const DataLiteralExpr*>(node)); return;
        case NodeKind::DataUpdateExpr: {
            const auto* n = static_cast<const DataUpdateExpr*>(node);
            compileExpression(n->base.get());
            emit(OpCode::CopyObject, 0, n->line);
            for (const auto& [fieldName, valueExpr] : n->fields) {
                emit(OpCode::Dup, 0, n->line);
                compileExpression(valueExpr.get());
                emit(OpCode::SetField, chunk_.addName(fieldName), n->line);
                emit(OpCode::Pop, 0, n->line);
            }
            return;
        }
        case NodeKind::FieldAccessExpr: compileFieldAccess(static_cast<const FieldAccessExpr*>(node)); return;
        case NodeKind::IndexAccessExpr: compileIndexAccess(static_cast<const IndexAccessExpr*>(node)); return;
        case NodeKind::FieldAssignExpr: compileFieldAssign(static_cast<const FieldAssignExpr*>(node)); return;
        case NodeKind::MethodCallExpr: compileMethodCall(static_cast<const MethodCallExpr*>(node)); return;
        case NodeKind::ThisExpr: compileThisExpr(static_cast<const ThisExpr*>(node)); return;
        case NodeKind::SuperCallExpr: compileSuperCallExpr(static_cast<const SuperCallExpr*>(node)); return;
        case NodeKind::SuperMethodCallExpr: compileSuperMethodCallExpr(static_cast<const SuperMethodCallExpr*>(node)); return;
        case NodeKind::LambdaExpr: compileLambdaExpr(static_cast<const LambdaExpr*>(node)); return;
        case NodeKind::MatchExpr: compileMatchExpr(static_cast<const MatchExpr*>(node)); return;
        case NodeKind::LogExpr: {
            const auto* n = static_cast<const LogExpr*>(node);
            compileExpression(n->argument.get());
            emit(OpCode::Log, 0, n->line);
            const std::size_t nilIdx = chunk_.addConstant(Value{});
            emit(OpCode::PushConst, nilIdx, n->line);
            return;
        }
        default:
            throw std::runtime_error("Compiler: node is not an expression");
    }
}

void Compiler::compileLiteral(const Literal* node) {
    Value value;
    switch (node->literalType) {
        case TokenType::INT_LITERAL: {
            try {
                std::size_t pos = 0;
                const auto parsed = std::stoll(node->raw, &pos);
                if (pos != node->raw.size()) throw std::invalid_argument("trailing characters");
                value = static_cast<std::int64_t>(parsed);
            } catch (const std::exception&) {
                throw std::runtime_error("Compiler: invalid integer literal at line " + std::to_string(node->line));
            }
            break;
        }
        case TokenType::DECIMAL_LITERAL:
        case TokenType::FLOAT_LITERAL: {
            try {
                std::size_t pos = 0;
                const double parsed = std::stod(node->raw, &pos);
                if (pos != node->raw.size() || !std::isfinite(parsed)) throw std::invalid_argument("invalid floating-point literal");
                value = parsed;
            } catch (const std::exception&) {
                throw std::runtime_error("Compiler: invalid floating-point literal at line " + std::to_string(node->line));
            }
            break;
        }
        case TokenType::STRING_LITERAL:
            value = node->raw;
            break;
        case TokenType::BOOL_LITERAL:
            value = (node->raw == "true");
            break;
        case TokenType::KW_NULL:
            value = Value{};
            break;
        default:
            throw std::runtime_error("Compiler: unknown literal type");
    }
    std::size_t idx = chunk_.addConstant(value);
    emit(OpCode::PushConst, idx, node->line);
}

void Compiler::compileIdentifier(const Identifier* node) {
    std::size_t idx = chunk_.addName(node->storageName);
    emit(OpCode::LoadVar, idx, node->line);
}

void Compiler::compileAwait(const AwaitExpr* node) {
    compileExpression(node->operand.get());
    emit(OpCode::Await, 0, node->line);
}

void Compiler::compileUnary(const UnaryExpr* node) {
    if (node->isOperatorOverload) {
        compileExpression(node->operand.get());
        std::string symbol;
        switch (node->op) {
            case TokenType::PLUS: return (void)(emit(OpCode::InvokeMethod, methodSlot(node->resolvedOperatorDispatch), node->line, 0));
            case TokenType::MINUS: return (void)(emit(OpCode::InvokeMethod, methodSlot(node->resolvedOperatorDispatch), node->line, 0));
            case TokenType::NOT: return (void)(emit(OpCode::InvokeMethod, methodSlot(node->resolvedOperatorDispatch), node->line, 0));
            case TokenType::BIT_NOT: return (void)(emit(OpCode::InvokeMethod, methodSlot(node->resolvedOperatorDispatch), node->line, 0));
            default: throw std::runtime_error("Compiler: unknown overloaded unary operator");
        }
    }
    compileExpression(node->operand.get());
    switch (node->op) {
        case TokenType::MINUS:   emit(OpCode::Neg, 0, node->line); break;
        case TokenType::NOT:     emit(OpCode::Not, 0, node->line); break;
        case TokenType::BIT_NOT: emit(OpCode::BitNot, 0, node->line); break;
        default:
            throw std::runtime_error("Compiler: unknown unary operator");
    }
}

namespace {

std::optional<Value> tryFoldBinaryLiterals(const BinaryExpr* node) {
    if (!node || !node->left || !node->right ||
        node->left->kind != NodeKind::Literal || node->right->kind != NodeKind::Literal ||
        node->isOperatorOverload) {
        return std::nullopt;
    }

    const auto* left = static_cast<const Literal*>(node->left.get());
    const auto* right = static_cast<const Literal*>(node->right.get());

    auto isInt = [](const Literal* lit) { return lit->literalType == TokenType::INT_LITERAL; };
    auto isNumber = [](const Literal* lit) {
        return lit->literalType == TokenType::INT_LITERAL ||
               lit->literalType == TokenType::DECIMAL_LITERAL ||
               lit->literalType == TokenType::FLOAT_LITERAL;
    };
    auto intValue = [](const Literal* lit) -> std::optional<std::int64_t> {
        try {
            std::size_t pos = 0;
            const auto value = std::stoll(lit->raw, &pos);
            if (pos != lit->raw.size()) return std::nullopt;
            return static_cast<std::int64_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    };
    auto doubleValue = [](const Literal* lit) -> std::optional<double> {
        try {
            std::size_t pos = 0;
            const double value = std::stod(lit->raw, &pos);
            if (pos != lit->raw.size() || !std::isfinite(value)) return std::nullopt;
            return value;
        } catch (...) {
            return std::nullopt;
        }
    };

    if (isNumber(left) && isNumber(right)) {
        const bool bothInt = isInt(left) && isInt(right);
        const auto intLeft = bothInt ? intValue(left) : std::optional<std::int64_t>{};
        const auto intRight = bothInt ? intValue(right) : std::optional<std::int64_t>{};
        if (bothInt && (!intLeft || !intRight)) return std::nullopt;
        const std::int64_t x = bothInt ? *intLeft : 0;
        const std::int64_t y = bothInt ? *intRight : 0;
        if (bothInt) {
            std::int64_t result = 0;
            switch (node->op) {
                case TokenType::PLUS:
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_add_overflow(x, y, &result)) return Value{result};
#else
                    if (!((y > 0 && x > std::numeric_limits<std::int64_t>::max() - y) ||
                          (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y))) return Value{x + y};
#endif
                    break;
                case TokenType::MINUS:
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_sub_overflow(x, y, &result)) return Value{result};
#else
                    if (!((y < 0 && x > std::numeric_limits<std::int64_t>::max() + y) ||
                          (y > 0 && x < std::numeric_limits<std::int64_t>::min() + y))) return Value{x - y};
#endif
                    break;
                case TokenType::STAR:
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_mul_overflow(x, y, &result)) return Value{result};
#else
                    return std::nullopt;
#endif
                    break;
                case TokenType::SLASH:
                    if (y != 0 && !(x == std::numeric_limits<std::int64_t>::min() && y == -1)) return Value{x / y};
                    break;
                case TokenType::PERCENT:
                    if (y != 0) {
                        // The VM defines the one case C++ leaves undefined.
                        if (x == std::numeric_limits<std::int64_t>::min() && y == -1) return Value{std::int64_t{0}};
                        return Value{x % y};
                    }
                    break;
                case TokenType::BIT_AND: return Value{x & y};
                case TokenType::BIT_OR:  return Value{x | y};
                case TokenType::BIT_XOR: return Value{x ^ y};
                case TokenType::SHL:
                    if (y >= 0 && y < 64) return Value{static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << static_cast<unsigned>(y))};
                    break;
                case TokenType::SHR:
                    if (y >= 0 && y < 64) {
                        const auto shift = static_cast<unsigned>(y);
                        std::uint64_t shifted = static_cast<std::uint64_t>(x) >> shift;
                        if (x < 0 && shift != 0) shifted |= (~std::uint64_t{0}) << (64 - shift);
                        return Value{static_cast<std::int64_t>(shifted)};
                    }
                    break;
                case TokenType::USHR:
                    if (y >= 0 && y < 64) return Value{static_cast<std::int64_t>(static_cast<std::uint64_t>(x) >> static_cast<unsigned>(y))};
                    break;
                case TokenType::LT: return Value{x < y};
                case TokenType::GT: return Value{x > y};
                case TokenType::LTE: return Value{x <= y};
                case TokenType::GTE: return Value{x >= y};
                case TokenType::EQ: return Value{x == y};
                case TokenType::NEQ: return Value{x != y};
                default: break;
            }
            // An int/int pair that failed int folding above (overflow,
            // MIN/-1) must raise at runtime. It must NOT fall through to
            // double folding below: stod parses int spellings fine, which
            // would silently fold `MAX + 1` into a double constant -
            // changing both the static type and the value, and swallowing
            // the overflow error the runtime would have raised.
            return std::nullopt;
        }

        const auto leftDouble = doubleValue(left);
        const auto rightDouble = doubleValue(right);
        if (!leftDouble || !rightDouble) return std::nullopt;
        const double a = *leftDouble;
        const double b = *rightDouble;
        switch (node->op) {
            // A non-finite double result raises at runtime, so it stays
            // unfolded (like an overflowing int fold above): folding it
            // would bake a silent inf into the chunk instead of raising.
            case TokenType::PLUS: if (std::isfinite(a + b)) return Value{a + b}; break;
            case TokenType::MINUS: if (std::isfinite(a - b)) return Value{a - b}; break;
            case TokenType::STAR: if (std::isfinite(a * b)) return Value{a * b}; break;
            case TokenType::SLASH: if (b != 0.0 && std::isfinite(a / b)) return Value{a / b}; break;
            case TokenType::PERCENT: if (b != 0.0) return Value{std::fmod(a, b)}; break;
            case TokenType::LT: return Value{a < b};
            case TokenType::GT: return Value{a > b};
            case TokenType::LTE: return Value{a <= b};
            case TokenType::GTE: return Value{a >= b};
            case TokenType::EQ: return Value{a == b};
            case TokenType::NEQ: return Value{a != b};
            default: break;
        }
    }

    if (left->literalType == TokenType::BOOL_LITERAL && right->literalType == TokenType::BOOL_LITERAL) {
        const bool a = left->raw == "true";
        const bool b = right->raw == "true";
        if (node->op == TokenType::EQ) return Value{a == b};
        if (node->op == TokenType::NEQ) return Value{a != b};
        if (node->op == TokenType::AND) return Value{a && b};
        if (node->op == TokenType::OR) return Value{a || b};
    }

    return std::nullopt;
}

} // namespace

void Compiler::compileBinary(const BinaryExpr* node) {
    if (const auto folded = tryFoldBinaryLiterals(node)) {
        emit(OpCode::PushConst, chunk_.addConstant(*folded), node->line);
        return;
    }

    if (node->isOperatorOverload) {
        compileExpression(node->left.get());
        compileExpression(node->right.get());
        std::string method;
        switch (node->op) {
            case TokenType::PLUS: method = "operator+"; break;
            case TokenType::MINUS: method = "operator-"; break;
            case TokenType::STAR: method = "operator*"; break;
            case TokenType::SLASH: method = "operator/"; break;
            case TokenType::PERCENT: method = "operator%"; break;
            case TokenType::POW: method = "operator^^"; break;
            case TokenType::BIT_AND: method = "operator&"; break;
            case TokenType::BIT_OR: method = "operator|"; break;
            case TokenType::BIT_XOR: method = "operator^"; break;
            case TokenType::SHL: method = "operator<<"; break;
            case TokenType::SHR: method = "operator>>"; break;
            case TokenType::USHR: method = "operator>>>"; break;
            case TokenType::EQ: method = "operator=="; break;
            case TokenType::NEQ: method = "operator!="; break;
            case TokenType::LT: method = "operator<"; break;
            case TokenType::GT: method = "operator>"; break;
            case TokenType::LTE: method = "operator<="; break;
            case TokenType::GTE: method = "operator>="; break;
            default: throw std::runtime_error("Compiler: unknown overloaded binary operator");
        }
        emit(OpCode::InvokeMethod, methodSlot(node->resolvedOperatorDispatch), node->line, 1);
        return;
    }

    // && and || must short-circuit (the right operand may have side effects,
    // e.g. func calls, or may be unsafe to evaluate at all, e.g. a
    // divide-by-zero guarded by a preceding `x != 0 &&` check), so they're
    // compiled with jumps instead of going through the generic eager path
    // below that evaluates both operands unconditionally.
    if (node->op == TokenType::AND) {
        compileExpression(node->left.get());
        // If the left side is falsy, short-circuit: skip the right operand
        // entirely and produce `false` as the result.
        std::size_t elseJump = emit(OpCode::JumpIfFalse, 0, node->line);
        compileExpression(node->right.get());
        // Normalize the right operand to a strict bool (double-NOT), the
        // same way binaryLogical used to for its result.
        emit(OpCode::Not, 0, node->line);
        emit(OpCode::Not, 0, node->line);
        std::size_t endJump = emit(OpCode::Jump, 0, node->line);
        patchJump(elseJump);
        emit(OpCode::PushConst, chunk_.addConstant(Value{false}), node->line);
        patchJump(endJump);
        return;
    }
    if (node->op == TokenType::OR) {
        compileExpression(node->left.get());
        // Not, Not turns "left was truthy" into a falsy value so a single
        // JumpIfFalse can express "jump when left was truthy" (there's no
        // dedicated JumpIfTrue opcode).
        emit(OpCode::Not, 0, node->line);
        std::size_t jumpToTrue = emit(OpCode::JumpIfFalse, 0, node->line);
        // Left was falsy: fall through and evaluate the right operand.
        compileExpression(node->right.get());
        emit(OpCode::Not, 0, node->line);
        emit(OpCode::Not, 0, node->line);
        std::size_t endJump = emit(OpCode::Jump, 0, node->line);
        patchJump(jumpToTrue);
        emit(OpCode::PushConst, chunk_.addConstant(Value{true}), node->line);
        patchJump(endJump);
        return;
    }

    compileExpression(node->left.get());
    compileExpression(node->right.get());
    switch (node->op) {
        case TokenType::PLUS:    emit(OpCode::Add, 0, node->line); break;
        case TokenType::MINUS:   emit(OpCode::Sub, 0, node->line); break;
        case TokenType::STAR:    emit(OpCode::Mul, 0, node->line); break;
        case TokenType::SLASH:   emit(OpCode::Div, 0, node->line); break;
        case TokenType::PERCENT: emit(OpCode::Mod, 0, node->line); break;
        case TokenType::POW:     emit(OpCode::Pow, 0, node->line); break;
        case TokenType::BIT_AND: emit(OpCode::BitAnd, 0, node->line); break;
        case TokenType::BIT_OR:  emit(OpCode::BitOr, 0, node->line); break;
        case TokenType::BIT_XOR: emit(OpCode::BitXor, 0, node->line); break;
        case TokenType::SHL:     emit(OpCode::Shl, 0, node->line); break;
        case TokenType::SHR:     emit(OpCode::Shr, 0, node->line); break;
        case TokenType::USHR:    emit(OpCode::Ushr, 0, node->line); break;
        case TokenType::EQ:      emit(OpCode::Eq, 0, node->line); break;
        case TokenType::NEQ:     emit(OpCode::Neq, 0, node->line); break;
        case TokenType::LT:      emit(OpCode::Lt, 0, node->line); break;
        case TokenType::GT:      emit(OpCode::Gt, 0, node->line); break;
        case TokenType::LTE:     emit(OpCode::Lte, 0, node->line); break;
        case TokenType::GTE:     emit(OpCode::Gte, 0, node->line); break;
        default:
            throw std::runtime_error("Compiler: unknown binary operator");
    }
}

// func(params) => expr   OR   func(params) { block }
//
// A lambda body compiles exactly like a normal func's - its own
// Chunk::functions entry, its own entryAddress - EXCEPT it appears mid-
// stream inside whatever func is currently being compiled, so its
// instructions have to be jumped OVER (not fallen into) by the surrounding
// code. Same "skip jump, compile body, patch the jump" shape
// Compiler::compile itself uses for every top-level func body, just
// done once per lambda instead of once for the whole program.
void Compiler::compileMatchExpr(const MatchExpr* node) {
    // Keep the original subject in a generated local so nested patterns can
    // inspect fields without disturbing the arm-to-arm control flow.
    compileExpression(node->subject.get());
    const std::string subjectTemp = "__match_subject_" + std::to_string(chunk_.names.size());
    const std::size_t subjectName = chunk_.addName(subjectTemp);
    emit(OpCode::DefineVar, subjectName, node->line);

    std::vector<std::size_t> endJumps;
    std::size_t tempCounter = 0;
    const std::unordered_map<std::string, std::string>* bindings = nullptr;
    std::function<void(const MatchExpr::Pattern&, std::size_t, std::vector<std::size_t>&)> compilePattern =
        [&](const MatchExpr::Pattern& pattern, std::size_t sourceName, std::vector<std::size_t>& failJumps) {
            auto loadSource = [&]() { emit(OpCode::LoadVar, sourceName, pattern.line); };
            switch (pattern.kind) {
                case MatchExpr::PatternKind::Wildcard:
                case MatchExpr::PatternKind::Variable:
                    if (pattern.kind == MatchExpr::PatternKind::Variable && !pattern.bindingName.empty() && pattern.bindingName != "_") {
                        loadSource();
                        emit(OpCode::DefineVar, chunk_.addName(bindings->at(pattern.bindingName)), pattern.line);
                    }
                    return;
                case MatchExpr::PatternKind::Literal: {
                    loadSource();
                    Value value;
                    switch (pattern.literalType) {
                        case TokenType::INT_LITERAL: value = static_cast<std::int64_t>(std::stoll(pattern.raw)); break;
                        case TokenType::DECIMAL_LITERAL:
                        case TokenType::FLOAT_LITERAL: value = std::stod(pattern.raw); break;
                        case TokenType::STRING_LITERAL: value = pattern.raw; break;
                        case TokenType::BOOL_LITERAL: value = (pattern.raw == "true"); break;
                        case TokenType::KW_NULL: value = Value{}; break;
                        default: throw std::runtime_error("Compiler: unsupported match literal");
                    }
                    emit(OpCode::PushConst, chunk_.addConstant(value), pattern.line);
                    emit(OpCode::Eq, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    return;
                }
                case MatchExpr::PatternKind::EnumMember:
                    loadSource();
                    emit(OpCode::PushConst, chunk_.addConstant(Value{pattern.enumMemberName}), pattern.line);
                    emit(OpCode::Eq, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    return;
                case MatchExpr::PatternKind::Type:
                    loadSource();
                    emit(OpCode::PushConst, chunk_.addConstant(Value{describeTypeAnnotation(pattern.typePattern)}), pattern.line);
                    emit(OpCode::MatchType, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    if (!pattern.bindingName.empty() && pattern.bindingName != "_") {
                        loadSource();
                        emit(OpCode::DefineVar, chunk_.addName(bindings->at(pattern.bindingName)), pattern.line);
                    }
                    return;
                case MatchExpr::PatternKind::List: {
                    loadSource();
                    const auto lenIdx = findNativeFunction("Collection.length");
                    if (!lenIdx) throw std::runtime_error("Compiler: missing native func Collection.length");
                    emit(OpCode::CallNative, *lenIdx, pattern.line);
                    emit(OpCode::PushConst, chunk_.addConstant(Value{static_cast<std::int64_t>(pattern.elements.size())}), pattern.line);
                    emit(OpCode::Eq, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    if (pattern.containerKind == "set" || pattern.containerKind == "Set") {
                        const auto hasIdx = findNativeFunction("Collection.setHas");
                        if (!hasIdx) throw std::runtime_error("Compiler: missing native func Collection.setHas");
                        for (const auto& child : pattern.elements) {
                            if (child->kind == MatchExpr::PatternKind::Wildcard) continue;
                            if (child->kind == MatchExpr::PatternKind::Literal) {
                                Value value;
                                switch (child->literalType) {
                                    case TokenType::INT_LITERAL: value = static_cast<std::int64_t>(std::stoll(child->raw)); break;
                                    case TokenType::DECIMAL_LITERAL:
                                    case TokenType::FLOAT_LITERAL: value = std::stod(child->raw); break;
                                    case TokenType::STRING_LITERAL: value = child->raw; break;
                                    case TokenType::BOOL_LITERAL: value = (child->raw == "true"); break;
                                    case TokenType::KW_NULL: value = Value{}; break;
                                    default: throw std::runtime_error("Compiler: unsupported set match literal");
                                }
                                loadSource();
                                emit(OpCode::PushConst, chunk_.addConstant(value), child->line);
                                emit(OpCode::CallNative, *hasIdx, child->line);
                                failJumps.push_back(emit(OpCode::JumpIfFalse, 0, child->line));
                            } else if (child->kind == MatchExpr::PatternKind::EnumMember) {
                                loadSource();
                                emit(OpCode::PushConst, chunk_.addConstant(Value{child->enumMemberName}), child->line);
                                emit(OpCode::CallNative, *hasIdx, child->line);
                                failJumps.push_back(emit(OpCode::JumpIfFalse, 0, child->line));
                            }
                        }
                        return;
                    }
                    for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
                        const std::string itemTemp = "__match_item_" + std::to_string(tempCounter++);
                        const std::size_t itemName = chunk_.addName(itemTemp);
                        loadSource();
                        emit(OpCode::PushConst, chunk_.addConstant(Value{static_cast<std::int64_t>(i)}), pattern.line);
                        const auto getIdx = findNativeFunction("Collection.get");
                        if (!getIdx) throw std::runtime_error("Compiler: missing native func Collection.get");
                        emit(OpCode::CallNative, *getIdx, pattern.line);
                        emit(OpCode::DefineVar, itemName, pattern.line);
                        compilePattern(*pattern.elements[i], itemName, failJumps);
                    }
                    return;
                }
                case MatchExpr::PatternKind::Map: {
                    const auto hasIdx = findNativeFunction("Collection.mapHas");
                    const auto getIdx = findNativeFunction("Collection.mapGet");
                    if (!hasIdx || !getIdx) throw std::runtime_error("Compiler: missing map reflection natives");
                    loadSource();
                    const auto lenIdx = findNativeFunction("Collection.length");
                    if (!lenIdx) throw std::runtime_error("Compiler: missing native func Collection.length");
                    emit(OpCode::CallNative, *lenIdx, pattern.line);
                    emit(OpCode::PushConst, chunk_.addConstant(Value{static_cast<std::int64_t>(pattern.mapEntries.size())}), pattern.line);
                    emit(OpCode::Eq, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    for (const auto& entry : pattern.mapEntries) {
                        const auto& key = *entry.key;
                        Value keyValue;
                        if (key.kind == MatchExpr::PatternKind::Literal) {
                            switch (key.literalType) {
                                case TokenType::INT_LITERAL: keyValue = static_cast<std::int64_t>(std::stoll(key.raw)); break;
                                case TokenType::DECIMAL_LITERAL:
                                case TokenType::FLOAT_LITERAL: keyValue = std::stod(key.raw); break;
                                case TokenType::STRING_LITERAL: keyValue = key.raw; break;
                                case TokenType::BOOL_LITERAL: keyValue = (key.raw == "true"); break;
                                case TokenType::KW_NULL: keyValue = Value{}; break;
                                default: throw std::runtime_error("Compiler: unsupported map key pattern");
                            }
                        } else {
                            keyValue = Value{key.enumMemberName};
                        }
                        loadSource();
                        emit(OpCode::PushConst, chunk_.addConstant(keyValue), key.line);
                        emit(OpCode::CallNative, *hasIdx, key.line);
                        failJumps.push_back(emit(OpCode::JumpIfFalse, 0, key.line));
                        const std::string valTemp = "__match_map_value_" + std::to_string(tempCounter++);
                        const std::size_t valName = chunk_.addName(valTemp);
                        loadSource();
                        emit(OpCode::PushConst, chunk_.addConstant(keyValue), key.line);
                        emit(OpCode::CallNative, *getIdx, key.line);
                        emit(OpCode::DefineVar, valName, key.line);
                        compilePattern(*entry.value, valName, failJumps);
                    }
                    return;
                }
                case MatchExpr::PatternKind::Data: {
                    loadSource();
                    emit(OpCode::PushConst, chunk_.addConstant(Value{pattern.typePattern.name}), pattern.line);
                    emit(OpCode::MatchType, 0, pattern.line);
                    failJumps.push_back(emit(OpCode::JumpIfFalse, 0, pattern.line));
                    for (const auto& field : pattern.fields) {
                        const std::string fieldTemp = "__match_field_" + std::to_string(tempCounter++);
                        const std::size_t fieldName = chunk_.addName(fieldTemp);
                        loadSource();
                        emit(OpCode::GetField, chunk_.addName(field.fieldName), field.line);
                        emit(OpCode::DefineVar, fieldName, field.line);
                        compilePattern(*field.pattern, fieldName, failJumps);
                    }
                    return;
                }
            }
        };

    for (const auto& arm : node->arms) {
        bindings = &arm.storageBindings;
        std::function<std::unique_ptr<MatchExpr::Pattern>(const MatchExpr::Pattern&)> clonePattern = [&](const MatchExpr::Pattern& src) {
            auto out = std::make_unique<MatchExpr::Pattern>();
            out->kind = src.kind; out->raw = src.raw; out->literalType = src.literalType;
            out->enumTypeName = src.enumTypeName; out->enumMemberName = src.enumMemberName;
            out->typePattern = src.typePattern; out->bindingName = src.bindingName;
            out->containerKind = src.containerKind; out->positional = src.positional; out->line = src.line;
            for (const auto& child : src.elements) out->elements.push_back(child ? clonePattern(*child) : nullptr);
            for (const auto& entry : src.mapEntries) {
                MatchExpr::MapEntryPattern copied; copied.line = entry.line;
                copied.key = entry.key ? clonePattern(*entry.key) : nullptr;
                copied.value = entry.value ? clonePattern(*entry.value) : nullptr;
                out->mapEntries.push_back(std::move(copied));
            }
            for (const auto& child : src.fields) {
                MatchExpr::DataFieldPattern copied; copied.fieldName = child.fieldName; copied.line = child.line;
                copied.pattern = child.pattern ? clonePattern(*child.pattern) : nullptr;
                out->fields.push_back(std::move(copied));
            }
            return out;
        };

        MatchExpr::Pattern p;
        p.kind = arm.patternKind; p.raw = arm.raw; p.literalType = arm.literalType;
        p.enumTypeName = arm.enumTypeName; p.enumMemberName = arm.enumMemberName;
        p.typePattern = arm.typePattern; p.bindingName = arm.bindingName; p.positional = arm.positional;
        p.containerKind = arm.containerKind; p.line = arm.line;
        for (const auto& child : arm.listElements) p.elements.push_back(child ? clonePattern(*child) : nullptr);
        for (const auto& entry : arm.mapEntries) {
            MatchExpr::MapEntryPattern copied; copied.line = entry.line;
            copied.key = entry.key ? clonePattern(*entry.key) : nullptr;
            copied.value = entry.value ? clonePattern(*entry.value) : nullptr;
            p.mapEntries.push_back(std::move(copied));
        }
        std::vector<std::size_t> failJumps;
        if (arm.patternKind == MatchExpr::PatternKind::Data) {
            MatchExpr::Pattern root;
            root.kind = MatchExpr::PatternKind::Data;
            root.typePattern = arm.typePattern;
            root.positional = arm.positional;
            root.line = arm.line;
            for (const auto& f : arm.dataFields) {
                MatchExpr::DataFieldPattern copied; copied.fieldName = f.fieldName; copied.line = f.line;
                copied.pattern = f.pattern ? clonePattern(*f.pattern) : nullptr;
                root.fields.push_back(std::move(copied));
            }
            compilePattern(root, subjectName, failJumps);
        } else {
            compilePattern(p, subjectName, failJumps);
        }
        if (arm.guard) {
            compileExpression(arm.guard.get());
            const std::size_t guardFail = emit(OpCode::JumpIfFalse, 0, arm.line);
            compileExpression(arm.result.get());
            endJumps.push_back(emit(OpCode::Jump, 0, arm.line));
            patchJump(guardFail);
            for (auto j : failJumps) patchJump(j);
        } else {
            compileExpression(arm.result.get());
            endJumps.push_back(emit(OpCode::Jump, 0, arm.line));
            for (auto j : failJumps) patchJump(j);
        }
    }
    // Static coverage proves a matching arm for well-typed inputs. Do not
    // leak a subject/stack value if a dynamic or malformed value evades it.
    emit(OpCode::NewObject, chunk_.addName("Exception"), node->line);
    emit(OpCode::Dup, 0, node->line);
    emit(OpCode::PushConst, chunk_.addConstant(std::string("non-exhaustive match")), node->line);
    emit(OpCode::InvokeMethod, methodSlot(DispatchSignature{"Exception", {{DispatchTypeKind::STRING, {}}}}), node->line, 1);
    emit(OpCode::Pop, 0, node->line);
    emit(OpCode::Throw, 0, node->line);
    for (auto j : endJumps) patchJump(j);
}

void Compiler::compileLambdaExpr(const LambdaExpr* node) {
    std::size_t skipJump = emit(OpCode::Jump, 0, node->line);
    std::size_t entryAddress = chunk_.code.size();

    std::vector<std::string> paramNames;
    paramNames.reserve(node->params.size());
    for (const auto& p : node->params) paramNames.push_back(p.storageName);

    std::string lambdaName = "$lambda" + std::to_string(lambdaCounter_++);
    std::size_t funcIndex = chunk_.functions.size();
    std::vector<std::string> lambdaParameterTypeNames = node->inferredParameterTypeNames;
    if (lambdaParameterTypeNames.size() != node->params.size()) {
        lambdaParameterTypeNames.clear();
        for (const auto& param : node->params) {
            lambdaParameterTypeNames.push_back(param.type.name.empty() ? "unknown" : param.type.name);
        }
    }
    const std::string lambdaReturnTypeName =
        node->inferredReturnTypeName.empty() ? "unknown" : node->inferredReturnTypeName;
    FunctionInfo lambdaInfo{std::move(lambdaName), paramNames, std::move(lambdaParameterTypeNames),
                            lambdaReturnTypeName, entryAddress, false, node->isAsync, false, currentClassName_, {}, node->captureStorageNames};
    chunk_.functions.push_back(std::move(lambdaInfo));

    const auto savedOwnedLocals = activeOwnedLocalNames_;
    activeOwnedLocalNames_.clear();
    for (const auto& param : node->params) {
        if (param.ownership == OwnershipKind::OWNED) activeOwnedLocalNames_.push_back(param.storageName);
    }

    if (node->hasExprBody) {
        // `=> expr` - the expression's value IS the return value, implicitly.
        compileExpression(node->exprBody.get());
        emit(OpCode::Return, 0, node->line);
    } else {
        compileStatement(node->blockBody.get());
        // Implicit `return nil` if the block falls off the end without an
        // explicit `return` - same fallback named functions get (see
        // Compiler::compile's pass 2). A block that cannot fall through
        // (it ends in `return`/`throw`, or in an if/else whose arms all do)
        // already left the function, and its own `return` ran the same owned
        // local cleanup this tail would, so the tail is skipped rather than
        // emitted as bytes the VM can never reach.
        if (canFallThrough(node->blockBody.get())) {
            std::size_t nilIdx = chunk_.addConstant(Value{});
            emit(OpCode::PushConst, nilIdx, node->line);
            emitOwnedLocalCleanup(node->line);
            emit(OpCode::Return, 0, node->line);
        }
    }

    chunk_.functions[funcIndex].ownedLocalNames = activeOwnedLocalNames_;
    activeOwnedLocalNames_ = savedOwnedLocals;
    patchJump(skipJump);
    // Capture-by-value happens HERE, at MakeClosure - every time this
    // LambdaExpr is evaluated (e.g. each time through a loop), a fresh
    // snapshot of the CURRENT scope is taken, so two closures created on
    // different iterations don't share captured state.
    emit(OpCode::MakeClosure, funcIndex, node->line);
}

void Compiler::compileCall(const CallExpr* node) {
    if (node->isClassTypeLiteral) {
        const auto it = chunk_.classReflection.find(node->classTypeLiteralName);
        if (it == chunk_.classReflection.end() || !it->second.runtimeType) {
            throw std::runtime_error("Compiler: missing reflection metadata for type '" + node->classTypeLiteralName + "'");
        }
        auto typeValue = makeGCObject();
        typeValue->className = node->classTypeLiteralName;
        typeValue->runtimeType = it->second.runtimeType;
        const std::size_t constant = chunk_.addConstant(Value{ObjectRef(std::move(typeValue))});
        emit(OpCode::PushConst, constant, node->line);
        const auto idx = findNativeFunction("Type.of");
        if (!idx) throw std::runtime_error("Compiler: missing native func Type.of");
        emit(OpCode::CallNative, *idx, node->line);
        return;
    }
    if (node->namespaceName.empty() && node->calleeName == "share") {
        auto idx = findNativeFunction("share");
        if (!idx) throw std::runtime_error("Compiler: missing native func share");
        for (const auto& arg : node->arguments) compileExpression(arg.get());
        emit(OpCode::CallNative, *idx, node->line,
             node->nativeFactoryTypeName.empty() ? 0 : chunk_.addName(node->nativeFactoryTypeName) + 1);
        return;
    }
    if (!node->namespaceName.empty()) {
        auto staticIt = std::find_if(chunk_.functions.begin(), chunk_.functions.end(),
            [&](const FunctionInfo& f) {
                return f.isStatic && f.ownerClassName == node->namespaceName &&
                       f.dispatchSignature == node->resolvedDispatch;
            });
        if (staticIt != chunk_.functions.end()) {
            for (const auto& arg : node->arguments) compileExpression(arg.get());
            emit(OpCode::Call, static_cast<std::size_t>(staticIt - chunk_.functions.begin()), node->line,
                 typeArgNameOperand(chunk_, node->resolvedTypeArgNames));
            return;
        }

        std::string nativeName = node->namespaceName + "." + node->calleeName;
        auto idx = findNativeFunction(nativeName);
        if (!idx) {
            throw std::runtime_error("Compiler: unknown library func '" + nativeName + "'");
        }
        const NativeFunction& native = nativeFunctionTable()[*idx];
        if (native.arity() != node->arguments.size()) {
            throw std::runtime_error(
                "Compiler: '" + nativeName + "' expects " + std::to_string(native.arity()) +
                " argument(s), got " + std::to_string(node->arguments.size()));
        }
        for (const auto& arg : node->arguments) compileExpression(arg.get());
        emit(OpCode::CallNative, *idx, node->line,
             node->nativeFactoryTypeName.empty() ? 0 : chunk_.addName(node->nativeFactoryTypeName) + 1);
        return;
    }

    // `someVar(args)` where someVar holds a lambda-produced func value
    // (see CallExpr::isValueCall, set by TypeChecker::inferCall) - an
    // indirect call through a variable, not a same-class self-call. Args
    // push first, then the callee value on top - see OpCode::CallValue's
    // own comment for the exact stack layout it expects.
    if (node->isValueCall) {
        for (const auto& arg : node->arguments) compileExpression(arg.get());
        std::size_t nameIdx = chunk_.addName(node->calleeStorageName);
        emit(OpCode::LoadVar, nameIdx, node->line);
        emit(OpCode::CallValue, 0, node->line, node->arguments.size());
        return;
    }

    // Bare `foo(args)` (no receiver, no namespace) means an implicit
    // same-class self-call - which may resolve to a method INHERITED from
    // an ancestor class, not necessarily declared directly on
    // currentClassName_ itself, so walk the chain the same way
    // collectOverloadsInChain does in the type checker. `node->resolvedDispatch`
    // is the SPECIFIC overload the type checker already picked for this call
    // site (see TypeChecker::inferCall) - the compiler doesn't redo overload
    // resolution itself, just looks up the exact signature it was told.
    const std::size_t functionIndex = resolveFunctionIndex(currentClassName_, node->resolvedDispatch);
    for (const auto& arg : node->arguments) compileExpression(arg.get());
    emit(OpCode::Call, functionIndex, node->line, typeArgNameOperand(chunk_, node->resolvedTypeArgNames));
}

// Assignment targets the lexical binding resolved by the checker. Validate
// before duplicating/storing so a failed write preserves both the old value
// and the surrounding expression's stack shape.
void Compiler::compileAssign(const AssignExpr* node) {
    compileExpression(node->value.get());
    if (!node->assertedTypeName.empty() && node->assertedTypeName != "unknown")
        emit(OpCode::AssertType, chunk_.addName(node->assertedTypeName), node->line);
    emit(OpCode::Dup, 0, node->line);
    std::size_t nameIdx = chunk_.addName(node->storageName);
    emit(OpCode::DefineVar, nameIdx, node->line);
}

void Compiler::compileMove(const MoveExpr* node) {
    const std::size_t nameIdx = chunk_.addName(node->storageName);
    emit(OpCode::MoveVar, nameIdx, node->line);
}

// {1, 2, 3}  or  {"a": 1, "b": 2}
//
// Built the same way regardless of length: create an empty container, then
// for each element/entry, Dup the container reference (one copy to keep
// building on, one copy consumed by the push/mapSet call), and Pop that
// call's nil return value. What's left on the stack at the end is exactly
// one reference to the finished container - the literal's result.
void Compiler::compileCollectionLiteral(const CollectionLiteral* node) {
    auto requireNative = [&](const char* qualifiedName) {
        auto idx = findNativeFunction(qualifiedName);
        if (!idx) throw std::runtime_error(std::string("Compiler: missing native func ") + qualifiedName);
        return *idx;
    };

    // A typed generic List/Map/Set literal is a real ZL object, equivalent to
    // constructing the corresponding generic collection and filling it through
    // its public methods. This keeps collection behavior in the ZL class layer
    // rather than adding another VM-level collection representation.
    if (!node->targetCollectionKind.empty() &&
        (node->targetCollectionKind == "List" ||
         node->targetCollectionKind == "Map" ||
         node->targetCollectionKind == "Set")) {
        const std::string& className = node->targetCollectionKind;
        std::size_t classIdx = chunk_.addName(className);
        const std::size_t ctorSlot = methodSlot(DispatchSignature{className, {}});
        // Tag the new object with its concrete generic instantiation (e.g.
        // "List<int>") so runtime method calls substitute the element type -
        // exactly what `new List<int>()` records via NewObject's operand3.
        // Without this a typed literal produced an erased bare `List` whose
        // element type was unknowable at runtime (and failed a List<int>
        // assignment assertion).
        Instruction litNewObject{OpCode::NewObject, classIdx, node->line};
        if (!node->targetCollectionClassName.empty()) {
            litNewObject.operand3 = chunk_.addName(node->targetCollectionClassName) + 1;
        }
        chunk_.code.push_back(litNewObject);
        emit(OpCode::Dup, 0, node->line);
        emit(OpCode::InvokeMethod, ctorSlot, node->line, 0);
        emit(OpCode::Pop, 0, node->line);

        const std::string method = className == "List" ? "push" :
                                   className == "Set" ? "add" : "put";
        DispatchSignature methodSignature = node->targetDispatch;
        methodSignature.name = method;
        const std::size_t methodSlotIndex = methodSlot(methodSignature);

        if (className == "Map") {
            for (const auto& entry : node->entries) {
                emit(OpCode::Dup, 0, node->line);
                compileExpression(entry.first.get());
                compileExpression(entry.second.get());
                emit(OpCode::InvokeMethod, methodSlotIndex, node->line, 2);
                emit(OpCode::Pop, 0, node->line);
            }
        } else {
            for (const auto& elem : node->elements) {
                emit(OpCode::Dup, 0, node->line);
                compileExpression(elem.get());
                emit(OpCode::InvokeMethod, methodSlotIndex, node->line, 1);
                emit(OpCode::Pop, 0, node->line);
            }
        }
        return;
    }

    // An empty literal has no entries to reveal its shape, so the container the
    // checker recorded from the declaration decides: `map<string,int> m = {}`
    // is a map even though `node->isMap` (which means "the literal itself had
    // key:value entries") is false. Without this the empty map literal fell
    // through to the list branch and tripped the declaration's runtime type
    // assertion on the reference pipeline.
    if (node->isMap || node->targetCollectionKind == "map") {
        std::size_t newMapIdx = requireNative("Collection.newMap");
        std::size_t mapSetIdx = requireNative("Collection.mapSet");
        emit(OpCode::CallNative, newMapIdx, node->line);
        for (const auto& entry : node->entries) {
            emit(OpCode::Dup, 0, node->line);
            compileExpression(entry.first.get());
            compileExpression(entry.second.get());
            emit(OpCode::CallNative, mapSetIdx, node->line);
            emit(OpCode::Pop, 0, node->line);
        }
    } else if (node->targetCollectionKind == "set") {
        std::size_t newSetIdx = requireNative("Collection.newSet");
        std::size_t setAddIdx = requireNative("Collection.setAdd");
        emit(OpCode::CallNative, newSetIdx, node->line);
        for (const auto& elem : node->elements) {
            emit(OpCode::Dup, 0, node->line);
            compileExpression(elem.get());
            emit(OpCode::CallNative, setAddIdx, node->line);
            emit(OpCode::Pop, 0, node->line);
        }
    } else {
        std::size_t newListIdx = requireNative("Collection.newList");
        std::size_t pushIdx = requireNative("Collection.push");
        emit(OpCode::CallNative, newListIdx, node->line);
        for (const auto& elem : node->elements) {
            emit(OpCode::Dup, 0, node->line);
            compileExpression(elem.get());
            emit(OpCode::CallNative, pushIdx, node->line);
            emit(OpCode::Pop, 0, node->line);
        }
    }
}

void Compiler::compileNewExpr(const NewExpr* node) {
    // Allocate and tag the object, then invoke its constructor through the
    // ordinary typed dispatch path. Keep a duplicate as the expression value.
    std::size_t classIdx = chunk_.addName(node->className);
    Instruction newObject{OpCode::NewObject, classIdx, node->line};
    if (!node->resolvedClassName.empty() && node->resolvedClassName != node->className) {
        newObject.operand3 = chunk_.addName(node->resolvedClassName) + 1;
    }
    chunk_.code.push_back(newObject);

    // We need the object ref as the result of the new expression, so duplicate it.
    // The constructor call will consume one.
    emit(OpCode::Dup, 0, node->line);
    
    for (const auto& arg : node->arguments) compileExpression(arg.get());
    
    // Constructors use the same typed dispatch slot machinery as ordinary
    // methods, so overloads remain distinct without runtime name lookup.
    const std::size_t ctorSlot = methodSlot(node->resolvedDispatch);
    emit(OpCode::InvokeMethod, ctorSlot, node->line, node->arguments.size()); // consumes object and args, pushes return value (void)
    emit(OpCode::Pop, 0, node->line); // discard the void return value
    
    // The Dup'd ObjectRef is now at the top of the stack.
}

void Compiler::compileDataLiteralExpr(const DataLiteralExpr* node) {
    // A data type has no constructor to call - just allocate, then set
    // every field directly. SetField's stack shape is (object, value) ->
    // (value) (see compileFieldAssign), so each field needs the object
    // re-Dup'd first and the resulting value popped back off afterward,
    // leaving exactly one ObjectRef on the stack once every field is set.
    std::size_t typeIdx = chunk_.addName(node->typeName);
    emit(OpCode::NewObject, typeIdx, node->line);

    for (const auto& [fieldName, valueExpr] : node->fields) {
        emit(OpCode::Dup, 0, node->line);           // [obj, obj]
        compileExpression(valueExpr.get());          // [obj, obj, value]
        std::size_t nameIdx = chunk_.addName(fieldName);
        emit(OpCode::SetField, nameIdx, node->line); // pops value+obj, pushes value back: [obj, value]
        emit(OpCode::Pop, 0, node->line);             // [obj]
    }
}

void Compiler::compileIndexAccess(const IndexAccessExpr* node) {
    compileExpression(node->object.get());
    compileExpression(node->index.get());
    emit(OpCode::GetIndex, 0, node->line);
}

void Compiler::compileFieldAccess(const FieldAccessExpr* node) {
    if (node->isStaticFieldAccess) {
        emit(OpCode::GetStaticField, chunk_.addName(node->staticFieldClassName), node->line, chunk_.addName(node->fieldName));
        return;
    }
    if (node->isFunctionReference) {
        const auto* idNode = node->object && node->object->kind == NodeKind::Identifier
            ? static_cast<const Identifier*>(node->object.get()) : nullptr;
        if (!idNode) throw std::runtime_error("Compiler: malformed function reference");
        const std::size_t functionIndex = resolveFunctionIndex(idNode->name, node->resolvedFunctionDispatch);
        if (functionIndex >= chunk_.functions.size())
            throw std::runtime_error("Compiler: function reference index out of bounds");
        emit(OpCode::MakeClosure, functionIndex, node->line);
        return;
    }
    if (node->isMathConstantAccess) {
        double value = 0.0;
        if (!mathConstantValue(node->fieldName, value))
            throw std::runtime_error("Compiler: unknown Math constant '" + node->fieldName + "'");
        emit(OpCode::PushConst, chunk_.addConstant(Value{value}), node->line);
        return;
    }
    if (node->isEnumMemberAccess) {
        // EnumName.MEMBER compiles straight to a string constant - see
        // FieldAccessExpr::isEnumMemberAccess's comment for why (there's no
        // "EnumName" object to evaluate at runtime at all).
        std::size_t idx = chunk_.addConstant(Value{node->fieldName});
        emit(OpCode::PushConst, idx, node->line);
        return;
    }
    compileExpression(node->object.get());
    std::size_t nameIdx = chunk_.addName(node->fieldName);
    emit(OpCode::GetField, nameIdx, node->line);
}

void Compiler::compileFieldAssign(const FieldAssignExpr* node) {
    if (node->isStaticFieldAssign) {
        compileExpression(node->value.get());
        emit(OpCode::SetStaticField, chunk_.addName(node->staticFieldClassName), node->line, chunk_.addName(node->fieldName));
        return;
    }
    // AssignExpr expects the value to be left on the stack.
    // The SetField instruction pops (value, object) and pushes (value) back.
    // That means we need object THEN value on the stack.
    compileExpression(node->object.get()); // push object
    compileExpression(node->value.get());  // push value
    std::size_t nameIdx = chunk_.addName(node->fieldName);
    emit(OpCode::SetField, nameIdx, node->line); // pops value, pops obj, pushes value
}

void Compiler::compileMethodCall(const MethodCallExpr* node) {
    if (node->isStringMethod) {
        // A method on a `string` is the resolved `String.*` native with the
        // receiver as its first argument (TypeChecker::inferMethodCall). The
        // native's own arity tells the VM how many values to pop, so the
        // receiver and the arguments are simply pushed in order.
        const auto idx = findNativeFunction(node->nativeMethodName);
        if (!idx) {
            throw std::runtime_error("Compiler: unknown string method native '" +
                                     node->nativeMethodName + "'");
        }
        compileExpression(node->object.get());
        for (const auto& arg : node->arguments) compileExpression(arg.get());
        emit(OpCode::CallNative, *idx, node->line);
        return;
    }

    if (node->isTaskMethod) {
        compileExpression(node->object.get());
        if (node->methodName == "block") {
            emit(OpCode::TaskBlock, 0, node->line);
        } else if (node->methodName == "ignore") {
            emit(OpCode::TaskIgnore, 0, node->line);
        } else {
            emit(OpCode::TaskCancel, 0, node->line);
        }
        return;
    }

    // 1. push object
    // 2. push args
    // 3. InvokeMethod (bare method name + explicit arg count - the VM
    //    resolves the actual class-qualified func using the receiver's
    //    runtime class; see the InvokeMethod comment in bytecode.hpp).
    //    The bare name carries the resolved overload's signature suffix
    //    too (see TypeChecker::inferMethodCall, which already picked which
    //    overload this call resolves to) - without it, two overloads of the
    //    same method name would be indistinguishable to InvokeMethod's
    //    chain walk, which matches by exact name string.
    compileExpression(node->object.get());
    for (const auto& arg : node->arguments) compileExpression(arg.get());

    std::size_t slot = methodSlot(node->resolvedDispatch);
    emit(OpCode::InvokeMethod, slot, node->line, node->arguments.size(),
         typeArgNameOperand(chunk_, node->resolvedTypeArgNames));
}

void Compiler::compileThisExpr(const ThisExpr* node) {
    std::size_t nameIdx = chunk_.addName("this");
    emit(OpCode::LoadVar, nameIdx, node->line);
}

void Compiler::compileSuperCallExpr(const SuperCallExpr* node) {
    // super(args) - calls the parent's constructor. Statically resolved:
    // `super` always means "my declared parent" (currentParentClassName_),
    // never whatever the receiver's runtime class happens to be, so there's
    // no dynamic-dispatch chain walk here (unlike InvokeMethod). The
    // specific constructor OVERLOAD was already picked by the type checker
    // (see TypeChecker::inferSuperCallExpr) - node->resolvedDispatch carries
    // which one.
    std::size_t thisIdx = chunk_.addName("this");
    emit(OpCode::LoadVar, thisIdx, node->line);
    for (const auto& arg : node->arguments) compileExpression(arg.get());

    const std::size_t functionIndex = resolveFunctionIndex(currentParentClassName_, node->resolvedDispatch);
    emit(OpCode::InvokeSuper, functionIndex, node->line, node->arguments.size());
}

void Compiler::compileSuperMethodCallExpr(const SuperMethodCallExpr* node) {
    // super.method(args) - same idea as compileSuperCallExpr, but for an
    // ordinary method instead of the constructor. node->resolvedDispatch
    // carries which overload the type checker already picked (see
    // TypeChecker::inferSuperMethodCallExpr).
    std::size_t thisIdx = chunk_.addName("this");
    emit(OpCode::LoadVar, thisIdx, node->line);
    for (const auto& arg : node->arguments) compileExpression(arg.get());

    const std::size_t functionIndex = resolveFunctionIndex(currentParentClassName_, node->resolvedDispatch);
    emit(OpCode::InvokeSuper, functionIndex, node->line, node->arguments.size(),
         typeArgNameOperand(chunk_, node->resolvedTypeArgNames));
}

} // namespace zl
