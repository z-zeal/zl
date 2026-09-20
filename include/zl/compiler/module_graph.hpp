#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "zl/parser/ast.hpp"

namespace zl {

// Owns the state that turns a set of parsed modules into one coherent
// import graph: module identity, DFS state, class provenance, and the
// per-class import visibility stamped onto flattened declarations.
// ModuleLoader remains responsible for filesystem resolution and parsing;
// ModuleGraph owns what has been discovered and merged.
class ModuleGraph {
public:
    // Returns true when this dotted module has already been merged.
    [[nodiscard]] bool isResolved(const std::string& dottedName) const;

    // Records a module as resolved to a concrete file. A second resolution
    // of the same dotted name is deliberately ignored by the graph caller.
    void recordResolved(const std::string& dottedName,
                        const std::filesystem::path& filePath);

    // DFS cycle state. beginLoading returns false if the module is already
    // on the active import stack.
    [[nodiscard]] bool beginLoading(const std::string& dottedName);
    [[nodiscard]] bool isLoading(const std::string& dottedName) const;
    void endLoading(const std::string& dottedName);

    // Registers type-declaration provenance (classes and memory declarations
    // share one namespace) and stamps each declaration with the imports that
    // were visible in its source module before that module is flattened.
    // Throws ModuleError through the supplied callback when duplicate
    // ownership is detected.
    template <typename ErrorFactory>
    void registerDeclarations(Program& program,
                              const std::string& dottedName,
                              ErrorFactory&& makeDuplicateError);

    [[nodiscard]] const std::unordered_map<std::string, std::filesystem::path>&
    resolvedModules() const { return resolvedByDottedName_; }

private:
    std::unordered_map<std::string, std::filesystem::path> resolvedByDottedName_;
    std::unordered_set<std::string> loading_;
    std::unordered_map<std::string, std::string> classOwners_;
};

template <typename ErrorFactory>
void ModuleGraph::registerDeclarations(Program& program,
                                       const std::string& dottedName,
                                       ErrorFactory&& makeDuplicateError) {
    std::unordered_set<std::string> ownImports;
    for (const auto& importNode : program.imports) {
        ownImports.insert(static_cast<const ImportDecl*>(importNode.get())->dottedName);
    }

    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl && decl->kind != NodeKind::MemoryDecl) continue;
        const std::string name = decl->kind == NodeKind::ClassDecl
            ? static_cast<const ClassDecl*>(decl.get())->name
            : static_cast<const MemoryDecl*>(decl.get())->name;
        auto [it, inserted] = classOwners_.emplace(name, dottedName);
        if (!inserted) {
            throw makeDuplicateError(name, it->second, dottedName);
        }
    }

    for (auto& decl : program.declarations) {
        if (decl->kind == NodeKind::ClassDecl) {
            static_cast<ClassDecl*>(decl.get())->visibleImports = ownImports;
        } else if (decl->kind == NodeKind::MemoryDecl) {
            static_cast<MemoryDecl*>(decl.get())->visibleImports = ownImports;
        }
    }
}

} // namespace zl
