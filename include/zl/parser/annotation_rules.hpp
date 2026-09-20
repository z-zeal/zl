#pragma once

// The annotation registry: the one table of every annotation name the
// compiler knows, and the declaration kinds each may annotate.
//
// Why this table exists (docs/memory-domains.md §5.5): parseAnnotations used
// to accept any identifier, so an unrecognised name was silently ignored -
// `@memroy(Pool)` compiled and did nothing. The fix is an allowlist with two
// halves, each enforced where the compiler has the context for it:
//
//   - the *name* is checked here, in the parser, at the `@` itself - the
//     earliest point a location is known, and the point that catches a typo
//     no matter where the annotation sits;
//   - the *target* is checked in the type checker (TypeChecker::
//     validateAnnotationTargets), because only there is the declaration kind
//     fully known - a `func Foo` member is a method or a constructor
//     depending on its name, and file scope only accepts `class`.
//
// Adding an annotation: add a row here, consume it where it means something,
// and give it a fixture under tests/zl. A name in this table with no
// consumer is a promise the compiler silently ignores again - exactly what
// this table exists to prevent.

#include <cstdint>
#include <string>
#include <vector>

namespace zl::annotations {

// Where an annotation may appear. `Field` is deliberately absent: annotations
// on fields are rejected by the parser (parseClassMember), and no annotation
// targets one. `Method` is a non-constructor member func/operator; `Function`
// is any member func/operator including constructors.
enum Target : std::uint8_t {
    NONE      = 0,
    TYPE_DECL = 1 << 0, // file-scope `class` (the only annotated type declaration)
    FUNCTION  = 1 << 1, // member func/operator, constructors included
    METHOD    = 1 << 2, // member func/operator, constructors excluded
};

struct Rule {
    const char* name;
    std::uint8_t targets;
    // One line on what consumes the annotation, so the next reader does not
    // have to grep for it.
    const char* consumedBy;
};

// Sorted by name; the unknown-annotation diagnostic lists them in this order.
inline const std::vector<Rule>& rules() {
    static const std::vector<Rule> kRules = {
        {"Deprecated",        TYPE_DECL | FUNCTION, "deprecation warnings (TypeChecker, semantic model isDeprecated)"},
        {"SuppressWarnings",  TYPE_DECL | FUNCTION, "deprecation-warning suppression; argument is 'deprecation' (TypeChecker)"},
        {"Override",          METHOD,               "override checking against the parent class / interfaces (TypeChecker::checkOverrideAnnotation)"},
        {"ffi",               FUNCTION,             "FFI declaration binding (native_ffi_declarations.cpp)"},
        {"native",            FUNCTION,             "native-body lookup instead of a compiled body (Compiler, FunctionInfo::isNative)"},
    };
    return kRules;
}

[[nodiscard]] inline const Rule* find(const std::string& name) {
    for (const Rule& rule : rules()) {
        if (name == rule.name) return &rule;
    }
    return nullptr;
}

[[nodiscard]] inline bool isKnown(const std::string& name) { return find(name) != nullptr; }

[[nodiscard]] inline bool allowsOn(const std::string& name, std::uint8_t target) {
    const Rule* rule = find(name);
    return rule != nullptr && (rule->targets & target) != 0;
}

// The declaration *positions* the checker validates against. A position maps
// to the set of target bits that may appear there: a constructor accepts
// only FUNCTION-target annotations, while a method accepts both FUNCTION and
// METHOD ones - which is what makes @Override (METHOD-only) a constructor
// error but a Deprecated (FUNCTION) a constructor-legal one.
enum Position : std::uint8_t {
    POS_TYPE_DECL,
    POS_CONSTRUCTOR,
    POS_METHOD,
};

[[nodiscard]] inline std::uint8_t acceptableTargets(Position position) {
    switch (position) {
        case POS_TYPE_DECL:   return TYPE_DECL;
        case POS_CONSTRUCTOR: return FUNCTION;
        case POS_METHOD:      return FUNCTION | METHOD;
    }
    return NONE;
}

// "a class declaration" / "a constructor" / "a method" - the position-side
// spelling used in diagnostics.
[[nodiscard]] inline const char* positionName(Position position) {
    switch (position) {
        case POS_TYPE_DECL:   return "a class declaration";
        case POS_CONSTRUCTOR: return "a constructor";
        case POS_METHOD:      return "a method";
    }
    return "this declaration";
}

// Comma list of known names, for the unknown-annotation error.
[[nodiscard]] inline std::string knownNames() {
    std::string out;
    for (const Rule& rule : rules()) {
        if (!out.empty()) out += ", ";
        out += std::string("@") + rule.name;
    }
    return out;
}

// Names that may annotate `position` - for "allowed here: ..." diagnostics.
[[nodiscard]] inline std::string allowedNamesFor(Position position) {
    const std::uint8_t acceptable = acceptableTargets(position);
    std::string out;
    for (const Rule& rule : rules()) {
        if ((rule.targets & acceptable) != 0) {
            if (!out.empty()) out += ", ";
            out += std::string("@") + rule.name;
        }
    }
    return out;
}

} // namespace zl::annotations
