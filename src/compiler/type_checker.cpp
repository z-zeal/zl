#include "zl/compiler/type_checker.hpp"
#include <iostream>
#include "zl/compiler/operator_rules.hpp"
#include "zl/compiler/generic_instantiation.hpp"
#include "zl/compiler/thread_capture.hpp"
#include "zl/parser/type_annotation.hpp"
#include "zl/vm/native.hpp"

#include <functional>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace zl {

namespace {

// The `string` method surface. Each entry is the spelling a program writes on
// a string receiver and the `String.*` native catalog entry it resolves to -
// the receiver binds that native's first parameter, the rest come from the
// call's argument list. This table is the whole definition: the checker
// validates arity and argument types against the catalog entry, and both
// backends read the resolved native back off the node. Nothing is implemented
// twice, and adding a method is adding a row here.
// See docs/language-guide.md#string-methods.
struct StringMethodMapping {
    const char* method;
    const char* native;
};

constexpr StringMethodMapping kStringMethods[] = {
    {"charAt",             "String.charAt"},
    {"codePointAt",        "String.codePointAt"},
    {"compare",            "String.compare"},
    {"compareIgnoreCase",  "String.compareIgnoreCase"},
    {"contains",           "String.contains"},
    {"endsWith",           "String.endsWith"},
    {"indexOf",            "String.indexOf"},
    {"indexOfFrom",        "String.indexOfFrom"},
    {"lastIndexOf",        "String.lastIndexOf"},
    {"length",             "String.length"},
    {"lower",              "String.lower"},
    // `repeat` is a loop keyword, so the native spelling is the only one
    // the lexer can deliver here - same reason Text.repeatText exists.
    {"repeatText",         "String.repeatText"},
    {"replace",            "String.replace"},
    {"split",              "String.split"},
    {"startsWith",         "String.startsWith"},
    {"substring",          "String.substring"},
    {"toFloat",            "String.toFloat"},
    {"toInt",              "String.toInt"},
    {"trim",               "String.trim"},
    {"trimEnd",            "String.trimEnd"},
    {"trimStart",          "String.trimStart"},
    {"upper",              "String.upper"},
    {"utf8ByteIndex",      "String.utf8ByteIndex"},
    {"utf8CharAt",         "String.utf8CharAt"},
    {"utf8CodePointAt",    "String.utf8CodePointAt"},
    {"utf8IndexFromByte",  "String.utf8IndexFromByte"},
    {"utf8Length",         "String.utf8Length"},
    {"utf8Reverse",        "String.utf8Reverse"},
    {"utf8Substring",      "String.utf8Substring"},
};

// nullptr when `method` is not part of the string method surface.
const StringMethodMapping* findStringMethodMapping(const std::string& method) {
    for (const auto& entry : kStringMethods) {
        if (method == entry.method) return &entry;
    }
    return nullptr;
}

// Every method name, for the diagnostic a miss produces - a wrong method name
// should teach the surface instead of only rejecting the call.
std::string stringMethodNames() {
    std::string names;
    for (const auto& entry : kStringMethods) {
        if (!names.empty()) names += ", ";
        names += entry.method;
    }
    return names;
}

bool genericCollectionCompatible(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) return true;
    if (from == to) return true;

    struct CollectionShape {
        std::string base;
        std::optional<std::size_t> fixedArraySize;
        std::string args;
    };

    auto parseShape = [](const std::string& s) -> CollectionShape {
        CollectionShape shape;
        const auto lt = s.find('<');
        const auto gt = s.rfind('>');
        const std::string head = lt == std::string::npos ? s : s.substr(0, lt);
        shape.args = (lt != std::string::npos && gt == s.size() - 1)
            ? s.substr(lt + 1, gt - lt - 1) : std::string();
        if (head.rfind("array[", 0) == 0 && head.back() == ']') {
            try {
                shape.base = "array";
                shape.fixedArraySize = static_cast<std::size_t>(std::stoull(
                    head.substr(6, head.size() - 7)));
                return shape;
            } catch (...) {
                // Preserve ordinary handling if metadata is malformed.
            }
        }
        shape.base = head;
        return shape;
    };

    const CollectionShape fs = parseShape(from);
    const CollectionShape ts = parseShape(to);
    if (fs.base != ts.base) return false;

    // Fixed-size and dynamic arrays share the same element contract. If both
    // sides are fixed-size, the length is part of assignability.
    if (fs.base == "array" && fs.fixedArraySize && ts.fixedArraySize &&
        *fs.fixedArraySize != *ts.fixedArraySize) return false;

    if (fs.args.empty() || ts.args.empty()) return true;
    if (fs.args == "unknown" || ts.args == "unknown") return true;

    auto split = [](const std::string& body) {
        std::vector<std::string> out;
        std::size_t start = 0, depth = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            if (i == body.size() || (body[i] == ',' && depth == 0)) {
                out.push_back(body.substr(start, i - start));
                start = i + 1;
                continue;
            }
            if (body[i] == '<') ++depth;
            else if (body[i] == '>') --depth;
        }
        return out;
    };
    const auto fparts = split(fs.args);
    const auto tparts = split(ts.args);
    if (fparts.size() != tparts.size()) return false;
    for (std::size_t i = 0; i < fparts.size(); ++i) {
        if (fparts[i] == "unknown" || tparts[i] == "unknown") continue;
        if (fparts[i] != tparts[i]) return false;
    }
    return true;
}
}

namespace {
bool sameMethodSignature(const ClassMethodInfo& a, const ClassMethodInfo& b) {
    if (a.returnType != b.returnType || a.returnClassName != b.returnClassName ||
        a.paramTypes != b.paramTypes || a.paramClassNames != b.paramClassNames) {
        return false;
    }
    if (a.functionParamTypes != b.functionParamTypes ||
        a.functionParamClassNames != b.functionParamClassNames ||
        a.functionReturnTypes != b.functionReturnTypes ||
        a.functionReturnClassNames != b.functionReturnClassNames ||
        a.functionParamHasSignature != b.functionParamHasSignature) {
        return false;
    }
    return a.returnFunctionParamTypes == b.returnFunctionParamTypes &&
           a.returnFunctionParamClassNames == b.returnFunctionParamClassNames &&
           a.returnFunctionReturnType == b.returnFunctionReturnType &&
           a.returnFunctionReturnClassName == b.returnFunctionReturnClassName &&
           a.returnFunctionHasSignature == b.returnFunctionHasSignature;
}
}

// ---------------------------------------------------------------------------
// SymbolTable
// ---------------------------------------------------------------------------

void SymbolTable::pushScope() {
    scopes_.emplace_back();
}

void SymbolTable::popScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

SymbolTable::VarInfo& SymbolTable::defineVar(const std::string& name, ZlType type, bool isConst, const std::string& className,
                             const std::vector<ZlType>& functionParamTypes,
                             ZlType functionReturnType,
                             ZlType taskValueType,
                             const std::string& taskValueClassName,
                             std::optional<int> fixedArraySize,
                             ZlType arrayElementType,
                             const std::vector<std::string>& functionCaptureNames,
                             bool functionUsesThis,
                             bool functionHasSignature,
                             OwnershipKind ownership) {
    return defineVar(name, VarInfo{type, isConst, ownership, className, functionParamTypes, {}, functionReturnType, "", false, taskValueType, taskValueClassName, functionCaptureNames, functionUsesThis,
                        functionHasSignature, fixedArraySize, arrayElementType});
}

SymbolTable::VarInfo& SymbolTable::defineVar(const std::string& name, VarInfo info) {
    if (scopes_.empty()) throw std::logic_error("variable definition without a scope");
    if (scopes_.back().count(name)) throw TypeCheckError("duplicate variable '" + name + "' in the same scope");
    info.storageName = "$local" + std::to_string(nextStorageId_++);
    return scopes_.back().emplace(name, std::move(info)).first->second;
}

void SymbolTable::refine(const std::string& name, VarInfo view) {
    // A pattern binding of the same spelling shadows the subject, rather
    // than changing the declaration/constness of the original variable.
    if (scopes_.back().count(name)) return;
    const auto source = lookupVar(name);
    if (!source) return;
    view.declaration = source->declaration ? source->declaration : std::make_shared<VarInfo>(*source);
    view.storageName = source->storageName;
    view.isConst = source->isConst;
    view.ownership = source->ownership;
    view.functionCaptureNames = source->functionCaptureNames;
    view.functionUsesThis = source->functionUsesThis;
    view.functionIsAsync = source->functionIsAsync;
    scopes_.back().emplace(name, std::move(view));
}

void SymbolTable::invalidate(const std::string& name) {
    const auto binding = lookupVar(name);
    if (!binding) return;
    const auto slot = binding->storageName;
    for (auto& scope : scopes_) for (auto& entry : scope) {
        auto& view = entry.second;
        if (view.storageName != slot || !view.declaration) continue;
        auto declaration = view.declaration;
        view = *declaration;
        view.declaration = std::move(declaration);
    }
}

void SymbolTable::joinRefinements(ScopeState entry, const std::vector<ScopeState>& exits) {
    restore(std::move(entry));
    for (std::size_t i = 0; i < scopes_.size(); ++i) for (auto& binding : scopes_[i]) {
        if (!binding.second.declaration) continue;
        for (const auto& exit : exits) {
            bool preserved = false;
            if (i < exit.size()) {
                const auto found = exit[i].find(binding.first);
                preserved = found != exit[i].end() && found->second.type == binding.second.type &&
                            found->second.className == binding.second.className;
            }
            if (!preserved) {
                auto declaration = binding.second.declaration;
                binding.second = *declaration;
                binding.second.declaration = std::move(declaration);
                break;
            }
        }
    }
}

void SymbolTable::setFunctionSignature(const std::string& name, std::vector<std::string> paramClassNames,
                                       std::string returnClassName, bool isAsync, bool hasSignature) {
    auto& info = binding(name);
    info.functionParamClassNames = std::move(paramClassNames);
    info.functionReturnClassName = std::move(returnClassName);
    info.functionIsAsync = isAsync;
    info.functionHasSignature = hasSignature;
}

std::optional<SymbolTable::VarInfo> SymbolTable::lookupVar(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return std::nullopt;
}

SymbolTable::VarInfo& SymbolTable::binding(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    throw std::logic_error("unknown variable binding: " + name);
}

std::string SymbolTable::VarInfo::runtimeTypeName() const {
    if (annotation) return describeTypeAnnotation(*annotation);
    const auto nameOf = [](ZlType type, const std::string& className) {
        return className.empty() ? zlTypeName(type) : className;
    };
    if (type == ZlType::FUNCTION && functionHasSignature) {
        std::string out = "func(";
        for (std::size_t i = 0; i < functionParamTypes.size(); ++i) {
            if (i) out += ",";
            out += nameOf(functionParamTypes[i], i < functionParamClassNames.size() ? functionParamClassNames[i] : "");
        }
        return out + "):" + nameOf(functionReturnType, functionReturnClassName);
    }
    if (type == ZlType::TASK && className.empty())
        return "Task<" + nameOf(taskValueType, taskValueClassName) + ">";
    return nameOf(type, className);
}

void SymbolTable::defineFunc(const FuncInfo& info) {
    functions_[info.name] = info;
}

std::optional<SymbolTable::FuncInfo>
SymbolTable::lookupFunc(const std::string& name) const {
    auto it = functions_.find(name);
    if (it != functions_.end()) return it->second;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// TypeChecker helpers
// ---------------------------------------------------------------------------

ZlType TypeChecker::resolveType(const TypeAnnotation& annotation, std::string* outClassName) {
    return typeResolver_.resolveType(annotation, currentClassName_, currentClassTypeParams_, outClassName);
}

SymbolTable::VarInfo TypeChecker::variableInfo(const TypeAnnotation& annotation) {
    SymbolTable::VarInfo info;
    info.type = resolveType(annotation, &info.className);
    info.annotation = annotation;
    info.fixedArraySize = annotation.fixedSize;
    if (info.type == ZlType::ARRAY && !annotation.typeArgs.empty())
        info.arrayElementType = resolveType(annotation.typeArgs.front());
    if (info.type == ZlType::TASK && !annotation.typeArgs.empty())
        info.taskValueType = resolveType(annotation.typeArgs.front(), &info.taskValueClassName);
    if (info.type == ZlType::FUNCTION) {
        info.functionHasSignature = annotation.functionHasSignature;
        for (const auto& param : annotation.functionParamTypes) {
            std::string name;
            info.functionParamTypes.push_back(resolveType(param, &name));
            info.functionParamClassNames.push_back(std::move(name));
        }
        if (annotation.functionReturnType)
            info.functionReturnType = resolveType(*annotation.functionReturnType, &info.functionReturnClassName);
    }
    return info;
}

void TypeChecker::validateOwnership(const TypeAnnotation& annotation, OwnershipKind ownership, std::size_t line) {
    if (ownership == OwnershipKind::GC) return;
    std::string className;
    const ZlType type = typeResolver_.resolveType(annotation, currentClassName_, currentClassTypeParams_, &className);
    const bool referenceLike = type == ZlType::OBJECT || type == ZlType::LIST || type == ZlType::MAP ||
                               type == ZlType::SET || type == ZlType::ARRAY || type == ZlType::FUNCTION ||
                               type == ZlType::TASK;
    if (!referenceLike) {
        throw TypeCheckError("ownership modifier requires a reference-like type at line " + std::to_string(line));
    }
    if (ownership == OwnershipKind::BORROW && type == ZlType::TASK) {
        throw TypeCheckError("borrow is not supported for task/thread handles at line " + std::to_string(line));
    }
}


static std::string canonicalTaskTypeName(ZlType valueType, const std::string& valueClassName) {
    const std::string valueName = valueClassName.empty() ? zlTypeName(valueType) : valueClassName;
    return "Task<" + valueName + ">";
}

bool TypeChecker::isAssignable(ZlType from, ZlType to, const std::string& fromClassName,
                                const std::string& toClassName) const {
    if (from == ZlType::UNKNOWN || to == ZlType::UNKNOWN) return true;
    if (from == ZlType::UNION) {
        const auto& members = typeResolver_.unionMembers(fromClassName);
        return std::all_of(members.begin(), members.end(), [&](const auto& member) {
            return isAssignable(member.type, to, member.className, toClassName);
        });
    }
    if (to == ZlType::UNION) {
        const auto& members = typeResolver_.unionMembers(toClassName);
        return std::any_of(members.begin(), members.end(), [&](const auto& member) {
            return isAssignable(from, member.type, fromClassName, member.className);
        });
    }
    if (from == to) {
        if (from == ZlType::FUNCTION && !fromClassName.empty() && !toClassName.empty())
            return fromClassName == toClassName;
        // Collection types carry canonical generic arguments in className even
        // though their runtime kind is LIST/MAP/SET/ARRAY. Preserve those
        // arguments through assignability so list<Field> cannot silently
        // collapse to list<string>, etc.
        if ((from == ZlType::LIST || from == ZlType::MAP || from == ZlType::SET || from == ZlType::ARRAY) &&
            (!fromClassName.empty() || !toClassName.empty())) {
            // One-sided generic information is intentionally compatible with a
            // bare collection type: bare list/map/set/array is an untyped
            // runtime slot. When both sides carry generic identities they must
            // match exactly.
            if (fromClassName.empty() || toClassName.empty()) return true;
            return genericCollectionCompatible(fromClassName, toClassName);
        }
        // OBJECT == OBJECT additionally needs the same concrete/interface
        // type, a superclass relationship, or an interface relationship.
        if (from == ZlType::OBJECT && !fromClassName.empty() && !toClassName.empty()) {
            if (fromClassName == toClassName) return true;
            if (semanticModel_.isSubclassOf(fromClassName, toClassName)) return true;
            if (semanticModel_.hasInterface(toClassName) && semanticModel_.implementsInterface(fromClassName, toClassName)) return true;
            if (semanticModel_.hasInterface(fromClassName) && semanticModel_.hasInterface(toClassName) &&
                semanticModel_.interfaceExtends(fromClassName, toClassName)) return true;
            return false;
        }
        if (from == ZlType::TASK && (!fromClassName.empty() || !toClassName.empty())) {
            if (fromClassName.empty() || toClassName.empty()) return true;
            return fromClassName == toClassName;
        }
        return true;
    }
    // NIL is only assignable to reference-like/runtime-nullable types.
    // Primitive numeric/bool values are not nullable.
    if (from == ZlType::NIL) {
        if (to == ZlType::OBJECT) {
            const auto* shape = semanticModel_.findClass(toClassName);
            if (shape && shape->isEnumType) return false;
        }
        switch (to) {
            case ZlType::STRING:
            case ZlType::LIST:
            case ZlType::MAP:
            case ZlType::SET:
            case ZlType::ARRAY:
            case ZlType::OBJECT:
            case ZlType::FUNCTION:
            case ZlType::TASK:
            case ZlType::UNKNOWN:
                return true;
            default:
                return false;
        }
    }
    // int -> double widening (numeric promotion).
    if (from == ZlType::INT && to == ZlType::DOUBLE) return true;
    // There's no dedicated array-literal syntax - a `[...]` collection
    // literal infers as LIST (and an empty `{...}` as SET; see
    // inferCollectionLiteral), and that same literal is the supported
    // construction form for array-typed variables (array[N]<T>). Both share
    // the same underlying runtime list representation, but an
    // explicitly-typed element mismatch must still be rejected.
    if ((from == ZlType::LIST || from == ZlType::SET) && to == ZlType::ARRAY) {
        if (!fromClassName.empty() && !toClassName.empty()) return genericCollectionCompatible(fromClassName, toClassName);
        return true;
    }
    // LIST and SET share the same underlying ListRef representation at runtime
    // (see collNewSet()), so allow them interchangeably only when their generic
    // element types agree. Bare collections remain compatible as untyped slots.
    if ((from == ZlType::LIST && to == ZlType::SET) ||
        (from == ZlType::SET && to == ZlType::LIST)) {
        if (!fromClassName.empty() && !toClassName.empty()) return genericCollectionCompatible(fromClassName, toClassName);
        return true;
    }
    return false;
}

std::vector<ResolvedTypeArg> TypeChecker::builtinSumCases(const std::string& subjectClassName) const {
    // The case set each built-in sum is closed over. The names are the classes
    // the standard library declares (src/compiler/builtin_library.cpp), listed
    // in the order the model sorts direct subclasses in.
    static const std::unordered_map<std::string, std::vector<std::string>> kBuiltinSums = {
        {"Option", {"None", "Some"}},
        {"Result", {"Err", "Ok"}},
    };
    const TypeName subject = parseTypeName(subjectClassName);
    if (subject.name.empty() || subject.args.empty()) return {};
    const auto sumIt = kBuiltinSums.find(subject.name);
    if (sumIt == kBuiltinSums.end()) return {};
    // A hierarchy is closed only while it is exactly the declared cases: a
    // program that extends `Result<int,string>` has added a shape the two case
    // patterns do not describe, so the plain pair stops being exhaustive. The
    // same check keeps a user class that merely reuses a case name from being
    // read as the built-in case.
    if (semanticModel_.directSubclasses(subject.name) != sumIt->second) return {};

    std::vector<ResolvedTypeArg> cases;
    cases.reserve(sumIt->second.size());
    for (const auto& caseName : sumIt->second) {
        const auto* shape = semanticModel_.findClass(caseName);
        if (shape == nullptr || shape->typeParams.size() != subject.args.size()) return {};
        // Both cases instantiate their parent positionally (`Some<T> extends
        // Option<T>`, `Ok<T,E> extends Result<T,E>`); a case that reorders or
        // transforms its arguments is not a shape this rule understands, so the
        // hierarchy stays open rather than being guessed at.
        if (shape->parentName != subject.name || shape->parentTypeArgs.size() != shape->typeParams.size()) return {};
        for (std::size_t i = 0; i < shape->typeParams.size(); ++i) {
            if (describeTypeAnnotation(shape->parentTypeArgs[i]) != shape->typeParams[i]) return {};
        }
        std::string caseType = caseName + "<";
        for (std::size_t i = 0; i < subject.args.size(); ++i) {
            if (i) caseType += ",";
            caseType += describeTypeName(subject.args[i]);
        }
        caseType += ">";
        cases.push_back({ZlType::OBJECT, std::move(caseType)});
    }
    return cases;
}

bool TypeChecker::typePatternCovers(const ResolvedTypeArg& member, ZlType patternType,
                                     const std::string& patternClass) const {
    if (isAssignable(member.type, patternType, member.className, patternClass)) return true;
    if (member.type != ZlType::OBJECT || patternType != ZlType::OBJECT) return false;
    // The case classes are only related to their sum by name until something
    // instantiates them, so a pattern naming the sum itself (`Result<int,string>`
    // covering `Ok<int,string>`) is decided on the template relationship plus
    // matching arguments - the same shape the runtime check accepts.
    const TypeName memberName = parseTypeName(member.className);
    const TypeName patternName = parseTypeName(patternClass);
    if (memberName.name.empty() || patternName.name.empty()) return false;
    if (memberName.args.size() != patternName.args.size()) return false;
    if (!semanticModel_.isSubclassOf(memberName.name, patternName.name)) return false;
    for (std::size_t i = 0; i < memberName.args.size(); ++i) {
        if (describeTypeName(memberName.args[i]) != describeTypeName(patternName.args[i])) return false;
    }
    return true;
}

void TypeChecker::fillTypePatternArgs(TypeAnnotation& pattern, const std::string& subjectClassName) const {
    if (!pattern.typeArgs.empty() || pattern.name.empty() || subjectClassName.empty()) return;
    const auto* shape = semanticModel_.findClass(pattern.name);
    if (shape == nullptr || shape->typeParams.empty()) return;
    const TypeName subject = parseTypeName(subjectClassName);
    if (subject.name.empty() || subject.args.empty()) return;

    // Walk from the pattern's class up to the class the subject instantiates,
    // carrying each parent's argument list - written in terms of the child's
    // parameters - down into bindings for the ancestor's parameters.
    std::unordered_map<std::string, std::string> bindings;
    std::string current = pattern.name;
    const ClassShapeInfo* ancestor = nullptr;
    for (int depth = 0; depth < 64; ++depth) {
        const auto* child = semanticModel_.findClass(current);
        if (child == nullptr || child->parentName.empty()) return;
        const auto* parent = semanticModel_.findClass(child->parentName);
        if (parent == nullptr) return;
        std::unordered_map<std::string, std::string> next;
        for (std::size_t i = 0; i < child->parentTypeArgs.size() && i < parent->typeParams.size(); ++i) {
            next.emplace(parent->typeParams[i],
                         substituteTypeParams(describeTypeAnnotation(child->parentTypeArgs[i]), bindings));
        }
        bindings = std::move(next);
        current = parent->name;
        if (current == subject.name) { ancestor = parent; break; }
        // An instantiated ancestor may itself carry arguments; `findClass`
        // resolves the template for those spellings, so continue from the
        // template's own name.
        const auto open = current.find('<');
        if (open != std::string::npos) current = current.substr(0, open);
    }
    if (ancestor == nullptr) return;

    // The subject fixes each ancestor parameter. A pattern parameter is only
    // recoverable when that argument is exactly that bare parameter - for
    // example `Wrapper<A> extends Base<list<A>>` cannot be inverted, and the
    // pattern then keeps the spelling the user wrote.
    std::unordered_map<std::string, std::string> argsByParam;
    for (std::size_t i = 0; i < ancestor->typeParams.size() && i < subject.args.size(); ++i) {
        const auto binding = bindings.find(ancestor->typeParams[i]);
        if (binding == bindings.end()) continue;
        if (std::find(shape->typeParams.begin(), shape->typeParams.end(), binding->second) == shape->typeParams.end()) continue;
        argsByParam.emplace(binding->second, describeTypeName(subject.args[i]));
    }
    if (argsByParam.size() != shape->typeParams.size()) return;

    pattern.typeArgs.clear();
    for (const auto& param : shape->typeParams) {
        pattern.typeArgs.push_back(typeAnnotationFromName(parseTypeName(argsByParam.at(param))));
    }
}

void TypeChecker::typeError(const std::string& message, std::size_t line) {
    throw TypeCheckError("type error (line " + std::to_string(line) + "): " + message);
}

void TypeChecker::typeWarning(const std::string& message, std::size_t line) {
    std::cerr << "warning (line " << line << "): " << message << "\n";
}

bool TypeChecker::hasAnnotation(const std::vector<Annotation>& annotations, const std::string& name) {
    return findAnnotation(annotations, name) != nullptr;
}

const Annotation* TypeChecker::findAnnotation(const std::vector<Annotation>& annotations, const std::string& name) {
    for (const auto& a : annotations) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

bool TypeChecker::deprecationSuppressed() const {
    return classSuppressesDeprecation_ || functionSuppressesDeprecation_;
}

void TypeChecker::warnIfDeprecatedMethod(const ClassMethodInfo& method, const std::string& ownerClassName,
                                          const std::string& methodName, std::size_t line) {
    if (!method.isDeprecated || deprecationSuppressed()) return;
    typeWarning("'" + ownerClassName + "." + methodName + "' is deprecated", line);
}

void TypeChecker::warnIfDeprecatedClass(const std::string& className, std::size_t line) {
    const auto* it = semanticModel_.findClass(className);
    if (it == nullptr || !it->isDeprecated || deprecationSuppressed()) return;
    typeWarning("class '" + className + "' is deprecated", line);
}

void TypeChecker::checkAccess(const std::string& className, const std::string& memberName,
                               AccessModifier access, bool isMethod, std::size_t line) const {
    if (access == AccessModifier::PUBLIC) return; // always fine

    if (currentClassName_ == className) return; // accessing our own class's member

    // Inside a generic template body, `this` carries the self-parameterized
    // name (`Shared<T>`) while the enclosing class is still the template
    // (`Shared`). That is the same declaring class, not a foreign one.
    const auto templateName = [](const std::string& name) {
        const auto open = name.find('<');
        return open == std::string::npos ? name : name.substr(0, open);
    };
    if (!currentClassTypeParams_.empty() && templateName(currentClassName_) == templateName(className)) return;

    // PRIVATE and DEFAULT (no modifier written - the roadmap's stated
    // default) are only accessible from inside the declaring class. PROTECTED
    // additionally allows any subclass of the declaring class.
    if (access == AccessModifier::PROTECTED && semanticModel_.isSubclassOf(currentClassName_, className)) {
        return;
    }

    const char* kind = isMethod ? "method" : "field";
    const char* modifierName = access == AccessModifier::PROTECTED ? "protected" : "private";
    typeError(
        std::string(modifierName) + " " + kind + " '" + memberName + "' of class '" + className +
        "' is not accessible " + (currentClassName_.empty() ? "here" : "from class '" + currentClassName_ + "'"),
        line);
}

std::string TypeChecker::paramTypesSuffix(const std::vector<ZlType>& paramTypes) {
    std::string s = "(";
    for (std::size_t i = 0; i < paramTypes.size(); ++i) {
        if (i) s += ",";
        s += zlTypeName(paramTypes[i]);
    }
    s += ")";
    return s;
}

std::string TypeChecker::methodParamSignature(const ClassMethodInfo& method) {
    std::string s = "(";
    for (std::size_t i = 0; i < method.paramTypes.size(); ++i) {
        if (i) s += ",";
        const bool generic = i < method.paramIsGeneric.size() && method.paramIsGeneric[i];
        if (generic) {
            s += "<generic:";
            s += (i < method.paramClassNames.size() ? method.paramClassNames[i] : "?");
            s += ">";
        } else if (method.paramTypes[i] == ZlType::OBJECT && i < method.paramClassNames.size() &&
                   !method.paramClassNames[i].empty()) {
            s += "object:" + method.paramClassNames[i];
        } else {
            s += zlTypeName(method.paramTypes[i]);
        }
    }
    s += ")";
    return s;
}

std::string TypeChecker::instantiateGenericClass(const std::string& genericName,
                                                   const std::vector<ResolvedTypeArg>& typeArgs,
                                                   std::size_t line) {
    return typeResolver_.instantiateGenericClass(genericName, typeArgs, line);
}

ClassMethodInfo TypeChecker::instantiateGenericMethod(const ClassMethodInfo& method,
                                                       const std::vector<ResolvedTypeArg>& typeArgs,
                                                       std::size_t line) {
    if (method.typeParams.size() != typeArgs.size()) {
        typeError("generic method takes " + std::to_string(method.typeParams.size()) +
                  " type argument(s), got " + std::to_string(typeArgs.size()), line);
    }
    std::unordered_map<std::string, std::string> bindings;
    for (std::size_t i = 0; i < method.typeParams.size(); ++i) {
        const auto& arg = typeArgs[i];
        bindings[method.typeParams[i]] = arg.className.empty() ? zlTypeName(arg.type) : arg.className;
    }
    auto substituteNamed = [&](ZlType type, const std::string& className) {
        const std::string source = className.empty() ? zlTypeName(type) : className;
        const std::string substituted = substituteTypeParams(source, bindings);
        std::string outClass;
        const ZlType outType = resolveType(typeAnnotationFromName(parseTypeName(substituted)), &outClass);
        return std::pair<ZlType, std::string>{outType, outClass};
    };

    ClassMethodInfo out = method;
    {
        auto [t, n] = substituteNamed(out.returnType, out.returnClassName);
        out.returnType = t;
        out.returnClassName = n;
    }
    for (std::size_t i = 0; i < out.paramTypes.size(); ++i) {
        const std::string cls = i < out.paramClassNames.size() ? out.paramClassNames[i] : std::string();
        auto [t, n] = substituteNamed(out.paramTypes[i], cls);
        out.paramTypes[i] = t;
        if (i < out.paramClassNames.size()) out.paramClassNames[i] = n;
        else out.paramClassNames.push_back(n);
    }
    // paramIsGeneric stays as on the unsubstituted original so dispatch remains GENERIC_OBJECT.
    for (std::size_t i = 0; i < out.functionParamTypes.size(); ++i) {
        for (std::size_t p = 0; p < out.functionParamTypes[i].size(); ++p) {
            const std::string cls = (i < out.functionParamClassNames.size() &&
                                     p < out.functionParamClassNames[i].size())
                ? out.functionParamClassNames[i][p] : std::string();
            auto [t, n] = substituteNamed(out.functionParamTypes[i][p], cls);
            out.functionParamTypes[i][p] = t;
            if (i < out.functionParamClassNames.size() && p < out.functionParamClassNames[i].size())
                out.functionParamClassNames[i][p] = n;
        }
        if (i < out.functionReturnTypes.size()) {
            const std::string cls = i < out.functionReturnClassNames.size()
                ? out.functionReturnClassNames[i] : std::string();
            auto [t, n] = substituteNamed(out.functionReturnTypes[i], cls);
            out.functionReturnTypes[i] = t;
            if (i < out.functionReturnClassNames.size()) out.functionReturnClassNames[i] = n;
        }
    }
    if (out.returnFunctionHasSignature) {
        for (std::size_t i = 0; i < out.returnFunctionParamTypes.size(); ++i) {
            const std::string cls = i < out.returnFunctionParamClassNames.size()
                ? out.returnFunctionParamClassNames[i] : std::string();
            auto [t, n] = substituteNamed(out.returnFunctionParamTypes[i], cls);
            out.returnFunctionParamTypes[i] = t;
            if (i < out.returnFunctionParamClassNames.size()) out.returnFunctionParamClassNames[i] = n;
        }
        auto [t, n] = substituteNamed(out.returnFunctionReturnType, out.returnFunctionReturnClassName);
        out.returnFunctionReturnType = t;
        out.returnFunctionReturnClassName = n;
    }
    return out;
}

TypeChecker::PreparedGenericOverloads TypeChecker::prepareGenericOverloads(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<TypeAnnotation>& typeArgs,
        const std::string& methodName,
        std::size_t line) {
    PreparedGenericOverloads prepared;
    const bool hasTypeArgs = !typeArgs.empty();
    std::vector<ResolvedTypeArg> resolved;
    if (hasTypeArgs) {
        resolved.reserve(typeArgs.size());
        for (const auto& ann : typeArgs) {
            std::string className;
            const ZlType type = resolveType(ann, &className);
            resolved.push_back(ResolvedTypeArg{type, className});
            prepared.resolvedTypeArgNames.push_back(className.empty() ? zlTypeName(type) : className);
        }
    }
    bool sawGeneric = false;
    for (const auto& candidate : candidates) {
        const bool isGeneric = !candidate.second.typeParams.empty();
        if (isGeneric) sawGeneric = true;
        if (hasTypeArgs) {
            if (!isGeneric) continue;
            if (candidate.second.typeParams.size() != typeArgs.size()) continue;
            prepared.originals.push_back(&candidate.second);
            prepared.candidates.emplace_back(
                candidate.first, instantiateGenericMethod(candidate.second, resolved, line));
        } else if (!isGeneric) {
            prepared.originals.push_back(&candidate.second);
            prepared.candidates.push_back(candidate);
        }
    }
    if (prepared.candidates.empty()) {
        if (hasTypeArgs) {
            typeError("no matching generic overload of '" + methodName +
                      "' for the given type arguments", line);
        }
        if (sawGeneric) {
            typeError("generic method '" + methodName + "' requires explicit type arguments", line);
        }
    }
    return prepared;
}

void TypeChecker::mergeMethodTypeParams(const std::vector<std::string>& methodParams, std::size_t line) {
    for (const auto& name : methodParams) {
        if (std::find(currentClassTypeParams_.begin(), currentClassTypeParams_.end(), name)
            != currentClassTypeParams_.end()) {
            typeError("method type parameter '" + name +
                      "' collides with class type parameter '" + name + "'", line);
        }
        currentClassTypeParams_.push_back(name);
    }
}

bool TypeChecker::isCurrentGenericTypeParam(const std::string& name) const {
    return typeResolver_.isCurrentGenericTypeParam(name, currentClassTypeParams_);
}


void TypeChecker::requireNumericGenericTypeParam(const std::string& name, std::size_t line) {
    (void)line;
    typeResolver_.requireNumericGenericTypeParam(name, currentClassName_, currentClassTypeParams_);
}



DispatchSignature TypeChecker::dispatchSignature(const std::string& methodName, const ClassMethodInfo& method) {
    DispatchSignature signature;
    signature.name = methodName;
    signature.parameters.reserve(method.paramTypes.size());
    for (std::size_t i = 0; i < method.paramTypes.size(); ++i) {
        const bool isGeneric = i < method.paramIsGeneric.size() && method.paramIsGeneric[i];
        if (isGeneric) {
            signature.parameters.push_back({DispatchTypeKind::GENERIC_OBJECT, {}});
            continue;
        }
        if (method.paramTypes[i] == ZlType::OBJECT && i < method.paramClassNames.size() &&
            !method.paramClassNames[i].empty()) {
            const std::string& className = method.paramClassNames[i];
            if (className.find('<') != std::string::npos) {
                // A concrete instantiation: keep the generic class's base name
                // so declarations over different generic classes (`Option<int>`
                // vs `List<int>`) never collapse into one dispatch identity.
                const auto open = className.find('<');
                signature.parameters.push_back({DispatchTypeKind::GENERIC_OBJECT,
                                                className.substr(0, open)});
            } else {
                signature.parameters.push_back({DispatchTypeKind::OBJECT, className});
            }
            continue;
        }
        switch (method.paramTypes[i]) {
            case ZlType::INT: signature.parameters.push_back({DispatchTypeKind::INT, {}}); break;
            case ZlType::DOUBLE: signature.parameters.push_back({DispatchTypeKind::DOUBLE, {}}); break;
            case ZlType::STRING: signature.parameters.push_back({DispatchTypeKind::STRING, {}}); break;
            case ZlType::BOOL: signature.parameters.push_back({DispatchTypeKind::BOOL, {}}); break;
            case ZlType::VOID_TYPE: signature.parameters.push_back({DispatchTypeKind::VOID_TYPE, {}}); break;
            case ZlType::LIST: signature.parameters.push_back({DispatchTypeKind::LIST, {}}); break;
            case ZlType::ARRAY: signature.parameters.push_back({DispatchTypeKind::ARRAY, {}}); break;
            case ZlType::SET: signature.parameters.push_back({DispatchTypeKind::SET, {}}); break;
            case ZlType::MAP: signature.parameters.push_back({DispatchTypeKind::MAP, {}}); break;
            case ZlType::FUNCTION: signature.parameters.push_back({DispatchTypeKind::FUNCTION, {}}); break;
            case ZlType::TASK: signature.parameters.push_back({DispatchTypeKind::GENERIC_OBJECT, "Task"}); break;
            case ZlType::UNION: signature.parameters.push_back({DispatchTypeKind::OBJECT, method.paramClassNames.at(i)}); break;
            case ZlType::NIL:
            case ZlType::OBJECT:
            case ZlType::UNKNOWN:
                signature.parameters.push_back({DispatchTypeKind::GENERIC_OBJECT, {}});
                break;
        }
    }
    return signature;
}

const ClassMethodInfo* TypeChecker::resolveOverload(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<ZlType>& argTypes, const std::vector<std::string>& argClassNames,
        const std::string& methodName, std::size_t line, std::string* outOwner) const {
    return OverloadResolver(*this).resolve(candidates, argTypes, argClassNames, methodName, line, outOwner);
}

namespace {

void collectFunctionLocals(const AstNode* node, std::unordered_set<std::string>& locals) {
    if (!node) return;
    switch (node->kind) {
        case NodeKind::VarDecl: {
            const auto* n = static_cast<const VarDecl*>(node);
            locals.insert(n->name);
            collectFunctionLocals(n->initializer.get(), locals);
            return;
        }
        case NodeKind::ForStmt: {
            const auto* n = static_cast<const ForStmt*>(node);
            if (!n->varName.empty()) locals.insert(n->varName);
            collectFunctionLocals(n->start.get(), locals);
            collectFunctionLocals(n->end.get(), locals);
            collectFunctionLocals(n->step.get(), locals);
            collectFunctionLocals(n->body.get(), locals);
            return;
        }
        case NodeKind::TryStmt: {
            const auto* n = static_cast<const TryStmt*>(node);
            collectFunctionLocals(n->tryBlock.get(), locals);
            for (const auto& c : n->catches) {
                if (!c.varName.empty()) locals.insert(c.varName);
                collectFunctionLocals(c.block.get(), locals);
            }
            collectFunctionLocals(n->finallyBlock.get(), locals);
            return;
        }
        case NodeKind::BlockStmt: {
            const auto* n = static_cast<const BlockStmt*>(node);
            for (const auto& stmt : n->statements) collectFunctionLocals(stmt.get(), locals);
            return;
        }
        case NodeKind::IfStmt: {
            const auto* n = static_cast<const IfStmt*>(node);
            for (const auto& b : n->branches) {
                collectFunctionLocals(b.condition.get(), locals);
                collectFunctionLocals(b.body.get(), locals);
            }
            collectFunctionLocals(n->elseBody.get(), locals);
            return;
        }
        case NodeKind::WhileStmt: {
            const auto* n = static_cast<const WhileStmt*>(node);
            collectFunctionLocals(n->condition.get(), locals);
            collectFunctionLocals(n->body.get(), locals);
            return;
        }
        case NodeKind::RepeatStmt: {
            const auto* n = static_cast<const RepeatStmt*>(node);
            collectFunctionLocals(n->body.get(), locals);
            collectFunctionLocals(n->condition.get(), locals);
            return;
        }
        case NodeKind::ExprStmt: {
            collectFunctionLocals(static_cast<const ExprStmt*>(node)->expression.get(), locals);
            return;
        }
        case NodeKind::ReturnStmt: {
            collectFunctionLocals(static_cast<const ReturnStmt*>(node)->value.get(), locals);
            return;
        }
        case NodeKind::ThrowStmt: {
            collectFunctionLocals(static_cast<const ThrowStmt*>(node)->value.get(), locals);
            return;
        }
        case NodeKind::LogStmt: {
            collectFunctionLocals(static_cast<const LogStmt*>(node)->argument.get(), locals);
            return;
        }
        case NodeKind::LogExpr: {
            collectFunctionLocals(static_cast<const LogExpr*>(node)->argument.get(), locals);
            return;
        }
        default:
            return;
    }
}

const FunctionDecl* asFunctionDecl(const AstNode* node) {
    if (!node || node->kind != NodeKind::FunctionDecl) return nullptr;
    return static_cast<const FunctionDecl*>(node);
}


} // namespace

void TypeChecker::indexNamedFunctions(const Program& program) {
    namedFunctions_.clear();
    namedFunctionConfinement_.clear();

    for (const auto& decl : program.declarations) {
        const ClassDecl* cls = nullptr;
        const DataDecl* data = nullptr;
        if (decl->kind == NodeKind::ClassDecl) cls = static_cast<const ClassDecl*>(decl.get());
        else if (decl->kind == NodeKind::DataDecl) data = static_cast<const DataDecl*>(decl.get());
        else continue;

        const std::string owner = cls ? cls->name : data->name;
        const auto& members = cls ? cls->members : data->members;
        const auto genericParams = cls ? cls->typeParams : std::vector<std::string>{};
        for (const auto& member : members) {
            const FunctionDecl* fn = asFunctionDecl(member.get());
            if (!fn || !fn->isStatic) continue;
            const auto signature = dispatchSignatureForFunction(*fn, genericParams);
            namedFunctions_[owner + "." + signature.describe()] = fn;
        }
    }
}

std::string TypeChecker::namedFunctionKey(const FunctionDecl* node) const {
    if (!node) return {};
    const auto* shape = semanticModel_.findClass(node->ownerClassName);
    const auto genericParams = shape ? shape->typeParams : std::vector<std::string>{};
    return node->ownerClassName + "." + dispatchSignatureForFunction(*node, genericParams).describe();
}

const FunctionDecl* TypeChecker::findNamedFunction(const std::string& ownerClassName,
                                                   const DispatchSignature& signature) const {
    auto it = namedFunctions_.find(ownerClassName + "." + signature.describe());
    if (it != namedFunctions_.end()) return it->second;

    // Match compiler lookup semantics for inherited static functions.
    std::string cur = ownerClassName;
    while (!cur.empty()) {
        it = namedFunctions_.find(cur + "." + signature.describe());
        if (it != namedFunctions_.end()) return it->second;
        const auto* shape = semanticModel_.findClass(cur);
        cur = shape ? shape->parentName : std::string();
    }
    return nullptr;
}

bool TypeChecker::isExplicitlySharedClass(const std::string& className) const {
    return className == "Shared" || className.rfind("Shared<", 0) == 0;
}

bool TypeChecker::checkNamedFunctionConfinementNode(
    const AstNode* node, const FunctionDecl* owner, std::unordered_set<std::string>& locals, std::string& reason) {
    if (!node) return true;

    switch (node->kind) {
        case NodeKind::FieldAccessExpr: {
            const auto* n = static_cast<const FieldAccessExpr*>(node);
            if (n->object && n->object->kind == NodeKind::Identifier) {
                const auto* id = static_cast<const Identifier*>(n->object.get());
                if (!locals.count(id->name)) {
                    const auto* cls = semanticModel_.findClass(id->name);
                    if (cls && !cls->isEnumType) {
                        std::string fieldOwner;
                        const auto* field = semanticModel_.findFieldInHierarchy(id->name, n->fieldName, &fieldOwner);
                        if (field && field->isStatic && !isExplicitlySharedClass(field->className)) {
                            reason = "accesses non-Shared static field '" + id->name + "." + n->fieldName + "'";
                            return false;
                        }
                        if (n->isFunctionReference || (field == nullptr)) {
                            if (n->isFunctionReference) {
                                const auto* fn = findNamedFunction(id->name, n->resolvedFunctionDispatch);
                                if (!fn) {
                                    reason = "references an unresolved named function '" + id->name + "." + n->fieldName + "'";
                                    return false;
                                }
                                if (!checkNamedFunctionConfinement(fn, reason)) return false;
                            }
                        }
                    }
                }
            }
            return checkNamedFunctionConfinementNode(n->object.get(), owner, locals, reason);
        }
        case NodeKind::FieldAssignExpr: {
            const auto* n = static_cast<const FieldAssignExpr*>(node);
            if (n->object && n->object->kind == NodeKind::Identifier) {
                const auto* id = static_cast<const Identifier*>(n->object.get());
                if (!locals.count(id->name)) {
                    const auto* cls = semanticModel_.findClass(id->name);
                    if (cls) {
                        std::string fieldOwner;
                        const auto* field = semanticModel_.findFieldInHierarchy(id->name, n->fieldName, &fieldOwner);
                        if (field && field->isStatic && !isExplicitlySharedClass(field->className)) {
                            reason = "writes non-Shared static field '" + id->name + "." + n->fieldName + "'";
                            return false;
                        }
                    }
                }
            }
            return checkNamedFunctionConfinementNode(n->object.get(), owner, locals, reason) &&
                   checkNamedFunctionConfinementNode(n->value.get(), owner, locals, reason);
        }
        case NodeKind::CallExpr: {
            const auto* n = static_cast<const CallExpr*>(node);
            if (!n->namespaceName.empty()) {
                const auto* cls = semanticModel_.findClass(n->namespaceName);
                if (cls) {
                    const auto candidates = semanticModel_.collectMethodOverloads(n->namespaceName, n->calleeName);
                    std::vector<std::pair<std::string, ClassMethodInfo>> statics;
                    for (const auto& candidate : candidates) if (candidate.second.isStatic) statics.push_back(candidate);
                    if (!statics.empty()) {
                        const DispatchSignature sig = n->resolvedDispatch.name.empty()
                            ? dispatchSignature(n->calleeName, statics.front().second) : n->resolvedDispatch;
                        const auto* fn = findNamedFunction(n->namespaceName, sig);
                        if (!fn) {
                            reason = "references an unresolved named function '" + n->namespaceName + "." + n->calleeName + "'";
                            return false;
                        }
                        if (!checkNamedFunctionConfinement(fn, reason)) return false;
                    }
                }
            } else if (!n->isValueCall && !locals.count(n->calleeName) && owner && !owner->ownerClassName.empty()) {
                const auto candidates = semanticModel_.collectMethodOverloads(owner->ownerClassName, n->calleeName);
                std::vector<std::pair<std::string, ClassMethodInfo>> statics;
                for (const auto& candidate : candidates) if (candidate.second.isStatic) statics.push_back(candidate);
                if (!statics.empty()) {
                    const DispatchSignature sig = n->resolvedDispatch.name.empty()
                        ? dispatchSignature(n->calleeName, statics.front().second) : n->resolvedDispatch;
                    const auto* fn = findNamedFunction(owner->ownerClassName, sig);
                    if (fn && fn != owner && !checkNamedFunctionConfinement(fn, reason)) return false;
                }
            }
            for (const auto& arg : n->arguments)
                if (!checkNamedFunctionConfinementNode(arg.get(), owner, locals, reason)) return false;
            return true;
        }
        case NodeKind::LambdaExpr: {
            const auto* n = static_cast<const LambdaExpr*>(node);
            // The lambda itself executes in the same worker as the named
            // function unless it is passed to another thread boundary. The
            // ordinary direct-lambda checker handles those nested boundaries.
            if (n->hasExprBody)
                return checkNamedFunctionConfinementNode(n->exprBody.get(), owner, locals, reason);
            return checkNamedFunctionConfinementNode(n->blockBody.get(), owner, locals, reason);
        }
        case NodeKind::VarDecl: {
            const auto* n = static_cast<const VarDecl*>(node);
            locals.insert(n->name);
            return checkNamedFunctionConfinementNode(n->initializer.get(), owner, locals, reason);
        }
        case NodeKind::BlockStmt: {
            const auto* n = static_cast<const BlockStmt*>(node);
            for (const auto& stmt : n->statements)
                if (!checkNamedFunctionConfinementNode(stmt.get(), owner, locals, reason)) return false;
            return true;
        }
        case NodeKind::IfStmt: {
            const auto* n = static_cast<const IfStmt*>(node);
            for (const auto& branch : n->branches) {
                if (!checkNamedFunctionConfinementNode(branch.condition.get(), owner, locals, reason)) return false;
                if (!checkNamedFunctionConfinementNode(branch.body.get(), owner, locals, reason)) return false;
            }
            return checkNamedFunctionConfinementNode(n->elseBody.get(), owner, locals, reason);
        }
        case NodeKind::ForStmt: {
            const auto* n = static_cast<const ForStmt*>(node);
            if (!checkNamedFunctionConfinementNode(n->start.get(), owner, locals, reason)) return false;
            if (!checkNamedFunctionConfinementNode(n->end.get(), owner, locals, reason)) return false;
            if (!checkNamedFunctionConfinementNode(n->step.get(), owner, locals, reason)) return false;
            locals.insert(n->varName);
            return checkNamedFunctionConfinementNode(n->body.get(), owner, locals, reason);
        }
        case NodeKind::WhileStmt: {
            const auto* n = static_cast<const WhileStmt*>(node);
            return checkNamedFunctionConfinementNode(n->condition.get(), owner, locals, reason) &&
                   checkNamedFunctionConfinementNode(n->body.get(), owner, locals, reason);
        }
        case NodeKind::RepeatStmt: {
            const auto* n = static_cast<const RepeatStmt*>(node);
            return checkNamedFunctionConfinementNode(n->body.get(), owner, locals, reason) &&
                   checkNamedFunctionConfinementNode(n->condition.get(), owner, locals, reason);
        }
        case NodeKind::TryStmt: {
            const auto* n = static_cast<const TryStmt*>(node);
            if (!checkNamedFunctionConfinementNode(n->tryBlock.get(), owner, locals, reason)) return false;
            for (const auto& c : n->catches) {
                locals.insert(c.varName);
                if (!checkNamedFunctionConfinementNode(c.block.get(), owner, locals, reason)) return false;
            }
            return checkNamedFunctionConfinementNode(n->finallyBlock.get(), owner, locals, reason);
        }
        case NodeKind::ReturnStmt:
            return checkNamedFunctionConfinementNode(static_cast<const ReturnStmt*>(node)->value.get(), owner, locals, reason);
        case NodeKind::ThrowStmt:
            return checkNamedFunctionConfinementNode(static_cast<const ThrowStmt*>(node)->value.get(), owner, locals, reason);
        case NodeKind::LogStmt:
            return checkNamedFunctionConfinementNode(static_cast<const LogStmt*>(node)->argument.get(), owner, locals, reason);
        case NodeKind::LogExpr:
            return checkNamedFunctionConfinementNode(static_cast<const LogExpr*>(node)->argument.get(), owner, locals, reason);
        case NodeKind::ExprStmt:
            return checkNamedFunctionConfinementNode(static_cast<const ExprStmt*>(node)->expression.get(), owner, locals, reason);
        case NodeKind::AssignExpr:
            return checkNamedFunctionConfinementNode(static_cast<const AssignExpr*>(node)->value.get(), owner, locals, reason);
        case NodeKind::MoveExpr:
            return true;
        case NodeKind::UnaryExpr:
            return checkNamedFunctionConfinementNode(static_cast<const UnaryExpr*>(node)->operand.get(), owner, locals, reason);
        case NodeKind::AwaitExpr:
            return checkNamedFunctionConfinementNode(static_cast<const AwaitExpr*>(node)->operand.get(), owner, locals, reason);
        case NodeKind::BinaryExpr: {
            const auto* n = static_cast<const BinaryExpr*>(node);
            return checkNamedFunctionConfinementNode(n->left.get(), owner, locals, reason) &&
                   checkNamedFunctionConfinementNode(n->right.get(), owner, locals, reason);
        }
        case NodeKind::MethodCallExpr: {
            const auto* n = static_cast<const MethodCallExpr*>(node);
            if (!checkNamedFunctionConfinementNode(n->object.get(), owner, locals, reason)) return false;
            for (const auto& arg : n->arguments)
                if (!checkNamedFunctionConfinementNode(arg.get(), owner, locals, reason)) return false;
            return true;
        }
        case NodeKind::NewExpr: {
            const auto* n = static_cast<const NewExpr*>(node);
            for (const auto& arg : n->arguments)
                if (!checkNamedFunctionConfinementNode(arg.get(), owner, locals, reason)) return false;
            return true;
        }
        case NodeKind::CollectionLiteral: {
            const auto* n = static_cast<const CollectionLiteral*>(node);
            for (const auto& e : n->elements)
                if (!checkNamedFunctionConfinementNode(e.get(), owner, locals, reason)) return false;
            for (const auto& e : n->entries) {
                if (!checkNamedFunctionConfinementNode(e.first.get(), owner, locals, reason)) return false;
                if (!checkNamedFunctionConfinementNode(e.second.get(), owner, locals, reason)) return false;
            }
            return true;
        }
        case NodeKind::DataLiteralExpr: {
            const auto* n = static_cast<const DataLiteralExpr*>(node);
            for (const auto& field : n->fields)
                if (!checkNamedFunctionConfinementNode(field.second.get(), owner, locals, reason)) return false;
            return true;
        }
        case NodeKind::DataUpdateExpr: {
            const auto* n = static_cast<const DataUpdateExpr*>(node);
            if (!checkNamedFunctionConfinementNode(n->base.get(), owner, locals, reason)) return false;
            for (const auto& field : n->fields)
                if (!checkNamedFunctionConfinementNode(field.second.get(), owner, locals, reason)) return false;
            return true;
        }
        default:
            return true;
    }
}

bool TypeChecker::checkNamedFunctionConfinement(const FunctionDecl* node, std::string& reason) {
    if (!node) {
        reason = "named function reference is unavailable";
        return false;
    }
    const std::string key = namedFunctionKey(node);
    auto& state = namedFunctionConfinement_[key];
    if (state.kind == NamedFunctionConfinementState::Kind::SAFE) return true;
    if (state.kind == NamedFunctionConfinementState::Kind::UNSAFE) {
        reason = state.reason;
        return false;
    }
    if (state.kind == NamedFunctionConfinementState::Kind::CHECKING) {
        // Recursive static call graph. Treat the cycle as safe for now; any
        // concrete unsafe static state reachable from the cycle will still be
        // found when its node is visited.
        return true;
    }

    state.kind = NamedFunctionConfinementState::Kind::CHECKING;
    if (!node->isStatic) {
        state.kind = NamedFunctionConfinementState::Kind::UNSAFE;
        state.reason = "is an instance function and is not transferable without an object ownership proof";
        reason = state.reason;
        return false;
    }

    std::unordered_set<std::string> locals;
    for (const auto& p : node->params) locals.insert(p.name);
    if (node->body) collectFunctionLocals(node->body.get(), locals);

    std::string localReason;
    const bool safe = checkNamedFunctionConfinementNode(node->body.get(), node, locals, localReason);
    state.kind = safe ? NamedFunctionConfinementState::Kind::SAFE : NamedFunctionConfinementState::Kind::UNSAFE;
    state.reason = localReason;
    if (!safe) reason = state.reason;
    return safe;
}

// ---------------------------------------------------------------------------
// TypeChecker - entry point
// ---------------------------------------------------------------------------

void TypeChecker::check(const Program& program, bool requireMain) {
    typeResolver_.reset();
    // The inference record is keyed by AST node identity, so a checker reused
    // for a second program must not keep the first program's entries alive.
    expressionTypes_.clear();

    // Push a global scope for class-level declarations.
    symbols_.pushScope();

    // Pass 0a: register every class/data/enum type NAME first (empty
    // shell), so type resolution recognizes them as a valid type
    // regardless of which class in the file references which - order
    // shouldn't matter. Also where a same-file name collision is caught -
    // any combination of class/data/enum sharing a name is rejected here,
    // since all three are equally "type names" in the same namespace (see
    // ClassShapeInfo::isDataType / isEnumType).
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::ClassDecl) {
            const auto* cls = static_cast<const ClassDecl*>(decl.get());
            if (semanticModel_.hasClass(cls->name)) {
                typeError("'" + cls->name + "' is declared more than once in this file", cls->line);
            }
            ClassShapeInfo shell;
            shell.name = cls->name;
            shell.typeParams = cls->typeParams;
            semanticModel_.defineClass(std::move(shell));
        } else if (decl->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(decl.get());
            if (semanticModel_.hasClass(data->name)) {
                typeError("'" + data->name + "' is declared more than once in this file", data->line);
            }
            ClassShapeInfo shell;
            shell.name = data->name;
            shell.isDataType = true;
            semanticModel_.defineClass(std::move(shell));
        } else if (decl->kind == NodeKind::EnumDecl) {
            const auto* en = static_cast<const EnumDecl*>(decl.get());
            if (semanticModel_.hasClass(en->name)) {
                typeError("'" + en->name + "' is declared more than once in this file", en->line);
            }
            ClassShapeInfo shell;
            shell.name = en->name;
            shell.isEnumType = true;
            semanticModel_.defineClass(std::move(shell));
        } else if (decl->kind == NodeKind::MemoryDecl) {
            // A memory declaration shares the type namespace: `memory Pool`
            // and `class Pool` in one file collide here, exactly as two
            // classes would. The shape starts as a shell; pass 0b fills it
            // after validating the domain contract.
            const auto* mem = static_cast<const MemoryDecl*>(decl.get());
            if (semanticModel_.hasClass(mem->name)) {
                typeError("'" + mem->name + "' is declared more than once in this file", mem->line);
            }
            ClassShapeInfo shell;
            shell.name = mem->name;
            shell.isMemoryDomain = true;
            semanticModel_.defineClass(std::move(shell));
        }
    }

    // Pass 0b: now fill in each class's actual fields + method/constructor
    // signatures, now that every class name is known.
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::ClassDecl) {
            registerClassShape(static_cast<const ClassDecl*>(decl.get()));
        } else if (decl->kind == NodeKind::DataDecl) {
            registerDataShape(static_cast<const DataDecl*>(decl.get()));
        } else if (decl->kind == NodeKind::EnumDecl) {
            registerEnumShape(static_cast<const EnumDecl*>(decl.get()));
        } else if (decl->kind == NodeKind::MemoryDecl) {
            registerMemoryDeclaration(static_cast<const MemoryDecl*>(decl.get()));
        }
    }

    // Pass 0a-interfaces: register every interface's own method signatures
    // (no merging with ancestors yet - that needs every interface's own
    // shape to exist first, same reasoning as classes above).
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::InterfaceDecl) {
            registerInterfaceShape(static_cast<const InterfaceDecl*>(decl.get()));
        }
    }

    // Pass 0b-interfaces: validate `extends` names, then force every
    // interface's ancestor-merge to run now (not lazily, only when some
    // class happens to `implements` it) - this is what actually catches
    // circular interface inheritance and diamond signature conflicts even
    // for an interface nothing implements yet.
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::InterfaceDecl) continue;
        const auto* iface = static_cast<const InterfaceDecl*>(decl.get());
        for (const std::string& parentName : iface->extendsNames) {
            if (!semanticModel_.hasInterface(parentName)) {
                typeError("interface '" + iface->name + "' extends unknown interface '" + parentName + "'",
                          iface->line);
            }
        }
    }
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::InterfaceDecl) {
            semanticModel_.resolveInterfaceMethods(static_cast<const InterfaceDecl*>(decl.get())->name);
        }
    }

    // Pass 0b-fixup: registerClassShape ran before interface registration
    // (class shapes must exist first so registerInterfaceShape can detect
    // name collisions), so a parameter or return type named after an
    // interface was recorded as UNKNOWN. Re-resolve those now: the recorded
    // metadata drives dispatch identities (ask(G) vs ask(object)), overload
    // resolution and interface-implementation matching, and a stale UNKNOWN
    // silently degrades all three (the call lowers to nothing and the
    // bytecode backend then drops it from the compiled body).
    // NOTE: member signatures are resolved in their OWNING class's context
    // (type parameters in scope). Resolving them at global scope misreads a
    // bare type-parameter reference (e.g. builtin Result<T, E>'s `unwrapErr(): E`)
    // as a class name - and if the user declares a generic class with that
    // same name, resolution fails with a bogus "requires N type arguments"
    // error pointing at an unrelated line.
    const std::string savedClassName = currentClassName_;
    const std::vector<std::string> savedClassTypeParams = currentClassTypeParams_;
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl && decl->kind != NodeKind::DataDecl) continue;
        const std::string className = decl->kind == NodeKind::ClassDecl
            ? static_cast<const ClassDecl*>(decl.get())->name
            : static_cast<const DataDecl*>(decl.get())->name;
        auto* classIt = semanticModel_.findClass(className);
        if (!classIt) continue;
        currentClassName_ = className;
        currentClassTypeParams_ = classIt->typeParams;
        const auto& members = decl->kind == NodeKind::ClassDecl
            ? static_cast<const ClassDecl*>(decl.get())->members
            : static_cast<const DataDecl*>(decl.get())->members;
        std::unordered_map<std::string, std::size_t> ctorIndex, methodIndex;
        for (const auto& member : members) {
            if (!member || member->kind != NodeKind::FunctionDecl) continue;
            const auto* fn = static_cast<const FunctionDecl*>(member.get());
            const auto savedFixupParams = currentClassTypeParams_;
            currentClassTypeParams_.insert(currentClassTypeParams_.end(),
                                           fn->typeParams.begin(), fn->typeParams.end());
            const std::size_t index = (fn->isConstructor ? ctorIndex : methodIndex)[fn->name]++;
            ClassMethodInfo* target = nullptr;
            if (fn->isConstructor) {
                target = index < classIt->constructors.size() ? &classIt->constructors[index] : nullptr;
            } else {
                const auto overloads = classIt->methods.find(fn->name);
                if (overloads != classIt->methods.end() && index < overloads->second.size())
                    target = &overloads->second[index];
            }
            if (!target) {
                currentClassTypeParams_ = savedFixupParams;
                continue;
            }
            std::string returnClassName;
            const ZlType returnType = resolveType(fn->returnType, &returnClassName);
            if (target->returnType == ZlType::UNKNOWN && target->returnClassName.empty() &&
                returnType != ZlType::UNKNOWN) {
                target->returnType = returnType;
                target->returnClassName = returnClassName;
            }
            for (std::size_t i = 0; i < fn->params.size() && i < target->paramTypes.size(); ++i) {
                std::string paramClassName;
                const ZlType paramType = resolveType(fn->params[i].type, &paramClassName);
                if (target->paramTypes[i] == ZlType::UNKNOWN && target->paramClassNames[i].empty() &&
                    paramType != ZlType::UNKNOWN) {
                    target->paramTypes[i] = paramType;
                    target->paramClassNames[i] = paramClassName;
                }
            }
            currentClassTypeParams_ = savedFixupParams;
        }
    }
    currentClassName_ = savedClassName;
    currentClassTypeParams_ = savedClassTypeParams;

    // Pass 0c: link `extends` parents for classes and data records. Record
    // inheritance is deliberately restricted to data -> data so value
    // semantics remain closed over the record hierarchy. Classes keep their
    // existing inheritance rules separately.
    for (const auto& decl : program.declarations) {
        std::string childName;
        std::string parentName;
        std::vector<TypeAnnotation> parentArgs;
        bool isData = false;
        if (decl->kind == NodeKind::ClassDecl) {
            const auto* cls = static_cast<const ClassDecl*>(decl.get());
            childName = cls->name; parentName = cls->extendsName; parentArgs = cls->extendsTypeArgs;
        } else if (decl->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(decl.get());
            childName = data->name; parentName = data->extendsName; parentArgs = data->extendsTypeArgs; isData = true;
        } else continue;
        if (parentName.empty()) continue;
        if (!semanticModel_.hasClass(parentName)) {
            typeError((isData ? "data type '" : "class '") + childName + "' extends unknown type '" + parentName + "'", decl->line);
        }
        const auto* parentShape = semanticModel_.findClass(parentName);
        if (parentShape && parentShape->isMemoryDomain) {
            typeError((isData ? "data type '" : "class '") + childName +
                          "' cannot extend memory declaration '" + parentName +
                          "' - a domain is not a class-hierarchy member",
                      decl->line);
        }
        if (isData && (!parentShape || !parentShape->isDataType)) {
            typeError("data type '" + childName + "' may only extend another data type", decl->line);
        }
        if (!isData && parentShape && parentShape->isDataType) {
            typeError("class '" + childName + "' cannot extend data type '" + parentName + "'", decl->line);
        }
        semanticModel_.setParentClass(childName, parentName, std::move(parentArgs));
    }
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl && decl->kind != NodeKind::DataDecl) continue;
        const std::string childName = decl->kind == NodeKind::ClassDecl
            ? static_cast<const ClassDecl*>(decl.get())->name
            : static_cast<const DataDecl*>(decl.get())->name;
        std::vector<std::string> chain{childName};
        std::string cur = semanticModel_.parentClassName(childName);
        while (!cur.empty()) {
            if (std::find(chain.begin(), chain.end(), cur) != chain.end()) {
                typeError("circular inheritance detected: '" + childName + "' -> ... -> '" + cur + "' -> ... -> itself", decl->line);
            }
            chain.push_back(cur);
            cur = semanticModel_.parentClassName(cur);
        }
    }

    // Object storage is keyed by field name, not by (declaring class, name).
    // Check after all shapes/parents exist so declaration order cannot hide
    // a second contract for the same inherited slot.
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl && decl->kind != NodeKind::DataDecl) continue;
        const auto name = decl->kind == NodeKind::ClassDecl ? static_cast<const ClassDecl*>(decl.get())->name
                                                          : static_cast<const DataDecl*>(decl.get())->name;
        const auto* shape = semanticModel_.findClass(name);
        if (shape->parentName.empty()) continue;
        for (const auto& field : shape->fields) {
            if (semanticModel_.findFieldInHierarchy(shape->parentName, field.first))
                typeError("type '" + name + "' cannot redeclare inherited field '" + field.first + "'", decl->line);
        }
    }

    // Every class shape (including inheritance links) is now complete. Any
    // generic instantiation created while resolving an earlier signature may
    // have been built from a template that was still an empty shell, so
    // rebuild them all here. Without this, whether `List<string>` has methods
    // would depend on which file happened to be checked first.
    typeResolver_.refreshInstantiations();

    // Index all named static functions before any body is checked so interprocedural
    // confinement analysis is independent of declaration order.
    indexNamedFunctions(program);

    // Check generic class bodies first so inferred generic constraints (for
    // example Vector2<T,U>'s numeric parameters) are known before an earlier
    // entry class attempts to instantiate them. All class shapes are already
    // registered, so this ordering does not change declaration visibility.
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        if (!cls->typeParams.empty()) checkClassDecl(cls);
    }
    for (const auto& decl : program.declarations) {
        if (decl->kind == NodeKind::ClassDecl) {
            const auto* cls = static_cast<const ClassDecl*>(decl.get());
            if (cls->typeParams.empty()) checkClassDecl(cls);
        } else if (decl->kind == NodeKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(decl.get());
            currentClassTypeParams_.clear();
            for (const auto& member : data->members)
                checkFunctionDecl(static_cast<const FunctionDecl*>(member.get()));
        } else if (decl->kind == NodeKind::MemoryDecl) {
            checkMemoryDeclaration(static_cast<const MemoryDecl*>(decl.get()));
        }
    }

    // Validate the program entry point before code generation. The compiler
    // injects CLI arguments only for the canonical list<string> form, so an
    // arbitrary main signature would otherwise become a runtime type mismatch.
    const FunctionDecl* mainFn = nullptr;
    for (const auto& decl : program.declarations) {
        if (decl->kind != NodeKind::ClassDecl) continue;
        const auto* cls = static_cast<const ClassDecl*>(decl.get());
        for (const auto& member : cls->members) {
            if (member->kind != NodeKind::FunctionDecl) continue;
            const auto* fn = static_cast<const FunctionDecl*>(member.get());
            if (fn->name != "main") continue;
            if (!mainFn) mainFn = fn;
        }
    }
    if (!mainFn && !requireMain) {
        // Library/module check mode does not require an entry point.
    } else {
        if (!mainFn) typeError("program has no main() func", program.line);
        if (mainFn->isStatic) typeError("main() must be an instance method", mainFn->line);
        if (mainFn->returnType.name != "void") {
            typeError("main() must return void", mainFn->line);
        }
        if (mainFn->params.size() > 1) {
            typeError("main() accepts either no parameters or one list<string> parameter", mainFn->line);
        }
        if (mainFn->params.size() == 1) {
            const auto& t = mainFn->params[0].type;
            const bool plainList = t.name == "list" && t.typeArgs.size() == 1 && t.typeArgs[0].name == "string";
            const bool genericList = t.name == "List" && t.typeArgs.size() == 1 && t.typeArgs[0].name == "string";
            if (!plainList && !genericList) {
                typeError("main() parameter must be list<string>", mainFn->params[0].type.line);
            }
        }
    }

    symbols_.popScope();
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

void TypeChecker::registerClassShape(const ClassDecl* node) {
    validateAnnotationTargets(node->annotations, annotations::POS_TYPE_DECL, node->name);

    ClassShapeInfo info;
    info.name = node->name;
    info.typeParams = node->typeParams;
    info.typeParamInterfaceConstraints = node->typeParamInterfaceConstraints;
    info.isDeprecated = hasAnnotation(node->annotations, "Deprecated");
    info.implementsNames = node->implementsNames;
    info.visibleImports = node->visibleImports;
    {
        std::unordered_set<std::string> seenInterfaces;
        for (const auto& iface : node->implementsNames) {
            if (!seenInterfaces.insert(iface).second) {
                typeError("class '" + node->name + "' implements '" + iface + "' more than once", node->line);
            }
        }
    }

    // In scope for every resolveType call below, so a bare reference to one
    // of this class's own type parameters (e.g. `T` inside `class Box<T>`)
    // resolves to an unsubstituted placeholder instead of an unknown type.
    // Cleared before returning, including on the early-return paths that
    // don't exist here today but might later - RAII would be nicer, but a
    // single well-scoped clear at the end matches the rest of this codebase's
    // existing style (e.g. checkFunctionDecl's currentClassName_/inConstructor_).
    currentClassName_ = node->name;
    currentClassTypeParams_ = node->typeParams;

    for (const auto& member : node->members) {
        if (member->kind == NodeKind::VarDecl) {
            const auto* field = static_cast<const VarDecl*>(member.get());
            std::string fieldClassName;
            ZlType fieldType = field->hasExplicitType ? resolveType(field->type, &fieldClassName) : ZlType::UNKNOWN;
            ClassFieldInfo fieldInfo{fieldType, fieldClassName, field->access};
            fieldInfo.isStatic = field->isStatic;
            fieldInfo.ownership = field->ownership;
            fieldInfo.functionHasSignature = fieldType == ZlType::FUNCTION && field->type.functionHasSignature;
            if (fieldType == ZlType::FUNCTION) {
                for (const auto& param : field->type.functionParamTypes) {
                    std::string cls;
                    fieldInfo.functionParamTypes.push_back(resolveType(param, &cls));
                    fieldInfo.functionParamClassNames.push_back(std::move(cls));
                }
                if (field->type.functionReturnType) {
                    fieldInfo.functionReturnType = resolveType(*field->type.functionReturnType,
                                                               &fieldInfo.functionReturnClassName);
                }
            }
            info.fields[field->name] = std::move(fieldInfo);
            info.fieldOrder.push_back(field->name);
            continue;
        }
        if (member->kind != NodeKind::FunctionDecl) continue;
        const auto* fn = static_cast<const FunctionDecl*>(member.get());

        ClassMethodInfo m;
        m.typeParams = fn->typeParams;
        const auto savedMethodParams = currentClassTypeParams_;
        mergeMethodTypeParams(fn->typeParams, fn->line);
        std::string returnClassName;
        m.returnType = resolveType(fn->returnType, &returnClassName);
        m.returnClassName = returnClassName;
        m.access = fn->access;
        m.isDeprecated = hasAnnotation(fn->annotations, "Deprecated");
        m.isStatic = fn->isStatic;
        m.isAsync = fn->isAsync;
        if (m.returnType == ZlType::FUNCTION) {
            m.returnFunctionHasSignature = fn->returnType.functionHasSignature;
            for (const auto& rp : fn->returnType.functionParamTypes) {
                std::string cls;
                m.returnFunctionParamTypes.push_back(resolveType(rp, &cls));
                m.returnFunctionParamClassNames.push_back(std::move(cls));
            }
            if (fn->returnType.functionReturnType) {
                m.returnFunctionReturnType = resolveType(*fn->returnType.functionReturnType,
                                                         &m.returnFunctionReturnClassName);
            }
        }
        for (const auto& p : fn->params) {
            std::string paramClassName;
            ZlType paramType = resolveType(p.type, &paramClassName);
            m.paramTypes.push_back(paramType);
            m.paramClassNames.push_back(paramClassName);
            if (paramType == ZlType::FUNCTION) {
                std::vector<ZlType> paramFunctionTypes;
                std::vector<std::string> paramFunctionClasses;
                for (const auto& fp : p.type.functionParamTypes) {
                    std::string cls;
                    paramFunctionTypes.push_back(resolveType(fp, &cls));
                    paramFunctionClasses.push_back(std::move(cls));
                }
                m.functionParamTypes.push_back(std::move(paramFunctionTypes));
                m.functionParamClassNames.push_back(std::move(paramFunctionClasses));
                ZlType paramReturn = ZlType::UNKNOWN;
                std::string paramReturnClass;
                if (p.type.functionReturnType) {
                    paramReturn = resolveType(*p.type.functionReturnType, &paramReturnClass);
                }
                m.functionReturnTypes.push_back(paramReturn);
                m.functionReturnClassNames.push_back(std::move(paramReturnClass));
                m.functionParamHasSignature.push_back(p.type.functionHasSignature);
            } else {
                m.functionParamTypes.emplace_back();
                m.functionParamClassNames.emplace_back();
                m.functionReturnTypes.push_back(ZlType::UNKNOWN);
                m.functionReturnClassNames.emplace_back();
                m.functionParamHasSignature.push_back(false);
            }
            // A bare type-parameter reference is EXACTLY the case resolved
            // just above via currentClassTypeParams_ - paramType == OBJECT
            // and paramClassName is literally the parameter's own name
            // (e.g. "T"), which is how it's distinguished from an ordinary
            // object-typed parameter (paramClassName would be a real,
            // registered class name there instead).
            const bool isGeneric = std::find(currentClassTypeParams_.begin(), currentClassTypeParams_.end(), paramClassName) != currentClassTypeParams_.end() ||
                (paramType == ZlType::UNION && containsTypeParameter(p.type, currentClassTypeParams_));
            m.paramIsGeneric.push_back(isGeneric);
        }
        currentClassTypeParams_ = savedMethodParams;

        if (fn->isConstructor && fn->isStatic) {
            typeError("constructor '" + node->name + "' cannot be static", fn->line);
        }

        if (fn->isConstructor) {            for (const auto& existing : info.constructors) {
                if (methodParamSignature(existing) == methodParamSignature(m)) {
                    typeError("class '" + node->name + "' already declares a constructor" +
                               paramTypesSuffix(m.paramTypes), fn->line);
                }
            }
            info.constructors.push_back(m);
        } else {
            auto& overloads = info.methods[fn->name];
            for (const auto& existing : overloads) {
                if (methodParamSignature(existing) == methodParamSignature(m)) {
                    typeError("class '" + node->name + "' already declares '" + fn->name +
                               paramTypesSuffix(m.paramTypes) + "'", fn->line);
                }
            }
            overloads.push_back(m);
        }
    }

    semanticModel_.defineClass(std::move(info));
    currentClassName_.clear();
    currentClassTypeParams_.clear();
}

void TypeChecker::registerDataShape(const DataDecl* node) {
    ClassShapeInfo info;
    info.name = node->name;
    info.isDataType = true;
    currentClassTypeParams_.clear();

    for (const auto& field : node->fields) {
        if (field.type.name.empty()) {
            typeError("data type '" + node->name + "' field '" + field.name +
                       "' needs an explicit type (e.g. '" + field.name + ": string') - data fields are always typed",
                       node->line);
        }
        std::string fieldClassName;
        ZlType fieldType = resolveType(field.type, &fieldClassName);
        // No access modifier concept on a data field - see Param, which
        // (unlike a class's VarDecl fields) has no `access` member: a
        // `data` type is a plain public record, not an encapsulated
        // object, so every field is simply PUBLIC.
        ClassFieldInfo fieldInfo{fieldType, fieldClassName, AccessModifier::PUBLIC};
        fieldInfo.ownership = field.ownership;
        fieldInfo.functionHasSignature = fieldType == ZlType::FUNCTION && field.type.functionHasSignature;
        if (fieldType == ZlType::FUNCTION) {
            for (const auto& param : field.type.functionParamTypes) {
                std::string cls;
                fieldInfo.functionParamTypes.push_back(resolveType(param, &cls));
                fieldInfo.functionParamClassNames.push_back(std::move(cls));
            }
            if (field.type.functionReturnType) {
                fieldInfo.functionReturnType = resolveType(*field.type.functionReturnType,
                                                           &fieldInfo.functionReturnClassName);
            }
        }
        info.fields[field.name] = std::move(fieldInfo);
        info.fieldOrder.push_back(field.name);
    }

    // Record/data methods are ordinary public instance methods. They share the
    // normal class dispatch machinery but cannot be static or constructors.
    for (const auto& member : node->members) {
        const auto* fn = static_cast<const FunctionDecl*>(member.get());
        if (fn->isStatic) {
            typeError("data method '" + fn->name + "' cannot be static", fn->line);
        }
        if (fn->isConstructor) {
            typeError("data type '" + node->name + "' cannot declare constructors", fn->line);
        }

        ClassMethodInfo m;
        m.typeParams = fn->typeParams;
        const auto savedMethodParams = currentClassTypeParams_;
        mergeMethodTypeParams(fn->typeParams, fn->line);
        std::string returnClassName;
        m.returnType = resolveType(fn->returnType, &returnClassName);
        m.returnClassName = returnClassName;
        m.access = AccessModifier::PUBLIC;
        m.isDeprecated = hasAnnotation(fn->annotations, "Deprecated");
        m.isStatic = false;
        m.isAsync = fn->isAsync;
        if (m.returnType == ZlType::FUNCTION) {
            m.returnFunctionHasSignature = fn->returnType.functionHasSignature;
            for (const auto& rp : fn->returnType.functionParamTypes) {
                std::string cls;
                m.returnFunctionParamTypes.push_back(resolveType(rp, &cls));
                m.returnFunctionParamClassNames.push_back(std::move(cls));
            }
            if (fn->returnType.functionReturnType)
                m.returnFunctionReturnType = resolveType(*fn->returnType.functionReturnType, &m.returnFunctionReturnClassName);
        }
        for (const auto& param : fn->params) {
            std::string cls;
            const ZlType pt = resolveType(param.type, &cls);
            m.paramTypes.push_back(pt);
            m.paramClassNames.push_back(cls);
            const bool isGeneric = std::find(currentClassTypeParams_.begin(), currentClassTypeParams_.end(), cls) != currentClassTypeParams_.end() ||
                (pt == ZlType::UNION && containsTypeParameter(param.type, currentClassTypeParams_));
            m.paramIsGeneric.push_back(isGeneric);
            if (pt == ZlType::FUNCTION) {
                std::vector<ZlType> fpTypes;
                std::vector<std::string> fpClasses;
                for (const auto& fp : param.type.functionParamTypes) {
                    std::string fpc;
                    fpTypes.push_back(resolveType(fp, &fpc));
                    fpClasses.push_back(std::move(fpc));
                }
                m.functionParamTypes.push_back(std::move(fpTypes));
                m.functionParamClassNames.push_back(std::move(fpClasses));
                ZlType fr = ZlType::UNKNOWN;
                std::string frc;
                if (param.type.functionReturnType) fr = resolveType(*param.type.functionReturnType, &frc);
                m.functionReturnTypes.push_back(fr);
                m.functionReturnClassNames.push_back(std::move(frc));
                m.functionParamHasSignature.push_back(param.type.functionHasSignature);
            } else {
                m.functionParamTypes.emplace_back();
                m.functionParamClassNames.emplace_back();
                m.functionReturnTypes.push_back(ZlType::UNKNOWN);
                m.functionReturnClassNames.emplace_back();
                m.functionParamHasSignature.push_back(false);
            }
        }
        currentClassTypeParams_ = savedMethodParams;
        auto& overloads = info.methods[fn->name];
        for (const auto& existing : overloads) {
            if (methodParamSignature(existing) == methodParamSignature(m))
                typeError("data type '" + node->name + "' already declares '" + fn->name + paramTypesSuffix(m.paramTypes) + "'", fn->line);
        }
        overloads.push_back(std::move(m));
    }

    semanticModel_.defineClass(std::move(info));
}

void TypeChecker::registerEnumShape(const EnumDecl* node) {
    ClassShapeInfo info;
    info.name = node->name;
    info.isEnumType = true;

    std::unordered_set<std::string> seen;
    for (const auto& member : node->members) {
        if (!seen.insert(member).second) {
            typeError("enum '" + node->name + "' declares member '" + member + "' more than once", node->line);
        }
    }
    info.enumMembers = node->members;

    semanticModel_.defineClass(std::move(info));
}

// ---------------------------------------------------------------------------
// Memory declarations (docs/memory-domains.md §4.1, §5.1 - the Phase 1
// contract skeleton). Nothing consumes a domain yet: registration validates
// the contract and builds a shape so the contract bodies typecheck like any
// method body, and nothing downstream emits the declaration. The first
// consumer (allocation with a domain) lands with the arena phase, which is
// exactly why the contract is spelled out and checked now, while the only
// way to be wrong about it is a compile error rather than a runtime one.
// ---------------------------------------------------------------------------

namespace {

// The contract surface of a `memory` declaration. Required = the compiler
// refuses the declaration without it; optional = may be absent, but a
// declaration that spells it must spell it correctly (a mis-typed optional
// method is a silent no-op waiting to happen - the same reasoning as the
// unknown-annotation error).
struct MemoryContractEntry {
    const char* name;
    const char* paramType;   // nullptr = takes no parameter
    const char* returnType;  // "void", or "Option<MemorySlot>"
    bool required;
};

constexpr MemoryContractEntry kMemoryContract[] = {
    // Required. acquire is called by the compiler's Alloc for one box;
    // release when a live owner count can no longer exist.
    {"acquire",   "MemoryShape", "Option<MemorySlot>", true},
    {"release",   "MemorySlot",  "void",               true},
    // Optional. Bulk release at region exit; exhaustion policy; a view of
    // the global trace. Defaults exist in the runtime design (§4.1).
    {"reset",     nullptr,       "void",               false},
    {"exhausted", "MemoryShape", "void",               false},
    {"onCollect", "MemoryStats", "void",               false},
};

constexpr std::size_t kMemoryContractSize = sizeof(kMemoryContract) / sizeof(kMemoryContract[0]);

const MemoryContractEntry* findMemoryContractEntry(const std::string& name) {
    for (std::size_t i = 0; i < kMemoryContractSize; ++i) {
        if (name == kMemoryContract[i].name) return &kMemoryContract[i];
    }
    return nullptr;
}

// True when `annotation` is a bare, unparameterized, non-union spelling of
// `name` - the only form a contract signature accepts. A parameter spelled
// `list<MemoryShape>` or `int|MemoryShape` is a different signature, and the
// contract is fixed (§4.1), so this is an error, not an overload.
bool isBareTypeNamed(const TypeAnnotation& annotation, const char* name) {
    return annotation.name == name && annotation.typeArgs.empty() && annotation.unionOf.empty() &&
           !annotation.functionHasSignature;
}

bool isVoidReturn(const TypeAnnotation& annotation) {
    return isBareTypeNamed(annotation, "void");
}

// The one non-void contract return: Option<MemorySlot>, exactly.
bool isOptionOfMemorySlot(const TypeAnnotation& annotation) {
    return annotation.name == "Option" && annotation.typeArgs.size() == 1 &&
           isBareTypeNamed(annotation.typeArgs[0], "MemorySlot") && annotation.unionOf.empty();
}

std::string describeMemoryContractSignature(const MemoryContractEntry& entry) {
    std::string out = "public func ";
    out += entry.name;
    out += "(";
    if (entry.paramType != nullptr) out += entry.paramType;
    out += "): ";
    out += entry.returnType;
    return out;
}

} // namespace

void TypeChecker::registerMemoryDeclaration(const MemoryDecl* node) {
    // The parser already rejected statics/asyncs/operators/nested
    // declarations (and annotations on members); what is left here is the
    // contract: which members exist, their access, their exact signatures,
    // and the declaration-shape rules a domain cannot relax (no constructor,
    // no inheritance - pass 0c).

    ClassShapeInfo info;
    info.name = node->name;
    info.isMemoryDomain = true;
    info.visibleImports = node->visibleImports;

    currentClassName_ = node->name;
    currentClassTypeParams_.clear();

    // Fields are the domain's state (its counters, its capacity). They are
    // not part of the public contract - the compiler is the only caller of a
    // domain - so a public field is the same mistake as a public helper.
    for (const auto& member : node->members) {
        if (member->kind != NodeKind::VarDecl) continue;
        const auto* field = static_cast<const VarDecl*>(member.get());
        if (field->access == AccessModifier::PUBLIC) {
            typeError("field '" + node->name + "." + field->name +
                          "' must not be public in a memory declaration - a domain's state is not "
                          "part of its contract; make it private",
                      field->line);
        }
        std::string fieldClassName;
        ZlType fieldType = field->hasExplicitType ? resolveType(field->type, &fieldClassName) : ZlType::UNKNOWN;
        ClassFieldInfo fieldInfo{fieldType, fieldClassName, field->access};
        fieldInfo.ownership = field->ownership;
        fieldInfo.functionHasSignature = fieldType == ZlType::FUNCTION && field->type.functionHasSignature;
        if (fieldType == ZlType::FUNCTION) {
            for (const auto& param : field->type.functionParamTypes) {
                std::string cls;
                fieldInfo.functionParamTypes.push_back(resolveType(param, &cls));
                fieldInfo.functionParamClassNames.push_back(std::move(cls));
            }
            if (field->type.functionReturnType) {
                fieldInfo.functionReturnType = resolveType(*field->type.functionReturnType,
                                                           &fieldInfo.functionReturnClassName);
            }
        }
        info.fields[field->name] = std::move(fieldInfo);
        info.fieldOrder.push_back(field->name);
    }

    // The contract methods, checked against the table above, and the private
    // helpers around them.
    std::unordered_set<std::string> declaredContract;
    for (const auto& member : node->members) {
        if (member->kind != NodeKind::FunctionDecl) continue;
        const auto* fn = static_cast<const FunctionDecl*>(member.get());

        if (fn->isConstructor) {
            typeError("memory declaration '" + node->name +
                          "' cannot declare a constructor - a domain is not instantiated by user code",
                      fn->line);
        }

        const MemoryContractEntry* entry = findMemoryContractEntry(fn->name);
        if (entry == nullptr) {
            // A private helper is fine; a public one is contract surface the
            // design does not have (§5.1: fields, private helpers, and the
            // contract - nothing else).
            if (fn->access == AccessModifier::PUBLIC) {
                typeError("method '" + node->name + "." + fn->name +
                              "' must not be public in a memory declaration - only the contract "
                              "methods (acquire, release, reset, exhausted, onCollect) may be public; "
                              "make it private",
                          fn->line);
            }
            continue; // helper signatures are not constrained by the contract
        }

        if (!declaredContract.insert(fn->name).second) {
            typeError("memory declaration '" + node->name + "' declares contract method '" + fn->name +
                          "' more than once",
                      fn->line);
        }
        if (fn->access != AccessModifier::PUBLIC) {
            typeError("contract method '" + fn->name + "' in memory declaration '" + node->name +
                          "' must be public - the compiler is its only caller, but it is part of "
                          "the domain's contract surface",
                      fn->line);
        }

        // Signature, compared on the written annotation so the diagnostic can
        // quote the source spelling.
        std::string declared = "func " + fn->name + "(";
        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            if (i != 0) declared += ", ";
            declared += describeTypeAnnotation(fn->params[i].type);
        }
        declared += "): " + describeTypeAnnotation(fn->returnType);
        const std::string required = describeMemoryContractSignature(*entry);

        const std::size_t expectedParams = entry->paramType != nullptr ? 1u : 0u;
        if (fn->params.size() != expectedParams) {
            typeError("contract method '" + fn->name + "' in memory declaration '" + node->name +
                          "' has the wrong signature: expected '" + required + "', declared '" + declared + "'",
                      fn->line);
        } else if (entry->paramType != nullptr &&
                   !isBareTypeNamed(fn->params[0].type, entry->paramType)) {
            typeError("contract method '" + fn->name + "' in memory declaration '" + node->name +
                          "' must take a " + entry->paramType + " parameter, not '" +
                          describeTypeAnnotation(fn->params[0].type) + "' - expected '" + required + "'",
                      fn->line);
        } else if (std::string_view(entry->returnType) == "void" && !isVoidReturn(fn->returnType)) {
            typeError("contract method '" + fn->name + "' in memory declaration '" + node->name +
                          "' must return void, not '" + describeTypeAnnotation(fn->returnType) +
                          "' - expected '" + required + "'",
                      fn->line);
        } else if (std::string_view(entry->returnType) != "void" && !isOptionOfMemorySlot(fn->returnType)) {
            typeError("contract method '" + fn->name + "' in memory declaration '" + node->name +
                          "' must return Option<MemorySlot>, not '" +
                          describeTypeAnnotation(fn->returnType) + "' - expected '" + required + "'",
                      fn->line);
        }

        ClassMethodInfo m;
        m.typeParams = fn->typeParams;
        const auto savedMethodParams = currentClassTypeParams_;
        mergeMethodTypeParams(fn->typeParams, fn->line);
        std::string returnClassName;
        m.returnType = resolveType(fn->returnType, &returnClassName);
        m.returnClassName = returnClassName;
        m.access = fn->access;
        m.isDeprecated = hasAnnotation(fn->annotations, "Deprecated");
        m.isStatic = false;
        m.isAsync = fn->isAsync;
        if (m.returnType == ZlType::FUNCTION) {
            m.returnFunctionHasSignature = fn->returnType.functionHasSignature;
            for (const auto& rp : fn->returnType.functionParamTypes) {
                std::string cls;
                m.returnFunctionParamTypes.push_back(resolveType(rp, &cls));
                m.returnFunctionParamClassNames.push_back(std::move(cls));
            }
            if (fn->returnType.functionReturnType) {
                m.returnFunctionReturnType = resolveType(*fn->returnType.functionReturnType,
                                                         &m.returnFunctionReturnClassName);
            }
        }
        for (const auto& p : fn->params) {
            std::string cls;
            const ZlType pt = resolveType(p.type, &cls);
            m.paramTypes.push_back(pt);
            m.paramClassNames.push_back(cls);
            const bool isGeneric =
                std::find(currentClassTypeParams_.begin(), currentClassTypeParams_.end(), cls) !=
                    currentClassTypeParams_.end() ||
                (pt == ZlType::UNION && containsTypeParameter(p.type, currentClassTypeParams_));
            m.paramIsGeneric.push_back(isGeneric);
        }
        currentClassTypeParams_ = savedMethodParams;

        info.methods[fn->name].push_back(std::move(m));
    }

    // The required half of the contract, reported at the declaration so the
    // location names the `memory` line the reader wrote.
    for (std::size_t i = 0; i < kMemoryContractSize; ++i) {
        if (!kMemoryContract[i].required) continue;
        if (declaredContract.count(kMemoryContract[i].name) != 0) continue;
        typeError("memory declaration '" + node->name + "' does not declare the required contract method '" +
                      kMemoryContract[i].name + "' - every domain must provide '" +
                      describeMemoryContractSignature(kMemoryContract[i]) + "'",
                  node->line);
    }

    semanticModel_.defineClass(std::move(info));
}

void TypeChecker::checkMemoryDeclaration(const MemoryDecl* node) {
    // Mirrors checkClassDecl's member walk without the class-only parts
    // (implements, type parameters, static fields - the parser rejects
    // statics): set the owning-name context, then check each contract method
    // and helper body like any method body. Nothing is emitted for these.
    const std::string previousClassName = currentClassName_;
    currentClassName_ = node->name;
    for (const auto& member : node->members) {
        if (member->kind == NodeKind::FunctionDecl) {
            checkFunctionDecl(static_cast<const FunctionDecl*>(member.get()));
        }
    }
    currentClassName_ = previousClassName;
}

void TypeChecker::registerInterfaceShape(const InterfaceDecl* node) {
    if (semanticModel_.hasClass(node->name)) {
        typeError("interface '" + node->name + "' conflicts with an existing type of the same name", node->line);
    }
    if (semanticModel_.hasInterface(node->name)) {
        typeError("interface '" + node->name + "' is declared more than once", node->line);
    }

    std::unordered_set<std::string> seenParents;
    for (const auto& parent : node->extendsNames) {
        if (!seenParents.insert(parent).second) {
            typeError("interface '" + node->name + "' extends '" + parent + "' more than once", node->line);
        }
    }

    InterfaceShapeInfo info;
    info.name = node->name;
    info.line = node->line;
    info.extendsNames = node->extendsNames;
    for (const auto& sig : node->methods) {
        ClassMethodInfo m;
        m.typeParams = sig.typeParams;
        const auto savedMethodParams = currentClassTypeParams_;
        mergeMethodTypeParams(sig.typeParams, sig.line);
        std::string returnClassName;
        m.returnType = resolveType(sig.returnType, &returnClassName);
        m.returnClassName = returnClassName;
        m.access = AccessModifier::PUBLIC; // interface methods/operators are implicitly public
        for (const auto& p : sig.params) {
            std::string paramClassName;
            const ZlType paramType = resolveType(p.type, &paramClassName);
            m.paramTypes.push_back(paramType);
            m.paramClassNames.push_back(paramClassName);
            const bool isGeneric = std::find(currentClassTypeParams_.begin(), currentClassTypeParams_.end(), paramClassName) != currentClassTypeParams_.end() ||
                (paramType == ZlType::UNION && containsTypeParameter(p.type, currentClassTypeParams_));
            m.paramIsGeneric.push_back(isGeneric);
            if (paramType == ZlType::FUNCTION) {
                std::vector<ZlType> nestedTypes;
                std::vector<std::string> nestedClasses;
                for (const auto& fp : p.type.functionParamTypes) {
                    std::string cls;
                    nestedTypes.push_back(resolveType(fp, &cls));
                    nestedClasses.push_back(std::move(cls));
                }
                m.functionParamTypes.push_back(std::move(nestedTypes));
                m.functionParamClassNames.push_back(std::move(nestedClasses));
                ZlType nestedReturn = ZlType::UNKNOWN;
                std::string nestedReturnClass;
                if (p.type.functionReturnType) {
                    nestedReturn = resolveType(*p.type.functionReturnType, &nestedReturnClass);
                }
                m.functionReturnTypes.push_back(nestedReturn);
                m.functionReturnClassNames.push_back(std::move(nestedReturnClass));
                m.functionParamHasSignature.push_back(p.type.functionHasSignature);
            } else {
                m.functionParamTypes.emplace_back();
                m.functionParamClassNames.emplace_back();
                m.functionReturnTypes.push_back(ZlType::UNKNOWN);
                m.functionReturnClassNames.emplace_back();
                m.functionParamHasSignature.push_back(false);
            }
        }
        if (m.returnType == ZlType::FUNCTION) {
            m.returnFunctionHasSignature = sig.returnType.functionHasSignature;
            for (const auto& rp : sig.returnType.functionParamTypes) {
                std::string cls;
                m.returnFunctionParamTypes.push_back(resolveType(rp, &cls));
                m.returnFunctionParamClassNames.push_back(std::move(cls));
            }
            if (sig.returnType.functionReturnType) {
                m.returnFunctionReturnType = resolveType(*sig.returnType.functionReturnType,
                                                         &m.returnFunctionReturnClassName);
            }
        }
        currentClassTypeParams_ = savedMethodParams;
        auto existing = info.ownMethods.find(sig.name);
        if (existing != info.ownMethods.end() &&
            paramTypesSuffix(existing->second.paramTypes) == paramTypesSuffix(m.paramTypes)) {
            typeError("interface '" + node->name + "' declares duplicate method/operator '" + sig.name +
                      paramTypesSuffix(m.paramTypes) + "'", sig.line);
        }
        info.ownMethods[sig.name] = m;
    }
    semanticModel_.defineInterface(std::move(info));
}

void TypeChecker::checkImplements(const ClassDecl* node) {
    std::vector<std::string> problems;

    for (const std::string& ifaceName : node->implementsNames) {
        const auto* ifaceIt = semanticModel_.findInterface(ifaceName);
        if (ifaceIt == nullptr) {
            typeError("class '" + node->name + "' implements unknown interface '" + ifaceName + "'", node->line);
        }
        semanticModel_.resolveInterfaceMethods(ifaceName);

        for (const auto& [methodName, requiredSig] : ifaceIt->methods) {
            const ClassMethodInfo* found =
                semanticModel_.findExactMethodInHierarchy(node->name, methodName, requiredSig.paramTypes);
            if (!found) {
                problems.push_back("missing method '" + methodName + "' required by interface '" + ifaceName +
                                    "' (need " + paramTypesSuffix(requiredSig.paramTypes) + ")");
                continue;
            }
            if (found->returnType != requiredSig.returnType ||
                found->returnClassName != requiredSig.returnClassName) {
                problems.push_back(
                    "method '" + methodName + paramTypesSuffix(requiredSig.paramTypes) +
                    "' does not match the return type required by interface '" + ifaceName + "'");
            }
        }
    }

    if (!problems.empty()) {
        std::string message = "class '" + node->name + "' does not fully implement its interface(s):";
        for (const auto& p : problems) message += "\n  - " + p;
        typeError(message, node->line);
    }
}


void TypeChecker::checkClassDecl(const ClassDecl* node) {
    for (const auto& [param, constraints] : node->typeParamInterfaceConstraints) {
        if (std::find(node->typeParams.begin(), node->typeParams.end(), param) == node->typeParams.end()) {
            typeError("generic constraint references unknown type parameter '" + param + "'", node->line);
        }
        std::unordered_set<std::string> seenConstraints;
        for (const auto& ifaceName : constraints) {
            if (!seenConstraints.insert(ifaceName).second) {
                typeError("generic type parameter '" + param + "' repeats interface constraint '" + ifaceName + "'", node->line);
            }
            if (!semanticModel_.hasInterface(ifaceName)) {
                typeError("generic type parameter '" + param + "' has unknown interface constraint '" + ifaceName + "'", node->line);
            }
            semanticModel_.resolveInterfaceMethods(ifaceName);
        }
    }
    // Function signatures for forward references (and now, inherited-method
    // resolution) come straight from the semantic model - already fully built
    // for every class in pass 0b/0c, before any body here gets checked - so
    // there's no separate registration pass needed here (see
    // findMethodInChain, used by inferCall's bare-self-call branch).
    if (!node->implementsNames.empty()) {
        checkImplements(node);
    }

    bool previousClassSuppresses = classSuppressesDeprecation_;
    if (const Annotation* sw = findAnnotation(node->annotations, "SuppressWarnings")) {
        classSuppressesDeprecation_ = sw->argument.value_or("") == "deprecation";
    }

    std::vector<std::string> previousTypeParams = currentClassTypeParams_;
    currentClassTypeParams_ = node->typeParams;

    const std::string previousClassName = currentClassName_;
    currentClassName_ = node->name;
    for (const auto& member : node->members) {
        if (member->kind == NodeKind::VarDecl) {
            const auto* field = static_cast<const VarDecl*>(member.get());
            if (!field->isStatic) continue;
            if (field->initializer) {
                const InferredType init = inferExpr(field->initializer.get());
                if (field->hasExplicitType) {
                    std::string declaredClassName;
                    const ZlType declaredType = resolveType(field->type, &declaredClassName);
                    if (!isAssignable(init.type, declaredType, init.className, declaredClassName)) {
                        typeError("cannot initialize static field '" + node->name + "." + field->name + "' with " + zlTypeName(init.type), field->line);
                    }
                }
            }
            continue;
        }
        if (member->kind == NodeKind::FunctionDecl) {
            checkFunctionDecl(static_cast<const FunctionDecl*>(member.get()));
        }
    }
    currentClassName_ = previousClassName;

    currentClassTypeParams_ = std::move(previousTypeParams);
    classSuppressesDeprecation_ = previousClassSuppresses;
}

void TypeChecker::validateAnnotationTargets(const std::vector<Annotation>& annotations,
                                            annotations::Position position, const std::string& declName) {
    for (const auto& ann : annotations) {
        // An unknown name never reaches here - the parser rejects it at the
        // '@' itself (annotation_rules.hpp). Skip rather than double-report.
        if (!annotations::isKnown(ann.name)) continue;
        if (annotations::allowsOn(ann.name, annotations::acceptableTargets(position))) continue;
        typeError(std::string("annotation '@") + ann.name + "' is not allowed on " +
                      annotations::positionName(position) + " '" + declName + "' - allowed there: " +
                      annotations::allowedNamesFor(position),
                  ann.line);
    }
}

void TypeChecker::checkOverrideAnnotation(const FunctionDecl* node) {
    const auto* classIt = semanticModel_.findClass(node->ownerClassName);
    if (classIt == nullptr) return; // shouldn't happen - registered in pass 0a/0b
    const ClassShapeInfo& cls = *classIt;
    bool annotated = hasAnnotation(node->annotations, "Override");

    // This method's own signature, built the same way registerClassShape
    // does - needed either way (comparing against a parent-class override or
    // an interface's required signature), so build it once up front.
    ClassMethodInfo thisSig;
    std::string returnClassName;
    thisSig.returnType = resolveType(node->returnType, &returnClassName);
    thisSig.returnClassName = returnClassName;
    for (const auto& p : node->params) {
        std::string paramClassName;
        const ZlType paramType = resolveType(p.type, &paramClassName);
        thisSig.paramTypes.push_back(paramType);
        thisSig.paramClassNames.push_back(paramClassName);
        if (paramType == ZlType::FUNCTION) {
            std::vector<ZlType> nestedTypes;
            std::vector<std::string> nestedClasses;
            for (const auto& fp : p.type.functionParamTypes) {
                std::string nestedClass;
                nestedTypes.push_back(resolveType(fp, &nestedClass));
                nestedClasses.push_back(std::move(nestedClass));
            }
            thisSig.functionParamTypes.push_back(std::move(nestedTypes));
            thisSig.functionParamClassNames.push_back(std::move(nestedClasses));
            ZlType nestedReturn = ZlType::UNKNOWN;
            std::string nestedReturnClass;
            if (p.type.functionReturnType) {
                nestedReturn = resolveType(*p.type.functionReturnType, &nestedReturnClass);
            }
            thisSig.functionReturnTypes.push_back(nestedReturn);
            thisSig.functionReturnClassNames.push_back(std::move(nestedReturnClass));
            thisSig.functionParamHasSignature.push_back(p.type.functionHasSignature);
        } else {
            thisSig.functionParamTypes.emplace_back();
            thisSig.functionParamClassNames.emplace_back();
            thisSig.functionReturnTypes.push_back(ZlType::UNKNOWN);
            thisSig.functionReturnClassNames.emplace_back();
            thisSig.functionParamHasSignature.push_back(false);
        }
    }
    if (thisSig.returnType == ZlType::FUNCTION) {
        thisSig.returnFunctionHasSignature = node->returnType.functionHasSignature;
        for (const auto& rp : node->returnType.functionParamTypes) {
            std::string nestedClass;
            thisSig.returnFunctionParamTypes.push_back(resolveType(rp, &nestedClass));
            thisSig.returnFunctionParamClassNames.push_back(std::move(nestedClass));
        }
        if (node->returnType.functionReturnType) {
            thisSig.returnFunctionReturnType = resolveType(*node->returnType.functionReturnType,
                                                           &thisSig.returnFunctionReturnClassName);
        }
    }

    // First check the class-inheritance chain (search starts one level up,
    // at the parent - not at node->ownerClassName itself, which would
    // trivially find this exact method since registerClassShape already
    // registered it).
    if (!cls.parentName.empty()) {
        std::string owner;
        const ClassMethodInfo* parentMethod =
            semanticModel_.findExactMethodInHierarchy(cls.parentName, node->name, thisSig.paramTypes, &owner);
        if (parentMethod) {
            if (!annotated) {
                typeWarning(
                    "method '" + node->ownerClassName + "." + node->name + "' overrides '" + owner + "." +
                    node->name + "' but is not annotated @Override",
                    node->line);
                return;
            }
            if (!sameMethodSignature(thisSig, *parentMethod)) {
                typeError(
                    "method '" + node->name + "' is annotated @Override, but its signature doesn't match '" +
                    owner + "." + node->name + "'",
                    node->line);
            }
            return;
        }
    }

    // Not a class-parent override - check whether it's implementing a
    // method required by one of this class's own `implements` interfaces
    // instead (Java/C# convention: @Override applies to interface
    // implementations too, not just superclass overrides).
    for (const std::string& ifaceName : cls.implementsNames) {
        const auto* ifaceIt = semanticModel_.findInterface(ifaceName);
        if (ifaceIt == nullptr) continue; // unknown interface - reported by checkImplements
        auto methodIt = ifaceIt->methods.find(node->name);
        if (methodIt == ifaceIt->methods.end()) continue;

        if (!annotated) {
            typeWarning(
                "method '" + node->ownerClassName + "." + node->name + "' implements '" + ifaceName + "." +
                node->name + "' but is not annotated @Override",
                node->line);
            return;
        }
        if (!sameMethodSignature(thisSig, methodIt->second)) {
            typeError(
                "method '" + node->name + "' is annotated @Override, but its signature doesn't match '" +
                ifaceName + "." + node->name + "'",
                node->line);
        }
        return;
    }

    // No matching method anywhere - a parent class OR an implemented
    // interface. @Override on it is a mistake; otherwise it's just an
    // ordinary new method, correctly left unannotated.
    if (annotated) {
        typeError(
            "method '" + node->name + "' is annotated @Override, but does not override or implement any "
            "method in '" + node->ownerClassName + "'s parent class or implemented interfaces",
            node->line);
    }
}


namespace {

bool statementAlwaysReturns(const AstNode* node) {
    if (!node) return false;
    switch (node->kind) {
        case NodeKind::ReturnStmt:
        case NodeKind::ThrowStmt:
            return true;
        case NodeKind::BlockStmt: {
            const auto* block = static_cast<const BlockStmt*>(node);
            for (const auto& stmt : block->statements) {
                if (statementAlwaysReturns(stmt.get())) return true;
            }
            return false;
        }
        case NodeKind::TryStmt: {
            const auto* tr = static_cast<const TryStmt*>(node);
            const bool finallyReturns = tr->finallyBlock && statementAlwaysReturns(tr->finallyBlock.get());
            const bool tryReturns = statementAlwaysReturns(tr->tryBlock.get());
            if (finallyReturns) return true;
            if (tr->catches.empty()) return tryReturns;
            for (const auto& clause : tr->catches) {
                if (!statementAlwaysReturns(clause.block.get())) return false;
            }
            return tryReturns;
        }
        case NodeKind::IfStmt: {
            const auto* iff = static_cast<const IfStmt*>(node);
            if (!iff->elseBody || iff->branches.empty()) return false;
            for (const auto& branch : iff->branches) {
                if (!statementAlwaysReturns(branch.body.get())) return false;
            }
            return statementAlwaysReturns(iff->elseBody.get());
        }
        default:
            return false;
    }
}

} // namespace

void TypeChecker::checkFunctionDecl(const FunctionDecl* node) {
    bool previousFunctionIsStatic = currentFunctionIsStatic_;
    bool previousFunctionIsAsync = currentFunctionIsAsync_;
    currentFunctionIsStatic_ = node->isStatic;
    currentFunctionIsAsync_ = node->isAsync;
    // The annotation target check runs on every member func, constructor or
    // not - a method-only annotation (@Override) on a constructor is exactly
    // the misplaced-annotation case the registry exists for.
    validateAnnotationTargets(node->annotations,
                              node->isConstructor ? annotations::POS_CONSTRUCTOR : annotations::POS_METHOD,
                              node->ownerClassName.empty() ? node->name : node->ownerClassName + "." + node->name);
    std::vector<std::string> previousTypeParams = currentClassTypeParams_;
    mergeMethodTypeParams(node->typeParams, node->line);
    currentReturnType_ = resolveType(node->returnType, &currentReturnClassName_);
    currentReturnFunctionParamTypes_.clear();
    currentReturnFunctionParamClassNames_.clear();
    currentReturnFunctionReturnType_ = ZlType::UNKNOWN;
    currentReturnFunctionReturnClassName_.clear();
    currentReturnFunctionHasSignature_ = false;
    if (currentReturnType_ == ZlType::FUNCTION) {
        currentReturnFunctionHasSignature_ = node->returnType.functionHasSignature;
        for (const auto& param : node->returnType.functionParamTypes) {
            std::string cls;
            currentReturnFunctionParamTypes_.push_back(resolveType(param, &cls));
            currentReturnFunctionParamClassNames_.push_back(std::move(cls));
        }
        if (node->returnType.functionReturnType) {
            currentReturnFunctionReturnType_ = resolveType(*node->returnType.functionReturnType,
                                                            &currentReturnFunctionReturnClassName_);
        }
    }
    currentFunctionName_ = node->name;
    currentClassName_ = node->ownerClassName; // "" for a free func; used by checkAccess
    inConstructor_ = node->isConstructor;

    bool previousFunctionSuppresses = functionSuppressesDeprecation_;
    if (const Annotation* sw = findAnnotation(node->annotations, "SuppressWarnings")) {
        functionSuppressesDeprecation_ = sw->argument.value_or("") == "deprecation";
    }

    // @Override is checked regardless of whether the annotation is present -
    // see checkOverrideAnnotation's own comment for the exact warn/error
    // matrix. Doesn't apply to constructors (there's no "overriding" a
    // constructor - each class's constructor is its own thing).
    if (!node->isConstructor && !node->ownerClassName.empty()) {
        checkOverrideAnnotation(node);
    }

    // New scope for the func's parameters and locals.
    symbols_.pushScope();
    movedVariables_.clear();
    borrowSources_.clear();
    namedFunctionBindings_.clear();
    heldLockDepth_ = 0;
    heldLockNames_.clear();
    sharedPayloadOwners_.clear();
    sharedCellAliases_.clear();
    lockAliases_.clear();
    heldSharedLocks_.clear();
    lockOrderRanks_.clear();

    for (const auto& p : node->params) {
        auto info = variableInfo(p.type);
        info.ownership = p.ownership;
        validateOwnership(p.type, p.ownership, p.type.line);
        p.storageName = symbols_.defineVar(p.name, std::move(info)).storageName;
        if (p.ownership == OwnershipKind::BORROW) borrowSources_[p.name] = "<borrow-parameter>";
    }

    if (node->body) {
        // A constructor's `super(...)` call, if present at all, must be
        // exactly the first statement - checked structurally here (not via
        // checkStatement's generic recursion, which has no notion of
        // "statement position") so inferSuperCallExpr can reject every other
        // occurrence, however deeply nested.
        const auto* block = static_cast<const BlockStmt*>(node->body.get());
        bool hasExplicitLeadingSuperCall = false;
        for (std::size_t i = 0; i < block->statements.size(); ++i) {
            const AstNode* stmt = block->statements[i].get();
            bool isDirectSuperCall = stmt->kind == NodeKind::ExprStmt &&
                                      static_cast<const ExprStmt*>(stmt)->expression &&
                                      static_cast<const ExprStmt*>(stmt)->expression->kind == NodeKind::SuperCallExpr;
            allowSuperCallExprHere_ = inConstructor_ && isDirectSuperCall && (i == 0);
            if (i == 0) hasExplicitLeadingSuperCall = allowSuperCallExprHere_;
            checkStatement(stmt);
        }
        allowSuperCallExprHere_ = false;

        // If this constructor doesn't call super(...) itself, the compiler
        // injects an implicit no-arg one (Compiler::compile) - so make sure
        // that's actually going to work: the parent's constructor must
        // accept zero arguments. Otherwise this would previously only fail
        // at runtime, deep inside the VM, instead of here.
        if (inConstructor_ && !hasExplicitLeadingSuperCall) {
            const auto* classIt = semanticModel_.findClass(currentClassName_);
            if (classIt != nullptr && !classIt->parentName.empty()) {
                const std::string& parentName = classIt->parentName;
                const ClassShapeInfo& parent = *semanticModel_.findClass(parentName);
                bool hasNoArgConstructor = false;
                for (const auto& ctor : parent.constructors) {
                    if (ctor.paramTypes.empty()) { hasNoArgConstructor = true; break; }
                }
                if (!hasNoArgConstructor) {
                    typeError(
                        "class '" + currentClassName_ + "' must call super(...) explicitly, as its first "
                        "statement, because '" + parentName + "' has no no-argument constructor",
                        node->line);
                }
            }
        }

        // Non-void functions must return on every reachable path. Constructors
        // are exempt because they return the constructed object implicitly.
        if (!node->isConstructor && currentReturnType_ != ZlType::VOID_TYPE &&
            !statementAlwaysReturns(node->body.get())) {
            typeError("func '" + currentFunctionName_ + "' may reach the end without returning " +
                      zlTypeName(currentReturnType_), node->line);
        }
    }

    symbols_.popScope();
    namedFunctionBindings_.clear();
    currentClassTypeParams_ = std::move(previousTypeParams);
    currentClassName_.clear();
    inConstructor_ = false;
    functionSuppressesDeprecation_ = previousFunctionSuppresses;
    currentFunctionIsStatic_ = previousFunctionIsStatic;
    currentFunctionIsAsync_ = previousFunctionIsAsync;
    heldLockDepth_ = 0;
    heldLockNames_.clear();
    sharedPayloadOwners_.clear();
    sharedCellAliases_.clear();
    lockAliases_.clear();
    heldSharedLocks_.clear();
    lockOrderRanks_.clear();
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void TypeChecker::checkStatement(const AstNode* node) {
    switch (node->kind) {
        case NodeKind::VarDecl:      checkVarDecl(static_cast<const VarDecl*>(node)); return;
        case NodeKind::LogStmt:      checkLogStmt(static_cast<const LogStmt*>(node)); return;
        case NodeKind::LogExpr: {
            const auto* n = static_cast<const LogExpr*>(node);
            if (n->argument) (void)inferExpr(n->argument.get());
            return;
        }
        case NodeKind::IfStmt:       checkIfStmt(static_cast<const IfStmt*>(node)); return;
        case NodeKind::ReturnStmt:   checkReturnStmt(static_cast<const ReturnStmt*>(node)); return;
        case NodeKind::ForStmt:      checkForStmt(static_cast<const ForStmt*>(node)); return;
        case NodeKind::WhileStmt:    checkWhileStmt(static_cast<const WhileStmt*>(node)); return;
        case NodeKind::RepeatStmt:   checkRepeatStmt(static_cast<const RepeatStmt*>(node)); return;
        case NodeKind::BlockStmt:    checkBlock(static_cast<const BlockStmt*>(node)); return;
        case NodeKind::TryStmt:      checkTryStmt(static_cast<const TryStmt*>(node)); return;
        case NodeKind::ThrowStmt:    checkThrowStmt(static_cast<const ThrowStmt*>(node)); return;
        case NodeKind::ExprStmt:     checkExprStmt(static_cast<const ExprStmt*>(node)); return;
        case NodeKind::BreakStmt:    return; // no type implications
        case NodeKind::ContinueStmt: return; // no type implications
        default:
            return; // unknown statement kinds are silently ignored
    }
}

void TypeChecker::checkBlock(const BlockStmt* node) {
    // Blocks introduce lexical variable lifetimes. Ownership flow for names
    // declared outside the block must continue past the block, but borrows
    // introduced inside the block must not escape it. In particular, keep
    // the check name-based only within this lexical boundary and reject any
    // pre-existing borrow whose source resolves to a declaration that dies
    // here; this also prevents shadowing from accidentally extending a
    // short-lived borrow.
    std::unordered_set<std::string> declaredHere;
    for (const auto& stmt : node->statements) {
        if (stmt && stmt->kind == NodeKind::VarDecl) {
            declaredHere.insert(static_cast<const VarDecl*>(stmt.get())->name);
        }
    }

    const std::unordered_set<std::string> movedBefore = movedVariables_;
    const std::unordered_set<std::string> borrowsBefore = [&]() {
        std::unordered_set<std::string> names;
        names.reserve(borrowSources_.size());
        for (const auto& [name, _] : borrowSources_) names.insert(name);
        return names;
    }();
    const auto sharedPayloadOwnersBefore = sharedPayloadOwners_;
    const auto sharedCellAliasesBefore = sharedCellAliases_;
    const auto lockAliasesBefore = lockAliases_;

    symbols_.pushScope();
    for (const auto& stmt : node->statements) {
        checkStatement(stmt.get());
    }

    // A borrow that existed before this block cannot point at a local declared
    // in the block after the block ends. Walk the borrow chain so a transitive
    // borrow cannot smuggle the inner owner outside its lexical lifetime.
    for (const auto& [borrowName, initialSource] : borrowSources_) {
        if (!borrowsBefore.count(borrowName)) continue;
        std::string source = initialSource;
        std::unordered_set<std::string> seen;
        while (!source.empty() && seen.insert(source).second) {
            const auto dot = source.find('.');
            const std::string root = dot == std::string::npos ? source : source.substr(0, dot);
            if (declaredHere.count(root)) {
                typeError("borrow '" + borrowName + "' escapes its lexical owner '" + root + "'", node->line);
            }
            auto next = borrowSources_.find(source);
            if (next == borrowSources_.end()) {
                if (root != source) {
                    auto rootNext = borrowSources_.find(root);
                    if (rootNext != borrowSources_.end()) source = rootNext->second;
                    else break;
                } else {
                    break;
                }
            } else {
                source = next->second;
            }
        }
    }

    // Remove borrows introduced by the block and moves of block-local names.
    for (auto it = borrowSources_.begin(); it != borrowSources_.end();) {
        if (!borrowsBefore.count(it->first)) it = borrowSources_.erase(it);
        else ++it;
    }
    for (const auto& name : declaredHere) {
        if (!movedBefore.count(name)) movedVariables_.erase(name);
    }
    sharedPayloadOwners_ = sharedPayloadOwnersBefore;
    sharedCellAliases_ = sharedCellAliasesBefore;
    lockAliases_ = lockAliasesBefore;
    symbols_.popScope();
}

static std::optional<std::string> resolveSharedCellAlias(
    const AstNode* expr,
    const std::unordered_map<std::string, std::string>& aliases) {
    if (!expr || expr->kind != NodeKind::Identifier) return std::nullopt;
    std::string name = static_cast<const Identifier*>(expr)->name;
    std::unordered_set<std::string> seen;
    while (true) {
        auto it = aliases.find(name);
        if (it == aliases.end()) return name;
        // A cell created by `new Shared<T>(...)` is registered as its own
        // canonical name. That is a self-edge, not a cycle, and has to
        // terminate here rather than fall through to the loop guard.
        if (it->second == name) return name;
        if (!seen.insert(name).second) return std::nullopt;
        name = it->second;
    }
}

static std::optional<std::string> resolveLockAlias(
    const AstNode* expr,
    const std::unordered_map<std::string, std::string>& aliases) {
    if (!expr || expr->kind != NodeKind::Identifier) return std::nullopt;
    std::string name = static_cast<const Identifier*>(expr)->name;
    std::unordered_set<std::string> seen;
    while (true) {
        if (!seen.insert(name).second) return std::nullopt;
        auto it = aliases.find(name);
        if (it == aliases.end()) return name;
        name = it->second;
    }
}

// Resolve an identifier to the Shared cell it aliases, but only when it is
// actually a tracked cell. `resolveSharedCellAlias` answers for any identifier,
// returning it unchanged when it is not in the map - which is what it has to do
// when *registering* an alias, but is wrong here: it made every zero-argument
// `get()` on any object look like a protected Shared payload read, so a plain
// `class Holder<T> { func get(): T }` was rejected with "protected Shared
// payload access requires Shared.withLock".
static std::optional<std::string> trackedSharedCellForExpr(
    const AstNode* expr,
    const std::unordered_map<std::string, std::string>& owners,
    const std::unordered_map<std::string, std::string>& aliases) {
    if (!expr || expr->kind != NodeKind::Identifier) return std::nullopt;
    const std::string& name = static_cast<const Identifier*>(expr)->name;
    if (aliases.count(name)) return resolveSharedCellAlias(expr, aliases);
    auto it = owners.find(name);
    if (it != owners.end()) return it->second;
    return std::nullopt;
}

static std::optional<std::string> sharedPayloadOwnerForExpr(
    const AstNode* expr,
    const std::unordered_map<std::string, std::string>& owners,
    const std::unordered_map<std::string, std::string>& aliases) {
    if (!expr) return std::nullopt;
    if (expr->kind == NodeKind::Identifier) {
        const auto* id = static_cast<const Identifier*>(expr);
        auto it = owners.find(id->name);
        if (it != owners.end()) return it->second;
    }
    if (expr->kind == NodeKind::MethodCallExpr) {
        const auto* call = static_cast<const MethodCallExpr*>(expr);
        if (call->methodName == "get" && call->arguments.empty())
            return trackedSharedCellForExpr(call->object.get(), owners, aliases);
    }
    return std::nullopt;
}

TypeChecker::InferredType TypeChecker::inferIndexAccess(const IndexAccessExpr* node) {
    const InferredType object = inferExpr(node->object.get());
    if (auto owner = sharedPayloadOwnerForExpr(node->object.get(), sharedPayloadOwners_, sharedCellAliases_)) {
        if (std::find(heldSharedLocks_.begin(), heldSharedLocks_.end(), *owner) == heldSharedLocks_.end()) {
            typeError("protected Shared payload access requires Shared.withLock on '" + *owner + "'", node->line);
        }
    }
    const InferredType index = inferExpr(node->index.get());
    // Check the object first: indexing a Map (or anything else) with a string
    // key used to report "list index must be an int", blaming the key when the
    // real problem is that [] only works on List<T>.
    if (object.type != ZlType::OBJECT || object.className.rfind("List<", 0) != 0 || object.className.back() != '>') {
        typeError("indexed access requires a List<T>", node->line);
    }
    if (index.type != ZlType::INT && index.type != ZlType::UNKNOWN) {
        typeError("list index must be an int", node->line);
    }
    InferredType result;
    result.ownership = OwnershipKind::GC;
    if (!object.className.empty()) {
        const std::string prefix = "List<";
        if (object.className.rfind(prefix, 0) == 0 && object.className.back() == '>') {
            // The element type is the generic argument, e.g. "Task<int>" from
            // "List<Task<int>>". It can itself be instantiated, so decode the
            // full name via the shared name<->type helpers rather than
            // re-matching spellings here: a class keeps its full instantiation
            // as its name (`List<int>`, `Shared<int>`, `Widget`), and `Task<T>`
            // carries its value type so `xs[0].block()` type-checks the same as
            // `xs.get(0).block()`.
            const std::string elem = object.className.substr(prefix.size(), object.className.size() - prefix.size() - 1);
            auto inferNamedType = [](InferredType& out, const std::string& fullName) {
                const auto [base, args] = splitGenericName(fullName);
                out.type = zlTypeFromBaseName(base);
                out.className = (out.type == ZlType::OBJECT || out.type == ZlType::TASK) ? fullName : std::string{};
                if (out.type == ZlType::TASK) {
                    out.taskValueType = zlTypeFromBaseName(splitGenericName(args).first);
                    out.taskValueClassName = (out.taskValueType == ZlType::OBJECT) ? args : std::string{};
                }
            };
            inferNamedType(result, elem);
            return result;
        }
    }
    result.type = ZlType::UNKNOWN;
    return result;
}

TypeChecker::InferredType TypeChecker::inferExpected(const AstNode* node, ZlType type,
                                                     const std::string& className, const TypeAnnotation& annotation) {
    struct RestoreExpectation {
        FunctionExpectation& slot;
        FunctionExpectation saved;
        ~RestoreExpectation() { slot = std::move(saved); }
    } restore{currentLambdaExpectation_, std::move(currentLambdaExpectation_)};
    currentLambdaExpectation_ = {};
    if (type == ZlType::FUNCTION && node->kind == NodeKind::LambdaExpr && annotation.functionHasSignature) {
        currentLambdaExpectation_.active = true;
        for (const auto& param : annotation.functionParamTypes) {
            std::string cls;
            currentLambdaExpectation_.paramTypes.push_back(resolveType(param, &cls));
            currentLambdaExpectation_.paramClassNames.push_back(std::move(cls));
        }
        if (annotation.functionReturnType) {
            currentLambdaExpectation_.returnType = resolveType(*annotation.functionReturnType, &currentLambdaExpectation_.returnClassName);
        }
    }
    if (node->kind == NodeKind::CollectionLiteral) {
        // Both branches below bypass inferExpr, which is the one place a node's
        // type is recorded for the backend seam. Record here as well, or a
        // collection literal written under a type annotation - `list<int> xs =
        // [1, 2]` - looks untyped to every backend even though the checker just
        // proved exactly what it is.
        InferredType result;
        if ((type == ZlType::LIST || type == ZlType::MAP || type == ZlType::SET) && annotation.typeArgs.empty()) {
            const auto* literal = static_cast<const CollectionLiteral*>(node);
            if (type == ZlType::SET) literal->targetCollectionKind = "set";
            result = inferCollectionLiteral(literal);
        } else {
            result = InferredType(
                inferCollectionLiteralExpected(static_cast<const CollectionLiteral*>(node), type, className,
                                               annotation),
                className);
        }
        expressionTypes_[node] = result;
        return result;
    }
    return inferExpr(node);
}

void TypeChecker::checkVarDecl(const VarDecl* node) {
    ZlType initType = ZlType::NIL;
    std::string initClassName;
    ZlType initTaskValueType = ZlType::UNKNOWN;
    std::string initTaskValueClassName;

    if (node->hasExplicitType) {
        std::string declaredClassName;
        ZlType declaredType = resolveType(node->type, &declaredClassName);
        validateOwnership(node->type, node->ownership, node->line);

        if (!node->initializer) {
            typeError("explicitly typed local variable '" + node->name + "' must be initialized", node->line);
        }
        const InferredType initResult = inferExpected(node->initializer.get(), declaredType, declaredClassName, node->type);
        initType = initResult.type;
        initClassName = initResult.className;
        initTaskValueType = initResult.taskValueType;
        initTaskValueClassName = initResult.taskValueClassName;
        bool taskAssignable = true;
        if (declaredType == ZlType::FUNCTION && initType == ZlType::FUNCTION) {
            validateFunctionTypeAssignment(node->type, initResult, node->line);
        }
        if (declaredType == ZlType::TASK && initType == ZlType::TASK && node->type.typeArgs.size() == 1) {
            std::string expectedTaskClassName;
            const ZlType expectedTaskValueType = resolveType(node->type.typeArgs[0], &expectedTaskClassName);
            taskAssignable = isAssignable(initTaskValueType, expectedTaskValueType,
                                          initTaskValueClassName, expectedTaskClassName);
        }
        if (!taskAssignable || !isAssignable(initType, declaredType, initClassName, declaredClassName)) {
            typeError(
                "cannot assign " + zlTypeName(initType) + " to variable '" +
                node->name + "' of type " + zlTypeName(declaredType),
                node->line);
        }
        if (initResult.functionIsNamedReference) {
            namedFunctionBindings_[node->name] = NamedFunctionBinding{
                initResult.functionReferenceOwner, initResult.functionReferenceDispatch};
        } else {
            namedFunctionBindings_.erase(node->name);
        }
        if (node->ownership == OwnershipKind::OWNED) {
            if (initResult.ownership != OwnershipKind::OWNED) {
                typeError("owned variable '" + node->name + "' requires an owned initializer (use 'move' for an existing owned value)", node->line);
            }
            if (node->initializer && node->initializer->kind == NodeKind::Identifier) {
                typeError("owned variable '" + node->name + "' cannot copy identifier '" +
                          static_cast<const Identifier*>(node->initializer.get())->name + "'; use 'move " +
                          static_cast<const Identifier*>(node->initializer.get())->name + "'", node->line);
            }
        } else if (node->ownership == OwnershipKind::BORROW) {
            auto region = borrowRegionForExpr(node->initializer.get());
            if (region) {
                borrowSources_[node->name] = *region;
            } else if (node->initializer && node->initializer->kind == NodeKind::Identifier) {
                const auto* rhsId = static_cast<const Identifier*>(node->initializer.get());
                auto rhsInfo = symbols_.lookupVar(rhsId->name);
                if (!rhsInfo) typeError("cannot borrow undefined variable '" + rhsId->name + "'", node->line);
                if (rhsInfo->ownership == OwnershipKind::BORROW) {
                    auto it = borrowSources_.find(rhsId->name);
                    if (it != borrowSources_.end()) borrowSources_[node->name] = it->second;
                    else borrowSources_[node->name] = rhsId->name;
                } else {
                    borrowSources_[node->name] = rhsId->name;
                }
            } else {
                typeError("borrow variable '" + node->name + "' must be initialized from an existing variable or owned field", node->line);
            }
        } else if (initResult.ownership == OwnershipKind::BORROW) {
            typeError("cannot initialize non-borrow variable '" + node->name + "' from a borrowed value", node->line);
        }
        auto info = variableInfo(node->type);
        info.isConst = node->isConst;
        info.ownership = node->ownership;
        info.functionIsAsync = initResult.functionIsAsync;
        info.functionCaptureNames = initResult.functionCaptureNames;
        info.functionUsesThis = initResult.functionUsesThis;
        symbols_.defineVar(node->name, std::move(info));
    } else {
        InferredType initResult;
        bool hasInitializer = false;
        if (node->initializer) {
            initResult = inferExpr(node->initializer.get());
            hasInitializer = true;
            initType = initResult.type;
            initClassName = initResult.className;
        }
        if (initResult.ownership == OwnershipKind::BORROW) {
            typeError("borrowed values require an explicit 'borrow' declaration for '" + node->name + "'", node->line);
        }
        const OwnershipKind inferredOwnership =
            (node->initializer && node->initializer->kind == NodeKind::MoveExpr)
                ? OwnershipKind::OWNED : OwnershipKind::GC;
        if (initResult.functionIsNamedReference) {
            namedFunctionBindings_[node->name] = NamedFunctionBinding{
                initResult.functionReferenceOwner, initResult.functionReferenceDispatch};
        } else {
            namedFunctionBindings_.erase(node->name);
        }
        if (initType == ZlType::FUNCTION) {
            symbols_.defineVar(node->name, initType, node->isConst, initClassName,
                               initResult.functionParamTypes, initResult.functionReturnType,
                               ZlType::UNKNOWN, "", std::nullopt, ZlType::UNKNOWN,
                               initResult.functionCaptureNames, initResult.functionUsesThis, initResult.functionHasSignature, inferredOwnership);
            symbols_.setFunctionSignature(node->name, initResult.functionParamClassNames,
                                           initResult.functionReturnClassName, initResult.functionIsAsync, initResult.functionHasSignature);
        } else if (hasInitializer && initType == ZlType::TASK) {
            symbols_.defineVar(node->name, initType, node->isConst, initClassName,
                               {}, ZlType::UNKNOWN, initResult.taskValueType, initResult.taskValueClassName, std::nullopt, ZlType::UNKNOWN, {}, false, false, node->ownership);
        } else {
            symbols_.defineVar(node->name, initType, node->isConst, initClassName, {}, ZlType::UNKNOWN, ZlType::UNKNOWN, "", std::nullopt, ZlType::UNKNOWN, {}, false, false, inferredOwnership);
        }
    }
    node->storageName = symbols_.lookupVar(node->name)->storageName;
    if (node->hasExplicitType) symbols_.binding(node->name).annotation = node->type;
    node->assertedTypeName = symbols_.lookupVar(node->name)->runtimeTypeName();
    if (node->initializer) {
        if (auto owner = sharedPayloadOwnerForExpr(node->initializer.get(), sharedPayloadOwners_, sharedCellAliases_))
            sharedPayloadOwners_[node->name] = *owner;
        else
            sharedPayloadOwners_.erase(node->name);

        // A `new Shared<T>(...)` infers the bare class name "Shared" while a
        // copy of an existing cell keeps the instantiated "Shared<T>", so both
        // spellings have to register the var as a cell. Without the bare form
        // the cell was never tracked, and the payload guard only appeared to
        // work because it used to treat every zero-argument `get()` as a
        // Shared read.
        const bool looksLikeSharedCell =
            initType == ZlType::OBJECT &&
            (initClassName == "Shared" || initClassName.rfind("Shared<", 0) == 0);
        if (looksLikeSharedCell) {
            if (auto alias = resolveSharedCellAlias(node->initializer.get(), sharedCellAliases_))
                sharedCellAliases_[node->name] = *alias;
            else
                // `new Shared<T>(...)` is not an alias of anything else - it is
                // a fresh cell, so it has to register as its own. Without this
                // only copies of a cell were tracked and the cell built by the
                // `new` itself escaped the payload guard entirely.
                sharedCellAliases_[node->name] = node->name;
        } else {
            sharedCellAliases_.erase(node->name);
        }

        const bool looksLikeLock = initType == ZlType::OBJECT &&
            (initClassName == "Mutex" || initClassName == "RwLock");
        if (looksLikeLock) {
            if (auto alias = resolveLockAlias(node->initializer.get(), lockAliases_))
                lockAliases_[node->name] = *alias;
            else
                lockAliases_.erase(node->name);
        } else {
            lockAliases_.erase(node->name);
        }
    } else {
        sharedPayloadOwners_.erase(node->name);
        sharedCellAliases_.erase(node->name);
        lockAliases_.erase(node->name);
    }
}

void TypeChecker::validateFunctionTypeAssignment(const TypeAnnotation& expected, const InferredType& actual, std::size_t line) {
    if (expected.name != "func" || actual.type != ZlType::FUNCTION) return;
    if (!expected.functionHasSignature) return;
    if (expected.functionParamTypes.size() != actual.functionParamTypes.size()) {
        typeError("lambda has " + std::to_string(actual.functionParamTypes.size()) +
                  " parameter(s), expected " + std::to_string(expected.functionParamTypes.size()), line);
    }
    for (std::size_t i = 0; i < expected.functionParamTypes.size(); ++i) {
        std::string expectedClass;
        const ZlType expectedType = resolveType(expected.functionParamTypes[i], &expectedClass);
        const std::string actualClass = i < actual.functionParamClassNames.size() ? actual.functionParamClassNames[i] : std::string();
        const std::string actualTypeName = zlTypeName(actual.functionParamTypes[i]);
        if (expectedType == ZlType::UNKNOWN || actual.functionParamTypes[i] == ZlType::UNKNOWN) continue;
        if (!isAssignable(actual.functionParamTypes[i], expectedType, actualClass, expectedClass) ||
            !isAssignable(expectedType, actual.functionParamTypes[i], expectedClass, actualClass)) {
            typeError("lambda parameter " + std::to_string(i + 1) + " has type " + actualTypeName +
                      ", expected " + zlTypeName(expectedType), line);
        }
    }
    if (!expected.functionReturnType) return;
    std::string expectedReturnClass;
    const ZlType expectedReturn = resolveType(*expected.functionReturnType, &expectedReturnClass);
    if (expectedReturn != ZlType::UNKNOWN && actual.functionReturnType != ZlType::UNKNOWN &&
        (!isAssignable(actual.functionReturnType, expectedReturn, actual.functionReturnClassName, expectedReturnClass) ||
         !isAssignable(expectedReturn, actual.functionReturnType, expectedReturnClass, actual.functionReturnClassName))) {
        typeError("lambda return type is " + zlTypeName(actual.functionReturnType) +
                  ", expected " + zlTypeName(expectedReturn), line);
    }
}

void TypeChecker::validateFunctionArguments(const std::string& calleeName, const ClassMethodInfo& method,
                                            const InferredArguments& args, std::size_t line) {
    for (std::size_t i = 0; i < method.paramTypes.size() && i < args.types.size(); ++i) {
        if (method.paramTypes[i] != ZlType::FUNCTION || i >= method.functionParamTypes.size()) continue;
        // A parameter declared bare `func` has no signature to check against;
        // that spelling stays dynamically checked.
        if (i < method.functionParamHasSignature.size() && !method.functionParamHasSignature[i]) continue;
        // The argument has to carry a signature of its own to be comparable.
        // A bare `func` value forwarded from somewhere else has none, and
        // refusing it would take away the compatibility escape hatch bare
        // `func` exists for - the runtime type assertion still covers it.
        if (i >= args.functionHasSignature.size() || !args.functionHasSignature[i]) continue;
        const auto& expectedParams = method.functionParamTypes[i];
        const auto& actualParams = args.functionParamTypes[i];
        if (expectedParams.size() != actualParams.size()) {
            typeError("argument " + std::to_string(i + 1) + " to func '" + calleeName +
                      "': expected a func with " + std::to_string(expectedParams.size()) +
                      " parameter(s), got " + std::to_string(actualParams.size()), line);
        }
        for (std::size_t p = 0; p < expectedParams.size() && p < actualParams.size(); ++p) {
            const std::string expectedClass =
                (i < method.functionParamClassNames.size() &&
                 p < method.functionParamClassNames[i].size())
                    ? method.functionParamClassNames[i][p] : std::string();
            const std::string actualClass =
                (i < args.functionParamClassNames.size() &&
                 p < args.functionParamClassNames[i].size())
                    ? args.functionParamClassNames[i][p] : std::string();
            if (expectedParams[p] != ZlType::UNKNOWN && actualParams[p] != ZlType::UNKNOWN &&
                (!isAssignable(actualParams[p], expectedParams[p], actualClass, expectedClass) ||
                 !isAssignable(expectedParams[p], actualParams[p], expectedClass, actualClass))) {
                typeError("argument " + std::to_string(i + 1) + " to func '" + calleeName +
                          "': incompatible func parameter type", line);
            }
        }
        const ZlType expectedReturn = i < method.functionReturnTypes.size()
            ? method.functionReturnTypes[i] : ZlType::UNKNOWN;
        const ZlType actualReturn = i < args.functionReturnTypes.size()
            ? args.functionReturnTypes[i] : ZlType::UNKNOWN;
        const std::string actualReturnClass = i < args.functionReturnClassNames.size()
            ? args.functionReturnClassNames[i] : std::string();
        const std::string expectedReturnClass = i < method.functionReturnClassNames.size()
            ? method.functionReturnClassNames[i] : std::string();
        if (expectedReturn != ZlType::UNKNOWN && actualReturn != ZlType::UNKNOWN &&
            (!isAssignable(actualReturn, expectedReturn, actualReturnClass, expectedReturnClass) ||
             !isAssignable(expectedReturn, actualReturn, expectedReturnClass, actualReturnClass))) {
            typeError("argument " + std::to_string(i + 1) + " to func '" + calleeName +
                      "': incompatible func return type", line);
        }
    }
}

void TypeChecker::checkLogStmt(const LogStmt* node) {
    if (node->argument) (void)inferExpr(node->argument.get());
}

std::optional<std::string> TypeChecker::borrowRootOwner(const std::string& name) const {
    // A borrow can have path-sensitive alternatives after a CFG join. The
    // flow solver encodes them as `regionA|regionB`. A lifetime proof survives
    // such a join only when every alternative ultimately roots in the same
    // owned local/parameter.
    std::vector<std::string> alternatives;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto sep = name.find('|', start);
        alternatives.push_back(name.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    if (alternatives.empty()) return std::nullopt;

    std::optional<std::string> commonOwner;
    for (std::string current : alternatives) {
        std::unordered_set<std::string> seen;
        std::optional<std::string> owner;
        while (!current.empty() && seen.insert(current).second) {
            const auto sep = current.find_first_of(".[");
            const std::string root = sep == std::string::npos ? current : current.substr(0, sep);
            auto info = symbols_.lookupVar(root);
            if (info && info->ownership == OwnershipKind::OWNED) {
                owner = root;
                break;
            }

            auto it = borrowSources_.find(root);
            if (it != borrowSources_.end()) {
                current = it->second;
                continue;
            }
            it = borrowSources_.find(current);
            if (it != borrowSources_.end()) {
                current = it->second;
                continue;
            }
            break;
        }
        if (!owner) return std::nullopt;
        if (!commonOwner) commonOwner = owner;
        else if (*commonOwner != *owner) return std::nullopt;
    }
    return commonOwner;
}

std::optional<std::string> TypeChecker::borrowRegionForExpr(const AstNode* node) const {
    if (!node) return std::nullopt;
    if (node->kind == NodeKind::Identifier) {
        const auto* id = static_cast<const Identifier*>(node);
        auto info = symbols_.lookupVar(id->name);
        if (!info) return std::nullopt;
        if (info->ownership == OwnershipKind::OWNED)
            return id->name;
        auto it = borrowSources_.find(id->name);
        if (it != borrowSources_.end())
            return it->second;
        return std::nullopt;
    }
    if (node->kind == NodeKind::IndexAccessExpr) {
        const auto* index = static_cast<const IndexAccessExpr*>(node);
        auto baseRegion = borrowRegionForExpr(index->object.get());
        if (!baseRegion) return std::nullopt;
        // A constant index gets a stable logical element region. Dynamic indices
        // conservatively alias the whole element storage because the compiler cannot
        // prove which slot is referenced. Structural mutation still invalidates all
        // element regions because push/pop can relocate storage.
        if (auto indexRegion = constantIndexRegion(index->index.get()))
            return *baseRegion + *indexRegion;
        return *baseRegion + "[*]";
    }
    if (node->kind != NodeKind::FieldAccessExpr) return std::nullopt;

    const auto* field = static_cast<const FieldAccessExpr*>(node);
    if (field->isStaticFieldAccess || field->isEnumMemberAccess || field->isMathConstantAccess ||
        field->isFunctionReference || !field->object) {
        return std::nullopt;
    }

    auto baseRegion = borrowRegionForExpr(field->object.get());
    if (!baseRegion) return std::nullopt;

    std::function<std::optional<std::string>(const AstNode*)> classForExpr =
        [&](const AstNode* expr) -> std::optional<std::string> {
            if (!expr) return std::nullopt;
            if (expr->kind == NodeKind::Identifier) {
                const auto* id = static_cast<const Identifier*>(expr);
                auto info = symbols_.lookupVar(id->name);
                if (!info || info->type != ZlType::OBJECT || info->className.empty()) return std::nullopt;
                return info->className;
            }
            if (expr->kind != NodeKind::FieldAccessExpr) return std::nullopt;
            const auto* fa = static_cast<const FieldAccessExpr*>(expr);
            if (fa->isStaticFieldAccess || fa->isEnumMemberAccess || fa->isMathConstantAccess || fa->isFunctionReference)
                return std::nullopt;
            auto parentClass = classForExpr(fa->object.get());
            if (!parentClass) return std::nullopt;
            const ClassFieldInfo* parentField = semanticModel_.findFieldInHierarchy(*parentClass, fa->fieldName, nullptr);
            if (!parentField || parentField->type != ZlType::OBJECT || parentField->className.empty()) return std::nullopt;
            return parentField->className;
        };

    auto ownerClass = classForExpr(field->object.get());
    if (!ownerClass) return std::nullopt;
    const ClassFieldInfo* fieldInfo = semanticModel_.findFieldInHierarchy(*ownerClass, field->fieldName, nullptr);
    if (!fieldInfo || fieldInfo->isStatic || fieldInfo->ownership != OwnershipKind::OWNED) return std::nullopt;
    return *baseRegion + "." + field->fieldName;
}

bool TypeChecker::regionOverlaps(const std::string& a, const std::string& b) {
    const auto split = [](const std::string& value) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto sep = value.find('|', start);
            out.push_back(value.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        return out;
    };
    const auto lefts = split(a);
    const auto rights = split(b);
    const auto prefix = [](const std::string& shorter, const std::string& longer) {
        return longer.size() > shorter.size() &&
               longer.compare(0, shorter.size(), shorter) == 0 &&
               (longer[shorter.size()] == '.' || longer[shorter.size()] == '[');
    };
    const auto wildcardElementRegion = [](const std::string& region) {
        return region.size() >= 3 && region.compare(region.size() - 3, 3, "[*]") == 0;
    };
    const auto wildcardOverlaps = [&](const std::string& a, const std::string& b) {
        if (wildcardElementRegion(a)) {
            const std::string base = a.substr(0, a.size() - 3);
            if (b.size() > base.size() && b.compare(0, base.size(), base) == 0 && b[base.size()] == '[') return true;
        }
        if (wildcardElementRegion(b)) {
            const std::string base = b.substr(0, b.size() - 3);
            if (a.size() > base.size() && a.compare(0, base.size(), base) == 0 && a[base.size()] == '[') return true;
        }
        return false;
    };
    for (const auto& left : lefts) {
        for (const auto& right : rights) {
            if (left == right || prefix(left, right) || prefix(right, left) || wildcardOverlaps(left, right)) return true;
        }
    }
    return false;
}

std::optional<std::string> TypeChecker::constantIndexRegion(const AstNode* node) {
    if (!node || node->kind != NodeKind::Literal) return std::nullopt;
    const auto* literal = static_cast<const Literal*>(node);
    if (literal->literalType != TokenType::INT_LITERAL) return std::nullopt;
    try {
        const long long index = std::stoll(literal->raw);
        if (index < 0) return std::nullopt;
        return "[" + std::to_string(index) + "]";
    } catch (...) {
        return std::nullopt;
    }
}


TypeChecker::OwnershipFlowState TypeChecker::captureOwnershipState() const {
    OwnershipFlowState state;
    state.moved = movedVariables_;
    state.borrows = borrowSources_;
    state.sharedPayloadOwners = sharedPayloadOwners_;
    state.sharedCellAliases = sharedCellAliases_;
    state.lockAliases = lockAliases_;
    return state;
}

void TypeChecker::restoreOwnershipState(const OwnershipFlowState& state) {
    movedVariables_ = state.moved;
    borrowSources_ = state.borrows;
    sharedPayloadOwners_ = state.sharedPayloadOwners;
    sharedCellAliases_ = state.sharedCellAliases;
    lockAliases_ = state.lockAliases;
}

TypeChecker::OwnershipFlowState TypeChecker::joinOwnershipStates(
    const std::vector<OwnershipFlowState>& states) const {
    OwnershipFlowState joined;
    if (states.empty()) return joined;

    // A move is valid after a join only when every path preserved the source.
    // Therefore the post-join moved set is the union: using a variable is
    // unsafe if any reachable path consumed it.
    joined.moved = states.front().moved;
    for (std::size_t i = 1; i < states.size(); ++i) {
        for (const auto& name : states[i].moved) joined.moved.insert(name);
    }

    // A borrow is live after the join when any path leaves that borrow live.
    // Preserve path-sensitive regions rather than collapsing different paths
    // to an unrooted sentinel. The root solver can prove a common owner when
    // appropriate, while overlap checks remain conservative across alternatives.
    for (const auto& state : states) {
        for (const auto& [borrowName, sourceName] : state.borrows) {
            auto it = joined.borrows.find(borrowName);
            if (it == joined.borrows.end()) {
                joined.borrows.emplace(borrowName, sourceName);
            } else if (it->second != sourceName) {
                std::vector<std::string> regions;
                auto addRegions = [&regions](const std::string& encoded) {
                    std::size_t start = 0;
                    while (start <= encoded.size()) {
                        const auto sep = encoded.find('|', start);
                        const std::string region = encoded.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
                        if (!region.empty() && std::find(regions.begin(), regions.end(), region) == regions.end()) regions.push_back(region);
                        if (sep == std::string::npos) break;
                        start = sep + 1;
                    }
                };
                addRegions(it->second);
                addRegions(sourceName);
                std::sort(regions.begin(), regions.end());
                it->second.clear();
                for (std::size_t i = 0; i < regions.size(); ++i) {
                    if (i) it->second += '|';
                    it->second += regions[i];
                }
            }
        }
    }
    // Shared-cell aliases and payload provenance are valid after a join only
    // when every reachable path agrees on the same canonical cell. If any path
    // clears, changes, or introduces a different identity for a name, discard
    // that provenance rather than granting a false protection proof.
    const auto joinMapsByAgreement = [](const auto& states, auto selector) {
        using Map = std::decay_t<decltype(selector(states.front()))>;
        Map result;
        if (states.empty()) return result;
        const auto& first = selector(states.front());
        for (const auto& [name, value] : first) {
            bool agree = true;
            for (std::size_t i = 1; i < states.size(); ++i) {
                const auto& current = selector(states[i]);
                auto it = current.find(name);
                if (it == current.end() || it->second != value) {
                    agree = false;
                    break;
                }
            }
            if (agree) result.emplace(name, value);
        }
        return result;
    };
    joined.sharedPayloadOwners = joinMapsByAgreement(states, [](const OwnershipFlowState& state) -> const auto& {
        return state.sharedPayloadOwners;
    });
    joined.sharedCellAliases = joinMapsByAgreement(states, [](const OwnershipFlowState& state) -> const auto& {
        return state.sharedCellAliases;
    });
    joined.lockAliases = joinMapsByAgreement(states, [](const OwnershipFlowState& state) -> const auto& {
        return state.lockAliases;
    });

    return joined;
}

void TypeChecker::checkIfStmt(const IfStmt* node) {
    const OwnershipFlowState incoming = captureOwnershipState();
    std::vector<OwnershipFlowState> exits;
    const auto entryTypes = symbols_.snapshot();
    auto fallthroughTypes = entryTypes;
    std::vector<SymbolTable::ScopeState> typeExits;
    for (const auto& branch : node->branches) {
        restoreOwnershipState(incoming);
        symbols_.restore(fallthroughTypes);
        if (branch.condition) (void)inferExpr(branch.condition.get());
        fallthroughTypes = symbols_.snapshot();
        if (branch.body) checkStatement(branch.body.get());
        exits.push_back(captureOwnershipState());
        typeExits.push_back(symbols_.snapshot());
    }
    symbols_.restore(fallthroughTypes);
    if (node->elseBody) {
        restoreOwnershipState(incoming);
        checkStatement(node->elseBody.get());
        exits.push_back(captureOwnershipState());
        typeExits.push_back(symbols_.snapshot());
    } else {
        exits.push_back(incoming);
        typeExits.push_back(fallthroughTypes);
    }
    restoreOwnershipState(joinOwnershipStates(exits));
    symbols_.joinRefinements(entryTypes, typeExits);
}

void TypeChecker::checkReturnStmt(const ReturnStmt* node) {
    ZlType returnedType = ZlType::NIL; // bare `return` returns nil
    std::string returnedClassName;
    InferredType returnedResult;

    // A function-typed return is an expected-type position just like a typed
    // local initializer: untyped lambda parameters should inherit the declared
    // signature and its return type should be checked immediately.
    FunctionExpectation previousExpectation = currentLambdaExpectation_;
    if (currentReturnType_ == ZlType::FUNCTION && currentReturnFunctionHasSignature_ && node->value &&
        node->value->kind == NodeKind::LambdaExpr) {
        currentLambdaExpectation_ = {};
        currentLambdaExpectation_.active = true;
        currentLambdaExpectation_.paramTypes = currentReturnFunctionParamTypes_;
        currentLambdaExpectation_.paramClassNames = currentReturnFunctionParamClassNames_;
        currentLambdaExpectation_.returnType = currentReturnFunctionReturnType_;
        currentLambdaExpectation_.returnClassName = currentReturnFunctionReturnClassName_;
    }
    if (node->value) {
        returnedResult = inferExpr(node->value.get());
        returnedType = returnedResult.type;
        returnedClassName = returnedResult.className;
    }
    currentLambdaExpectation_ = std::move(previousExpectation);

    if (returnedResult.ownership == OwnershipKind::BORROW) {
        typeError("cannot return a borrowed value from func '" + currentFunctionName_ + "'", node->line);
    }

    if (currentReturnType_ == ZlType::VOID_TYPE && node->value) {
        typeError("void func '" + currentFunctionName_ + "' cannot return a value", node->line);
    }
    if (currentReturnType_ != ZlType::VOID_TYPE &&
        !isAssignable(returnedType, currentReturnType_, returnedClassName, currentReturnClassName_)) {
        typeError(
            "func '" + currentFunctionName_ + "' returns " +
            zlTypeName(returnedType) + " but expected " +
            zlTypeName(currentReturnType_),
            node->line);
    }
    if (currentFunctionName_ == "<lambda>") {
        lastFunctionHadReturn_ = true;
        if (returnedType != ZlType::UNKNOWN) {
            if (lastFunctionReturnType_ == ZlType::UNKNOWN) {
                lastFunctionReturnType_ = returnedType;
                lastFunctionReturnClassName_ = returnedClassName;
            } else if (lastFunctionReturnType_ != returnedType || lastFunctionReturnClassName_ != returnedClassName) {
                if (!isAssignable(lastFunctionReturnType_, returnedType, lastFunctionReturnClassName_, returnedClassName) ||
                    !isAssignable(returnedType, lastFunctionReturnType_, returnedClassName, lastFunctionReturnClassName_)) {
                    typeError("lambda block returns incompatible types", node->line);
                }
            }
        }
    }

    if (currentReturnType_ == ZlType::FUNCTION && currentReturnFunctionHasSignature_ && returnedType == ZlType::FUNCTION) {
        if (currentReturnFunctionParamTypes_.size() != returnedResult.functionParamTypes.size()) {
            typeError("func '" + currentFunctionName_ + "' returns a func with " +
                      std::to_string(returnedResult.functionParamTypes.size()) +
                      " parameter(s), expected " +
                      std::to_string(currentReturnFunctionParamTypes_.size()), node->line);
        }
        for (std::size_t i = 0; i < currentReturnFunctionParamTypes_.size(); ++i) {
            const std::string expectedClass =
                i < currentReturnFunctionParamClassNames_.size()
                    ? currentReturnFunctionParamClassNames_[i] : std::string();
            const std::string actualClass =
                i < returnedResult.functionParamClassNames.size()
                    ? returnedResult.functionParamClassNames[i] : std::string();
            if (currentReturnFunctionParamTypes_[i] != ZlType::UNKNOWN &&
                returnedResult.functionParamTypes[i] != ZlType::UNKNOWN &&
                (!isAssignable(returnedResult.functionParamTypes[i], currentReturnFunctionParamTypes_[i], actualClass, expectedClass) ||
                 !isAssignable(currentReturnFunctionParamTypes_[i], returnedResult.functionParamTypes[i], expectedClass, actualClass))) {
                typeError("func '" + currentFunctionName_ + "' returns an incompatible func parameter type", node->line);
            }
        }
        if (currentReturnFunctionReturnType_ != ZlType::UNKNOWN && returnedResult.functionReturnType != ZlType::UNKNOWN &&
            (!isAssignable(returnedResult.functionReturnType, currentReturnFunctionReturnType_,
                           returnedResult.functionReturnClassName, currentReturnFunctionReturnClassName_) ||
             !isAssignable(currentReturnFunctionReturnType_, returnedResult.functionReturnType,
                           currentReturnFunctionReturnClassName_, returnedResult.functionReturnClassName))) {
            typeError("func '" + currentFunctionName_ + "' returns an incompatible func return type", node->line);
        }
    }
}

void TypeChecker::checkForStmt(const ForStmt* node) {
    for (const auto& assigned : collectLambdaCaptureRefs(node, {}).assignedNames) symbols_.invalidate(assigned);
    ZlType startType = inferExpr(node->start.get());
    ZlType endType = inferExpr(node->end.get());
    ZlType stepType = inferExpr(node->step.get());

    if ((startType != ZlType::INT && startType != ZlType::DOUBLE) ||
        (endType != ZlType::INT && endType != ZlType::DOUBLE) ||
        (stepType != ZlType::INT && stepType != ZlType::DOUBLE)) {
        typeError("for-loop range and step must be numbers", node->line);
    }
    ZlType loopType = (startType == ZlType::DOUBLE || endType == ZlType::DOUBLE ||
                       stepType == ZlType::DOUBLE) ? ZlType::DOUBLE : ZlType::INT;
    symbols_.pushScope();
    node->storageName = symbols_.defineVar(node->varName, loopType, /*isConst=*/false).storageName;

    const OwnershipFlowState incoming = captureOwnershipState();
    if (node->body) {
        checkStatement(node->body.get());
        const OwnershipFlowState bodyExit = captureOwnershipState();
        // A range can execute zero times, so the body state and incoming state
        // are both reachable after the loop.
        restoreOwnershipState(joinOwnershipStates({incoming, bodyExit}));
    }
    symbols_.popScope();
}

void TypeChecker::checkWhileStmt(const WhileStmt* node) {
    for (const auto& assigned : collectLambdaCaptureRefs(node, {}).assignedNames) symbols_.invalidate(assigned);
    if (node->condition) (void)inferExpr(node->condition.get());
    const OwnershipFlowState incoming = captureOwnershipState();
    if (node->body) {
        checkStatement(node->body.get());
        const OwnershipFlowState bodyExit = captureOwnershipState();
        // A while loop can execute zero times; after the loop either state may
        // be observed, so ownership must be conservative.
        restoreOwnershipState(joinOwnershipStates({incoming, bodyExit}));
    }
}

void TypeChecker::checkRepeatStmt(const RepeatStmt* node) {
    for (const auto& assigned : collectLambdaCaptureRefs(node, {}).assignedNames) symbols_.invalidate(assigned);
    // do-while executes the body at least once, then may execute it again.
    if (node->body) checkStatement(node->body.get());
    if (node->condition) (void)inferExpr(node->condition.get());
}

void TypeChecker::checkTryStmt(const TryStmt* node) {
    if (node->tryBlock) checkStatement(node->tryBlock.get());

    std::vector<std::string> typedCatchClasses;
    bool seenCatchAll = false;
    for (const auto& clause : node->catches) {
        symbols_.pushScope();
        if (seenCatchAll) {
            throw TypeCheckError("catch clause is unreachable after a catch-all clause");
        }
        if (clause.type) {
            std::string catchClass;
            const ZlType catchType = resolveType(*clause.type, &catchClass);
            if (catchType != ZlType::OBJECT || catchClass.empty() ||
                (!semanticModel_.isSubclassOf(catchClass, "Exception") && catchClass != "Exception")) {
                throw TypeCheckError("catch type must derive from Exception");
            }
            for (const std::string& earlier : typedCatchClasses) {
                if (catchClass == earlier || semanticModel_.isSubclassOf(catchClass, earlier)) {
                    throw TypeCheckError("unreachable catch clause: " + catchClass + " is shadowed by " + earlier);
                }
            }
            typedCatchClasses.push_back(catchClass);
            symbols_.defineVar(clause.varName, ZlType::OBJECT, false, catchClass);
        } else {
            seenCatchAll = true;
            symbols_.defineVar(clause.varName, ZlType::STRING, false);
        }
        clause.storageName = symbols_.lookupVar(clause.varName)->storageName;
        checkStatement(clause.block.get());
        symbols_.popScope();
    }
    if (node->finallyBlock) checkStatement(node->finallyBlock.get());
}

void TypeChecker::checkThrowStmt(const ThrowStmt* node) {
    if (!node->value) return;
    const InferredType thrown = inferExpr(node->value.get());
    const ZlType thrownType = thrown.type;
    const std::string thrownClass = thrown.className;
    if (thrownType != ZlType::OBJECT || thrownClass.empty() ||
        (!semanticModel_.isSubclassOf(thrownClass, "Exception") && thrownClass != "Exception")) {
        throw TypeCheckError("throw requires an Exception-derived object");
    }
}

void TypeChecker::checkExprStmt(const ExprStmt* node) {
    if (node->expression) (void)inferExpr(node->expression.get());
}

// ---------------------------------------------------------------------------
// Expressions - type inference
// ---------------------------------------------------------------------------

// A container expectation is only propagable when the parameter is a
// parameterized collection - `map<string,int>`, `list<int>`, `Set<int>`. The
// element types live in the name, and without them
// inferCollectionLiteralExpected has nothing to check the literal against and
// says so ("map literal requires map<K,V> type information"). A bare `list`
// slot, an array (whose size an empty literal cannot satisfy), and everything
// non-collection are left alone.
static bool isPropagableContainer(ZlType type, const std::string& className) {
    if (className.empty() || className.find('<') == std::string::npos) return false;
    if (type == ZlType::LIST || type == ZlType::SET || type == ZlType::MAP) return true;
    if (type != ZlType::OBJECT) return false;
    const std::string base = className.substr(0, className.find('<'));
    return base == "List" || base == "Map" || base == "Set";
}

TypeChecker::ArgumentExpectation TypeChecker::argumentExpectation(ZlType type, const std::string& className) const {
    ArgumentExpectation expectation;
    expectation.seen = true;
    expectation.type = type;
    expectation.className = className;
    expectation.usable = isPropagableContainer(type, className);
    if (expectation.usable) expectation.annotation = typeAnnotationFromName(parseTypeName(className));
    return expectation;
}

std::vector<TypeChecker::ArgumentExpectation> TypeChecker::argumentExpectations(
    const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
    std::size_t argumentCount) const {
    std::vector<ArgumentExpectation> expectations(argumentCount);
    for (const auto& candidate : candidates) {
        const ClassMethodInfo& method = candidate.second;
        // Overload resolution only ever considers arity matches, so a
        // candidate of a different arity cannot be the one that ends up
        // receiving this argument - and its parameter types must not veto a
        // position either.
        if (method.paramTypes.size() != argumentCount) continue;
        for (std::size_t i = 0; i < argumentCount; ++i) {
            ArgumentExpectation& slot = expectations[i];
            const std::string className =
                i < method.paramClassNames.size() ? method.paramClassNames[i] : std::string();
            if (!slot.seen) {
                slot = argumentExpectation(method.paramTypes[i], className);
                continue;
            }
            // Two overloads that want different containers at this position
            // (`f(map<K,V>)` and `f(list<T>)`) give the literal no single
            // answer. Drop the expectation and let resolution report the
            // mismatch rather than guessing.
            if (slot.type != method.paramTypes[i] || slot.className != className) slot.usable = false;
        }
    }
    return expectations;
}

TypeChecker::InferredArguments TypeChecker::inferArguments(const std::vector<NodePtr>& arguments) {
    return inferArguments(arguments, {});
}

TypeChecker::InferredArguments TypeChecker::inferArguments(const std::vector<NodePtr>& arguments,
                                                          const std::vector<ArgumentExpectation>& expectations) {
    InferredArguments result;
    result.types.reserve(arguments.size());
    result.classNames.reserve(arguments.size());
    result.functionParamTypes.reserve(arguments.size());
    result.functionParamClassNames.reserve(arguments.size());
    result.functionReturnTypes.reserve(arguments.size());
    result.functionReturnClassNames.reserve(arguments.size());
    result.functionHasSignature.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const AstNode* argument = arguments[index].get();
        InferredType inferred;
        if (index < expectations.size() && expectations[index].usable &&
            argument->kind == NodeKind::CollectionLiteral) {
            // Only an *empty* literal is re-read against the parameter: an
            // empty one has nothing to contradict the expectation, while a
            // non-empty `{1, 2}` is also how a variadic argument list is
            // written (Text.format(pattern, {"a", "b"})) and must stay a list.
            const auto* literal = static_cast<const CollectionLiteral*>(argument);
            if (literal->entries.empty() && literal->elements.empty()) {
                inferred = inferExpected(argument, expectations[index].type, expectations[index].className,
                                         expectations[index].annotation);
            } else {
                inferred = inferExpr(argument);
            }
        } else {
            inferred = inferExpr(argument);
        }
        result.types.push_back(inferred.type);
        result.classNames.push_back(inferred.className);
        result.functionParamTypes.push_back(inferred.functionParamTypes);
        result.functionParamClassNames.push_back(inferred.functionParamClassNames);
        result.functionReturnTypes.push_back(inferred.functionReturnType);
        result.functionReturnClassNames.push_back(inferred.functionReturnClassName);
        result.functionHasSignature.push_back(inferred.functionHasSignature);
    }
    return result;
}


void TypeChecker::analyzeLambdaCaptures(LambdaExpr* node) {
    std::unordered_set<std::string> params;
    for (const auto& p : node->params) params.insert(p.name);
    const AstNode* body = node->hasExprBody ? static_cast<const AstNode*>(node->exprBody.get())
                                            : static_cast<const AstNode*>(node->blockBody.get());
    const auto refs = collectLambdaCaptureRefs(body, params);
    node->captureNames.assign(refs.names.begin(), refs.names.end());
    std::sort(node->captureNames.begin(), node->captureNames.end());
    node->captureStorageNames.clear();
    for (const auto& name : node->captureNames) {
        if (name == "this") node->captureStorageNames.push_back(name);
        else if (const auto variable = symbols_.lookupVar(name)) node->captureStorageNames.push_back(variable->storageName);
    }
    node->usesThis = refs.usesThis;
}
void TypeChecker::validateThreadLambda(const LambdaExpr* node, const char* apiName) {
    if (!node) return;
    if (node->usesThis) typeError(std::string(apiName)+" cannot capture 'this'; wrap the object in Shared<T>", node->line);
    // Same policy and class list as the func-value spawn/start paths in
    // inferCall, via the one shared predicate - do not re-inline it here.
    for (const auto& name : node->captureNames) {
        if (capturedValueCrossesThreadBoundary(symbols_, name))
            typeError(std::string(apiName)+" cannot capture '"+name+"' across a thread boundary; use Shared<T>, Atomic, or Mutex for mutable shared state", node->line);
    }
}
// --- backend seam (MIR lowering) -------------------------------------------

const TypeChecker::InferredType* TypeChecker::expressionType(const AstNode* node) const {
    if (!node) return nullptr;
    const auto it = expressionTypes_.find(node);
    return it == expressionTypes_.end() ? nullptr : &it->second;
}

const std::vector<ResolvedTypeArg>& TypeChecker::unionMembers(const std::string& unionName) const {
    return typeResolver_.unionMembers(unionName);
}

TypeChecker::InferredType TypeChecker::inferExpr(const AstNode* node) {
    // Single funnel for every typed expression. Recording here (rather than in
    // each inferX helper) means the backend seam sees exactly the types the
    // checker itself used, including for nodes reached only through
    // inferExpected's collection/lambda special cases, which all route back
    // through this function.
    const InferredType result = inferExprInner(node);
    expressionTypes_[node] = result;
    return result;
}

TypeChecker::InferredType TypeChecker::inferExprInner(const AstNode* node) {
    switch (node->kind) {
        case NodeKind::Literal:           return inferLiteral(static_cast<const Literal*>(node));
        case NodeKind::Identifier:        return inferIdentifier(static_cast<const Identifier*>(node));
        case NodeKind::UnaryExpr:         return inferUnary(static_cast<const UnaryExpr*>(node));
        case NodeKind::AwaitExpr:          return inferAwait(static_cast<const AwaitExpr*>(node));
        case NodeKind::BinaryExpr:        return inferBinary(static_cast<const BinaryExpr*>(node));
        case NodeKind::CallExpr:          return inferCall(static_cast<const CallExpr*>(node));
        case NodeKind::AssignExpr:        return inferAssign(static_cast<const AssignExpr*>(node));
        case NodeKind::MoveExpr:          return inferMove(static_cast<const MoveExpr*>(node));
        case NodeKind::CollectionLiteral: return inferCollectionLiteral(static_cast<const CollectionLiteral*>(node));
        case NodeKind::NewExpr:           return inferNewExpr(static_cast<const NewExpr*>(node));
        case NodeKind::DataLiteralExpr:   return inferDataLiteral(static_cast<const DataLiteralExpr*>(node));
        case NodeKind::DataUpdateExpr:     return inferDataUpdate(static_cast<const DataUpdateExpr*>(node));
        case NodeKind::FieldAccessExpr:   return inferFieldAccess(static_cast<const FieldAccessExpr*>(node));
        case NodeKind::IndexAccessExpr:   return inferIndexAccess(static_cast<const IndexAccessExpr*>(node));
        case NodeKind::FieldAssignExpr:   return inferFieldAssign(static_cast<const FieldAssignExpr*>(node));
        case NodeKind::MethodCallExpr:    return inferMethodCall(static_cast<const MethodCallExpr*>(node));
        case NodeKind::ThisExpr:          return inferThisExpr(static_cast<const ThisExpr*>(node));
        case NodeKind::SuperCallExpr:       return inferSuperCallExpr(static_cast<const SuperCallExpr*>(node));
        case NodeKind::SuperMethodCallExpr: return inferSuperMethodCallExpr(static_cast<const SuperMethodCallExpr*>(node));
        case NodeKind::LambdaExpr:        return inferLambdaExpr(static_cast<const LambdaExpr*>(node));
        case NodeKind::MatchExpr:         return inferMatchExpr(const_cast<MatchExpr*>(static_cast<const MatchExpr*>(node)));
        case NodeKind::LogExpr: {
            const auto* n = static_cast<const LogExpr*>(node);
            if (n->argument) (void)inferExpr(n->argument.get());
            return InferredType(ZlType::VOID_TYPE);
        }
        default:
            return ZlType::UNKNOWN;
    }
}

namespace {
// Decode one rendered generic argument name (e.g. "int", "Widget",
// "List<int>") into the (kind, className) used to check a match pattern's
// children. A generic class keeps its full instantiation as its class name.
struct DecodedType { ZlType type; std::string className; };
DecodedType decodeRenderedType(std::string name) {
    const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\n' && c != '\r'; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    const auto [base, args] = splitGenericName(name);
    const ZlType type = zlTypeFromBaseName(base);
    return {type, (type == ZlType::OBJECT || !args.empty()) ? name : std::string{}};
}
} // namespace

TypeChecker::InferredType TypeChecker::inferMatchExpr(MatchExpr* node) {
    const InferredType subject = inferExpr(node->subject.get());
    if (node->arms.empty()) typeError("match requires at least one arm", node->line);

    InferredType result;
    bool haveResult = false;
    bool hasWildcard = false;
    bool matchedTrue = false, matchedFalse = false;
    const bool subjectIsUnion = subject.type == ZlType::UNION;
    std::function<bool(ZlType, const std::string&, ZlType, const std::string&)> overlaps;
    overlaps = [&](ZlType actual, const std::string& actualClass, ZlType pattern, const std::string& patternClass) {
        if (actual == ZlType::UNION) {
            const auto& members = typeResolver_.unionMembers(actualClass);
            return std::any_of(members.begin(), members.end(), [&](const auto& member) {
                return overlaps(member.type, member.className, pattern, patternClass);
            });
        }
        if (pattern == ZlType::UNION) return overlaps(pattern, patternClass, actual, actualClass);
        return isAssignable(actual, pattern, actualClass, patternClass) || isAssignable(pattern, actual, patternClass, actualClass);
    };
    std::vector<ResolvedTypeArg> remaining = subjectIsUnion
        ? typeResolver_.unionMembers(subject.className) : std::vector<ResolvedTypeArg>{{subject.type, subject.className}};
    // A built-in sum subject is closed, so completeness is decided over its
    // cases (`Some`/`None`, `Ok`/`Err`) rather than over the parent class -
    // which no single case pattern can ever cover, and which also carries the
    // implicit nil alternative. Every other hierarchy stays open: its parent
    // member remains, so a subclass arm alone never makes a match exhaustive.
    std::vector<ResolvedTypeArg> builtinCases = subjectIsUnion ? std::vector<ResolvedTypeArg>{}
                                                              : builtinSumCases(subject.className);
    const bool subjectIsBuiltinSum = !builtinCases.empty();
    if (subjectIsBuiltinSum) remaining = std::move(builtinCases);
    {
        // Reference types are nullable in ZL. A string/object type pattern
        // excludes nil, so its implicit nil alternative still needs coverage -
        // except on a built-in sum, where the cases are every value the type
        // has: a nil subject matches no case, and the lowering's unmatched path
        // raises the catchable "non-exhaustive match" instead of guessing.
        if (!subjectIsBuiltinSum &&
            std::any_of(remaining.begin(), remaining.end(), [&](const auto& member) {
                return isAssignable(ZlType::NIL, member.type, "", member.className);
            }) && std::none_of(remaining.begin(), remaining.end(), [](const auto& member) { return member.type == ZlType::NIL; }))
            remaining.push_back({ZlType::NIL, ""});
    }
    const auto entryTypes = symbols_.snapshot();
    auto fallthroughTypes = entryTypes;
    std::vector<SymbolTable::ScopeState> typeExits;
    const auto* subjectId = node->subject->kind == NodeKind::Identifier
        ? static_cast<const Identifier*>(node->subject.get()) : nullptr;
    bool stableSubject = subjectId != nullptr;
    std::unordered_map<std::string, std::unordered_set<std::string>> coveredEnums;
    std::unordered_set<std::string> unconditionalPatterns;
    std::vector<std::pair<std::string, std::string>> priorTypePatterns;
    bool unconditionalPatternSeen = false;

    const auto concretePatternKey = [](const MatchExpr::Pattern& child) -> std::string {
        if (child.kind == MatchExpr::PatternKind::EnumMember)
            return "enum:" + child.enumTypeName + "." + child.enumMemberName;
        if (child.kind != MatchExpr::PatternKind::Literal) return {};
        switch (child.literalType) {
            case TokenType::INT_LITERAL:
                try { return "num:" + std::to_string(std::stoll(child.raw)); } catch (...) { return "lit:" + child.raw; }
            case TokenType::DECIMAL_LITERAL:
            case TokenType::FLOAT_LITERAL:
                try {
                    std::ostringstream oss;
                    oss << std::setprecision(17) << std::stod(child.raw);
                    return "num:" + oss.str();
                } catch (...) { return "lit:" + child.raw; }
            case TokenType::STRING_LITERAL: return "str:" + child.raw;
            case TokenType::BOOL_LITERAL: return std::string("bool:") + (child.raw == "true" ? "1" : "0");
            case TokenType::KW_NULL: return "null";
            default: return "lit:" + child.raw;
        }
    };

    std::function<bool(const MatchExpr::Pattern&, ZlType, const std::string&)> checkPattern =
        [&](const MatchExpr::Pattern& pattern, ZlType subjectType, const std::string& subjectClass) -> bool {
            switch (pattern.kind) {
                case MatchExpr::PatternKind::Literal: {
                    Literal literal;
                    literal.literalType = pattern.literalType;
                    literal.raw = pattern.raw;
                    const InferredType pt = inferLiteral(&literal);
                    const bool compatible = subjectType == ZlType::UNION
                        ? overlaps(subjectType, subjectClass, pt.type, pt.className)
                        : ((subjectType == ZlType::UNKNOWN) ||
                           isAssignable(pt.type, subjectType, pt.className, subjectClass) ||
                           isAssignable(subjectType, pt.type, subjectClass, pt.className));
                    if (!compatible)
                        typeError("match literal pattern type does not match subject type", pattern.line);
                    if (pattern.literalType == TokenType::BOOL_LITERAL && !pattern.raw.empty()) {
                        if (pattern.raw == "true") matchedTrue = true; else if (pattern.raw == "false") matchedFalse = true;
                    }
                    return false;
                }
                case MatchExpr::PatternKind::EnumMember: {
                    const auto* enumShape = semanticModel_.findClass(pattern.enumTypeName);
                    if (!enumShape || !enumShape->isEnumType) typeError("unknown enum type in match pattern: '" + pattern.enumTypeName + "'", pattern.line);
                    if (std::find(enumShape->enumMembers.begin(), enumShape->enumMembers.end(), pattern.enumMemberName) == enumShape->enumMembers.end())
                        typeError("unknown enum member '" + pattern.enumTypeName + "." + pattern.enumMemberName + "'", pattern.line);
                    const bool enumCompatible = subjectType == ZlType::UNION
                        ? overlaps(subjectType, subjectClass, ZlType::OBJECT, pattern.enumTypeName)
                        : (subjectType == ZlType::OBJECT && subjectClass == pattern.enumTypeName);
                    if (!enumCompatible)
                        typeError("enum match pattern '" + pattern.enumTypeName + "." + pattern.enumMemberName + "' does not match subject type", pattern.line);

                    return false;
                }
                case MatchExpr::PatternKind::Wildcard:
                    return true;
                case MatchExpr::PatternKind::Variable:
                    if (!pattern.bindingName.empty() && pattern.bindingName != "_")
                        symbols_.defineVar(pattern.bindingName, subjectType, false, subjectClass);
                    return true;
                case MatchExpr::PatternKind::Type: {
                    std::string patternClass;
                    const ZlType patternType = resolveType(pattern.typePattern, &patternClass);
                    const bool typeCompatible = subjectType == ZlType::UNION
                        ? overlaps(subjectType, subjectClass, patternType, patternClass)
                        : ((subjectType == ZlType::UNKNOWN) ||
                           isAssignable(patternType, subjectType, patternClass, subjectClass) ||
                           isAssignable(subjectType, patternType, subjectClass, patternClass));
                    if (!typeCompatible)
                        typeError("match type pattern does not match subject type", pattern.line);
                    if (!pattern.bindingName.empty() && pattern.bindingName != "_")
                        symbols_.defineVar(pattern.bindingName, variableInfo(pattern.typePattern));
                    // Flow-sensitive narrowing: within this arm the original subject identifier
                    // is refined to the matched type. The arm scope is discarded afterward.

                    return false;
                }
                case MatchExpr::PatternKind::List: {
                    const bool isSetPattern = pattern.containerKind == "set" || pattern.containerKind == "Set";
                    const bool isGenericObject = subjectType == ZlType::OBJECT;
                    const std::string baseName = [&]() {
                        if (!isGenericObject) return std::string();
                        const auto lt = subjectClass.find('<');
                        return lt == std::string::npos ? subjectClass : subjectClass.substr(0, lt);
                    }();
                    const bool subjectMatches = isSetPattern
                        ? (subjectType == ZlType::SET || (isGenericObject && baseName == "Set"))
                        : (subjectType == ZlType::LIST || (isGenericObject && baseName == "List"));
                    if (!subjectMatches)
                        typeError(isSetPattern ? "set pattern does not match subject type" : "list pattern does not match subject type", pattern.line);

                    ZlType elementType = ZlType::UNKNOWN;
                    std::string elementClass;
                    // A list/set subject carries its element type as the first
                    // (and only) generic argument in its rendered class name.
                    {
                        const auto [subBase, subArgs] = splitGenericName(subjectClass);
                        if (!subArgs.empty() && (subBase == "List" || subBase == "Set" ||
                                                 subBase == "list" || subBase == "set")) {
                            const auto arg = splitGenericArgs(subArgs).front();
                            const auto decoded = decodeRenderedType(arg);
                            elementType = decoded.type;
                            elementClass = decoded.className;
                        }
                    }
                    std::unordered_set<std::string> seenSetElements;
                    for (const auto& child : pattern.elements) {
                        if (!child) continue;
                        if (isSetPattern && child->kind != MatchExpr::PatternKind::Literal &&
                            child->kind != MatchExpr::PatternKind::EnumMember &&
                            child->kind != MatchExpr::PatternKind::Wildcard) {
                            typeError("set match elements currently require literal, enum-member, or wildcard patterns", child->line);
                        }
                        if (isSetPattern && child->kind != MatchExpr::PatternKind::Wildcard) {
                            const std::string key = concretePatternKey(*child);
                            if (!key.empty() && !seenSetElements.insert(key).second)
                                typeError("duplicate concrete element in set match pattern", child->line);
                        }
                        checkPattern(*child, elementType, elementClass);
                    }
                    return false;
                }
                case MatchExpr::PatternKind::Map: {
                    if (subjectType != ZlType::MAP)
                        typeError("map pattern does not match subject type", pattern.line);

                    ZlType keyType = ZlType::UNKNOWN, valueType = ZlType::UNKNOWN;
                    std::string keyClass, valueClass;
                    // A map subject preserves its K,V arguments in className
                    // (e.g. map<string,int>); split on the top-level comma so a
                    // nested generic argument stays intact.
                    {
                        const auto [subBase, subArgs] = splitGenericName(subjectClass);
                        if (!subArgs.empty() && (subBase == "map" || subBase == "Map")) {
                            const auto kv = splitGenericArgs(subArgs);
                            if (kv.size() == 2) {
                                const auto k = decodeRenderedType(kv[0]);
                                const auto v = decodeRenderedType(kv[1]);
                                keyType = k.type; keyClass = k.className;
                                valueType = v.type; valueClass = v.className;
                            }
                        }
                    }

                    std::unordered_set<std::string> seenMapKeys;
                    for (const auto& entry : pattern.mapEntries) {
                        if (!entry.key || (entry.key->kind != MatchExpr::PatternKind::Literal &&
                                           entry.key->kind != MatchExpr::PatternKind::EnumMember)) {
                            typeError("map match keys currently require literal or enum-member patterns", entry.line);
                        }
                        if (entry.key) {
                            const std::string key = concretePatternKey(*entry.key);
                            if (!key.empty() && !seenMapKeys.insert(key).second)
                                typeError("duplicate concrete key in map match pattern", entry.line);
                            checkPattern(*entry.key, keyType, keyClass);
                        }
                        if (entry.value) checkPattern(*entry.value, valueType, valueClass);
                    }
                    return false;
                }
                case MatchExpr::PatternKind::Data: {
                    const auto* shape = semanticModel_.findClass(pattern.typePattern.name);
                    if (!shape || !shape->isDataType)
                        typeError("unknown data type in match pattern: '" + pattern.typePattern.name + "'", pattern.line);
                    const bool dataCompatible = subjectType == ZlType::UNION
                        ? overlaps(subjectType, subjectClass, ZlType::OBJECT, pattern.typePattern.name)
                        : (subjectType == ZlType::OBJECT && subjectClass == pattern.typePattern.name);
                    if (!dataCompatible)
                        typeError("data match pattern does not match subject type", pattern.line);
                    bool irrefutable = true;
                    std::size_t positionalIndex = 0;
                    for (auto& fieldPattern : pattern.fields) {
                        std::string fieldName = fieldPattern.fieldName;
                        if (pattern.positional) {
                            if (positionalIndex >= shape->fields.size())
                                typeError("too many positional fields in data match pattern '" + pattern.typePattern.name + "'", fieldPattern.line);
                            if (positionalIndex >= shape->fieldOrder.size())
                                typeError("too many positional fields in data match pattern '" + pattern.typePattern.name + "'", fieldPattern.line);
                            fieldName = shape->fieldOrder[positionalIndex++];
                        }
                        const auto* field = semanticModel_.findFieldInHierarchy(pattern.typePattern.name, fieldName);
                        if (!field) typeError("unknown data field '" + pattern.typePattern.name + "." + fieldName + "'", fieldPattern.line);
                        if (!checkPattern(*fieldPattern.pattern, field->type, field->className)) irrefutable = false;
                    }
                    return irrefutable;
                }
            }
            return false;
        };

    std::function<std::unique_ptr<MatchExpr::Pattern>(const MatchExpr::Pattern&)> cloneMatchPattern =
        [&](const MatchExpr::Pattern& src) {
            auto out = std::make_unique<MatchExpr::Pattern>();
            out->kind = src.kind;
            out->raw = src.raw;
            out->literalType = src.literalType;
            out->enumTypeName = src.enumTypeName;
            out->enumMemberName = src.enumMemberName;
            out->typePattern = src.typePattern;
            out->bindingName = src.bindingName;
            out->containerKind = src.containerKind;
            out->positional = src.positional;
            out->line = src.line;
            for (const auto& child : src.elements)
                out->elements.push_back(child ? cloneMatchPattern(*child) : nullptr);
            for (const auto& entry : src.mapEntries) {
                MatchExpr::MapEntryPattern copied;
                copied.line = entry.line;
                copied.key = entry.key ? cloneMatchPattern(*entry.key) : nullptr;
                copied.value = entry.value ? cloneMatchPattern(*entry.value) : nullptr;
                out->mapEntries.push_back(std::move(copied));
            }
            for (const auto& field : src.fields) {
                MatchExpr::DataFieldPattern copied;
                copied.fieldName = field.fieldName;
                copied.line = field.line;
                copied.pattern = field.pattern ? cloneMatchPattern(*field.pattern) : nullptr;
                out->fields.push_back(std::move(copied));
            }
            return out;
        };

    for (auto& arm : node->arms) {
        symbols_.restore(fallthroughTypes);
        if (remaining.empty()) {
            // A built-in sum is exhausted by its cases, but nil is still a
            // value of the subject's type - it matches no case - so a null
            // literal, or a catch-all that also covers nil, is a reachable arm
            // even with no case left. Anything else here really is unreachable.
            const bool matchesNil = arm.patternKind == MatchExpr::PatternKind::Wildcard ||
                                    arm.patternKind == MatchExpr::PatternKind::Variable ||
                                    (arm.patternKind == MatchExpr::PatternKind::Literal &&
                                     arm.literalType == TokenType::KW_NULL);
            if (!(subjectIsBuiltinSum && matchesNil))
                typeError("unreachable match arm: subject is already covered", arm.line);
        }
        if (unconditionalPatternSeen)
            typeError("unreachable match arm: a previous irrefutable pattern matches every value", arm.line);
        const bool armUnconditional = (arm.guard == nullptr);
        std::string patternKey;
        switch (arm.patternKind) {
            case MatchExpr::PatternKind::Wildcard: patternKey = "wildcard"; break;
            case MatchExpr::PatternKind::Literal: {
                MatchExpr::Pattern literalPattern;
                literalPattern.kind = MatchExpr::PatternKind::Literal;
                literalPattern.literalType = arm.literalType;
                literalPattern.raw = arm.raw;
                patternKey = concretePatternKey(literalPattern);
                break;
            }
            case MatchExpr::PatternKind::EnumMember: patternKey = "enum:" + arm.enumTypeName + "." + arm.enumMemberName; break;
            case MatchExpr::PatternKind::Type: patternKey = "type:" + describeTypeAnnotation(arm.typePattern); break;
            default: patternKey.clear(); break;
        }
        if (armUnconditional && !patternKey.empty() && unconditionalPatterns.count(patternKey))
            typeError("unreachable duplicate match arm", arm.line);
        if (armUnconditional && (arm.patternKind == MatchExpr::PatternKind::Type) && arm.typePattern.typeArgs.empty() && !arm.typePattern.name.empty()) {
            for (const auto& prior : priorTypePatterns) {
                const bool laterIsSubtype = isAssignable(ZlType::OBJECT, ZlType::OBJECT, arm.typePattern.name, prior.second);
                if (laterIsSubtype && arm.typePattern.name != prior.second) {
                    typeError("unreachable match arm: previous type pattern '" + prior.first + "' matches subtype '" + arm.typePattern.name + "'", arm.line);
                }
            }
        }
        if (armUnconditional && arm.patternKind == MatchExpr::PatternKind::Wildcard) unconditionalPatternSeen = true;
        symbols_.pushScope();
        // Positional data patterns (`Pair(v, w)`) carry empty field names in
        // the AST; the checker resolves them by declaration order here and
        // writes the names back so the bytecode compiler and the MIR lowerer
        // both see concrete fields instead of re-deriving the order.
        if (arm.patternKind == MatchExpr::PatternKind::Data && arm.positional) {
            if (const auto* shape = semanticModel_.findClass(arm.typePattern.name)) {
                std::size_t positionalIndex = 0;
                for (auto& field : arm.dataFields) {
                    if (positionalIndex < shape->fieldOrder.size())
                        field.fieldName = shape->fieldOrder[positionalIndex++];
                }
            }
        }
        auto armSubject = ResolvedTypeArg{subject.type, subject.className};
        const bool catchAll = arm.patternKind == MatchExpr::PatternKind::Wildcard ||
                              arm.patternKind == MatchExpr::PatternKind::Variable;
        if (subjectIsUnion && catchAll && !remaining.empty())
            armSubject = remaining.size() == 1 ? remaining.front() : typeResolver_.makeUnion(remaining);
        // In a pattern, the name of a case of a built-in sum means that case:
        // `None =>` and `Err =>` test it rather than binding a variable named
        // `None`. Only the two closed sums read this way, and only when the
        // subject is the sum itself, so an ordinary hierarchy keeps binding a
        // variable of that name.
        if (arm.patternKind == MatchExpr::PatternKind::Variable && !arm.bindingName.empty()) {
            for (const auto& caseType : builtinSumCases(armSubject.className)) {
                if (parseTypeName(caseType.className).name != arm.bindingName) continue;
                arm.patternKind = MatchExpr::PatternKind::Type;
                arm.typePattern = TypeAnnotation{};
                arm.typePattern.name = arm.bindingName;
                arm.typePattern.line = arm.line;
                arm.bindingName = "_";
                break;
            }
        }
        // A type pattern may name a generic class without repeating the
        // arguments the subject already fixes: against `Result<int,string>`,
        // `Ok v` means `Ok<int,string> v`. The checker writes the concrete
        // annotation back here - the same write-back the positional data
        // patterns get above - so the binding, the bytecode compiler and the
        // MIR lowerer all see one spelled-out type.
        if (arm.patternKind == MatchExpr::PatternKind::Type)
            fillTypePatternArgs(arm.typePattern, armSubject.className);
        MatchExpr::Pattern p;
        p.kind = arm.patternKind; p.raw = arm.raw; p.literalType = arm.literalType;
        p.enumTypeName = arm.enumTypeName; p.enumMemberName = arm.enumMemberName;
        p.typePattern = arm.typePattern; p.bindingName = arm.bindingName;
        p.positional = arm.positional;
        for (const auto& field : arm.dataFields) {
            MatchExpr::DataFieldPattern copied;
            copied.fieldName = field.fieldName;
            copied.line = field.line;
            copied.pattern = field.pattern ? cloneMatchPattern(*field.pattern) : nullptr;
            p.fields.push_back(std::move(copied));
        }
        for (const auto& child : arm.listElements)
            p.elements.push_back(child ? cloneMatchPattern(*child) : nullptr);
        for (const auto& entry : arm.mapEntries) {
            MatchExpr::MapEntryPattern copied;
            copied.line = entry.line;
            copied.key = entry.key ? cloneMatchPattern(*entry.key) : nullptr;
            copied.value = entry.value ? cloneMatchPattern(*entry.value) : nullptr;
            p.mapEntries.push_back(std::move(copied));
        }
        p.containerKind = arm.containerKind; p.line = arm.line;
        const bool irrefutableShape = checkPattern(p, armSubject.type, armSubject.className);
        if (stableSubject) {
            if (arm.patternKind == MatchExpr::PatternKind::Type || arm.patternKind == MatchExpr::PatternKind::Data ||
                arm.patternKind == MatchExpr::PatternKind::EnumMember) {
                TypeAnnotation annotation = arm.typePattern;
                if (arm.patternKind == MatchExpr::PatternKind::EnumMember) annotation.name = arm.enumTypeName;
                symbols_.refine(subjectId->name, variableInfo(annotation));
            } else if (subjectIsUnion && catchAll) {
                SymbolTable::VarInfo view;
                view.type = armSubject.type;
                view.className = armSubject.className;
                symbols_.refine(subjectId->name, std::move(view));
            }
        }
        arm.storageBindings.clear();
        for (const auto& binding : symbols_.currentScope()) arm.storageBindings[binding.first] = binding.second.storageName;
        // Only a root wildcard or bare variable is an unconditional catch-all
        // for purposes of arm reachability/exhaustiveness. Structural patterns
        // may contain only irrefutable children (for example Point(x, y)) but
        // still match only that concrete outer shape and must not shadow later
        // arms or satisfy an arbitrary union's fallback requirement.
        const bool rootCatchAll = armUnconditional &&
            (arm.patternKind == MatchExpr::PatternKind::Wildcard ||
             arm.patternKind == MatchExpr::PatternKind::Variable);
        if (rootCatchAll) hasWildcard = true;
        if (armUnconditional && !patternKey.empty()) unconditionalPatterns.insert(patternKey);
        if (armUnconditional && arm.patternKind == MatchExpr::PatternKind::Type && arm.typePattern.typeArgs.empty() && !arm.typePattern.name.empty()) {
            priorTypePatterns.emplace_back(arm.typePattern.name, arm.typePattern.name);
        }
        if (rootCatchAll) unconditionalPatternSeen = true;
        if (armUnconditional) {
            if (arm.patternKind == MatchExpr::PatternKind::Literal && arm.literalType == TokenType::BOOL_LITERAL) {
                if (arm.raw == "true") matchedTrue = true; else matchedFalse = true;
            }
            if (arm.patternKind == MatchExpr::PatternKind::EnumMember)
                coveredEnums[arm.enumTypeName].insert(arm.enumMemberName);
            remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](const auto& member) {
                if (catchAll) return true;
                if (arm.patternKind == MatchExpr::PatternKind::Type) {
                    std::string cls;
                    auto type = resolveType(arm.typePattern, &cls);
                    if (member.type == ZlType::NIL) return type == ZlType::NIL;
                    if (member.type == ZlType::UNKNOWN) return arm.typePattern.name == "unknown";
                    return typePatternCovers(member, type, cls);
                }
                if (arm.patternKind == MatchExpr::PatternKind::Data && irrefutableShape) {
                    // A data pattern on a built-in sum case (`Some{value: v}`)
                    // names the case directly, and the expanded member is the
                    // instantiated case (`Some<int>`), so the base name is what
                    // identifies it.
                    if (member.type == ZlType::OBJECT && !member.className.empty() &&
                        parseTypeName(member.className).name == arm.typePattern.name)
                        return true;
                    return member.type == ZlType::OBJECT &&
                           isAssignable(member.type, ZlType::OBJECT, member.className, arm.typePattern.name);
                }
                if (arm.patternKind == MatchExpr::PatternKind::Literal) {
                    if (member.type == ZlType::NIL && arm.literalType == TokenType::KW_NULL) return true;
                    return member.type == ZlType::BOOL && matchedTrue && matchedFalse;
                }
                if (arm.patternKind == MatchExpr::PatternKind::EnumMember && member.type == ZlType::OBJECT) {
                    const auto* shape = semanticModel_.findClass(member.className);
                    return shape && shape->isEnumType && std::all_of(shape->enumMembers.begin(), shape->enumMembers.end(), [&](const auto& name) {
                        return coveredEnums[member.className].count(name) != 0;
                    });
                }
                return false;
            }), remaining.end());
        }
        bool guardWritesSubject = false;
        if (subjectId && arm.guard) {
            for (const auto& name : collectLambdaCaptureRefs(arm.guard.get(), {}).assignedNames) {
                const auto binding = symbols_.lookupVar(name);
                guardWritesSubject = guardWritesSubject || (binding && binding->storageName == subjectId->storageName);
            }
        }
        if (arm.guard) {
            const InferredType guardType = inferExpr(arm.guard.get());
            if (guardType.type != ZlType::BOOL && guardType.type != ZlType::UNKNOWN)
                typeError("match guard must produce bool", arm.line);
        }
        const auto afterGuard = symbols_.snapshot();
        const InferredType armResult = inferExpr(arm.result.get());
        if (!haveResult) { result = armResult; haveResult = true; }
        else if (armResult.type != result.type || armResult.className != result.className) {
            if (!isAssignable(armResult.type, result.type, armResult.className, result.className) ||
                !isAssignable(result.type, armResult.type, result.className, armResult.className))
                typeError("all match arms must produce the same type", arm.line);
        }
        symbols_.popScope();
        typeExits.push_back(symbols_.snapshot());
        symbols_.restore(afterGuard);
        symbols_.popScope();
        symbols_.joinRefinements(fallthroughTypes, {fallthroughTypes, symbols_.snapshot()});
        fallthroughTypes = symbols_.snapshot();
        stableSubject = stableSubject && !guardWritesSubject;
    }
    symbols_.joinRefinements(entryTypes, typeExits);

    if (!hasWildcard && !remaining.empty()) {
        std::string uncovered;
        for (const auto& member : remaining) {
            if (!uncovered.empty()) uncovered += "|";
            uncovered += member.className.empty() ? zlTypeName(member.type) : member.className;
        }
        typeError("non-exhaustive match: uncovered " + uncovered + "; add a covering pattern or wildcard", node->line);
    }
    return result;
}

TypeChecker::InferredType TypeChecker::inferLiteral(const Literal* node) {
    switch (node->literalType) {
        case TokenType::INT_LITERAL:
            try {
                std::size_t pos = 0;
                (void)std::stoll(node->raw, &pos);
                if (pos != node->raw.size()) {
                    typeError("invalid integer literal '" + node->raw + "'", node->line);
                }
            } catch (...) {
                typeError("integer literal is out of range '" + node->raw + "'", node->line);
            }
            return ZlType::INT;
        case TokenType::DECIMAL_LITERAL:
        case TokenType::FLOAT_LITERAL:
            try {
                std::size_t pos = 0;
                (void)std::stod(node->raw, &pos);
                if (pos != node->raw.size()) {
                    typeError("invalid floating-point literal '" + node->raw + "'", node->line);
                }
            } catch (...) {
                typeError("floating-point literal is invalid or out of range '" + node->raw + "'", node->line);
            }
            return ZlType::DOUBLE;
        case TokenType::STRING_LITERAL: return ZlType::STRING;
        case TokenType::BOOL_LITERAL:    return ZlType::BOOL;
        case TokenType::KW_NULL:          return ZlType::NIL;
        default:                         return ZlType::UNKNOWN;
    }
}

TypeChecker::InferredType TypeChecker::inferIdentifier(const Identifier* node) {
    auto info = symbols_.lookupVar(node->name);
    if (info) {
        node->storageName = info->storageName;
        if (movedVariables_.count(node->name)) {
            typeError("use of moved variable '" + node->name + "'", node->line);
        }
        auto borrowIt = borrowSources_.find(node->name);
        if (borrowIt != borrowSources_.end() && movedVariables_.count(borrowIt->second)) {
            typeError("borrow '" + node->name + "' refers to moved variable '" + borrowIt->second + "'", node->line);
        }
        InferredType result(info->type, info->className);
        result.ownership = info->ownership;
        if (info->ownership == OwnershipKind::BORROW) {
            auto it = borrowSources_.find(node->name);
            if (it != borrowSources_.end()) result.borrowSource = it->second;
        }
        result.functionParamTypes = info->functionParamTypes;
        result.functionParamClassNames = info->functionParamClassNames;
        result.functionReturnType = info->functionReturnType;
        result.functionReturnClassName = info->functionReturnClassName;
        result.functionIsAsync = info->functionIsAsync;
        result.functionHasSignature = info->functionHasSignature;
        result.taskValueType = info->taskValueType;
        result.taskValueClassName = info->taskValueClassName;
        auto namedIt = namedFunctionBindings_.find(node->name);
        if (namedIt != namedFunctionBindings_.end()) {
            result.functionIsNamedReference = true;
            result.functionReferenceOwner = namedIt->second.ownerClassName;
            result.functionReferenceDispatch = namedIt->second.dispatch;
        }
        return result;
    }
    // Every variable in this language is scoped to a single class/func
    // (params, locals, for/catch vars) and always defined in the same or an
    // enclosing scope before use - there's no forward-declaration, hoisting,
    // or module-global variable that would legitimately be missing from the
    // symbol table here. An unresolved name is a genuine typo/undefined
    // reference, so report it now rather than deferring to a runtime crash.
    typeError("undefined variable '" + node->name + "'", node->line);
    return ZlType::UNKNOWN; // unreachable - typeError throws
}

TypeChecker::InferredType TypeChecker::inferAwait(const AwaitExpr* node) {
    if (!currentFunctionIsAsync_) {
        typeError("await can only be used inside an async func", node->line);
    }
    if (heldLockDepth_ > 0) {
        typeError("cannot await while a Mutex/RwLock capability is held; release the lock before suspension", node->line);
    }
    if (!borrowSources_.empty()) {
        std::string unsafeBorrows;
        for (const auto& [borrowName, initialSource] : borrowSources_) {
            // A borrow may safely cross an await only when its lifetime is
            // anchored to an owned value whose storage is itself part of the
            // suspended async frame. Borrowing from another borrow (or from a
            // caller-owned borrow parameter) still lacks a region proof.
            bool resolvesToOwned = false;
            if (initialSource != "<borrow-parameter>") {
                const auto dot = initialSource.find('.');
                const auto bracket = initialSource.find('[');
                const auto cut = std::min(dot == std::string::npos ? initialSource.size() : dot,
                                         bracket == std::string::npos ? initialSource.size() : bracket);
                const std::string root = initialSource.substr(0, cut);
                resolvesToOwned = borrowRootOwner(root).has_value() && !movedVariables_.count(root);
            }
            if (!resolvesToOwned) {
                if (!unsafeBorrows.empty()) unsafeBorrows += ", ";
                unsafeBorrows += borrowName;
            }
        }
        if (!unsafeBorrows.empty()) {
            typeError("cannot await while borrow(s) without an owned-frame lifetime are active: " + unsafeBorrows +
                      "; end the borrow or use an owned/shared value before suspension", node->line);
        }
    }
    const auto operand = inferExpr(node->operand.get());
    if (operand.type != ZlType::TASK) {
        typeError("await expects a Task value, got " + zlTypeName(operand.type), node->line);
    }
    InferredType result(operand.taskValueType, operand.taskValueClassName);
    return result;
}


// The front end's view of the shared operator table: its tokens are lexer
// concerns, and this mapping is the single point where they become the
// Operator names `zl::OperatorRules` is keyed by. Keeping the table itself
// free of lexer types is what lets a backend (the MIR verifier) consult it.
[[nodiscard]] zl::Operator operatorFromToken(TokenType token) {
    switch (token) {
        case TokenType::PLUS: return zl::Operator::Plus;
        case TokenType::MINUS: return zl::Operator::Minus;
        case TokenType::STAR: return zl::Operator::Multiply;
        case TokenType::SLASH: return zl::Operator::Divide;
        case TokenType::PERCENT: return zl::Operator::Modulo;
        case TokenType::POW: return zl::Operator::Power;
        case TokenType::BIT_AND: return zl::Operator::BitAnd;
        case TokenType::BIT_OR: return zl::Operator::BitOr;
        case TokenType::BIT_XOR: return zl::Operator::BitXor;
        case TokenType::SHL: return zl::Operator::ShiftLeft;
        case TokenType::SHR: return zl::Operator::ShiftRight;
        case TokenType::USHR: return zl::Operator::ShiftRightZero;
        case TokenType::EQ: return zl::Operator::Equal;
        case TokenType::NEQ: return zl::Operator::NotEqual;
        case TokenType::LT: return zl::Operator::Less;
        case TokenType::GT: return zl::Operator::Greater;
        case TokenType::LTE: return zl::Operator::LessEqual;
        case TokenType::GTE: return zl::Operator::GreaterEqual;
        case TokenType::NOT: return zl::Operator::Not;
        case TokenType::BIT_NOT: return zl::Operator::BitNot;
        case TokenType::AND: return zl::Operator::LogicalAnd;
        case TokenType::OR: return zl::Operator::LogicalOr;
        default: return zl::Operator::Unknown;
    }
}

TypeChecker::InferredType TypeChecker::inferUnary(const UnaryExpr* node) {
    const auto operand = inferExpr(node->operand.get());
    ZlType operandType = operand.type;
    std::string operandClass = operand.className;

    // Generic type parameters can participate in primitive numeric operators.
    // The operation is compiled to the normal VM opcode, while the concrete
    // instantiation is checked against the inferred numeric constraint.
    if (operandType == ZlType::OBJECT && isCurrentGenericTypeParam(operandClass) &&
        (node->op == TokenType::MINUS || node->op == TokenType::PLUS)) {
        requireNumericGenericTypeParam(operandClass, node->line);
        return InferredType(ZlType::OBJECT, operandClass);
    }

    // User-defined unary operators are dispatched on object receivers.
    if (operandType == ZlType::OBJECT && !operandClass.empty()) {
        const std::string methodName = OperatorRules::methodName(operatorFromToken(node->op));
        std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
        if (semanticModel_.hasInterface(operandClass)) {
            semanticModel_.resolveInterfaceMethods(operandClass);
            const auto& iface = *semanticModel_.findInterface(operandClass);
            auto it = iface.methods.find(methodName);
            if (it != iface.methods.end()) candidates.emplace_back(operandClass, it->second);
        } else {
            candidates = semanticModel_.collectMethodOverloads(operandClass, methodName);
        }
        if (!candidates.empty()) {
            std::vector<ZlType> noArgs;
            std::string owner;
            const ClassMethodInfo* method = resolveOverload(candidates, noArgs, {}, methodName, node->line, &owner);
            checkAccess(owner, methodName, method->access, /*isMethod=*/true, node->line);
            node->isOperatorOverload = true;
            node->resolvedOperatorDispatch = dispatchSignature(methodName, *method);
            return InferredType(method->returnType, method->returnClassName);
        }
        typeError("class '" + operandClass + "' has no unary operator '" + OperatorRules::methodName(operatorFromToken(node->op)) + "'", node->line);
    }

    if (const auto result = OperatorRules::unaryResult(operatorFromToken(node->op), operandType)) return *result;
    if (node->op == TokenType::NOT) return ZlType::BOOL;
    typeError(OperatorRules::unaryError(operatorFromToken(node->op)), node->line);

}

TypeChecker::InferredType TypeChecker::inferBinary(const BinaryExpr* node) {
    const auto leftResult = inferExpr(node->left.get());
    ZlType left = leftResult.type;
    std::string leftClass = leftResult.className;
    const auto rightResult = inferExpr(node->right.get());
    ZlType right = rightResult.type;
    std::string rightClass = rightResult.className;

    const bool leftGeneric = left == ZlType::OBJECT && isCurrentGenericTypeParam(leftClass);
    const bool rightGeneric = right == ZlType::OBJECT && isCurrentGenericTypeParam(rightClass);
    const bool numericOp = node->op == TokenType::PLUS || node->op == TokenType::MINUS ||
                           node->op == TokenType::STAR || node->op == TokenType::SLASH ||
                           node->op == TokenType::PERCENT || node->op == TokenType::POW;

    if ((left == ZlType::UNION || right == ZlType::UNION) &&
        node->op != TokenType::EQ && node->op != TokenType::NEQ &&
        node->op != TokenType::AND && node->op != TokenType::OR &&
        !(node->op == TokenType::PLUS && (left == ZlType::STRING || right == ZlType::STRING))) {
        typeError("operator requires narrowing the union operand with match", node->line);
    }

    // Untyped lambda parameters are intentionally UNKNOWN until ZL grows
    // full func-type inference. Keep those expressions polymorphic rather
    // than rejecting otherwise valid lambdas such as `func(x) => x + 1`.
    if (left == ZlType::UNKNOWN || right == ZlType::UNKNOWN) {
        if (node->op == TokenType::EQ || node->op == TokenType::NEQ ||
            node->op == TokenType::LT || node->op == TokenType::GT ||
            node->op == TokenType::LTE || node->op == TokenType::GTE) {
            return ZlType::BOOL;
        }
        return ZlType::UNKNOWN;
    }

    // Generic type parameters may carry interface constraints. Resolve user-defined
    // operators from those interface contracts while retaining the generic receiver
    // in the dispatch signature; concrete instantiations provide the implementation.
    if (leftGeneric && leftClass == leftResult.className && !currentClassName_.empty()) {
        const auto* current = semanticModel_.findClass(currentClassName_);
        if (current != nullptr) {
            auto constraintIt = current->typeParamInterfaceConstraints.find(leftClass);
            if (constraintIt != current->typeParamInterfaceConstraints.end()) {
                const std::string methodName = OperatorRules::methodName(operatorFromToken(node->op));
                std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
                for (const auto& ifaceName : constraintIt->second) {
                    semanticModel_.resolveInterfaceMethods(ifaceName);
                    const auto* iface = semanticModel_.findInterface(ifaceName);
                    if (iface == nullptr) continue;
                    auto methodIt = iface->methods.find(methodName);
                    if (methodIt != iface->methods.end()) candidates.emplace_back(ifaceName, methodIt->second);
                }
                if (!candidates.empty()) {
                    std::vector<ZlType> argTypes{right};
                    std::string owner;
                    const ClassMethodInfo* method = resolveOverload(candidates, argTypes, {rightClass}, methodName, node->line, &owner);
                    checkAccess(owner, methodName, method->access, /*isMethod=*/true, node->line);
                    node->isOperatorOverload = true;
                    node->resolvedOperatorDispatch = dispatchSignature(methodName, *method);
                    return InferredType(method->returnType, method->returnClassName);
                }
            }
        }
    }

    if ((leftGeneric || rightGeneric) && numericOp) {
        if (leftGeneric) requireNumericGenericTypeParam(leftClass, node->line);
        if (rightGeneric) requireNumericGenericTypeParam(rightClass, node->line);

        // Same generic parameter on both sides preserves the parameter's
        // concrete numeric type after instantiation. A generic component
        // combined with a concrete double/int is represented as double so
        // callers can safely use the result in floating-point math.
        if (leftGeneric && rightGeneric && leftClass == rightClass) {
            return InferredType(ZlType::OBJECT, leftClass);
        }
        if ((leftGeneric && (right == ZlType::INT || right == ZlType::DOUBLE)) ||
            (rightGeneric && (left == ZlType::INT || left == ZlType::DOUBLE))) {
            return ZlType::DOUBLE;
        }
        return ZlType::UNKNOWN;
    }

    // User-defined binary operators dispatch on the left-hand operand.
    if (left == ZlType::OBJECT && !leftClass.empty()) {
        const std::string methodName = OperatorRules::methodName(operatorFromToken(node->op));
        std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
        if (semanticModel_.hasInterface(leftClass)) {
            semanticModel_.resolveInterfaceMethods(leftClass);
            const auto& iface = *semanticModel_.findInterface(leftClass);
            auto it = iface.methods.find(methodName);
            if (it != iface.methods.end()) candidates.emplace_back(leftClass, it->second);
        } else {
            candidates = semanticModel_.collectMethodOverloads(leftClass, methodName);
        }
        if (!candidates.empty()) {
            std::vector<ZlType> argTypes{right};
            std::string owner;
            const ClassMethodInfo* method = resolveOverload(candidates, argTypes, {rightClass}, methodName, node->line, &owner);
            checkAccess(owner, methodName, method->access, /*isMethod=*/true, node->line);
            node->isOperatorOverload = true;
            node->resolvedOperatorDispatch = dispatchSignature(methodName, *method);
            return InferredType(method->returnType, method->returnClassName);
        }
        // Preserve the existing structural equality behavior for objects, but
        // all other operators on objects must be explicitly declared.
        if (node->op != TokenType::EQ && node->op != TokenType::NEQ) {
            typeError("class '" + leftClass + "' has no operator '" + OperatorRules::methodName(operatorFromToken(node->op)) + "'", node->line);
        }
    }

    if (const auto result = OperatorRules::binaryResult(operatorFromToken(node->op), left, right)) return *result;
    typeError(OperatorRules::binaryError(operatorFromToken(node->op)), node->line);

}

TypeChecker::InferredType TypeChecker::inferCall(const CallExpr* node) {
    // Class/type literal reflection: `Type.of(MyClass)`. The identifier is a
    // type name, not a variable reference, so it must be recognized before
    // ordinary argument inference. Keep the native `Type.of(value)` path
    // unchanged for runtime values.
    if (node->namespaceName == "Type" && node->calleeName == "of" &&
        node->arguments.size() == 1 && node->arguments[0]->kind == NodeKind::Identifier) {
        const auto* id = static_cast<const Identifier*>(node->arguments[0].get());
        if (semanticModel_.findClass(id->name) != nullptr) {
            node->isClassTypeLiteral = true;
            node->classTypeLiteralName = id->name;
            return InferredType(ZlType::OBJECT, "Type");
        }
    }
    if (node->namespaceName.empty() && node->calleeName == "share") {
        if (node->arguments.size() != 1) typeError("'share' expects one argument", node->line);
        const auto value = inferExpr(node->arguments[0].get());
        const std::string sharedClass =
            instantiateGenericClass("Shared",
                {ResolvedTypeArg{value.type, value.className}},
                node->line);
        node->nativeFactoryTypeName = sharedClass;
        return InferredType(ZlType::OBJECT, sharedClass);
    }

    auto validateThreadBoundaryCallable = [&](const InferredType& callable, const char* apiName) {
        if (callable.type != ZlType::FUNCTION) return;
        if (callable.functionIsAsync) {
            typeError(std::string(apiName) + " cannot use an async func value; use Task/await for async work", node->line);
        }
        if (callable.functionHasSignature && !callable.functionParamTypes.empty()) {
            typeError(std::string(apiName) + " requires a zero-parameter func value", node->line);
        }
    };

    if (!node->namespaceName.empty() && node->namespaceName == "Task" &&
        node->calleeName == "spawn" && node->arguments.size() == 1) {
        if (heldLockDepth_ > 0) {
            typeError("Task.spawn cannot cross a Mutex/RwLock capability while the lock is held", node->line);
        }
        const auto* argument = node->arguments[0].get();
        const InferredType callable = inferExpr(argument);
        if (callable.functionIsNamedReference) {
            const auto* fn = findNamedFunction(callable.functionReferenceOwner, callable.functionReferenceDispatch);
            std::string reason;
            if (!checkNamedFunctionConfinement(fn, reason))
                typeError("Task.spawn cannot transfer named function: " + reason, node->line);
        } else if (argument->kind == NodeKind::Identifier) {
            const auto* id = static_cast<const Identifier*>(argument);
            const auto info = symbols_.lookupVar(id->name);
            if (info && info->type == ZlType::FUNCTION) {
                if (info->functionUsesThis)
                    typeError("Task.spawn cannot use a func value that captures 'this'; wrap the object in Shared<T>", node->line);
                for (const auto& captured : info->functionCaptureNames) {
                    if (capturedValueCrossesThreadBoundary(symbols_, captured))
                        typeError("Task.spawn cannot cross thread boundary with captured '" + captured + "'; use Shared<T>, Atomic, or Mutex for mutable shared state", node->line);
                }
            }
        }
        validateThreadBoundaryCallable(callable, "Task.spawn");
    }
    if (!node->namespaceName.empty() && node->namespaceName == "Thread" &&
        node->calleeName == "start" && node->arguments.size() == 1) {
        if (heldLockDepth_ > 0) {
            typeError("Thread.start cannot cross a Mutex/RwLock capability while the lock is held", node->line);
        }
        const auto* argument = node->arguments[0].get();
        const InferredType callable = inferExpr(argument);
        if (callable.functionIsNamedReference) {
            const auto* fn = findNamedFunction(callable.functionReferenceOwner, callable.functionReferenceDispatch);
            std::string reason;
            if (!checkNamedFunctionConfinement(fn, reason))
                typeError("Thread.start cannot transfer named function: " + reason, node->line);
        } else if (argument->kind == NodeKind::Identifier) {
            const auto* id = static_cast<const Identifier*>(argument);
            const auto info = symbols_.lookupVar(id->name);
            if (info && info->type == ZlType::FUNCTION) {
                if (info->functionUsesThis)
                    typeError("Thread.start cannot use a func value that captures 'this'; wrap the object in Shared<T>", node->line);
                for (const auto& captured : info->functionCaptureNames) {
                    if (capturedValueCrossesThreadBoundary(symbols_, captured))
                        typeError("Thread.start cannot cross thread boundary with captured '" + captured + "'; use Shared<T>, Atomic, or Mutex for mutable shared state", node->line);
                }
            }
        }
        validateThreadBoundaryCallable(callable, "Thread.start");
    }

    // Locking callbacks execute while the corresponding synchronization capability
    // is held. Track that lexical capability while checking a direct callback body so
    // await/task/thread suspension or transfer cannot accidentally retain the lock.
    if (!node->namespaceName.empty() &&
        (node->namespaceName == "Mutex" || node->namespaceName == "RwLock") &&
        (node->calleeName == "withLock" || node->calleeName == "withRead" || node->calleeName == "withWrite") &&
        node->arguments.size() == 2) {
        (void)inferExpr(node->arguments[0].get());

        std::optional<std::string> lockName = resolveLockAlias(node->arguments[0].get(), lockAliases_);

        if (lockName) {
            const auto [it, inserted] = lockOrderRanks_.emplace(*lockName, lockOrderRanks_.size());
            const std::size_t rank = it->second;
            std::size_t maxHeldRank = 0;
            bool hasHeldRank = false;
            for (const auto& held : heldLockNames_) {
                const auto heldIt = lockOrderRanks_.find(held);
                if (heldIt != lockOrderRanks_.end()) {
                    maxHeldRank = std::max(maxHeldRank, heldIt->second);
                    hasHeldRank = true;
                }
            }
            if (hasHeldRank && rank < maxHeldRank &&
                std::find(heldLockNames_.begin(), heldLockNames_.end(), *lockName) == heldLockNames_.end()) {
                typeError("lock-order inversion: acquiring '" + *lockName +
                          "' while holding a later-ranked lock; acquire locks in a consistent order", node->line);
            }
            heldLockNames_.push_back(*lockName);
        }

        ++heldLockDepth_;
        try {
            const auto callback = inferExpr(node->arguments[1].get());
            --heldLockDepth_;
            if (lockName) heldLockNames_.pop_back();
            validateThreadBoundaryCallable(callback, (node->namespaceName + "." + node->calleeName).c_str());
            if (callback.type != ZlType::FUNCTION) {
                typeError(node->namespaceName + "." + node->calleeName + " expects a callable callback", node->line);
            }
            return InferredType(callback.functionReturnType, callback.functionReturnClassName);
        } catch (...) {
            --heldLockDepth_;
            if (lockName && !heldLockNames_.empty()) heldLockNames_.pop_back();
            throw;
        }
    }

    // --- static class method call (Namespace.name) ---
    // ZL-owned library APIs can be implemented as static methods while
    // primitive operations remain native. Static methods take precedence over
    // the native table for the same qualified name.
    if (!node->namespaceName.empty()) {
        const auto* classIt = semanticModel_.findClass(node->namespaceName);
        if (classIt != nullptr && !classIt->isDataType && !classIt->isEnumType) {
            auto methodIt = classIt->methods.find(node->calleeName);
            if (methodIt != classIt->methods.end()) {
                std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
                for (const auto& method : methodIt->second) {
                    if (method.isStatic) candidates.push_back({node->namespaceName, method});
                }
                if (!candidates.empty()) {
                    const auto prepared = prepareGenericOverloads(candidates, node->typeArgs, node->calleeName, node->line);
                    const auto args = inferArguments(
                        node->arguments, argumentExpectations(prepared.candidates, node->arguments.size()));
                    std::string owner;
                    const ClassMethodInfo* method = resolveOverload(
                        prepared.candidates, args.types, args.classNames, node->calleeName, node->line, &owner);
                    const ClassMethodInfo* original = method;
                    for (std::size_t i = 0; i < prepared.candidates.size(); ++i) {
                        if (&prepared.candidates[i].second == method) {
                            original = prepared.originals[i];
                            break;
                        }
                    }
                    checkAccess(owner, node->calleeName, method->access, /*isMethod=*/true, node->line);
                    warnIfDeprecatedMethod(*method, owner, node->calleeName, node->line);
                    node->resolvedDispatch = dispatchSignature(node->calleeName, *original);
                    node->resolvedTypeArgNames = prepared.resolvedTypeArgNames;
                    // A `func(int, string): bool` parameter is checked here
                    // exactly as it is on the implicit-self and method paths:
                    // a mismatch is a compile error, not the VM's argument
                    // count mismatch at the moment of the call.
                    validateFunctionArguments(node->calleeName, *method, args, node->line);
                    if (method->isAsync) {
                        InferredType task(ZlType::TASK);
                        task.taskValueType = method->returnType;
                        task.taskValueClassName = method->returnClassName;
                        task.className = canonicalTaskTypeName(task.taskValueType, task.taskValueClassName);
                        return task;
                    }
                    // Carry the returned callable's signature out, so a static
                    // factory that hands back a `func(...): T` produces a value
                    // the next call site can check against.
                    InferredType result(method->returnType, method->returnClassName);
                    result.functionParamTypes = method->returnFunctionParamTypes;
                    result.functionParamClassNames = method->returnFunctionParamClassNames;
                    result.functionReturnType = method->returnFunctionReturnType;
                    result.functionReturnClassName = method->returnFunctionReturnClassName;
                    result.functionHasSignature = method->returnFunctionHasSignature;
                    return result;
                }
            }
        }

        // --- native / library call (Namespace.name) ---
        std::string qualifiedName = node->namespaceName + "." + node->calleeName;
        auto sigOpt = findNativeSignature(qualifiedName);
        if (sigOpt) {
            const NativeSignature* sig = *sigOpt;

            // Package visibility: "zl.lang" is auto-imported everywhere, so
            // only a non-"zl.lang" homePackage needs checking against the
            // CURRENT class's own file's explicit imports.
            if (sig->homePackage != "zl.lang") {
                bool visible = false;
                const auto* nativeClassIt = semanticModel_.findClass(currentClassName_);
                if (nativeClassIt != nullptr) {
                    visible = nativeClassIt->visibleImports.count(sig->homePackage) > 0;
                    if (!visible && !currentClassName_.empty()) {
                        const std::string suffix = "." + currentClassName_;
                        if (sig->homePackage.size() > suffix.size() &&
                            sig->homePackage.compare(sig->homePackage.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            visible = true;
                        }
                    }
                }
                if (!visible) {
                    typeError(
                        "'" + qualifiedName + "' requires 'import " + sig->homePackage +
                        "' (not imported in this file)",
                        node->line);
                }
            }

            if ((qualifiedName == "Task.spawn" || qualifiedName == "Thread.start") &&
                node->arguments.size() == 1 && node->arguments[0]->kind == NodeKind::LambdaExpr) {
                validateThreadLambda(static_cast<const LambdaExpr*>(node->arguments[0].get()), qualifiedName.c_str());
            }

            // Check argument count.
            if (sig->paramTypes.size() != node->arguments.size()) {
                typeError(
                    "'" + qualifiedName + "' expects " +
                    std::to_string(sig->paramTypes.size()) +
                    " argument(s), got " +
                    std::to_string(node->arguments.size()),
                    node->line);
            }

            const auto args = inferArguments(node->arguments);
            const auto nativeBindings = nativeTypeBindings(*sig, args.types.empty() ? "unknown" :
                (args.classNames.front().empty() ? zlTypeName(args.types.front()) : args.classNames.front()));

            for (std::size_t i = 0; i < args.types.size(); ++i) {
                const ZlType argType = args.types[i];
                const std::string& argClass = args.classNames[i];
                const bool genericNumericAccepted = sig->paramTypes[i] == ZlType::DOUBLE &&
                    argType == ZlType::OBJECT && isCurrentGenericTypeParam(argClass);

                bool accepted = false;
                if (i < sig->acceptedParamTypes.size() && !sig->acceptedParamTypes[i].empty()) {
                    std::vector<ResolvedTypeArg> alternatives;
                    for (const auto candidate : sig->acceptedParamTypes[i])
                        if (candidate != ZlType::UNKNOWN) alternatives.push_back({candidate, ""});
                    // UNKNOWN allows a genuinely dynamic argument, not a
                    // wildcard that silently admits known unsupported kinds.
                    const auto allowed = typeResolver_.makeUnion(std::move(alternatives));
                    accepted = isAssignable(argType, allowed.type, argClass, allowed.className);
                } else {
                    accepted = isAssignable(argType, sig->paramTypes[i], argClass) || genericNumericAccepted;
                }

                if (accepted && i > 0 && i < sig->parameterTypeNames.size()) {
                    const auto expectedName = substituteTypeParams(sig->parameterTypeNames[i], nativeBindings);
                    const auto expected = variableInfo(typeAnnotationFromName(parseTypeName(expectedName)));
                    accepted = isAssignable(argType, expected.type, argClass, expected.className);
                }

                if (!accepted) {
                    const auto& acceptedTypes = (i < sig->acceptedParamTypes.size())
                        ? sig->acceptedParamTypes[i] : std::vector<ZlType>{};
                    std::string expected = zlTypeName(sig->paramTypes[i]);
                    if (!acceptedTypes.empty()) {
                        expected.clear();
                        for (std::size_t j = 0; j < acceptedTypes.size(); ++j) {
                            if (j) expected += ", ";
                            expected += zlTypeName(acceptedTypes[j]);
                        }
                    }
                    typeError(
                        "argument " + std::to_string(i + 1) + " to '" +
                        qualifiedName + "': expected " + expected + ", got " +
                        zlTypeName(argType),
                        node->line);
                }
            }

            if (sig->returnTypeRule == NativeReturnTypeRule::SAME_NUMERIC_KIND) {
                bool allInts = !args.types.empty();
                for (const ZlType type : args.types) allInts = allInts && (type == ZlType::INT);
                if (allInts) return ZlType::INT;
            }
            if (qualifiedName == "Task.spawn" && args.types.size() == 1 &&
                args.types[0] == ZlType::FUNCTION) {
                InferredType task(ZlType::TASK);
                task.taskValueType = args.functionReturnTypes.empty() ? ZlType::UNKNOWN : args.functionReturnTypes[0];
                task.taskValueClassName = args.functionReturnClassNames.empty() ? std::string() : args.functionReturnClassNames[0];
                task.className = canonicalTaskTypeName(task.taskValueType, task.taskValueClassName);
                return task;
            }
            if (sig->taskValueType != ZlType::UNKNOWN) {
                InferredType task(ZlType::TASK);
                task.taskValueType = sig->taskValueType;
                task.className = canonicalTaskTypeName(task.taskValueType, "");
                return task;
            }
            if (!sig->returnClassName.empty()) {
                const auto resultName = substituteTypeParams(sig->returnClassName, nativeBindings);
                const auto info = variableInfo(typeAnnotationFromName(parseTypeName(resultName)));
                InferredType result(info.type, info.className);
                result.functionParamTypes = info.functionParamTypes;
                result.functionParamClassNames = info.functionParamClassNames;
                result.functionReturnType = info.functionReturnType;
                result.functionReturnClassName = info.functionReturnClassName;
                result.functionHasSignature = info.functionHasSignature;
                result.taskValueType = info.taskValueType;
                result.taskValueClassName = info.taskValueClassName;
                return result;
            }
            return InferredType(sig->returnType, sig->returnClassName);
        }
        // --- runtime-registered extension native (zl-bind output) ---
        // Extensions have no catalog signature, so the check above cannot see
        // them - but they are real, callable natives once linked in. Their
        // declared arity is validated here exactly like the catalog's, and the
        // call is dynamically typed: the binding converts and validates each
        // argument at runtime and throws a clean, function-specific error on
        // mismatch. Without this fallback every extension native is
        // unreachable from ZL.
        if (const auto extIdx = findNativeFunctionByName(qualifiedName)) {
            const NativeFunction& ext = nativeFunctionTable()[*extIdx];
            std::size_t extArity = 0;
            try {
                extArity = ext.arity();
            } catch (const std::logic_error&) {
                typeError("'" + qualifiedName + "' is registered without a declared arity", node->line);
            }
            if (extArity != node->arguments.size()) {
                typeError("'" + qualifiedName + "' expects " + std::to_string(extArity) +
                          " argument(s), got " + std::to_string(node->arguments.size()),
                          node->line);
            }
            const auto extArgs = inferArguments(node->arguments);
            (void)extArgs;
            return ZlType::UNKNOWN;
        }
        typeError("unknown qualified func '" + qualifiedName + "'", node->line);
        return ZlType::UNKNOWN; // unreachable
    }

    // --- indirect call through a local variable holding a func value
    // (from a LambdaExpr), e.g. `var square = func(x) => x*x; square(5)` ---
    // Checked before the class-method chain below so a local lambda variable
    // correctly SHADOWS a same-named method, matching how plain variable
    // lookups already shadow outer scopes - same reasoning as Identifier
    // resolution never falling through to class members.
    {
        auto localVar = symbols_.lookupVar(node->calleeName);
        // Untyped lambda parameters are UNKNOWN until a func annotation is
        // written. Calling them (`func(f) => f(21)`) is a value call, not a
        // lookup of a named function `f`.
        if (localVar && (localVar->type == ZlType::FUNCTION || localVar->type == ZlType::UNKNOWN)) {
            node->isValueCall = true;
            node->calleeStorageName = localVar->storageName;
            if (!localVar->functionParamTypes.empty() &&
                localVar->functionParamTypes.size() != node->arguments.size()) {
                typeError("func value '" + node->calleeName + "' expects " +
                          std::to_string(localVar->functionParamTypes.size()) +
                          " argument(s), got " + std::to_string(node->arguments.size()), node->line);
            }
            // The slot's own declared signature is the only expectation
            // available here - there is no candidate set to agree on.
            std::vector<ArgumentExpectation> slotExpectations;
            slotExpectations.reserve(localVar->functionParamTypes.size());
            for (std::size_t i = 0; i < localVar->functionParamTypes.size(); ++i) {
                slotExpectations.push_back(argumentExpectation(
                    localVar->functionParamTypes[i],
                    i < localVar->functionParamClassNames.size() ? localVar->functionParamClassNames[i]
                                                                 : std::string()));
            }
            const auto args = inferArguments(node->arguments, slotExpectations);
            for (std::size_t i = 0; i < args.types.size(); ++i) {
                const ZlType argType = args.types[i];
                if (i < localVar->functionParamTypes.size()) {
                    const std::string expectedClass =
                        i < localVar->functionParamClassNames.size()
                            ? localVar->functionParamClassNames[i] : std::string();
                    const std::string actualClass =
                        i < args.classNames.size() ? args.classNames[i] : std::string();
                    if (!isAssignable(argType, localVar->functionParamTypes[i], actualClass, expectedClass)) {
                        typeError("argument " + std::to_string(i + 1) + " to func value '" +
                                  node->calleeName + "': expected " +
                                  zlTypeName(localVar->functionParamTypes[i]) + ", got " +
                                  zlTypeName(argType), node->line);
                    }
                }
            }
            return InferredType(localVar->functionReturnType, localVar->functionReturnClassName);
        }
    }

    // --- user-defined func call (implicit self-call; also resolves
    // methods inherited - but not overridden away - from an ancestor class,
    // by walking up from currentClassName_) ---
    if (!currentClassName_.empty()) {
        auto candidates = semanticModel_.collectMethodOverloads(currentClassName_, node->calleeName);
        if (!candidates.empty()) {
            // Argument types have to be known BEFORE an overload can be
            // picked (that's the whole point of overload resolution), so
            // infer every argument up front rather than per-parameter
            // against a single already-known signature like before.
            const auto prepared = prepareGenericOverloads(candidates, node->typeArgs, node->calleeName, node->line);
            const auto args = inferArguments(
                node->arguments, argumentExpectations(prepared.candidates, node->arguments.size()));

            std::string owner;
            const ClassMethodInfo* method =
                resolveOverload(prepared.candidates, args.types, args.classNames, node->calleeName, node->line, &owner);
            const ClassMethodInfo* original = method;
            for (std::size_t i = 0; i < prepared.candidates.size(); ++i) {
                if (&prepared.candidates[i].second == method) {
                    original = prepared.originals[i];
                    break;
                }
            }
            warnIfDeprecatedMethod(*method, owner, node->calleeName, node->line);
            node->resolvedDispatch = dispatchSignature(node->calleeName, *original);
            node->resolvedTypeArgNames = prepared.resolvedTypeArgNames;
                        validateFunctionArguments(node->calleeName, *method, args, node->line);
            if (method->isAsync) {
                InferredType task(ZlType::TASK);
                task.taskValueType = method->returnType;
                task.taskValueClassName = method->returnClassName;
                task.className = canonicalTaskTypeName(task.taskValueType, task.taskValueClassName);
                return task;
            }
            InferredType result(method->returnType, method->returnClassName);
            result.functionParamTypes = method->returnFunctionParamTypes;
            result.functionParamClassNames = method->returnFunctionParamClassNames;
            result.functionReturnType = method->returnFunctionReturnType;
            result.functionReturnClassName = method->returnFunctionReturnClassName;
            result.functionHasSignature = method->returnFunctionHasSignature;
            return result;
        }
    }

    // A call that reaches this point has no known declaration. Do not use
    // UNKNOWN as an escape hatch: accepting an unknown call makes a typo
    // look like a dynamically typed func and can defer a real error to
    // bytecode/runtime. Arguments are still checked first so diagnostics
    // inside them are preserved when possible.
    for (const auto& arg : node->arguments) {
        (void)inferExpr(arg.get());
    }
    typeError("undefined func '" + node->calleeName + "'", node->line);
    return ZlType::UNKNOWN; // unreachable
}

TypeChecker::InferredType TypeChecker::inferMove(const MoveExpr* node) {
    auto info = symbols_.lookupVar(node->name);
    if (!info) typeError("cannot move undefined variable '" + node->name + "'", node->line);
    if (movedVariables_.count(node->name))
        typeError("cannot move already-moved variable '" + node->name + "'", node->line);
    auto borrowIt = borrowSources_.find(node->name);
    if (borrowIt != borrowSources_.end())
        typeError("cannot move borrowed variable '" + node->name + "'", node->line);
    for (const auto& [borrowName, sourceName] : borrowSources_) {
        if (regionOverlaps(sourceName, node->name)) {
            typeError("cannot move owner '" + node->name + "' while borrow '" + borrowName + "' is active", node->line);
        }
    }
    if (info->ownership != OwnershipKind::OWNED)
        typeError("move requires an owned variable; '" + node->name + "' is " + ownershipName(info->ownership), node->line);
    node->storageName = info->storageName;
    movedVariables_.insert(node->name);
    InferredType result(info->type, info->className);
    result.ownership = OwnershipKind::OWNED;
    return result;
}

TypeChecker::InferredType TypeChecker::inferAssign(const AssignExpr* node) {
    auto varInfo = symbols_.lookupVar(node->name);
    if (varInfo) {
        node->storageName = varInfo->storageName;
        if (varInfo->declaration) varInfo = *varInfo->declaration;
    }
    InferredType valueResult;
    TypeAnnotation expected;
    if (varInfo) {
        expected = varInfo->annotation ? *varInfo->annotation : typeAnnotationFromName(parseTypeName(varInfo->runtimeTypeName()));
        valueResult = inferExpected(node->value.get(), varInfo->type, varInfo->className, expected);
    } else {
        valueResult = inferExpr(node->value.get());
    }
    ZlType valueType = valueResult.type;
    if (varInfo) {
        node->assertedTypeName = varInfo->runtimeTypeName();
        if (valueType == ZlType::FUNCTION)
            validateFunctionTypeAssignment(expected, valueResult, node->line);
        for (const auto& [borrowName, sourceName] : borrowSources_) {
            if (sourceName == node->name && borrowName != node->name) {
                typeError("cannot reassign owner '" + node->name + "' while borrow '" + borrowName + "' is active", node->line);
            }
        }
        std::string valueClassName = valueResult.className;
        if (varInfo->isConst) {
            typeError(
                "cannot reassign constant '" + node->name + "'",
                node->line);
        }
        if (varInfo->ownership == OwnershipKind::OWNED) {
            if (valueResult.ownership != OwnershipKind::OWNED) {
                typeError(std::string("cannot assign ") + ownershipName(valueResult.ownership) +
                          " value to owned variable '" + node->name + "'; use move", node->line);
            }
            // An owned assignment consumes an owned RHS. A direct identifier is
            // therefore required to be explicitly moved, preventing accidental
            // aliasing through the shared runtime representation. Fresh values
            // (e.g. `new Foo()`) are already unique and need no move wrapper.
            if (node->value->kind == NodeKind::Identifier) {
                typeError("assigning owned variable '" + node->name + "' from an identifier requires 'move " +
                          static_cast<const Identifier*>(node->value.get())->name + "'", node->line);
            }
        } else if (varInfo->ownership == OwnershipKind::BORROW) {
            auto region = borrowRegionForExpr(node->value.get());
            if (region) {
                borrowSources_[node->name] = *region;
            } else if (node->value->kind == NodeKind::Identifier) {
                const auto* rhsId = static_cast<const Identifier*>(node->value.get());
                auto rhsInfo = symbols_.lookupVar(rhsId->name);
                if (!rhsInfo) typeError("cannot borrow undefined variable '" + rhsId->name + "'", node->line);
                if (rhsInfo->ownership == OwnershipKind::BORROW) {
                    auto it = borrowSources_.find(rhsId->name);
                    if (it != borrowSources_.end() && movedVariables_.count(it->second))
                        typeError("cannot borrow from '" + rhsId->name + "' because its owner was moved", node->line);
                } else if (movedVariables_.count(rhsId->name)) {
                    typeError("cannot borrow moved variable '" + rhsId->name + "'", node->line);
                }
                borrowSources_[node->name] = rhsId->name;
            } else {
                typeError("borrow variable '" + node->name + "' must borrow from an existing variable or owned field", node->line);
            }
        } else if (valueResult.ownership == OwnershipKind::BORROW) {
            typeError("cannot store a borrowed value in non-borrow variable '" + node->name + "'", node->line);
        }
        if (varInfo->type == ZlType::ARRAY && varInfo->fixedArraySize) {
            if (node->value->kind == NodeKind::CollectionLiteral) {
                const auto* lit = static_cast<const CollectionLiteral*>(node->value.get());
                if (!lit->isMap && static_cast<std::size_t>(*varInfo->fixedArraySize) != lit->elements.size()) {
                    typeError("array variable '" + node->name + "' requires exactly " +
                              std::to_string(*varInfo->fixedArraySize) + " element(s)", node->line);
                }
            } else if (node->value->kind == NodeKind::Identifier) {
                const auto* id = static_cast<const Identifier*>(node->value.get());
                auto source = symbols_.lookupVar(id->name);
                if (source && source->type == ZlType::ARRAY) {
                    if (source->fixedArraySize && *source->fixedArraySize != *varInfo->fixedArraySize) {
                        typeError("cannot assign array[" + std::to_string(*source->fixedArraySize) +
                                  "] to array[" + std::to_string(*varInfo->fixedArraySize) + "] variable '" +
                                  node->name + "'", node->line);
                    }

                }
            }
        }
        if (!isAssignable(valueType, varInfo->type, valueClassName, varInfo->className)) {
            typeError(
                "cannot assign " + zlTypeName(valueType) +
                " to variable '" + node->name + "' of type " +
                zlTypeName(varInfo->type),
                node->line);
        }
        if (valueResult.functionIsNamedReference) {
            namedFunctionBindings_[node->name] = NamedFunctionBinding{
                valueResult.functionReferenceOwner, valueResult.functionReferenceDispatch};
        } else {
            namedFunctionBindings_.erase(node->name);
        }
        if (auto owner = sharedPayloadOwnerForExpr(node->value.get(), sharedPayloadOwners_, sharedCellAliases_))
            sharedPayloadOwners_[node->name] = *owner;
        else
            sharedPayloadOwners_.erase(node->name);

        const bool looksLikeSharedCell =
            valueType == ZlType::OBJECT && valueClassName.rfind("Shared<", 0) == 0;
        if (looksLikeSharedCell) {
            if (auto alias = resolveSharedCellAlias(node->value.get(), sharedCellAliases_))
                sharedCellAliases_[node->name] = *alias;
        } else {
            sharedCellAliases_.erase(node->name);
        }

        const bool looksLikeLock = varInfo->type == ZlType::OBJECT &&
            (varInfo->className == "Mutex" || varInfo->className == "RwLock");
        if (looksLikeLock) {
            if (auto alias = resolveLockAlias(node->value.get(), lockAliases_))
                lockAliases_[node->name] = *alias;
            else
                lockAliases_.erase(node->name);
        } else {
            lockAliases_.erase(node->name);
        }
        symbols_.invalidate(node->name);
        InferredType result(varInfo->type, varInfo->className);
        result.ownership = varInfo->ownership;
        if (varInfo->ownership == OwnershipKind::BORROW) result.borrowSource = borrowSources_[node->name];
        return result;
    }

    // Same reasoning as inferIdentifier: there's no scope this checker
    // doesn't model and no auto-declaring assignment in this language -
    // assigning to a name with no prior var/let is a genuine undefined
    // reference, not something to silently wave through.
    typeError("undefined variable '" + node->name + "'", node->line);
    return ZlType::UNKNOWN; // unreachable - typeError throws
}

void TypeChecker::validateCollectionElement(const AstNode* expr, ZlType expected,
                                             const std::string& expectedClassName,
                                             const std::string& context, std::size_t line) {
    const auto actualResult = inferExpr(expr);
    ZlType actual = actualResult.type;
    std::string actualClass = actualResult.className;
    if (!isAssignable(actual, expected, actualClass, expectedClassName)) {
        typeError("collection " + context + " expects " + zlTypeName(expected) +
                  " but got " + zlTypeName(actual), line);
    }
}

ZlType TypeChecker::inferCollectionLiteralExpected(const CollectionLiteral* node,
                                                    ZlType expectedType,
                                                    const std::string& expectedClassName,
                                                    const TypeAnnotation& expectedAnnotation) {
    // An empty literal (`{}` or `[]`) holds nothing that could contradict the
    // declared type, so the declaration alone decides the container: it is a
    // map where a map is expected and a list/set where one of those is. The
    // shape checks below are about *entries*, and an empty literal has none -
    // `map<string,int> m = {}` is an empty map, not a literal that "requires
    // key:value entries".
    const bool literalIsEmpty = node->entries.empty() && node->elements.empty();

    // Arrays use the same compact runtime list representation, but their
    // compile-time contract is stricter: element type must match and a fixed
    // size must be satisfied exactly.
    if (expectedType == ZlType::ARRAY) {
        if (node->isMap) typeError("array literal cannot contain key:value entries", node->line);
        if (expectedAnnotation.typeArgs.size() != 1) {
            typeError("array literal requires an element type", node->line);
        }
        if (expectedAnnotation.fixedSize &&
            static_cast<std::size_t>(*expectedAnnotation.fixedSize) != node->elements.size()) {
            typeError("array literal requires exactly " +
                      std::to_string(*expectedAnnotation.fixedSize) +
                      " element(s), got " + std::to_string(node->elements.size()), node->line);
        }
        std::string elemClass;
        ZlType elemType = resolveType(expectedAnnotation.typeArgs[0], &elemClass);
        node->targetCollectionKind = "array";
        node->targetDispatch = dispatchSignature("", ClassMethodInfo{{}, {elemType}, {elemClass}, {}, ZlType::VOID_TYPE, "", AccessModifier::PUBLIC});
        for (const auto& elem : node->elements) {
            validateCollectionElement(elem.get(), elemType, elemClass, "element", node->line);
        }
        return ZlType::ARRAY;
    }

    // Primitive collection annotations: list<T>, set<T>, map<K,V>.
    if (expectedType == ZlType::LIST || expectedType == ZlType::SET || expectedType == ZlType::MAP) {
        if (expectedType == ZlType::MAP && !node->isMap && !literalIsEmpty) {
            typeError("map literal requires key:value entries", node->line);
        }
        if (expectedType != ZlType::MAP && node->isMap) {
            typeError("list/set literal cannot contain map entries", node->line);
        }

        node->targetCollectionKind = expectedType == ZlType::LIST ? "list" :
                                      expectedType == ZlType::SET ? "set" : "map";

        if (expectedType == ZlType::MAP) {
            if (expectedAnnotation.typeArgs.size() != 2) {
                typeError("map literal requires map<K,V> type information", node->line);
            }
            std::string keyClass, valueClass;
            ZlType keyType = resolveType(expectedAnnotation.typeArgs[0], &keyClass);
            ZlType valueType = resolveType(expectedAnnotation.typeArgs[1], &valueClass);
            node->targetDispatch = dispatchSignature("", ClassMethodInfo{{}, {keyType, valueType}, {keyClass, valueClass}, {}, ZlType::VOID_TYPE, "", AccessModifier::PUBLIC});
            for (const auto& entry : node->entries) {
                validateCollectionElement(entry.first.get(), keyType, keyClass, "key", node->line);
                validateCollectionElement(entry.second.get(), valueType, valueClass, "value", node->line);
            }
        } else {
            if (expectedAnnotation.typeArgs.size() != 1) {
                typeError("collection literal requires an element type", node->line);
            }
            std::string elemClass;
            ZlType elemType = resolveType(expectedAnnotation.typeArgs[0], &elemClass);
            node->targetDispatch = dispatchSignature("", ClassMethodInfo{{}, {elemType}, {elemClass}, {}, ZlType::VOID_TYPE, "", AccessModifier::PUBLIC});
            if (expectedAnnotation.fixedSize &&
                static_cast<std::size_t>(*expectedAnnotation.fixedSize) != node->elements.size()) {
                typeError("array literal requires exactly " +
                          std::to_string(*expectedAnnotation.fixedSize) +
                          " element(s), got " + std::to_string(node->elements.size()), node->line);
            }
            for (const auto& elem : node->elements) {
                validateCollectionElement(elem.get(), elemType, elemClass, "element", node->line);
            }
        }
        std::string collectionName;
        if (expectedType == ZlType::MAP) {
            std::string k, v;
            const ZlType kt = resolveType(expectedAnnotation.typeArgs[0], &k);
            const ZlType vt = resolveType(expectedAnnotation.typeArgs[1], &v);
            if (k.empty()) k = zlTypeName(kt);
            if (v.empty()) v = zlTypeName(vt);
            collectionName = "map<" + k + "," + v + ">";
        } else {
            std::string e;
            const ZlType et = resolveType(expectedAnnotation.typeArgs[0], &e);
            if (e.empty()) e = zlTypeName(et);
            collectionName = (expectedType == ZlType::LIST ? "list<" : "set<") + e + ">";
        }
        return InferredType(expectedType, collectionName);
    }

    // Generic object collections: List<T>, Map<K,V>, Set<T>. These are real
    // ZL generic classes, so a typed literal should produce the same object
    // as `new List<T>()`, etc., while still checking every element at compile time.
    if (expectedType == ZlType::OBJECT) {
        std::string className = expectedClassName;
        const std::size_t lt = className.find('<');
        std::string base = lt == std::string::npos ? className : className.substr(0, lt);
        if (base == "List" || base == "Map" || base == "Set") {
            if ((base == "Map") != node->isMap && !literalIsEmpty) {
                typeError(base == "Map"
                    ? "Map literal requires key:value entries"
                    : "List/Set literal cannot contain map entries", node->line);
            }
            node->targetCollectionKind = base;
            node->targetCollectionClassName = className;

            if (base == "Map") {
                if (expectedAnnotation.typeArgs.size() != 2) {
                    typeError("Map literal requires Map<K,V> type information", node->line);
                }
                std::string keyClass, valueClass;
                ZlType keyType = resolveType(expectedAnnotation.typeArgs[0], &keyClass);
                ZlType valueType = resolveType(expectedAnnotation.typeArgs[1], &valueClass);
                // Generic built-in classes compile their template methods once,
                // so runtime dispatch uses the shared generic object bucket.
                node->targetDispatch = DispatchSignature{"", {{DispatchTypeKind::GENERIC_OBJECT, {}}, {DispatchTypeKind::GENERIC_OBJECT, {}}}};
                for (const auto& entry : node->entries) {
                    validateCollectionElement(entry.first.get(), keyType, keyClass, "key", node->line);
                    validateCollectionElement(entry.second.get(), valueType, valueClass, "value", node->line);
                }
            } else {
                if (expectedAnnotation.typeArgs.size() != 1) {
                    typeError(base + " literal requires an element type", node->line);
                }
                std::string elemClass;
                ZlType elemType = resolveType(expectedAnnotation.typeArgs[0], &elemClass);
                // List<T>/Set<T> template methods dispatch through the
                // shared generic object bucket at runtime.
                node->targetDispatch = DispatchSignature{"", {{DispatchTypeKind::GENERIC_OBJECT, {}}}};
                for (const auto& elem : node->elements) {
                    validateCollectionElement(elem.get(), elemType, elemClass, "element", node->line);
                }
            }
            return InferredType(ZlType::OBJECT, className);
        }
    }

    // No expected collection type: preserve the existing inference behavior.
    return inferCollectionLiteral(node);
}

TypeChecker::InferredType TypeChecker::inferCollectionLiteral(const CollectionLiteral* node) {
    if (node->isMap) {
        InferredType result(ZlType::MAP);
        if (node->entries.empty()) return result;
        const InferredType firstKey = inferExpr(node->entries.front().first.get());
        const InferredType firstValue = inferExpr(node->entries.front().second.get());
        bool same = true;
        for (std::size_t i = 1; i < node->entries.size(); ++i) {
            const InferredType k = inferExpr(node->entries[i].first.get());
            const InferredType v = inferExpr(node->entries[i].second.get());
            same = same && isAssignable(k.type, firstKey.type, k.className, firstKey.className) &&
                   isAssignable(firstKey.type, k.type, firstKey.className, k.className) &&
                   isAssignable(v.type, firstValue.type, v.className, firstValue.className) &&
                   isAssignable(firstValue.type, v.type, firstValue.className, v.className);
        }
        if (same) {
            const std::string k = firstKey.className.empty() ? zlTypeName(firstKey.type) : firstKey.className;
            const std::string v = firstValue.className.empty() ? zlTypeName(firstValue.type) : firstValue.className;
            result.className = "map<" + k + "," + v + ">";
        }
        return result;
    }
    // An empty literal has no elements to infer from, so its spelling is the
    // only evidence there is: `[]` is the list spelling and `{}` the set one
    // (docs/language-guide.md, "Typed collection literals"). Inferring `{}` as
    // a list made `var s = {}` a `list<unknown>`, which a later `set<int> t = s`
    // then refused with a MIR type-flow violation. A *non-empty* `{1, 2}` stays
    // a list: that spelling is also how a variadic argument list is written
    // (`Text.format(pattern, {"a", "b"})`), where set deduplication would be
    // wrong.
    if (node->elements.empty()) {
        // The spelling decides the container, and the element type is written
        // down as `unknown` rather than left off. An empty className reads as a
        // *bare* collection slot to isAssignable, which then accepts it
        // anywhere a list or a set is wanted - while MIR, which builds the same
        // literal as `set<unknown>` (lowering.cpp), refuses it. That
        // disagreement is what made `take({})` resolve to a `list<int>`
        // parameter and die in the verifier instead of in resolution (P2-10).
        if (node->bracketSyntax) return InferredType(ZlType::LIST, "list<unknown>");
        node->targetCollectionKind = "set";
        return InferredType(ZlType::SET, "set<unknown>");
    }
    InferredType result(ZlType::LIST);
    const InferredType first = inferExpr(node->elements.front().get());
    bool same = true;
    for (std::size_t i = 1; i < node->elements.size(); ++i) {
        const InferredType elem = inferExpr(node->elements[i].get());
        same = same && isAssignable(elem.type, first.type, elem.className, first.className) &&
               isAssignable(first.type, elem.type, first.className, elem.className);
    }
    if (same) {
        result.className = "list<" + (first.className.empty() ? zlTypeName(first.type) : first.className) + ">";
    }
    return result;
}

TypeChecker::InferredType TypeChecker::inferNewExpr(const NewExpr* node) {
    const auto* classIt = semanticModel_.findClass(node->className);
    if (classIt == nullptr) {
        typeError("unknown class '" + node->className + "'", node->line);
    }
    if (classIt->isDataType) {
        typeError("'" + node->className + "' is a data type, not a class - construct it with '" +
                   node->className + " { field: value, ... }' instead of '" + node->className + "(...)'",
                   node->line);
    }
    if (classIt->isEnumType) {
        typeError("'" + node->className + "' is an enum, not a class - use one of its members directly, e.g. '" +
                   node->className + "." +
                   (classIt->enumMembers.empty() ? "MEMBER" : classIt->enumMembers.front()) + "'",
                   node->line);
    }
    if (classIt->isMemoryDomain) {
        // Fail closed (the same convention as the native backend's
        // EnterRegion/ExitRegion): no consumer for a domain exists yet, so
        // no value of one can be created. The error names the phase that
        // removes it, so the message goes stale exactly when it stops being
        // true (docs/memory-domains.md §5.1).
        typeError("memory declaration '" + node->className +
                      "' cannot be constructed - nothing allocates into a domain yet; "
                      "allocation with a domain lands with the arena phase (docs/memory-domains.md)",
                  node->line);
    }

    // Generic instantiation, e.g. `new Box<int>(1)` - resolve the written
    // type arguments and substitute into a concrete synthetic shape (see
    // instantiateGenericClass); everything below then operates on THAT
    // shape instead of the generic template's own unsubstituted one.
    // node->className itself is left untouched either way - it's what the
    // Compiler uses for the actual runtime class (always the bare generic
    // name; see instantiateGenericClass's header comment on why - methods
    // are compiled once, generically, and there's no "Box<int>" class at
    // runtime, only "Box").
    std::string resolvedClassKey = node->className;
    if (!classIt->typeParams.empty()) {
        if (node->typeArgs.size() != classIt->typeParams.size()) {
            typeError(
                "class '" + node->className + "' takes " + std::to_string(classIt->typeParams.size()) +
                " type argument(s), got " + std::to_string(node->typeArgs.size()), node->line);
        }
        std::vector<ResolvedTypeArg> resolvedArgs;
        resolvedArgs.reserve(node->typeArgs.size());
        for (const auto& argAnnotation : node->typeArgs) {
            std::string argClassName;
            ZlType argType = resolveType(argAnnotation, &argClassName);
            ResolvedTypeArg arg{argType, argClassName};
            if (argType == ZlType::FUNCTION && argAnnotation.functionHasSignature) {
                arg.functionHasSignature = true;
                for (const auto& paramAnnotation : argAnnotation.functionParamTypes) {
                    std::string paramClass;
                    ZlType paramType = resolveType(paramAnnotation, &paramClass);
                    arg.functionParamTypes.push_back(ResolvedTypeArg{paramType, paramClass});
                }
                if (argAnnotation.functionReturnType) {
                    std::string returnClass;
                    ZlType returnType = resolveType(*argAnnotation.functionReturnType, &returnClass);
                    arg.functionReturnType = std::make_shared<ResolvedTypeArg>(ResolvedTypeArg{returnType, returnClass});
                }
            }
            resolvedArgs.push_back(std::move(arg));
        }
        resolvedClassKey = instantiateGenericClass(node->className, resolvedArgs, node->line);
        classIt = semanticModel_.findClass(resolvedClassKey);
    } else if (!node->typeArgs.empty()) {
        typeError("class '" + node->className + "' is not generic - it takes no type arguments", node->line);
    }

    const ClassShapeInfo& cls = *classIt;
    node->resolvedClassName = resolvedClassKey;
    warnIfDeprecatedClass(node->className, node->line);

    // Constructors aren't inherited, so no chain-walk here - just this
    // class's own declared constructors, wrapped into the (owner, info)
    // shape resolveOverload expects (owner is trivially resolvedClassKey
    // for every candidate, but resolveOverload is written generically for
    // the chain-walking method case too).
    std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
    candidates.reserve(cls.constructors.size());
    for (const auto& ctor : cls.constructors) {
        candidates.emplace_back(resolvedClassKey, ctor);
    }
    if (candidates.empty()) {
        // Shouldn't happen - Parser::parseClassDecl always synthesizes a
        // no-arg constructor - but stay permissive rather than crash if it
        // somehow does.
        if (!node->arguments.empty()) {
            typeError("class '" + node->className + "' has no constructor, cannot pass arguments", node->line);
        }
        for (const auto& arg : node->arguments) (void)inferExpr(arg.get());
        InferredType fresh(ZlType::OBJECT, resolvedClassKey);
        fresh.ownership = OwnershipKind::OWNED;
        return fresh;
    }

    const auto args = inferArguments(
        node->arguments, argumentExpectations(candidates, node->arguments.size()));

    std::string owner;
    const ClassMethodInfo* ctor = resolveOverload(candidates, args.types, args.classNames, node->className, node->line, &owner);
    warnIfDeprecatedMethod(*ctor, node->className, node->className, node->line);
    node->resolvedDispatch = dispatchSignature(node->className, *ctor);

    InferredType fresh(ZlType::OBJECT, resolvedClassKey);
    fresh.ownership = OwnershipKind::OWNED;
    return fresh;
}

TypeChecker::InferredType TypeChecker::inferDataLiteral(const DataLiteralExpr* node) {
    const auto* it = semanticModel_.findClass(node->typeName);
    if (it == nullptr) {
        typeError("unknown data type '" + node->typeName + "'", node->line);
    }
    const ClassShapeInfo& shape = *it;
    if (!shape.isDataType) {
        if (shape.isEnumType) {
            typeError("'" + node->typeName + "' is an enum, not a data type - use one of its members directly, e.g. '" +
                       node->typeName + "." +
                       (shape.enumMembers.empty() ? "MEMBER" : shape.enumMembers.front()) + "'", node->line);
        }
        typeError("'" + node->typeName + "' is a class, not a data type - construct it with '" +
                   node->typeName + "(...)' instead of '" + node->typeName + " { ... }'", node->line);
    }

    std::unordered_set<std::string> provided;
    for (const auto& [fieldName, valueExpr] : node->fields) {
        const ClassFieldInfo* fieldIt = semanticModel_.findFieldInHierarchy(node->typeName, fieldName);
        if (!fieldIt) {
            typeError("data type '" + node->typeName + "' has no field '" + fieldName + "'", node->line);
        }
        if (!provided.insert(fieldName).second) {
            typeError("field '" + fieldName + "' given more than once in '" + node->typeName + "' literal",
                      node->line);
        }

        const auto valueResult = inferExpr(valueExpr.get());
        ZlType valueType = valueResult.type;
        std::string valueClassName = valueResult.className;
        const ClassFieldInfo& field = *fieldIt;
        if (!isAssignable(valueType, field.type, valueClassName, field.className)) {
            typeError("field '" + fieldName + "' of '" + node->typeName + "' expects " + zlTypeName(field.type) +
                       ", got " + zlTypeName(valueType), node->line);
        }
        if (field.type == ZlType::FUNCTION && field.functionHasSignature && valueType == ZlType::FUNCTION) {
            if (field.functionParamTypes.size() != valueResult.functionParamTypes.size()) {
                typeError("field '" + fieldName + "' of '" + node->typeName + "' expects a func with " +
                          std::to_string(field.functionParamTypes.size()) + " parameter(s)", node->line);
            }
            for (std::size_t i = 0; i < field.functionParamTypes.size(); ++i) {
                const std::string expectedClass = i < field.functionParamClassNames.size()
                    ? field.functionParamClassNames[i] : std::string();
                const std::string actualClass = i < valueResult.functionParamClassNames.size()
                    ? valueResult.functionParamClassNames[i] : std::string();
                if (field.functionParamTypes[i] != ZlType::UNKNOWN &&
                    valueResult.functionParamTypes[i] != ZlType::UNKNOWN &&
                    (!isAssignable(valueResult.functionParamTypes[i], field.functionParamTypes[i], actualClass, expectedClass) ||
                     !isAssignable(field.functionParamTypes[i], valueResult.functionParamTypes[i], expectedClass, actualClass))) {
                    typeError("field '" + fieldName + "' of '" + node->typeName + "' has incompatible func parameter types", node->line);
                }
            }
            if (field.functionReturnType != ZlType::UNKNOWN && valueResult.functionReturnType != ZlType::UNKNOWN &&
                (!isAssignable(valueResult.functionReturnType, field.functionReturnType,
                               valueResult.functionReturnClassName, field.functionReturnClassName) ||
                 !isAssignable(field.functionReturnType, valueResult.functionReturnType,
                               field.functionReturnClassName, valueResult.functionReturnClassName))) {
                typeError("field '" + fieldName + "' of '" + node->typeName + "' has incompatible func return type", node->line);
            }
        }
    }

    // No default field values exist yet (see DataDecl's own comment), so a
    // partial literal - one that would otherwise leave some field nil - is
    // rejected outright rather than silently allowed.
    std::vector<std::string> requiredFields;
    std::vector<std::string> chain;
    for (std::string cur = node->typeName; !cur.empty();) {
        chain.push_back(cur);
        cur = semanticModel_.parentClassName(cur);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto* ancestor = semanticModel_.findClass(*it);
        if (!ancestor) continue;
        requiredFields.insert(requiredFields.end(), ancestor->fieldOrder.begin(), ancestor->fieldOrder.end());
    }
    for (const auto& fieldName : requiredFields) {
        if (!provided.count(fieldName)) {
            typeError("'" + node->typeName + "' literal is missing field '" + fieldName + "'", node->line);
        }
    }

    return InferredType(ZlType::OBJECT, node->typeName);
}

TypeChecker::InferredType TypeChecker::inferDataUpdate(const DataUpdateExpr* node) {
    const auto baseResult = inferExpr(node->base.get());
    if (baseResult.type != ZlType::OBJECT || baseResult.className.empty()) {
        typeError("'with' requires a data value, got " + zlTypeName(baseResult.type), node->line);
    }
    const auto* shape = semanticModel_.findClass(baseResult.className);
    if (!shape || !shape->isDataType) {
        typeError("'with' requires a data value, got '" + baseResult.className + "'", node->line);
    }
    std::unordered_set<std::string> seen;
    for (const auto& [fieldName, valueExpr] : node->fields) {
        if (!seen.insert(fieldName).second) {
            typeError("field '" + fieldName + "' given more than once in 'with' update", node->line);
        }
        std::string owner;
        const auto* field = semanticModel_.findFieldInHierarchy(baseResult.className, fieldName, &owner);
        if (!field) {
            typeError("data type '" + baseResult.className + "' has no field '" + fieldName + "'", node->line);
        }
        auto valueResult = inferExpr(valueExpr.get());
        if (!isAssignable(valueResult.type, field->type, valueResult.className, field->className)) {
            typeError("field '" + fieldName + "' of '" + baseResult.className + "' expects " + zlTypeName(field->type) +
                      ", got " + zlTypeName(valueResult.type), node->line);
        }
    }
    return baseResult;
}

TypeChecker::InferredType TypeChecker::inferFieldAccess(const FieldAccessExpr* node) {
    // Builtin Math constants are namespace values rather than instance fields.
    // Keep this before inferExpr(object), because `Math` is not a local variable.
    if (node->object->kind == NodeKind::Identifier) {
        const auto* mathIdNode = static_cast<const Identifier*>(node->object.get());
        double constantValue = 0.0;
        if (mathIdNode->name == "Math" && mathConstantValue(node->fieldName, constantValue)) {
            node->isMathConstantAccess = true;
            return ZlType::DOUBLE;
        }
    }

    // EnumName.MEMBER - checked BEFORE inferExpr(object), since "EnumName"
    // was never a variable and inferIdentifier would otherwise report it
    // as an undefined one. A local variable of the same name always wins
    // (shadowing an enum type name the same way any other name can be
    // shadowed in this language) - only a bare, not-currently-defined
    // identifier that names a known enum type is treated as enum-member
    // access.
    if (node->object->kind == NodeKind::Identifier) {
        const auto* idNode = static_cast<const Identifier*>(node->object.get());
        if (!symbols_.lookupVar(idNode->name)) {
            const auto* enumIt = semanticModel_.findClass(idNode->name);
            if (enumIt != nullptr && enumIt->isEnumType) {
                const auto& members = enumIt->enumMembers;
                if (std::find(members.begin(), members.end(), node->fieldName) == members.end()) {
                    typeError("enum '" + idNode->name + "' has no member '" + node->fieldName + "'", node->line);
                }
                node->isEnumMemberAccess = true;
                return InferredType(ZlType::OBJECT, idNode->name);
            }

            const ClassShapeInfo* classShape = semanticModel_.findClass(idNode->name);
            if (!classShape && semanticModel_.findInterface(idNode->name) != nullptr) {
                typeError("interface '" + idNode->name + "' cannot be used as a static-field namespace", node->line);
            }
            if (classShape && !classShape->isEnumType) {
                std::string staticOwner;
                const ClassFieldInfo* field = semanticModel_.findFieldInHierarchy(idNode->name, node->fieldName, &staticOwner);
                if (field && field->isStatic) {
                    // Static fields are inherited members, but storage is owned by the
                    // declaring class. Emit the declaring owner so Child.value and
                    // Parent.value name the same process-wide field.
                    checkAccess(staticOwner, node->fieldName, field->access, /*isMethod=*/false, node->line);
                    node->isStaticFieldAccess = true;
                    node->staticFieldClassName = staticOwner;
                    InferredType result(field->type, field->className);
                    result.functionParamTypes = field->functionParamTypes;
                    result.functionParamClassNames = field->functionParamClassNames;
                    result.functionReturnType = field->functionReturnType;
                    result.functionReturnClassName = field->functionReturnClassName;
                    return result;
                }
            }

            // A qualified static function reference is a first-class callable
            // value.  Calls use the same `Class.func(...)` syntax, but when the
            // member is not followed by parentheses the expression itself is
            // the callable.  Require an unambiguous static overload here;
            // overloaded function references can be added later with explicit
            // function-type contextual selection.
            const auto candidates = semanticModel_.collectMethodOverloads(idNode->name, node->fieldName);
            std::vector<std::pair<std::string, ClassMethodInfo>> staticCandidates;
            for (const auto& candidate : candidates) {
                if (candidate.second.isStatic) staticCandidates.push_back(candidate);
            }
            if (staticCandidates.size() == 1) {
                const auto& method = staticCandidates.front().second;
                node->isFunctionReference = true;
                node->resolvedFunctionDispatch = dispatchSignature(node->fieldName, method);
                InferredType result(ZlType::FUNCTION);
                result.className.clear();
                result.functionParamTypes = method.paramTypes;
                result.functionParamClassNames = method.paramClassNames;
                result.functionReturnType = method.isAsync ? ZlType::TASK : method.returnType;
                result.functionReturnClassName = method.isAsync
                    ? canonicalTaskTypeName(method.returnType, method.returnClassName)
                    : method.returnClassName;
                if (method.isAsync) {
                    result.taskValueType = method.returnType;
                    result.taskValueClassName = method.returnClassName;
                }
                result.functionIsAsync = method.isAsync;
                result.functionHasSignature = true;
                result.functionIsNamedReference = true;
                result.functionReferenceOwner = idNode->name;
                result.functionReferenceDispatch = node->resolvedFunctionDispatch;
                return result;
            }
            if (staticCandidates.size() > 1) {
                typeError("ambiguous static func reference '" + idNode->name + "." + node->fieldName + "'", node->line);
            }
        }
    }

    const auto objectResult = inferExpr(node->object.get());
    if (auto owner = sharedPayloadOwnerForExpr(node->object.get(), sharedPayloadOwners_, sharedCellAliases_)) {
        if (std::find(heldSharedLocks_.begin(), heldSharedLocks_.end(), *owner) == heldSharedLocks_.end()) {
            typeError("protected Shared payload field access requires Shared.withLock on '" + *owner + "'", node->line);
        }
    }
    ZlType objType = objectResult.type;
    std::string objClassName = objectResult.className;

    if (objType != ZlType::OBJECT || objClassName.empty()) {
        typeError("cannot access field '" + node->fieldName + "' on value of type " +
                  zlTypeName(objType), node->line);
    }

    std::string owner;
    const ClassFieldInfo* field = semanticModel_.findFieldInHierarchy(objClassName, node->fieldName, &owner);
    if (!field) {
        typeError("class '" + objClassName + "' has no field '" + node->fieldName + "'", node->line);
    }
    const auto fieldType = typeResolver_.fieldInContext(*field, objClassName, owner, currentClassName_, currentClassTypeParams_);
    field = &fieldType;
    if (field->isStatic) {
        typeError("static field '" + objClassName + "." + node->fieldName + "' must be accessed through its class", node->line);
    }
    checkAccess(owner, node->fieldName, field->access, /*isMethod=*/false, node->line);

    InferredType result(field->type, field->className);
    result.functionParamTypes = field->functionParamTypes;
    result.functionParamClassNames = field->functionParamClassNames;
    result.functionReturnType = field->functionReturnType;
    result.functionReturnClassName = field->functionReturnClassName;
    return result;
}

TypeChecker::InferredType TypeChecker::inferFieldAssign(const FieldAssignExpr* node) {
    if (node->object->kind == NodeKind::Identifier) {
        const auto* idNode = static_cast<const Identifier*>(node->object.get());
        if (!symbols_.lookupVar(idNode->name)) {
            const auto* cls = semanticModel_.findClass(idNode->name);
            if (cls) {
                std::string staticOwner;
                const ClassFieldInfo* field = semanticModel_.findFieldInHierarchy(idNode->name, node->fieldName, &staticOwner);
                if (field && field->isStatic) {
                    const auto valueResult = inferExpr(node->value.get());
                    if (!isAssignable(valueResult.type, field->type, valueResult.className, field->className)) {
                        typeError("cannot assign " + zlTypeName(valueResult.type) + " to static field '" + idNode->name + "." + node->fieldName + "'", node->line);
                    }
                    checkAccess(staticOwner, node->fieldName, field->access, /*isMethod=*/false, node->line);
                    node->isStaticFieldAssign = true;
                    node->staticFieldClassName = staticOwner;
                    return valueResult;
                }
            }
        }
        const auto* mathIdNode = static_cast<const Identifier*>(node->object.get());
        if (mathIdNode->name == "Math" &&
            (node->fieldName == "PI" || node->fieldName == "E" || node->fieldName == "TAU")) {
            typeError("cannot assign to Math." + node->fieldName, node->line);
        }
    }

    const auto objectResult = inferExpr(node->object.get());
    if (auto owner = sharedPayloadOwnerForExpr(node->object.get(), sharedPayloadOwners_, sharedCellAliases_)) {
        if (std::find(heldSharedLocks_.begin(), heldSharedLocks_.end(), *owner) == heldSharedLocks_.end()) {
            typeError("protected Shared payload field assignment requires Shared.withLock on '" + *owner + "'", node->line);
        }
    }
    ZlType objType = objectResult.type;
    std::string objClassName = objectResult.className;
    const auto valueResult = inferExpr(node->value.get());
    ZlType valueType = valueResult.type;

    if (objType != ZlType::OBJECT || objClassName.empty()) {
        typeError("cannot assign field '" + node->fieldName + "' on value of type " +
                  zlTypeName(objType), node->line);
    }

    const auto* objectShape = semanticModel_.findClass(objClassName);
    if (objectShape && objectShape->isDataType) {
        typeError("cannot assign to field '" + node->fieldName + "' of immutable data type '" + objClassName + "'", node->line);
    }

    std::string owner;
    const ClassFieldInfo* field = semanticModel_.findFieldInHierarchy(objClassName, node->fieldName, &owner);
    if (!field) {
        typeError("class '" + objClassName + "' has no field '" + node->fieldName + "'", node->line);
    }
    checkAccess(owner, node->fieldName, field->access, /*isMethod=*/false, node->line);

    if (node->object) {
        auto baseRegion = borrowRegionForExpr(node->object.get());
        if (baseRegion) {
            const std::string targetRegion = *baseRegion + "." + node->fieldName;
            for (const auto& [borrowName, sourceRegion] : borrowSources_) {
                if (regionOverlaps(targetRegion, sourceRegion)) {
                    typeError("cannot assign field '" + node->fieldName + "' while borrow '" + borrowName + "' of that region is active", node->line);
                }
            }
        }
    }

    std::string valueClassName = valueResult.className;
    const auto fieldType = typeResolver_.fieldInContext(*field, objClassName, owner, currentClassName_, currentClassTypeParams_);
    field = &fieldType;
    if (!isAssignable(valueType, field->type, valueClassName, field->className)) {
        typeError(
            "cannot assign " + zlTypeName(valueType) + " to field '" + node->fieldName +
            "' of class '" + objClassName + "' (expected " + zlTypeName(field->type) + ")",
            node->line);
    }
    if (field->type == ZlType::FUNCTION && valueType == ZlType::FUNCTION) {
        if (field->functionParamTypes.size() != valueResult.functionParamTypes.size()) {
            typeError("field '" + node->fieldName + "' expects a func with " +
                      std::to_string(field->functionParamTypes.size()) + " parameter(s)", node->line);
        }
        for (std::size_t i = 0; i < field->functionParamTypes.size(); ++i) {
            const std::string expectedClass = i < field->functionParamClassNames.size()
                ? field->functionParamClassNames[i] : std::string();
            const std::string actualClass = i < valueResult.functionParamClassNames.size()
                ? valueResult.functionParamClassNames[i] : std::string();
            if (field->functionParamTypes[i] != ZlType::UNKNOWN && valueResult.functionParamTypes[i] != ZlType::UNKNOWN &&
                (!isAssignable(valueResult.functionParamTypes[i], field->functionParamTypes[i], actualClass, expectedClass) ||
                 !isAssignable(field->functionParamTypes[i], valueResult.functionParamTypes[i], expectedClass, actualClass))) {
                typeError("field '" + node->fieldName + "' has incompatible func parameter types", node->line);
            }
        }
        if (field->functionReturnType != ZlType::UNKNOWN && valueResult.functionReturnType != ZlType::UNKNOWN &&
            (!isAssignable(valueResult.functionReturnType, field->functionReturnType,
                           valueResult.functionReturnClassName, field->functionReturnClassName) ||
             !isAssignable(field->functionReturnType, valueResult.functionReturnType,
                           field->functionReturnClassName, valueResult.functionReturnClassName))) {
            typeError("field '" + node->fieldName + "' has incompatible func return type", node->line);
        }
    }
    return valueType;
}

TypeChecker::InferredType TypeChecker::inferMethodCall(const MethodCallExpr* node) {
    const auto objectResult = inferExpr(node->object.get());
    if (auto owner = sharedPayloadOwnerForExpr(node->object.get(), sharedPayloadOwners_, sharedCellAliases_)) {
        if (std::find(heldSharedLocks_.begin(), heldSharedLocks_.end(), *owner) == heldSharedLocks_.end()) {
            typeError("protected Shared payload access requires Shared.withLock on '" + *owner + "'", node->line);
        }
    }
    ZlType objType = objectResult.type;
    if (objectResult.type == ZlType::OBJECT &&
        (objectResult.className == "Shared" || objectResult.className.rfind("Shared<", 0) == 0) &&
        node->methodName == "withLock" && node->arguments.size() == 1) {
        std::optional<std::string> sharedLockName;
        if (node->object && node->object->kind == NodeKind::Identifier) {
            sharedLockName = resolveSharedCellAlias(node->object.get(), sharedCellAliases_);
            if (sharedLockName) heldSharedLocks_.push_back(*sharedLockName);
        }
        ++heldLockDepth_;
        try {
            auto callback = inferExpr(node->arguments[0].get());
            --heldLockDepth_;
            if (sharedLockName && !heldSharedLocks_.empty()) heldSharedLocks_.pop_back();
            if (callback.type != ZlType::FUNCTION) {
                typeError("Shared.withLock expects a zero-argument func callback", node->line);
            }
            // This special-case bypasses ordinary class-method overload lookup,
            // so record the same dispatch key the compiler would assign to
            // Shared.withLock(func) before returning its callback result type.
            node->resolvedDispatch = DispatchSignature{
                "withLock", {{DispatchTypeKind::FUNCTION, {}}}
            };
            return InferredType(callback.functionReturnType, callback.functionReturnClassName);
        } catch (...) {
            --heldLockDepth_;
            if (sharedLockName && !heldSharedLocks_.empty()) heldSharedLocks_.pop_back();
            throw;
        }
    }

    std::string objClassName = objectResult.className;

    // List storage may relocate on push/pop/put. While an element-region borrow
    // is active, conservatively reject those mutators so the borrow remains valid.
    if (objType == ZlType::OBJECT && objClassName.rfind("List<", 0) == 0 &&
        (node->methodName == "push" || node->methodName == "pop" || node->methodName == "put")) {
        auto baseRegion = borrowRegionForExpr(node->object.get());
        if (baseRegion) {
            // push/pop can relocate the entire element storage. `put` only
            // invalidates the specific logical slot when the index is statically
            // known; a dynamic put conservatively invalidates all element regions.
            const bool mutatesSingleSlot = node->methodName == "put" && node->arguments.size() >= 2;
            std::optional<std::string> targetRegion;
            if (mutatesSingleSlot) {
                targetRegion = constantIndexRegion(node->arguments[0].get());
                if (targetRegion) *targetRegion = *baseRegion + *targetRegion;
            }
            if (!targetRegion) targetRegion = *baseRegion + "[*]";
            for (const auto& [borrowName, sourceRegion] : borrowSources_) {
                if (regionOverlaps(*targetRegion, sourceRegion)) {
                    typeError("cannot mutate collection while borrow '" + borrowName + "' of an element is active", node->line);
                }
            }
        }
    }

    // Task<T> exposes blocking and explicit-discard operations directly from
    // the language runtime. They are not ordinary class methods/vtable entries.
    if (objType == ZlType::TASK) {
        if (!node->arguments.empty()) {
            typeError("Task." + node->methodName + " takes no arguments", node->line);
        }
        if (node->methodName == "block") {
            if (currentFunctionIsAsync_) {
                typeError("Task.block() cannot be used inside an async func; use await instead", node->line);
            }
            node->isTaskMethod = true;
            return InferredType(objectResult.taskValueType, objectResult.taskValueClassName);
        }
        if (node->methodName == "ignore") {
            node->isTaskMethod = true;
            return ZlType::VOID_TYPE;
        }
        if (node->methodName == "cancel") {
            node->isTaskMethod = true;
            return ZlType::VOID_TYPE;
        }
        typeError("Task has no method '" + node->methodName + "'", node->line);
    }

    // A `string` receiver: the method is the `String.*` native primitive with
    // the receiver bound as its first argument (see kStringMethods). There is
    // no class behind `string`, so there is no dispatch to emit - both
    // backends read the resolved native off the node and call it, which is how
    // `s.length()` and `String.length(s)` stay one implementation.
    if (objType == ZlType::STRING) {
        const StringMethodMapping* mapping = findStringMethodMapping(node->methodName);
        if (mapping == nullptr) {
            typeError("type 'string' has no method '" + node->methodName +
                      "' (string methods: " + stringMethodNames() + ")", node->line);
        }
        const auto signature = findNativeSignature(mapping->native);
        if (!signature) {
            // The table and the catalog are compiled together, so this is a
            // stale row rather than anything a program did.
            typeError(std::string("internal error: string method '") + mapping->method +
                      "' maps to unknown native '" + mapping->native + "'", node->line);
        }
        const NativeSignature* sig = *signature;
        if (sig->paramTypes.empty() || sig->paramTypes.front() != ZlType::STRING) {
            typeError(std::string("internal error: native '") + mapping->native +
                      "' does not take a string receiver", node->line);
        }
        // The receiver occupies the native's first parameter; the call's own
        // arguments line up with the rest.
        const std::size_t expectedArguments = sig->paramTypes.size() - 1;
        if (node->arguments.size() != expectedArguments) {
            typeError("string method '" + node->methodName + "' expects " +
                      std::to_string(expectedArguments) + " argument(s), got " +
                      std::to_string(node->arguments.size()), node->line);
        }
        const auto args = inferArguments(node->arguments);
        for (std::size_t i = 0; i < args.types.size(); ++i) {
            const ZlType paramType = sig->paramTypes[i + 1];
            if (!isAssignable(args.types[i], paramType, args.classNames[i])) {
                typeError("argument " + std::to_string(i + 1) + " to string method '" +
                          node->methodName + "': expected " + zlTypeName(paramType) +
                          ", got " + zlTypeName(args.types[i]), node->line);
            }
        }
        node->isStringMethod = true;
        node->nativeMethodName = sig->qualifiedName;
        node->nativeMethodId = static_cast<std::int32_t>(sig->id);
        // A native whose result is a collection names it (String.split returns
        // list<string>); resolve that the same way the qualified-call path does.
        if (!sig->returnClassName.empty()) {
            const auto info = variableInfo(typeAnnotationFromName(parseTypeName(sig->returnClassName)));
            return InferredType(info.type, info.className);
        }
        return InferredType(sig->returnType, sig->returnClassName);
    }

    if (objType != ZlType::OBJECT || objClassName.empty()) {
        for (const auto& arg : node->arguments) (void)inferExpr(arg.get());
        typeError("cannot call method '" + node->methodName + "' on value of type " +
                  zlTypeName(objType), node->line);
        return ZlType::UNKNOWN; // unreachable
    }

    std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
    if (semanticModel_.hasInterface(objClassName)) {
        const auto& iface = *semanticModel_.findInterface(objClassName);
        semanticModel_.resolveInterfaceMethods(objClassName);
        auto methodIt = iface.methods.find(node->methodName);
        if (methodIt != iface.methods.end()) {
            candidates.emplace_back(objClassName, methodIt->second);
        }
    } else {
        candidates = semanticModel_.collectMethodOverloads(objClassName, node->methodName);
    }
    if (candidates.empty()) {
        typeError("type '" + objClassName + "' has no method '" + node->methodName + "'", node->line);
    }
    for (const auto& candidate : candidates) {
        if (candidate.second.isStatic) {
            typeError("static method '" + objClassName + "." + node->methodName + "' must be called through the class name", node->line);
        }
    }

    // Argument types have to be known before an overload can be picked.
    const auto prepared = prepareGenericOverloads(candidates, node->typeArgs, node->methodName, node->line);
    const auto args = inferArguments(
        node->arguments, argumentExpectations(prepared.candidates, node->arguments.size()));

    std::string owner;
    const ClassMethodInfo* method = resolveOverload(prepared.candidates, args.types, args.classNames, node->methodName, node->line, &owner);
    const ClassMethodInfo* original = method;
    for (std::size_t i = 0; i < prepared.candidates.size(); ++i) {
        if (&prepared.candidates[i].second == method) {
            original = prepared.originals[i];
            break;
        }
    }
    checkAccess(owner, node->methodName, method->access, /*isMethod=*/true, node->line);
    warnIfDeprecatedMethod(*method, owner, node->methodName, node->line);
    node->resolvedDispatch = dispatchSignature(node->methodName, *original);
    node->resolvedTypeArgNames = prepared.resolvedTypeArgNames;

        validateFunctionArguments(node->methodName, *method, args, node->line);

    if (method->isAsync) { InferredType task(ZlType::TASK); task.taskValueType = method->returnType; task.taskValueClassName = method->returnClassName; task.className = canonicalTaskTypeName(task.taskValueType, task.taskValueClassName); return task; }
    InferredType result(method->returnType, method->returnClassName);
    result.functionParamTypes = method->returnFunctionParamTypes;
    result.functionParamClassNames = method->returnFunctionParamClassNames;
    result.functionReturnType = method->returnFunctionReturnType;
    result.functionReturnClassName = method->returnFunctionReturnClassName;
    result.functionHasSignature = method->returnFunctionHasSignature;
    return result;
}

TypeChecker::InferredType TypeChecker::inferThisExpr(const ThisExpr* node) {
    if (currentFunctionIsStatic_) {
        typeError("'this' cannot be used inside a static func", node->line);
    }
    if (currentClassName_.empty()) {
        typeError("'this' used outside of a class member", node->line);
    }
    // Inside a generic template body, `this` denotes the self-parameterized
    // form (`Map<K,V>`), which is what a self-referential parameter such as
    // `putAll(Map<K,V> other)` resolves to. Reporting the bare template name
    // would make `other.putAll(this)` fail to match its own signature.
    const auto* thisShape = semanticModel_.findClass(currentClassName_);
    const std::vector<std::string>& classParams = thisShape ? thisShape->typeParams : currentClassTypeParams_;
    if (!classParams.empty() && currentClassName_.find('<') == std::string::npos) {
        std::string self = currentClassName_ + "<";
        for (std::size_t i = 0; i < classParams.size(); ++i) {
            if (i) self += ",";
            self += classParams[i];
        }
        self += ">";
        if (semanticModel_.findClass(self)) return InferredType(ZlType::OBJECT, self);
    }
    return InferredType(ZlType::OBJECT, currentClassName_);
}

TypeChecker::InferredType TypeChecker::inferSuperCallExpr(const SuperCallExpr* node) {
    bool allowed = allowSuperCallExprHere_;
    allowSuperCallExprHere_ = false; // consumed - any nested/later occurrence sees this as false

    if (!inConstructor_) {
        typeError("'super(...)' can only be called inside a constructor", node->line);
    }
    if (!allowed) {
        typeError("'super(...)' must be the first statement of a constructor", node->line);
    }

    const auto* classIt = semanticModel_.findClass(currentClassName_);
    if (classIt == nullptr || classIt->parentName.empty()) {
        typeError("class '" + currentClassName_ + "' does not extend anything - no super() to call", node->line);
    }
    const std::string& parentName = classIt->parentName;
    const ClassShapeInfo& parent = *semanticModel_.findClass(parentName);

    // Every class always has at least one constructor by this point (an
    // implicit no-arg one is synthesized for any class that doesn't declare
    // one explicitly - see Parser::parseClassDecl); which ONE it resolves to
    // now depends on the argument types, same as any other overloaded call.
    std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
    candidates.reserve(parent.constructors.size());
    for (const auto& ctor : parent.constructors) {
        candidates.emplace_back(parentName, ctor);
    }

    const auto args = inferArguments(
        node->arguments, argumentExpectations(candidates, node->arguments.size()));

    std::string owner;
    const ClassMethodInfo* ctor = resolveOverload(candidates, args.types, args.classNames, parentName, node->line, &owner);
    node->resolvedDispatch = dispatchSignature(parentName, *ctor);

    return ZlType::VOID_TYPE;
}

TypeChecker::InferredType TypeChecker::inferSuperMethodCallExpr(const SuperMethodCallExpr* node) {
    if (currentClassName_.empty()) {
        typeError("'super' used outside of a class member", node->line);
    }
    const auto* classIt = semanticModel_.findClass(currentClassName_);
    if (classIt == nullptr || classIt->parentName.empty()) {
        typeError("class '" + currentClassName_ + "' does not extend anything - no super." +
                  node->methodName + "() to call", node->line);
    }
    const std::string& parentName = classIt->parentName;

    auto candidates = semanticModel_.collectMethodOverloads(parentName, node->methodName);
    if (candidates.empty()) {
        typeError("no method '" + node->methodName + "' found on '" + parentName + "' or its ancestors", node->line);
    }

    const auto prepared = prepareGenericOverloads(candidates, node->typeArgs, node->methodName, node->line);
    const auto args = inferArguments(
        node->arguments, argumentExpectations(prepared.candidates, node->arguments.size()));

    std::string owner;
    const ClassMethodInfo* method = resolveOverload(prepared.candidates, args.types, args.classNames, node->methodName, node->line, &owner);
    const ClassMethodInfo* original = method;
    for (std::size_t i = 0; i < prepared.candidates.size(); ++i) {
        if (&prepared.candidates[i].second == method) {
            original = prepared.originals[i];
            break;
        }
    }
    checkAccess(owner, node->methodName, method->access, /*isMethod=*/true, node->line);
    node->resolvedDispatch = dispatchSignature(node->methodName, *original);
    node->resolvedTypeArgNames = prepared.resolvedTypeArgNames;

        validateFunctionArguments(node->methodName, *method, args, node->line);

    if (method->isAsync) { InferredType task(ZlType::TASK); task.taskValueType = method->returnType; task.taskValueClassName = method->returnClassName; task.className = canonicalTaskTypeName(task.taskValueType, task.taskValueClassName); return task; }
    InferredType result(method->returnType, method->returnClassName);
    result.functionParamTypes = method->returnFunctionParamTypes;
    result.functionParamClassNames = method->returnFunctionParamClassNames;
    result.functionReturnType = method->returnFunctionReturnType;
    result.functionReturnClassName = method->returnFunctionReturnClassName;
    result.functionHasSignature = method->returnFunctionHasSignature;
    return result;
}

// func(params) => expr   OR   func(params) { block }
//
// Params get their own nested scope (pushScope/popScope), same as
// checkFunctionDecl's - so a param name shadows an outer variable of the
// same name inside the lambda body, but the body can still read (capture)
// anything else from the enclosing scope via ordinary SymbolTable::lookupVar
// scope-chain walking. currentReturnType_/currentFunctionName_ are saved and
// restored around the body check, exactly like checkFunctionDecl does, so
// a `return` inside the lambda doesn't get checked against (or clobber) the
// ENCLOSING func's own return type.
TypeChecker::InferredType TypeChecker::inferLambdaExpr(const LambdaExpr* node) {
    analyzeLambdaCaptures(const_cast<LambdaExpr*>(node));
    for (const auto& captured : node->captureNames) {
        auto info = symbols_.lookupVar(captured);
        if (info && (info->ownership == OwnershipKind::OWNED || info->ownership == OwnershipKind::BORROW)) {
            typeError("lambda cannot capture " + std::string(ownershipName(info->ownership)) +
                      " variable '" + captured + "' by value", node->line);
        }
    }
    // A lambda's return inference is its own. These four fields are the scratch
    // accumulator that the `return` statements of a body write into, and until
    // this lambda's body is checked they belong to whatever function encloses
    // it, so they are saved here and restored on the way out - exactly like
    // currentReturnType_ below. Without that, a lambda *created inside* a
    // lambda body left its own return type behind in the accumulator, and the
    // enclosing lambda then claimed it as its inferred return:
    //     func() { var n = callIt(func() { return 1 }) if (n == 1) { log("y") } }
    // was typed `func(): int` - a non-void body with no `return` anywhere, which
    // MIR lowered to an `unreachable` exit block and the VM then ran as an
    // endless loop over the body.
    const std::vector<ZlType> previousParamTypes = lastFunctionParamTypes_;
    const ZlType previousInferredReturnType = lastFunctionReturnType_;
    const std::string previousInferredReturnClassName = lastFunctionReturnClassName_;
    const bool previousHadReturn = lastFunctionHadReturn_;
    lastFunctionParamTypes_.clear();
    lastFunctionReturnType_ = ZlType::UNKNOWN;
    const auto enclosingSymbols = symbols_.snapshot();
    symbols_.pushScope();
    const auto* body = node->hasExprBody ? node->exprBody.get() : node->blockBody.get();
    std::unordered_set<std::string> boundParams;
    for (const auto& param : node->params) boundParams.insert(param.name);
    for (const auto& assigned : collectLambdaCaptureRefs(body, boundParams).assignedNames) symbols_.invalidate(assigned);
    for (std::size_t i = 0; i < node->params.size(); ++i) {
        const auto& p = node->params[i];
        SymbolTable::VarInfo info;
        if (!p.type.name.empty() || !p.type.unionOf.empty()) {
            info = variableInfo(p.type);
            validateOwnership(p.type, p.ownership, p.type.line);
        } else if (currentLambdaExpectation_.active && i < currentLambdaExpectation_.paramTypes.size()) {
            info.type = currentLambdaExpectation_.paramTypes[i];
            if (i < currentLambdaExpectation_.paramClassNames.size()) info.className = currentLambdaExpectation_.paramClassNames[i];
        }
        info.ownership = p.ownership;
        p.storageName = symbols_.defineVar(p.name, std::move(info)).storageName;
    }

    const auto previousMovedVariables = movedVariables_;
    const auto previousBorrowSources = borrowSources_;
    ZlType previousReturnType = currentReturnType_;
    std::string previousReturnClassName = currentReturnClassName_;
    std::string previousFunctionName = currentFunctionName_;
    const bool previousFunctionIsAsync = currentFunctionIsAsync_;
    currentFunctionIsAsync_ = node->isAsync;
    currentReturnType_ = currentLambdaExpectation_.active
        ? currentLambdaExpectation_.returnType : ZlType::UNKNOWN;
    currentReturnClassName_ = currentLambdaExpectation_.active
        ? currentLambdaExpectation_.returnClassName : std::string();
    // An explicit `func(...): T` annotation is the authority for this lambda's
    // result, overriding whatever the call site happened to expect.
    ZlType declaredReturnType = ZlType::UNKNOWN;
    std::string declaredReturnClassName;
    if (node->hasDeclaredReturnType) {
        declaredReturnType = resolveType(node->declaredReturnType, &declaredReturnClassName);
        currentReturnType_ = declaredReturnType;
        currentReturnClassName_ = declaredReturnClassName;
    }
    // A callable annotation describes Task<T>, but an async body returns T.
    // Decode the complete expected signature rather than depending on the
    // optional task-value side metadata (which returned callbacks lacked).
    if (node->isAsync && currentReturnType_ == ZlType::TASK) {
        const auto [taskName, valueName] = splitGenericName(currentReturnClassName_);
        const auto value = decodeRenderedType(valueName);
        currentReturnType_ = value.type;
        currentReturnClassName_ = value.className;
    }
    currentFunctionName_ = "<lambda>";
    lastFunctionReturnType_ = ZlType::UNKNOWN;
    lastFunctionReturnClassName_.clear();
    lastFunctionHadReturn_ = false;

    InferredType inferredReturnResult(ZlType::NIL);
    if (node->hasExprBody) {
        inferredReturnResult = inferExpr(node->exprBody.get());
    } else {
        checkStatement(node->blockBody.get());
        if (lastFunctionReturnType_ != ZlType::UNKNOWN) {
            inferredReturnResult.type = lastFunctionReturnType_;
            inferredReturnResult.className = lastFunctionReturnClassName_;
        } else if (lastFunctionHadReturn_) {
            // Block body had a `return` but its type was UNKNOWN (e.g. untyped
            // param `x` in `func(x) { return x*2 }`). Arrow form keeps UNKNOWN
            // and allows the return value; block form previously dropped it to
            // NIL/void and triggered `[mir.return]` verification error (P0-1).
            inferredReturnResult.type = ZlType::UNKNOWN;
            inferredReturnResult.className.clear();
        } else if (currentLambdaExpectation_.active &&
                   currentLambdaExpectation_.returnType != ZlType::UNKNOWN) {
            inferredReturnResult.type = currentReturnType_;
            inferredReturnResult.className = currentReturnClassName_;
        }
    }
    if (node->hasDeclaredReturnType) {
        const bool bodyIsVoidLike = inferredReturnResult.type == ZlType::NIL ||
                                    inferredReturnResult.type == ZlType::VOID_TYPE;
        const bool declaredVoid = declaredReturnType == ZlType::VOID_TYPE;
        if (!(declaredVoid && bodyIsVoidLike) &&
            !isAssignable(inferredReturnResult.type, declaredReturnType,
                          inferredReturnResult.className, declaredReturnClassName)) {
            const std::string got = inferredReturnResult.className.empty()
                ? zlTypeName(inferredReturnResult.type) : inferredReturnResult.className;
            const std::string want = declaredReturnClassName.empty()
                ? zlTypeName(declaredReturnType) : declaredReturnClassName;
            typeError("lambda declares return type '" + want + "' but its body returns '" + got + "'",
                      node->line);
        }
        // The declaration, not the inferred body type, is this lambda's
        // contract - so a widening annotation stays visible to callers.
        inferredReturnResult.type = declaredReturnType;
        inferredReturnResult.className = declaredReturnClassName;
    }
    const ZlType inferredReturn = inferredReturnResult.type;
    const std::string inferredReturnClassName = inferredReturnResult.className;
    lastFunctionParamTypes_.reserve(node->params.size());
    std::vector<std::string> paramClassNames;
    paramClassNames.reserve(node->params.size());
    node->inferredParameterTypeNames.clear();
    node->inferredParameterTypeNames.reserve(node->params.size());
    for (std::size_t i = 0; i < node->params.size(); ++i) {
        const auto& p = node->params[i];
        std::string paramClass;
        const ZlType paramType = (p.type.name.empty() && p.type.unionOf.empty())
            ? (currentLambdaExpectation_.active && i < currentLambdaExpectation_.paramTypes.size()
                ? currentLambdaExpectation_.paramTypes[i] : ZlType::UNKNOWN)
            : resolveType(p.type, &paramClass);
        if (paramClass.empty() && currentLambdaExpectation_.active && (p.type.name.empty() && p.type.unionOf.empty()) &&
            i < currentLambdaExpectation_.paramClassNames.size()) {
            paramClass = currentLambdaExpectation_.paramClassNames[i];
        }
        lastFunctionParamTypes_.push_back(paramType);
        paramClassNames.push_back(paramClass);
        std::string typeName = zlTypeName(paramType);
        if (!paramClass.empty()) typeName = paramClass;
        node->inferredParameterTypeNames.push_back(typeName);
    }
    lastFunctionReturnType_ = inferredReturn;
    lastFunctionReturnClassName_ = inferredReturnClassName;
    const std::string inferredReturnName =
        inferredReturnClassName.empty() ? zlTypeName(inferredReturn) : inferredReturnClassName;
    // Bytecode/closure metadata stores the body's result, just like a named
    // async function. Only the callable signature below adds Task<T>.
    node->inferredReturnTypeName = inferredReturnName;

    InferredType result(ZlType::FUNCTION);
    result.functionParamTypes = lastFunctionParamTypes_;
    result.functionParamClassNames = std::move(paramClassNames);
    result.functionReturnType = node->isAsync ? ZlType::TASK : lastFunctionReturnType_;
    result.functionReturnClassName = node->isAsync
        ? canonicalTaskTypeName(inferredReturn, inferredReturnClassName)
        : inferredReturnClassName;
    if (node->isAsync) {
        result.taskValueType = inferredReturn;
        result.taskValueClassName = inferredReturnClassName;
    }
    result.functionIsAsync = node->isAsync;
    result.functionCaptureNames = node->captureNames;
    result.functionUsesThis = node->usesThis;
    result.functionHasSignature = true;

    currentReturnType_ = previousReturnType;
    currentReturnClassName_ = previousReturnClassName;
    currentFunctionName_ = previousFunctionName;
    currentFunctionIsAsync_ = previousFunctionIsAsync;
    lastFunctionParamTypes_ = previousParamTypes;
    lastFunctionReturnType_ = previousInferredReturnType;
    lastFunctionReturnClassName_ = previousInferredReturnClassName;
    lastFunctionHadReturn_ = previousHadReturn;
    symbols_.restore(enclosingSymbols);
    movedVariables_ = previousMovedVariables;
    borrowSources_ = previousBorrowSources;
    return result;
}

} // namespace zl