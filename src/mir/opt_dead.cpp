// Dead-block elimination and dead-value elimination.
//
// These are the only passes in the framework that delete from a *function*,
// and the only ones for which "delete" has to be argued rather than assumed.
// Both rest on the effect classification in `effects.hpp`: an instruction is
// removed only when the classification says it does nothing but compute, and a
// block is removed only when no edge - normal *or* unwind - can reach it.
//
// Two things these passes deliberately do not do:
//
//   * no dead *store* is removed unless the slot is never read anywhere in the
//     function. Removing a store whose slot is read on some other path would
//     need an inter-block reaching-definition proof this framework does not
//     claim to have, and an unproven one is a wrong value, not a missed
//     optimisation;
//   * no function is removed. These passes are function-local, and "nobody
//     calls this" is not something a function-local analysis can establish.
//     Function removal is the module pass `eliminate-dead-functions`
//     (opt_dead_functions.cpp), which performs it only under the reachability
//     report's completeness proof - reflection and unpinned function values
//     included - and never from inside a function pass.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/dataflow.hpp"
#include "zl/mir/passes.hpp"

namespace zl::mir {
namespace {

// Which opcodes read and write a slot. Slots are memory, so "reads" and
// "writes" are properties of the opcode rather than of the operand list, and a
// `move`/`drop`/`end_borrow` does both: it reads the cell and leaves it
// unusable.
bool slotReadBy(Opcode opcode) {
    switch (opcode) {
        case Opcode::Load:
        case Opcode::Move:
        case Opcode::Drop:
        case Opcode::EndBorrow: return true;
        default: return false;
    }
}

bool slotWrittenBy(Opcode opcode) {
    switch (opcode) {
        case Opcode::Store:
        case Opcode::Move:
        case Opcode::Borrow:
        case Opcode::EndBorrow:
        case Opcode::Drop: return true;
        default: return false;
    }
}

class DeadBlockEliminationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "eliminate-dead-blocks"; }
    [[nodiscard]] std::string description() const override {
        return "removes blocks no edge can enter, counting exception edges: a catch "
               "block is live even though nothing falls through to it";
    }

    bool runOnFunction(Module&, Function& function, FunctionAnalysisManager& analyses) override {
        const std::size_t before = function.blocks.size();
        // `pruneUnreachableBlocks` follows unwind edges as well as normal ones,
        // renumbers the survivors, rewrites every reference to them and rebuilds
        // the edge list - the whole job, in the one place that already owns it.
        const std::size_t removed = pruneUnreachableBlocks(function);
        if (removed == 0) return false;
        analyses.invalidate(function);
        note_ = "removed " + std::to_string(removed) + " dead block(s) of " + std::to_string(before);
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;
};

class DeadValueEliminationPass final : public FunctionPass {
public:
    [[nodiscard]] std::string name() const override { return "eliminate-dead-values"; }
    [[nodiscard]] std::string description() const override {
        return "deletes an instruction whose result nothing reads and whose effect "
               "set allows deletion, and a store to a slot no path reads; iterates, "
               "because removing one instruction can orphan another";
    }

    bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) override {
        std::size_t instructionsRemoved = 0;
        std::size_t storesRemoved = 0;

        // Removing an instruction can make its operands unused, which can make
        // *their* definitions removable. The lattice only ever shrinks, so this
        // terminates; the bound is a guard, not the stopping condition.
        for (std::size_t round = 0; round < 64; ++round) {
            bool progress = false;

            // --- unread results --------------------------------------------
            // Def-use is rebuilt each round because the previous round changed
            // the function and a stale use list is exactly what would make this
            // delete a live instruction.
            const DefUseInfo defUse(function);
            for (auto& block : function.blocks) {
                auto& instructions = block.instructions;
                const auto removed = std::remove_if(
                    instructions.begin(), instructions.end(), [&](const Instruction& instruction) {
                        if (instruction.result == kNoTemp) return false;
                        if (defUse.isUsed(tempValue(instruction.result))) return false;
                        // The whole safety argument, in one call: an
                        // instruction is deleted only when it computes or reads
                        // and nothing else - and, crucially, only when it
                        // cannot raise. A dead `div` by a value that might be
                        // zero is not dead: it is an error the program raises.
                        return isRemovableIfResultUnused(module, instruction);
                    });
                if (removed != instructions.end()) {
                    const std::size_t count = static_cast<std::size_t>(std::distance(removed, instructions.end()));
                    instructions.erase(removed, instructions.end());
                    instructionsRemoved += count;
                    progress = true;
                }
            }

            // --- stores to a slot nothing reads -----------------------------
            std::unordered_set<SlotId> readSlots;
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.slot != 0 && slotReadBy(instruction.opcode))
                        readSlots.insert(instruction.slot);
                }
            }
            for (auto& block : function.blocks) {
                auto& instructions = block.instructions;
                const auto removed = std::remove_if(
                    instructions.begin(), instructions.end(), [&](const Instruction& instruction) {
                        if (instruction.opcode != Opcode::Store || instruction.slot == 0) return false;
                        if (readSlots.count(instruction.slot) != 0) return false;
                        return storeIsRemovable(function, instruction.slot);
                    });
                if (removed != instructions.end()) {
                    const std::size_t count = static_cast<std::size_t>(std::distance(removed, instructions.end()));
                    instructions.erase(removed, instructions.end());
                    storesRemoved += count;
                    progress = true;
                }
            }

            if (!progress) break;
        }

        if (instructionsRemoved == 0 && storesRemoved == 0) return false;

        analyses.invalidate(function);
        note_ = "deleted " + std::to_string(instructionsRemoved) + " dead instruction(s) and " +
                std::to_string(storesRemoved) + " dead store(s)";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    // A store to a slot no path reads is removable only for the plainest kind
    // of slot. The exclusions are the point, and the last one is the one that
    // costs real work to state:
    //
    //   * a catch binding is written by the runtime when an exception arrives,
    //     outside the CFG this analysis walks;
    //   * an owned or borrowed slot carries a lifetime contract - the store is
    //     where a value enters storage whose release has to happen, and the
    //     release is an ownership event, not a read;
    //   * a closure body's slots are the closure's captured environment, not
    //     this invocation's private locals. ZL captures by value, and a
    //     captured `var` that a closure mutates persists between calls of that
    //     closure - `makeCounter` in examples/intermediate/Closures.zl is
    //     exactly that program. "No read in this function" therefore does not
    //     mean "no effect": the next invocation reads it. How the runtime
    //     spells that (copy in on entry, copy out on return) is a backend
    //     contract this pass does not restate, so no store in a function with
    //     captures is removed at all.
    static bool storeIsRemovable(const Function& function, SlotId slot) {
        if (!function.captures.empty()) return false;
        const Slot* declaration = function.slot(slot);
        if (!declaration) return false;
        if (declaration->isCatchBinding) return false;
        if (declaration->ownership != zl::OwnershipKind::GC) return false;
        return true;
    }

    std::string note_;
};

} // namespace

std::unique_ptr<FunctionPass> createDeadBlockEliminationPass() {
    return std::unique_ptr<FunctionPass>(new DeadBlockEliminationPass());
}

std::unique_ptr<FunctionPass> createDeadValueEliminationPass() {
    return std::unique_ptr<FunctionPass>(new DeadValueEliminationPass());
}

} // namespace zl::mir
