#pragma once

// ---------------------------------------------------------------------------
// The MIR optimisation framework
// ---------------------------------------------------------------------------
//
// This header is the whole optimisation layer in one include, ordered the way
// the layer is built: analyses first, then the pass interface, then the
// pipeline that drives them, then the individual passes.
//
// The framework is deliberately boring, because the interesting property is
// not what it can do but what it will not do. Four rules define it:
//
//   1. **Every pass reports whether it changed anything.** Most passes are
//      function-to-function transforms, and one (see `ModulePass`) deliberately
//      is not; what every pass shares is that it answers the question. Nothing
//      rewrites MIR silently, and the pipeline uses the answer both to decide
//      whether another iteration is worth running and to decide whether to
//      re-verify.
//
//   2. **The verifier runs between passes.** Any pass that reports a change is
//      followed by `verifyFunction`. A pass that produces invalid MIR is a
//      bug in the pass, and the pipeline treats it as one: the function is
//      restored from the pre-pass copy and the failure is recorded in the
//      report rather than being handed to a backend.
//
//   3. **Safety is a property of the instruction, not of the pass.** Each pass
//      asks `effects.hpp` what an instruction does before it deletes, replaces
//      or reorders it. There is no per-pass notion of "safe" to drift out of
//      sync with the others.
//
//   4. **Where safety cannot be proven, nothing is done.** Every pass counts
//      what it declined as well as what it changed, and `OptimizationReport`
//      carries both. A skipped optimisation is a correct program; a wrong one
//      is a miscompile, and the framework will always choose the former.
//
// Ordering is explicit and inspectable: `PassManager::describePipeline()`
// prints the sequence, and the default pipeline's comment says why each pass
// sits where it does.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "zl/mir/analysis.hpp"
#include "zl/mir/dataflow.hpp"
#include "zl/mir/effects.hpp"
#include "zl/mir/function.hpp"
#include "zl/mir/verifier.hpp"

namespace zl::mir {

// ---------------------------------------------------------------------------
// Analysis manager
// ---------------------------------------------------------------------------
//
// Analyses are derived, never stored in the MIR, so running the same one twice
// is wasted work and - worse - a chance for two passes to see two different
// versions of the same fact. The manager owns one result per (analysis,
// function) and hands out references, so a pass asks for `cfg(function)` as
// many times as it likes and always gets the same object.
//
// Freshness is the caller's job, stated once: **a pass that mutates a function
// must call `invalidate(function)` before it asks for another analysis.** The
// manager cannot detect mutation, and a stale `DefUseInfo` is exactly the kind
// of thing that turns a correct rewrite into a dangling operand. Nothing is
// silently recomputed, because recomputing on every query would defeat the
// point of caching, and guessing would be worse.
class FunctionAnalysisManager {
public:
    explicit FunctionAnalysisManager(const Module& module) noexcept : module_(module) {}

    [[nodiscard]] const ControlFlowGraph& cfg(const Function& function);
    [[nodiscard]] const DefUseInfo& defUse(const Function& function);
    [[nodiscard]] const LivenessAnalysis& liveness(const Function& function);
    [[nodiscard]] const ConstantAnalysis& constants(const Function& function);

    // Drops every cached result for one function. Called by a pass after it
    // rewrites that function, and by the pipeline after any change.
    void invalidate(const Function& function);
    // Drops everything. Used between pipeline iterations and after a rollback.
    void invalidateAll();

    // Statistics, so a report can say whether the cache is earning its keep.
    [[nodiscard]] std::size_t analysesBuilt() const noexcept { return built_; }
    [[nodiscard]] std::size_t analysesReused() const noexcept { return reused_; }

private:
    struct Entry {
        virtual ~Entry() = default;
    };
    template <typename Analysis>
    struct Holder : Entry {
        explicit Holder(Analysis value) : value(std::move(value)) {}
        Analysis value;
    };

    template <typename Analysis>
    static std::size_t analysisId() noexcept {
        static const std::size_t id = nextAnalysisId();
        return id;
    }
    static std::size_t nextAnalysisId() noexcept {
        static std::size_t counter = 0;
        return counter++;
    }

    template <typename Analysis>
    const Analysis& get(const Function& function);

    const Module& module_;
    std::unordered_map<std::size_t, std::unordered_map<FunctionId, std::unique_ptr<Entry>>> cache_;
    std::size_t built_{0};
    std::size_t reused_{0};
};

// ---------------------------------------------------------------------------
// Pass interface
// ---------------------------------------------------------------------------

class FunctionAnalysisManager;

// Base class of everything the pipeline can run. `name()` is the registry key
// and appears in every diagnostic, so it must be stable and unique.
class Pass {
public:
    virtual ~Pass() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool isModulePass() const noexcept = 0;
    // One line for `describePipeline()`: what this pass does, in the terms of
    // the safety argument rather than of the implementation.
    [[nodiscard]] virtual std::string description() const = 0;
    // What the pass did on its most recent run, in its own words - "folded 3
    // instruction(s), declined 1 that could raise". Recorded in the report so a
    // `--mir-opt-check` trace says more than "changed", and so a declined
    // optimisation is visible rather than invisible.
    [[nodiscard]] virtual std::string lastNote() const { return {}; }
};

// Runs once per function. Every optimisation in this layer is one of these:
// the correctness argument for each (dominance, liveness, the effect
// classification) is stated per function and needs no whole-module view.
//
// Returns true when the function was rewritten. A pass that declines every
// opportunity must return false, so the pipeline can stop early and so the
// verifier is not run for nothing.
class FunctionPass : public Pass {
public:
    [[nodiscard]] bool isModulePass() const noexcept final { return false; }

    virtual bool runOnFunction(Module& module, Function& function, FunctionAnalysisManager& analyses) = 0;
};

// Runs once over the whole module. Reserved for transforms whose correctness
// argument needs every function at once (inter-procedural analysis, whole
// module dead-function elimination, call-graph rewrites). One ships today:
// `eliminate-dead-functions`, which removes functions only when the
// reachability report proves the call graph closed - reflection considered,
// every function value pinned - so that "nobody calls this" really means
// "this cannot run" (see `reachability.hpp` for that argument, and the pass's
// file header for the keep set). A module pass gets no per-pass verification -
// there is no per-module slot in that loop - so its rewrite is built to refuse
// anything it cannot fully explain, and the final whole-module verification is
// the backstop that fails the pipeline rather than hand a broken module to a
// backend.
class ModulePass : public Pass {
public:
    [[nodiscard]] bool isModulePass() const noexcept final { return true; }

    // The function analysis manager is available because a module pass that
    // needs per-function facts should use the same cached ones the function
    // passes use, not a private copy that can disagree with them.
    virtual bool runOnModule(Module& module, FunctionAnalysisManager& analyses) = 0;
};

// ---------------------------------------------------------------------------
// Shared rewrite helpers
// ---------------------------------------------------------------------------
//
// Operand surgery that more than one pass needs, stated once so two passes
// cannot disagree about whether a rewrite is legal.

// Replaces every use of `from` with `to`, but only at use sites whose operand
// type is exactly `to.type`.
//
// The type condition is the whole safety argument. A MIR operand carries the
// type the *use* expects, which need not be the type the *definition* claims
// (a value flows into a wider union, a dynamic slot, an interface-typed
// parameter). Substituting a constant that is typed by its definition at a
// site that expects something else would silently narrow the operand, and the
// verifier's type rules are the tripwire we would be walking into. Refusing
// the substitution costs an optimisation; performing it could cost the
// program's meaning.
//
// Returns the number of uses rewritten.
std::size_t replaceValueUsesTyped(Function& function, ValueId from, const Operand& to);

// True when `replacement` may be substituted for a use of the value `from`
// produces: a constant must match the definition's type, and a value must be
// the value itself or one defined where the use can see it.
[[nodiscard]] bool operandIsReplaceable(const Function& function, const Operand& use,
                                       const Operand& replacement);

[[nodiscard]] std::size_t countInstructions(const Function& function) noexcept;
[[nodiscard]] std::size_t countBlocks(const Function& function) noexcept;

// The instruction that defines a value, or nullptr for a parameter, a block
// parameter, or a value nothing defines. MIR is SSA, so there is at most one,
// and several passes need to inspect it (an identity rewrite has to see that
// its operand was produced by the instruction it cancels out).
[[nodiscard]] const Instruction* definingInstruction(const Function& function, ValueId value);

// The type a value was declared or computed with, or 0 when it cannot be
// determined. Used to check that a constant substituted for a value really
// denotes the same type.
[[nodiscard]] std::uint32_t typeOfValue(const Function& function, ValueId value);

// ---------------------------------------------------------------------------
// The individual passes
// ---------------------------------------------------------------------------
//
// Each factory returns a fresh pass; the registry below is how a pipeline is
// named rather than assembled by hand.

// Folds an instruction whose operands are all constants into the constant it
// computes, then replaces the uses of its result. Refuses to fold anything the
// evaluator refuses - an overflowing add, a division by zero, an out of range
// shift - so the operation's raise stays in the program.
std::unique_ptr<FunctionPass> createConstantFoldingPass();

// Replaces uses of a value with the constant every path agrees it has,
// including through block parameters (a phi whose every incoming edge hands it
// the same constant). The definition is left in place; dead-value elimination
// removes it if that is safe.
std::unique_ptr<FunctionPass> createConstantPropagationPass();

// Rewrites algebraic identities (`x + 0`, `x * 1`, `x & ~0`, `x - x`) and
// self-comparisons. Only identities that cannot introduce or remove a raise
// are used: `x - x` is 0 with no overflow, but `x + x` is not rewritten to
// `x * 2`, because that one can overflow where the original did not.
std::unique_ptr<FunctionPass> createAlgebraicSimplificationPass();

// Removes conversions that no longer convert: a `widen` whose operand is
// already a double, a `refine` to the type the operand already has, a
// `null_check` on a type `null` is not a value of. Each is only removed when
// the check it carries is provably incapable of failing - the instruction is
// not deleted, its result is replaced by its operand, and dead-value
// elimination then drops it if nothing else needs it.
std::unique_ptr<FunctionPass> createRedundantConversionPass();

// Local (single-block) copy propagation: store-to-load forwarding, a second
// load of a slot nothing wrote in between, and a block parameter every
// incoming edge hands the same value. Each is a copy whose source is visible
// in the same block, which is what makes "this value and that value are the
// same" provable without an inter-block analysis.
std::unique_ptr<FunctionPass> createCopyPropagationPass();

// Simplifies control flow that has been decided: a branch on a constant, a
// switch on a constant, a branch whose two arms are the same block, and a jump
// through an empty block. Rewriting a branch can strand the block it no
// longer targets, which is why dead-block elimination follows it.
std::unique_ptr<FunctionPass> createBranchSimplificationPass();

// Deletes blocks nothing can enter. Unwind targets count as enterable: a
// catch block is live even though nothing falls through to it.
std::unique_ptr<FunctionPass> createDeadBlockEliminationPass();

// Deletes instructions whose result nothing reads and whose effect set allows
// deletion (see `isRemovableIfResultUnused`), plus stores to a slot no path
// reads. Iterates, because removing one instruction can orphan another.
std::unique_ptr<FunctionPass> createDeadValueEliminationPass();

// The one module pass: deletes functions the reachability report proves
// unreachable. The licence to delete is `ReachabilityReport::complete()` - an
// entry point exists, no reachable reflection invoke, no unpinned
// function-value call - so a program that can reach code by name or by value
// keeps every function, and the refusal is recorded in the pass's note rather
// than performed silently. Keeps every static initializer regardless, since
// reflection can trigger it. Renumbers the survivors and rewrites every
// function id the removal invalidates; a rewrite it cannot explain commits
// nothing.
std::unique_ptr<Pass> createEliminateDeadFunctionsPass();

// ---------------------------------------------------------------------------
// Pass registry
// ---------------------------------------------------------------------------
//
// Passes are created by name so a pipeline can be written down (`--mir-opt-
// passes=fold-constants,propagate-copies`) and so the default ordering lives
// in one place instead of being reassembled at every call site.
class PassRegistry {
public:
    using Factory = std::function<std::unique_ptr<Pass>()>;

    [[nodiscard]] static PassRegistry& instance();

    // Returns false when the name is taken, rather than overwriting a pass
    // another pipeline is already referring to.
    bool registerPass(std::string name, Factory factory);
    [[nodiscard]] bool has(const std::string& name) const;
    // Null when the name is unknown: an unknown pass is a typo in a pipeline
    // spec, and the caller reports it as such.
    [[nodiscard]] std::unique_ptr<Pass> create(const std::string& name) const;
    // Registered names, sorted, so `--help` output is stable.
    [[nodiscard]] std::vector<std::string> passNames() const;

private:
    PassRegistry() = default;
    std::unordered_map<std::string, Factory> factories_;
};

// Registers the passes above. Idempotent, so a test that builds a pipeline
// does not have to care whether something else already did.
void registerBuiltinPasses();

// ---------------------------------------------------------------------------
// Pass manager and pipeline
// ---------------------------------------------------------------------------

struct OptimizationOptions {
    // Run `verifyFunction` after every pass that changed a function. This is
    // the framework's central guard and is on by default; turning it off is
    // for measuring, never for shipping.
    bool verifyBetweenPasses{true};
    // Restore the pre-pass function when that verification fails. On by
    // default: a broken rewrite must not reach a backend.
    bool rollbackOnVerificationFailure{true};
    // Run `verifyModule` once at the end, with `options.verifier` as given.
    bool verifyAtEnd{true};
    // Keep the textual MIR from before and after each changed pass in the
    // report, for `--mir-opt-check` and for tests.
    bool keepSnapshots{false};
    // Directory to write one `.mir` file per changed pass into, named
    // `<sequence>-<pass>-<function>.mir`. Empty means write nothing.
    std::string snapshotDirectory;
    // How many times the whole pipeline may run. The default is enough for
    // the passes to reach each other's leftovers (folding enables branch
    // simplification, which enables block pruning, which enables more
    // folding); the loop exits as soon as an iteration changes nothing.
    std::size_t maxIterations{4};
    // Options for the final whole-module verification. Intermediate checks
    // relax `unreachableBlocksAreErrors`, because a pass legitimately
    // *creates* unreachable blocks for the pruning pass to remove.
    VerifierOptions verifier;

    [[nodiscard]] std::string describe() const;
};

struct PassRunRecord {
    std::string pass;
    std::string function;
    std::size_t iteration{0};
    bool changed{false};
    std::size_t instructionsBefore{0};
    std::size_t instructionsAfter{0};
    std::size_t blocksBefore{0};
    std::size_t blocksAfter{0};
    // False when verification was skipped (no change, or disabled).
    bool verified{false};
    // True when this pass broke the MIR and the function was restored.
    bool rolledBack{false};
    std::string verificationError;
    // What the pass did, in its own words.
    std::string note;

    [[nodiscard]] std::string describe() const;
};

// One textual MIR capture. `before` and `after` are the whole function as the
// printer renders it, which is what makes a pass's effect reviewable by a
// person rather than only by a test.
struct MirSnapshot {
    std::size_t sequence{0};
    std::string pass;
    std::string function;
    bool isBefore{true};
    std::string text;
};

struct OptimizationReport {
    std::vector<PassRunRecord> runs;
    std::vector<MirSnapshot> snapshots;
    std::size_t iterations{0};
    std::size_t rollbacks{0};
    std::size_t snapshotsWritten{0};
    // Result of the final whole-module verification (only meaningful when
    // `OptimizationOptions::verifyAtEnd` was set).
    bool verifiedAtEnd{false};
    VerificationReport finalVerification;
    // Analysis cache behaviour, so a report can show that repeated queries
    // really are hitting the cache.
    std::size_t analysesBuilt{0};
    std::size_t analysesReused{0};

    [[nodiscard]] bool changed() const;
    [[nodiscard]] size_t instructionsRemoved() const;
    [[nodiscard]] size_t blocksRemoved() const;
    // True when the module is safe to hand to a backend: nothing was rolled
    // back and the final verification (when requested) passed.
    [[nodiscard]] bool trustworthy() const;

    // A short summary: passes run, changes, rollbacks, verification.
    [[nodiscard]] std::string describe() const;
    // One line per pass run, the form `--mir-opt-check` prints.
    [[nodiscard]] std::string describeTrace() const;
};

class PassManager {
public:
    PassManager() = default;
    // Movable, not copyable: a pipeline owns its passes, and a copy would
    // leave two managers claiming the same ones.
    PassManager(PassManager&&) = default;
    PassManager& operator=(PassManager&&) = default;
    PassManager(const PassManager&) = delete;
    PassManager& operator=(const PassManager&) = delete;

    // Appends a pass. Order is the order of these calls, and the pipeline's
    // correctness depends on it - see `defaultPipeline()`.
    void addPass(std::unique_ptr<Pass> pass);
    // Looks the name up in the registry. Returns false for an unknown name
    // rather than silently running a shorter pipeline.
    bool addPassByName(const std::string& name);

    [[nodiscard]] const std::vector<std::unique_ptr<Pass>>& passes() const noexcept { return passes_; }
    // "1. fold-constants - ...", one line per pass.
    [[nodiscard]] std::string describePipeline() const;

    // Runs the pipeline over every function in the module, verifying between
    // passes and rolling back on failure, until nothing changes or
    // `options.maxIterations` is reached.
    [[nodiscard]] OptimizationReport run(Module& module, const OptimizationOptions& options = {});

    // The curated ordering, with the reason each pass sits where it does.
    [[nodiscard]] static PassManager defaultPipeline();
    // "default" is the curated pipeline; "none" is empty; anything else is
    // read as a comma-separated list of registry names.
    [[nodiscard]] static PassManager namedPipeline(const std::string& spec, std::string& error);

private:
    std::vector<std::unique_ptr<Pass>> passes_;
};

// Convenience entry point: the default pipeline, default options.
[[nodiscard]] OptimizationReport optimizeModule(Module& module, const OptimizationOptions& options = {});

// ---------------------------------------------------------------------------
// Before / after inspection
// ---------------------------------------------------------------------------
//
// A compact unified-ish diff of two textual MIR dumps. Line based, with a
// bounded LCS, because the question a person asks of a pass is "what did you
// change", and the answer has to fit on a screen.
[[nodiscard]] std::string diffLines(const std::string& before, const std::string& after,
                                    std::size_t contextLines = 2);

// Writes `text` to `<directory>/<name>`, creating the directory if needed.
// Returns false (and does not throw) when it cannot, because a debugging aid
// must never be the reason a compilation fails.
bool writeTextFile(const std::string& path, const std::string& text);

} // namespace zl::mir
