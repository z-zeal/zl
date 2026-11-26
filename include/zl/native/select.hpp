#pragma once

#include <string>
#include <vector>

#include "zl/mir/function.hpp"
#include "zl/native/lir.hpp"
#include "zl/native/target.hpp"

// ---------------------------------------------------------------------------
// MIR -> Native IR (instruction selection)
// ---------------------------------------------------------------------------
//
// This is the native backend's entry point, and the only place it reads MIR.
//
// Two rules define it, and they are the reason the file exists at all:
//
//   1. **It consumes verified MIR and nothing else.** No AST, no type checker,
//      no re-derivation of what an expression meant. Every fact selection acts
//      on - the class of a value, the operand order of a subtraction, which
//      block a branch goes to - is read out of the MIR the verifier accepted.
//      If a fact is not in the MIR, the answer is to put it in the MIR, not to
//      reconstruct it here.
//
//   2. **Refusal is a first-class result.** The native subset is small on
//      purpose. Everything outside it is *rejected by name*, per function, with
//      the MIR opcode and source line that caused the rejection; the caller
//      keeps running that function on the VM. A backend that guesses at an
//      operation it does not implement is worse than one that admits it, and a
//      silently mis-lowered arithmetic operation is the single hardest class of
//      bug to find later.
//
// The subset selected today, stated exactly:
//
//   * value classes Integer (ZL int, bool) and Float (ZL double);
//   * integer and double constants;
//   * add/sub/mul/div/mod/neg on both, and the integer bitwise operations;
//   * all six comparisons on both, and boolean not/and/or;
//   * int -> double widening;
//   * Load/Store of primitive slots;
//   * Jump, Branch, Return, Unreachable;
//   * direct Call to another function that is itself in the subset, plus
//     CallRuntime for explicitly routed runtime operations;
//   * allocation and named field access for non-generic data records, through
//     the GC-aware runtime bridge, with int/bool/double and nested data-record
//     fields.
//
// Everything else - general classes/constructors, strings, collections,
// closures, equality, dynamic dispatch, exceptions, async, and ownership
// events - is refused. References are never treated as integers: supported
// data-record references are tracked by the collector at native safepoints.

namespace zl::native {

// Why a function was not lowered natively.
struct SelectionRejection {
    std::string function;
    std::string reason;
    std::uint32_t line{0};
};

struct SelectionOptions {
    // Route an unsupported *call* to the runtime instead of refusing the whole
    // function. Off by default: a runtime call has an ABI the caller must
    // honour, so it is opt-in per pipeline rather than a silent fallback.
    bool allowRuntimeCalls{true};
    // Refuse a function whose MIR is flagged `incomplete` by the lowerer. On
    // by default: an incomplete translation is not a translation.
    bool requireCompleteMir{true};
};

struct SelectionResult {
    LirModule module;
    // Functions that were lowered, in module order.
    std::vector<std::string> lowered;
    // Functions that were refused, each with a reason.
    std::vector<SelectionRejection> rejected;
    // Hard errors: the module could not be processed at all.
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// Maps a MIR type to its machine value class. Returns Void for `void`, and
// Reference for every heap-managed type; a type the mapping does not
// understand also reports Reference, because "unknown" must fail closed
// towards the conservative class rather than towards an integer.
[[nodiscard]] ValueClass classifyType(const zl::mir::Module& module, std::uint32_t typeId);

// Selects native IR for every function of a verified MIR module.
//
// `module` must have passed `zl::mir::verifyModule`. Selection re-checks
// nothing the verifier already guarantees; it checks only membership of the
// native subset.
[[nodiscard]] SelectionResult selectModule(const zl::mir::Module& module,
                                           const TargetMachine& target,
                                           const SelectionOptions& options = {});

} // namespace zl::native
