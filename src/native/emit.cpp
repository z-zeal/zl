#include "zl/native/emit.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace zl::native {
namespace {

// ---------------------------------------------------------------------------
// x86-64 encoder
// ---------------------------------------------------------------------------
//
// Small on purpose: only the forms the LIR subset needs, each written once,
// each named after the assembly it produces. Everything above this struct
// talks about value classes and vregs; only this struct knows about REX bytes.

struct Assembler {
    std::vector<std::uint8_t> code;

    void u8(std::uint8_t v) { code.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    [[nodiscard]] std::size_t here() const { return code.size(); }

    void rexW(unsigned reg, unsigned rm) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1)));
    }
    void modrmReg(unsigned reg, unsigned rm) {
        u8(static_cast<std::uint8_t>(0xc0 | ((reg & 7) << 3) | (rm & 7)));
    }
    // [rbp + disp32]
    void modrmRbp(unsigned reg, std::int32_t disp) {
        u8(static_cast<std::uint8_t>(0x80 | ((reg & 7) << 3) | 5));
        i32(disp);
    }

    // --- integer moves ----------------------------------------------------
    void movRegImm64(unsigned reg, std::uint64_t value) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1)));
        u8(static_cast<std::uint8_t>(0xb8 + (reg & 7)));
        u64(value);
    }
    void movRegMem(unsigned reg, std::int32_t disp) { rexW(reg, 5); u8(0x8b); modrmRbp(reg, disp); }
    void movMemReg(std::int32_t disp, unsigned reg) { rexW(reg, 5); u8(0x89); modrmRbp(reg, disp); }
    void movRegReg(unsigned dst, unsigned src) { rexW(src, dst); u8(0x89); modrmReg(src, dst); }

    // --- integer ALU (dst op= src) ---------------------------------------
    void aluRegReg(std::uint8_t opcode, unsigned dst, unsigned src) {
        rexW(src, dst); u8(opcode); modrmReg(src, dst);
    }
    void addRegReg(unsigned d, unsigned s) { aluRegReg(0x01, d, s); }
    void subRegReg(unsigned d, unsigned s) { aluRegReg(0x29, d, s); }
    void andRegReg(unsigned d, unsigned s) { aluRegReg(0x21, d, s); }
    void orRegReg(unsigned d, unsigned s)  { aluRegReg(0x09, d, s); }
    void xorRegReg(unsigned d, unsigned s) { aluRegReg(0x31, d, s); }
    void cmpRegReg(unsigned d, unsigned s) { aluRegReg(0x39, d, s); }
    void testRegReg(unsigned d, unsigned s) { aluRegReg(0x85, d, s); }
    void imulRegReg(unsigned dst, unsigned src) { rexW(dst, src); u8(0x0f); u8(0xaf); modrmReg(dst, src); }
    void negReg(unsigned reg) { rexW(0, reg); u8(0xf7); modrmReg(3, reg); }
    void notReg(unsigned reg) { rexW(0, reg); u8(0xf7); modrmReg(2, reg); }
    void cqo() { u8(0x48); u8(0x99); }
    void idivReg(unsigned reg) { rexW(0, reg); u8(0xf7); modrmReg(7, reg); }
    // shift by cl: /4 shl, /5 shr (logical), /7 sar (arithmetic)
    void shiftByCl(unsigned ext, unsigned reg) { rexW(0, reg); u8(0xd3); modrmReg(ext, reg); }
    void addRegImm32(unsigned reg, std::int32_t imm) { rexW(0, reg); u8(0x81); modrmReg(0, reg); i32(imm); }
    void cmpRegImm32(unsigned reg, std::int32_t imm) { rexW(0, reg); u8(0x81); modrmReg(7, reg); i32(imm); }

    // setcc r8 then zero-extend to 64 bits.
    void setcc(std::uint8_t cc, unsigned reg) {
        if (reg >= 4) u8(static_cast<std::uint8_t>(0x40 | ((reg >> 3) & 1)));
        u8(0x0f); u8(cc); u8(static_cast<std::uint8_t>(0xc0 | (reg & 7)));
    }
    void movzxReg8(unsigned reg) {
        u8(static_cast<std::uint8_t>(0x48 | ((reg >> 3) & 1) << 2 | ((reg >> 3) & 1)));
        u8(0x0f); u8(0xb6); modrmReg(reg, reg);
    }

    // --- stack / control --------------------------------------------------
    void pushRbp() { u8(0x55); }
    void popRbp() { u8(0x5d); }
    void movRbpRsp() { u8(0x48); u8(0x89); u8(0xe5); }
    void movRspRbp() { u8(0x48); u8(0x89); u8(0xec); }
    void subRspImm32(std::uint32_t n) { u8(0x48); u8(0x81); u8(0xec); u32(n); }
    void ret() { u8(0xc3); }
    void ud2() { u8(0x0f); u8(0x0b); }
    // Returns the offset of the rel32 field so it can be patched.
    std::size_t jmpRel32() { u8(0xe9); std::size_t at = here(); u32(0); return at; }
    std::size_t jccRel32(std::uint8_t cc) { u8(0x0f); u8(cc); std::size_t at = here(); u32(0); return at; }
    std::size_t callRel32() { u8(0xe8); std::size_t at = here(); u32(0); return at; }
    void patchRel32(std::size_t at, std::size_t target) {
        const std::int32_t delta = static_cast<std::int32_t>(
            static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + 4));
        std::memcpy(code.data() + at, &delta, 4);
    }

    // --- SSE scalar double -------------------------------------------------
    void sseRex(unsigned reg, unsigned rm, bool wide = false) {
        const std::uint8_t rex = static_cast<std::uint8_t>((wide ? 0x48 : 0x40) |
                                                           (((reg >> 3) & 1) << 2) | ((rm >> 3) & 1));
        if (rex != 0x40) u8(rex);
    }
    void movsdRegMem(unsigned xmm, std::int32_t disp) {
        u8(0xf2); sseRex(xmm, 5); u8(0x0f); u8(0x10); modrmRbp(xmm, disp);
    }
    void movsdMemReg(std::int32_t disp, unsigned xmm) {
        u8(0xf2); sseRex(xmm, 5); u8(0x0f); u8(0x11); modrmRbp(xmm, disp);
    }
    void sseArith(std::uint8_t op, unsigned dst, unsigned src) {
        u8(0xf2); sseRex(dst, src); u8(0x0f); u8(op); modrmReg(dst, src);
    }
    void addsd(unsigned d, unsigned s) { sseArith(0x58, d, s); }
    void subsd(unsigned d, unsigned s) { sseArith(0x5c, d, s); }
    void mulsd(unsigned d, unsigned s) { sseArith(0x59, d, s); }
    void divsd(unsigned d, unsigned s) { sseArith(0x5e, d, s); }
    void ucomisd(unsigned a, unsigned b) { u8(0x66); sseRex(a, b); u8(0x0f); u8(0x2e); modrmReg(a, b); }
    void cvtsi2sd(unsigned xmm, unsigned gpr) {
        u8(0xf2); sseRex(xmm, gpr, /*wide=*/true); u8(0x0f); u8(0x2a); modrmReg(xmm, gpr);
    }
    // movq xmm, r64 / movq r64, xmm
    void movqXmmGpr(unsigned xmm, unsigned gpr) {
        u8(0x66); sseRex(xmm, gpr, true); u8(0x0f); u8(0x6e); modrmReg(xmm, gpr);
    }
    void movqGprXmm(unsigned gpr, unsigned xmm) {
        u8(0x66); sseRex(xmm, gpr, true); u8(0x0f); u8(0x7e); modrmReg(xmm, gpr);
    }
};

// Condition codes for setcc / jcc (the low byte of the 0F 9x / 0F 8x forms).
constexpr std::uint8_t kSetE = 0x94, kSetNE = 0x95, kSetL = 0x9c, kSetLE = 0x9e,
                       kSetG = 0x9f, kSetGE = 0x9d, kSetB = 0x92, kSetBE = 0x96,
                       kSetA = 0x97, kSetAE = 0x93, kSetNP = 0x9b, kSetP = 0x9a;
constexpr std::uint8_t kJE = 0x84;
constexpr std::uint8_t kJNE = 0x85;
constexpr std::uint8_t kJNO = 0x81;
constexpr std::uint8_t kJA = 0x87;

// ---------------------------------------------------------------------------
// Function emitter
// ---------------------------------------------------------------------------

struct FunctionEmitter {
    const LirFunction& fn;
    const TargetMachine& target;
    Assembler a;
    std::string error;

    // Baseline location policy: every vreg has a frame slot. See emit.hpp for
    // why this is the right first tier.
    std::unordered_map<VReg, std::int32_t> vregOffset;
    std::unordered_map<FrameSlot, std::int32_t> slotOffset;
    std::vector<EmittedFrameLocation> frameLocations;
    std::vector<SafepointMap> safepoints;
    std::size_t frameSize{0};

    std::unordered_map<LirBlockId, std::size_t> blockOffset;
    struct Fixup { std::size_t at; LirBlockId block; };
    std::vector<Fixup> fixups;
    std::vector<EmittedFunction::Relocation> relocations;

    unsigned scratchI0{0}, scratchI1{1}, scratchI2{2};
    unsigned scratchF0{8}, scratchF1{9};

    FunctionEmitter(const LirFunction& function, const TargetMachine& machine)
        : fn(function), target(machine) {
        const auto& cc = target.cc;
        if (cc.intScratch.size() >= 3) {
            scratchI0 = cc.intScratch[0].id;
            scratchI1 = cc.intScratch[1].id;
            scratchI2 = cc.intScratch[2].id;
        }
        if (cc.floatScratch.size() >= 2) {
            scratchF0 = cc.floatScratch[0].id;
            scratchF1 = cc.floatScratch[1].id;
        }
    }

    bool fail(const std::string& message) {
        error = fn.name + ": " + message;
        return false;
    }

    void layoutFrame() {
        std::int32_t offset = 0;
        frameLocations.clear();
        for (std::size_t i = 1; i < fn.slots.size(); ++i) {
            offset -= static_cast<std::int32_t>(fn.slots[i].size == 0 ? 8 : fn.slots[i].size);
            slotOffset[fn.slots[i].id] = offset;
            frameLocations.push_back(EmittedFrameLocation{
                EmittedFrameLocationKind::FrameSlot, fn.slots[i].id, offset,
                fn.slots[i].isReference});
        }
        for (std::size_t i = 1; i < fn.vregs.size(); ++i) {
            offset -= 8;
            vregOffset[fn.vregs[i].id] = offset;
            frameLocations.push_back(EmittedFrameLocation{
                EmittedFrameLocationKind::VirtualRegister, fn.vregs[i].id, offset,
                fn.vregs[i].cls == ValueClass::Reference});
        }
        const std::size_t used = static_cast<std::size_t>(-offset);
        // A caller must keep the convention's shadow space at the top of its
        // own frame at the moment a `call` executes: a C-compiled callee may
        // legally spill into [rsp, rsp+shadowSpace). Emitted callees never
        // write there (they touch only their own frame), and this tier emits
        // no runtime calls or stack arguments yet, so the reservation is not
        // observable today - but the moment a CallRuntime callee from the C
        // world lands, a calling frame shorter than the shadow space would
        // hand that callee the caller's saved rbp to clobber. Reserve it in
        // every function that calls.
        bool hasCall = false;
        for (const auto& block : fn.blocks)
            for (const auto& ins : block.instructions)
                if (ins.opcode == LirOpcode::Call || ins.opcode == LirOpcode::CallRuntime)
                    hasCall = true;
        const std::size_t minimum = hasCall ? target.cc.shadowSpace : 0;
        frameSize = ((std::max(used, minimum) + 15) / 16) * 16;
    }

    std::int32_t offsetOf(VReg v) const { return vregOffset.at(v); }

    // Loads an operand into `reg` of the given class.
    bool loadOperand(const LirOperand& op, ValueClass cls, unsigned reg) {
        switch (op.kind) {
            case LirOperandKind::VirtualReg:
                if (cls == ValueClass::Float) a.movsdRegMem(reg, offsetOf(op.vreg));
                else a.movRegMem(reg, offsetOf(op.vreg));
                return true;
            case LirOperandKind::ImmInt:
                if (cls == ValueClass::Float) {
                    const double d = static_cast<double>(op.imm);
                    std::uint64_t bits; std::memcpy(&bits, &d, 8);
                    a.movRegImm64(scratchI2, bits);
                    a.movqXmmGpr(reg, scratchI2);
                } else {
                    a.movRegImm64(reg, static_cast<std::uint64_t>(op.imm));
                }
                return true;
            case LirOperandKind::ImmFloat: {
                std::uint64_t bits; std::memcpy(&bits, &op.fimm, 8);
                if (cls == ValueClass::Float) {
                    a.movRegImm64(scratchI2, bits);
                    a.movqXmmGpr(reg, scratchI2);
                } else {
                    a.movRegImm64(reg, bits);
                }
                return true;
            }
            case LirOperandKind::Frame:
                if (cls == ValueClass::Float) a.movsdRegMem(reg, slotOffset.at(op.slot));
                else a.movRegMem(reg, slotOffset.at(op.slot));
                return true;
            default:
                return fail("operand kind cannot be loaded into a register");
        }
    }

    void storeResult(VReg v, ValueClass cls, unsigned reg) {
        if (v == kNoVReg) return;
        if (cls == ValueClass::Float) a.movsdMemReg(offsetOf(v), reg);
        else a.movMemReg(offsetOf(v), reg);
    }

    // The language is fail-closed: the VM reports unrepresentable arithmetic
    // results as catchable ArithmeticErrors. This tier has no unwind tables
    // (docs/native-backend.md, "deliberately missing"), so the emitted code
    // cannot throw; its contract for an arithmetic fault is the one the
    // division guards already used - branch to a `ud2`, which terminates the
    // process with SIGILL rather than wrapping silently or faulting deeper
    // in the hardware.

    // OF is set (signed overflow). Call after add/sub/mul/neg, whose two-
    // operand forms set OF exactly when the signed result is unrepresentable.
    void trapOnOverflowFlag() {
        const std::size_t ok = a.jccRel32(kJNO);
        a.ud2();
        a.patchRel32(ok, a.here());
    }

    // The bit pattern of a double is in scratchI0 and must survive this
    // check - FNeg's sign-flipped result lives there until it is written back
    // to xmm - so the mask goes into scratchI1 (free after the xor, or
    // unused) and the and-compare runs on scratchI2. A double is non-finite
    // (infinity or NaN) exactly when its exponent field (bits 62..52) is all
    // ones. The jump-over is what keeps the finite path out of the trap -
    // the trap sits in the fall-through, so a jump-TO the trap here would
    // send every finite value into it. Returns the trap's position so a
    // second check can branch to the same `ud2`.
    std::size_t trapOnNonFiniteInGpr() {
        a.movRegReg(scratchI2, scratchI0);
        a.movRegImm64(scratchI1, 0x7FF0000000000000ull);
        a.andRegReg(scratchI2, scratchI1);
        a.cmpRegReg(scratchI2, scratchI1);
        const std::size_t ok = a.jccRel32(kJNE);
        const std::size_t trapAt = a.here();
        a.ud2();
        a.patchRel32(ok, a.here());
        return trapAt;
    }

    bool emitIntCompare(const LirInstruction& ins, std::uint8_t cc) {
        if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
        if (!loadOperand(ins.operands[1], ValueClass::Integer, scratchI1)) return false;
        a.cmpRegReg(scratchI0, scratchI1);
        a.setcc(cc, scratchI0);
        a.movzxReg8(scratchI0);
        storeResult(ins.result, ValueClass::Integer, scratchI0);
        return true;
    }

    // Ordered float comparison. ucomisd sets CF/ZF/PF; an unordered operand
    // (NaN) sets all three, so the "less" forms must additionally require
    // PF=0. The "greater" forms need no extra test because CF=1 already makes
    // seta/setae false. Equality must exclude unordered; inequality must
    // include it - which is exactly IEEE-754, and the reason this is spelled
    // out rather than reduced to one setcc.
    bool emitFloatCompare(const LirInstruction& ins, LirOpcode op) {
        if (!loadOperand(ins.operands[0], ValueClass::Float, scratchF0)) return false;
        if (!loadOperand(ins.operands[1], ValueClass::Float, scratchF1)) return false;
        a.ucomisd(scratchF0, scratchF1);
        switch (op) {
            case LirOpcode::FCmpEq:
                a.setcc(kSetE, scratchI0); a.movzxReg8(scratchI0);
                a.setcc(kSetNP, scratchI1); a.movzxReg8(scratchI1);
                a.andRegReg(scratchI0, scratchI1);
                break;
            case LirOpcode::FCmpNe:
                a.setcc(kSetNE, scratchI0); a.movzxReg8(scratchI0);
                a.setcc(kSetP, scratchI1); a.movzxReg8(scratchI1);
                a.orRegReg(scratchI0, scratchI1);
                break;
            case LirOpcode::FCmpLt:
                a.setcc(kSetB, scratchI0); a.movzxReg8(scratchI0);
                a.setcc(kSetNP, scratchI1); a.movzxReg8(scratchI1);
                a.andRegReg(scratchI0, scratchI1);
                break;
            case LirOpcode::FCmpLe:
                a.setcc(kSetBE, scratchI0); a.movzxReg8(scratchI0);
                a.setcc(kSetNP, scratchI1); a.movzxReg8(scratchI1);
                a.andRegReg(scratchI0, scratchI1);
                break;
            case LirOpcode::FCmpGt: a.setcc(kSetA, scratchI0); a.movzxReg8(scratchI0); break;
            case LirOpcode::FCmpGe: a.setcc(kSetAE, scratchI0); a.movzxReg8(scratchI0); break;
            default: return fail("not a float comparison");
        }
        storeResult(ins.result, ValueClass::Integer, scratchI0);
        return true;
    }

    bool emitInstruction(const LirInstruction& ins) {
        switch (ins.opcode) {
            case LirOpcode::Nop: return true;

            case LirOpcode::Move:
            case LirOpcode::LoadImmI:
            case LirOpcode::LoadImmF: {
                const ValueClass cls = ins.resultClass;
                const unsigned reg = cls == ValueClass::Float ? scratchF0 : scratchI0;
                if (!loadOperand(ins.operands[0], cls, reg)) return false;
                storeResult(ins.result, cls, reg);
                return true;
            }

            case LirOpcode::IAdd: case LirOpcode::ISub: case LirOpcode::IMul:
            case LirOpcode::IAnd: case LirOpcode::IOr: case LirOpcode::IXor: {
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                if (!loadOperand(ins.operands[1], ValueClass::Integer, scratchI1)) return false;
                switch (ins.opcode) {
                    case LirOpcode::IAdd: a.addRegReg(scratchI0, scratchI1); break;
                    case LirOpcode::ISub: a.subRegReg(scratchI0, scratchI1); break;
                    case LirOpcode::IMul: a.imulRegReg(scratchI0, scratchI1); break;
                    case LirOpcode::IAnd: a.andRegReg(scratchI0, scratchI1); break;
                    case LirOpcode::IOr:  a.orRegReg(scratchI0, scratchI1); break;
                    default:              a.xorRegReg(scratchI0, scratchI1); break;
                }
                // The VM throws a catchable ArithmeticError when +, - or *
                // produces an unrepresentable result; the bitwise ops cannot
                // overflow, so only the three arithmetic forms get the trap.
                // Plain add/sub/imul would wrap, which is not the language's
                // meaning.
                if (ins.opcode == LirOpcode::IAdd || ins.opcode == LirOpcode::ISub ||
                    ins.opcode == LirOpcode::IMul)
                    trapOnOverflowFlag();
                storeResult(ins.result, ValueClass::Integer, scratchI0);
                return true;
            }

            case LirOpcode::IDiv:
            case LirOpcode::IMod: {
                // The divisor must be in a register that is not rdx (cqo
                // clobbers it), and idiv must not execute in two cases the
                // hardware would trap on: a zero divisor (#DE), and
                // INT64_MIN / -1, whose quotient does not fit in int64
                // (overflow #DE). The VM reports both as catchable
                // ArithmeticErrors; this tier has no unwind tables, so both
                // get the trap. INT64_MIN % -1 is the exception: the VM
                // defines it as 0, so it is computed, not trapped.
                if (!loadOperand(ins.operands[1], ValueClass::Integer, scratchI1)) return false;
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.testRegReg(scratchI1, scratchI1);
                const std::size_t jz = a.jccRel32(kJE);             // divisor == 0 -> trap
                a.cmpRegImm32(scratchI1, -1);
                const std::size_t jneDivisor = a.jccRel32(kJNE);    // divisor != -1 -> divide
                a.movRegImm64(scratchI2, 0x8000000000000000ull);
                a.cmpRegReg(scratchI0, scratchI2);
                const std::size_t jneDividend = a.jccRel32(kJNE);   // dividend != INT64_MIN -> divide
                if (ins.opcode == LirOpcode::IMod) {
                    // INT64_MIN % -1 == 0, as the VM defines it.
                    a.movRegImm64(scratchI0, 0);
                    storeResult(ins.result, ValueClass::Integer, scratchI0);
                    const std::size_t skip = a.jmpRel32();          // -> done
                    const std::size_t trapAt = a.here();
                    a.ud2();                                        // .trap: zero divisor
                    a.patchRel32(jz, trapAt);
                    const std::size_t divideAt = a.here();
                    a.patchRel32(jneDivisor, divideAt);
                    a.patchRel32(jneDividend, divideAt);
                    a.cqo();
                    a.idivReg(scratchI1);
                    // Remainder in rdx.
                    storeResult(ins.result, ValueClass::Integer, 2u);
                    const std::size_t skip2 = a.jmpRel32();         // -> done
                    a.patchRel32(skip, a.here());
                    a.patchRel32(skip2, a.here());
                    return true;
                }
                // INT64_MIN / -1: overflow. The fall-through lands in the
                // trap, which the zero divisor reaches the same way.
                const std::size_t trapAt = a.here();
                a.ud2();                                            // .trap
                a.patchRel32(jz, trapAt);
                const std::size_t divideAt = a.here();
                a.patchRel32(jneDivisor, divideAt);
                a.patchRel32(jneDividend, divideAt);
                a.cqo();
                a.idivReg(scratchI1);
                // Quotient in rax.
                storeResult(ins.result, ValueClass::Integer, 0u);
                const std::size_t skip = a.jmpRel32();              // -> done
                a.patchRel32(skip, a.here());
                return true;
            }

            case LirOpcode::INeg: {
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.negReg(scratchI0);
                // OF is set exactly when the operand was INT64_MIN - the one
                // negation the VM reports as an overflow error.
                trapOnOverflowFlag();
                storeResult(ins.result, ValueClass::Integer, scratchI0);
                return true;
            }
            case LirOpcode::IBitNot: {
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.notReg(scratchI0);
                storeResult(ins.result, ValueClass::Integer, scratchI0);
                return true;
            }
            case LirOpcode::INot: {
                // Logical complement of a 0/1 bool: x ^ 1.
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.movRegImm64(scratchI1, 1);
                a.xorRegReg(scratchI0, scratchI1);
                storeResult(ins.result, ValueClass::Integer, scratchI0);
                return true;
            }

            case LirOpcode::IShl: case LirOpcode::IShr: case LirOpcode::IUshr: {
                // The shift count must be in cl, which is scratchI1's low byte
                // on both x86-64 conventions described here.
                if (scratchI1 != 1) return fail("shift needs rcx as the second scratch register");
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                if (!loadOperand(ins.operands[1], ValueClass::Integer, scratchI1)) return false;
                // The VM rejects counts outside 0..63; x86 would silently
                // mask the count to its low six bits, so `1 << 64` would
                // compute `1 << 0` - not the language's meaning. One unsigned
                // compare covers both ends: a negative count is a huge
                // unsigned number.
                a.cmpRegImm32(scratchI1, 63);
                const std::size_t ja = a.jccRel32(kJA);
                const unsigned ext = ins.opcode == LirOpcode::IShl ? 4u
                                   : ins.opcode == LirOpcode::IShr ? 7u // arithmetic: ZL `>>` is signed
                                                                   : 5u; // logical: `>>>`
                a.shiftByCl(ext, scratchI0);
                storeResult(ins.result, ValueClass::Integer, scratchI0);
                // The invalid path jumps forward to the trap, so the valid
                // path must jump over it - the trap sits between the check
                // and the return code the emitter is about to append.
                const std::size_t skip = a.jmpRel32();
                const std::size_t trapAt = a.here();
                a.ud2();
                a.patchRel32(ja, trapAt);
                a.patchRel32(skip, a.here());
                return true;
            }

            case LirOpcode::FAdd: case LirOpcode::FSub: case LirOpcode::FMul: case LirOpcode::FDiv: {
                if (!loadOperand(ins.operands[0], ValueClass::Float, scratchF0)) return false;
                if (!loadOperand(ins.operands[1], ValueClass::Float, scratchF1)) return false;
                std::size_t zeroDivisorJe = 0;
                if (ins.opcode == LirOpcode::FDiv) {
                    // The VM throws on a zero divisor before dividing. IEEE
                    // would instead produce an infinity, which the language
                    // has no room for - so trap on the zero bit pattern
                    // (both -0.0 and +0.0 are all-zero bits).
                    a.movqGprXmm(scratchI0, scratchF1);
                    a.testRegReg(scratchI0, scratchI0);
                    zeroDivisorJe = a.jccRel32(kJE);
                }
                switch (ins.opcode) {
                    case LirOpcode::FAdd: a.addsd(scratchF0, scratchF1); break;
                    case LirOpcode::FSub: a.subsd(scratchF0, scratchF1); break;
                    case LirOpcode::FMul: a.mulsd(scratchF0, scratchF1); break;
                    default:              a.divsd(scratchF0, scratchF1); break;
                }
                // The VM throws when a floating-point result is not finite
                // (overflow to infinity, NaN from a domain error). Check the
                // exponent field before the result reaches a frame slot.
                a.movqGprXmm(scratchI0, scratchF0);
                const std::size_t trapAt = trapOnNonFiniteInGpr();
                if (ins.opcode == LirOpcode::FDiv)
                    a.patchRel32(zeroDivisorJe, trapAt);
                storeResult(ins.result, ValueClass::Float, scratchF0);
                return true;
            }
            case LirOpcode::FNeg: {
                // Flip the sign bit through a GPR: no constant pool needed,
                // and it is exact for zeros, infinities and NaNs alike.
                if (!loadOperand(ins.operands[0], ValueClass::Float, scratchF0)) return false;
                a.movqGprXmm(scratchI0, scratchF0);
                a.movRegImm64(scratchI1, 0x8000000000000000ull);
                a.xorRegReg(scratchI0, scratchI1);
                // The tier's invariant: a non-finite double never exists,
                // because every producer traps. A non-finite input would
                // break it, so the flipped pattern is checked before it
                // leaves the GPR - the corner the VM does not need, because
                // its values cannot be non-finite either.
                trapOnNonFiniteInGpr();
                a.movqXmmGpr(scratchF0, scratchI0);
                storeResult(ins.result, ValueClass::Float, scratchF0);
                return true;
            }
            case LirOpcode::IntToFloat: {
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.cvtsi2sd(scratchF0, scratchI0);
                storeResult(ins.result, ValueClass::Float, scratchF0);
                return true;
            }

            case LirOpcode::ICmpEq: return emitIntCompare(ins, kSetE);
            case LirOpcode::ICmpNe: return emitIntCompare(ins, kSetNE);
            case LirOpcode::ICmpLt: return emitIntCompare(ins, kSetL);
            case LirOpcode::ICmpLe: return emitIntCompare(ins, kSetLE);
            case LirOpcode::ICmpGt: return emitIntCompare(ins, kSetG);
            case LirOpcode::ICmpGe: return emitIntCompare(ins, kSetGE);
            case LirOpcode::FCmpEq: case LirOpcode::FCmpNe: case LirOpcode::FCmpLt:
            case LirOpcode::FCmpLe: case LirOpcode::FCmpGt: case LirOpcode::FCmpGe:
                return emitFloatCompare(ins, ins.opcode);

            case LirOpcode::Load: {
                const FrameSlot slot = ins.operands[0].slot;
                const ValueClass cls = fn.slots[slot].cls;
                const unsigned reg = cls == ValueClass::Float ? scratchF0 : scratchI0;
                if (cls == ValueClass::Float) a.movsdRegMem(reg, slotOffset.at(slot));
                else a.movRegMem(reg, slotOffset.at(slot));
                storeResult(ins.result, cls, reg);
                return true;
            }
            case LirOpcode::Store: {
                const FrameSlot slot = ins.operands[0].slot;
                const ValueClass cls = fn.slots[slot].cls;
                const unsigned reg = cls == ValueClass::Float ? scratchF0 : scratchI0;
                if (!loadOperand(ins.operands[1], cls, reg)) return false;
                if (cls == ValueClass::Float) a.movsdMemReg(slotOffset.at(slot), reg);
                else a.movMemReg(slotOffset.at(slot), reg);
                return true;
            }

            case LirOpcode::Call:
            case LirOpcode::CallRuntime:
                return emitCall(ins);

            case LirOpcode::Jump: {
                fixups.push_back({a.jmpRel32(), ins.operands[0].block});
                return true;
            }
            case LirOpcode::BranchIf: {
                if (!loadOperand(ins.operands[0], ValueClass::Integer, scratchI0)) return false;
                a.testRegReg(scratchI0, scratchI0);
                fixups.push_back({a.jccRel32(kJE), ins.operands[2].block}); // false edge
                fixups.push_back({a.jmpRel32(), ins.operands[1].block});    // true edge
                return true;
            }
            case LirOpcode::Return: {
                if (!ins.operands.empty()) {
                    const ValueClass cls = fn.returnClass;
                    const unsigned reg = cls == ValueClass::Float ? target.cc.floatReturnReg.id
                                                                  : target.cc.intReturnReg.id;
                    if (!loadOperand(ins.operands[0], cls, reg)) return false;
                }
                a.movRspRbp();
                a.popRbp();
                a.ret();
                return true;
            }
            case LirOpcode::Unreachable:
                a.ud2();
                return true;

            case LirOpcode::FrameAddr:
                return fail("frameaddr is declared but not selected by this tier");
        }
        return fail(std::string("unencodable instruction '") + lirOpcodeName(ins.opcode) + "'");
    }

    bool emitCall(const LirInstruction& ins) {
        const auto& cc = target.cc;
        std::size_t intUsed = 0, floatUsed = 0;
        // Arguments come out of frame slots, so there is no clobber hazard
        // between them: each load writes exactly one argument register and
        // reads only memory.
        for (std::size_t i = 0; i < ins.operands.size(); ++i) {
            const ValueClass cls = i < ins.argClasses.size() ? ins.argClasses[i] : ValueClass::Integer;
            if (cls == ValueClass::Float) {
                if (floatUsed >= cc.floatArgRegs.size()) return fail("too many float arguments");
                if (!loadOperand(ins.operands[i], cls, cc.floatArgRegs[floatUsed++].id)) return false;
            } else {
                if (intUsed >= cc.intArgRegs.size()) return fail("too many integer arguments");
                if (!loadOperand(ins.operands[i], cls, cc.intArgRegs[intUsed++].id)) return false;
            }
        }
        const std::size_t callOffset = a.here();
        const std::size_t at = a.callRel32();
        relocations.push_back(EmittedFunction::Relocation{
            at, ins.callee, ins.opcode == LirOpcode::CallRuntime});
        SafepointMap safepoint;
        safepoint.callOffset = callOffset;
        for (const auto& location : frameLocations)
            if (location.isReference) safepoint.referenceOffsets.push_back(location.offset);
        safepoints.push_back(std::move(safepoint));
        if (ins.result != kNoVReg) {
            const unsigned reg = ins.resultClass == ValueClass::Float ? cc.floatReturnReg.id
                                                                      : cc.intReturnReg.id;
            storeResult(ins.result, ins.resultClass, reg);
        }
        return true;
    }

    bool emit() {
        layoutFrame();

        a.pushRbp();
        a.movRbpRsp();
        if (frameSize != 0) a.subRspImm32(static_cast<std::uint32_t>(frameSize));

        // A conservative safepoint map includes every reference-bearing
        // location, including spill slots whose value is not defined yet.
        // Initialize those locations to null so a future collector never
        // interprets uninitialized stack bytes as a managed pointer.
        for (const auto& location : frameLocations) {
            if (!location.isReference) continue;
            a.movRegImm64(scratchI0, 0);
            a.movMemReg(location.offset, scratchI0);
        }

        // Spill the incoming arguments into their vregs' frame slots. This is
        // the prologue half of the calling convention: after it, no value is
        // in a register the rest of the body has to keep track of.
        for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
            const VReg v = fn.parameters[i];
            const VRegInfo* info = fn.vreg(v);
            if (info == nullptr || !info->fixed.has_value()) return fail("parameter has no ABI register");
            if (info->cls == ValueClass::Float) a.movsdMemReg(offsetOf(v), info->fixed->id);
            else a.movMemReg(offsetOf(v), info->fixed->id);
        }

        for (const auto& block : fn.blocks) {
            blockOffset[block.id] = a.here();
            for (const auto& ins : block.instructions)
                if (!emitInstruction(ins)) return false;
        }

        for (const auto& fixup : fixups) {
            auto it = blockOffset.find(fixup.block);
            if (it == blockOffset.end()) return fail("branch to an unknown block");
            a.patchRel32(fixup.at, it->second);
        }
        return true;
    }
};

} // namespace

EmitResult emitModule(const LirModule& module, const TargetMachine& target) {
    EmitResult result;
    if (!target.encoderAvailable()) {
        result.error = std::string("no machine-code encoder for target '") + target.triple + "'";
        return result;
    }

    for (const auto& fn : module.functions) {
        FunctionEmitter emitter(fn, target);
        if (!emitter.emit()) {
            result.error = emitter.error;
            result.functions.clear();
            return result;
        }
        EmittedFunction out;
        out.name = fn.name;
        out.code = std::move(emitter.a.code);
        out.frameSize = emitter.frameSize;
        out.frameLocations = std::move(emitter.frameLocations);
        out.safepoints = std::move(emitter.safepoints);
        out.parameterCount = fn.parameters.size();
        out.returnClass = fn.returnClass;
        out.relocations = std::move(emitter.relocations);
        result.functions.push_back(std::move(out));
    }
    result.success = true;
    return result;
}

} // namespace zl::native
