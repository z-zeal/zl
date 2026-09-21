// Whole-module dead-function elimination - the module pass.
//
// This is the only place in the optimiser that removes functions, and it does
// so under exactly one condition: the reachability report for the module is
// *complete*. `ReachabilityReport::complete()` is true when the module has an
// entry point, no reachable function touches a native of the reflection family
// (natives that enter code by name, or read the function table as data), and
// every reachable call through a function value is pinned to a single closure
// body. Those three facts are what turn "nothing calls this"
// from an absence of evidence into evidence of absence: with the call graph
// closed, a function outside the reachable set has no way in, and removing it
// is a size win, not a miscompile. When the report is incomplete the pass is
// a no-op and says why in its note.
//
// The keep set is the report's reachable set, widened by two things the report
// itself does not need to model:
//
//   * every static field's initializer, referenced or not - reflection can
//     read `Class.field` by name, and reading a lazy static runs its
//     initializer, so an "unreferenced" static's body is reachable through the
//     very feature the analysis refuses to chase;
//   * every function a kept instruction names in `target.function`, closed
//     transitively - if an opcode ever carries a callee reference the analysis
//     does not treat as an edge, this pass stays consistent with the verifier
//     rather than with any enumeration of edge kinds.
//
// Removal renumbers the survivors (function ids are positional), and rewrites
// every id the removal invalidates: `entryPoint`, each `StaticField::initializer`,
// every kept instruction's `target.function`, and each `Function::id`. Classes,
// interfaces, statics and constants are name-keyed or append-only, so they
// need no rewriting and none is done. If any rewrite cannot be explained by
// the keep set - a reference to something about to be deleted, an out-of-range
// id - the pass commits nothing and says so; a missed removal is a bigger
// binary, a wrong one is a crash, and this framework refuses the second kind
// (rule 4 of `passes.hpp`).

#include <cstddef>
#include <string>
#include <vector>

#include "zl/mir/function.hpp"
#include "zl/mir/passes.hpp"
#include "zl/mir/reachability.hpp"

namespace zl::mir {
namespace {

class EliminateDeadFunctionsPass final : public ModulePass {
public:
    [[nodiscard]] std::string name() const override { return "eliminate-dead-functions"; }
    [[nodiscard]] std::string description() const override {
        return "deletes functions the reachability report proves unreachable; a no-op "
               "unless the report is complete (entry point, no reachable reflection native, every "
               "function-value call pinned to one closure)";
    }

    bool runOnModule(Module& module, FunctionAnalysisManager& analyses) override {
        const std::size_t total = module.functions.size();
        if (total == 0) {
            note_ = "empty module";
            return false;
        }

        // The licence to delete anything at all. Keep this guard in sync with
        // the contract in `reachability.hpp`: `functions` is only a removal
        // guarantee when `complete()` holds; otherwise it is a lower bound.
        const ReachabilityReport report = reachableFunctions(module);
        if (!report.complete()) {
            note_ = "kept all " + std::to_string(total) + " function(s): " + refusalReason(report);
            return false;
        }

        // The keep set: reachable functions, the entry point, and every
        // static initializer (see file header for why statics count even when
        // nothing references them).
        std::vector<bool> keep(total + 1, false);
        for (FunctionId id : report.functions) {
            if (id >= 1 && id <= total) keep[id] = true;
        }
        if (module.entryPoint >= 1 && module.entryPoint <= total) keep[module.entryPoint] = true;
        for (const StaticField& field : module.statics) {
            if (field.initializer >= 1 && field.initializer <= total) keep[field.initializer] = true;
        }

        // Transitive closure over instruction-level callee references. The
        // analysis already walks call edges, so this normally adds nothing;
        // what it guarantees is that no *kept* instruction is left naming a
        // deleted function, whatever opcode carries that field.
        std::vector<FunctionId> worklist;
        for (std::size_t id = 1; id <= total; ++id) {
            if (keep[id]) worklist.push_back(static_cast<FunctionId>(id));
        }
        for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
            const Function* function = module.function(worklist[cursor]);
            if (!function) return refuse(total, "the module references a nonexistent function id");
            for (const BasicBlock& block : function->blocks) {
                for (const Instruction& instruction : block.instructions) {
                    const FunctionId callee = instruction.target.function;
                    if (callee == kNoFunction) continue;
                    if (callee > total) return refuse(total, "an instruction references a nonexistent function id");
                    if (!keep[callee]) {
                        keep[callee] = true;
                        worklist.push_back(callee);
                    }
                }
            }
        }

        std::size_t keptCount = 0;
        for (std::size_t id = 1; id <= total; ++id) keptCount += keep[id] ? 1 : 0;
        if (keptCount == total) {
            note_ = "kept all " + std::to_string(total) + " function(s): every one is reachable";
            return false;
        }

        // Plan first, commit once: the loop below builds the replacement
        // vector and its id map; `module.functions` is touched only at the
        // swap, so any refusal during the rewrite leaves the module exactly
        // as it was found.
        std::vector<FunctionId> newId(total + 1, kNoFunction);
        std::vector<Function> kept;
        kept.reserve(keptCount);
        std::size_t removedInstructions = 0;
        std::string removedNames;
        std::size_t removedCount = 0;
        std::size_t namesShown = 0;
        for (Function& function : module.functions) {
            if (keep[function.id]) {
                newId[function.id] = static_cast<FunctionId>(kept.size() + 1);
                kept.push_back(std::move(function));
                continue;
            }
            ++removedCount;
            removedInstructions += countInstructions(function);
            if (namesShown < 4) {
                removedNames += (removedNames.empty() ? "" : ", ") + function.name;
                ++namesShown;
            }
        }


        auto rewrite = [&](FunctionId& id, const char* what) -> bool {
            if (id == kNoFunction) return true;
            if (id > total || newId[id] == kNoFunction) {
                note_ = std::string("kept all ") + std::to_string(total) + " function(s): " + what +
                        " references a function this pass would remove";
                return false;
            }
            id = newId[id];
            return true;
        };

        for (Function& function : kept) {
            for (BasicBlock& block : function.blocks) {
                for (Instruction& instruction : block.instructions) {
                    if (!rewrite(instruction.target.function, "an instruction")) return false;
                }
            }
        }
        if (!rewrite(module.entryPoint, "the entry point")) return false;
        for (StaticField& field : module.statics) {
            if (!rewrite(field.initializer, "a static initializer")) return false;
        }
        for (std::size_t i = 0; i < kept.size(); ++i) kept[i].id = static_cast<FunctionId>(i + 1);

        module.functions.swap(kept);
        analyses.invalidateAll();

        note_ = "removed " + std::to_string(removedCount) + " unreachable function(s) of " +
                std::to_string(total) + " (" + std::to_string(removedInstructions) + " instruction(s)): " +
                removedNames;
        if (removedCount > namesShown) note_ += ", +" + std::to_string(removedCount - namesShown) + " more";
        return true;
    }

    [[nodiscard]] std::string lastNote() const override { return note_; }

private:
    std::string note_;

    bool refuse(std::size_t total, const char* why) {
        note_ = "kept all " + std::to_string(total) + " function(s): " + why;
        return false;
    }

    // The report's own words for why deletion is refused, kept short because
    // the note appears on the `--mir-opt-check` trace for every module that
    // has it.
    [[nodiscard]] static std::string refusalReason(const ReachabilityReport& report) {
        if (!report.hasEntryPoint) return "no entry point (library module)";
        if (report.dynamicEntry)
            return "reflection opens the call graph: " +
                   (report.dynamicEntryReason.empty() ? std::string("dynamic entry") : report.dynamicEntryReason);
        if (report.unresolvedCalls) {
            std::string reason = std::to_string(report.unresolvedCallCount) + " unpinned function-value call(s)";
            if (!report.unresolvedCallReason.empty()) reason += ": " + report.unresolvedCallReason;
            return reason;
        }
        return "the reachability report is not complete";
    }
};

} // namespace

std::unique_ptr<Pass> createEliminateDeadFunctionsPass() {
    return std::make_unique<EliminateDeadFunctionsPass>();
}

} // namespace zl::mir
