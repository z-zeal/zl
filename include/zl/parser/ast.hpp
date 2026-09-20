#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zl/lexer/token.hpp"
#include "zl/common/dispatch_signature.hpp"
#include "zl/common/ownership.hpp"

namespace zl {

enum class NodeKind {
    Program,
    ImportDecl,
    ClassDecl,
    InterfaceDecl,
    DataDecl,
    FunctionDecl,
    BlockStmt,
    VarDecl,
    LogStmt,
    LogExpr,
    IfStmt,
    ReturnStmt,
    ForStmt,
    WhileStmt,
    RepeatStmt,
    BreakStmt,
    ContinueStmt,
    TryStmt,
    ThrowStmt,
    ExprStmt,
    Literal,
    Identifier,
    UnaryExpr,
    AwaitExpr,
    BinaryExpr,
    CallExpr,
    AssignExpr,
    MoveExpr,
    CollectionLiteral,
    NewExpr,
    FieldAccessExpr,
    IndexAccessExpr,
    FieldAssignExpr,
    MethodCallExpr,
    ThisExpr,
    SuperCallExpr,
    SuperMethodCallExpr,
    DataLiteralExpr,
    DataUpdateExpr,
    EnumDecl,
    MemoryDecl,
    LambdaExpr,
    MatchExpr,
};

enum class AccessModifier {
    DEFAULT,
    PUBLIC,
    PRIVATE,
    PROTECTED,
};

// Base class. Every node "is-a" AstNode, same idea as a Java/C# abstract base.
struct AstNode {
    NodeKind kind;
    std::size_t line{0};
    // Populated by ModuleLoader before merging imported declarations.
    std::string sourceFile;

    explicit AstNode(NodeKind k) : kind(k) {}
    virtual ~AstNode() = default;
};

using NodePtr = std::unique_ptr<AstNode>;

// A type annotation, e.g. `int`, `array<int>`, `array[10]<int>`, `list<string>`,
// `set<int>`, `map<string, int>`, or a union `int|string`. This is plain data
// (not an AstNode) - it's descriptive metadata attached to a VarDecl/Param/
// FunctionDecl, not something that gets "executed."
//
// NOTE self-reference: TypeAnnotation holds std::vector<TypeAnnotation> as a
// member of itself. That's legal in C++17+ specifically because std::vector
// is guaranteed to support an incomplete element type - most other containers
// (or a plain member, or Java/C# without something like a List<T>) can't do this.
struct TypeAnnotation {
    std::string name;                        // "int", "string", "array", "list", "set", "map", "void", or a custom type name
    std::optional<int> fixedSize;             // array[10]<T> -> 10; nullopt for dynamic-size
    std::vector<TypeAnnotation> typeArgs;     // generic args: list<int> -> [int]; map<string,int> -> [string, int]; Box<int> -> [int]
    std::vector<TypeAnnotation> unionOf;      // int|string -> [int, string]; empty if this isn't a union
    // Function-type annotations: func(int, string): bool. The return type is
    // heap-linked because TypeAnnotation is recursively defined.
    std::vector<TypeAnnotation> functionParamTypes;
    std::shared_ptr<TypeAnnotation> functionReturnType;
    // Distinguishes bare `func` (any callable) from the explicit zero-argument
    // signature `func(): ...`.
    bool functionHasSignature{false};
    std::size_t line{0};                      // set by Parser::parseTypeAnnotation; used for generic-instantiation error messages
};

// A named, typed slot - used for both func parameters and data fields.
struct Param {
    std::string name;
    TypeAnnotation type;
    OwnershipKind ownership{OwnershipKind::GC};
    mutable std::string storageName;
};

// @Override, @Deprecated, @SuppressWarnings("deprecation")
// Only @SuppressWarnings takes an argument today (a string naming the
// warning category to suppress); the others are always bare.
struct Annotation {
    std::string name;
    // General annotation arguments. The raw lexeme is preserved so future
    // annotations can define their own typed argument semantics without
    // changing the parser/AST shape again.
    std::vector<std::string> arguments;
    // Backward-compatible shorthand for the existing single-string warning
    // annotation API.
    std::optional<std::string> argument;
    std::size_t line{0};
};

// A single method signature inside an `interface` body - name/params/return
// type only, no body (interfaces declare a contract, not an implementation).
struct InterfaceMethodSig {
    std::string name;
    std::vector<std::string> typeParams; // from `method<T>(...)`; empty if not generic
    std::vector<Param> params;
    TypeAnnotation returnType;
    bool isOperator{false};
    TokenType operatorToken{};
    std::size_t line{0};
};

struct Program : AstNode {
    std::vector<NodePtr> imports;        // ImportDecl, must all appear before any declaration
    std::vector<NodePtr> declarations;   // ClassDecl / InterfaceDecl / DataDecl / EnumDecl at file scope
    Program() : AstNode(NodeKind::Program) {}
};

// import io.github.test.AppTest
// Java-like: the dotted path maps directly to a source file path, one
// segment per directory, with the last segment naming the .zl file (minus
// extension) - so this example resolves to <root>/io/github/test/AppTest.zl.
struct ImportDecl : AstNode {
    std::vector<std::string> pathSegments;  // e.g. {"io", "github", "test", "AppTest"}
    std::string dottedName;                 // e.g. "io.github.test.AppTest" - for diagnostics
    ImportDecl() : AstNode(NodeKind::ImportDecl) {}
};

// class Name { member member ... }
// A file may contain multiple classes (C#-style) or just one (Java-style) -
// both are just "however many ClassDecls are in Program::declarations."
struct ClassDecl : AstNode {
    // Dotted import names (e.g. "zl.util.Queue") visible in the file this
    // class was declared in - i.e. that file's own `import` statements.
    // Stamped by ModuleLoader while merging, since file boundaries are
    // otherwise lost once every file's declarations land in one flat
    // Program. Auto-imported packages like zl.lang are NOT listed here -
    // they're always visible and checked separately (see
    // NativeSignature::homePackage / TypeChecker's native-call resolution).
    std::unordered_set<std::string> visibleImports;
    std::string name;
    std::vector<std::string> typeParams;   // from `class Box<T>` / `class Pair<A, B>`; empty if not generic
    std::unordered_map<std::string, std::vector<std::string>> typeParamInterfaceConstraints; // e.g. T: Addable
    std::string extendsName;               // parent class name from `extends Parent`; empty if none
    std::vector<TypeAnnotation> extendsTypeArgs; // from `extends Container<T>`; empty if the parent isn't generic (or there's no parent)
    std::vector<std::string> implementsNames; // interface names from `implements A, B, ...`; empty if none
    std::vector<NodePtr> members;          // FunctionDecl or VarDecl (fields)
    std::vector<Annotation> annotations;   // e.g. @Deprecated on the class itself
    ClassDecl() : AstNode(NodeKind::ClassDecl) {}
};

// interface Name extends Base1, Base2, ... { methodSig methodSig ... }
// Multiple-inheritance is allowed for interfaces (unlike classes' single
// `extends`), since a contract-only type has no state/method-body conflicts
// to resolve - only signature conflicts, which the type checker rejects at
// declaration time (see TypeChecker::registerInterfaceShape).
struct InterfaceDecl : AstNode {
    std::string name;
    std::vector<std::string> extendsNames;
    std::vector<InterfaceMethodSig> methods;
    InterfaceDecl() : AstNode(NodeKind::InterfaceDecl) {}
};

// data Name { field field ... } - a plain struct/record type.
// Fields are typed, no initializers yet (that's a future data-literal feature).
struct DataDecl : AstNode {
    std::string name;
    std::string extendsName;
    std::vector<TypeAnnotation> extendsTypeArgs;
    std::vector<Param> fields;
    std::vector<NodePtr> members; // FunctionDecl only: immutable record methods
    DataDecl() : AstNode(NodeKind::DataDecl) {}
};

// memory Name { ... } - a user-written memory-domain declaration
// (docs/memory-domains.md §5.1). `memory` is a contextual keyword: it opens a
// declaration only at file scope when followed by a name, so existing
// programs using `memory` as an identifier keep parsing.
//
// The body holds the domain's contract methods (acquire/release, plus the
// optional reset/exhausted/onCollect), private helpers, and its state as
// fields - the same member forms a class body takes, minus statics,
// constructors, operators and nested declarations. The type checker
// validates the contract (TypeChecker::registerMemoryDeclaration); nothing
// consumes a domain yet, so no declaration here is emitted into bytecode.
struct MemoryDecl : AstNode {
    // Dotted import names visible in the file this declaration was written
    // in - stamped by ModuleLoader like ClassDecl::visibleImports, for the
    // same reason: file boundaries are lost once every file's declarations
    // land in one flat Program.
    std::unordered_set<std::string> visibleImports;
    std::string name;
    std::vector<NodePtr> members; // FunctionDecl (contract methods, helpers) or VarDecl (state)
    MemoryDecl() : AstNode(NodeKind::MemoryDecl) {}
};

// TypeName { field: expr, field: expr, ... } - constructs a value of a
// `data` type. Every declared field must be given exactly once (no default
// values exist - see DataDecl's own comment above); the type checker
// enforces that, not the parser. Order in `fields` is the order the person
// wrote them in, which is also the order field-assignment gets compiled in
// (see Compiler::compileDataLiteralExpr) - irrelevant semantically (fields
// are looked up by name, not position) but keeps codegen deterministic.
//
// Only usable for a type actually declared with `data`, never a `class` -
// classes keep their existing `ClassName(args)`/`new ClassName(args)`
// construction path. See TypeChecker::inferDataLiteral.
struct DataLiteralExpr : AstNode {
    std::string typeName;
    std::vector<std::pair<std::string, NodePtr>> fields;
    DataLiteralExpr() : AstNode(NodeKind::DataLiteralExpr) {}
};

// `base with { field: expr, ... }` creates an immutable copy-update of a data value.
// The receiver keeps its concrete runtime type; only the named fields are replaced.
struct DataUpdateExpr : AstNode {
    NodePtr base;
    std::vector<std::pair<std::string, NodePtr>> fields;
    DataUpdateExpr() : AstNode(NodeKind::DataUpdateExpr) {}
};


// enum Name { MEMBER MEMBER ... } - a closed, ordered set of named
// constants. Deliberately minimal for a first cut (see the roadmap grab-bag
// item this satisfies): no associated data per member, no methods, no
// ordinal()/values() - just named constants with real static typing
// (accessed as `Name.MEMBER`, see FieldAccessExpr::isEnumMemberAccess) and
// structural equality/printing for free (they compile straight to string
// constants - see Compiler::compileFieldAccess and
// TypeChecker::inferFieldAccess).
struct EnumDecl : AstNode {
    std::string name;
    std::vector<std::string> members; // declaration order
    EnumDecl() : AstNode(NodeKind::EnumDecl) {}
};


// A first-class match expression. The initial Phase 7 slice supports literal
// and enum-member patterns. Each arm produces one expression result.
struct MatchExpr : AstNode {
    enum class PatternKind { Literal, EnumMember, Wildcard, Type, Variable, Data, List, Map };
    struct Pattern;
    struct DataFieldPattern {
        std::string fieldName;
        std::unique_ptr<Pattern> pattern;
        std::size_t line{0};
    };
    struct MapEntryPattern {
        std::unique_ptr<Pattern> key;
        std::unique_ptr<Pattern> value;
        std::size_t line{0};
    };
    struct Pattern {
        PatternKind kind{PatternKind::Wildcard};
        std::string raw;
        TokenType literalType{TokenType::UNKNOWN};
        std::string enumTypeName;
        std::string enumMemberName;
        TypeAnnotation typePattern;
        std::string bindingName;
        std::vector<DataFieldPattern> fields;
        std::vector<std::unique_ptr<Pattern>> elements;
        std::vector<MapEntryPattern> mapEntries;
        std::string containerKind;
        bool positional{false};
        std::size_t line{0};
    };
    struct Arm {
        mutable std::unordered_map<std::string, std::string> storageBindings;
        PatternKind patternKind{PatternKind::Literal};
        std::string raw;
        TokenType literalType{TokenType::UNKNOWN};
        std::string enumTypeName;
        std::string enumMemberName;
        // Type-pattern fields: `Type name` with optional `when` guard.
        TypeAnnotation typePattern;
        std::string bindingName;
        std::vector<DataFieldPattern> dataFields;
        std::vector<std::unique_ptr<Pattern>> listElements;
        std::vector<MapEntryPattern> mapEntries;
        std::string containerKind;
        bool positional{false};
        NodePtr guard;
        NodePtr result;
        std::size_t line{0};
    };
    NodePtr subject;
    std::vector<Arm> arms;
    MatchExpr() : AstNode(NodeKind::MatchExpr) {}
};

// func Name(param: Type, ...): ReturnType { body }
// `func` is the sole function declaration keyword.
struct FunctionDecl : AstNode {
    std::string name;
    bool isOperator{false};
    TokenType operatorToken{TokenType::UNKNOWN};
    std::vector<std::string> typeParams; // from `func firstOf<T>(...)`; empty if not generic
    std::vector<Param> params;
    TypeAnnotation returnType;   // name == "void" if none was written
    NodePtr body;                 // a BlockStmt
    bool isConstructor{false};    // true when name matches the enclosing class name
    bool isStatic{false};         // true for `static func` class members
    bool isAsync{false};          // true for `async func` declarations
    std::string ownerClassName;   // the class this func belongs to (empty for free functions)
    AccessModifier access{AccessModifier::DEFAULT};
    std::vector<Annotation> annotations;   // @Override, @Deprecated, @SuppressWarnings(...)
    FunctionDecl() : AstNode(NodeKind::FunctionDecl) {}
};

struct BlockStmt : AstNode {
    std::vector<NodePtr> statements;
    BlockStmt() : AstNode(NodeKind::BlockStmt) {}
};

// Three declaration shapes share this node:
//   var x = 5          (isConst = false, hasExplicitType = false)
//   let x = 5          (isConst = true,  hasExplicitType = false)
//   int x = 5          (isConst = false, hasExplicitType = true, type = int)
struct VarDecl : AstNode {
    bool isConst{false};
    bool hasExplicitType{false};
    TypeAnnotation type;       // only meaningful when hasExplicitType is true
    std::string name;
    mutable std::string storageName;
    mutable std::string assertedTypeName;
    NodePtr initializer;       // nullptr if no `= expr` given
    AccessModifier access{AccessModifier::DEFAULT};
    bool isStatic{false};
    OwnershipKind ownership{OwnershipKind::GC};
    VarDecl() : AstNode(NodeKind::VarDecl) {}
};

// log(expr)
struct LogStmt : AstNode {
    NodePtr argument;
    LogStmt() : AstNode(NodeKind::LogStmt) {}
};

// log(expr) as an expression; evaluates to void after performing the log side effect.
struct LogExpr : AstNode {
    NodePtr argument;
    LogExpr() : AstNode(NodeKind::LogExpr) {}
};

// if cond { ... } elif cond { ... } else { ... }
// "else if" (two words) is folded into the same branch list as "elif" by the parser.
struct IfStmt : AstNode {
    struct Branch {
        NodePtr condition;
        NodePtr body;   // a BlockStmt
    };
    std::vector<Branch> branches;  // branches[0] is the `if`, rest are elif
    NodePtr elseBody;               // nullptr if there's no else
    IfStmt() : AstNode(NodeKind::IfStmt) {}
};

// return expr   OR   return   (bare)
struct ReturnStmt : AstNode {
    NodePtr value;   // nullptr for a bare `return`
    ReturnStmt() : AstNode(NodeKind::ReturnStmt) {}
};

// for i in start..end step s { body }   ('step s' is optional, defaults to 1)
// Direction is resolved at RUNTIME from step's sign, so both ascending and
// descending ranges work: `for i in 0..10 { }` and `for i in 10..0 step -1 { }`.
struct ForStmt : AstNode {
    std::string varName;
    mutable std::string storageName;
    NodePtr start;
    NodePtr end;
    NodePtr step;      // never null - parser fills in a literal 1 if 'step' was omitted
    NodePtr body;       // a BlockStmt
    ForStmt() : AstNode(NodeKind::ForStmt) {}
};

// while cond { body }
struct WhileStmt : AstNode {
    NodePtr condition;
    NodePtr body;
    WhileStmt() : AstNode(NodeKind::WhileStmt) {}
};

// repeat { body } while cond   - runs body once, THEN checks the condition
// (do-while semantics; complements `while`, which can run zero times)
struct RepeatStmt : AstNode {
    NodePtr body;
    NodePtr condition;
    RepeatStmt() : AstNode(NodeKind::RepeatStmt) {}
};

struct BreakStmt : AstNode {
    BreakStmt() : AstNode(NodeKind::BreakStmt) {}
};

struct ContinueStmt : AstNode {
    ContinueStmt() : AstNode(NodeKind::ContinueStmt) {}
};

// try { tryBlock } catch [ExceptionType] e { catchBlock } finally { finallyBlock }
struct CatchClause {
    std::string varName;
    std::optional<TypeAnnotation> type;
    NodePtr block;
    std::size_t line{0};
    mutable std::string storageName;
};

struct TryStmt : AstNode {
    NodePtr tryBlock;
    std::vector<CatchClause> catches;
    NodePtr finallyBlock;
    TryStmt() : AstNode(NodeKind::TryStmt) {}
};

// throw expr - converts expr to a string message and raises it; catchable by
// an enclosing try/catch, or a program-ending error if nothing catches it.
struct ThrowStmt : AstNode {
    NodePtr value;
    ThrowStmt() : AstNode(NodeKind::ThrowStmt) {}
};

// An expression used as a statement on its own, e.g. a bare func call.
struct ExprStmt : AstNode {
    NodePtr expression;
    ExprStmt() : AstNode(NodeKind::ExprStmt) {}
};

// --- expressions ---

struct Literal : AstNode {
    TokenType literalType{TokenType::UNKNOWN}; // INT_LITERAL, STRING_LITERAL, etc.
    std::string raw;                            // raw text; compiler stage converts it
    Literal() : AstNode(NodeKind::Literal) {}
};

struct Identifier : AstNode {
    std::string name;
    mutable std::string storageName;
    Identifier() : AstNode(NodeKind::Identifier) {}
};

// e.g. -x, !flag, ~mask
struct AwaitExpr : AstNode {
    NodePtr operand;
    AwaitExpr() : AstNode(NodeKind::AwaitExpr) {}
};

struct UnaryExpr : AstNode {
    TokenType op{TokenType::UNKNOWN};
    NodePtr operand;
    mutable bool isOperatorOverload{false};
    mutable DispatchSignature resolvedOperatorDispatch;
    UnaryExpr() : AstNode(NodeKind::UnaryExpr) {}
};

// e.g. a + b, x << 2, a && b
struct BinaryExpr : AstNode {
    TokenType op{TokenType::UNKNOWN};
    NodePtr left;
    NodePtr right;
    mutable bool isOperatorOverload{false};
    mutable DispatchSignature resolvedOperatorDispatch;
    BinaryExpr() : AstNode(NodeKind::BinaryExpr) {}
};

// name(arg, arg, ...)   OR   Namespace.name(arg, arg, ...)  (a library call)
struct CallExpr : AstNode {
    std::string namespaceName; // e.g. "Math" in Math.sqrt(x); empty for a plain user-func call
    std::string calleeName;
    std::vector<TypeAnnotation> typeArgs; // from `firstOf<int>(...)`; empty if omitted
    // Resolved method-level type arguments (canonical names) filled by the checker.
    mutable std::vector<std::string> resolvedTypeArgNames;
    std::vector<NodePtr> arguments;
    // Filled in by TypeChecker::inferCall (Phase 2, method overloading):
    // the signature suffix of the SPECIFIC overload chosen for this call
    // site, e.g. "(int,string)" - see TypeChecker::paramTypesSuffix. Empty
    // for native/namespaced calls (Math.sqrt etc.), which don't overload.
    // The compiler reads this back during compileCallExpr instead of
    // re-running overload resolution itself - overload resolution is a
    // type-checking concern (it needs inferred argument types), so doing it
    // twice would mean duplicating that inference logic in the compiler too.
    mutable DispatchSignature resolvedDispatch;
    // Set by TypeChecker::inferCall when `calleeName` turns out to resolve to
    // a LOCAL VARIABLE holding a func value (produced by a LambdaExpr),
    // not a class-qualified method - i.e. this is really `someVar(args)`,
    // an indirect call through a closure, not a same-class self-call.
    // Compiler::compileCall checks this first, the same way
    // FieldAccessExpr::isEnumMemberAccess redirects compileFieldAccess.
    mutable bool isValueCall{false};
    mutable std::string calleeStorageName;
    // `Type.of(MyClass)` is a class/type literal rather than a variable lookup.
    mutable bool isClassTypeLiteral{false};
    mutable std::string classTypeLiteralName;
    // Declared identity of a fresh native-factory result (not a cast). Kept
    // separate from value inference so existing objects are never relabelled.
    mutable std::string nativeFactoryTypeName;
    CallExpr() : AstNode(NodeKind::CallExpr) {}
};

// name = value   (an EXPRESSION, not a declaration - no var/let/type prefix,
// and it evaluates to the assigned value, so `a = b = 5` works). Only a bare
// identifier target is supported for now - no field/array assignment yet.
// move expr - explicitly consumes an owned local and transfers its ownership.
struct MoveExpr : AstNode {
    std::string name;
    mutable std::string storageName;
    MoveExpr() : AstNode(NodeKind::MoveExpr) {}
};

struct AssignExpr : AstNode {
    std::string name;
    mutable std::string storageName;
    NodePtr value;
    // The storage contract chosen by semantic checking, not an inferred
    // type of the RHS. Codegen checks it before overwriting the old value.
    mutable std::string assertedTypeName;
    AssignExpr() : AstNode(NodeKind::AssignExpr) {}
};

// { }                      - empty literal (ambiguous type until assigned)
// { 1, 2, 3 }              - array/list/set literal
// { "a": 1, "b": 2 }       - map literal (":" separator, JSON-style)
// Parsed generically here; which concrete collection type it becomes is
// resolved later from the declared TypeAnnotation - not implemented yet
// (parser scaffolding only, per the type-system plan).
struct CollectionLiteral : AstNode {
    bool isMap{false};
    bool bracketSyntax{false};                         // true for [a, b, c] list literals
    std::vector<NodePtr> elements;                     // populated when !isMap
    std::vector<std::pair<NodePtr, NodePtr>> entries;   // populated when isMap

    // Filled by the type checker when an explicit collection type provides
    // expected element/key/value information. Empty for inferred literals.
    mutable std::string targetCollectionKind;          // "list", "map", "set", "List", "Map", "Set"
    mutable std::string targetCollectionClassName;     // concrete generic class key for object collections
    mutable DispatchSignature targetDispatch;          // push/add or map put signature
    CollectionLiteral() : AstNode(NodeKind::CollectionLiteral) {}
};

// ClassName(arg, arg, ...) - object construction expression.
struct NewExpr : AstNode {
    std::string className;
    std::vector<TypeAnnotation> typeArgs; // e.g. new Box<int>(1) -> [int]; empty for a non-generic class
    std::vector<NodePtr> arguments;
    mutable DispatchSignature resolvedDispatch; // see CallExpr::resolvedDispatch - here, the chosen constructor overload
    mutable std::string resolvedClassName; // concrete generic identity (e.g. Box<int>), while runtime dispatch stays on className
    NewExpr() : AstNode(NodeKind::NewExpr) {}
};

// obj.field - read a field from an object reference.
// object[index] - indexed access. The first lifetime-aware implementation
// supports list elements; the type checker may attach a collection-region
// identity to the expression when the source list is an owned value.
struct IndexAccessExpr : AstNode {
    NodePtr object;
    NodePtr index;
    IndexAccessExpr() : AstNode(NodeKind::IndexAccessExpr) {}
};

struct FieldAccessExpr : AstNode {
    NodePtr object;         // the expression producing the object
    std::string fieldName;
    // Set by TypeChecker::inferFieldAccess (same mutable-annotation
    // pattern as CallExpr::resolvedDispatch) when `object` turns out to be a
    // bare reference to an enum TYPE name rather than a variable - i.e.
    // this node is actually `EnumName.MEMBER`, not real instance field
    // access. Compiler::compileFieldAccess checks this first: when true it
    // just pushes fieldName as a string constant instead of trying to
    // compile `object` as an expression (which would fail at runtime -
    // "EnumName" was never a variable to begin with).
    mutable bool isEnumMemberAccess{false};
    // Set when this is a builtin numeric constant such as Math.PI.
    // These are namespace constants rather than instance fields.
    mutable bool isMathConstantAccess{false};
    // Set for a static named-function reference such as `Worker.run`.
    // The object is a namespace/type name rather than an instance value.
    mutable bool isFunctionReference{false};
    mutable bool isStaticFieldAccess{false};
    mutable std::string staticFieldClassName;
    mutable DispatchSignature resolvedFunctionDispatch;
    FieldAccessExpr() : AstNode(NodeKind::FieldAccessExpr) {}
};

// obj.field = value - write a field on an object reference.
struct FieldAssignExpr : AstNode {
    NodePtr object;         // the expression producing the object
    std::string fieldName;
    NodePtr value;
    mutable bool isStaticFieldAssign{false};
    mutable std::string staticFieldClassName;
    FieldAssignExpr() : AstNode(NodeKind::FieldAssignExpr) {}
};

// obj.method(args...) - call a method on an object reference.
struct MethodCallExpr : AstNode {
    NodePtr object;         // the expression producing the object
    std::string methodName;
    std::vector<TypeAnnotation> typeArgs; // from `items.map<U>(...)`; empty if omitted
    mutable std::vector<std::string> resolvedTypeArgNames;
    std::vector<NodePtr> arguments;
    mutable DispatchSignature resolvedDispatch; // see CallExpr::resolvedDispatch
    mutable bool isTaskMethod{false};
    // Set by TypeChecker::inferMethodCall when the receiver is a `string`:
    // the call is the native catalog entry in `nativeMethodName`, with the
    // receiver bound as that native's first argument. Both backends read this
    // back instead of dispatching a method - there is no class behind a
    // `string`, and no second implementation behind the method spelling.
    // See docs/language-guide.md#string-methods.
    mutable bool isStringMethod{false};
    mutable std::string nativeMethodName;      // catalog name, e.g. "String.length"
    mutable std::int32_t nativeMethodId{-1};   // its NativeId, or -1 when unresolved
    MethodCallExpr() : AstNode(NodeKind::MethodCallExpr) {}
};

// `this` - refers to the current object instance inside a method/constructor.
struct ThisExpr : AstNode {
    ThisExpr() : AstNode(NodeKind::ThisExpr) {}
};

// super(args) - calls the parent class's constructor. Only legal as the very
// first statement of a constructor, in a class that `extends` something (see
// TypeChecker::checkClassDecl). If a constructor doesn't call `super(...)`
// explicitly, the compiler injects an implicit no-arg one - see
// Compiler::compile.
struct SuperCallExpr : AstNode {
    std::vector<NodePtr> arguments;
    mutable DispatchSignature resolvedDispatch; // see CallExpr::resolvedDispatch - here, the chosen parent-constructor overload
    SuperCallExpr() : AstNode(NodeKind::SuperCallExpr) {}
};

// super.method(args) - calls the parent class's implementation of `method`
// directly, bypassing dynamic dispatch (regular `this.method()` calls resolve
// using the receiver's actual runtime class, which is how overriding works;
// `super.method()` deliberately skips that and always means "my declared
// parent's version", resolved once at compile time).
struct SuperMethodCallExpr : AstNode {
    std::string methodName;
    std::vector<TypeAnnotation> typeArgs; // from `super.method<T>(...)`; empty if omitted
    mutable std::vector<std::string> resolvedTypeArgNames;
    std::vector<NodePtr> arguments;
    mutable DispatchSignature resolvedDispatch; // see CallExpr::resolvedDispatch
    SuperMethodCallExpr() : AstNode(NodeKind::SuperMethodCallExpr) {}
};

// func(params) => expr        - single-expression body, `return` implied
// func(params) { statements } - block body, explicit `return` required
//   (consistent with named functions - see FunctionDecl)
//
// An anonymous func VALUE: can be assigned to a variable, passed as a
// call argument, or returned from a func - unlike FunctionDecl, which is
// only ever a top-level/method declaration, never usable in expression
// position. See ROADMAP.md Phase 7.
//
// Captures its enclosing scope BY VALUE at the moment it's evaluated (a
// snapshot of every local currently in scope, copied - not a live
// reference). A lambda's captured variables do NOT see later mutations of
// the originals; see Compiler::compileLambdaExpr / OpCode::MakeClosure for
// where the snapshot is actually taken, and value.hpp's ClosureBox for
// where it's stored.
//
// Params may be untyped (bare `x`, dynamically typed like most everything
// else pre-inference) or typed (`int x`) - same Param shape as
// FunctionDecl's params. There is no declared return-type annotation on a
// lambda itself (first-cut scope decision - see TypeChecker::inferLambdaExpr):
// the body's inferred/checked type is used as-is, permissively, the same way
// an unannotated `var` declaration's type comes from its initializer.
struct LambdaExpr : AstNode {
    std::vector<Param> params;
    bool isAsync{false};
    bool hasExprBody{false};  // true for `=> expr`; false for `{ block }`
    NodePtr exprBody;         // used when hasExprBody (implicit `return`)
    NodePtr blockBody;        // used when !hasExprBody, a BlockStmt (explicit `return` required)
    mutable std::vector<std::string> captureNames;
    mutable std::vector<std::string> captureStorageNames;
    mutable bool usesThis{false};
    // Type information inferred during semantic checking and copied into the
    // runtime closure for callable reflection.
    mutable std::vector<std::string> inferredParameterTypeNames;
    // The body result; isAsync wraps it in Task<T> at the call boundary.
    mutable std::string inferredReturnTypeName;
    // Optional `func(params): T => ...` annotation. When present the body's
    // inferred result must be assignable to it, so a lambda can state its
    // contract the same way a named func does.
    bool hasDeclaredReturnType{false};
    TypeAnnotation declaredReturnType;
    LambdaExpr() : AstNode(NodeKind::LambdaExpr) {}
};

// Calls `visit` on every child node directly owned by `node`, in a stable order.
// Leaf nodes simply visit nothing.
//
// This exists so that passes which have to scan a body for something - the MIR
// lowerer looking for lambdas to pre-declare, or for locals a body assigns to -
// have one exhaustive answer to "what is inside this node?". Hand-rolled
// switches over a handful of container kinds silently miss everything nested
// deeper: a walker that only descends into BlockStmt never sees the lambda in
// `Thread.start(func() => ...)`, because that one is inside a CallExpr.
//
// Declaration-level nodes (Program, ClassDecl, InterfaceDecl, DataDecl,
// EnumDecl, FunctionDecl) are treated as opaque: their members are separate
// compilation units for these purposes, and a pass that wants them walks
// Program::declarations itself.
//
// `visit` receives the child and decides whether to recurse; nothing here walks
// transitively. That keeps a pass in control of boundaries it cares about, such
// as not descending into a lambda body.
void forEachChild(const AstNode* node, const std::function<void(const AstNode*)>& visit);

} // namespace zl
