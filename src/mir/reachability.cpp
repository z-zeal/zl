#include "zl/mir/reachability.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/compiler/native_catalog.hpp"
#include "zl/mir/backend_edges.hpp"

namespace zl::mir {

namespace {

// The class hierarchy as dispatch needs it.
//
// `closure(name)` answers "which classes can this value's runtime class be", and
// `supertypesOf(name)` answers "where can the method be declared". Resolving a
// virtual call needs both: `Derived d; d.m()` runs `Derived.m` when `Derived`
// overrides it and the inherited `Base.m` when it does not.
//
// Parents and interfaces are walked to a fixpoint, because an interface can
// extend an interface and a class can reach one through its parent. The type
// checker rules out cycles, but every walk still checks for revisiting, so a
// malformed module cannot make a diagnostic hang.
//
// The hierarchy is the analysis run's, not a global cache: it is built once per
// `reachableFunctions` call over the module it answers for, and the
// per-(receiver, method) dispatch results are cached inside it for the same run
// only.
// The name a dispatch site speaks: the bare method token, without the
// parameter list and without class qualification. The backend resolves a
// virtual site by exactly this token (`methodToken(fn.simpleName)` in
// vm_backend.cpp), so the analysis must match callees the same way - if the
// two spellings diverge, the analysis keeps a different function than the one
// the call site can actually run, and that is a hole in the removal argument
// rather than a missed optimisation.
[[nodiscard]] std::string nameToken(const std::string& text) {
    const std::size_t paren = text.find('(');
    const std::size_t dot = text.rfind('.', paren == std::string::npos ? text.size() : paren);
    const std::size_t start = dot == std::string::npos ? 0 : dot + 1;
    return paren == std::string::npos ? text.substr(start) : text.substr(start, paren - start);
}

// The dispatch token of a function: its `simpleName` ("get()") when lowering
// filled it, otherwise the qualified name with its parameter list stripped
// ("Shared.get(int)" -> "get"). Both spellings land on the bare token.
[[nodiscard]] std::string nameTokenOf(const Function& function) {
    return nameToken(function.simpleName.empty() ? function.name : function.simpleName);
}

class Hierarchy {
public:
    explicit Hierarchy(const Module& module) : module_(module) {
        for (const auto& layout : module.classes) layoutByName_.emplace(layout.name, &layout);
        for (const auto& interface : module.interfaces) interfaceBases_.emplace(interface.name, interface.bases);
        for (const auto& function : module.functions) {
            if (!function.ownerClass.empty()) methodsByClass_[function.ownerClass].push_back(function.id);
        }
        // Every class's supertypes, computed once, so the closure queries below
        // are set lookups rather than repeated graph walks.
        for (const auto& [name, layout] : layoutByName_) {
            (void)layout;
            (void)supertypesOf(name);
        }
        // The inverse relation, computed once from those same supertype sets:
        // every name -> the classes that can be its runtime value. `closure`
        // becomes a lookup plus a union instead of a scan of every class for
        // each distinct receiver - that scan is what made deep hierarchies cost
        // O(classes) per dispatch site.
        for (const auto& [className, layout] : layoutByName_) {
            (void)layout;
            for (const auto& supertype : supertypesOf(className)) subtypes_[supertype].insert(className);
        }
    }

    // Every class name that a value of static type `name` can actually be: the
    // name itself, plus its subtypes - and, when `name` is an interface, the
    // classes that implement it (transitively, through interface extends).
    [[nodiscard]] const std::unordered_set<std::string>& closure(const std::string& name) const {
        const auto cached = closure_.find(name);
        if (cached != closure_.end()) return cached->second;

        std::unordered_set<std::string> result{name};
        const auto subtypes = subtypes_.find(name);
        if (subtypes != subtypes_.end()) result.insert(subtypes->second.begin(), subtypes->second.end());
        return closure_.emplace(name, std::move(result)).first->second;
    }

    // Transitive parents and interfaces of one class, memoised.
    [[nodiscard]] const std::unordered_set<std::string>& supertypesOf(const std::string& name) const {
        const auto cached = supertypes_.find(name);
        if (cached != supertypes_.end()) return cached->second;

        std::unordered_set<std::string> supers;
        std::unordered_set<std::string> seen;
        std::vector<std::string> work{name};
        while (!work.empty()) {
            const std::string current = work.back();
            work.pop_back();
            if (!seen.insert(current).second) continue;

            const auto layout = layoutByName_.find(current);
            if (layout != layoutByName_.end()) {
                const auto& parent = layout->second->parent;
                if (!parent.empty() && parent != current) {
                    supers.insert(parent);
                    work.push_back(parent);
                }
                for (const auto& interfaceName : layout->second->interfaces) {
                    if (interfaceName == current) continue;
                    supers.insert(interfaceName);
                    work.push_back(interfaceName);
                }
            }
            const auto bases = interfaceBases_.find(current);
            if (bases != interfaceBases_.end()) {
                for (const auto& base : bases->second) {
                    if (base == current) continue;
                    supers.insert(base);
                    work.push_back(base);
                }
            }
        }
        // A class is never its own supertype; `closure` adds the name back.
        supers.erase(name);
        return supertypes_.emplace(name, std::move(supers)).first->second;
    }

    // The dispatch candidates for one (static type, method name) pair: every
    // function that a call of `methodName` on a receiver of static type
    // `className` can run. That is the methods of every class the receiver's
    // runtime value can be (the closure) plus every class/interface the method
    // could be declared in (the supertypes), filtered by declared name.
    //
    // Computed once per pair and cached for the lifetime of this analysis run,
    // so N dispatch sites with the same receiver cost the candidate search
    // once, not N times - while a pair never seen stays uncached, so the cache
    // cannot outlive the module it was built for.
    [[nodiscard]] const std::vector<FunctionId>& dispatchCandidates(const std::string& className,
                                                                    const std::string& methodName) const {
        const auto byMethod = dispatch_.find(className);
        if (byMethod != dispatch_.end()) {
            const auto found = byMethod->second.find(methodName);
            if (found != byMethod->second.end()) return found->second;
        }

        std::vector<FunctionId> candidates;
        const auto& runtimeClasses = closure(className);
        const auto& declaringClasses = supertypesOf(className);
        const auto consider = [&](const std::string& candidateClass) {
            const auto methods = methodsByClass_.find(candidateClass);
            if (methods == methodsByClass_.end()) return;
            for (const FunctionId method : methods->second) {
                const Function* callee = module_.function(method);
                if (callee == nullptr || nameTokenOf(*callee) != nameToken(methodName)) continue;
                candidates.push_back(method);
            }
        };
        for (const auto& candidateClass : runtimeClasses) consider(candidateClass);
        for (const auto& candidateClass : declaringClasses) consider(candidateClass);
        return dispatch_[className].emplace(methodName, std::move(candidates)).first->second;
    }

private:
    const Module& module_;
    std::unordered_map<std::string, const ClassLayout*> layoutByName_;
    std::unordered_map<std::string, std::vector<std::string>> interfaceBases_;
    std::unordered_map<std::string, std::vector<FunctionId>> methodsByClass_;
    // name -> classes whose transitive supertypes include name.
    std::unordered_map<std::string, std::unordered_set<std::string>> subtypes_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> supertypes_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> closure_;
    // (static type, method name) -> dispatch candidates, for this run only.
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::vector<FunctionId>>> dispatch_;
};

// --- function-value proofs --------------------------------------------------
//
// A `CallIndirect` (or a Task.spawn / Thread.start / withLock site) executes a
// *value*, not a named target. The analysis can prove such an edge to a single
// body only from SSA structure: the value has one definition, and that
// definition is a closure - possibly through a slot the function stores to
// exactly once. Anything else (a function-valued parameter, a block parameter,
// a static, a temp with any other definition, a slot with zero or multiple
// stores) may hold any function the caller chose, and the edge is unresolved.
//
// The proof is deliberately narrow: it never guesses from the value's type,
// its current lowering pattern, or its name. A later function-value analysis
// can prove more values to be single closures, shrinking the unresolved set;
// it cannot change what the classification means.
struct ValueProof {
    bool built{false};
    // TempId -> the instruction that defines the temp.
    std::unordered_map<std::uint32_t, const Instruction*> defOf;
    // SlotId -> every store to the slot, in block order.
    std::unordered_map<std::uint32_t, std::vector<const Instruction*>> storesTo;

    void build(const Function& function) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.result != kNoTemp) defOf[instruction.result] = &instruction;
                if (instruction.opcode == Opcode::Store) storesTo[instruction.slot].push_back(&instruction);
            }
        }
        built = true;
    }
};

std::optional<FunctionId> proveTemp(ValueProof& proof, const Function& function, const Operand& operand,
                                    std::unordered_set<std::uint32_t>& visitedSlots) {
    if (operand.kind != OperandKind::Temp) return std::nullopt;
    if (!proof.built) proof.build(function);
    const auto def = proof.defOf.find(operand.index);
    if (def == proof.defOf.end()) return std::nullopt;
    const Instruction* instruction = def->second;
    if (instruction->opcode == Opcode::MakeClosure) {
        if (instruction->target.function == kNoFunction) return std::nullopt;
        return instruction->target.function;
    }
    if (instruction->opcode == Opcode::Load) {
        // Follow the slot: provable only when the function stores to it
        // exactly once, and the stored value is itself provable. A slot read
        // before any store (zero stores) or written from several values
        // (multiple stores) can hold any function.
        if (!visitedSlots.insert(instruction->slot).second) return std::nullopt;
        const auto stores = proof.storesTo.find(instruction->slot);
        if (stores == proof.storesTo.end() || stores->second.size() != 1) return std::nullopt;
        const Instruction* store = stores->second.front();
        if (store->operands.empty()) return std::nullopt;
        return proveTemp(proof, function, store->operands[0], visitedSlots);
    }
    return std::nullopt;
}

// The single closure body `operand` is pinned to, or nullopt when it is not
// provable. `proof` is the per-function index, shared by every edge in the
// function; the slot-visit guard is per proof, so one value's chain cannot
// poison another's.
std::optional<FunctionId> provenClosureBody(ValueProof& proof, const Function& function,
                                            const Operand& operand) {
    std::unordered_set<std::uint32_t> visitedSlots;
    return proveTemp(proof, function, operand, visitedSlots);
}

std::string functionName(const Module& module, FunctionId id) {
    const Function* function = module.function(id);
    return function != nullptr ? function->name : std::string{"<missing>"};
}

} // namespace

std::string ReachabilityReport::describe(const Module& module) const {
    if (!hasEntryPoint) {
        return "no entry point: a library module, so nothing is known to run";
    }
    std::string text = std::to_string(functions.size()) + " of " +
                       std::to_string(module.functions.size()) +
                       " function(s) can run, from " + functionName(module, module.entryPoint);
    if (dynamicEntry) {
        text += "; more through reflection (" + dynamicEntryReason + ")";
    }
    if (unresolvedCalls) {
        text += "; more through " + std::to_string(unresolvedCallCount) +
                " function-value call(s) that are not pinned to a single closure body (" +
                unresolvedCallReason + ")";
    }
    return text;
}

ReachabilityReport reachableFunctions(const Module& module) {
    ReachabilityReport report;
    if (module.entryPoint == kNoFunction || module.function(module.entryPoint) == nullptr) {
        return report;
    }
    report.hasEntryPoint = true;

    const Hierarchy hierarchy(module);

    // Name indexes, built once: a dispatch site should not scan the module's
    // functions, and this analysis runs on modules with hundreds of them.
    std::unordered_map<std::string, std::vector<FunctionId>> functionsByName;
    for (const auto& function : module.functions) {
        // A name-based fallback lookup must answer whichever spelling a call
        // site carries: the bare method token ("speak") or the declared name
        // with its class qualification ("Cat.speak"). Overloads of one name
        // share a key on purpose - keeping all of them is the
        // over-approximation this analysis promises.
        functionsByName[nameTokenOf(function)].push_back(function.id);
    }

    std::size_t dispatchKept = 0;
    std::unordered_set<FunctionId> queued;
    std::vector<FunctionId> work;

    const auto enqueue = [&](FunctionId id) {
        if (id == kNoFunction || module.function(id) == nullptr) return;
        if (queued.insert(id).second) work.push_back(id);
    };
    const auto enqueueStaticInitializer = [&](StaticId id) {
        const StaticField* field = module.staticField(id);
        if (field != nullptr) enqueue(field->initializer);
    };
    // Static access names its field the way the verifier reads it: class plus
    // field name, not an id.
    const auto enqueueStaticInitializerOf = [&](const std::string& className, const std::string& fieldName) {
        if (className.empty() || fieldName.empty()) return;
        for (const auto& field : module.statics) {
            if (field.className == className && field.name == fieldName) {
                enqueue(field.initializer);
                return;
            }
        }
    };
    // The sound fallback when a dispatch site names a class or method this
    // module does not have: keep every function with that method name. An empty
    // answer here would be an absence of evidence read as evidence of absence,
    // so it is reported upward as an open edge rather than treated as a
    // resolved call to nothing.
    const auto enqueueByName = [&](const std::string& methodName) {
        const auto candidates = functionsByName.find(nameToken(methodName));
        if (candidates == functionsByName.end()) return std::size_t(0);
        dispatchKept += candidates->second.size();
        for (const FunctionId candidate : candidates->second) enqueue(candidate);
        return candidates->second.size();
    };
    // An execution edge the analysis cannot pin to a body. The edge is
    // recorded, not dropped: the report becomes incomplete, and a caller that
    // would remove anything must refuse.
    const auto noteUnresolvedEdge = [&](const Function& function, const char* construct) {
        report.unresolvedCalls = true;
        report.unresolvedCallCount++;
        if (report.unresolvedCallReason.empty()) {
            report.unresolvedCallReason = "'" + function.name + "' " + construct;
        }
    };
    // Executes the function value in operand slot `operandIndex` of `instruction`:
    // proven bodies are enqueued, unprovable values open the graph. `proof` is
    // the per-function index, built lazily on first use.
    const auto executeValue = [&](const Function& function, ValueProof& proof,
                                  const Instruction& instruction, std::size_t operandIndex,
                                  const char* construct) {
        if (operandIndex >= instruction.operands.size()) {
            // Malformed shape: the value is not there, so it cannot be pinned.
            noteUnresolvedEdge(function, construct);
            return;
        }
        const std::optional<FunctionId> body =
            provenClosureBody(proof, function, instruction.operands[operandIndex]);
        if (body.has_value()) {
            enqueue(*body);
            return;
        }
        noteUnresolvedEdge(function, construct);
    };

    // Resolve one dispatch site the way the backend will resolve it: over the
    // receiver's class hierarchy, falling back to "every function with that
    // method name", and - if neither answers - recording an open edge. A
    // dispatch site that resolves to nothing is never treated as "resolved to
    // no targets": either the backend finds a method and can run it, or it
    // refuses the function that reached it, and a removal analysis that called
    // that site closed would delete the callee the refusal needs.
    const auto keepDispatchEdge = [&](const Function& where, const std::string& className,
                                      const std::string& methodName) {
        if (methodName.empty()) {
            noteUnresolvedEdge(where, "dispatches a method with no name");
            return;
        }
        std::size_t keptHere = 0;
        if (!className.empty()) {
            const auto& candidates = hierarchy.dispatchCandidates(className, methodName);
            if (!candidates.empty()) {
                dispatchKept += candidates.size();
                for (const FunctionId candidate : candidates) enqueue(candidate);
                keptHere = candidates.size();
            }
        }
        if (keptHere == 0) keptHere = enqueueByName(methodName);
        if (keptHere == 0)
            noteUnresolvedEdge(where, "dispatches a method no function in this module can serve");
    };

    enqueue(module.entryPoint);

    while (!work.empty()) {
        const FunctionId id = work.back();
        work.pop_back();
        report.functions.insert(id);

        const Function* function = module.function(id);
        if (function == nullptr) continue;

        // The per-function index for value proofs, built lazily on the first
        // function-value edge in this function.
        ValueProof proof;

        for (const auto& block : function->blocks) {
            for (const auto& instruction : block.instructions) {
                switch (instruction.opcode) {                    // Closed edges: the target is recorded on the instruction.
                    case Opcode::Call:
                    case Opcode::InvokeSuper:
                    case Opcode::InvokeStatic:
                    case Opcode::MakeClosure:
                        enqueue(instruction.target.function);
                        break;
                    // Virtual/interface dispatch: closed by over-approximation
                    // over the recorded hierarchy.
                    case Opcode::InvokeMethod:
                        keepDispatchEdge(*function, instruction.target.className,
                                         instruction.target.methodName);
                        break;
                    // Indirect/unresolved call: the callee is a value, not a
                    // recorded target. Proven to a single closure body -> the
                    // edge is closed and the body is enqueued; anything else
                    // -> the report is incomplete. Never silently dropped.
                    case Opcode::CallIndirect:
                        executeValue(*function, proof, instruction, 0,
                                     "calls through a function value that is not pinned to a single closure body");
                        break;
                    // Function values the runtime executes for the program:
                    // the spawned task's closure, the worker thread's closure,
                    // the lock's critical-section closure. Same classification
                    // as CallIndirect - a value that cannot be pinned opens the
                    // graph, because the caller may have chosen any body.
                    case Opcode::TaskSpawn:
                        executeValue(*function, proof, instruction, 0,
                                     "runs a function value through Task.spawn that is not pinned to a single "
                                     "closure body");
                        break;
                    case Opcode::ThreadStart:
                        executeValue(*function, proof, instruction, 0,
                                     "runs a function value through Thread.start that is not pinned to a single "
                                     "closure body");
                        break;
                    case Opcode::MutexWithLock:
                        executeValue(*function, proof, instruction, 1,
                                     "runs a function value through Mutex.withLock that is not pinned to a single "
                                     "closure body");
                        break;
                    case Opcode::RwLockWithRead:
                        executeValue(*function, proof, instruction, 1,
                                     "runs a function value through RwLock.withRead that is not pinned to a single "
                                     "closure body");
                        break;
                    case Opcode::RwLockWithWrite:
                        executeValue(*function, proof, instruction, 1,
                                     "runs a function value through RwLock.withWrite that is not pinned to a single "
                                     "closure body");
                        break;
                    // Shared-cell access: the backend lowers each of these to
                    // a dispatch of the matching `Shared` method (and
                    // `withLock` additionally *runs* its callback operand).
                    // `sharedMethodFor` is the one list, shared with the
                    // emitter, so the analysis keeps exactly what the
                    // translation dispatches to.
                    case Opcode::SharedGet:
                    case Opcode::SharedSet:
                    case Opcode::SharedWithLock: {
                        if (const char* method = sharedMethodFor(instruction.opcode))
                            keepDispatchEdge(*function, "Shared", method);
                        if (instruction.opcode == Opcode::SharedWithLock)
                            executeValue(*function, proof, instruction, 1,
                                         "runs a function value through Shared.withLock that is not pinned to a "
                                         "single closure body");
                        break;
                    }
                    // Collection literals grow through the collection class's
                    // own methods - see `collectionAppendMethod`: an
                    // `index_store` on a class-layer List/Set/Map is compiled
                    // by the backend into a `push`/`add`/`put` dispatch, so
                    // those methods are live from this function even though no
                    // call instruction names them.
                    case Opcode::IndexStore: {
                        if (instruction.operands.empty()) break;
                        const std::string base =
                            classCollectionBase(module.types, instruction.operands[0].type);
                        if (base.empty()) break;
                        keepDispatchEdge(*function, base, collectionAppendMethod(base));
                        break;
                    }
                    // And the literal's construction dispatches to the
                    // collection class's empty constructor, found by the same
                    // predicate the emitter scans with.
                    case Opcode::NewCollection: {
                        const std::string base = classCollectionBase(module.types, instruction.resultType);
                        if (base.empty()) break;
                        std::size_t keptHere = 0;
                        for (const auto& candidate : module.functions) {
                            if (isCollectionConstructor(candidate, base)) {
                                enqueue(candidate.id);
                                ++keptHere;
                            }
                        }
                        if (keptHere == 0)
                            noteUnresolvedEdge(*function,
                                              "builds a literal of a collection class with no empty constructor");
                        break;
                    }
                    // Native opaque entries: execution leaves this module's
                    // functions by design (a known symbol), so they cannot
                    // enter a function this analysis forgot - except the
                    // reflection family, which both enters code by name and
                    // reads the function table as data, and either makes the
                    // table load bearing.
                    case Opcode::CallNative:
                        if (instruction.target.nativeId >= 0 &&
                            ::zl::nativeIsReflective(
                                static_cast<::zl::NativeId>(instruction.target.nativeId))) {
                            if (!report.dynamicEntry) {
                                report.dynamicEntry = true;
                                report.dynamicEntryReason =
                                    instruction.target.nativeName + " reads or enters code by name";
                            }
                        }
                        break;
                    case Opcode::StaticLoad:
                    case Opcode::StaticStore:
                        enqueueStaticInitializerOf(instruction.target.className, instruction.name);
                        break;
                    default:
                        break;
                }
                for (const auto& operand : instruction.operands) {
                    if (operand.kind == OperandKind::Static) enqueueStaticInitializer(operand.index);
                }
            }
        }
    }

    if (dispatchKept != 0) {
        report.notes.push_back("virtual dispatch resolved over the class hierarchy: " +
                               std::to_string(dispatchKept) + " override candidate(s) kept");
    }
    if (report.unresolvedCalls) {
        report.notes.push_back(std::to_string(report.unresolvedCallCount) +
                               " call(s) through function values could not be resolved statically: "
                               "the reachable set is a lower bound");
    }
    if (report.dynamicEntry) {
        report.notes.push_back("reflection makes the call graph open: this is a lower bound");
    }
    return report;
}

} // namespace zl::mir
