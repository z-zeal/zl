#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/compiler/semantic_types.hpp"

namespace zl {

// Compile-time class shape used by semantic analysis.
struct ClassFieldInfo {
    ZlType type;
    std::string className;
    AccessModifier access;
    bool isStatic{false};
    OwnershipKind ownership{OwnershipKind::GC};
    // Preserve callable shape when the field type is func(...): ... rather
    // than collapsing it to the FUNCTION tag.
    std::vector<ZlType> functionParamTypes;
    std::vector<std::string> functionParamClassNames;
    ZlType functionReturnType{ZlType::UNKNOWN};
    std::string functionReturnClassName;
    bool functionHasSignature{false};
};

struct ClassMethodInfo {
    std::vector<std::string> typeParams; // method-level type parameters; empty if not generic
    std::vector<ZlType> paramTypes;
    std::vector<std::string> paramClassNames;
    std::vector<bool> paramIsGeneric;
    ZlType returnType;
    std::string returnClassName;
    AccessModifier access;
    bool isDeprecated{false};
    bool isStatic{false};
    bool isAsync{false};
    // For parameters whose type is `func(...) : ...`, retain the callable
    // signature instead of collapsing it to the FUNCTION tag alone.
    std::vector<std::vector<ZlType>> functionParamTypes;
    std::vector<std::vector<std::string>> functionParamClassNames;
    std::vector<ZlType> functionReturnTypes;
    std::vector<std::string> functionReturnClassNames;
    std::vector<bool> functionParamHasSignature;
    // Return callable shape when this method itself returns func(...): ...
    std::vector<ZlType> returnFunctionParamTypes;
    std::vector<std::string> returnFunctionParamClassNames;
    ZlType returnFunctionReturnType{ZlType::UNKNOWN};
    std::string returnFunctionReturnClassName;
    bool returnFunctionHasSignature{false};
};

struct ClassShapeInfo {
    std::string name;
    std::vector<std::string> typeParams;
    std::unordered_map<std::string, std::vector<std::string>> typeParamInterfaceConstraints;
    std::string parentName;
    std::vector<TypeAnnotation> parentTypeArgs;
    std::vector<std::string> implementsNames;
    std::unordered_set<std::string> numericTypeParams;
    std::unordered_map<std::string, ClassFieldInfo> fields;
    std::vector<std::string> fieldOrder;
    std::unordered_map<std::string, std::vector<ClassMethodInfo>> methods;
    std::vector<ClassMethodInfo> constructors;
    bool isDeprecated{false};
    std::unordered_set<std::string> visibleImports;
    bool isDataType{false};
    bool isEnumType{false};
    // True for a `memory` declaration's shape: it exists so contract method
    // bodies typecheck, but nothing emits it - no layout, no dispatch, no
    // reflection, and `new` of it is refused (inferNewExpr).
    bool isMemoryDomain{false};
    std::vector<std::string> enumMembers;
};

struct InterfaceShapeInfo {
    std::string name;
    std::size_t line{0};
    std::vector<std::string> extendsNames;
    std::unordered_map<std::string, ClassMethodInfo> ownMethods;
    std::unordered_map<std::string, ClassMethodInfo> methods;
    bool methodsResolved{false};
    bool visiting{false};
};

// SemanticModel owns declaration shapes built before body checking. It is the
// semantic lookup surface for class/interface identity, inheritance, and
// member shape data; TypeChecker owns the rules applied to those shapes.
class SemanticModel {
public:
    using ClassMap = std::unordered_map<std::string, ClassShapeInfo>;
    using InterfaceMap = std::unordered_map<std::string, InterfaceShapeInfo>;

    [[nodiscard]] bool hasClass(const std::string& name) const { return classes_.count(name) != 0; }
    [[nodiscard]] bool hasInterface(const std::string& name) const { return interfaces_.count(name) != 0; }

    [[nodiscard]] const std::string& parentClassName(const std::string& name) const;
    [[nodiscard]] bool isSubclassOf(const std::string& className, const std::string& ancestorName) const;
    // Every declared class whose direct parent is `className`, by name and in a
    // stable order. The model owns the parent links, so callers that reason
    // about a complete case set (built-in sum exhaustiveness) never re-derive
    // the hierarchy by scanning shapes themselves.
    [[nodiscard]] std::vector<std::string> directSubclasses(const std::string& className) const;
    [[nodiscard]] bool interfaceExtends(const std::string& interfaceName, const std::string& ancestorName) const;
    [[nodiscard]] bool implementsInterface(const std::string& className, const std::string& interfaceName) const;
    [[nodiscard]] const ClassFieldInfo* findFieldInHierarchy(
        const std::string& className, const std::string& fieldName, std::string* outOwner = nullptr) const;
    [[nodiscard]] std::vector<std::pair<std::string, ClassMethodInfo>> collectMethodOverloads(
        const std::string& className, const std::string& methodName) const;
    [[nodiscard]] const ClassMethodInfo* findExactMethodInHierarchy(
        const std::string& className, const std::string& methodName,
        const std::vector<ZlType>& paramTypes, std::string* outOwner = nullptr) const;
    void setParentClass(const std::string& name, std::string parentName,
                        std::vector<TypeAnnotation> parentTypeArgs);
    void markNumericGenericTypeParam(const std::string& className, const std::string& typeParam);

    // Resolves and caches the complete inherited method contract for an
    // interface. The model owns the traversal, cycle detection, and merge
    // rules so callers never need to inspect interface storage directly.
    const InterfaceShapeInfo* resolveInterfaceMethods(const std::string& name);

    const ClassShapeInfo* findClass(const std::string& name) const;
    ClassShapeInfo* findClass(const std::string& name);
    const InterfaceShapeInfo* findInterface(const std::string& name) const;

    void defineClass(ClassShapeInfo shape);
    void defineInterface(InterfaceShapeInfo shape);

    void clear();

private:
    ClassMap classes_;
    InterfaceMap interfaces_;
};

} // namespace zl
