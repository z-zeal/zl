#pragma once
// ---------------------------------------------------------------------------
// Edges the bytecode backend materialises after lowering
// ---------------------------------------------------------------------------
//
// A few MIR instructions do not name a callee, yet translating them for the
// bytecode backend *dispatches*: a `shared_get` becomes a virtual call to the
// `get` method of `Shared`, a collection literal grows by calling `push`,
// `add` or `put`, and a `new_collection` of a class-layer collection invokes
// that collection's empty constructor. These are execution edges - functions
// whose bodies the program will run - recorded nowhere as call instructions,
// because the lowering that creates the dispatch is in the backend.
//
// Two consumers have to agree on the mapping:
//
//   * `vm_backend.cpp`, which emits through it and refuses (stubs) the
//     containing function when the named method is not in the module - the
//     fail-closed half of the contract;
//   * `reachability.cpp`, which keeps whatever these edges can reach live:
//     a function only ever called through a hidden edge is still callable,
//     and `eliminate-dead-functions` must not remove it.
//
// This header is the single source of the mapping. A future backend that
// starts dispatching another opcode adds the entry here, and the analysis and
// the emitter stay in step because neither one keeps its own list.

#include <cstddef>
#include <cstdint>
#include <string>

#include "zl/mir/function.hpp"
#include "zl/mir/instruction.hpp"
#include "zl/mir/type.hpp"

namespace zl::mir {

// The class-layer collection base of the MIR type `typeId`: "List", "Map" or
// "Set" - or empty for anything else, including raw native collections, which
// take different lowering paths.
[[nodiscard]] inline std::string classCollectionBase(const TypeArena& types, std::uint32_t typeId) {
    const Type* type = types.find(typeId);
    if (!type || type->kind != TypeKind::Object) return {};
    const std::size_t angle = type->name.find('<');
    const std::string base = angle == std::string::npos ? type->name : type->name.substr(0, angle);
    if (base == "List" || base == "Map" || base == "Set") return base;
    return {};
}

// The method by which the backend grows a class-layer collection of `base`
// with an `index_store`: list literals append through `push`, sets insert
// through `add`, maps through `put`. Null for a non-collection base.
[[nodiscard]] inline const char* collectionAppendMethod(const std::string& base) {
    if (base == "List") return "push";
    if (base == "Set") return "add";
    if (base == "Map") return "put";
    return nullptr;
}

// The `Shared` method the backend dispatches a shared-cell opcode to, or null
// when the opcode is not a shared-cell access.
[[nodiscard]] inline const char* sharedMethodFor(Opcode opcode) {
    switch (opcode) {
        case Opcode::SharedGet: return "get";
        case Opcode::SharedSet: return "setValue";
        case Opcode::SharedWithLock: return "withLock";
        default: return nullptr;
    }
}

// True when `function` is the empty constructor of a class-layer collection
// `base` that a `new_collection` of that type would pick: the same predicate
// the emitter scans with, so the analysis keeps what the emitter finds.
[[nodiscard]] inline bool isCollectionConstructor(const Function& function, const std::string& base) {
    return function.isConstructor && function.ownerClass == base && function.parameters.size() == 1;
}

} // namespace zl::mir
