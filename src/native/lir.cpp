#include "zl/native/lir.hpp"

#include <algorithm>
#include <sstream>

namespace zl::native {

const char* lirOpcodeName(LirOpcode op) noexcept {
    switch (op) {
        case LirOpcode::Nop: return "nop";
        case LirOpcode::Move: return "move";
        case LirOpcode::LoadImmI: return "imm.i";
        case LirOpcode::LoadImmF: return "imm.f";
        case LirOpcode::IAdd: return "iadd";
        case LirOpcode::ISub: return "isub";
        case LirOpcode::IMul: return "imul";
        case LirOpcode::IDiv: return "idiv";
        case LirOpcode::IMod: return "imod";
        case LirOpcode::INeg: return "ineg";
        case LirOpcode::FAdd: return "fadd";
        case LirOpcode::FSub: return "fsub";
        case LirOpcode::FMul: return "fmul";
        case LirOpcode::FDiv: return "fdiv";
        case LirOpcode::FNeg: return "fneg";
        case LirOpcode::ICmpEq: return "icmp.eq";
        case LirOpcode::ICmpNe: return "icmp.ne";
        case LirOpcode::ICmpLt: return "icmp.lt";
        case LirOpcode::ICmpLe: return "icmp.le";
        case LirOpcode::ICmpGt: return "icmp.gt";
        case LirOpcode::ICmpGe: return "icmp.ge";
        case LirOpcode::FCmpEq: return "fcmp.eq";
        case LirOpcode::FCmpNe: return "fcmp.ne";
        case LirOpcode::FCmpLt: return "fcmp.lt";
        case LirOpcode::FCmpLe: return "fcmp.le";
        case LirOpcode::FCmpGt: return "fcmp.gt";
        case LirOpcode::FCmpGe: return "fcmp.ge";
        case LirOpcode::INot: return "inot";
        case LirOpcode::IAnd: return "iand";
        case LirOpcode::IOr: return "ior";
        case LirOpcode::IXor: return "ixor";
        case LirOpcode::IShl: return "ishl";
        case LirOpcode::IShr: return "ishr";
        case LirOpcode::IUshr: return "iushr";
        case LirOpcode::IBitNot: return "ibitnot";
        case LirOpcode::IntToFloat: return "i2f";
        case LirOpcode::Load: return "load";
        case LirOpcode::Store: return "store";
        case LirOpcode::FrameAddr: return "frameaddr";
        case LirOpcode::Call: return "call";
        case LirOpcode::CallRuntime: return "call.runtime";
        case LirOpcode::CallIndirect: return "call.indirect";
        case LirOpcode::Jump: return "jump";
        case LirOpcode::BranchIf: return "branch";
        case LirOpcode::Return: return "ret";
        case LirOpcode::Unreachable: return "unreachable";
    }
    return "?";
}

bool lirOpcodeIsTerminator(LirOpcode op) noexcept {
    switch (op) {
        case LirOpcode::Jump:
        case LirOpcode::BranchIf:
        case LirOpcode::Return:
        case LirOpcode::Unreachable: return true;
        default: return false;
    }
}

bool lirOpcodeMayTrap(LirOpcode op) noexcept {
    // Integer division and remainder trap on a zero divisor (and on
    // INT64_MIN / -1). Indirect calls can trap when runtime dispatch returns
    // no target. The emitter materialises those guards; recording the fact here
    // keeps an optimiser from hoisting one out of its guard.
    return op == LirOpcode::IDiv || op == LirOpcode::IMod || op == LirOpcode::Unreachable ||
           op == LirOpcode::CallIndirect;
}

bool lirOpcodeIsCall(LirOpcode op) noexcept {
    return op == LirOpcode::Call || op == LirOpcode::CallRuntime || op == LirOpcode::CallIndirect;
}

const VRegInfo* LirFunction::vreg(VReg id) const {
    if (id == kNoVReg || id >= vregs.size()) return nullptr;
    return &vregs[id];
}

LirBlock* LirFunction::block(LirBlockId id) {
    for (auto& b : blocks)
        if (b.id == id) return &b;
    return nullptr;
}

const LirBlock* LirFunction::block(LirBlockId id) const {
    for (const auto& b : blocks)
        if (b.id == id) return &b;
    return nullptr;
}

void LirFunction::rebuildCfg() {
    for (auto& b : blocks) {
        b.successors.clear();
        b.predecessors.clear();
    }
    for (auto& b : blocks) {
        if (b.instructions.empty()) continue;
        const LirInstruction& term = b.instructions.back();
        if (!lirOpcodeIsTerminator(term.opcode)) continue;
        for (const auto& op : term.operands)
            if (op.kind == LirOperandKind::Block) b.successors.push_back(op.block);
    }
    for (auto& b : blocks)
        for (LirBlockId s : b.successors)
            if (LirBlock* target = block(s)) target->predecessors.push_back(b.id);
}

namespace {

std::string operandText(const LirFunction& fn, const LirOperand& op) {
    std::ostringstream out;
    switch (op.kind) {
        case LirOperandKind::None: return "_";
        case LirOperandKind::VirtualReg: {
            out << "%" << op.vreg;
            if (const VRegInfo* info = fn.vreg(op.vreg); info && !info->name.empty())
                out << ":" << info->name;
            return out.str();
        }
        case LirOperandKind::PhysicalReg: return std::string("$") + op.preg.name;
        case LirOperandKind::ImmInt: out << op.imm; return out.str();
        case LirOperandKind::ImmFloat: out << op.fimm; return out.str();
        case LirOperandKind::Frame: {
            out << "slot" << op.slot;
            if (op.slot < fn.slots.size() && !fn.slots[op.slot].name.empty())
                out << ":" << fn.slots[op.slot].name;
            return out.str();
        }
        case LirOperandKind::Block: out << "bb" << op.block; return out.str();
        case LirOperandKind::Symbol: return "@" + op.symbol;
    }
    return "?";
}

} // namespace

std::string printLirFunction(const LirFunction& function) {
    std::ostringstream out;
    out << "func " << function.name << "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) out << ", ";
        out << "%" << function.parameters[i] << ": "
            << valueClassName(function.parameterClasses[i]);
    }
    out << ") -> " << valueClassName(function.returnClass) << " {\n";

    for (std::size_t i = 1; i < function.slots.size(); ++i) {
        const auto& s = function.slots[i];
        out << "  ; slot" << s.id << " " << valueClassName(s.cls) << " size=" << s.size;
        if (!s.name.empty()) out << " '" << s.name << "'";
        if (s.isReference) out << " gc-root";
        out << "\n";
    }

    for (const auto& block : function.blocks) {
        out << " bb" << block.id;
        if (!block.label.empty()) out << " (" << block.label << ")";
        out << ":\n";
        for (const auto& ins : block.instructions) {
            out << "   ";
            if (ins.result != kNoVReg)
                out << "%" << ins.result << ":" << valueClassName(ins.resultClass) << " = ";
            out << lirOpcodeName(ins.opcode);
            if (!ins.callee.empty()) out << " @" << ins.callee;
            for (std::size_t i = 0; i < ins.operands.size(); ++i)
                out << (i == 0 && ins.callee.empty() ? " " : ", ") << operandText(function, ins.operands[i]);
            if (ins.opcode == LirOpcode::CallIndirect)
                out << " via " << operandText(function, ins.calleeTarget);
            if (!ins.possibleCallees.empty()) {
                out << " targets{";
                for (std::size_t i = 0; i < ins.possibleCallees.size(); ++i)
                    out << (i ? ", " : "") << ins.possibleCallees[i];
                out << "}";
            }
            if (!ins.comment.empty()) out << "   ; " << ins.comment;
            out << "\n";
        }
    }
    out << "}\n";
    return out.str();
}

std::string printLirModule(const LirModule& module) {
    std::ostringstream out;
    out << "; native ir module '" << module.name << "'\n";
    if (!module.runtimeImports.empty()) {
        out << "; runtime imports:";
        for (const auto& r : module.runtimeImports) out << " " << r;
        out << "\n";
    }
    for (std::size_t i = 0; i < module.runtimeStrings.size(); ++i)
        out << "; runtime string[" << i << "] = \"" << module.runtimeStrings[i] << "\"\n";
    for (const auto& target : module.dispatchTargets)
        out << "; dispatch[" << target.dispatchId << "] " << target.runtimeClass
            << " -> " << target.function << "\n";
    for (const auto& fn : module.functions) out << "\n" << printLirFunction(fn);
    return out.str();
}

} // namespace zl::native
