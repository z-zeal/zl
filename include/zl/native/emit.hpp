#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/native/lir.hpp"
#include "zl/native/target.hpp"

// ---------------------------------------------------------------------------
// Native IR -> machine code
// ---------------------------------------------------------------------------
//
// The last arrow of the pipeline, and the only file allowed to know what a
// byte of x86-64 looks like. Everything it needs about the target arrives in a
// `TargetMachine`; everything it needs about the program arrives in a
// `LirFunction`.
//
// Register assignment here is deliberately the simplest correct policy: every
// virtual register gets a frame slot, and each instruction loads its operands
// into the convention's scratch registers, computes, and stores its result
// back. That is slow code, and it is *correct* code under every control-flow
// shape without a liveness analysis, which is the right trade for the first
// native tier: a real allocator is an optimisation over a working baseline,
// whereas a half-right allocator is a miscompile. The frame layout, the
// argument-register handling, and the encoder are all what a linear-scan pass
// would reuse unchanged - it would only change which operand lives where.
//
// Code is emitted position-independent within a function: branches are
// resolved by a relocation pass over the emitted buffer, so blocks may be laid
// out in any order and forward edges cost nothing.

namespace zl::native {

enum class EmittedFrameLocationKind : std::uint8_t { FrameSlot, VirtualRegister };

struct EmittedFrameLocation {
    EmittedFrameLocationKind kind{EmittedFrameLocationKind::FrameSlot};
    std::uint32_t id{0};
    std::int32_t offset{0}; // negative displacement from rbp
    bool isReference{false};
};

struct SafepointMap {
    // Byte offset of the CALL opcode itself (not its relocation field).
    std::size_t callOffset{0};
    // Conservative roots: every reference-bearing location in this frame.
    std::vector<std::int32_t> referenceOffsets;
};

struct EmittedFunction {
    std::string name;
    std::vector<std::uint8_t> code;
    std::size_t frameSize{0};
    // Target-specific layout for all LIR slots and vreg spill slots. Keeping
    // both kinds explicit lets safepoint maps describe every root location.
    std::vector<EmittedFrameLocation> frameLocations;
    // One conservative map per emitted call instruction, keyed by code offset.
    std::vector<SafepointMap> safepoints;
    std::size_t parameterCount{0};
    ValueClass returnClass{ValueClass::Void};
    // Byte offsets of call sites that need a symbol bound before execution,
    // paired with the symbol. The caller (a JIT or an object writer) resolves
    // them; the emitter never invents an address.
    struct Relocation {
        std::size_t offset{0};   // offset of the rel32 field
        std::string symbol;      // callee name
        bool isRuntimeCall{false};
    };
    std::vector<Relocation> relocations;
};

struct EmitResult {
    bool success{false};
    std::vector<EmittedFunction> functions;
    std::string error;
};

// Emits machine code for every function of a LIR module.
//
// Fails - rather than emitting anything - when the target has no encoder
// (`TargetMachine::encoderAvailable()` is false) or when an instruction cannot
// be encoded. The failure names the function and the instruction.
[[nodiscard]] EmitResult emitModule(const LirModule& module, const TargetMachine& target);

} // namespace zl::native
