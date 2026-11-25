#include "zl/compiler/ir_lowering.hpp"
#include <unordered_map>
#include <sstream>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace zl::ir {
namespace {

struct LocalInfo { ValueId slot{0}; OwnershipKind ownership{OwnershipKind::GC}; };

struct FnLowerer {
    Function out;
    std::vector<BasicBlock> blocks;
    std::size_t current{0};
    ValueId nextValue{1};
    bool complete{true};
    std::vector<std::string> diagnostics;
    std::unordered_map<std::string, LocalInfo> locals;
    std::vector<BlockId> breakTargets;
    std::vector<BlockId> continueTargets;
    std::string ownerClassName;

    explicit FnLowerer(const FunctionDecl& fn) {
        ownerClassName = fn.ownerClassName;
        out.name = fn.ownerClassName.empty() ? fn.name : fn.ownerClassName + "." + fn.name;
        out.returnType = fn.returnType.name.empty() ? "void" : fn.returnType.name;
        out.returnOwnership = OwnershipKind::GC;
        blocks.push_back(BasicBlock{0, {}, {}});
        for (const auto& p : fn.params) {
            LocalInfo info{nextValue++, p.ownership};
            locals[p.name] = info;
            out.parameterTypes.push_back(p.type.name);
            out.parameterOwnership.push_back(p.ownership);
            blocks[0].instructions.push_back({Opcode::DefineLocal, info.slot, 0, 0, p.name, p.type.line, p.ownership});
        }
        out.isAsync = fn.isAsync;
        for (const auto& a : fn.annotations) if (a.name == "native") out.isNative = true;
    }

    ValueId fresh() { return nextValue++; }
    BasicBlock& bb() { return blocks[current]; }
    BlockId newBlock() {
        BlockId id = static_cast<BlockId>(blocks.size());
        blocks.push_back(BasicBlock{id, {}, {}});
        return id;
    }
    void setCurrent(BlockId id) { current = static_cast<std::size_t>(id); }
    void unsupported(const AstNode* node, const std::string& what) {
        complete = false;
        std::ostringstream oss;
        oss << "IR lowering: unsupported " << what << " at line " << (node ? node->line : 0);
        diagnostics.push_back(oss.str());
    }

    ValueId expr(const AstNode* n) {
        if (!n) { unsupported(n, "null expression"); return 0; }
        switch (n->kind) {
            case NodeKind::Literal: {
                const auto* lit = static_cast<const Literal*>(n);
                ValueId r = fresh();
                bb().instructions.push_back({Opcode::Const, r, 0, 0, lit->raw, n->line, OwnershipKind::GC});
                return r;
            }
            case NodeKind::Identifier: {
                const auto* id = static_cast<const Identifier*>(n);
                auto it = locals.find(id->name);
                if (it == locals.end()) { unsupported(n, "unbound identifier '" + id->name + "'"); return 0; }
                ValueId r = fresh();
                bb().instructions.push_back({Opcode::LoadLocal, r, it->second.slot, 0, id->name, n->line, it->second.ownership});
                return r;
            }
            case NodeKind::MoveExpr: {
                const auto* mv = static_cast<const MoveExpr*>(n);
                auto it = locals.find(mv->name);
                if (it == locals.end()) { unsupported(n, "move of unknown local '" + mv->name + "'"); return 0; }
                ValueId r = fresh();
                bb().instructions.push_back({Opcode::MoveLocal, r, it->second.slot, 0, mv->name, n->line, OwnershipKind::OWNED});
                return r;
            }
            case NodeKind::UnaryExpr: {
                const auto* u = static_cast<const UnaryExpr*>(n);
                ValueId operand = expr(u->operand.get());
                if (!operand) return 0;
                ValueId r = fresh();
                bb().instructions.push_back({Opcode::Unary, r, operand, 0, std::to_string(static_cast<int>(u->op)), n->line, OwnershipKind::GC});
                return r;
            }
            case NodeKind::BinaryExpr: {
                const auto* b = static_cast<const BinaryExpr*>(n);
                if (b->op == TokenType::AND || b->op == TokenType::OR) {
                    // Logical operators must short-circuit exactly like the VM.
                    // Materialize the result in one merge-local so each branch can
                    // write its boolean outcome and the join can load one stable value.
                    ValueId left = expr(b->left.get());
                    if (!left) return 0;
                    const ValueId resultSlot = fresh();
                    const ValueId falseValue = fresh();
                    bb().instructions.push_back({Opcode::Const, falseValue, 0, 0, b->op == TokenType::AND ? "false" : "true", n->line, OwnershipKind::GC});
                    bb().instructions.push_back({Opcode::DefineLocal, resultSlot, falseValue, 0, "__logical_result", n->line, OwnershipKind::GC});
                    const BlockId shortCircuit = newBlock();
                    const BlockId rhsBlock = newBlock();
                    const BlockId join = newBlock();
                    if (b->op == TokenType::AND) {
                        bb().successors.push_back(rhsBlock);
                        bb().successors.push_back(shortCircuit);
                    } else {
                        bb().successors.push_back(shortCircuit);
                        bb().successors.push_back(rhsBlock);
                    }
                    bb().instructions.push_back({Opcode::Branch, 0, left, 0, {}, n->line, OwnershipKind::GC});

                    setCurrent(shortCircuit);
                    bb().successors.push_back(join);
                    bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, n->line, OwnershipKind::GC});

                    setCurrent(rhsBlock);
                    ValueId right = expr(b->right.get());
                    if (!right) return 0;
                    bb().instructions.push_back({Opcode::AssignLocal, resultSlot, right, 0, "__logical_result", n->line, OwnershipKind::GC});
                    if (!hasTerminator()) {
                        bb().successors.push_back(join);
                        bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, n->line, OwnershipKind::GC});
                    }
                    setCurrent(join);
                    ValueId result = fresh();
                    bb().instructions.push_back({Opcode::LoadLocal, result, resultSlot, 0, "__logical_result", n->line, OwnershipKind::GC});
                    return result;
                }
                ValueId l = expr(b->left.get());
                ValueId rgt = expr(b->right.get());
                if (!l || !rgt) return 0;
                ValueId r = fresh();
                bb().instructions.push_back({Opcode::Binary, r, l, rgt, std::to_string(static_cast<int>(b->op)), n->line, OwnershipKind::GC});
                return r;
            }
            case NodeKind::CallExpr: {
                const auto* call = static_cast<const CallExpr*>(n);
                if (call->isValueCall || call->isClassTypeLiteral) {
                    unsupported(n, "indirect/class-literal call in native MIR subset");
                    return 0;
                }
                std::vector<ValueId> args;
                args.reserve(call->arguments.size());
                for (const auto& arg : call->arguments) {
                    ValueId a = expr(arg.get());
                    if (!a) return 0;
                    args.push_back(a);
                }
                std::string target;
                if (!call->namespaceName.empty()) target = call->namespaceName + "." + call->calleeName;
                else target = ownerClassName.empty() ? call->calleeName : ownerClassName + "." + call->calleeName;
                ValueId r = fresh();
                Instruction ins{Opcode::Call, r, 0, 0, target, n->line, OwnershipKind::GC};
                ins.operands = std::move(args);
                bb().instructions.push_back(std::move(ins));
                return r;
            }
            case NodeKind::MatchExpr: {
                const auto* m = static_cast<const MatchExpr*>(n);
                if (m->arms.empty()) { unsupported(n, "empty match expression"); return 0; }
                const auto* last = &m->arms.back();
                if (last->patternKind != MatchExpr::PatternKind::Wildcard &&
                    last->patternKind != MatchExpr::PatternKind::Variable) {
                    unsupported(n, "non-exhaustive match in native MIR subset");
                    return 0;
                }
                if (last->patternKind == MatchExpr::PatternKind::Variable && !last->bindingName.empty() && last->bindingName != "_") {
                    unsupported(n, "match variable binding in native MIR subset");
                    return 0;
                }
                ValueId subject = expr(m->subject.get());
                if (!subject) return 0;
                const ValueId resultSlot = fresh();
                bb().instructions.push_back({Opcode::DefineLocal, resultSlot, 0, 0, "__match_result", n->line, OwnershipKind::GC});
                const BlockId initialTest = static_cast<BlockId>(current);
                const BlockId join = newBlock();
                std::vector<BlockId> testBlocks;
                std::vector<BlockId> armBlocks;
                if (m->arms.size() > 1) {
                    testBlocks.push_back(initialTest);
                    for (std::size_t i = 1; i + 1 < m->arms.size(); ++i) testBlocks.push_back(newBlock());
                    for (std::size_t i = 0; i + 1 < m->arms.size(); ++i) armBlocks.push_back(newBlock());
                }
                const BlockId finalArm = newBlock();

                for (std::size_t i = 0; i + 1 < m->arms.size(); ++i) {
                    const auto& arm = m->arms[i];
                    if (arm.patternKind != MatchExpr::PatternKind::Literal) {
                        unsupported(n, "complex match pattern in native MIR subset");
                        return 0;
                    }
                    if (arm.literalType != TokenType::INT_LITERAL && arm.literalType != TokenType::BOOL_LITERAL) {
                        unsupported(n, "non-primitive match literal in native MIR subset");
                        return 0;
                    }
                    setCurrent(testBlocks[i]);
                    ValueId testValue = fresh();
                    bb().instructions.push_back({Opcode::Const, testValue, 0, 0, arm.raw, arm.line, OwnershipKind::GC});
                    ValueId cond = fresh();
                    bb().instructions.push_back({Opcode::Binary, cond, subject, testValue, std::to_string(static_cast<int>(TokenType::EQ)), arm.line, OwnershipKind::GC});
                    const BlockId falseTarget = (i + 1 < testBlocks.size()) ? testBlocks[i + 1] : finalArm;
                    bb().successors.push_back(armBlocks[i]);
                    bb().successors.push_back(falseTarget);
                    bb().instructions.push_back({Opcode::Branch, 0, cond, 0, {}, arm.line, OwnershipKind::GC});

                    setCurrent(armBlocks[i]);
                    ValueId armResult = expr(arm.result.get());
                    if (!armResult) return 0;
                    bb().instructions.push_back({Opcode::AssignLocal, resultSlot, armResult, 0, "__match_result", arm.line, OwnershipKind::GC});
                    bb().successors.push_back(join);
                    bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, arm.line, OwnershipKind::GC});
                }

                setCurrent(finalArm);
                ValueId finalResult = expr(last->result.get());
                if (!finalResult) return 0;
                bb().instructions.push_back({Opcode::AssignLocal, resultSlot, finalResult, 0, "__match_result", last->line, OwnershipKind::GC});
                bb().successors.push_back(join);
                bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, last->line, OwnershipKind::GC});

                setCurrent(join);
                ValueId result = fresh();
                bb().instructions.push_back({Opcode::LoadLocal, result, resultSlot, 0, "__match_result", n->line, OwnershipKind::GC});
                return result;
            }
            case NodeKind::MethodCallExpr:
                unsupported(n, "method call in native MIR subset");
                return 0;
            case NodeKind::AwaitExpr:
                unsupported(n, "await expression in ownership IR subset");
                return 0;
            default:
                unsupported(n, "expression node");
                return 0;
        }
    }

    bool hasTerminator() const {
        const auto& ins = blocks[current].instructions;
        if (ins.empty()) return false;
        switch (ins.back().opcode) {
            case Opcode::Return:
            case Opcode::Throw:
            case Opcode::Jump:
            case Opcode::Branch:
                return true;
            default: return false;
        }
    }

    void lowerIf(const IfStmt* node) {
        const BlockId join = newBlock();
        std::vector<BlockId> branchEntry;
        branchEntry.reserve(node->branches.size() + (node->elseBody ? 1 : 0));
        for (std::size_t i = 0; i < node->branches.size(); ++i) branchEntry.push_back(newBlock());
        BlockId elseEntry = node->elseBody ? newBlock() : join;

        for (std::size_t i = 0; i < node->branches.size(); ++i) {
            const auto& br = node->branches[i];
            ValueId cond = expr(br.condition.get());
            if (!cond) return;
            const BlockId falseTarget = (i + 1 < branchEntry.size()) ? branchEntry[i + 1] : elseEntry;
            bb().successors.push_back(branchEntry[i]);
            bb().successors.push_back(falseTarget);
            bb().instructions.push_back({Opcode::Branch, 0, cond, 0, {}, br.condition ? br.condition->line : node->line, OwnershipKind::GC});
            setCurrent(branchEntry[i]);
            if (br.body) stmt(br.body.get());
            if (!hasTerminator()) {
                bb().successors.push_back(join);
                bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, br.body ? br.body->line : node->line, OwnershipKind::GC});
            }
            if (i + 1 < branchEntry.size()) {
                setCurrent(branchEntry[i + 1]);
            }
        }
        if (node->elseBody) {
            setCurrent(elseEntry);
            stmt(node->elseBody.get());
            if (!hasTerminator()) {
                bb().successors.push_back(join);
                bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->elseBody->line, OwnershipKind::GC});
            }
        } else {
            // The final false path reaches the join directly.
            if (elseEntry == join) {
                // The last branch was responsible for constructing the branch
                // instruction; no additional block is required.
                // It still needs a CFG edge from the currently active false
                // path. That edge was emitted above.
            }
        }
        setCurrent(join);
    }

    void lowerWhile(const WhileStmt* node) {
        const BlockId header = newBlock();
        const BlockId body = newBlock();
        const BlockId exit = newBlock();
        bb().successors.push_back(header);
        bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        setCurrent(header);
        ValueId cond = expr(node->condition.get());
        if (!cond) return;
        bb().successors.push_back(body);
        bb().successors.push_back(exit);
        bb().instructions.push_back({Opcode::Branch, 0, cond, 0, {}, node->line, OwnershipKind::GC});
        breakTargets.push_back(exit);
        continueTargets.push_back(header);
        setCurrent(body);
        stmt(node->body.get());
        breakTargets.pop_back();
        continueTargets.pop_back();
        if (!hasTerminator()) {
            bb().successors.push_back(header);
            bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        }
        setCurrent(exit);
    }

    void lowerRepeat(const RepeatStmt* node) {
        const BlockId body = newBlock();
        const BlockId condBlock = newBlock();
        const BlockId exit = newBlock();
        bb().successors.push_back(body);
        bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        setCurrent(body);
        breakTargets.push_back(exit);
        continueTargets.push_back(condBlock);
        stmt(node->body.get());
        breakTargets.pop_back();
        continueTargets.pop_back();
        if (!hasTerminator()) {
            bb().successors.push_back(condBlock);
            bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        }
        setCurrent(condBlock);
        ValueId cond = expr(node->condition.get());
        if (!cond) return;
        bb().successors.push_back(body);
        bb().successors.push_back(exit);
        bb().instructions.push_back({Opcode::Branch, 0, cond, 0, {}, node->line, OwnershipKind::GC});
        setCurrent(exit);
    }

    void lowerFor(const ForStmt* node) {
        if (node->iterable) {
            unsupported(node, "list for-in loop in the legacy IR lowerer");
            return;
        }
        ValueId start = expr(node->start.get());
        ValueId end = expr(node->end.get());
        ValueId step = expr(node->step.get());
        if (!start || !end || !step) return;
        LocalInfo info{fresh(), OwnershipKind::GC};
        locals[node->varName] = info;
        bb().instructions.push_back({Opcode::DefineLocal, info.slot, start, 0, node->varName, node->line, OwnershipKind::GC});
        const BlockId header = newBlock();
        const BlockId ascCheck = newBlock();
        const BlockId ascBody = newBlock();
        const BlockId descCheck = newBlock();
        const BlockId descBody = newBlock();
        const BlockId increment = newBlock();
        const BlockId exit = newBlock();
        bb().successors.push_back(header);
        bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        setCurrent(header);
        // When the source step is a known integer literal, its sign is known at
        // lowering time. Avoid generating the generic direction-dispatch branch
        // and let the optimizer/native backend see a straight loop immediately.
        // Dynamic steps retain the fully general ascending/descending path.
        std::optional<std::int64_t> knownStep;
        if (node->step && node->step->kind == NodeKind::Literal) {
            const auto* lit = static_cast<const Literal*>(node->step.get());
            if (lit->literalType == TokenType::INT_LITERAL) {
                try {
                    std::size_t pos = 0;
                    const auto parsed = std::stoll(lit->raw, &pos);
                    if (pos == lit->raw.size()) knownStep = parsed;
                } catch (...) {
                    knownStep.reset();
                }
            }
        }
        if (knownStep.has_value()) {
            const BlockId target = *knownStep >= 0 ? ascCheck : descCheck;
            bb().successors.push_back(target);
            bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, node->line, OwnershipKind::GC});
        } else {
            ValueId zero = fresh();
            bb().instructions.push_back({Opcode::Const, zero, 0, 0, "0", node->line, OwnershipKind::GC});
            ValueId stepNonNeg = fresh();
            bb().instructions.push_back({Opcode::Binary, stepNonNeg, step, zero, std::to_string(static_cast<int>(zl::TokenType::GTE)), node->line, OwnershipKind::GC});
            bb().successors.push_back(ascCheck);
            bb().successors.push_back(descCheck);
            bb().instructions.push_back({Opcode::Branch, 0, stepNonNeg, 0, {}, node->line, OwnershipKind::GC});
        }
        setCurrent(ascCheck);
        ValueId curA = fresh();
        bb().instructions.push_back({Opcode::LoadLocal, curA, info.slot, 0, node->varName, node->line, OwnershipKind::GC});
        ValueId condA = fresh();
        bb().instructions.push_back({Opcode::Binary, condA, curA, end, std::to_string(static_cast<int>(zl::TokenType::LT)), node->line, OwnershipKind::GC});
        bb().successors.push_back(ascBody);
        bb().successors.push_back(exit);
        bb().instructions.push_back({Opcode::Branch, 0, condA, 0, {}, node->line, OwnershipKind::GC});
        setCurrent(descCheck);
        ValueId curD = fresh();
        bb().instructions.push_back({Opcode::LoadLocal, curD, info.slot, 0, node->varName, node->line, OwnershipKind::GC});
        ValueId condD = fresh();
        bb().instructions.push_back({Opcode::Binary, condD, curD, end, std::to_string(static_cast<int>(zl::TokenType::GT)), node->line, OwnershipKind::GC});
        bb().successors.push_back(descBody);
        bb().successors.push_back(exit);
        bb().instructions.push_back({Opcode::Branch, 0, condD, 0, {}, node->line, OwnershipKind::GC});
        breakTargets.push_back(exit);
        continueTargets.push_back(increment);
        setCurrent(ascBody); stmt(node->body.get()); if (!hasTerminator()) { bb().successors.push_back(increment); bb().instructions.push_back({Opcode::Jump,0,0,0,{},node->line,OwnershipKind::GC}); }
        setCurrent(descBody); stmt(node->body.get()); if (!hasTerminator()) { bb().successors.push_back(increment); bb().instructions.push_back({Opcode::Jump,0,0,0,{},node->line,OwnershipKind::GC}); }
        breakTargets.pop_back(); continueTargets.pop_back();
        setCurrent(increment);
        ValueId cur = fresh(); bb().instructions.push_back({Opcode::LoadLocal,cur,info.slot,0,node->varName,node->line,OwnershipKind::GC});
        ValueId next = fresh(); bb().instructions.push_back({Opcode::Binary,next,cur,step,std::to_string(static_cast<int>(zl::TokenType::PLUS)),node->line,OwnershipKind::GC});
        bb().instructions.push_back({Opcode::AssignLocal,info.slot,next,0,node->varName,node->line,OwnershipKind::GC});
        bb().successors.push_back(header); bb().instructions.push_back({Opcode::Jump,0,0,0,{},node->line,OwnershipKind::GC});
        setCurrent(exit);
    }

    void stmt(const AstNode* n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::BlockStmt: {
                const auto* b = static_cast<const BlockStmt*>(n);
                for (const auto& s : b->statements) stmt(s.get());
                return;
            }
            case NodeKind::IfStmt:
                lowerIf(static_cast<const IfStmt*>(n));
                return;
            case NodeKind::WhileStmt:
                lowerWhile(static_cast<const WhileStmt*>(n));
                return;
            case NodeKind::RepeatStmt:
                lowerRepeat(static_cast<const RepeatStmt*>(n));
                return;
            case NodeKind::ForStmt:
                lowerFor(static_cast<const ForStmt*>(n));
                return;
            case NodeKind::BreakStmt:
                if (breakTargets.empty()) { unsupported(n, "break outside loop"); return; }
                bb().successors.push_back(breakTargets.back());
                bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, n->line, OwnershipKind::GC});
                return;
            case NodeKind::ContinueStmt:
                if (continueTargets.empty()) { unsupported(n, "continue outside loop"); return; }
                bb().successors.push_back(continueTargets.back());
                bb().instructions.push_back({Opcode::Jump, 0, 0, 0, {}, n->line, OwnershipKind::GC});
                return;
            case NodeKind::VarDecl: {
                const auto* v = static_cast<const VarDecl*>(n);
                if (!v->initializer) { unsupported(n, "uninitialized local declaration"); return; }
                LocalInfo info{fresh(), v->ownership};
                ValueId value;
                if (v->ownership == OwnershipKind::BORROW && v->initializer->kind == NodeKind::Identifier) {
                    const auto* id = static_cast<const Identifier*>(v->initializer.get());
                    auto it = locals.find(id->name);
                    if (it == locals.end()) { unsupported(n, "borrow of unknown local '" + id->name + "'"); return; }
                    value = it->second.slot;
                    locals[v->name] = info;
                    bb().instructions.push_back({Opcode::BorrowLocal, info.slot, value, 0, v->name, n->line, OwnershipKind::BORROW});
                    return;
                }
                value = expr(v->initializer.get());
                if (!value) return;
                locals[v->name] = info;
                Opcode op = (v->ownership == OwnershipKind::GC || v->ownership == OwnershipKind::SHARED)
                          ? Opcode::DefineLocal : Opcode::DefineLocal;
                bb().instructions.push_back({op, info.slot, value, 0, v->name, n->line, v->ownership});
                return;
            }
            case NodeKind::AssignExpr: {
                const auto* a = static_cast<const AssignExpr*>(n);
                auto it = locals.find(a->name);
                if (it == locals.end()) { unsupported(n, "assignment to unknown local '" + a->name + "'"); return; }
                ValueId value = expr(a->value.get());
                if (!value) return;
                bb().instructions.push_back({Opcode::AssignLocal, it->second.slot, value, 0, a->name, n->line, it->second.ownership});
                return;
            }
            case NodeKind::ReturnStmt: {
                const auto* r = static_cast<const ReturnStmt*>(n);
                ValueId value = r->value ? expr(r->value.get()) : 0;
                bb().instructions.push_back({Opcode::Return, 0, value, 0, {}, n->line, OwnershipKind::GC});
                return;
            }
            case NodeKind::ThrowStmt: {
                const auto* t = static_cast<const ThrowStmt*>(n);
                ValueId value = expr(t->value.get());
                bb().instructions.push_back({Opcode::Throw, 0, value, 0, {}, n->line, OwnershipKind::GC});
                return;
            }
            case NodeKind::ExprStmt: {
                const auto* e = static_cast<const ExprStmt*>(n);
                if (e->expression && e->expression->kind == NodeKind::AssignExpr) {
                    const auto* a = static_cast<const AssignExpr*>(e->expression.get());
                    auto it = locals.find(a->name);
                    if (it == locals.end()) { unsupported(n, "assignment to unknown local '" + a->name + "'"); return; }
                    ValueId value = expr(a->value.get());
                    if (!value) return;
                    bb().instructions.push_back({Opcode::AssignLocal, it->second.slot, value, 0, a->name, n->line, it->second.ownership});
                    return;
                }
                (void)expr(e->expression.get());
                return;
            }
            default:
                unsupported(n, "statement node");
                return;
        }
    }

    Function finish() {
        if (!hasTerminator()) blocks[current].instructions.push_back({Opcode::Return, 0, 0, 0, {}, 0, OwnershipKind::GC});
        out.blocks = std::move(blocks);
        return std::move(out);
    }
};

void collectFunction(const NodePtr& decl, Module& module, std::vector<std::string>& diagnostics, bool& complete, bool nativeOnly) {
    if (!decl) return;
    auto lower = [&](const FunctionDecl& fn) {
        FnLowerer l(fn);
        if (fn.body) l.stmt(fn.body.get());
        complete = complete && l.complete;
        diagnostics.insert(diagnostics.end(), l.diagnostics.begin(), l.diagnostics.end());
        module.functions.push_back(l.finish());
    };
    if (decl->kind == NodeKind::ClassDecl) {
        for (const auto& member : static_cast<const ClassDecl*>(decl.get())->members)
            if (member->kind == NodeKind::FunctionDecl) {
                const auto& fn = *static_cast<const FunctionDecl*>(member.get());
                if (!nativeOnly || std::any_of(fn.annotations.begin(), fn.annotations.end(), [](const Annotation& a) { return a.name == "native"; })) lower(fn);
            }
    } else if (decl->kind == NodeKind::DataDecl) {
        for (const auto& member : static_cast<const DataDecl*>(decl.get())->members)
            if (member->kind == NodeKind::FunctionDecl) {
                const auto& fn = *static_cast<const FunctionDecl*>(member.get());
                if (!nativeOnly || std::any_of(fn.annotations.begin(), fn.annotations.end(), [](const Annotation& a) { return a.name == "native"; })) lower(fn);
            }
    } else if (decl->kind == NodeKind::FunctionDecl) {
        const auto& fn = *static_cast<const FunctionDecl*>(decl.get());
        if (!nativeOnly || std::any_of(fn.annotations.begin(), fn.annotations.end(), [](const Annotation& a) { return a.name == "native"; })) lower(fn);
    }
}
}

LoweringResult lowerProgram(const Program& program, bool nativeOnly) {
    LoweringResult result;
    for (const auto& d : program.declarations) collectFunction(d, result.module, result.diagnostics, result.complete, nativeOnly);
    std::string verificationError;
    if (!verify(result.module, &verificationError)) {
        result.complete = false;
        result.diagnostics.push_back("IR verification failed: " + verificationError);
    }
    return result;
}
}
