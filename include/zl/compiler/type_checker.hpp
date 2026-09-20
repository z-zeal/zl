#pragma once

#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zl/parser/ast.hpp"
#include "zl/parser/annotation_rules.hpp"
#include "zl/compiler/semantic_types.hpp"
#include "zl/compiler/inferred_type.hpp"

namespace zl {


} // namespace zl

#include "zl/compiler/native_catalog.hpp"
#include "zl/compiler/semantic_model.hpp"
#include "zl/compiler/generic_instantiation.hpp"
#include "zl/compiler/type_resolver.hpp"
#include "zl/common/dispatch_signature.hpp"
#include "zl/compiler/overload_resolver.hpp"
#include "zl/compiler/dispatch_table.hpp"

namespace zl {

// ---------------------------------------------------------------------------
// SymbolTable - tracks variable types and func signatures per scope.
// ---------------------------------------------------------------------------
// Lexical scopes resolve source names to unique storage names. Read
// refinements are scoped views; they never replace a binding's write contract.
// ---------------------------------------------------------------------------
class SymbolTable {
public:
    struct VarInfo {
        ZlType type{ZlType::UNKNOWN};
        bool isConst{false};
        OwnershipKind ownership{OwnershipKind::GC};
        std::string className; // full identity for objects, collections, tasks and unions
        std::vector<ZlType> functionParamTypes; // function value signature
        std::vector<std::string> functionParamClassNames;
        ZlType functionReturnType{ZlType::UNKNOWN};
        std::string functionReturnClassName;
        bool functionIsAsync{false};
        ZlType taskValueType{ZlType::UNKNOWN};
        std::string taskValueClassName;
        std::vector<std::string> functionCaptureNames;
        bool functionUsesThis{false};
        bool functionHasSignature{false};
        std::optional<int> fixedArraySize; // fixed array length when declared locally
        ZlType arrayElementType{ZlType::UNKNOWN};
        // Preserve the source contract for writes/expected-type inference.
        // Inferred bindings render their resolved metadata instead.
        std::optional<TypeAnnotation> annotation;
        std::string storageName;
        // A refinement is a read view of the same immutable storage contract.
        std::shared_ptr<const VarInfo> declaration;
        [[nodiscard]] std::string runtimeTypeName() const;
    };

    struct FuncInfo {
        std::string name;
        std::vector<ZlType> paramTypes;
        ZlType returnType;
    };

    void pushScope();
    void popScope();

    // Every lexical binding has a distinct bytecode name. Refinements keep
    // their original binding's identity instead of creating a runtime local.
    VarInfo& defineVar(const std::string& name, VarInfo info);
    VarInfo& defineVar(const std::string& name, ZlType type, bool isConst, const std::string& className = "",
                   const std::vector<ZlType>& functionParamTypes = {},
                   ZlType functionReturnType = ZlType::UNKNOWN,
                   ZlType taskValueType = ZlType::UNKNOWN,
                   const std::string& taskValueClassName = "",
                   std::optional<int> fixedArraySize = std::nullopt,
                   ZlType arrayElementType = ZlType::UNKNOWN,
                   const std::vector<std::string>& functionCaptureNames = {},
                   bool functionUsesThis = false,
                   bool functionHasSignature = false,
                   OwnershipKind ownership = OwnershipKind::GC);
    // Read snapshots survive recursive inference restoring a branch scope.
    [[nodiscard]] std::optional<VarInfo> lookupVar(const std::string& name) const;
    VarInfo& binding(const std::string& name);
    void setFunctionSignature(const std::string& name, std::vector<std::string> paramClassNames,
                              std::string returnClassName, bool isAsync, bool hasSignature = true);

    using ScopeState = std::vector<std::unordered_map<std::string, VarInfo>>;
    [[nodiscard]] ScopeState snapshot() const { return scopes_; }
    void restore(ScopeState state) { scopes_ = std::move(state); }
    const auto& currentScope() const { return scopes_.back(); }
    void refine(const std::string& name, VarInfo view);
    void invalidate(const std::string& name);
    void joinRefinements(ScopeState entry, const std::vector<ScopeState>& exits);

    void defineFunc(const FuncInfo& info);
    [[nodiscard]] std::optional<FuncInfo> lookupFunc(const std::string& name) const;

private:
    // Each element is one scope: name -> VarInfo.
    ScopeState scopes_;
    std::size_t nextStorageId_{0};

    // Functions are "global" within the compilation unit (forward-referenceable),
    // so they don't participate in scope push/pop.
    std::unordered_map<std::string, FuncInfo> functions_;
};

// ---------------------------------------------------------------------------
// TypeChecker - the main semantic analysis pass.
// ---------------------------------------------------------------------------
// Usage:
//     TypeChecker tc;
//     tc.check(program);   // throws TypeCheckError on the first violation
// ---------------------------------------------------------------------------
class TypeChecker {
    friend class OverloadResolver;
public:
    // The public, backend-consumable form of the checker's inference result.
    // Kept as a nested alias so every existing `TypeChecker::InferredType`
    // spelling keeps compiling while the definition itself lives in
    // zl/compiler/inferred_type.hpp, where MIR lowering can reach it.
    using InferredType = ::zl::InferredType;

    // Analyses the entire program. Throws TypeCheckError if any type rule is
    // violated; does nothing (returns normally) if the program is well-typed.
    void check(const Program& program, bool requireMain = true);

    // --- backend seam (MIR lowering) ---------------------------------------
    //
    // Semantic analysis already resolves a static type for every expression it
    // visits. Backend stages used to have no way to see that work, so each one
    // either re-derived types from the untyped AST or gave up on typing
    // altogether. The checker now records what it inferred, keyed by AST node
    // identity, and these accessors expose that record.
    //
    // These are read-only views over state that is only meaningful once
    // `check()` has run over the program that owns the queried nodes.

    // The inferred type recorded for `node`, or nullptr when semantic analysis
    // never inferred that node (unreachable code, or a node kind the checker
    // does not type). Never returns a dangling pointer: the record lives as
    // long as this TypeChecker does.
    [[nodiscard]] const InferredType* expressionType(const AstNode* node) const;

    // The full inference record: AST node -> resolved static type.
    [[nodiscard]] const std::unordered_map<const AstNode*, InferredType>& expressionTypes() const {
        return expressionTypes_;
    }

    // Declaration shapes (classes, interfaces, fields, methods, inheritance).
    [[nodiscard]] const SemanticModel& semanticModel() const { return semanticModel_; }

    // Members of a union type as rendered by the checker (e.g. "int|string"),
    // empty when `unionName` is not a union this checker resolved.
    [[nodiscard]] const std::vector<ResolvedTypeArg>& unionMembers(const std::string& unionName) const;

private:
    // --- declarations ---
    void registerClassShape(const ClassDecl* node); // pass 0: fields + method/constructor signatures, before ANY body is checked
    void checkClassDecl(const ClassDecl* node);
    // A `memory` declaration is registered like a class shape (fields +
    // contract-method signatures, so bodies typecheck) but is validated
    // against the domain contract first (docs/memory-domains.md §5.1): the
    // required acquire/release, the optional reset/exhausted/onCollect, each
    // public, each with its exact signature. Nothing consumes a domain yet,
    // so the shape exists only for checking - no code is emitted for it.
    void registerMemoryDeclaration(const MemoryDecl* node); // pass 0: contract validation + shape
    void checkMemoryDeclaration(const MemoryDecl* node);    // body pass: contract method bodies, like checkClassDecl's member walk
    void checkFunctionDecl(const FunctionDecl* node);
    void registerDataShape(const DataDecl* node); // pass 0: fields only - a data type has no methods/constructors/body to check
    void registerEnumShape(const EnumDecl* node);  // pass 0: members only

    // --- interfaces ---
    void registerInterfaceShape(const InterfaceDecl* node); // pass 0: own method signatures only (no merging yet)
    // Post-order merge of an interface's own methods with everything inherited
    // via `extends` (possibly from multiple parents - interfaces allow
    // multiple inheritance, unlike classes). Memoized via methodsResolved
    // since the extends graph is a DAG (one interface can be reached as an
    // ancestor via more than one path). If two distinct ancestors provide
    // different signatures for the same method name, and this interface
    // doesn't redeclare that name itself, that's a diamond conflict -
    // reported as an error at THIS interface's declaration, the earliest
    // point the conflict is actually knowable.
    // For each interface in node->implementsNames, verify every one of its
    // (fully merged) required methods is implemented somewhere in node's own
    // class hierarchy (via findMethodInChain) with a matching signature.
    // Collects every missing/mismatched method across every implemented
    // interface into a single error, rather than stopping at the first one.
    void checkImplements(const ClassDecl* node);

    // --- @Override / @Deprecated / @SuppressWarnings ---
    // Called for every non-constructor method during checkFunctionDecl.
    // Always walks the parent chain (starting one level up) looking for a
    // same-name method, regardless of whether @Override is present:
    //   - found, @Override present  -> validate signature matches
    //   - found, @Override absent   -> WARN (this silently overrides a
    //                                   parent method - almost certainly
    //                                   meant to be annotated)
    //   - not found, @Override present -> error (nothing to override)
    //   - not found, @Override absent  -> fine, an ordinary new method
    void checkOverrideAnnotation(const FunctionDecl* node);
    // The target half of the annotation registry (annotation_rules.hpp): the
    // parser rejects an unknown @name at the '@' itself; here - where the
    // declaration kind is fully known - a *known* name annotating a position
    // it does not target is an error ('@Override' on a class or constructor,
    // '@ffi' on a class). Unknown names are skipped, not re-reported.
    static void validateAnnotationTargets(const std::vector<Annotation>& annotations,
                                          annotations::Position position, const std::string& declName);
    [[nodiscard]] static bool hasAnnotation(const std::vector<Annotation>& annotations, const std::string& name);
    [[nodiscard]] static const Annotation* findAnnotation(const std::vector<Annotation>& annotations, const std::string& name);
    // Non-throwing diagnostic: prints "warning: ..." to stderr and returns,
    // unlike typeError (which throws and stops compilation).
    static void typeWarning(const std::string& message, std::size_t line);
    // True if a deprecation warning should be suppressed right now, i.e. the
    // class currently being checked or the specific method/constructor
    // currently being checked carries @SuppressWarnings("deprecation").
    [[nodiscard]] bool deprecationSuppressed() const;
    // Warns (unless suppressed) if `method` carries @Deprecated - called
    // from every call-resolution site (bare self-call, obj.method(), and
    // constructors via inferNewExpr) right after the ClassMethodInfo is
    // resolved.
    void warnIfDeprecatedMethod(const ClassMethodInfo& method, const std::string& ownerClassName,
                                 const std::string& methodName, std::size_t line);
    void warnIfDeprecatedClass(const std::string& className, std::size_t line);

    // --- statements ---
    void checkStatement(const AstNode* node);
    void checkBlock(const BlockStmt* node);
    void checkVarDecl(const VarDecl* node);
    void checkIfStmt(const IfStmt* node);
    void checkReturnStmt(const ReturnStmt* node);
    void checkForStmt(const ForStmt* node);
    void checkWhileStmt(const WhileStmt* node);
    void checkRepeatStmt(const RepeatStmt* node);
    void checkTryStmt(const TryStmt* node);
    void checkThrowStmt(const ThrowStmt* node);
    void checkExprStmt(const ExprStmt* node);
    void checkLogStmt(const LogStmt* node);


    // --- expressions: returns the inferred type and its object identity ---
    // inferExpr records the result in expressionTypes_ for the backend seam;
    // inferExprInner is the actual per-node dispatch and must not be called
    // directly by anything that wants the result to be visible downstream.
    [[nodiscard]] InferredType inferExpr(const AstNode* node);
    [[nodiscard]] InferredType inferExprInner(const AstNode* node);
    [[nodiscard]] InferredType inferExpected(const AstNode* node, ZlType type,
                                             const std::string& className, const TypeAnnotation& annotation);
    void analyzeLambdaCaptures(LambdaExpr* node);
    void validateThreadLambda(const LambdaExpr* node, const char* apiName);

    // Named static function confinement is checked interprocedurally at a
    // thread/task boundary. A named function has no closure capture, but it
    // can still reach process-shared mutable state through static fields or
    // another named static function that does so.
    struct NamedFunctionConfinementState {
        enum class Kind { UNKNOWN, CHECKING, SAFE, UNSAFE };
        Kind kind{Kind::UNKNOWN};
        std::string reason;
    };
    void indexNamedFunctions(const Program& program);
    [[nodiscard]] std::string namedFunctionKey(const FunctionDecl* node) const;
    [[nodiscard]] const FunctionDecl* findNamedFunction(const std::string& ownerClassName,
                                                         const DispatchSignature& signature) const;
    [[nodiscard]] bool isExplicitlySharedClass(const std::string& className) const;
    [[nodiscard]] bool checkNamedFunctionConfinement(const FunctionDecl* node, std::string& reason);
    [[nodiscard]] bool checkNamedFunctionConfinementNode(const AstNode* node,
                                                          const FunctionDecl* owner,
                                                          std::unordered_set<std::string>& locals,
                                                          std::string& reason);
    [[nodiscard]] InferredType inferLiteral(const Literal* node);
    [[nodiscard]] InferredType inferIdentifier(const Identifier* node);
    [[nodiscard]] InferredType inferUnary(const UnaryExpr* node);
    [[nodiscard]] InferredType inferAwait(const AwaitExpr* node);
    [[nodiscard]] InferredType inferBinary(const BinaryExpr* node);
    [[nodiscard]] InferredType inferCall(const CallExpr* node);
    [[nodiscard]] InferredType inferAssign(const AssignExpr* node);
    [[nodiscard]] InferredType inferMove(const MoveExpr* node);
    [[nodiscard]] InferredType inferCollectionLiteral(const CollectionLiteral* node);
    [[nodiscard]] ZlType inferCollectionLiteralExpected(const CollectionLiteral* node,
                                                        ZlType expectedType,
                                                        const std::string& expectedClassName,
                                                        const TypeAnnotation& expectedAnnotation);
    void validateCollectionElement(const AstNode* expr, ZlType expected, const std::string& expectedClassName,
                                   const std::string& context, std::size_t line);
    [[nodiscard]] InferredType inferNewExpr(const NewExpr* node);
    [[nodiscard]] InferredType inferDataLiteral(const DataLiteralExpr* node);
    [[nodiscard]] InferredType inferDataUpdate(const DataUpdateExpr* node);
    [[nodiscard]] InferredType inferFieldAccess(const FieldAccessExpr* node);
    [[nodiscard]] InferredType inferIndexAccess(const IndexAccessExpr* node);
    [[nodiscard]] InferredType inferFieldAssign(const FieldAssignExpr* node);
    [[nodiscard]] InferredType inferMethodCall(const MethodCallExpr* node);
    [[nodiscard]] InferredType inferThisExpr(const ThisExpr* node);
    [[nodiscard]] InferredType inferSuperCallExpr(const SuperCallExpr* node);
    [[nodiscard]] InferredType inferSuperMethodCallExpr(const SuperMethodCallExpr* node);
    [[nodiscard]] InferredType inferLambdaExpr(const LambdaExpr* node);
    [[nodiscard]] InferredType inferMatchExpr(MatchExpr* node);
    void validateFunctionTypeAssignment(const TypeAnnotation& expected, const InferredType& actual, std::size_t line);

    struct FunctionExpectation {
        std::vector<ZlType> paramTypes;
        std::vector<std::string> paramClassNames;
        ZlType returnType{ZlType::UNKNOWN};
        std::string returnClassName;
        // For an async lambda whose contextual callable return is Task<T>,
        // the lambda body itself returns T rather than a Task value.
        ZlType taskValueType{ZlType::UNKNOWN};
        std::string taskValueClassName;
        bool active{false};
        bool hasSignature{false};
    };

    struct InferredArguments {
        std::vector<ZlType> types;
        std::vector<std::string> classNames;
        std::vector<std::vector<ZlType>> functionParamTypes;
        std::vector<std::vector<std::string>> functionParamClassNames;
        std::vector<ZlType> functionReturnTypes;
        std::vector<std::string> functionReturnClassNames;
        // True when the argument carries a callable signature of its own: a
        // `func(int): bool` annotation, or a lambda whose parameters are typed.
        // A bare `func` value reports false and stays exempt from the call-site
        // signature check - that is the compatibility escape hatch bare `func`
        // relies on.
        std::vector<bool> functionHasSignature;
    };

    // Expected-type propagation into call arguments.
    //
    // An empty collection literal takes its container kind from its
    // declaration - `map<string,int> m = {}` - but in argument position there
    // is no declaration to read, and arguments are inferred *before* overload
    // resolution picks a parameter to offer. So `countEntries({})` against
    // `countEntries(map<string,int>)` had nothing to declare it and failed
    // with "no overload matches".
    //
    // This is the missing half of that plumbing: the call's own candidates say
    // what each position wants. Where every arity-matching candidate agrees on
    // a container type, the literal is inferred against it; where they
    // disagree there is no single answer, the position is left alone, and the
    // literal keeps today's spelling-based inference. Non-empty literals never
    // participate - `{1, 2}` is how a variadic argument list is written, and
    // re-reading it as a set would be wrong.
    struct ArgumentExpectation {
        ZlType type{ZlType::UNKNOWN};
        std::string className;
        TypeAnnotation annotation;
        bool seen{false};    // at least one candidate described this position
        bool usable{false};  // every candidate described it, as the same container
    };

    // One expectation per argument position, from the candidates' parameter
    // types. `candidates` are the already-substituted overloads of the call.
    [[nodiscard]] std::vector<ArgumentExpectation> argumentExpectations(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        std::size_t argumentCount) const;
    // The same, for a single known parameter type - the func-value call path
    // has one signature rather than a candidate set.
    [[nodiscard]] ArgumentExpectation argumentExpectation(ZlType type, const std::string& className) const;

    [[nodiscard]] InferredArguments inferArguments(const std::vector<NodePtr>& arguments);
    [[nodiscard]] InferredArguments inferArguments(const std::vector<NodePtr>& arguments,
                                                   const std::vector<ArgumentExpectation>& expectations);

    // Call-site half of the same contract. A parameter declared
    // `func(int, string): bool` is checked against the callable actually
    // passed - arity, parameter types, return type - at compile time, on every
    // call path, instead of surfacing as a VM argument-count mismatch or a
    // runtime type assertion. Parameters declared bare `func`, and arguments
    // that carry no signature of their own (a bare `func` value forwarded
    // through), are skipped: that is the compatibility escape hatch.
    void validateFunctionArguments(const std::string& calleeName, const ClassMethodInfo& method,
                                   const InferredArguments& args, std::size_t line);

    // --- helpers ---

    // Converts a parsed TypeAnnotation (from the AST) into a ZlType tag. If
    // the annotation names a known class, returns OBJECT and fills
    // outClassName (when non-null) with that class's name.
    // Resolves a TypeAnnotation to a ZlType (+ out-of-band class name for
    // OBJECT). No longer const (Phase 3: generics) - resolving a generic
    // instantiation like Box<int> may need to build and cache a new
    // synthetic ClassShapeInfo in classRegistry_ (see instantiateGenericClass),
    // which mutates state. Also consults currentClassTypeParams_: inside a
    // generic class's own body, a bare reference to one of ITS OWN type
    // parameter names (e.g. `T` inside `class Box<T>`) resolves to
    // (OBJECT, "T") - an unsubstituted placeholder, later replaced by
    // instantiateGenericClass wherever a concrete instantiation needs it.
    [[nodiscard]] ZlType resolveType(const TypeAnnotation& annotation, std::string* outClassName = nullptr);
    void validateOwnership(const TypeAnnotation& annotation, OwnershipKind ownership, std::size_t line);
    [[nodiscard]] SymbolTable::VarInfo variableInfo(const TypeAnnotation& annotation);

    // True when `from` can be used where `to` is expected. This handles
    // UNKNOWN (always compatible), NIL (compatible with reference types), and
    // int -> double numeric widening for expressions (not assignments).
    // OBJECT additionally requires matching class names.
    [[nodiscard]] bool isAssignable(ZlType from, ZlType to, const std::string& fromClassName = "",
                                     const std::string& toClassName = "") const;

    // The standard library's two sum types are closed: `Option<T>` is exactly
    // `Some<T> | None<T>` and `Result<T,E>` is exactly `Ok<T,E> | Err<T,E>`.
    // A match that covers every case covers the subject, so `match` reasons
    // about these subjects as their case set rather than as their parent class
    // (which no single case pattern can cover). Every other hierarchy stays
    // open: a subclass arm never makes its parent look exhaustive, because a
    // third subclass could still appear. Returns the concrete case types for a
    // parameterized subject (`Result<int,string>` -> `Ok<int,string>`,
    // `Err<int,string>`), or empty when the class is not one of the two
    // built-in sums or a registered subclass has made it open again.
    [[nodiscard]] std::vector<ResolvedTypeArg> builtinSumCases(const std::string& subjectClassName) const;

    // True when a type pattern naming the resolved class `patternClass` matches
    // every value of `member`. Assignability is the general rule; on top of it,
    // a pattern naming an ancestor class whose arguments agree covers the case:
    // `Result<int,string>` covers `Ok<int,string>`, which is exactly what the
    // runtime's base-class walk accepts (reflectiveObjectMatches).
    [[nodiscard]] bool typePatternCovers(const ResolvedTypeArg& member, ZlType patternType,
                                          const std::string& patternClass) const;

    // `Ok v` on a `Result<int,string>` subject: a generic type pattern may omit
    // the arguments the subject already fixes. Writes the concrete arguments
    // into `pattern` when every one of the class's parameters can be read off
    // the subject's instantiation, and leaves it untouched otherwise (the
    // resolver then reports the missing arguments exactly as it does today).
    void fillTypePatternArgs(TypeAnnotation& pattern, const std::string& subjectClassName) const;

    // Emits a TypeCheckError with a formatted message including the line number.
    [[noreturn]] static void typeError(const std::string& message, std::size_t line);

    // Looks up `memberName` (a field or method) on class `className` and
    // verifies it's accessible from `currentClassName_` per Java-style rules:
    // PUBLIC is always fine; PRIVATE/DEFAULT require currentClassName_ ==
    // className; PROTECTED additionally allows access from any subclass of
    // className (see isSubclassOf). Throws on violation or if the member
    // doesn't exist at all.
    void checkAccess(const std::string& className, const std::string& memberName,
                      AccessModifier access, bool isMethod, std::size_t line) const;

    // --- method overload resolution (Phase 2) ---
    //
    // A method call now resolves in two steps: (1) collect every VISIBLE
    // overload of a name (walking the chain, merging by signature so a
    // subclass overriding only ONE overload doesn't hide its siblings -
    // see SemanticModel::collectMethodOverloads), then (2) pick the best-matching one for
    // this specific call's argument types (resolveOverload). Splitting it
    // this way is what makes "override one overload, inherit the rest"
    // work correctly.

    // Builds the typed dispatch identity for a resolved overload. The compiler
    // consumes this exact value when assigning a vtable slot, so overload
    // selection and code generation share a structured contract instead of
    // independently rebuilding a string suffix.
    [[nodiscard]] static std::string paramTypesSuffix(const std::vector<ZlType>& paramTypes);
    [[nodiscard]] static std::string methodParamSignature(const ClassMethodInfo& method);

    // --- generics (Phase 3) ---
    //
    // Scope, deliberately: FIELD types and RETURN types are fully
    // substituted at each concrete instantiation and strictly checked
    // (a Box<int>'s get() really does return int, checked against int).
    // Method/constructor PARAMETER types declared as a bare type-parameter
    // name (e.g. `value: T`) ARE substituted for argument-compatibility
    // checking (so `new Box<int>(1)` correctly accepts an int) but their
    // contribution to a call site's dispatch suffix always stays "object"
    // regardless (see ClassMethodInfo::paramIsGeneric / dispatchSignature) -
    // this sidesteps a much larger change that would otherwise be needed to
    // keep the compiler's independently-rebuilt suffix in sync.
    //
    // Direct generic inheritance (`class SpecialContainer<T> extends
    // Container<T>`) IS substituted through correctly - instantiating
    // SpecialContainer<int> also instantiates Container<int> as its parent,
    // recursively (see the parentTypeArgs handling in
    // instantiateGenericClass). What's NOT substituted: a generic class's
    // OWN field typed as some OTHER generic class parameterized by ITS OWN
    // type parameter (e.g. `class Pair<A,B> { Box<A> boxedA }`) - a
    // narrower, still-real limitation, not a silent crash risk (best-effort:
    // produces an under-substituted but still valid shape).

    // Builds (or returns the already-cached) synthetic ClassShapeInfo for a
    // concrete instantiation of a generic class, e.g. genericName="Box",
    // typeArgs=[(INT,"")] -> "Box<int>", with every field/return-type
    // (and, for arg-compatibility purposes, every parameter type) that
    // referenced one of Box's own typeParams replaced with the
    // corresponding concrete type. Registers the synthetic shape in
    // classRegistry_ under the returned key (idempotent - calling this
    // again with the same genericName+typeArgs returns the same key without
    // rebuilding). typeArgs.size() must equal the generic class's own
    // typeParams.size() - throws a type error at `line` otherwise.
    [[nodiscard]] std::string instantiateGenericClass(const std::string& genericName,
                                                        const std::vector<ResolvedTypeArg>& typeArgs,
                                                        std::size_t line);

    // The dispatch-relevant signature suffix for a method/constructor -
    // identical to paramTypesSuffix(method.paramTypes) UNLESS a parameter
    // is marked paramIsGeneric, in which case that position always
    // contributes "object" regardless of what it's currently substituted
    // to. For a non-generic class (paramIsGeneric all false, or empty),
    // this is byte-for-byte identical to paramTypesSuffix - Phase 2's
    // existing behavior is unaffected. This is what gets written to
    // CallExpr/NewExpr/etc.'s resolvedDispatch; paramTypesSuffix (showing the
    // real substituted types) is still what error messages use, since it's
    // more informative for a person reading them.
    [[nodiscard]] static DispatchSignature dispatchSignature(const std::string& methodName, const ClassMethodInfo& method);
    // Instantiates a method-level generic (`firstOf<T>`) by substituting the
    // given type arguments into parameter and return types. The result is used
    // only for overload matching and result typing; dispatch still uses the
    // unsubstituted method so the template slot is GENERIC_OBJECT.
    [[nodiscard]] ClassMethodInfo instantiateGenericMethod(const ClassMethodInfo& method,
                                                            const std::vector<ResolvedTypeArg>& typeArgs,
                                                            std::size_t line);
    // Instantiates generic overloads (when type arguments are written) or
    // filters them out (when they are omitted: no inference). `originals` is
    // parallel to `candidates` and points at the unsubstituted methods so
    // dispatch still uses GENERIC_OBJECT.
    struct PreparedGenericOverloads {
        std::vector<std::pair<std::string, ClassMethodInfo>> candidates;
        std::vector<const ClassMethodInfo*> originals;
        std::vector<std::string> resolvedTypeArgNames;
    };
    [[nodiscard]] PreparedGenericOverloads prepareGenericOverloads(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<TypeAnnotation>& typeArgs,
        const std::string& methodName,
        std::size_t line);
    // Rejects a method type parameter that collides with a class type
    // parameter, then appends the method's parameters to the current scope.
    void mergeMethodTypeParams(const std::vector<std::string>& methodParams, std::size_t line);
    [[nodiscard]] bool isCurrentGenericTypeParam(const std::string& name) const;
    void requireNumericGenericTypeParam(const std::string& name, std::size_t line);

    // Walks className's `extends` chain merging every overload of
    // methodName by signature (paramTypes) - the most-derived class's
    // declaration of a given signature wins (that's what overriding means),
    // but a signature the most-derived class does NOT redeclare still comes
    // through from whichever ancestor originally declared it. Each result
    // is tagged with the name of the class that actually owns that specific
    // overload (needed by checkAccess/warnIfDeprecatedMethod, since those
    // are per-declaration, not per-name).

    // Picks the best-matching candidate for this call's actual argument
    // types out of `candidates` (each already tagged with its owning class
    // name - see SemanticModel::collectMethodOverloads). Prefers an EXACT type match on
    // every parameter; if none, falls back to the same isAssignable
    // widening used elsewhere (e.g. int -> double) - if exactly one
    // candidate matches (at either tier), that's the answer. Zero matches or
    // more than one ambiguous match at the same tier is a compile error
    // listing every candidate's signature. On success, also sets *outOwner
    // to the winning candidate's owning class.
    [[nodiscard]] const ClassMethodInfo* resolveOverload(
        const std::vector<std::pair<std::string, ClassMethodInfo>>& candidates,
        const std::vector<ZlType>& argTypes, const std::vector<std::string>& argClassNames,
        const std::string& methodName, std::size_t line, std::string* outOwner) const;

    // True if className is ancestorName itself, or has ancestorName
    // somewhere in its `extends` chain. Used to grant PROTECTED access from
    // subclasses.
    [[nodiscard]] bool isSubclassOf(const std::string& className, const std::string& ancestorName) const;

    SymbolTable symbols_;
    SemanticModel semanticModel_;

    // Every static type inferred during `check()`, keyed by AST node identity.
    // This is the single inference record shared with backend stages (see the
    // backend seam above); it is append-only during a check and never consulted
    // by the checker itself.
    std::unordered_map<const AstNode*, InferredType> expressionTypes_;

    // Source-level named functions indexed before body checking so a
    // boundary call can recursively inspect a function declared later (or
    // in an imported class) without depending on declaration order.
    std::unordered_map<std::string, const FunctionDecl*> namedFunctions_;
    std::unordered_map<std::string, NamedFunctionConfinementState> namedFunctionConfinement_;
    struct NamedFunctionBinding {
        std::string ownerClassName;
        DispatchSignature dispatch;
    };
    std::unordered_map<std::string, NamedFunctionBinding> namedFunctionBindings_;
    TypeResolver typeResolver_{semanticModel_};

    // The type-parameter names (e.g. ["T"], ["A","B"]) of whichever generic
    // class's own members are CURRENTLY being registered/resolved (empty
    // otherwise, including while checking a non-generic class). Consulted
    // by resolveType so a bare reference to one of these names inside the
    // class's own body resolves to an unsubstituted placeholder rather than
    // an unknown-type fallback. Set/cleared by registerClassShape around
    // each class's own member-registration loop - never needs to nest,
    // since class declarations can't nest in this language.
    // Structured generic-instantiation identity. The string class name remains
    // the compatibility key used by SemanticModel, while this cache keeps
    // the actual generic identity as a value instead of requiring callers to
    // parse formatted names back into type arguments.
    std::vector<std::string> currentClassTypeParams_;

    // Set while checking a class whose own ClassDecl carries
    // @SuppressWarnings("deprecation") / a func whose own FunctionDecl
    // does - either one suppresses deprecation warnings raised by anything
    // resolved from inside that class's members / that func's body
    // (see deprecationSuppressed()).
    bool classSuppressesDeprecation_{false};
    bool functionSuppressesDeprecation_{false};

    // The return type of the func currently being checked - used by
    // checkReturnStmt to validate the returned expression's type.
    // Flow-sensitive ownership state for the current function.
    struct OwnershipFlowState {
        std::unordered_set<std::string> moved;
        std::unordered_map<std::string, std::string> borrows;
        std::unordered_map<std::string, std::string> sharedPayloadOwners;
        std::unordered_map<std::string, std::string> sharedCellAliases;
        std::unordered_map<std::string, std::string> lockAliases;
    };
    [[nodiscard]] OwnershipFlowState captureOwnershipState() const;
    void restoreOwnershipState(const OwnershipFlowState& state);
    [[nodiscard]] OwnershipFlowState joinOwnershipStates(const std::vector<OwnershipFlowState>& states) const;
    // Returns a stable region path for a borrowable field expression, e.g.
    // `owner.field` or `owner.inner.field`. The root must ultimately be an
    // owned local/parameter (possibly through existing borrows); GC-owned
    // roots intentionally do not acquire region identities.
    [[nodiscard]] std::optional<std::string> borrowRegionForExpr(const AstNode* node) const;
    [[nodiscard]] std::optional<std::string> borrowRootOwner(const std::string& name) const;
    [[nodiscard]] static bool regionOverlaps(const std::string& a, const std::string& b);
    [[nodiscard]] static std::optional<std::string> constantIndexRegion(const AstNode* node);

    std::unordered_set<std::string> movedVariables_;
    std::unordered_map<std::string, std::string> borrowSources_;
    std::unordered_map<std::string, std::string> lockAliases_;

    ZlType currentReturnType_{ZlType::VOID_TYPE};
    std::string currentReturnClassName_;
    std::string currentFunctionName_;

    // True while checking the body of a constructor (used to validate
    // super(...) calls: only legal inside a constructor, and only as its
    // literal first statement).
    bool inConstructor_{false};
    bool currentFunctionIsStatic_{false};
    bool currentFunctionIsAsync_{false};
    // >0 while type-checking a callback executed under a Mutex/RwLock.
    // Lock capabilities are lexical: suspension or transfer while held is
    // rejected because the lock cannot safely span an await or execution-context boundary.
    unsigned heldLockDepth_{0};
    std::vector<std::string> heldLockNames_;
    std::unordered_map<std::string, std::string> sharedPayloadOwners_;
    // Maps local aliases of Shared cells to one canonical named cell identity.
    // This is intentionally flow-local; branch-aware merging can refine it later.
    std::unordered_map<std::string, std::string> sharedCellAliases_;
    std::vector<std::string> heldSharedLocks_;
    std::unordered_map<std::string, std::size_t> lockOrderRanks_;
    FunctionExpectation currentLambdaExpectation_;
    ZlType lastFunctionReturnType_{ZlType::UNKNOWN};
    std::string lastFunctionReturnClassName_;
    bool lastFunctionHadReturn_{false};
    std::vector<ZlType> currentReturnFunctionParamTypes_;
    std::vector<std::string> currentReturnFunctionParamClassNames_;
    ZlType currentReturnFunctionReturnType_{ZlType::UNKNOWN};
    std::string currentReturnFunctionReturnClassName_;
    bool currentReturnFunctionHasSignature_{false};


    // Set (only) immediately before checking a constructor's structurally-
    // first statement, when that statement is itself a direct `super(...)`
    // call - see checkFunctionDecl. inferSuperCallExpr consumes this
    // (reading it, then immediately clearing it) to decide whether THIS
    // particular super(...) occurrence is in the one legal position; every
    // other occurrence - nested inside another expression, or at any other
    // statement index - sees it as false and is rejected.
    bool allowSuperCallExprHere_{false};

    // Which class's member is currently being checked (empty when not inside
    // any class member) - used by checkAccess to decide whether a
    // private/protected/default member access is happening from "inside".
    std::string currentClassName_;

    // Side channel for direct lambda inference. A FUNCTION value has a
    // runtime closure object, but the checker can preserve the lambda's
    // parameter/return signature when it is immediately assigned to a local.
    std::vector<ZlType> lastFunctionParamTypes_;
};

} // namespace zl