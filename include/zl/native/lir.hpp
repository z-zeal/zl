#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zl/native/target.hpp"

// ---------------------------------------------------------------------------
// Native IR ("LIR"): the backend's own representation
// ---------------------------------------------------------------------------
//
// MIR is a *language* IR: its operands carry ZL types, its `Add` means whatever
// ZL says `+` means for the operand types, and it has no notion of a register
// or a frame. Machine code is the opposite: no types, fixed registers, explicit
// stack. Putting one directly on the other would force instruction selection,
// type-directed operator resolution, register assignment, and encoding to all
// happen in a single pass - which is exactly how a backend becomes impossible
// to test.
//
// So there is one representation in between, and it is defined by what it
// *removes* relative to MIR:
//
//   * types collapse to value classes (Integer / Float / Reference), so an
//     instruction's meaning no longer depends on consulting a type table;
//   * one MIR opcode becomes one *typed* machine-level opcode: MIR `Add` on
//     ints becomes `IAdd`, on doubles `FAdd`. After selection, nothing has to
//     ask "what did this mean?" again;
//   * values become virtual registers - unlimited, each with a class - and
//     mutable MIR slots become explicit frame slots with Load/Store;
//   * control flow becomes a linear list of blocks with explicit branches, and
//     MIR block parameters become parallel copies on the predecessor edges
//     (which is the only representation an allocator can act on).
//
// And by what it *keeps*: SSA-ish single definition per vreg, a real CFG,
// per-instruction source locations, and the distinction between a reference
// and an integer. It stays target independent - the only target facts in a
// LIR function are the value classes and whatever the calling convention put
// in the prologue contract.

namespace zl::native {

// --- virtual registers -----------------------------------------------------
//
// A vreg is a value with a class and no location. Ids are dense and 1-based;
// 0 means "no register", which is what an instruction with no result uses.
using VReg = std::uint32_t;
constexpr VReg kNoVReg = 0;

using LirBlockId = std::uint32_t;
constexpr LirBlockId kNoLirBlock = 0;

// A frame slot: a named, class-tagged, addressable home for a value. Mutable
// MIR slots become these; so do spills. Offsets are assigned by the frame
// layout in the emitter, not here, because the offset is a target fact.
using FrameSlot = std::uint32_t;
constexpr FrameSlot kNoFrameSlot = 0;

struct VRegInfo {
    VReg id{kNoVReg};
    ValueClass cls{ValueClass::Integer};
    // Diagnostic name, usually derived from the MIR temp or slot it came from.
    std::string name;
    // Set when this vreg must live in one specific physical register - the
    // incoming argument registers and the outgoing result register. Recorded
    // as a constraint rather than by pre-colouring the code, so the allocator
    // can see it.
    std::optional<PhysReg> fixed;
};

struct FrameSlotInfo {
    FrameSlot id{kNoFrameSlot};
    ValueClass cls{ValueClass::Integer};
    std::uint32_t size{8};
    std::string name;
    // True for a slot holding a managed reference: the GC map for the frame is
    // exactly the set of these that are live at a safepoint.
    bool isReference{false};
};

// --- operands --------------------------------------------------------------
//
// A LIR operand is deliberately much smaller than a MIR operand: no type id,
// no ownership, no constant pool index. A constant that fits an immediate *is*
// an immediate; anything else was materialised into a vreg by selection.
enum class LirOperandKind : std::uint8_t {
    None,
    VirtualReg,
    PhysicalReg,
    // 64-bit signed integer immediate. Whether the target can encode it at
    // this position is the encoder's problem, and the encoder is allowed to
    // materialise it into a scratch register.
    ImmInt,
    // IEEE-754 double immediate. No target encodes one inline, so the emitter
    // routes it through the constant pool or an integer materialisation.
    ImmFloat,
    // Frame slot address/contents, used by Load/Store.
    Frame,
    // Branch target.
    Block,
    // A named callee (a MIR function's qualified name, or a runtime symbol).
    Symbol,
};

struct LirOperand {
    LirOperandKind kind{LirOperandKind::None};
    VReg vreg{kNoVReg};
    PhysReg preg{};
    std::int64_t imm{0};
    double fimm{0.0};
    FrameSlot slot{kNoFrameSlot};
    LirBlockId block{kNoLirBlock};
    std::string symbol;

    [[nodiscard]] bool isNone() const noexcept { return kind == LirOperandKind::None; }

    [[nodiscard]] static LirOperand none() { return LirOperand{}; }
    [[nodiscard]] static LirOperand reg(VReg v) {
        LirOperand o; o.kind = LirOperandKind::VirtualReg; o.vreg = v; return o;
    }
    [[nodiscard]] static LirOperand phys(PhysReg p) {
        LirOperand o; o.kind = LirOperandKind::PhysicalReg; o.preg = p; return o;
    }
    [[nodiscard]] static LirOperand immediate(std::int64_t v) {
        LirOperand o; o.kind = LirOperandKind::ImmInt; o.imm = v; return o;
    }
    [[nodiscard]] static LirOperand immediateF(double v) {
        LirOperand o; o.kind = LirOperandKind::ImmFloat; o.fimm = v; return o;
    }
    [[nodiscard]] static LirOperand frame(FrameSlot s) {
        LirOperand o; o.kind = LirOperandKind::Frame; o.slot = s; return o;
    }
    [[nodiscard]] static LirOperand target(LirBlockId b) {
        LirOperand o; o.kind = LirOperandKind::Block; o.block = b; return o;
    }
    [[nodiscard]] static LirOperand sym(std::string s) {
        LirOperand o; o.kind = LirOperandKind::Symbol; o.symbol = std::move(s); return o;
    }
};

// --- opcodes ---------------------------------------------------------------
//
// Machine-level and *typed*: the operand class is part of the opcode, not of
// the operands. That is the whole point of selection - after it, `IAdd` adds
// integers and there is no second question to ask.
enum class LirOpcode : std::uint16_t {
    Nop,

    // Materialisation. Move copies a value of the operand's class; LoadImmI /
    // LoadImmF put a constant in a fresh vreg.
    Move,
    LoadImmI,
    LoadImmF,

    // Integer arithmetic (64-bit, two's complement, wrapping like ZL's `int`).
    IAdd, ISub, IMul,
    // Signed division/remainder. These may raise (division by zero), which the
    // emitter must express as a real check, so they are marked in
    // lirOpcodeMayTrap.
    IDiv, IMod,
    INeg,

    // Floating-point arithmetic (binary64).
    FAdd, FSub, FMul, FDiv, FNeg,

    // Comparisons produce an Integer-class 0/1 value, which is how a ZL bool
    // is represented. Keeping the boolean *materialised* rather than leaving a
    // flags state live means a compare and the branch that consumes it need
    // not be adjacent, and the peephole that fuses them stays an optimisation
    // instead of a correctness requirement.
    ICmpEq, ICmpNe, ICmpLt, ICmpLe, ICmpGt, ICmpGe,
    FCmpEq, FCmpNe, FCmpLt, FCmpLe, FCmpGt, FCmpGe,

    // Logical/bitwise on integers.
    INot, // logical complement of a 0/1 bool
    IAnd, IOr, IXor, IShl, IShr, IUshr, IBitNot,

    // Conversions.
    IntToFloat,

    // Memory. Load reads a frame slot into a vreg; Store writes one.
    Load,
    Store,
    // The address of a frame slot. Not used by the current subset; declared so
    // that aggregates have somewhere obvious to land.
    FrameAddr,

    // Calls. Call names a symbol; CallRuntime names a runtime helper by symbol
    // and is otherwise identical - the distinction is kept because a runtime
    // call is the documented escape hatch for anything the backend cannot
    // lower yet, and it must be greppable.
    Call,
    CallRuntime,

    // Terminators.
    Jump,
    // Branch on a 0/1 integer vreg: operand 0 is the condition, then the true
    // and false block operands.
    BranchIf,
    Return,
    // Emitted where MIR says control cannot arrive. Encodes as a trap, never
    // as fallthrough.
    Unreachable,
};

[[nodiscard]] const char* lirOpcodeName(LirOpcode op) noexcept;
[[nodiscard]] bool lirOpcodeIsTerminator(LirOpcode op) noexcept;
[[nodiscard]] bool lirOpcodeMayTrap(LirOpcode op) noexcept;
// True when the instruction is a call: a safepoint, a clobber of every
// caller-saved register, and the only place a GC map is required today.
[[nodiscard]] bool lirOpcodeIsCall(LirOpcode op) noexcept;

struct LirInstruction {
    LirOpcode opcode{LirOpcode::Nop};
    // Defined vreg, or kNoVReg.
    VReg result{kNoVReg};
    ValueClass resultClass{ValueClass::Void};
    std::vector<LirOperand> operands;
    // Call only: the callee's symbol and the classes of its arguments, so the
    // emitter can assign argument registers without re-deriving them.
    std::string callee;
    std::vector<ValueClass> argClasses;
    // Source line, carried through from MIR so a native fault can be blamed on
    // ZL source.
    std::uint32_t line{0};
    std::string comment;
};

struct LirBlock {
    LirBlockId id{kNoLirBlock};
    std::string label;
    std::vector<LirInstruction> instructions; // last one is the terminator
    std::vector<LirBlockId> successors;
    std::vector<LirBlockId> predecessors;
};

struct LirFunction {
    std::string name;
    // Parameter vregs in declaration order, already constrained to their
    // argument registers by the convention.
    std::vector<VReg> parameters;
    std::vector<ValueClass> parameterClasses;
    ValueClass returnClass{ValueClass::Void};

    std::vector<VRegInfo> vregs;      // index 0 unused
    std::vector<FrameSlotInfo> slots; // index 0 unused
    std::vector<LirBlock> blocks;
    LirBlockId entry{kNoLirBlock};

    [[nodiscard]] const VRegInfo* vreg(VReg id) const;
    [[nodiscard]] LirBlock* block(LirBlockId id);
    [[nodiscard]] const LirBlock* block(LirBlockId id) const;
    void rebuildCfg();
};

struct LirModule {
    std::string name;
    std::vector<LirFunction> functions;
    // Runtime helpers referenced by CallRuntime, deduplicated. The linker/JIT
    // resolves these; listing them makes "what did this module fall back on?"
    // answerable without scanning the code.
    std::vector<std::string> runtimeImports;
    // NUL-free strings used as runtime metadata (currently class and field
    // names for the native object bridge). Instructions pass indices so code
    // remains relocatable and each executable owns its metadata lifetime.
    std::vector<std::string> runtimeStrings;
};

[[nodiscard]] std::string printLirModule(const LirModule& module);
[[nodiscard]] std::string printLirFunction(const LirFunction& function);

} // namespace zl::native
