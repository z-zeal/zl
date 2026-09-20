#include "zl/compiler/module_loader.hpp"

#include <fstream>
#include <sstream>

#include "zl/lexer/lexer.hpp"
#include "zl/parser/parser.hpp"
#include "zl/compiler/builtin_library.hpp"

namespace zl {
namespace {
void stampSourceFile(AstNode* root, const std::string& sourceFile) {
    std::function<void(const AstNode*)> stampSource = [&](const AstNode* node) {
        if (!node) return;
        // The tree is owned and mutable here; forEachChild exposes a read-only walk.
        const_cast<AstNode*>(node)->sourceFile = sourceFile;
        switch (node->kind) {
            case NodeKind::Program:
                for (const auto& n : static_cast<const Program*>(node)->declarations) stampSource(n.get());
                for (const auto& n : static_cast<const Program*>(node)->imports) stampSource(n.get());
                break;
            case NodeKind::ClassDecl:
                for (const auto& n : static_cast<const ClassDecl*>(node)->members) stampSource(n.get());
                break;
            case NodeKind::DataDecl:
                for (const auto& n : static_cast<const DataDecl*>(node)->members) stampSource(n.get());
                break;
            case NodeKind::MemoryDecl:
                for (const auto& n : static_cast<const MemoryDecl*>(node)->members) stampSource(n.get());
                break;
            case NodeKind::FunctionDecl:
                stampSource(static_cast<const FunctionDecl*>(node)->body.get());
                break;
            default: forEachChild(node, stampSource); break;
        }
    };
    stampSource(root);
}
} // namespace


ModuleLoader::ModuleLoader(std::filesystem::path entryFile, std::vector<std::filesystem::path> extraRoots)
    : entryFile_(std::move(entryFile)), extraRoots_(std::move(extraRoots)) {
    sourceRoot_ = resolveSourceRoot();
}

ModuleLoader::ModuleLoader(std::filesystem::path entryFile, std::filesystem::path stdlibRoot)
    : ModuleLoader(std::move(entryFile),
                   stdlibRoot.empty() ? std::vector<std::filesystem::path>{}
                                       : std::vector<std::filesystem::path>{std::move(stdlibRoot)}) {}

std::filesystem::path ModuleLoader::resolveSourceRoot() const {
    return resolveProjectSourceRoot(entryFile_);
}

std::filesystem::path ModuleLoader::resolveImportPath(const ImportDecl& imp, const std::filesystem::path& importingFile) const {
    std::filesystem::path rel;
    for (const auto& segment : imp.pathSegments) rel /= segment;
    rel += ".zl";

    // Imports are first resolved relative to the file containing the import.
    // This supports both `import Helper` and local package-style imports such
    // as `import support.Helper` inside nested source trees. If no local file
    // exists, the normal project/stdlib/dependency roots below still apply.
    std::filesystem::path localPath = importingFile.parent_path() / rel;
    if (std::filesystem::exists(localPath)) return localPath;

    std::filesystem::path projectPath = sourceRoot_ / rel;
    if (std::filesystem::exists(projectPath)) return projectPath;

    for (const auto& root : extraRoots_) {
        if (root.empty()) continue;
        std::filesystem::path candidate = root / rel;
        if (std::filesystem::exists(candidate)) return candidate;
    }

    // No root has it - report every path actually tried, so "cannot
    // resolve import" doesn't leave the person guessing which root(s) were
    // searched. Substring-compatible with the existing "module error:
    // cannot resolve import" test expectation.
    std::string message = "cannot resolve import '" + imp.dottedName + "' - expected a file at " +
                           projectPath.string();
    for (const auto& root : extraRoots_) {
        if (root.empty()) continue;
        message += " or " + (root / rel).string();
    }
    throw ModuleError(message);
}

std::unique_ptr<Program> ModuleLoader::parseFile(const std::filesystem::path& filePath,
                                                   const std::string& expectedClassName) const {
    std::ifstream file(filePath);
    if (!file) {
        throw ModuleError("could not open file: " + filePath.string());
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    Lexer lexer(buffer.str());
    Parser parser(lexer.tokenize());
    auto program = parser.parse(); // may throw ParseError - let it propagate as-is
    const auto sourceFile = std::filesystem::absolute(filePath).lexically_normal().string();
    stampSourceFile(program.get(), sourceFile);


    // A loaded module may be backed by any named top-level type: class, data,
    // interface, enum, or memory declaration. The primary-name contract
    // applies to that declared type and helper declarations may coexist in
    // the same file.
    bool foundPrimaryType = false;
    for (const auto& decl : program->declarations) {
        switch (decl->kind) {
            case NodeKind::ClassDecl:
                foundPrimaryType = static_cast<const ClassDecl*>(decl.get())->name == expectedClassName;
                break;
            case NodeKind::DataDecl:
                foundPrimaryType = static_cast<const DataDecl*>(decl.get())->name == expectedClassName;
                break;
            case NodeKind::InterfaceDecl:
                foundPrimaryType = static_cast<const InterfaceDecl*>(decl.get())->name == expectedClassName;
                break;
            case NodeKind::MemoryDecl:
                foundPrimaryType = static_cast<const MemoryDecl*>(decl.get())->name == expectedClassName;
                break;
            default:
                break;
        }
        if (foundPrimaryType) break;
        // Enum declarations use their declared type name when present.
        if (decl->kind == NodeKind::EnumDecl) {
            const auto* e = static_cast<const EnumDecl*>(decl.get());
            if (e->name == expectedClassName) { foundPrimaryType = true; break; }
        }
    }
    if (!foundPrimaryType) {
        throw ParseError("file " + filePath.string() + " must define primary type '" +
                         expectedClassName + "'");
    }

    return program;
}

void ModuleLoader::loadInto(Program& merged, const std::filesystem::path& filePath, const std::string& dottedName) {
    std::string expectedClassName = filePath.stem().string();
    std::unique_ptr<Program> program = parseFile(filePath, expectedClassName);

    // ModuleGraph owns class provenance and import visibility. Keeping this
    // knowledge in the graph means flattening cannot lose which module owned
    // a declaration or which imports were visible from that module.
    graph_.registerDeclarations(*program, dottedName,
        [](const std::string& className, const std::string& firstOwner, const std::string& secondOwner) {
            return ModuleError("class '" + className + "' is defined in both " + firstOwner +
                               " and " + secondOwner);
        });

    // Move this file's own declarations into the merged program BEFORE
    // recursing into its imports, so the entry file's declarations always
    // land first (pre-order). Compiler::compile() picks the first func
    // named `main` it finds across the merged program, so this keeps the
    // entry file's own `main` authoritative even if some imported file also
    // happens to define a same-named func.
    for (auto& decl : program->declarations) {
        merged.declarations.push_back(std::move(decl));
    }

    for (const auto& importNode : program->imports) {
        const auto* imp = static_cast<const ImportDecl*>(importNode.get());

        if (graph_.isLoading(imp->dottedName)) {
            throw ModuleError("circular import detected: '" + imp->dottedName + "' (imported from " +
                               dottedName + ")");
        }
        if (graph_.isResolved(imp->dottedName)) {
            continue; // already loaded and merged in via an earlier import - don't merge it twice
        }

        std::filesystem::path importedPath = resolveImportPath(*imp, filePath); // throws ModuleError if unresolvable in any root

        graph_.recordResolved(imp->dottedName, importedPath);
        (void)graph_.beginLoading(imp->dottedName);
        loadInto(merged, importedPath, imp->dottedName);
        graph_.endLoading(imp->dottedName);
    }
}

std::string ModuleLoader::entryDottedName() const {
    std::filesystem::path abs = std::filesystem::absolute(entryFile_);
    std::filesystem::path rel = abs.lexically_relative(sourceRoot_);

    // lexically_relative yields a path whose first component is ".." if
    // `abs` isn't actually under sourceRoot_ - fall back to just the file's
    // own name. Compared component-wise (not via native()/string()) because
    // path::native() is std::wstring on Windows, which can't be compared
    // against a narrow ".." string literal.
    if (rel.empty() || (rel.begin() != rel.end() && *rel.begin() == "..")) {
        return abs.stem().string();
    }

    rel.replace_extension();
    std::string dotted;
    for (const auto& part : rel) {
        if (!dotted.empty()) dotted += ".";
        dotted += part.string();
    }
    return dotted;
}

std::unique_ptr<Program> ModuleLoader::load() {
    auto merged = std::make_unique<Program>();

    // Built-in classes are regular ZL source: parse them through the same
    // parser as user files, then merge them ahead of the entry program.
    std::size_t builtinIndex = 0;
    for (const auto source : builtinLibrarySources()) {
        Lexer lexer{std::string(source)};
        Parser parser(lexer.tokenize());
        std::unique_ptr<Program> builtins = parser.parse();
        stampSourceFile(builtins.get(), "<builtin:" + std::to_string(builtinIndex++) + ">");
        graph_.registerDeclarations(*builtins, "<builtin>",
            [](const std::string& className, const std::string& firstOwner, const std::string& secondOwner) {
                return ModuleError("class '" + className + "' is defined in both " + firstOwner + " and " + secondOwner);
            });
        for (auto& decl : builtins->declarations) {
            merged->declarations.push_back(std::move(decl));
        }
    }

    std::string entryName = entryDottedName();
    // Mark the entry file itself as "currently loading" under its own
    // dotted name (even though it was reached directly, not via an
    // `import`), so a cycle that loops back around to the entry file is
    // caught by the same module-graph loading check every other import goes through.
    (void)graph_.beginLoading(entryName);
    loadInto(*merged, entryFile_, entryName);
    graph_.endLoading(entryName);
    return merged;
}

} // namespace zl
