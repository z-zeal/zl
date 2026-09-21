#include "zl/mir/passes.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "zl/mir/printer.hpp"

namespace zl::mir {
namespace {

// ---------------------------------------------------------------------------
// Analysis construction
// ---------------------------------------------------------------------------
// One trait per analysis: how to build it from a module and a function. This is
// the only place that knows, so a new analysis is added here and in the
// manager's accessor, nowhere else.
template <typename Analysis>
struct AnalysisTraits;

template <>
struct AnalysisTraits<ControlFlowGraph> {
    static ControlFlowGraph make(const Module&, const Function& function) { return ControlFlowGraph(function); }
};
template <>
struct AnalysisTraits<DefUseInfo> {
    static DefUseInfo make(const Module&, const Function& function) { return DefUseInfo(function); }
};
template <>
struct AnalysisTraits<LivenessAnalysis> {
    static LivenessAnalysis make(const Module&, const Function& function) { return LivenessAnalysis(function); }
};
template <>
struct AnalysisTraits<ConstantAnalysis> {
    static ConstantAnalysis make(const Module& module, const Function& function) {
        return ConstantAnalysis(module, function);
    }
};

std::string trimmed(std::string value) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
    while (!value.empty() && isSpace(value.back())) value.pop_back();
    return value;
}

std::vector<std::string> splitOnCommas(const std::string& spec) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : spec) {
        if (c == ',') {
            parts.push_back(trimmed(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(trimmed(current));
    parts.erase(std::remove_if(parts.begin(), parts.end(),
                               [](const std::string& part) { return part.empty(); }),
                parts.end());
    return parts;
}

// A file-system-safe rendering of a function name: "Greeter.greet(string)"
// would otherwise try to create a directory called "Greeter.greet(string)".
std::string sanitizeForFileName(const std::string& value) {
    std::string out;
    for (const char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    if (out.size() > 96) out.resize(96);
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

} // namespace

// ---------------------------------------------------------------------------
// Analysis manager
// ---------------------------------------------------------------------------

template <typename Analysis>
const Analysis& FunctionAnalysisManager::get(const Function& function) {
    auto& byFunction = cache_[analysisId<Analysis>()];
    const auto found = byFunction.find(function.id);
    if (found != byFunction.end()) {
        ++reused_;
        return static_cast<Holder<Analysis>&>(*found->second).value;
    }
    auto holder = std::make_unique<Holder<Analysis>>(AnalysisTraits<Analysis>::make(module_, function));
    const Analysis& result = holder->value;
    byFunction.emplace(function.id, std::move(holder));
    ++built_;
    return result;
}

const ControlFlowGraph& FunctionAnalysisManager::cfg(const Function& function) {
    return get<ControlFlowGraph>(function);
}
const DefUseInfo& FunctionAnalysisManager::defUse(const Function& function) {
    return get<DefUseInfo>(function);
}
const LivenessAnalysis& FunctionAnalysisManager::liveness(const Function& function) {
    return get<LivenessAnalysis>(function);
}
const ConstantAnalysis& FunctionAnalysisManager::constants(const Function& function) {
    return get<ConstantAnalysis>(function);
}

void FunctionAnalysisManager::invalidate(const Function& function) {
    for (auto& entry : cache_) entry.second.erase(function.id);
}

void FunctionAnalysisManager::invalidateAll() { cache_.clear(); }

// ---------------------------------------------------------------------------
// Shared rewrite helpers
// ---------------------------------------------------------------------------

std::size_t replaceValueUsesTyped(Function& function, ValueId from, const Operand& to) {
    std::size_t rewritten = 0;
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) {
                if (operand.isNone()) continue;
                if (valueOf(operand) != from) continue;
                if (operand.type != to.type) continue;
                operand = to;
                ++rewritten;
            }
        }
        Terminator& terminator = block.terminator;
        if (!terminator.value.isNone() && valueOf(terminator.value) == from &&
            terminator.value.type == to.type) {
            terminator.value = to;
            ++rewritten;
        }
        for (auto& arguments : terminator.edgeArguments) {
            for (auto& argument : arguments) {
                if (argument.isNone()) continue;
                if (valueOf(argument) != from) continue;
                if (argument.type != to.type) continue;
                argument = to;
                ++rewritten;
            }
        }
    }
    return rewritten;
}

bool operandIsReplaceable(const Function&, const Operand& use, const Operand& replacement) {
    // The only condition this framework can state in general: the substituted
    // operand must carry exactly the type the use site declared. Anything
    // finer (dominance, liveness) is the caller's proof to make first.
    if (use.isNone() || replacement.isNone()) return false;
    return use.type == replacement.type;
}

std::size_t countInstructions(const Function& function) noexcept {
    std::size_t total = 0;
    for (const auto& block : function.blocks) total += block.instructions.size();
    return total;
}

std::size_t countBlocks(const Function& function) noexcept { return function.blocks.size(); }

const Instruction* definingInstruction(const Function& function, ValueId value) {
    if (!value.isTemp()) return nullptr;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.result != kNoTemp && instruction.result == value.index) return &instruction;
        }
    }
    return nullptr;
}

std::uint32_t typeOfValue(const Function& function, ValueId value) {
    if (value.isParam()) {
        if (value.index >= function.parameters.size()) return 0;
        return function.parameters[static_cast<std::size_t>(value.index)].type;
    }
    if (value.isBlockParam()) {
        const BasicBlock* owner = function.blockOfParameter(value.index);
        if (!owner) return 0;
        for (const auto& parameter : owner->parameters) {
            if (parameter.id == value.index) return parameter.type;
        }
        return 0;
    }
    if (value.isTemp()) {
        const Instruction* definition = definingInstruction(function, value);
        return definition ? definition->resultType : 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Pass registry
// ---------------------------------------------------------------------------

PassRegistry& PassRegistry::instance() {
    static PassRegistry registry;
    return registry;
}

bool PassRegistry::registerPass(std::string name, Factory factory) {
    if (name.empty() || factories_.count(name) != 0) return false;
    factories_.emplace(std::move(name), std::move(factory));
    return true;
}

bool PassRegistry::has(const std::string& name) const { return factories_.count(name) != 0; }

std::unique_ptr<Pass> PassRegistry::create(const std::string& name) const {
    const auto found = factories_.find(name);
    if (found == factories_.end()) return nullptr;
    return found->second();
}

std::vector<std::string> PassRegistry::passNames() const {
    std::vector<std::string> names;
    for (const auto& entry : factories_) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

void registerBuiltinPasses() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    auto& registry = PassRegistry::instance();
    registry.registerPass("fold-constants", [] { return createConstantFoldingPass(); });
    registry.registerPass("propagate-constants", [] { return createConstantPropagationPass(); });
    registry.registerPass("simplify-algebraic", [] { return createAlgebraicSimplificationPass(); });
    registry.registerPass("remove-redundant-conversions", [] { return createRedundantConversionPass(); });
    registry.registerPass("propagate-copies", [] { return createCopyPropagationPass(); });
    registry.registerPass("simplify-branches", [] { return createBranchSimplificationPass(); });
    registry.registerPass("eliminate-dead-blocks", [] { return createDeadBlockEliminationPass(); });
    registry.registerPass("eliminate-dead-values", [] { return createDeadValueEliminationPass(); });
    registry.registerPass("eliminate-dead-functions", [] { return createEliminateDeadFunctionsPass(); });
}

// ---------------------------------------------------------------------------
// Options and reports
// ---------------------------------------------------------------------------

std::string OptimizationOptions::describe() const {
    std::ostringstream out;
    out << "verify-between-passes=" << (verifyBetweenPasses ? "yes" : "no")
        << " rollback=" << (rollbackOnVerificationFailure ? "yes" : "no")
        << " verify-at-end=" << (verifyAtEnd ? "yes" : "no")
        << " keep-snapshots=" << (keepSnapshots ? "yes" : "no")
        << " snapshots=" << (snapshotDirectory.empty() ? "(none)" : snapshotDirectory)
        << " max-iterations=" << maxIterations;
    return out.str();
}

std::string PassRunRecord::describe() const {
    std::ostringstream out;
    out << pass << " on " << function;
    if (!changed) {
        out << ": no change";
        return out.str();
    }
    out << ": " << instructionsBefore << " -> " << instructionsAfter << " instruction(s), "
        << blocksBefore << " -> " << blocksAfter << " block(s)";
    if (!note.empty()) out << " (" << note << ")";
    if (rolledBack) out << " ROLLED BACK: " << verificationError;
    return out.str();
}

bool OptimizationReport::changed() const {
    for (const auto& run : runs) {
        if (run.changed && !run.rolledBack) return true;
    }
    return false;
}

size_t OptimizationReport::instructionsRemoved() const {
    size_t total = 0;
    for (const auto& run : runs) {
        if (run.rolledBack || !run.changed) continue;
        total += run.instructionsBefore - std::min(run.instructionsBefore, run.instructionsAfter);
    }
    return total;
}

size_t OptimizationReport::blocksRemoved() const {
    size_t total = 0;
    for (const auto& run : runs) {
        if (run.rolledBack || !run.changed) continue;
        total += run.blocksBefore - std::min(run.blocksBefore, run.blocksAfter);
    }
    return total;
}

bool OptimizationReport::trustworthy() const {
    if (rollbacks != 0) return false;
    if (verifiedAtEnd) return finalVerification.ok();
    return true;
}

std::string OptimizationReport::describe() const {
    std::ostringstream out;
    std::size_t changedRuns = 0;
    for (const auto& run : runs) {
        if (run.changed && !run.rolledBack) ++changedRuns;
    }
    out << "mir optimiser: " << runs.size() << " pass run(s) over " << iterations
        << " iteration(s); " << changedRuns << " changed; " << instructionsRemoved()
        << " instruction(s) and " << blocksRemoved() << " block(s) removed";
    if (rollbacks != 0) out << "; " << rollbacks << " rollback(s)";
    if (verifiedAtEnd) out << "; final verification " << (finalVerification.ok() ? "passed" : "FAILED");
    out << "; analyses " << analysesBuilt << " built / " << analysesReused << " reused";
    if (snapshotsWritten != 0) out << "; " << snapshotsWritten << " snapshot(s) written";
    return out.str();
}

std::string OptimizationReport::describeTrace() const {
    std::ostringstream out;
    for (const auto& run : runs) {
        out << "  [" << run.iteration << "] " << run.pass << ": " << run.describe() << '\n';
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

void PassManager::addPass(std::unique_ptr<Pass> pass) {
    if (pass) passes_.push_back(std::move(pass));
}

bool PassManager::addPassByName(const std::string& name) {
    registerBuiltinPasses();
    auto pass = PassRegistry::instance().create(name);
    if (!pass) return false;
    passes_.push_back(std::move(pass));
    return true;
}

std::string PassManager::describePipeline() const {
    std::ostringstream out;
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        out << (i + 1) << ". " << passes_[i]->name() << " - " << passes_[i]->description() << '\n';
    }
    return out.str();
}

PassManager PassManager::defaultPipeline() {
    registerBuiltinPasses();
    PassManager manager;

    // Ordering, and why:
    //
    //   eliminate-dead-functions
    //                       first, and the only module pass: a function that
    //                       cannot run needs no per-function passes run over
    //                       it, and the smaller module is what every later
    //                       pass (and the verifier at the end) walks. It runs
    //                       again each iteration, so a call site some other
    //                       pass deletes can strand a function that was live
    //                       on the way in.
    //   simplify-branches   first among the function passes, because a branch
    //                       on a constant is the cheapest win available and it
    //                       *creates* the unreachable blocks the next pass
    //                       removes.
    //   eliminate-dead-blocks
    //                       immediately after, so the value passes walk a CFG
    //                       with no dead regions in it - a dead block can
    //                       otherwise hand a block parameter a value that no
    //                       live path ever produces, and an analysis would
    //                       believe it.
    //   fold-constants      before propagation: folding *interns* the results,
    //                       and the constant analysis only reports constants
    //                       the pool already spells out, so folding is what
    //                       makes `2 + 3` visible to propagation at all.
    //   propagate-constants then carries those constants to every use, across
    //                       blocks and through block parameters.
    //   simplify-algebraic  after the constants have moved, because an operand
    //                       that only *became* zero or one is an identity that
    //                       was not visible before.
    //   remove-redundant-conversions
    //                       same reason: the conversions that stop converting
    //                       are the ones whose operand was just retyped.
    //   propagate-copies    after the identities, which turn instructions into
    //                       copies that this pass can then dissolve.
    //   eliminate-dead-values
    //                       last, because everything before it leaves
    //                       definitions behind: folding and propagation
    //                       replace *uses*, and only this pass removes the
    //                       definitions that are now unread.
    //
    // The pipeline then repeats (OptimizationOptions::maxIterations), because
    // the last pass enables the first one again: values that become constant
    // late turn into branches on constants that were not constant on the way
    // in.
    manager.addPass(createEliminateDeadFunctionsPass());
    manager.addPass(createBranchSimplificationPass());
    manager.addPass(createDeadBlockEliminationPass());
    manager.addPass(createConstantFoldingPass());
    manager.addPass(createConstantPropagationPass());
    manager.addPass(createAlgebraicSimplificationPass());
    manager.addPass(createRedundantConversionPass());
    manager.addPass(createCopyPropagationPass());
    manager.addPass(createDeadValueEliminationPass());
    return manager;
}

PassManager PassManager::namedPipeline(const std::string& spec, std::string& error) {
    registerBuiltinPasses();
    const std::string name = trimmed(spec);
    if (name.empty() || name == "default") return defaultPipeline();
    if (name == "none" || name == "off") return PassManager();

    PassManager manager;
    for (const auto& part : splitOnCommas(name)) {
        if (!manager.addPassByName(part)) {
            error = "unknown MIR optimisation pass '" + part + "'";
            return PassManager();
        }
    }
    return manager;
}

OptimizationReport PassManager::run(Module& module, const OptimizationOptions& options) {
    registerBuiltinPasses();
    OptimizationReport report;
    FunctionAnalysisManager analyses(module);

    const std::size_t iterations = options.maxIterations == 0 ? 1 : options.maxIterations;
    std::size_t sequence = 0;

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        bool iterationChanged = false;

        for (auto& pass : passes_) {
            // A module pass runs once per iteration, over the whole module.
            if (pass->isModulePass()) {
                auto* modulePass = static_cast<ModulePass*>(pass.get());
                // The module-wide shape before and after, so a module pass
                // appears in the report with real numbers rather than zeros -
                // `instructionsRemoved()` and the `--mir-opt-check` trace both
                // read these fields, and a whole-module rewrite that shows up
                // as "0 -> 0" would hide exactly the change it should be
                // accountable for.
                std::size_t instructionsBefore = 0;
                std::size_t blocksBefore = 0;
                for (const Function& function : module.functions) {
                    instructionsBefore += countInstructions(function);
                    blocksBefore += countBlocks(function);
                }
                const bool changed = modulePass->runOnModule(module, analyses);
                std::size_t instructionsAfter = instructionsBefore;
                std::size_t blocksAfter = blocksBefore;
                if (changed) {
                    instructionsAfter = 0;
                    blocksAfter = 0;
                    for (const Function& function : module.functions) {
                        instructionsAfter += countInstructions(function);
                        blocksAfter += countBlocks(function);
                    }
                }
                PassRunRecord record;
                record.pass = pass->name();
                record.function = "<module>";
                record.iteration = iteration;
                record.changed = changed;
                record.instructionsBefore = instructionsBefore;
                record.instructionsAfter = instructionsAfter;
                record.blocksBefore = blocksBefore;
                record.blocksAfter = blocksAfter;
                record.note = pass->lastNote();
                report.runs.push_back(record);
                if (changed) {
                    analyses.invalidateAll();
                    iterationChanged = true;
                }
                continue;
            }

            auto* functionPass = static_cast<FunctionPass*>(pass.get());
            for (auto& function : module.functions) {
                PassRunRecord record;
                record.pass = pass->name();
                record.function = function.name;
                record.iteration = iteration;

                // Nothing to optimise, or a body this framework will not touch:
                // a function lowering could not translate completely is a
                // partial picture of the source, and deleting from a partial
                // picture is how an optimiser invents behaviour.
                if (function.blocks.empty() || function.incomplete) {
                    record.note = function.incomplete ? "skipped: incomplete lowering" : "skipped: no body";
                    report.runs.push_back(record);
                    continue;
                }

                const std::size_t instructionsBefore = countInstructions(function);
                const std::size_t blocksBefore = countBlocks(function);
                const bool captureText = options.keepSnapshots || !options.snapshotDirectory.empty();
                const std::string beforeText =
                    captureText ? printFunction(module, function) : std::string{};

                // The pre-pass copy is what a failed verification restores.
                // Taken unconditionally: it is cheap next to the verification
                // it protects, and taking it only "when it might be needed"
                // is how a rollback turns out to be unavailable exactly once.
                const Function backup = function;

                bool changed = false;
                try {
                    changed = functionPass->runOnFunction(module, function, analyses);
                } catch (const std::exception& error) {
                    // A throwing pass is a bug, and the response is the same as
                    // for any other bug: restore, record, carry on.
                    function = backup;
                    analyses.invalidate(function);
                    record.changed = true;
                    record.rolledBack = true;
                    record.verificationError = std::string("pass threw: ") + error.what();
                    ++report.rollbacks;
                    report.runs.push_back(record);
                    continue;
                }

                record.changed = changed;
                record.note = pass->lastNote();
                record.instructionsBefore = instructionsBefore;
                record.blocksBefore = blocksBefore;
                record.instructionsAfter = countInstructions(function);
                record.blocksAfter = countBlocks(function);

                if (!changed) {
                    report.runs.push_back(record);
                    continue;
                }

                // The function changed, so every fact about it is now stale.
                analyses.invalidate(function);
                // Passes are expected to leave the edge list consistent, but
                // the invariant is cheap to re-establish and expensive to have
                // wrong: the verifier reads `edges` as well as the terminators.
                function.rebuildEdges();

                if (options.verifyBetweenPasses) {
                    // Intermediate verification relaxes the unreachable-block
                    // rule on purpose. Simplifying a branch *creates*
                    // unreachable blocks; the pruning pass is what removes
                    // them, and it runs after. Failing the build in between
                    // would make the two passes unable to cooperate.
                    VerifierOptions intermediate = options.verifier;
                    intermediate.unreachableBlocksAreErrors = false;
                    const VerificationReport verification = verifyFunction(module, function, intermediate);
                    record.verified = verification.ok();
                    if (!verification.ok()) {
                        record.verificationError = verification.describe();
                        if (options.rollbackOnVerificationFailure) {
                            function = backup;
                            function.rebuildEdges();
                            analyses.invalidate(function);
                            record.rolledBack = true;
                            record.instructionsAfter = record.instructionsBefore;
                            record.blocksAfter = record.blocksBefore;
                            ++report.rollbacks;
                        }
                    }
                }

                if (!options.snapshotDirectory.empty()) {
                    const std::string afterText = printFunction(module, function);
                    std::ostringstream body;
                    body << "; MIR snapshot " << sequence << '\n'
                         << "; pass: " << pass->name() << " (iteration " << iteration << ")\n"
                         << "; function: " << function.name << '\n'
                         << "; changed: yes\n"
                         << "; rolled back: " << (record.rolledBack ? "yes" : "no") << '\n';
                    if (!record.verified) body << "; verification: " << record.verificationError << '\n';
                    body << "\n; ---- before ----\n"
                         << beforeText << "\n; ---- after ----\n"
                         << afterText << "\n; ---- diff ----\n"
                         << diffLines(beforeText, afterText);
                    const std::string path = options.snapshotDirectory + "/" +
                                             std::to_string(sequence) + "-" +
                                             sanitizeForFileName(pass->name()) + "-" +
                                             sanitizeForFileName(function.name) + ".mir";
                    if (writeTextFile(path, body.str())) ++report.snapshotsWritten;
                }

                if (options.keepSnapshots) {
                    const std::string afterText = printFunction(module, function);
                    MirSnapshot before;
                    before.sequence = sequence;
                    before.pass = pass->name();
                    before.function = function.name;
                    before.isBefore = true;
                    before.text = beforeText;
                    MirSnapshot after = before;
                    after.isBefore = false;
                    after.text = afterText;
                    report.snapshots.push_back(std::move(before));
                    report.snapshots.push_back(std::move(after));
                }

                ++sequence;
                if (!record.rolledBack) iterationChanged = true;
                report.runs.push_back(record);
            }
        }

        report.iterations = iteration + 1;
        if (!iterationChanged) break;
    }

    report.analysesBuilt = analyses.analysesBuilt();
    report.analysesReused = analyses.analysesReused();

    if (options.verifyAtEnd) {
        report.finalVerification = verifyModule(module, options.verifier);
        // `verifiedAtEnd` means "the final check was run"; whether it passed is
        // `finalVerification.ok()`. Conflating them would make "not verified"
        // indistinguishable from "verified and clean".
        report.verifiedAtEnd = true;
    }

    return report;
}

OptimizationReport optimizeModule(Module& module, const OptimizationOptions& options) {
    PassManager manager = PassManager::defaultPipeline();
    return manager.run(module, options);
}

// ---------------------------------------------------------------------------
// Before / after inspection
// ---------------------------------------------------------------------------

bool writeTextFile(const std::string& path, const std::string& text) {
    try {
        std::filesystem::path target(path);
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        return out.good();
    } catch (const std::exception&) {
        return false;
    }
}

std::string diffLines(const std::string& before, const std::string& after, std::size_t contextLines) {
    const std::vector<std::string> a = splitLines(before);
    const std::vector<std::string> b = splitLines(after);

    // Guard against quadratic memory on a pathological dump: a diff is a
    // debugging aid, and a debugging aid that exhausts memory is worse than
    // one that says "too big to diff".
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if (n * m > 4u * 1024u * 1024u) {
        std::ostringstream out;
        out << "(diff skipped: " << n << " before / " << m << " after lines)\n";
        return out.str();
    }

    // Classic LCS table; `lcs[i][j]` is the length of the longest common
    // subsequence of a[i..] and b[j..].
    std::vector<std::vector<std::uint32_t>> lcs(n + 1, std::vector<std::uint32_t>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    struct Op {
        char kind{';'}; // '-' removed, '+' added, ' ' unchanged
        std::string text;
        std::size_t beforeLine{0};
        std::size_t afterLine{0};
    };
    std::vector<Op> ops;
    ops.reserve(n + m);
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            ops.push_back(Op{' ', a[i], i + 1, j + 1});
            ++i;
            ++j;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            ops.push_back(Op{'-', a[i], i + 1, 0});
            ++i;
        } else {
            ops.push_back(Op{'+', b[j], 0, j + 1});
            ++j;
        }
    }
    while (i < n) ops.push_back(Op{'-', a[i], i + 1, 0}), ++i;
    while (j < m) ops.push_back(Op{'+', b[j], 0, j + 1}), ++j;

    std::vector<bool> keep(ops.size(), false);
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].kind == ' ') continue;
        const std::size_t from = k > contextLines ? k - contextLines : 0;
        const std::size_t to = std::min(ops.size(), k + contextLines + 1);
        for (std::size_t x = from; x < to; ++x) keep[x] = true;
    }

    std::ostringstream out;
    std::size_t k = 0;
    while (k < ops.size()) {
        if (!keep[k]) {
            ++k;
            continue;
        }
        std::size_t start = k;
        std::size_t removed = 0;
        std::size_t added = 0;
        std::size_t beforeLine = 0;
        std::size_t afterLine = 0;
        while (k < ops.size() && keep[k]) {
            if (ops[k].kind == '-') ++removed;
            if (ops[k].kind == '+') ++added;
            if (beforeLine == 0 && ops[k].beforeLine != 0) beforeLine = ops[k].beforeLine;
            if (afterLine == 0 && ops[k].afterLine != 0) afterLine = ops[k].afterLine;
            ++k;
        }
        out << "@@ -" << (beforeLine == 0 ? 0 : beforeLine) << "," << removed << " +"
            << (afterLine == 0 ? 0 : afterLine) << "," << added << " @@\n";
        for (std::size_t x = start; x < k; ++x) out << ops[x].kind << ops[x].text << '\n';
    }
    if (out.str().empty()) return "(no textual difference)\n";
    return out.str();
}

} // namespace zl::mir
