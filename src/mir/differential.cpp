#include "zl/mir/differential.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/effects.hpp"
#include "zl/mir/instruction.hpp"
#include "zl/mir/passes.hpp"

namespace zl::mir {
namespace {

// A stable rendering of one observable instruction: what it does and what it
// does it to, never the values involved. Values are exactly what the optimiser
// is allowed to change (a folded constant replaces a temp), so including them
// would make every correct optimisation look like a divergence.
std::string renderEvent(const Module& module, const Function& function, const Instruction& instruction) {
    std::ostringstream out;
    out << opcodeName(instruction.opcode);

    switch (instruction.opcode) {
        case Opcode::Call:
        case Opcode::InvokeStatic:
        case Opcode::MakeClosure: {
            const Function* callee = module.function(instruction.target.function);
            out << " " << (callee ? callee->name : "<unknown-function>")
                << "(" << instruction.operands.size() << ")";
            break;
        }
        case Opcode::InvokeMethod:
        case Opcode::InvokeSuper:
            out << " " << instruction.target.className << "." << instruction.target.methodName << "("
                << instruction.operands.size() << ")";
            break;
        case Opcode::CallNative:
            out << " " << instruction.target.nativeName << "#" << instruction.target.nativeId << "("
                << instruction.operands.size() << ")";
            break;
        case Opcode::FfiCall:
            out << " " << instruction.target.ffiLibrary << ":" << instruction.target.ffiSymbol << "("
                << instruction.operands.size() << ")";
            break;
        case Opcode::CallIndirect:
        case Opcode::CallbackInvoke:
            out << "(" << instruction.operands.size() << ")";
            break;
        case Opcode::FieldLoad:
        case Opcode::FieldStore:
            out << " " << instruction.name;
            break;
        case Opcode::StaticLoad:
        case Opcode::StaticStore:
            out << " " << instruction.name;
            break;
        case Opcode::Store:
            out << " slot " << instruction.slot;
            break;
        case Opcode::Move:
        case Opcode::Borrow:
        case Opcode::EndBorrow:
        case Opcode::Drop:
            if (instruction.slot != 0) {
                const Slot* slot = function.slot(instruction.slot);
                out << " slot " << instruction.slot << " " << (slot ? slot->name : "?");
            }
            break;
        default:
            break;
    }
    return out.str();
}

std::string renderSignature(const Module& module, const Function& function) {
    std::ostringstream out;
    out << function.name << "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i != 0) out << ",";
        out << module.types.render(function.parameters[i].type);
    }
    out << "):" << module.types.render(function.returnType)
        << (function.isAsync ? " async" : "");
    return out.str();
}

// The baseline: the unoptimised module with the default pipeline run over it.
//
// An optimisation is allowed to remove an observable event - that is what
// deleting dead code *is* - but only when the event was unreachable or
// unobservable to begin with, and deciding that is exactly the work the
// optimiser does. So the comparison asks a question that can be answered: does
// the optimised module observe the same thing as the unoptimised module *once
// the same pipeline has decided which of its code can run*?
//
// The check is therefore as strong as it can be and no stronger, and the
// residual is covered by running both modules and comparing what they print
// (tests/mir_opt_pipeline_tests.cpp, tools/mir_opt_diff.sh). Two things it
// deliberately cannot see, stated rather than glossed over:
//
//   * values. Rewriting `x + 0` to the wrong value changes no *event*, so only
//     a run can catch it;
//   * a bug shared by the pipeline and the baseline, since they are the same
//     passes. What it does catch is any rewrite the pipeline does not justify:
//     an added effect, a removed output, a changed call, a lost ownership
//     event.
// The unoptimised module with the pipeline under test run over it: the set of
// events this pipeline *can* remove, which is what "removed something it should
// not have" is measured against.
Module baselineOf(const Module& module, const std::string& pipeline) {
    Module copy = module;
    std::string error;
    PassManager manager = (!pipeline.empty() && pipeline != "default")
                              ? PassManager::namedPipeline(pipeline, error)
                              : PassManager::defaultPipeline();
    // An unknown name cannot have produced the module being judged - the
    // command line rejects one before it optimises anything - so falling back
    // to the default is the only reference that means anything here.
    if (!error.empty()) manager = PassManager::defaultPipeline();
    OptimizationOptions options;
    options.verifyBetweenPasses = false;
    options.verifyAtEnd = false;
    (void)manager.run(copy, options);
    return copy;
}

// The set of slots written per function, so "the optimiser may remove a slot
// write but never add one" is checkable.
std::multiset<std::string> slotWritesOf(const Function& function) {
    std::multiset<std::string> writes;
    const ControlFlowGraph cfg(function);
    for (const BlockId id : cfg.reversePostOrder()) {
        const BasicBlock* block = function.block(id);
        if (!block) continue;
        for (const auto& instruction : block->instructions) {
            if (instruction.opcode != Opcode::Store || instruction.slot == 0) continue;
            writes.insert("store slot " + std::to_string(instruction.slot));
        }
    }
    return writes;
}

void addMismatch(DifferentialResult& result, const DifferentialOptions& options, std::string function,
                 std::string kind, std::string detail) {
    if (result.mismatches.size() >= options.maxMismatches) {
        result.truncated = true;
        return;
    }
    DifferentialMismatch mismatch;
    mismatch.function = std::move(function);
    mismatch.kind = std::move(kind);
    mismatch.detail = std::move(detail);
    result.mismatches.push_back(std::move(mismatch));
}

} // namespace

std::string DifferentialOptions::describe() const {
    std::ostringstream out;
    out << "structure=" << (compareStructure ? "yes" : "no")
        << " observable-events=" << (compareObservableEvents ? "yes" : "no")
        << " no-growth=" << (requireNoGrowth ? "yes" : "no")
        << " reference=" << (referencePipeline.empty() ? "default" : referencePipeline)
        << " max-mismatches=" << maxMismatches;
    return out.str();
}

DifferentialOptions DifferentialOptions::withReferencePipeline(std::string spec) const {
    DifferentialOptions options = *this;
    options.referencePipeline = std::move(spec);
    return options;
}

std::string DifferentialMismatch::describe() const {
    if (function.empty()) return kind + ": " + detail;
    return function + ": " + kind + ": " + detail;
}

std::string DifferentialResult::describe() const {
    std::ostringstream out;
    out << "differential: " << functionsCompared << " function(s) compared, "
        << instructionsBefore << " -> " << instructionsAfter << " instruction(s), "
        << eventsCompared << " observable event(s)";
    if (equivalent) {
        out << " - equivalent";
    } else {
        out << " - " << mismatches.size() << " mismatch(es)";
        if (truncated) out << " (truncated)";
    }
    return out.str();
}

std::vector<std::string> observableEventsOf(const Module& module, const Function& function,
                                            const std::unordered_set<SlotId>&) {
    std::vector<std::string> events;
    if (function.blocks.empty()) return events;

    const ControlFlowGraph cfg(function);
    for (const BlockId id : cfg.reversePostOrder()) {
        const BasicBlock* block = function.block(id);
        if (!block) continue;
        for (const auto& instruction : block->instructions) {
            // A write to a local slot is not counted as an event. It is
            // observable only through a later read of that slot, and deciding
            // whether a particular store can be observed is a
            // reaching-definitions question - the same work the optimiser does
            // in order to remove the store. Counting stores would make this
            // comparison either blind (allow everything) or circular (lean on
            // the pass being checked). Slot writes get their own, weaker rule
            // instead: the optimiser may remove one, never add one.
            if (instruction.opcode == Opcode::Store) continue;
            const EffectSet effects = classifyInstruction(module, instruction);
            if (!isObservableEffect(effects)) continue;
            events.push_back(renderEvent(module, function, instruction));
        }
        // Raising is observable: it is the program's answer to something.
        if (block->terminator.kind == TerminatorKind::Throw) events.push_back("throw");
    }
    return events;
}

DifferentialResult compareModules(const Module& before, const Module& after,
                                  const DifferentialOptions& options) {
    DifferentialResult result;
    auto countAll = [](const Module& module) {
        std::size_t total = 0;
        for (const auto& function : module.functions) {
            for (const auto& block : function.blocks) total += block.instructions.size();
        }
        return total;
    };
    result.instructionsBefore = countAll(before);
    result.instructionsAfter = countAll(after);

    if (options.compareStructure) {
        // Removal is legal only for `eliminate-dead-functions`, and its licence
        // is the reachability report's completeness proof, not something this
        // comparison can re-derive. So the shape rule is asymmetric: the
        // optimiser may shrink the function list (the pairing below requires a
        // subsequence, so nothing may be added, moved or renamed), and every
        // other change to the module's shape stays a mismatch.
        if (after.functions.size() > before.functions.size()) {
            addMismatch(result, options, "", "structure",
                        "function count grew: " + std::to_string(before.functions.size()) + " -> " +
                            std::to_string(after.functions.size()));
        }
        // The entry point is compared by name, not by id: removing functions
        // renumbers the survivors, and renumbering is not the entry point
        // changing. A null entry point may only stay null.
        const Function* beforeEntry = before.function(before.entryPoint);
        const Function* afterEntry = after.function(after.entryPoint);
        if ((beforeEntry == nullptr) != (afterEntry == nullptr) ||
            (beforeEntry && afterEntry && beforeEntry->name != afterEntry->name)) {
            addMismatch(result, options, "", "structure",
                        "entry point changed: " +
                            std::string(beforeEntry ? beforeEntry->name : "(none)") + " -> " +
                            std::string(afterEntry ? afterEntry->name : "(none)"));
        }
        if (before.classes.size() != after.classes.size()) {
            addMismatch(result, options, "", "structure",
                        "class count changed: " + std::to_string(before.classes.size()) + " -> " +
                            std::to_string(after.classes.size()));
        }
        if (before.statics.size() != after.statics.size()) {
            addMismatch(result, options, "", "structure",
                        "static count changed: " + std::to_string(before.statics.size()) + " -> " +
                            std::to_string(after.statics.size()));
        }
    }

    if (options.requireNoGrowth && result.instructionsAfter > result.instructionsBefore) {
        addMismatch(result, options, "", "growth",
                    "instruction count grew: " + std::to_string(result.instructionsBefore) + " -> " +
                        std::to_string(result.instructionsAfter));
    }

    // The module the optimised one is judged against: the unoptimised module
    // with the default pipeline run over it, unless the caller opted out (which
    // makes the event comparison strict against the raw original).
    const Module reference = options.normalizeBefore ? baselineOf(before, options.referencePipeline) : before;

    // Functions are paired by name, not by index: `eliminate-dead-functions`
    // may remove entries from the middle of the vector and renumber what
    // survives, and index pairing would read that as every later function
    // having been renamed. The pairing is still exact where exactness is the
    // point - each function of `after` must appear in `before` once, the order
    // of survivors must be the order they had before, and anything else is a
    // mismatch. What removal itself does is get a note, not a failure: whether
    // a function was allowed to go at all is decided by the reachability
    // report's completeness proof, and the pipeline's final whole-module
    // verification is what confirms nothing still names a removed function.
    std::unordered_map<std::string, std::size_t> afterIndices;
    for (std::size_t j = 0; j < after.functions.size(); ++j)
        afterIndices.emplace(after.functions[j].name, j);
    std::unordered_map<std::string, std::size_t> referenceIndices;
    for (std::size_t j = 0; j < reference.functions.size(); ++j)
        referenceIndices.emplace(reference.functions[j].name, j);
    std::unordered_set<std::string> beforeNames;
    for (const Function& function : before.functions) beforeNames.insert(function.name);

    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    std::size_t removedFunctions = 0;
    std::size_t lastMatchedAfter = 0;
    bool haveLastMatched = false;
    for (std::size_t i = 0; i < before.functions.size(); ++i) {
        const std::string& name = before.functions[i].name;
        const auto found = afterIndices.find(name);
        if (found == afterIndices.end()) {
            ++removedFunctions;
            continue;
        }
        if (haveLastMatched && found->second < lastMatchedAfter) {
            addMismatch(result, options, name, "structure", "function order changed");
            continue;
        }
        lastMatchedAfter = found->second;
        haveLastMatched = true;
        pairs.emplace_back(i, found->second);
    }
    if (pairs.size() != after.functions.size()) {
        for (const Function& function : after.functions) {
            if (beforeNames.count(function.name) == 0) {
                addMismatch(result, options, function.name, "structure", "function was added");
                break;
            }
        }
    }
    if (removedFunctions != 0) {
        result.notes.push_back("module: " + std::to_string(removedFunctions) +
                              " function(s) removed by the optimiser; the survivors pair with "
                              "the original by name and order");
    }

    for (const auto& [beforeIndex, afterIndex] : pairs) {
        const Function& beforeFunction = before.functions[beforeIndex];
        const Function& afterFunction = after.functions[afterIndex];
        ++result.functionsCompared;

        if (options.compareStructure) {
            const std::string beforeSignature = renderSignature(before, beforeFunction);
            const std::string afterSignature = renderSignature(after, afterFunction);
            if (beforeSignature != afterSignature) {
                addMismatch(result, options, beforeFunction.name, "structure",
                            "signature changed: " + beforeSignature + " -> " + afterSignature);
            }
        }

        if (!options.compareObservableEvents) continue;

        const auto baselineIt = referenceIndices.find(beforeFunction.name);
        if (baselineIt == referenceIndices.end()) {
            // The reference is the same pipeline run over the same input; if it
            // dropped a function `after` kept (or vice versa), the module under
            // judgment was not built the way the options claim, and event
            // comparison against this baseline would say something about that
            // mismatch and nothing about the optimisation.
            addMismatch(result, options, beforeFunction.name, "structure",
                        "the baseline module removed this function but the optimised one kept it - "
                        "`after` was not built by the reference pipeline");
            continue;
        }
        const Function& baselineFunction = reference.functions[baselineIt->second];
        const std::vector<std::string> beforeEvents = observableEventsOf(before, beforeFunction, {});
        const std::vector<std::string> baselineEvents =
            observableEventsOf(reference, baselineFunction, {});
        const std::vector<std::string> afterEvents = observableEventsOf(after, afterFunction, {});
        result.eventsCompared += beforeEvents.size();

        // --- no effect may be invented ------------------------------------
        // This one is unconditional: an optimiser may delete, never create.
        {
            std::multiset<std::string> raw(beforeEvents.begin(), beforeEvents.end());
            for (const auto& event : afterEvents) {
                const auto found = raw.find(event);
                if (found != raw.end()) { raw.erase(found); continue; }
                addMismatch(result, options, beforeFunction.name, "observable-events",
                            "the optimised function performs an observable event the original does "
                            "not: " + event);
                break;
            }
        }

        // --- no effect may be lost ----------------------------------------
        // Compared against the baseline, so that deleting code the pipeline
        // itself proves unreachable or unobservable is not reported as a
        // divergence, while anything else is.
        std::vector<std::string> sortedBaseline = baselineEvents;
        std::vector<std::string> sortedAfter = afterEvents;
        std::sort(sortedBaseline.begin(), sortedBaseline.end());
        std::sort(sortedAfter.begin(), sortedAfter.end());
        if (sortedBaseline != sortedAfter) {
            std::map<std::string, int> delta;
            for (const auto& event : sortedBaseline) --delta[event];
            for (const auto& event : sortedAfter) ++delta[event];
            std::ostringstream detail;
            detail << "observable events changed relative to the baseline:";
            std::size_t shown = 0;
            for (const auto& entry : delta) {
                if (entry.second == 0) continue;
                if (shown++ >= 8) { detail << " ..."; break; }
                detail << " " << (entry.second > 0 ? "+" : "") << entry.second << "x " << entry.first;
            }
            addMismatch(result, options, beforeFunction.name, "observable-events", detail.str());
        } else if (beforeEvents != afterEvents) {
            // The contents agree with the baseline but not with the raw
            // original: events went away, and the baseline says they were
            // unreachable. Worth seeing, not worth failing on.
            result.notes.push_back(beforeFunction.name + ": " +
                                   std::to_string(beforeEvents.size() - afterEvents.size()) +
                                   " observable event(s) removed as dead code");
        }

        // --- no slot write may be invented --------------------------------
        const std::multiset<std::string> beforeWrites = slotWritesOf(beforeFunction);
        std::multiset<std::string> afterWrites = slotWritesOf(afterFunction);
        for (const auto& write : beforeWrites) {
            const auto found = afterWrites.find(write);
            // The write may legitimately be absent - removing a slot write is
            // an optimisation - so only erase what is there. Erasing end()
            // would be undefined behaviour, and on a multiset it corrupts the
            // tree rather than crashing.
            if (found != afterWrites.end()) afterWrites.erase(found);
        }
        if (!afterWrites.empty()) {
            addMismatch(result, options, beforeFunction.name, "observable-events",
                        "the optimised function writes a slot the original does not: " +
                            *afterWrites.begin());
        }
    }

    result.equivalent = result.mismatches.empty();
    return result;
}

} // namespace zl::mir
