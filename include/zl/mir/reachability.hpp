#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "zl/mir/function.hpp"

// ---------------------------------------------------------------------------
// Which functions a program can enter
// ---------------------------------------------------------------------------
//
// MIR knows every call a program makes, so "which functions can run" is a
// question the IR can answer without guessing - with one deliberate exception.
// The reflection family reads or enters code by name: the invoke forms take a
// `Method`/`Function`/`Constructor` value and call whatever it describes, and
// the enumeration forms (`Type.methods()` and friends) hand the module's
// function table to the program as data. A module that reaches either has no
// closed call graph *and* an observable function table, and this analysis says
// so instead of returning a plausible subset. The enumeration half is not
// paranoia: a removal that nothing can execute still changes what such a
// program prints, because the method list it prints is built from the function
// list the removal edited (`nativeIsReflective` in the catalog is the list).
//
// Everything here is an *over*-approximation, and the direction matters:
//
//   * virtual dispatch is resolved over the class hierarchy in `ClassLayout`, so
//     every override in every subtype is kept, not only the ones a whole-program
//     type derivation could prove reachable;
//   * a call whose class name is not in the module keeps every function with
//     that method name, matched on the bare method token the backend
//     dispatches by (`push`, `area` - not `Shape.area`);
//   * a static field's initializer is kept when the field is referenced, because
//     "lazily initialised" means the initializer runs at first access, not at
//     load time (the removal pass widens this to *every* initializer: see
//     `opt_dead_functions.cpp`);
//   * dispatch the *backend* materialises from instructions that name no
//     callee - shared-cell access, collection literal growth and
//     construction - is followed through the same table the emitter uses
//     (`backend_edges.hpp`), and a dispatch site that resolves to no
//     candidate anywhere makes the report incomplete rather than "closed to
//     nothing".
//
// Over-approximating is what makes the result usable for anything that would
// *remove* code: a function this analysis calls unreachable really is
// unreachable. Under-approximating would be a miscompile, so there is no case
// where the analysis answers "definitely not reachable" from an absence of
// evidence - only from the absence of an edge.
//
// The analysis is a question, not a transform: nothing here rewrites the module.
// `ReachabilityReport::complete()` is what a caller must check before acting on
// `functions`, and a module with reflection in it reports incomplete.
//
// ### The semantic contract of `complete()`
//
// Every instruction that can transfer execution to code is classified, and
// `complete() == true` **only when every possible execution edge relevant to
// this module has been statically accounted for** - that is, when the module
// has an entry point and every edge is one of the closed kinds below:
//
//   * **direct/static call** - `Call`, `InvokeStatic`, `InvokeSuper`: the target
//     function is recorded on the instruction;
//   * **virtual/interface dispatch** - `InvokeMethod`: resolved over the
//     recorded class hierarchy (over-approximated, see above);
//   * **known closure/function value** - `CallIndirect` (and `TaskSpawn`,
//     `ThreadStart`, the `*WithLock` family) through a value the analysis can
//     *prove* is one closure body: a single SSA definition, possibly through
//     a slot with exactly one store;
//   * **native opaque entry** - `CallNative` / `FfiCall` / `CallbackInvoke`
//     into a known symbol: the execution leaves this module's functions by
//     design, so it cannot enter a function this analysis forgot.
//
// The report is **incomplete** when any edge of an open kind is reachable:
//
//   * **indirect/unresolved call** - a call through a function value that
//     cannot be pinned to a single body (a function-valued parameter, a
//     block parameter, a static, a temp with any other definition, a slot
//     with zero or multiple stores). The value may hold *any* function the
//     caller chooses, so the reachable set is a lower bound;
//   * **reflection** - any native of the reflection family: an invoke form
//     makes the callee a runtime value, and an enumeration form makes the
//     function table itself observable, so removals are visible in output.
//
// An unresolved edge is never silently dropped and never "explained away" by
// assuming the operand's current lowering pattern: a callee that is provably
// known is enqueued, and everything else makes the report incomplete. A later
// function-value analysis can only *shrink* the set of unresolved edges (by
// proving more values to be single closures) - it cannot change what
// `complete()` means.

namespace zl::mir {

struct ReachabilityReport {
    // Every function the program can enter, including the entry point.
    std::unordered_set<FunctionId> functions;
    // False when the module has no entry point at all (a library): `functions`
    // is then empty and says nothing about the module.
    bool hasEntryPoint{false};
    // Set when a function that can run reaches a native of the reflection
    // family (one that enters code by name, or reads the function table as
    // data). `functions` is a lower bound in that case, and the table itself
    // is observable, so a caller that would delete anything must refuse.
    bool dynamicEntry{false};
    std::string dynamicEntryReason;
    // Set when a function that can run executes a function value it cannot
    // pin to a single closure body (an unresolved indirect call). Like
    // dynamicEntry, this makes `functions` a lower bound: the value may hold
    // any function the caller of that function chose.
    bool unresolvedCalls{false};
    // The first unresolved site, named: which function and which construct.
    std::string unresolvedCallReason;
    // Every unresolved site, counted - the report is a lower bound by at least
    // this many edges.
    std::size_t unresolvedCallCount{0};
    // Human-readable notes about the approximation: how many functions were kept
    // by hierarchy-wide dispatch resolution, and why.
    std::vector<std::string> notes;

    // True only when every possible execution edge relevant to this module has
    // been statically accounted for: an entry point exists, no reflection
    // native opens the graph, and no reachable call executes a function value
    // that is not provably a single closure body. That is the condition under
    // which `functions` is safe to use as a removal guarantee; in every other
    // case it is a lower bound and a caller that would delete anything must
    // refuse.
    [[nodiscard]] bool complete() const noexcept {
        return hasEntryPoint && !dynamicEntry && !unresolvedCalls;
    }
    [[nodiscard]] std::size_t size() const noexcept { return functions.size(); }
    [[nodiscard]] bool contains(FunctionId id) const { return functions.count(id) != 0; }

    // One line for a report: how many of the module's functions can run, which
    // entry point they start from, and the caveat that applies.
    [[nodiscard]] std::string describe(const Module& module) const;
};

// Computes the over-approximation described above.
[[nodiscard]] ReachabilityReport reachableFunctions(const Module& module);

} // namespace zl::mir
