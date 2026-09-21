#include "zl/mir/lowering.hpp"

#include <algorithm>
#include <sstream>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "zl/parser/type_annotation.hpp"
#include "zl/common/type_name.hpp"
#include "zl/compiler/dispatch_table.hpp"
#include "zl/compiler/native_catalog.hpp"
#include "zl/vm/native.hpp"
#include "zl/compiler/operator_rules.hpp"
#include "zl/compiler/semantic_types.hpp"
#include "zl/lexer/token.hpp"
#include "zl/mir/builder.hpp"

namespace zl::mir {
namespace {

// ---------------------------------------------------------------------------
// Type conversion
// ---------------------------------------------------------------------------
//
// MIR types are built from the *structured* type-name parse (`zl::TypeName`),
// not by re-scanning rendered strings. The rendered form still matters, because
// that is how the type checker records a collection or generic identity, but it
// is parsed once by the language's own grammar so `List<int>` and `list<int>`
// are distinguished exactly as the checker distinguishes them.

struct TypeConverter {
    const TypeArena& arena;
    // Type parameters in scope for the function being lowered. A bare reference
    // to one of these names is a TypeParam, not a class.
    const std::vector<std::string>& typeParams;

    [[nodiscard]] TypeId fromName(const zl::TypeName& name) const {
        if (!name.unionMembers.empty()) {
            std::vector<TypeId> members;
            members.reserve(name.unionMembers.size());
            for (const auto& member : name.unionMembers) members.push_back(fromName(member));
            return arena.unionType(std::move(members));
        }
        const std::string& base = name.name;
        std::vector<TypeId> arguments;
        arguments.reserve(name.args.size());
        for (const auto& argument : name.args) arguments.push_back(fromName(argument));

        if (base == "int") return arena.intType();
        if (base == "double" || base == "float" || base == "decimal") return arena.doubleType();
        if (base == "bool") return arena.boolType();
        if (base == "string") return arena.stringType();
        if (base == "void") return arena.voidType();
        if (base == "nil" || base == "null") return arena.nilType();
        if (base == "unknown") return arena.unknownType();
        if (base == "list") return arena.listType(arguments.empty() ? arena.unknownType() : arguments.front());
        if (base == "set") return arena.setType(arguments.empty() ? arena.unknownType() : arguments.front());
        if (base == "map") {
            return arena.mapType(arguments.size() > 0 ? arguments[0] : arena.unknownType(),
                                 arguments.size() > 1 ? arguments[1] : arena.unknownType());
        }
        if (base == "array") {
            return arena.arrayType(arguments.empty() ? arena.unknownType() : arguments.front(),
                                   name.fixedSize);
        }
        if (base == "Task") {
            TypeId payload = arguments.empty() ? arena.voidType() : arguments.front();
            if (const Type* payloadType = arena.find(payload)) {
                if (payloadType->kind == TypeKind::Nil) payload = arena.voidType();
            }
            return arena.taskType(payload);
        }
        if (base == "Shared") return arena.sharedType(arguments.empty() ? arena.unknownType() : arguments.front());
        // The concurrency primitives are semantic kinds, not plain classes: a
        // backend reading a `channel_send` operand must see "a channel" without
        // string-matching the name, and the verifier's confinement and
        // synchronisation rules key on these kinds (accepting the plain class
        // spelling wherever it still appears). Channels are currently untyped,
        // so a bare `Channel` carries an unknown element.
        if (base == "Thread") return arena.threadType();
        if (base == "Channel") {
            return arena.channelType(arguments.empty() ? arena.unknownType() : arguments.front());
        }
        if (base == "Mutex") return arena.mutexType();
        if (base == "RwLock") return arena.rwLockType();
        if (base == "Atomic") return arena.atomicType();
        if (base == "Semaphore") return arena.semaphoreType();
        if (base == "Condition") return arena.conditionType();
        if (base == "NativeHandle") return arena.nativeHandleType();
        // The builtin sums are semantic kinds, not plain classes: a backend
        // reading `Option<List<int>>` must see "a List<int> or nothing"
        // without string-matching the name. `Some`/`None`/`Ok`/`Err` stay
        // Object kinds - they are the runtime classes - and relate to these
        // through isOptionType/isResultType and the assignability rule.
        if (base == "Option") {
            return arena.optionType(arguments.empty() ? arena.unknownType() : arguments.front());
        }
        if (base == "Result") {
            return arena.resultType(arguments.size() > 0 ? arguments[0] : arena.unknownType(),
                                    arguments.size() > 1 ? arguments[1] : arena.unknownType());
        }
        if (base == "func") {
            FunctionSignature signature;
            // A bare `func` has no signature. Recording it as `func():void`
            // would invent an arity the source never stated and let a backend
            // reject a call the type checker accepted.
            if (arguments.empty()) {
                signature.hasSignature = false;
                signature.returnType = arena.unknownType();
                return arena.functionType(std::move(signature));
            }
            // `func(A,B):R` parses with the return type as the last argument.
            for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
                signature.parameterTypes.push_back(arguments[i]);
            }
            signature.returnType = arguments.back();
            return arena.functionType(std::move(signature));
        }
        if (std::find(typeParams.begin(), typeParams.end(), base) != typeParams.end()) {
            return arena.typeParam(base);
        }
        return arena.objectType(base, std::move(arguments));
    }

    [[nodiscard]] TypeId fromRendered(const std::string& rendered) const {
        if (rendered.empty()) return arena.unknownType();
        return fromName(zl::parseTypeName(rendered));
    }

    [[nodiscard]] TypeId fromAnnotation(const zl::TypeAnnotation& annotation) const {
        if (annotation.name.empty() && annotation.unionOf.empty()) return arena.voidType();
        return fromRendered(zl::describeTypeAnnotation(annotation));
    }

    // Converts a checker inference result. `className` carries the identity that
    // the coarse ZlType tag erases, so it is authoritative whenever present.
    [[nodiscard]] TypeId fromInferred(const zl::InferredType* inferred) const {
        if (!inferred) return arena.unknownType();
        switch (inferred->type) {
            case zl::ZlType::INT: return arena.intType();
            case zl::ZlType::DOUBLE: return arena.doubleType();
            case zl::ZlType::BOOL: return arena.boolType();
            case zl::ZlType::STRING: return arena.stringType();
            case zl::ZlType::NIL: return arena.nilType();
            case zl::ZlType::VOID_TYPE: return arena.voidType();
            case zl::ZlType::UNKNOWN: return arena.unknownType();
            case zl::ZlType::FUNCTION:
                // Prefer the preserved callable shape over the rendered name so
                // an unparameterised `func` does not lose its signature.
                if (inferred->functionHasSignature) {
                    FunctionSignature signature;
                    signature.hasSignature = true;
                    for (std::size_t i = 0; i < inferred->functionParamTypes.size(); ++i) {
                        const std::string rendered = i < inferred->functionParamClassNames.size()
                                                         ? inferred->functionParamClassNames[i]
                                                         : std::string{};
                        signature.parameterTypes.push_back(
                            rendered.empty() ? primitiveFor(inferred->functionParamTypes[i])
                                             : fromRendered(rendered));
                    }
                    const std::string returnRendered = inferred->functionReturnClassName;
                    // A signature that carries `isAsync` states the Task at the
                    // call boundary (verifier: expectedResult = taskType of the
                    // signature's return), so the signature itself must record
                    // the *body* result type, exactly like an async MIR function
                    // declares its body type and the callee flag adds Task<T>.
                    // The checker renders an async callable's return as the full
                    // Task<...> name, so decode the body type it stored beside it
                    // rather than re-wrapping what would already be a Task.
                    if (inferred->functionIsAsync) {
                        signature.returnType = inferred->taskValueClassName.empty()
                                                   ? primitiveFor(inferred->taskValueType)
                                                   : fromRendered(inferred->taskValueClassName);
                        if (inferred->taskValueType == zl::ZlType::NIL)
                            signature.returnType = arena.voidType();
                    } else {
                        signature.returnType = returnRendered.empty()
                                                   ? primitiveFor(inferred->functionReturnType)
                                                   : fromRendered(returnRendered);
                    }
                    signature.isAsync = inferred->functionIsAsync;
                    return arena.functionType(std::move(signature));
                }
                if (inferred->className.empty()) {
                    FunctionSignature signature;
                    signature.hasSignature = false;
                    signature.returnType = arena.unknownType();
                    return arena.functionType(std::move(signature));
                }
                return fromRendered(inferred->className);
            default:
                // LIST/MAP/SET/ARRAY/OBJECT/TASK/UNION all keep their identity in
                // className as a rendered type name.
                if (!inferred->className.empty()) return fromRendered(inferred->className);
                return primitiveFor(inferred->type);
        }
    }

    [[nodiscard]] TypeId primitiveFor(zl::ZlType type) const {
        switch (type) {
            case zl::ZlType::INT: return arena.intType();
            case zl::ZlType::DOUBLE: return arena.doubleType();
            case zl::ZlType::BOOL: return arena.boolType();
            case zl::ZlType::STRING: return arena.stringType();
            case zl::ZlType::NIL: return arena.nilType();
            case zl::ZlType::VOID_TYPE: return arena.voidType();
            case zl::ZlType::TASK: return arena.taskType(arena.voidType());
            default: return arena.unknownType();
        }
    }
};

// ---------------------------------------------------------------------------
// Lowering context
// ---------------------------------------------------------------------------

const std::vector<std::string> kNoTypeParams{};

struct LoweringContext {
    const zl::Program& program;
    const zl::TypeChecker& checker;
    const LoweringOptions& options;
    ModuleBuilder builder;
    LoweringResult result;

    // Qualified function name -> MIR id.
    std::unordered_map<std::string, FunctionId> functionIds;
    // Lambda body -> the MIR function that holds it.
    std::unordered_map<const zl::LambdaExpr*, FunctionId> lambdaIds;
    // The same lambdas in discovery order. `lambdaIds` is a hash map, so its
    // iteration order is arbitrary - and the passes that walk it need a real
    // order: an enclosing lambda must be handled before the lambdas nested
    // inside it, because the outer body is what fills in an inner closure's
    // capture types.
    std::vector<const zl::LambdaExpr*> lambdaOrder;
    // The generic parameters in scope where each lambda was written. A lambda
    // inside `class Box<T>` closes over values of type `T`, so its MIR function
    // is generic over `T` too - without this the closure's captured parameters
    // would carry an unsubstituted type parameter in a function that is not a
    // template, which the verifier rightly rejects.
    std::unordered_map<const zl::LambdaExpr*, std::vector<std::string>> lambdaTypeParams;
    std::unordered_map<std::string, std::vector<std::string>> classTypeParams;
    std::unordered_map<std::string, std::string> classParents;
    std::size_t lambdaCounter{0};
    const zl::FunctionDecl* mainFunction{nullptr};

    LoweringContext(const zl::Program& p, const zl::TypeChecker& c, const LoweringOptions& o)
        : program(p), checker(c), options(o), builder("zl") {}

    [[nodiscard]] std::vector<std::string> typeParamsFor(const std::string& className) const {
        const auto it = classTypeParams.find(className);
        return it == classTypeParams.end() ? std::vector<std::string>{} : it->second;
    }

    void note(const std::string& message) { result.diagnostics.push_back(message); }

    [[nodiscard]] std::string qualifiedName(const zl::FunctionDecl& function) const {
        const auto signature = zl::dispatchSignatureForFunction(function, typeParamsFor(function.ownerClassName));
        return function.ownerClassName.empty() ? signature.describe()
                                               : function.ownerClassName + "." + signature.describe();
    }
};

// ---------------------------------------------------------------------------
// Per-function lowering
// ---------------------------------------------------------------------------

struct LoopContext {
    BlockId continueTarget{kNoBlock};
    BlockId breakTarget{kNoBlock};
    // Number of active finally blocks when the loop was entered. A break or
    // continue from inside the loop runs the finalizers opened after loop
    // entry (innermost first) before leaving, exactly as the reference does.
    std::size_t finallyDepth{0};
    std::size_t scopeDepth{0};
};

// A name in scope resolves either to a mutable slot or directly to a parameter.
//
// A parameter that the body never reassigns stays an SSA value: promoting every
// parameter to a slot would add a store and a load around values that are
// already immutable, and - worse - would need the slot to be writable, which
// contradicts `this` and every `let`-like binding.
// How a name in the current scope resolves. Exactly one of the three kinds is
// set: a mutable local is a slot; a read-only binding is an SSA value, held
// either as the parameter it already is or as the operand its initialiser
// produced (a temp, a constant, or another parameter).
struct LocalRef {
    SlotId slot{0};
    ParamId param{0};
    bool isParameter{false};
    Operand value;
    bool isValue{false};

    [[nodiscard]] bool valid() const { return slot != 0 || isParameter || isValue; }

    [[nodiscard]] static LocalRef slotRef(SlotId id) {
        LocalRef ref{};
        ref.slot = id;
        return ref;
    }
    [[nodiscard]] static LocalRef parameterRef(ParamId id) {
        LocalRef ref{};
        ref.param = id;
        ref.isParameter = true;
        return ref;
    }
    [[nodiscard]] static LocalRef valueRef(Operand operand) {
        LocalRef ref{};
        ref.value = operand;
        ref.isValue = true;
        return ref;
    }
};

// Deep-copies a match pattern so an arm's nested patterns can be lowered
// uniformly through the same recursive walk the top-level pattern uses.
zl::MatchExpr::Pattern cloneMatchPattern(const zl::MatchExpr::Pattern& source);

struct FunctionLowerer {
    LoweringContext& ctx;
    FunctionBuilder fb;
    std::vector<std::string> typeParams;
    TypeConverter types;
    std::string ownerClass;
    bool isStaticContext{false};
    LocalRef thisRef;
    std::uint32_t thisType{0};
    std::vector<std::unordered_map<std::string, LocalRef>> scopes;
    std::vector<LoopContext> loops;
    // Dynamic handler chain for blocks created from here on.
    std::vector<ExceptionHandler> activeHandlers;
    // Finally blocks active at the current point, innermost last. A
    // return/break/continue runs them inner-to-outer before leaving; a thrown
    // exception runs them through the finally handler instead.
    std::vector<const zl::BlockStmt*> activeFinallyBlocks;
    // Names this body assigns to or moves out of (storage keys). A binding that
    // is in here needs real storage; one that is not can stay an SSA value.
    std::unordered_set<std::string> writtenLocals;
    // Owned slots that have been `move`d out of somewhere in this body and not
    // definitively re-initialised. The end-of-lifetime cleanup skips them: a
    // drop after a move would be a second consumption of a value that is no
    // longer there, and skipping is the conservative direction - the runtime
    // frame teardown still releases the (cleared) local, exactly as the
    // reference compiler relies on for its own early-return paths.
    std::set<SlotId> movedOwnedSlots;
    bool failed{false};

    FunctionLowerer(LoweringContext& context, FunctionId id, std::vector<std::string> params)
        : ctx(context), fb(context.builder.functionBuilder(id)), typeParams(std::move(params)),
          types{context.builder.types(), typeParams} {
        scopes.emplace_back();
    }

    // --- diagnostics ------------------------------------------------------
    void unsupported(const zl::AstNode* node, const std::string& what) {
        if (failed) return;
        failed = true;
        std::ostringstream out;
        out << "MIR lowering: unsupported " << what;
        if (node) out << " at line " << node->line;
        fb.markIncomplete(out.str());
        ctx.note(out.str() + " in " + fb.function().name);
    }

    [[nodiscard]] SourceLocation location(const zl::AstNode* node) const {
        SourceLocation loc;
        loc.file = node && !node->sourceFile.empty() ? node->sourceFile : fb.function().location.file;
        if (node) loc.line = static_cast<std::uint32_t>(node->line);
        return loc;
    }

    // --- blocks -----------------------------------------------------------
    [[nodiscard]] BlockId newBlock(zl::AstNode* node = nullptr, BlockKind kind = BlockKind::Normal) {
        const BlockId id = fb.addBlock(kind, location(node));
        // A new block inherits the handler chain that is dynamically active
        // where it was created, which is exactly how nested try/catch nests.
        fb.block(id).exceptionHandlers = activeHandlers;
        return id;
    }

    void gotoBlock(BlockId id) { fb.setCurrentBlock(id); }

    // True when the current block already ends, so further statements in it are
    // dead and must not be emitted.
    [[nodiscard]] bool isDead() {
        return fb.block(fb.currentBlock()).terminator.kind != TerminatorKind::None;
    }

    // --- scopes and locals ------------------------------------------------
    void pushScope() { scopes.emplace_back(); }
    void endBorrowsFromScope(std::size_t depth, SourceLocation loc = {}) {
        if (isDead()) return;
        std::set<SlotId> borrows;
        for (std::size_t i = depth; i < scopes.size(); ++i) {
            for (const auto& [name, local] : scopes[i]) {
                (void)name;
                const Slot* slot = fb.function().slot(local.slot);
                if (slot && slot->ownership == zl::OwnershipKind::BORROW) borrows.insert(local.slot);
            }
        }
        for (SlotId slot : borrows) {
            const auto& declared = fb.function().slot(slot)->location;
            fb.emitEndBorrow(slot, loc.valid() ? loc : declared);
        }
    }
    void popScope() {
        // The checker ends lexical borrows on scope exit. Make that event
        // explicit: a may-borrow join must not "forget" it by accident.
        endBorrowsFromScope(scopes.size() - 1);
        scopes.pop_back();
    }

    [[nodiscard]] LocalRef lookupLocal(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return LocalRef{};
    }

    [[nodiscard]] SlotId lookupSlot(const std::string& name) const {
        const LocalRef local = lookupLocal(name);
        // Only a slot is addressable storage; parameters and SSA lets are not.
        return local.slot;
    }

    void bindLocal(const std::string& name, LocalRef local) { scopes.back()[name] = local; }

    // A binding is reachable under both its source name and the unique storage
    // name semantic analysis gave it. The two are not always the same string -
    // a parameter's `storageName` can be empty while an assignment to it carries
    // one - and looking a binding up under the wrong one reads as "unbound".
    void bindLocalBoth(const std::string& name, const std::string& storageName, LocalRef local) {
        if (!name.empty()) bindLocal(name, local);
        if (!storageName.empty() && storageName != name) bindLocal(storageName, local);
    }

    [[nodiscard]] static const std::string& storageKey(const std::string& storageName, const std::string& name) {
        return storageName.empty() ? name : storageName;
    }

    SlotId defineSlot(const std::string& storageName, const std::string& name, TypeId type, bool isMutable,
                      zl::OwnershipKind ownership, const zl::AstNode* node) {
        const SlotId slot = fb.addSlot(storageKey(storageName, name), type, isMutable, ownership, {},
                                       location(node));
        bindLocalBoth(name, storageName, LocalRef::slotRef(slot));
        return slot;
    }

    // The function-level slot registered under `key`, or 0. Scopes may shadow
    // a name with a read-only value binding (a match arm rebinds its subject
    // to a refined value) while the addressable storage still exists here.
    [[nodiscard]] SlotId slotNamed(const std::string& key) const {
        for (std::size_t i = 0; i < fb.function().slots.size(); ++i) {
            if (fb.function().slots[i].name == key) return static_cast<SlotId>(i + 1);
        }
        return 0;
    }

    // Binds a name introduced by a match pattern. A binding the arm assigns to
    // (its storage key is in `writtenLocals`) gets its own writable slot - the
    // write needs addressable storage of the binding's own, shadowing any outer
    // name the binding may hide; every other binding stays a read-only value.
    void bindPatternBinding(const std::string& bindingName, const std::string& storageName,
                            const Operand& value, const zl::MatchExpr::Arm& arm, SourceLocation loc) {
        (void)arm;
        if (bindingName.empty() || bindingName == "_") return;
        const std::string key = storageKey(storageName, bindingName);
        if (!storageName.empty() && writtenLocals.count(key) != 0) {
            const SlotId slot = fb.addSlot(key, value.type, true, zl::OwnershipKind::GC, {}, loc);
            fb.emitStore(slot, value, loc);
            bindLocalBoth(bindingName, storageName, LocalRef::slotRef(slot));
            return;
        }
        bindLocalBoth(bindingName, storageName, LocalRef::valueRef(value));
    }

    // --- types ------------------------------------------------------------
    [[nodiscard]] TypeId typeOfNode(const zl::AstNode* node) const {
        return types.fromInferred(ctx.checker.expressionType(node));
    }

    // --- operands ---------------------------------------------------------
    [[nodiscard]] Operand constOperand(const zl::Literal& literal) {
        const TypeId type = typeOfNode(&literal);
        const SourceLocation loc = location(&literal);
        switch (literal.literalType) {
            case zl::TokenType::INT_LITERAL: {
                std::int64_t value = 0;
                try { value = std::stoll(literal.raw); } catch (...) { value = 0; }
                return Operand::constant(ctx.builder.constantInt(value), ctx.builder.types().intType());
            }
            case zl::TokenType::DECIMAL_LITERAL:
            case zl::TokenType::FLOAT_LITERAL: {
                double value = 0.0;
                try { value = std::stod(literal.raw); } catch (...) { value = 0.0; }
                return Operand::constant(ctx.builder.constantDouble(value), ctx.builder.types().doubleType());
            }
            case zl::TokenType::BOOL_LITERAL:
                return Operand::constant(ctx.builder.constantBool(literal.raw == "true"),
                                         ctx.builder.types().boolType());
            case zl::TokenType::STRING_LITERAL: {
                // literal.raw is the lexer's already-unescaped token text
                // (delimiters stripped, escapes processed); use it verbatim.
                // A previous revision stripped a leading/trailing quote here
                // on the assumption the raw lexeme still carried delimiters,
                // which silently corrupted any literal whose VALUE starts
                // with '"' or '\'' (e.g. "\"q\"" compiled to `q`).
                return Operand::constant(ctx.builder.constantString(std::string(literal.raw)),
                                         ctx.builder.types().stringType());
            }
            case zl::TokenType::KW_NULL: {
                // A bare null literal has type nil; assigned to a reference it
                // keeps the nil constant and takes the target's type.
                return Operand::constant(ctx.builder.constantNil(), type ? type : ctx.builder.types().nilType());
            }
            default:
                unsupported(&literal, "literal '" + literal.raw + "'");
                return Operand::none();
        }
        (void)loc;
    }

    // --- expressions ------------------------------------------------------
    [[nodiscard]] Operand expression(const zl::AstNode* node) {
        if (!node) {
            unsupported(nullptr, "null expression");
            return Operand::none();
        }
        if (failed) return Operand::none();
        switch (node->kind) {
            case zl::NodeKind::Literal:
                return constOperand(static_cast<const zl::Literal&>(*node));
            case zl::NodeKind::Identifier:
                return identifier(static_cast<const zl::Identifier&>(*node));
            case zl::NodeKind::ThisExpr:
                return thisOperand(static_cast<const zl::ThisExpr&>(*node));
            case zl::NodeKind::UnaryExpr:
                return unary(static_cast<const zl::UnaryExpr&>(*node));
            case zl::NodeKind::BinaryExpr:
                return binary(static_cast<const zl::BinaryExpr&>(*node));
            case zl::NodeKind::AssignExpr:
                return assign(static_cast<const zl::AssignExpr&>(*node));
            case zl::NodeKind::MoveExpr:
                return move(static_cast<const zl::MoveExpr&>(*node));
            case zl::NodeKind::CallExpr:
                return call(static_cast<const zl::CallExpr&>(*node));
            case zl::NodeKind::MethodCallExpr:
                return methodCall(static_cast<const zl::MethodCallExpr&>(*node));
            case zl::NodeKind::SuperMethodCallExpr:
                return superMethodCall(static_cast<const zl::SuperMethodCallExpr&>(*node));
            case zl::NodeKind::SuperCallExpr:
                return superCall(static_cast<const zl::SuperCallExpr&>(*node), node);
            case zl::NodeKind::NewExpr:
                return newExpression(static_cast<const zl::NewExpr&>(*node));
            case zl::NodeKind::FieldAccessExpr:
                return fieldAccess(static_cast<const zl::FieldAccessExpr&>(*node));
            case zl::NodeKind::IndexAccessExpr:
                return indexAccess(static_cast<const zl::IndexAccessExpr&>(*node));
            case zl::NodeKind::FieldAssignExpr:
                return fieldAssign(static_cast<const zl::FieldAssignExpr&>(*node));
            case zl::NodeKind::CollectionLiteral:
                return collectionLiteral(static_cast<const zl::CollectionLiteral&>(*node));
            case zl::NodeKind::AwaitExpr:
                return await(static_cast<const zl::AwaitExpr&>(*node));
            case zl::NodeKind::DataLiteralExpr:
                return dataLiteral(static_cast<const zl::DataLiteralExpr&>(*node));
            case zl::NodeKind::DataUpdateExpr:
                return dataUpdate(static_cast<const zl::DataUpdateExpr&>(*node));
            case zl::NodeKind::MatchExpr:
                return matchExpr(static_cast<const zl::MatchExpr&>(*node));
            case zl::NodeKind::LambdaExpr:
                return lambda(static_cast<const zl::LambdaExpr&>(*node));
            case zl::NodeKind::LogExpr: {
                const auto& log = static_cast<const zl::LogExpr&>(*node);
                Operand value = expression(log.argument.get());
                if (value.isNone()) return Operand::none();
                fb.emitLog(value, location(node));
                return Operand::constant(ctx.builder.constantNil(), ctx.builder.types().voidType());
            }
            default:
                unsupported(node, "expression");
                return Operand::none();
        }
    }

    [[nodiscard]] Operand identifier(const zl::Identifier& node) {
        const LocalRef local = lookupLocal(storageKey(node.storageName, node.name));
        if (!local.valid()) {
            unsupported(&node, "reference to unbound local '" + node.name + "'");
            return Operand::none();
        }
        if (local.isParameter) return fb.parameterOperand(local.param);
        if (local.isValue) return local.value;
        return Operand::temp(fb.emitLoad(local.slot, location(&node)), fb.function().slot(local.slot)->type);
    }

    [[nodiscard]] Operand thisOperand(const zl::ThisExpr& node) {
        if (isStaticContext || !thisRef.valid()) {
            unsupported(&node, "'this' outside an instance method");
            return Operand::none();
        }
        if (thisRef.isParameter) return fb.parameterOperand(thisRef.param);
        return Operand::temp(fb.emitLoad(thisRef.slot, location(&node)), thisType);
    }

    [[nodiscard]] Operand unary(const zl::UnaryExpr& node) {
        Operand operand = expression(node.operand.get());
        if (operand.isNone()) return Operand::none();
        const SourceLocation loc = location(&node);
        const TypeArena& arena = ctx.builder.types();

        // An overloaded operator is a method call in ZL, not a builtin.
        if (node.isOperatorOverload) {
            const TypeId resultType = typeOfNode(&node);
            const Type* overloadReceiver = ctx.builder.types().find(operand.type);
            const std::string overloadClass = operandTypeName(operand);
            if (overloadClass.empty()) {
                unsupported(&node, "overloaded operator on a receiver whose class could not be resolved");
                return Operand::none();
            }
            return Operand::temp(fb.emitInvokeMethod(operand, overloadClass,
                                                     node.resolvedOperatorDispatch.name, {}, resultType, loc,
                                                     overloadReceiver ? overloadReceiver->arguments
                                                                      : std::vector<TypeId>{}),
                                 resultType);
        }
        switch (node.op) {
            case zl::TokenType::MINUS: {
                const auto result = zl::OperatorRules::unaryResult(zl::Operator::Minus, zlTypeOf(operand.type));
                if (!result) { unsupported(&node, "unary '-'"); return Operand::none(); }
                const TypeId type = typeIdFor(*result);
                return Operand::temp(fb.emitUnary(Opcode::Neg, operand, type, loc), type);
            }
            case zl::TokenType::PLUS:
                // Unary plus is the identity; no MIR instruction is needed.
                return operand;
            case zl::TokenType::NOT: {
                const TypeId type = arena.boolType();
                return Operand::temp(fb.emitUnary(Opcode::Not, operand, type, loc), type);
            }
            case zl::TokenType::BIT_NOT: {
                const TypeId type = arena.intType();
                return Operand::temp(fb.emitUnary(Opcode::BitNot, operand, type, loc), type);
            }
            default:
                unsupported(&node, "unary operator");
                return Operand::none();
        }
    }

    [[nodiscard]] Operand binary(const zl::BinaryExpr& node) {
        // `&&` and `||` short-circuit in ZL, so they are control flow here
        // rather than a boolean instruction: the right-hand side must not be
        // evaluated at all when the left decides the result.
        if (node.op == zl::TokenType::AND || node.op == zl::TokenType::OR) {
            return logical(node);
        }

        Operand left = expression(node.left.get());
        if (left.isNone()) return Operand::none();
        Operand right = expression(node.right.get());
        if (right.isNone()) return Operand::none();
        const SourceLocation loc = location(&node);

        if (node.isOperatorOverload) {
            const TypeId resultType = typeOfNode(&node);
            const Operand receiver = left;
            const Type* overloadReceiver = ctx.builder.types().find(receiver.type);
            const std::string overloadClass = operandTypeName(receiver);
            if (overloadClass.empty()) {
                unsupported(&node, "overloaded operator on a receiver whose class could not be resolved");
                return Operand::none();
            }
            return Operand::temp(fb.emitInvokeMethod(receiver, overloadClass,
                                                     node.resolvedOperatorDispatch.name, {right}, resultType, loc,
                                                     overloadReceiver ? overloadReceiver->arguments
                                                                      : std::vector<TypeId>{}),
                                 resultType);
        }

        const Opcode opcode = binaryOpcode(node.op);
        if (opcode == Opcode::Nop) {
            unsupported(&node, "binary operator");
            return Operand::none();
        }
        const auto result = zl::OperatorRules::binaryResult(operatorFor(opcode), zlTypeOf(left.type),
                                                            zlTypeOf(right.type));
        // An unknown operand is a dynamic boundary, not a type error: the
        // checker could not classify the operator statically (an untyped
        // lambda parameter, a dynamic value), and the runtime dispatches on
        // the actual values exactly as the reference path does. The static
        // table exists to classify fully typed operands; it must not reject
        // the dynamic ones or every closure over an untyped parameter would
        // be unlowerable.
        const auto dynamic = [this](TypeId id) {
            const Type* resolved = ctx.builder.types().find(id);
            return !resolved || resolved->kind == TypeKind::Unknown;
        };
        if (!result && !dynamic(left.type) && !dynamic(right.type)) {
            unsupported(&node, "binary operator on " + ctx.builder.types().render(left.type) + " and " +
                                   ctx.builder.types().render(right.type));
            return Operand::none();
        }
        if (result) {
            const TypeId type = typeIdFor(*result);
            // `1 + 2.5` is legal ZL, and OperatorRules types it as double. Left as
            // written, though, the MIR would carry an `add` of an int and a double,
            // so every backend would have to re-derive the promotion rule. Widen the
            // int operand instead: arithmetic in MIR is homogeneous, and the
            // conversion the language performs implicitly is visible in the graph.
            // An unknown operand is exempt: it feeds the runtime dispatcher as-is,
            // and refining it to the table's result would assert a type the
            // reference never checks (`"got " + x` accepts an int just fine).
            const auto unknown = [this](TypeId id) {
                const Type* resolved = ctx.builder.types().find(id);
                return !resolved || resolved->kind == TypeKind::Unknown;
            };
            if (!unknown(left.type)) left = coerce(left, type, loc);
            if (!unknown(right.type)) right = coerce(right, type, loc);
            if (left.isNone() || right.isNone()) return Operand::none();
            return Operand::temp(fb.emitBinary(opcode, left, right, type, loc), type);
        }
        // The dynamic boundary takes no coercion: inserting a Refine here would
        // assert a type the reference never checks (calling `func(x) => x * 2`
        // with a double is legitimate dynamic arithmetic), so the operands go
        // through exactly as the checker typed them.
        const TypeId type = typeOfNode(&node);
        return Operand::temp(fb.emitBinary(opcode, left, right, type, loc), type);
    }

    [[nodiscard]] Operand logical(const zl::BinaryExpr& node) {
        const SourceLocation loc = location(&node);
        const TypeArena& arena = ctx.builder.types();
        Operand left = asCondition(expression(node.left.get()), node.left.get());
        if (left.isNone()) return Operand::none();

        // The result lives in a slot so both arms can write it and the join can
        // read one stable value. This is the same shape the existing IR uses.
        const SlotId resultSlot = fb.addSlot("__logical", arena.boolType(), true, zl::OwnershipKind::GC, {}, loc);
        const bool isAnd = node.op == zl::TokenType::AND;
        const Operand shortCircuitValue = Operand::constant(ctx.builder.constantBool(!isAnd), arena.boolType());
        fb.emitStore(resultSlot, shortCircuitValue, loc);

        const BlockId rhsBlock = newBlock(const_cast<zl::BinaryExpr*>(&node));
        const BlockId shortCircuitBlock = newBlock(const_cast<zl::BinaryExpr*>(&node));
        const BlockId joinBlock = newBlock(const_cast<zl::BinaryExpr*>(&node));

        fb.emitBranch(left, isAnd ? rhsBlock : shortCircuitBlock, isAnd ? shortCircuitBlock : rhsBlock, loc);

        gotoBlock(shortCircuitBlock);
        fb.emitJump(joinBlock, loc);

        gotoBlock(rhsBlock);
        Operand right = asCondition(expression(node.right.get()), node.right.get());
        if (right.isNone()) return Operand::none();
        fb.emitStore(resultSlot, right, loc);
        fb.emitJump(joinBlock, loc);

        gotoBlock(joinBlock);
        return Operand::temp(fb.emitLoad(resultSlot, loc), arena.boolType());
    }

    [[nodiscard]] Operand assign(const zl::AssignExpr& node) {
        const std::string key = storageKey(node.storageName, node.name);
        SlotId slot = lookupSlot(key);
        if (slot == 0) {
            // A match arm rebinds its subject to a read-only (possibly
            // refined) value in the arm scope, while the addressable storage
            // the name refers to - a parameter or local materialized for the
            // write - still exists at function level. A write through such a
            // shadow must reach that storage; rebind the name to it here so
            // later reads in this scope observe the fresh value.
            slot = slotNamed(key);
            if (slot == 0 || !fb.function().slot(slot)->isMutable) {
                unsupported(&node, "assignment to unbound local '" + node.name + "'");
                return Operand::none();
            }
            bindLocalBoth(node.name, node.storageName, LocalRef::slotRef(slot));
        }
        Operand value = expression(node.value.get());
        if (value.isNone()) return Operand::none();
        const SourceLocation loc = location(&node);
        value = coerce(value, fb.function().slot(slot)->type, loc);
        if (value.isNone()) return Operand::none();
        fb.emitStore(slot, value, loc);
        // A store is a fresh value: any earlier move of this slot is undone.
        movedOwnedSlots.erase(slot);
        return value;
    }

    [[nodiscard]] Operand move(const zl::MoveExpr& node) {
        const SlotId slot = lookupSlot(storageKey(node.storageName, node.name));
        if (slot == 0) {
            unsupported(&node, "move of unbound local '" + node.name + "'");
            return Operand::none();
        }
        const TypeId type = fb.function().slot(slot)->type;
        movedOwnedSlots.insert(slot);
        return Operand::temp(fb.emitMove(slot, location(&node)), type);
    }

    // Lowers a qualified native call that names a concurrency primitive to its
    // dedicated MIR operation. Sets `lowered` and returns the call's operand on
    // success; returns none() with `lowered == false` when the name is not a
    // concurrency primitive and the generic CallNative path applies.
    //
    // Deliberately NOT intercepted: `Shared.__get/__set/__withLock` (the dunder
    // natives backing the builtin Shared methods - intercepting them would make
    // the backend's SharedGet lower back into the same method infinitely) and
    // `Time.sleepAsync` (a timer that happens to return a Task, not one of the
    // synchronisation primitives; it stays an ordinary async CallNative).
    [[nodiscard]] Operand concurrencyNativeCall(const std::string& qualifiedName,
                                                std::vector<Operand>& arguments, TypeId resultType,
                                                SourceLocation loc, bool& lowered) {
        lowered = false;
        const TypeArena& arena = ctx.builder.types();
        const bool returnsVoid = isVoid(resultType);

        // The Task a spawn produces. The checker types Task.spawn from the
        // closure's own return; when the closure is dynamically typed there is
        // no payload to name, and unknown is the honest answer.
        const auto spawnResult = [&](const Operand& closure) -> TypeId {
            if (const Type* recorded = arena.find(resultType)) {
                if (isTaskType(*recorded)) return resultType;
            }
            TypeId payload = arena.unknownType();
            if (const Type* closureType = arena.find(closure.type)) {
                if (closureType->kind == TypeKind::Function && closureType->signature.hasSignature) {
                    payload = closureType->signature.returnType;
                }
            }
            // An empty-body lambda reports nil, not void; the task it spawns
            // completes with the runtime nil either way, and Task<void> is the
            // spelling that agrees with the closure body's own void return.
            if (const Type* payloadType = arena.find(payload)) {
                if (payloadType->kind == TypeKind::Nil) payload = arena.voidType();
            }
            return arena.taskType(payload);
        };

        if (qualifiedName == "Task.spawn" && arguments.size() == 1) {
            lowered = true;
            const TypeId task = spawnResult(arguments[0]);
            const TempId temp = fb.emitTaskSpawn(std::move(arguments[0]), task, loc);
            return Operand::temp(temp, task);
        }
        if (qualifiedName == "Thread.start" && arguments.size() == 1) {
            lowered = true;
            const TypeId thread = arena.threadType();
            const TempId temp = fb.emitThreadStart(std::move(arguments[0]), thread, loc);
            return Operand::temp(temp, thread);
        }
        if (qualifiedName == "Thread.join" && arguments.size() == 1) {
            lowered = true;
            fb.emitThreadJoin(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Thread.isAlive" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitThreadIsAlive(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.boolType());
        }
        if (qualifiedName == "Mutex.withLock" && arguments.size() == 2) {
            lowered = true;
            if (returnsVoid) {
                const TempId ignored =
                    fb.emitMutexWithLock(std::move(arguments[0]), std::move(arguments[1]), 0, loc);
                (void)ignored;
                return Operand::none();
            }
            const TempId temp =
                fb.emitMutexWithLock(std::move(arguments[0]), std::move(arguments[1]), resultType, loc);
            return Operand::temp(temp, resultType);
        }
        if ((qualifiedName == "RwLock.withRead" || qualifiedName == "RwLock.withWrite") &&
            arguments.size() == 2) {
            lowered = true;
            TempId temp = kNoTemp;
            if (qualifiedName == "RwLock.withRead") {
                temp = fb.emitRwLockWithRead(std::move(arguments[0]), std::move(arguments[1]),
                                             returnsVoid ? 0 : resultType, loc);
            } else {
                temp = fb.emitRwLockWithWrite(std::move(arguments[0]), std::move(arguments[1]),
                                              returnsVoid ? 0 : resultType, loc);
            }
            return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
        }
        if (qualifiedName == "Atomic.load" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitAtomicLoad(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.intType());
        }
        if (qualifiedName == "Atomic.store" && arguments.size() == 2) {
            lowered = true;
            fb.emitAtomicStore(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Atomic.add" && arguments.size() == 2) {
            lowered = true;
            const TempId temp = fb.emitAtomicAdd(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::temp(temp, arena.intType());
        }
        if (qualifiedName == "Atomic.loadBool" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitAtomicLoadBool(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.boolType());
        }
        if (qualifiedName == "Atomic.storeBool" && arguments.size() == 2) {
            lowered = true;
            fb.emitAtomicStoreBool(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Atomic.loadDouble" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitAtomicLoadDouble(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.doubleType());
        }
        if (qualifiedName == "Atomic.storeDouble" && arguments.size() == 2) {
            lowered = true;
            fb.emitAtomicStoreDouble(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Atomic.loadRef" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitAtomicLoadRef(std::move(arguments[0]), resultType, loc);
            return Operand::temp(temp, resultType);
        }
        if (qualifiedName == "Atomic.storeRef" && arguments.size() == 2) {
            lowered = true;
            fb.emitAtomicStoreRef(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Semaphore.acquire" && arguments.size() == 1) {
            lowered = true;
            fb.emitSemaphoreAcquire(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Semaphore.release" && arguments.size() == 1) {
            lowered = true;
            fb.emitSemaphoreRelease(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Semaphore.available" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitSemaphoreAvailable(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.intType());
        }
        if (qualifiedName == "Semaphore.setPermits" && arguments.size() == 2) {
            lowered = true;
            fb.emitSemaphoreSetPermits(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Semaphore.tryAcquire" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitSemaphoreTryAcquire(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.boolType());
        }
        if (qualifiedName == "Semaphore.releaseMany" && arguments.size() == 2) {
            lowered = true;
            fb.emitSemaphoreReleaseMany(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Condition.wait" && arguments.size() == 1) {
            lowered = true;
            fb.emitConditionWait(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Condition.notifyOne" && arguments.size() == 1) {
            lowered = true;
            fb.emitConditionNotifyOne(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Condition.notifyAll" && arguments.size() == 1) {
            lowered = true;
            fb.emitConditionNotifyAll(std::move(arguments[0]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Condition.waitFor" && arguments.size() == 2) {
            lowered = true;
            const TempId temp =
                fb.emitConditionWaitFor(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::temp(temp, arena.boolType());
        }
        if (qualifiedName == "Channel.create" && arguments.size() == 1) {
            lowered = true;
            TypeId channel = resultType;
            if (const Type* recorded = arena.find(channel)) {
                if (!isChannelType(*recorded)) channel = arena.channelType(arena.unknownType());
            } else {
                channel = arena.channelType(arena.unknownType());
            }
            const TempId temp = fb.emitChannelCreate(std::move(arguments[0]), channel, loc);
            return Operand::temp(temp, channel);
        }
        if (qualifiedName == "Channel.send" && arguments.size() == 2) {
            lowered = true;
            fb.emitChannelSend(std::move(arguments[0]), std::move(arguments[1]), loc);
            return Operand::none();
        }
        if (qualifiedName == "Channel.receive" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitChannelReceive(std::move(arguments[0]), resultType, loc);
            return Operand::temp(temp, resultType);
        }
        if (qualifiedName == "Channel.size" && arguments.size() == 1) {
            lowered = true;
            const TempId temp = fb.emitChannelSize(std::move(arguments[0]), loc);
            return Operand::temp(temp, arena.intType());
        }
        // The checker leaves the async channel operations' Task payloads
        // unnamed (Task<void> by default rendering), which is right for the
        // send side and wrong for receive: a receive Task carries the next
        // value. Name both explicitly so await sees the honest payload.
        if (qualifiedName == "Channel.sendAsync" && arguments.size() == 2) {
            lowered = true;
            const TypeId task = arena.taskType(arena.voidType());
            const TempId temp =
                fb.emitChannelSendAsync(std::move(arguments[0]), std::move(arguments[1]), task, loc);
            return Operand::temp(temp, task);
        }
        if (qualifiedName == "Channel.receiveAsync" && arguments.size() == 1) {
            lowered = true;
            const TypeId task = arena.taskType(arena.unknownType());
            const TempId temp = fb.emitChannelReceiveAsync(std::move(arguments[0]), task, loc);
            return Operand::temp(temp, task);
        }
        return Operand::none();
    }

    [[nodiscard]] Operand call(const zl::CallExpr& node) {
        const SourceLocation loc = location(&node);

        // A call through a variable holding a closure is an indirect call.
        if (node.isValueCall) {
            const LocalRef local = lookupLocal(storageKey(node.calleeStorageName, node.calleeName));
            if (!local.valid()) {
                unsupported(&node, "indirect call through unbound local '" + node.calleeName + "'");
                return Operand::none();
            }
            Operand callee = local.isParameter
                                 ? fb.parameterOperand(local.param)
                                 : local.isValue ? local.value : Operand::temp(fb.emitLoad(local.slot, loc),
                                                 fb.function().slot(local.slot)->type);
            std::vector<Operand> arguments;
            for (const auto& argument : node.arguments) {
                Operand value = expression(argument.get());
                if (value.isNone()) return Operand::none();
                arguments.push_back(value);
            }
            const TypeId resultType = typeOfNode(&node);
            const bool returnsVoid = isVoid(resultType);
            // The callable's own signature is the boundary: an unknown-typed
            // argument is asserted to the parameter type, not silently passed.
            if (const Type* calleeType = ctx.builder.types().find(callee.type)) {
                if (calleeType->kind == TypeKind::Function && calleeType->signature.hasSignature) {
                    const FunctionSignature& signature = calleeType->signature;
                    for (std::size_t i = 0; i < arguments.size() && i < signature.parameterTypes.size(); ++i) {
                        Operand refined = coerce(arguments[i], signature.parameterTypes[i], loc);
                        if (!refined.isNone()) arguments[i] = std::move(refined);
                    }
                }
            }
            const TempId temp = fb.emitCallIndirect(callee, std::move(arguments),
                                                    returnsVoid ? 0 : resultType, loc);
            return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
        }

        std::vector<Operand> arguments;
        for (const auto& argument : node.arguments) {
            Operand value = expression(argument.get());
            if (value.isNone()) return Operand::none();
            arguments.push_back(value);
        }
        const TypeId resultType = typeOfNode(&node);
        const bool returnsVoid = isVoid(resultType);

        if (!node.namespaceName.empty()) {
            // A qualified call is either a native catalog entry or a static
            // method on another class.
            const std::string qualifiedName = node.namespaceName + "." + node.calleeName;
            if (const auto native = zl::findNativeSignature(qualifiedName)) {
                // The concurrency primitives lower to dedicated MIR operations
                // rather than to a generic CallNative, so suspension, blocking,
                // thread transfer, and synchronisation are explicit in MIR. The
                // backend translates each back to the same runtime mechanism
                // (the same catalog entry or opcode), so the generated bytecode
                // is unchanged - only the MIR is more precise.
                bool lowered = false;
                Operand concurrency =
                    concurrencyNativeCall(qualifiedName, arguments, resultType, loc, lowered);
                if (lowered) return concurrency;
                // No per-argument assertion here - the reference bytecode
                // compiler emits none for native calls. Statically typed
                // arguments are checked by the front end; dynamic ones are
                // checked by the native itself (its require* contract throws a
                // clean, function-specific type error) and by the container
                // storage-contract machinery. Asserting the catalog's declared
                // names at the call site would instead commit each argument's
                // pin as it passed - a batch that fails on argument N would
                // leave 1..N-1 pinned - and a generic native's tokens (push's
                // list<T>/T) are not resolvable outside the native's own frame.
                const TempId temp = fb.emitCallNative(qualifiedName, static_cast<std::int32_t>((*native)->id),
                                                      std::move(arguments), returnsVoid ? 0 : resultType,
                                                      (*native)->taskValueType != zl::ZlType::UNKNOWN, loc);
                return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
            }
            if (zl::findNativeFunctionByName(qualifiedName)) {
                // Runtime-registered extension native (zl-bind output): no
                // catalog entry, resolved by name alone. The id is -1, not
                // NativeId::EXTENSION: every extension shares that id, so an
                // id lookup would find the first extension rather than this
                // one - the backend resolves -1 by name. Arity was validated
                // by the front end against the declared arity; the result is
                // dynamic.
                const TempId temp = fb.emitCallNative(qualifiedName, -1, std::move(arguments),
                                                      returnsVoid ? 0 : resultType, false, loc);
                return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
            }
            const std::string target = node.namespaceName + "." + node.resolvedDispatch.describe();
            const FunctionId callee = lookupFunction(target);
            if (callee == kNoFunction) {
                unsupported(&node, "call to '" + target + "' which was not lowered");
                return Operand::none();
            }
            if (const Function* calleeFunction = ctx.builder.module().function(callee)) {
                refineArguments(arguments, *calleeFunction, 0, loc);
            }
            const TempId temp = fb.emitInvokeStatic(callee, std::move(arguments), returnsVoid ? 0 : resultType, loc);
            setLastInstructionName(node.resolvedTypeArgNames);
            return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
        }

        // A bare call can still be a global native. `share(x)` is spelled with
        // no namespace and the catalog keys it by its bare name, so consulting
        // only qualified names misses it - the call then reads as an implicit
        // self-call to a method that does not exist, and the note names an empty
        // dispatch. Checking the catalog by the bare name is what the bytecode
        // compiler does too; it special-cases `share` by hand, and this is the
        // same test without the hardcoding.
        if (node.namespaceName.empty()) {
            if (const auto native = zl::findNativeSignature(node.calleeName)) {
                // `share(value)` is the Shared<T> allocation spelled as a
                // global native; see the qualified path above.
                if (node.calleeName == "share" && arguments.size() == 1) {
                    const TempId temp = fb.emitSharedCreate(std::move(arguments[0]), resultType, loc);
                    return Operand::temp(temp, resultType);
                }
                // No per-argument assertion: the reference emits none for
                // native calls (see the qualified path above).
                const TempId temp = fb.emitCallNative(node.calleeName, static_cast<std::int32_t>((*native)->id),
                                                     std::move(arguments), returnsVoid ? 0 : resultType,
                                                     (*native)->taskValueType != zl::ZlType::UNKNOWN, loc);
                return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
            }
        }

        // A bare call is an implicit self-call.
        const std::string owner = ownerClass.empty() ? node.namespaceName : ownerClass;
        const std::string target = owner + "." + node.resolvedDispatch.describe();
        const FunctionId callee = lookupFunction(target);
        if (callee == kNoFunction) {
            unsupported(&node, "call to '" + target + "' which was not lowered");
            return Operand::none();
        }
        std::vector<Operand> callArguments;
        const Function* calleeFunction = ctx.builder.module().function(callee);
        const std::size_t leading = calleeFunction && calleeFunction->hasThisParameter ? 1 : 0;
        if (leading == 1) {
            Operand self = thisOperand(zl::ThisExpr{} , loc);
            if (self.isNone()) return Operand::none();
            callArguments.push_back(self);
        }
        for (auto& argument : arguments) callArguments.push_back(std::move(argument));
        if (calleeFunction) refineArguments(callArguments, *calleeFunction, leading, loc);
        const TempId temp = fb.emitCall(callee, std::move(callArguments), returnsVoid ? 0 : resultType, loc,
                                        callArguments.empty() ? std::vector<TypeId>{}
                                                              : typeArgumentsFor(callee, callArguments.front()));
        setLastInstructionName(node.resolvedTypeArgNames);
        return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
    }

    [[nodiscard]] Operand thisOperand(const zl::ThisExpr& node, SourceLocation loc) {
        (void)loc;
        return thisOperand(node);
    }

    [[nodiscard]] Operand methodCall(const zl::MethodCallExpr& node) {
        const SourceLocation loc = location(&node);
        Operand receiver = expression(node.object.get());
        if (receiver.isNone()) return Operand::none();
        std::vector<Operand> arguments;
        for (const auto& argument : node.arguments) {
            Operand value = expression(argument.get());
            if (value.isNone()) return Operand::none();
            arguments.push_back(value);
        }
        const TypeId resultType = typeOfNode(&node);
        const bool returnsVoid = isVoid(resultType);
        // A method on a `string` is the `String.*` native the checker resolved,
        // with the receiver bound as its first argument
        // (TypeChecker::inferMethodCall): the method spelling and the qualified
        // native call reach the same catalog entry, so there is nothing to
        // duplicate here either.
        if (node.isStringMethod && node.nativeMethodId >= 0) {
            std::vector<Operand> nativeArguments;
            nativeArguments.reserve(arguments.size() + 1);
            nativeArguments.push_back(receiver);
            for (auto& argument : arguments) nativeArguments.push_back(std::move(argument));
            const TempId temp = fb.emitCallNative(node.nativeMethodName, node.nativeMethodId,
                                                 std::move(nativeArguments), resultType, false, loc);
            return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
        }
        const Type* receiverType = ctx.builder.types().find(receiver.type);
        // Task and Shared instance methods are runtime operations, not ordinary
        // dispatch: Task.block/ignore/cancel are task lifecycle (the reference
        // compiler emits dedicated opcodes for them), and Shared.get/setValue/
        // withLock are synchronised cell access. Lowering them to dedicated
        // MIR keeps that explicit; the backend translates each back to the same
        // mechanism the generic path used, so the bytecode is unchanged.
        if (receiverType && isTaskType(*receiverType) && arguments.empty()) {
            if (node.methodName == "block") {
                if (returnsVoid) {
                    const TempId ignored = fb.emitTaskBlock(receiver, 0, loc);
                    (void)ignored;
                    return Operand::none();
                }
                const TempId temp = fb.emitTaskBlock(receiver, resultType, loc);
                return Operand::temp(temp, resultType);
            }
            if (node.methodName == "ignore") {
                fb.emitTaskIgnore(receiver, loc);
                return Operand::none();
            }
            if (node.methodName == "cancel") {
                fb.emitTaskCancel(receiver, loc);
                return Operand::none();
            }
        }
        if (receiverType && isSharedType(*receiverType)) {
            if (node.methodName == "get" && arguments.empty()) {
                const TempId temp = fb.emitSharedGet(receiver, resultType, loc);
                return Operand::temp(temp, resultType);
            }
            if (node.methodName == "setValue" && arguments.size() == 1) {
                fb.emitSharedSet(receiver, std::move(arguments[0]), loc);
                return Operand::none();
            }
            if (node.methodName == "withLock" && arguments.size() == 1) {
                if (returnsVoid) {
                    const TempId ignored =
                        fb.emitSharedWithLock(receiver, std::move(arguments[0]), 0, loc);
                    (void)ignored;
                    return Operand::none();
                }
                const TempId temp = fb.emitSharedWithLock(receiver, std::move(arguments[0]), resultType, loc);
                return Operand::temp(temp, resultType);
            }
        }
        // A virtual dispatch names the class it dispatches on. When semantic
        // analysis could not resolve the receiver to a class - which happens
        // where a generic's own type parameter comes back out, as with
        // `Shared<T>.get()` - there is no class to name, and emitting the
        // instruction anyway would hand the verifier a malformed call. Say so
        // instead and leave the function incomplete.
        const std::string className = operandTypeName(receiver);
        if (className.empty()) {
            unsupported(&node, "method call on a receiver whose class could not be resolved");
            return Operand::none();
        }
        const TempId temp = fb.emitInvokeMethod(receiver, className, node.methodName,
                                                std::move(arguments), returnsVoid ? 0 : resultType, loc,
                                                receiverType ? receiverType->arguments : std::vector<TypeId>{});
        setLastInstructionName(node.resolvedTypeArgNames);
        return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
    }

    [[nodiscard]] Operand superMethodCall(const zl::SuperMethodCallExpr& node) {
        const SourceLocation loc = location(&node);
        Operand self = thisOperand(zl::ThisExpr{}, loc);
        if (self.isNone()) return Operand::none();
        std::vector<Operand> arguments;
        for (const auto& argument : node.arguments) {
            Operand value = expression(argument.get());
            if (value.isNone()) return Operand::none();
            arguments.push_back(value);
        }
        const std::string parent = ctx.classParents.count(ownerClass) ? ctx.classParents.at(ownerClass) : ownerClass;
        const std::string target = parent + "." + node.resolvedDispatch.describe();
        const FunctionId callee = lookupFunction(target);
        if (callee == kNoFunction) {
            unsupported(&node, "super call to '" + target + "' which was not lowered");
            return Operand::none();
        }
        const TypeId resultType = typeOfNode(&node);
        const bool returnsVoid = isVoid(resultType);
        if (const Function* calleeFunction = ctx.builder.module().function(callee)) {
            refineArguments(arguments, *calleeFunction, 1, loc);
        }
        const TempId temp = fb.emitInvokeSuper(self, callee, std::move(arguments),
                                               returnsVoid ? 0 : resultType, loc,
                                               typeArgumentsFor(callee, self));
        setLastInstructionName(node.resolvedTypeArgNames);
        return returnsVoid ? Operand::none() : Operand::temp(temp, resultType);
    }

    // `super(args)` as a constructor's first statement: a direct, non-virtual
    // call to the parent constructor with this receiver. It produces no value.
    [[nodiscard]] Operand superCall(const zl::SuperCallExpr& node, const zl::AstNode* site) {
        const SourceLocation loc = location(site);
        Operand self = thisOperand(zl::ThisExpr{}, loc);
        if (self.isNone()) return Operand::none();
        std::vector<Operand> arguments{self};
        for (const auto& argument : node.arguments) {
            Operand value = expression(argument.get());
            if (value.isNone()) return Operand::none();
            arguments.push_back(value);
        }
        const std::string parent = ctx.classParents.count(ownerClass) ? ctx.classParents.at(ownerClass) : std::string{};
        if (parent.empty()) {
            unsupported(site, "super(...) in a class with no parent");
            return Operand::none();
        }
        zl::DispatchSignature signature = node.resolvedDispatch;
        if (signature.name.empty()) signature.name = parent;
        const std::string target = parent + "." + signature.describe();
        const FunctionId constructor = lookupFunctionIn(parent, signature.describe());
        if (constructor == kNoFunction) {
            unsupported(site, "super constructor '" + target + "' which was not lowered");
            return Operand::none();
        }
        (void)fb.emitCall(constructor, std::move(arguments), 0, loc,
                          typeArgumentsFor(constructor, Operand::temp(kNoTemp, thisType)));
        return Operand::none();
    }

    [[nodiscard]] Operand newExpression(const zl::NewExpr& node) {
        const SourceLocation loc = location(&node);
        const TypeId objectType = typeOfNode(&node);
        std::vector<Operand> arguments;
        for (const auto& argument : node.arguments) {
            Operand value = expression(argument.get());
            if (value.isNone()) return Operand::none();
            arguments.push_back(value);
        }
        std::vector<TypeId> typeArguments;
        // Derive the alloc's generic arguments from the checker-recorded
        // result type, not from re-resolving the syntactic annotations.
        // checkAlloc requires alloc typeArguments to equal the result type's
        // arguments exactly, and the two spellings disagree for names the
        // checker erases (`object` and `Thread` become `unknown` in the
        // recorded instantiation, while the MIR converter interns them as
        // distinct types) - `new List<object>()` then fails verification even
        // though the reference accepts it. The recorded type is authoritative:
        // the lowerer re-resolves nothing. Fall back to the annotations only
        // when the checker recorded no argument list to read.
        if (const Type* recorded = ctx.builder.types().find(objectType);
            recorded && recorded->arguments.size() == node.typeArgs.size()) {
            typeArguments = recorded->arguments;
        } else {
            for (const auto& argument : node.typeArgs) typeArguments.push_back(types.fromAnnotation(argument));
        }

        const TempId instance = fb.emitAlloc(node.className, std::move(typeArguments), objectType, loc);
        const Operand receiver = Operand::temp(instance, objectType);

        // Constructors are direct calls whose first parameter is the receiver.
        zl::DispatchSignature signature = node.resolvedDispatch;
        if (signature.name.empty()) signature.name = node.className;
        const std::string target = node.className + "." + signature.describe();
        const FunctionId constructor = lookupFunction(target);
        if (constructor != kNoFunction) {
            std::vector<Operand> callArguments{receiver};
            for (auto& argument : arguments) callArguments.push_back(std::move(argument));
            if (const Function* constructorFunction = ctx.builder.module().function(constructor)) {
                refineArguments(callArguments, *constructorFunction, 1, loc);
            }
            (void)fb.emitCall(constructor, std::move(callArguments), 0, loc,
                              typeArgumentsFor(constructor, receiver));
        } else if (!arguments.empty()) {
            // No lowered constructor to call: the allocation alone cannot honour
            // the constructor's arguments.
            unsupported(&node, "constructor '" + target + "' which was not lowered");
            return Operand::none();
        }
        return receiver;
    }

    // `Point { x: 10, y: 20 }`. A data record has no constructor to run, so
    // this is an allocation followed by a write per field - the same shape the
    // reference produces, and the reason a record literal needs no lookup into
    // the function table the way `new C(...)` does.
    [[nodiscard]] Operand dataLiteral(const zl::DataLiteralExpr& node) {
        const SourceLocation loc = location(&node);
        const TypeId objectType = typeOfNode(&node);
        const Operand instance = Operand::temp(fb.emitAlloc(node.typeName, {}, objectType, loc), objectType);
        const ClassLayout* layout = ctx.builder.module().classLayout(node.typeName);
        for (const auto& field : node.fields) {
            Operand value = expression(field.second.get());
            if (value.isNone()) return Operand::none();
            // A record literal's field writes are typed boundaries too: a
            // dynamic value is asserted against the declared field type rather
            // than silently stored.
            if (layout) {
                for (const auto& declared : layout->fields) {
                    if (declared.name != field.first) continue;
                    value = coerce(value, declared.type, loc);
                    break;
                }
            }
            if (value.isNone()) return Operand::none();
            fb.emitFieldStore(instance, field.first, value, loc);
        }
        return instance;
    }

    // `base with { field: value, ... }` - an immutable copy-update of a data
    // record. The reference compiles this to CopyObject followed by per-field
    // writes; MIR spells the copy out as an allocation plus one field_load /
    // field_store per field, so the graph shows exactly what is copied and what
    // is replaced rather than hiding the copy inside an opcode.
    [[nodiscard]] Operand dataUpdate(const zl::DataUpdateExpr& node) {
        const SourceLocation loc = location(&node);
        Operand base = expression(node.base.get());
        if (base.isNone()) return Operand::none();
        const TypeId objectType = typeOfNode(&node);
        const Type* baseType = ctx.builder.types().find(objectType);
        if (!baseType || baseType->kind != TypeKind::Object || baseType->name.empty()) {
            unsupported(&node, "`with` update on a non-object receiver");
            return Operand::none();
        }
        const Operand instance = Operand::temp(fb.emitAlloc(baseType->name, {}, objectType, loc), objectType);

        // The field list of the record, own fields then inherited ones. The
        // language forbids redeclaring an inherited field, so a most-derived
        // walk visits each storage slot once; the seen-set is a cycle guard
        // against malformed input rather than an expectation of shadowing.
        std::vector<std::pair<std::string, TypeId>> fields;
        std::unordered_set<std::string> seen;
        std::string current = baseType->name;
        std::size_t guard = 0;
        while (!current.empty() && guard++ < 64) {
            const ClassLayout* layout = ctx.builder.module().classLayout(current);
            if (!layout) break;
            for (const auto& field : layout->fields) {
                if (field.isStatic || !seen.insert(field.name).second) continue;
                fields.emplace_back(field.name, field.type);
            }
            current = layout->parent;
        }
        // Copy every field from the base, then overwrite the named ones.
        for (const auto& field : fields) {
            const Operand value = Operand::temp(fb.emitFieldLoad(base, field.first, field.second, loc),
                                                field.second);
            fb.emitFieldStore(instance, field.first, value, loc);
        }
        for (const auto& update : node.fields) {
            Operand value = expression(update.second.get());
            if (value.isNone()) return Operand::none();
            TypeId fieldType = 0;
            for (const auto& field : fields) {
                if (field.first == update.first) { fieldType = field.second; break; }
            }
            if (fieldType != 0) value = coerce(value, fieldType, loc);
            if (value.isNone()) return Operand::none();
            fb.emitFieldStore(instance, update.first, value, loc);
        }
        return instance;
    }

    [[nodiscard]] Operand fieldAccess(const zl::FieldAccessExpr& node) {
        const SourceLocation loc = location(&node);
        if (node.isEnumMemberAccess) {
            // `Color.RED`. The runtime represents a member as its name, but its
            // static type is the enum - so the constant carries the enum's name
            // and the operand is typed as that enum, not as string. Typing it as
            // string would make every `x == Color.RED` comparison and every call
            // taking an enum member read as a type mismatch.
            const std::string enumTypeName =
                node.object && node.object->kind == zl::NodeKind::Identifier
                    ? static_cast<const zl::Identifier&>(*node.object).name
                    : std::string{};
            if (enumTypeName.empty()) {
                unsupported(&node, "enum member access whose enum name could not be resolved");
                return Operand::none();
            }
            return Operand::constant(ctx.builder.constantEnumMember(enumTypeName, node.fieldName),
                                     ctx.builder.types().objectType(enumTypeName));
        }
        if (node.isMathConstantAccess) {
            // `Math.PI` and friends are namespace values, not fields on an
            // object - there is no receiver to load from, so the value is the
            // whole expression.
            double value = 0.0;
            if (!zl::mathConstantValue(node.fieldName, value)) {
                unsupported(&node, "unknown Math constant '" + node.fieldName + "'");
                return Operand::none();
            }
            return Operand::constant(ctx.builder.constantDouble(value),
                                     ctx.builder.types().doubleType());
        }
        if (node.isFunctionReference) {
            // `Worker.run` as a value: a first-class callable with no captures.
            // The referenced member is a static function, resolved through the
            // class hierarchy exactly as the bytecode compiler resolves it (a
            // subclass may name a static its parent declares). The result type
            // carries the full callable signature - parameter and return types -
            // because the checker recorded them on this node, so later stages
            // can validate arity and argument types without re-deriving them.
            const auto* idNode = node.object && node.object->kind == zl::NodeKind::Identifier
                                     ? static_cast<const zl::Identifier*>(node.object.get())
                                     : nullptr;
            if (!idNode) {
                unsupported(&node, "function reference whose owner could not be resolved");
                return Operand::none();
            }
            const std::string suffix = node.resolvedFunctionDispatch.describe();
            const FunctionId callee = lookupFunctionInClass(idNode->name, suffix);
            if (callee == kNoFunction) {
                unsupported(&node, "function reference to '" + idNode->name + "." + suffix +
                                       "' which was not lowered");
                return Operand::none();
            }
            const TypeId type = typeOfNode(&node);
            return Operand::temp(fb.emitMakeClosure(callee, {}, type, loc), type);
        }
        if (node.isStaticFieldAccess) {
            const TypeId type = typeOfNode(&node);
            return Operand::temp(fb.emitStaticLoad(node.staticFieldClassName, node.fieldName, type, loc), type);
        }
        Operand base = expression(node.object.get());
        if (base.isNone()) return Operand::none();
        const TypeId type = typeOfNode(&node);
        return Operand::temp(fb.emitFieldLoad(base, node.fieldName, type, loc), type);
    }

    [[nodiscard]] Operand fieldAssign(const zl::FieldAssignExpr& node) {
        const SourceLocation loc = location(&node);
        Operand value = expression(node.value.get());
        if (value.isNone()) return Operand::none();
        if (node.isStaticFieldAssign) {
            for (const auto& staticField : ctx.builder.module().statics) {
                if (staticField.name != node.fieldName) continue;
                value = coerce(value, staticField.type, loc);
                break;
            }
            if (value.isNone()) return Operand::none();
            fb.emitStaticStore(node.staticFieldClassName, node.fieldName, value, loc);
            return value;
        }
        Operand base = expression(node.object.get());
        if (base.isNone()) return Operand::none();
        // A field write is a typed boundary: assert a dynamic value against
        // the declared field type instead of storing it unchecked.
        if (const Type* baseType = ctx.builder.types().find(base.type)) {
            if (const ClassLayout* layout = ctx.builder.module().classLayout(baseType->name)) {
                for (const auto& declared : layout->fields) {
                    if (declared.name != node.fieldName) continue;
                    value = coerce(value, declared.type, loc);
                    break;
                }
            }
        }
        if (value.isNone()) return Operand::none();
        fb.emitFieldStore(base, node.fieldName, value, loc);
        return value;
    }

    [[nodiscard]] Operand indexAccess(const zl::IndexAccessExpr& node) {
        const SourceLocation loc = location(&node);
        Operand base = expression(node.object.get());
        if (base.isNone()) return Operand::none();
        Operand index = expression(node.index.get());
        if (index.isNone()) return Operand::none();
        const TypeId type = typeOfNode(&node);
        return Operand::temp(fb.emitIndexLoad(base, index, type, loc), type);
    }

    [[nodiscard]] Operand collectionLiteral(const zl::CollectionLiteral& node) {
        const SourceLocation loc = location(&node);
        TypeId type = typeOfNode(&node);
        const Type* collection = ctx.builder.types().find(type);
        if (!collection || !isCollectionType(*collection)) {
            // An empty literal (`[]`, `{}`) has no elements for the checker to
            // infer a type from, so it records `unknown` even though the call
            // or assignment it sits in fixes a concrete collection type. The
            // AST still knows the literal's shape; build an untyped container
            // of that shape and let the typed boundary refine it, exactly as
            // the reference's runtime type contract does. A heterogeneous
            // literal (`[1, "a"]`) lands here too and is a list.
            const std::string kind = node.isMap ? "map"
                                       : (!node.targetCollectionKind.empty() ? node.targetCollectionKind
                                                                             : "list");
            if (kind == "map") {
                type = ctx.builder.types().mapType(ctx.builder.types().unknownType(),
                                                   ctx.builder.types().unknownType());
            } else if (kind == "set") {
                type = ctx.builder.types().setType(ctx.builder.types().unknownType());
            } else {
                type = ctx.builder.types().listType(ctx.builder.types().unknownType());
            }
            collection = ctx.builder.types().find(type);
        }
        const TempId collection_ = fb.emitNewCollection(type, loc);
        const Operand result = Operand::temp(collection_, type);

        // Element writes are typed boundaries: the collection type's own
        // arguments are the contract, so a dynamic element is asserted against
        // the declared element (or key/value) type here.
        if (node.isMap) {
            const TypeId keyType = collection->arguments.size() > 0 ? collection->arguments[0] : 0;
            const TypeId valueType = collection->arguments.size() > 1 ? collection->arguments[1] : 0;
            for (const auto& entry : node.entries) {
                Operand key = expression(entry.first.get());
                if (key.isNone()) return Operand::none();
                Operand value = expression(entry.second.get());
                if (value.isNone()) return Operand::none();
                if (keyType != 0) key = coerce(key, keyType, loc);
                if (valueType != 0) value = coerce(value, valueType, loc);
                if (key.isNone() || value.isNone()) return Operand::none();
                fb.emitIndexStore(result, key, value, loc);
            }
            if (collection->kind == TypeKind::Object) return result;
            return pinRawCollectionContract(result, type, loc);
        }
        const TypeId elementType = collection->arguments.empty() ? 0 : collection->arguments.front();
        for (std::size_t i = 0; i < node.elements.size(); ++i) {
            Operand value = expression(node.elements[i].get());
            if (value.isNone()) return Operand::none();
            if (elementType != 0) value = coerce(value, elementType, loc);
            if (value.isNone()) return Operand::none();
            const Operand index = Operand::constant(ctx.builder.constantInt(static_cast<std::int64_t>(i)),
                                                    ctx.builder.types().intType());
            fb.emitIndexStore(result, index, value, loc);
        }
        if (collection->kind == TypeKind::Object) return result;
        return pinRawCollectionContract(result, type, loc);
    }

    // A raw native collection (list/map/set/array) is built as an untyped
    // native container plus element writes; nothing else records the literal's
    // declared element contract. Re-asserting the finished container against
    // its declared type pins the storage contract transactionally (the assert
    // descends into the elements, so nested containers inherit theirs), which
    // is what makes later writes enforce the literal's declared element type.
    // Class-layer collections need no pin: their push/add/put methods already
    // run the boundary through the call frame.
    [[nodiscard]] Operand pinRawCollectionContract(const Operand& value, TypeId type, SourceLocation loc) {
        return Operand::temp(fb.emitRefine(value, type, loc), type);
    }

    [[nodiscard]] Operand await(const zl::AwaitExpr& node) {
        const SourceLocation loc = location(&node);
        Operand task = expression(node.operand.get());
        if (task.isNone()) return Operand::none();
        TypeId resultType = typeOfNode(&node);
        if (isVoid(resultType)) {
            // The checker leaves some Task payloads unnamed (`Channel.
            // receiveAsync` observes Task<void> no matter what the channel
            // carries), while lowering names the honest payload on the Task
            // itself. A void await of a non-void payload would contradict the
            // task operand, so the payload is what the temp carries; when the
            // payload really is void there is no temp at all.
            if (const Type* taskType = ctx.builder.types().find(task.type)) {
                if (isTaskType(*taskType)) {
                    const std::uint32_t payload = taskPayloadFor(*taskType);
                    if (payload != 0) {
                        if (const Type* payloadType = ctx.builder.types().find(payload)) {
                            if (payloadType->kind != TypeKind::Void) resultType = payload;
                        }
                    }
                }
            }
        }
        if (isVoid(resultType)) {
            (void)fb.emitAwait(task, 0, loc);
            return Operand::none();
        }
        return Operand::temp(fb.emitAwait(task, resultType, loc), resultType);
    }

    [[nodiscard]] Operand lambda(const zl::LambdaExpr& node) {
        const SourceLocation loc = location(&node);
        const auto it = ctx.lambdaIds.find(&node);
        if (it == ctx.lambdaIds.end()) {
            unsupported(&node, "lambda that was not pre-declared");
            return Operand::none();
        }
        std::vector<Operand> captures;
        for (const auto& captureName : node.captureStorageNames) {
            const LocalRef local = lookupLocal(captureName);
            if (!local.valid()) {
                unsupported(&node, "capture of local that is not in scope");
                return Operand::none();
            }
            captures.push_back(local.isParameter
                                   ? fb.parameterOperand(local.param)
                                   : local.isValue ? local.value : Operand::temp(fb.emitLoad(local.slot, loc),
                                                   fb.function().slot(local.slot)->type));
        }
        // Now that the captured locals are known, the closure function can
        // record their real types instead of the unknown placeholders its
        // declaration needed.
        if (Function* body = ctx.builder.mutableModule().function(it->second)) {
            for (std::size_t i = 0; i < captures.size() && i < body->captures.size(); ++i) {
                body->captures[i].type = captures[i].type;
            }
        }
        const TypeId type = typeOfNode(&node);
        return Operand::temp(fb.emitMakeClosure(it->second, std::move(captures), type, loc), type);
    }

    // --- statements -------------------------------------------------------
    void statement(const zl::AstNode* node) {
        if (!node || failed) return;
        if (isDead()) return; // code after a terminator is unreachable; skip it
        switch (node->kind) {
            case zl::NodeKind::BlockStmt: {
                const auto& block = static_cast<const zl::BlockStmt&>(*node);
                pushScope();
                for (const auto& child : block.statements) statement(child.get());
                popScope();
                return;
            }
            case zl::NodeKind::VarDecl:
                varDecl(static_cast<const zl::VarDecl&>(*node));
                return;
            case zl::NodeKind::LogStmt: {
                const auto& log = static_cast<const zl::LogStmt&>(*node);
                Operand value = expression(log.argument.get());
                if (!value.isNone()) fb.emitLog(value, location(node));
                return;
            }
            case zl::NodeKind::ExprStmt: {
                const auto& statement_ = static_cast<const zl::ExprStmt&>(*node);
                (void)expression(statement_.expression.get());
                return;
            }
            case zl::NodeKind::IfStmt:
                ifStatement(static_cast<const zl::IfStmt&>(*node));
                return;
            case zl::NodeKind::WhileStmt:
                whileStatement(static_cast<const zl::WhileStmt&>(*node));
                return;
            case zl::NodeKind::RepeatStmt:
                repeatStatement(static_cast<const zl::RepeatStmt&>(*node));
                return;
            case zl::NodeKind::ForStmt:
                forStatement(static_cast<const zl::ForStmt&>(*node));
                return;
            case zl::NodeKind::BreakStmt:
                breakStatement(static_cast<const zl::BreakStmt&>(*node));
                return;
            case zl::NodeKind::ContinueStmt:
                continueStatement(static_cast<const zl::ContinueStmt&>(*node));
                return;
            case zl::NodeKind::ReturnStmt:
                returnStatement(static_cast<const zl::ReturnStmt&>(*node));
                return;
            case zl::NodeKind::ThrowStmt:
                throwStatement(static_cast<const zl::ThrowStmt&>(*node));
                return;
            case zl::NodeKind::TryStmt:
                tryStatement(static_cast<const zl::TryStmt&>(*node));
                return;
            default:
                unsupported(node, "statement");
                return;
        }
    }

    void varDecl(const zl::VarDecl& node) {
        const SourceLocation loc = location(&node);
        TypeId type = 0;
        if (node.hasExplicitType) type = types.fromAnnotation(node.type);
        Operand initial;
        if (node.initializer) {
            initial = expression(node.initializer.get());
            if (initial.isNone()) return;
            if (type == 0) type = initial.type;
        } else {
            // `var x` with no initialiser holds nil until assigned.
            if (type == 0) type = ctx.builder.types().nilType();
            initial = Operand::constant(ctx.builder.constantNil(), type);
        }
        // A borrow declaration establishes a region claim rather than copying a
        // value, so its slot is created before the owning value is stored.
        const bool isBorrow = node.ownership == zl::OwnershipKind::BORROW;
        initial = coerce(initial, type, loc);
        if (initial.isNone()) return;

        // A `let` that is never moved out is just a name for the value its
        // initialiser produced - the same rule as a read-only parameter. Giving
        // it a slot would mean storing into storage the language says can never
        // be written to, which the verifier is right to reject. A `let` that IS
        // moved still needs a slot, because move state is tracked per slot.
        const std::string& key = storageKey(node.storageName, node.name);
        if (node.isConst && !isBorrow && writtenLocals.count(key) == 0) {
            bindLocalBoth(node.name, node.storageName, LocalRef::valueRef(initial));
            return;
        }

        const SlotId slot = defineSlot(node.storageName, node.name, type, !node.isConst, node.ownership, &node);
        if (isBorrow) {
            fb.emitBorrow(slot, initial, loc);
            return;
        }
        fb.emitStore(slot, initial, loc);
        movedOwnedSlots.erase(slot);
    }

    void ifStatement(const zl::IfStmt& node) {
        const SourceLocation loc = location(&node);
        const BlockId exitBlock = newBlock(const_cast<zl::IfStmt*>(&node));
        const BlockId elseBody = node.elseBody ? newBlock(const_cast<zl::IfStmt*>(&node)) : kNoBlock;

        for (std::size_t i = 0; i < node.branches.size(); ++i) {
            const BlockId bodyBlock = newBlock(const_cast<zl::IfStmt*>(&node));
            // The false arm is the next condition's own block, so an if/elif
            // chain stays a plain sequence of two-way branches.
            const BlockId falseTarget = (i + 1 < node.branches.size())
                                            ? newBlock(const_cast<zl::IfStmt*>(&node))
                                            : (elseBody != kNoBlock ? elseBody : exitBlock);
            Operand condition = asCondition(expression(node.branches[i].condition.get()),
                                            node.branches[i].condition.get());
            if (failed || condition.isNone()) return;
            fb.emitBranch(condition, bodyBlock, falseTarget, loc);

            gotoBlock(bodyBlock);
            statement(node.branches[i].body.get());
            if (!isDead()) fb.emitJump(exitBlock, loc);
            gotoBlock(falseTarget);
        }
        if (elseBody != kNoBlock) {
            statement(node.elseBody.get());
            if (!isDead()) fb.emitJump(exitBlock, loc);
        }
        gotoBlock(exitBlock);
    }

    void whileStatement(const zl::WhileStmt& node) {
        const SourceLocation loc = location(&node);
        const BlockId conditionBlock = newBlock(const_cast<zl::WhileStmt*>(&node));
        const BlockId bodyBlock = newBlock(const_cast<zl::WhileStmt*>(&node));
        const BlockId exitBlock = newBlock(const_cast<zl::WhileStmt*>(&node));

        fb.emitJump(conditionBlock, loc);
        gotoBlock(conditionBlock);
        Operand condition = asCondition(expression(node.condition.get()), node.condition.get());
        if (failed || condition.isNone()) return;
        fb.emitBranch(condition, bodyBlock, exitBlock, loc);

        gotoBlock(bodyBlock);
        loops.push_back(LoopContext{conditionBlock, exitBlock, activeFinallyBlocks.size(), scopes.size()});
        statement(node.body.get());
        loops.pop_back();
        if (!isDead()) fb.emitJump(conditionBlock, loc);
        gotoBlock(exitBlock);
    }

    void repeatStatement(const zl::RepeatStmt& node) {
        const SourceLocation loc = location(&node);
        const BlockId bodyBlock = newBlock(const_cast<zl::RepeatStmt*>(&node));
        const BlockId conditionBlock = newBlock(const_cast<zl::RepeatStmt*>(&node));
        const BlockId exitBlock = newBlock(const_cast<zl::RepeatStmt*>(&node));

        fb.emitJump(bodyBlock, loc);
        gotoBlock(bodyBlock);
        loops.push_back(LoopContext{conditionBlock, exitBlock, activeFinallyBlocks.size(), scopes.size()});
        statement(node.body.get());
        loops.pop_back();
        if (!isDead()) fb.emitJump(conditionBlock, loc);

        gotoBlock(conditionBlock);
        Operand condition = asCondition(expression(node.condition.get()), node.condition.get());
        if (failed || condition.isNone()) return;
        // repeat/while runs the body again while the condition holds.
        fb.emitBranch(condition, bodyBlock, exitBlock, loc);
        gotoBlock(exitBlock);
    }

    void forStatement(const zl::ForStmt& node) {
        const SourceLocation loc = location(&node);
        Operand start = expression(node.start.get());
        if (start.isNone()) return;
        Operand end = expression(node.end.get());
        if (end.isNone()) return;
        Operand step = expression(node.step.get());
        if (step.isNone()) return;

        const TypeArena& arena = ctx.builder.types();
        const SlotId counter = defineSlot(node.storageName, node.varName, arena.intType(), true,
                                          zl::OwnershipKind::GC, &node);
        const SlotId endSlot = fb.addSlot("__for_end", arena.intType(), true, zl::OwnershipKind::GC, {}, loc);
        const SlotId stepSlot = fb.addSlot("__for_step", arena.intType(), true, zl::OwnershipKind::GC, {}, loc);
        fb.emitStore(counter, coerce(start, arena.intType(), loc), loc);
        fb.emitStore(endSlot, coerce(end, arena.intType(), loc), loc);
        fb.emitStore(stepSlot, coerce(step, arena.intType(), loc), loc);
        if (failed) return;

        const BlockId conditionBlock = newBlock(const_cast<zl::ForStmt*>(&node));
        const BlockId bodyBlock = newBlock(const_cast<zl::ForStmt*>(&node));
        const BlockId stepBlock = newBlock(const_cast<zl::ForStmt*>(&node));
        const BlockId exitBlock = newBlock(const_cast<zl::ForStmt*>(&node));

        fb.emitJump(conditionBlock, loc);
        gotoBlock(conditionBlock);
        const Operand current = Operand::temp(fb.emitLoad(counter, loc), arena.intType());
        const Operand endValue = Operand::temp(fb.emitLoad(endSlot, loc), arena.intType());
        const Operand stepValue = Operand::temp(fb.emitLoad(stepSlot, loc), arena.intType());
        // Direction is a runtime property of the step in ZL, and a zero step is
        // an error rather than an infinite loop. One instruction carries both.
        const Operand keepGoing = Operand::temp(fb.emitRangeInBounds(current, endValue, stepValue, loc),
                                                arena.boolType());
        fb.emitBranch(keepGoing, bodyBlock, exitBlock, loc);

        gotoBlock(bodyBlock);
        loops.push_back(LoopContext{stepBlock, exitBlock, activeFinallyBlocks.size(), scopes.size()});
        statement(node.body.get());
        loops.pop_back();
        if (!isDead()) fb.emitJump(stepBlock, loc);

        gotoBlock(stepBlock);
        const Operand counterValue = Operand::temp(fb.emitLoad(counter, loc), arena.intType());
        // Reload the step here rather than reusing the condition block's
        // `stepValue` temp: the latch is not always dominated by the
        // condition block. When the loop body can only exit through an
        // unwind edge (a try body whose last statement throws, so the only
        // path to the latch runs try -> catch -> after -> latch), a temp
        // defined in the condition block does not dominate its use in the
        // latch, and temporaries do not flow across unwind edges. The
        // counter above already reloads for the same reason.
        const Operand stepReload = Operand::temp(fb.emitLoad(stepSlot, loc), arena.intType());
        const Operand advanced = Operand::temp(fb.emitBinary(Opcode::Add, counterValue, stepReload,
                                                             arena.intType(), loc), arena.intType());
        fb.emitStore(counter, advanced, loc);
        fb.emitJump(conditionBlock, loc);
        gotoBlock(exitBlock);
    }

    void breakStatement(const zl::BreakStmt& node) {
        if (loops.empty()) {
            unsupported(&node, "break outside a loop");
            return;
        }
        const LoopContext& loop = loops.back();
        // A break exits the loop but leaves the current lexical region, so the
        // finalizers opened inside the loop run first, innermost first - the
        // same order the reference emits - and only then the edge to the loop
        // exit removes their handlers (via the normal chain sync).
        for (std::size_t i = activeFinallyBlocks.size(); i > loop.finallyDepth; --i) {
            emitFinallyCleanup(i - 1);
        }
        if (isDead()) return;
        endBorrowsFromScope(loop.scopeDepth, location(&node));
        fb.emitJump(loop.breakTarget, location(&node));
    }

    void continueStatement(const zl::ContinueStmt& node) {
        if (loops.empty()) {
            unsupported(&node, "continue outside a loop");
            return;
        }
        const LoopContext& loop = loops.back();
        for (std::size_t i = activeFinallyBlocks.size(); i > loop.finallyDepth; --i) {
            emitFinallyCleanup(i - 1);
        }
        if (isDead()) return;
        endBorrowsFromScope(loop.scopeDepth, location(&node));
        fb.emitJump(loop.continueTarget, location(&node));
    }

    // Runs the finalizer at `firstIndex` by lowering its body inline at the
    // current point. The body being emitted is removed from the active list
    // while it is lowered, so a return/break/continue written *inside* the
    // finalizer cleans up only the outer finalizers instead of recursing into
    // itself - the same guard the reference uses.
    void emitFinallyCleanup(std::size_t firstIndex) {
        if (firstIndex >= activeFinallyBlocks.size()) return;
        const zl::BlockStmt* block = activeFinallyBlocks[firstIndex];
        const auto saved = activeFinallyBlocks;
        activeFinallyBlocks.assign(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(firstIndex));
        statement(block);
        activeFinallyBlocks = saved;
    }

    // All currently active finalizers, innermost first.
    void emitActiveFinallyCleanup() {
        for (std::size_t i = activeFinallyBlocks.size(); i > 0; --i) emitFinallyCleanup(i - 1);
    }

    // Emits the deterministic end-of-lifetime events for every owned local
    // still holding a value: a `drop` of each owned slot (skipping slots moved
    // out of and never re-initialised) and of each owned parameter. This is
    // the MIR spelling of the reference compiler's owned-local cleanup, placed
    // at the same program point - immediately before control leaves the
    // function - so a backend translates it to the same release the reference
    // emits, and the ownership dataflow sees exactly where each resource dies.
    // GC storage needs no event: the collector owns those values.
    void emitOwnedCleanup(const SourceLocation& loc) {
        const Function& function = fb.function();
        for (std::size_t i = function.slots.size(); i >= 1; --i) {
            const SlotId slot = static_cast<SlotId>(i);
            const Slot* info = function.slot(slot);
            if (!info || info->ownership != zl::OwnershipKind::OWNED) continue;
            if (movedOwnedSlots.count(slot)) continue;
            fb.emitDrop(slot, loc);
        }
        for (std::size_t p = function.parameters.size(); p >= 1; --p) {
            const std::size_t index = p - 1;
            if (function.parameters[index].ownership != zl::OwnershipKind::OWNED) continue;
            fb.emitDrop(fb.parameterOperand(static_cast<ParamId>(index)), loc);
        }
    }

    void returnStatement(const zl::ReturnStmt& node) {
        const SourceLocation loc = location(&node);
        const TypeId returnType = fb.function().returnType;
        if (!node.value) {
            emitActiveFinallyCleanup();
            if (isDead()) return;
            emitOwnedCleanup(loc);
            fb.emitReturn(Operand::none(), loc);
            return;
        }
        Operand value = expression(node.value.get());
        if (value.isNone()) return;
        value = coerce(value, returnType, loc);
        if (value.isNone()) return;
        // A return leaves every region opened up to here, so its finalizers run
        // inner-to-outer before the owned cleanup and the actual return.
        emitActiveFinallyCleanup();
        if (isDead()) return;
        emitOwnedCleanup(loc);
        fb.emitReturn(value, loc);
    }

    void throwStatement(const zl::ThrowStmt& node) {
        Operand value = expression(node.value.get());
        if (value.isNone()) return;
        fb.emitThrow(value, location(&node));
    }

    void tryStatement(const zl::TryStmt& node) {
        const SourceLocation loc = location(&node);
        const bool hasFinally = node.finallyBlock != nullptr;
        if (node.catches.empty() && !hasFinally) {
            unsupported(&node, "try without a catch clause or a finally block");
            return;
        }

        // The handler chain in force just outside this try: the catches (and
        // the finally's cleanup block) run under it, and the block after the
        // try resumes it. Blocks are all created up front so their ids stay in
        // one contiguous run, but their bodies are lowered below in any order.
        const std::vector<ExceptionHandler> savedHandlers = activeHandlers;

        const BlockId afterBlock = newBlock(const_cast<zl::TryStmt*>(&node));
        std::vector<BlockId> catchBlocks;
        std::vector<ExceptionHandler> handlers;
        for (const auto& clause : node.catches) {
            TypeId catchType = 0;
            if (clause.type) catchType = types.fromAnnotation(*clause.type);
            // A catch-all binds the stringified message; a typed catch binds the
            // exception object. Both are immutable bindings.
            const TypeId bindingType = clause.type ? catchType : ctx.builder.types().stringType();
            const SlotId binding = fb.addSlot(storageKey(clause.storageName, clause.varName), bindingType,
                                              false, zl::OwnershipKind::GC, {}, loc);
            fb.function().slots[static_cast<std::size_t>(binding - 1)].isCatchBinding = true;
            const BlockId catchBlock = fb.addBlock(BlockKind::Catch, loc);
            // A catch body runs after this try's handlers were removed on
            // dispatch, so its chain is the one in force just outside the try:
            // a throw inside a catch is caught by the *outer* handlers.
            fb.block(catchBlock).exceptionHandlers = savedHandlers;
            catchBlocks.push_back(catchBlock);
            handlers.push_back(ExceptionHandler{catchType, catchBlock, binding});
        }

        // A finally block adds two blocks. The handler (innermost: appended
        // after the catches so it is searched only when no catch matches) is a
        // catch-all that rethrows after cleanup; its target binds the thrown
        // object into a pending slot, runs the finally body, and throws that
        // slot again. Normal completion runs the same body in a plain block
        // and then falls through to the join.
        BlockId finallyRethrowBlock = kNoBlock;
        BlockId finallyNormalBlock = kNoBlock;
        SlotId pendingSlot = 0;
        if (hasFinally) {
            const TypeId exceptionType = ctx.builder.types().objectType("Exception");
            pendingSlot = fb.addSlot("__finally_pending", exceptionType, true, zl::OwnershipKind::GC, {}, loc);
            fb.function().slots[static_cast<std::size_t>(pendingSlot - 1)].isCatchBinding = true;
            finallyRethrowBlock = fb.addBlock(BlockKind::Cleanup, loc);
            // The cleanup block also runs after dispatch, under the outer
            // chain only.
            fb.block(finallyRethrowBlock).exceptionHandlers = savedHandlers;
            finallyNormalBlock = newBlock(const_cast<zl::TryStmt*>(&node));
            ExceptionHandler finallyHandler{0, finallyRethrowBlock, pendingSlot};
            finallyHandler.isFinally = true;
            handlers.push_back(finallyHandler);
        }

        // The try body runs with the new chain installed: outer handlers, then
        // this try's catches, then its finally handler.
        activeHandlers = savedHandlers;
        for (const auto& handler : handlers) activeHandlers.push_back(handler);
        const BlockId tryBlock = newBlock(const_cast<zl::TryStmt*>(&node));
        fb.emitJump(tryBlock, loc);
        gotoBlock(tryBlock);
        if (hasFinally) activeFinallyBlocks.push_back(static_cast<const zl::BlockStmt*>(node.finallyBlock.get()));
        statement(node.tryBlock.get());
        if (hasFinally) activeFinallyBlocks.pop_back();
        if (!isDead()) fb.emitJump(hasFinally ? finallyNormalBlock : afterBlock, loc);
        activeHandlers = savedHandlers;

        for (std::size_t i = 0; i < node.catches.size(); ++i) {
            gotoBlock(catchBlocks[i]);
            pushScope();
            bindLocal(storageKey(node.catches[i].storageName, node.catches[i].varName),
                      LocalRef::slotRef(handlers[i].catchSlot));
            if (hasFinally) activeFinallyBlocks.push_back(static_cast<const zl::BlockStmt*>(node.finallyBlock.get()));
            statement(node.catches[i].block.get());
            if (hasFinally) activeFinallyBlocks.pop_back();
            popScope();
            if (!isDead()) fb.emitJump(hasFinally ? finallyNormalBlock : afterBlock, loc);
        }

        if (hasFinally) {
            gotoBlock(finallyNormalBlock);
            statement(node.finallyBlock.get());
            if (!isDead()) fb.emitJump(afterBlock, loc);

            gotoBlock(finallyRethrowBlock);
            statement(node.finallyBlock.get());
            if (!isDead()) {
                const TypeId exceptionType = ctx.builder.types().objectType("Exception");
                const Operand pending = Operand::temp(fb.emitLoad(pendingSlot, loc), exceptionType);
                fb.emitThrow(pending, loc);
            }
        }
        gotoBlock(afterBlock);
    }

    // --- helpers ----------------------------------------------------------
    void setLastInstructionName(const std::vector<std::string>& names) {
        if (names.empty()) return;
        auto& instructions = fb.block(fb.currentBlock()).instructions;
        if (instructions.empty()) return;
        std::string joined = names.front();
        for (std::size_t i = 1; i < names.size(); ++i) {
            joined += ';';
            joined += names[i];
        }
        instructions.back().name = std::move(joined);
    }

    [[nodiscard]] FunctionId lookupFunction(const std::string& qualifiedName) const {
        const auto it = ctx.functionIds.find(qualifiedName);
        if (it != ctx.functionIds.end()) return it->second;
        // Inherited methods resolve through the parent chain, the same way the
        // bytecode compiler resolves them.
        std::string current = ownerClass;
        std::size_t guard = 0;
        while (!current.empty() && guard++ < 64) {
            const auto parent = ctx.classParents.find(current);
            if (parent == ctx.classParents.end()) break;
            current = parent->second;
            const auto dot = qualifiedName.find('.');
            const std::string candidate = current + (dot == std::string::npos ? "" : qualifiedName.substr(dot));
            const auto found = ctx.functionIds.find(candidate);
            if (found != ctx.functionIds.end()) return found->second;
        }
        return kNoFunction;
    }

    [[nodiscard]] FunctionId lookupFunctionIn(const std::string& className,
                                              const std::string& signatureSuffix) const {
        const auto found = ctx.functionIds.find(className + "." + signatureSuffix);
        return found == ctx.functionIds.end() ? kNoFunction : found->second;
    }

    // Resolves a function referenced by its declaring class, walking the
    // inheritance chain the same way the bytecode compiler does. A static
    // member reference such as `Child.helper` must resolve to `Parent.helper`
    // when `Parent` declares it and `Child` only inherits it.
    [[nodiscard]] FunctionId lookupFunctionInClass(const std::string& className,
                                                   const std::string& signatureSuffix) const {
        std::string current = className;
        std::size_t guard = 0;
        while (!current.empty() && guard++ < 64) {
            const FunctionId found = lookupFunctionIn(current, signatureSuffix);
            if (found != kNoFunction) return found;
            const auto parent = ctx.classParents.find(current);
            if (parent == ctx.classParents.end()) break;
            current = parent->second;
        }
        return kNoFunction;
    }

    // The instantiation a call site selected. A MIR function stays a generic
    // template, so a call into a generic class has to say which arguments it
    // used; the receiver's own type is where the source recorded them.
    [[nodiscard]] std::vector<TypeId> typeArgumentsFor(FunctionId callee, const Operand& receiver) const {
        const Function* function = ctx.builder.module().function(callee);
        if (!function || function->typeParameters.empty()) return {};
        const Type* type = ctx.builder.types().find(receiver.type);
        if (!type || type->arguments.size() != function->typeParameters.size()) return {};
        return type->arguments;
    }

    [[nodiscard]] std::string operandTypeName(const Operand& operand) const {
        const Type* type = ctx.builder.types().find(operand.type);
        return type ? type->name : std::string{};
    }

    [[nodiscard]] zl::ZlType zlTypeOf(TypeId id) const {
        const Type* type = ctx.builder.types().find(id);
        if (!type) return zl::ZlType::UNKNOWN;
        switch (type->kind) {
            case TypeKind::Bool: return zl::ZlType::BOOL;
            case TypeKind::Int: return zl::ZlType::INT;
            case TypeKind::Double: return zl::ZlType::DOUBLE;
            case TypeKind::String: return zl::ZlType::STRING;
            case TypeKind::Nil: return zl::ZlType::NIL;
            case TypeKind::Void: return zl::ZlType::VOID_TYPE;
            case TypeKind::List: return zl::ZlType::LIST;
            case TypeKind::Map: return zl::ZlType::MAP;
            case TypeKind::Set: return zl::ZlType::SET;
            case TypeKind::Array: return zl::ZlType::ARRAY;
            case TypeKind::Task: return zl::ZlType::TASK;
            case TypeKind::Function: return zl::ZlType::FUNCTION;
            case TypeKind::Union: return zl::ZlType::UNION;
            default: return zl::ZlType::OBJECT;
        }
    }

    [[nodiscard]] TypeId typeIdFor(zl::ZlType type) const {
        return types.primitiveFor(type);
    }

    [[nodiscard]] bool isVoid(TypeId id) const {
        const Type* type = ctx.builder.types().find(id);
        return type && type->kind == TypeKind::Void;
    }

    [[nodiscard]] static Opcode binaryOpcode(zl::TokenType token) {
        switch (token) {
            case zl::TokenType::PLUS: return Opcode::Add;
            case zl::TokenType::MINUS: return Opcode::Sub;
            case zl::TokenType::STAR: return Opcode::Mul;
            case zl::TokenType::SLASH: return Opcode::Div;
            case zl::TokenType::PERCENT: return Opcode::Mod;
            case zl::TokenType::POW: return Opcode::Pow;
            case zl::TokenType::BIT_AND: return Opcode::BitAnd;
            case zl::TokenType::BIT_OR: return Opcode::BitOr;
            case zl::TokenType::BIT_XOR: return Opcode::BitXor;
            case zl::TokenType::SHL: return Opcode::Shl;
            case zl::TokenType::SHR: return Opcode::Shr;
            case zl::TokenType::USHR: return Opcode::Ushr;
            case zl::TokenType::EQ: return Opcode::Eq;
            case zl::TokenType::NEQ: return Opcode::Ne;
            case zl::TokenType::LT: return Opcode::Lt;
            case zl::TokenType::LTE: return Opcode::Le;
            case zl::TokenType::GT: return Opcode::Gt;
            case zl::TokenType::GTE: return Opcode::Ge;
            default: return Opcode::Nop;
        }
    }

    [[nodiscard]] static zl::Operator operatorFor(Opcode opcode) {
        switch (opcode) {
            case Opcode::Add: return zl::Operator::Plus;
            case Opcode::Sub: return zl::Operator::Minus;
            case Opcode::Mul: return zl::Operator::Multiply;
            case Opcode::Div: return zl::Operator::Divide;
            case Opcode::Mod: return zl::Operator::Modulo;
            case Opcode::Pow: return zl::Operator::Power;
            case Opcode::BitAnd: return zl::Operator::BitAnd;
            case Opcode::BitOr: return zl::Operator::BitOr;
            case Opcode::BitXor: return zl::Operator::BitXor;
            case Opcode::Shl: return zl::Operator::ShiftLeft;
            case Opcode::Shr: return zl::Operator::ShiftRight;
            case Opcode::Ushr: return zl::Operator::ShiftRightZero;
            case Opcode::Eq: return zl::Operator::Equal;
            case Opcode::Ne: return zl::Operator::NotEqual;
            case Opcode::Lt: return zl::Operator::Less;
            case Opcode::Le: return zl::Operator::LessEqual;
            case Opcode::Gt: return zl::Operator::Greater;
            case Opcode::Ge: return zl::Operator::GreaterEqual;
            default: return zl::Operator::Unknown;
        }
    }

    // Every condition position in ZL takes a bool. When semantic analysis
    // resolved the expression to bool this is a no-op; when it left the type
    // unknown - which happens for a call through an unparameterised `func` -
    // the MIR records the required type with an explicit refine rather than
    // branching on a value with no type at all.
    // A match arm's literal, as an operand. Patterns carry the token text
    // rather than a Literal node, so this is the same parsing `constOperand`
    // does, keyed off the arm instead of off an AST node.
    [[nodiscard]] Operand patternValue(const zl::MatchExpr::Pattern& pattern, TypeId subjectType) {
        switch (pattern.kind) {
            case zl::MatchExpr::PatternKind::Literal: {
                switch (pattern.literalType) {
                    case zl::TokenType::INT_LITERAL: {
                        std::int64_t value = 0;
                        try { value = std::stoll(pattern.raw); } catch (...) { value = 0; }
                        return Operand::constant(ctx.builder.constantInt(value), ctx.builder.types().intType());
                    }
                    case zl::TokenType::DECIMAL_LITERAL:
                    case zl::TokenType::FLOAT_LITERAL: {
                        double value = 0.0;
                        try { value = std::stod(pattern.raw); } catch (...) { value = 0.0; }
                        return Operand::constant(ctx.builder.constantDouble(value),
                                                 ctx.builder.types().doubleType());
                    }
                    case zl::TokenType::BOOL_LITERAL:
                        return Operand::constant(ctx.builder.constantBool(pattern.raw == "true"),
                                                 ctx.builder.types().boolType());
                    case zl::TokenType::STRING_LITERAL:
                        return Operand::constant(ctx.builder.constantString(pattern.raw),
                                                 ctx.builder.types().stringType());
                    case zl::TokenType::KW_NULL:
                        // `null` in a pattern is a value of the subject's type;
                        // typing it bare nil would make the comparison read as
                        // comparing different types.
                        return Operand::constant(ctx.builder.constantNil(),
                                                 subjectType ? subjectType : ctx.builder.types().nilType());
                    default:
                        unsupported(nullptr, "match literal '" + pattern.raw + "'");
                        return Operand::none();
                }
            }
            case zl::MatchExpr::PatternKind::EnumMember:
                return Operand::constant(
                    ctx.builder.constantEnumMember(pattern.enumTypeName, pattern.enumMemberName),
                    ctx.builder.types().objectType(pattern.enumTypeName));
            default:
                unsupported(nullptr, "pattern value for a non-literal pattern");
                return Operand::none();
        }
    }

    [[nodiscard]] Operand callNative(const std::string& qualifiedName, std::vector<Operand> arguments,
                                     TypeId resultType, SourceLocation loc) {
        const auto native = zl::findNativeSignature(qualifiedName);
        if (!native) {
            unsupported(nullptr, "native '" + qualifiedName + "' which is not in the catalog");
            return Operand::none();
        }
        return Operand::temp(fb.emitCallNative(qualifiedName, static_cast<std::int32_t>((*native)->id),
                                               std::move(arguments), resultType,
                                               (*native)->taskValueType != zl::ZlType::UNKNOWN, loc),
                             resultType);
    }

    // The element/key/value type a structural pattern inspects, drawn from the
    // subject's own type arguments so the tests stay typed without guessing.
    [[nodiscard]] TypeId elementTypeOf(const Type* type) const {
        return type && !type->arguments.empty() ? type->arguments.front() : ctx.builder.types().unknownType();
    }
    [[nodiscard]] TypeId keyTypeOf(const Type* type) const {
        return type && !type->arguments.empty() ? type->arguments.front() : ctx.builder.types().unknownType();
    }
    [[nodiscard]] TypeId valueTypeOf(const Type* type) const {
        return type && type->arguments.size() >= 2 ? type->arguments[1] : ctx.builder.types().unknownType();
    }
    // The declared type of a field on a class/data layout, walking the parent
    // chain exactly as the verifier does for field access.
    [[nodiscard]] TypeId fieldTypeOf(TypeId objectType, const std::string& fieldName) const {
        const Type* type = ctx.builder.types().find(objectType);
        std::string current = type ? type->name : std::string{};
        std::size_t guard = 0;
        while (!current.empty() && guard++ < 64) {
            const ClassLayout* layout = ctx.builder.module().classLayout(current);
            if (!layout) break;
            if (const FieldLayout* field = layout->field(fieldName)) return field->type;
            current = layout->parent;
        }
        return ctx.builder.types().unknownType();
    }

    // Lowers the test chain for one pattern against `subject`. Every point
    // where the pattern cannot match branches to `fail`; when the whole pattern
    // matches, control continues at `success`. Bindings named by the pattern are
    // bound into the current scope under their storage names.
    [[nodiscard]] bool lowerMatchPattern(const zl::MatchExpr::Pattern& pattern, const Operand& subject,
                                         BlockId success, BlockId fail,
                                         const zl::MatchExpr::Arm& arm, SourceLocation loc) {
        const TypeId boolType = ctx.builder.types().boolType();
        const TypeId intType = ctx.builder.types().intType();
        const Type* subjectType = ctx.builder.types().find(subject.type);

        switch (pattern.kind) {
            case zl::MatchExpr::PatternKind::Wildcard:
                break; // matches any value
            case zl::MatchExpr::PatternKind::Variable: {
                const auto storage = arm.storageBindings.find(pattern.bindingName);
                bindPatternBinding(pattern.bindingName,
                                   storage == arm.storageBindings.end() ? std::string{} : storage->second,
                                   subject, arm, loc);
                break;
            }
            case zl::MatchExpr::PatternKind::Literal:
            case zl::MatchExpr::PatternKind::EnumMember: {
                Operand value = patternValue(pattern, subject.type);
                if (value.isNone()) return false;
                Operand test = Operand::temp(fb.emitBinary(Opcode::Eq, subject, value, boolType, loc), boolType);
                const BlockId next = newBlock(nullptr);
                fb.emitBranch(test, next, fail, loc);
                gotoBlock(next);
                break;
            }
            case zl::MatchExpr::PatternKind::Type: {
                const TypeId narrowed = types.fromAnnotation(pattern.typePattern);
                if (narrowed == 0) {
                    unsupported(nullptr, "type pattern '" + pattern.typePattern.name + "'");
                    return false;
                }
                Operand test = Operand::temp(fb.emitTypeTest(subject, narrowed, loc), boolType);
                const BlockId next = newBlock(nullptr);
                fb.emitBranch(test, next, fail, loc);
                gotoBlock(next);
                if (!pattern.bindingName.empty() && pattern.bindingName != "_") {
                    Operand value = subject;
                    if (narrowed != subject.type) {
                        value = Operand::temp(fb.emitRefine(subject, narrowed, loc), narrowed);
                    }
                    const auto storage = arm.storageBindings.find(pattern.bindingName);
                    bindPatternBinding(pattern.bindingName,
                                       storage == arm.storageBindings.end() ? std::string{} : storage->second,
                                       value, arm, loc);
                }
                break;
            }
            case zl::MatchExpr::PatternKind::Data: {
                const TypeId narrowed = ctx.builder.types().objectType(pattern.typePattern.name);
                Operand test = Operand::temp(fb.emitTypeTest(subject, narrowed, loc), boolType);
                const BlockId next = newBlock(nullptr);
                fb.emitBranch(test, next, fail, loc);
                gotoBlock(next);
                for (const auto& field : pattern.fields) {
                    if (!field.pattern) continue;
                    const TypeId fieldType = fieldTypeOf(narrowed, field.fieldName);
                    Operand fieldValue = Operand::temp(fb.emitFieldLoad(subject, field.fieldName, fieldType, loc),
                                                       fieldType);
                    const BlockId after = newBlock(nullptr);
                    if (!lowerMatchPattern(*field.pattern, fieldValue, after, fail, arm, loc)) return false;
                    gotoBlock(after);
                }
                break;
            }
            case zl::MatchExpr::PatternKind::List: {
                const bool isSet = pattern.containerKind == "set" || pattern.containerKind == "Set";
                Operand length = callNative("Collection.length", {subject}, intType, loc);
                if (length.isNone()) return false;
                const Operand size = Operand::constant(
                    ctx.builder.constantInt(static_cast<std::int64_t>(pattern.elements.size())), intType);
                Operand same = Operand::temp(fb.emitBinary(Opcode::Eq, length, size, boolType, loc), boolType);
                const BlockId next = newBlock(nullptr);
                fb.emitBranch(same, next, fail, loc);
                gotoBlock(next);
                if (isSet) {
                    for (const auto& child : pattern.elements) {
                        if (!child || child->kind == zl::MatchExpr::PatternKind::Wildcard) continue;
                        Operand value = patternValue(*child, elementTypeOf(subjectType));
                        if (value.isNone()) return false;
                        Operand has = callNative("Collection.setHas", {subject, value}, boolType, loc);
                        if (has.isNone()) return false;
                        const BlockId after = newBlock(nullptr);
                        fb.emitBranch(has, after, fail, loc);
                        gotoBlock(after);
                    }
                } else {
                    const TypeId elementType = elementTypeOf(subjectType);
                    for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
                        if (!pattern.elements[i]) continue;
                        const Operand index = Operand::constant(ctx.builder.constantInt(static_cast<std::int64_t>(i)),
                                                                intType);
                        Operand item = Operand::temp(fb.emitIndexLoad(subject, index, elementType, loc), elementType);
                        const BlockId after = newBlock(nullptr);
                        if (!lowerMatchPattern(*pattern.elements[i], item, after, fail, arm, loc)) return false;
                        gotoBlock(after);
                    }
                }
                break;
            }
            case zl::MatchExpr::PatternKind::Map: {
                Operand length = callNative("Collection.length", {subject}, intType, loc);
                if (length.isNone()) return false;
                const Operand size = Operand::constant(
                    ctx.builder.constantInt(static_cast<std::int64_t>(pattern.mapEntries.size())), intType);
                Operand same = Operand::temp(fb.emitBinary(Opcode::Eq, length, size, boolType, loc), boolType);
                const BlockId next = newBlock(nullptr);
                fb.emitBranch(same, next, fail, loc);
                gotoBlock(next);
                for (const auto& entry : pattern.mapEntries) {
                    if (!entry.key) continue;
                    Operand key = patternValue(*entry.key, keyTypeOf(subjectType));
                    if (key.isNone()) return false;
                    Operand has = callNative("Collection.mapHas", {subject, key}, boolType, loc);
                    if (has.isNone()) return false;
                    const BlockId after = newBlock(nullptr);
                    fb.emitBranch(has, after, fail, loc);
                    gotoBlock(after);
                    if (entry.value) {
                        const TypeId valueType = valueTypeOf(subjectType);
                        Operand value = Operand::temp(fb.emitIndexLoad(subject, key, valueType, loc), valueType);
                        const BlockId afterValue = newBlock(nullptr);
                        if (!lowerMatchPattern(*entry.value, value, afterValue, fail, arm, loc)) return false;
                        gotoBlock(afterValue);
                    }
                }
                break;
            }
        }
        if (failed) return false;
        fb.emitJump(success, loc);
        return true;
    }

    // Assembles the top-level pattern for an arm from the arm's own fields, so
    // a literal arm and a destructuring arm lower through the same recursive
    // walk.
    [[nodiscard]] zl::MatchExpr::Pattern armPattern(const zl::MatchExpr::Arm& arm) const {
        zl::MatchExpr::Pattern root;
        root.kind = arm.patternKind;
        root.raw = arm.raw;
        root.literalType = arm.literalType;
        root.enumTypeName = arm.enumTypeName;
        root.enumMemberName = arm.enumMemberName;
        root.typePattern = arm.typePattern;
        root.bindingName = arm.bindingName;
        root.containerKind = arm.containerKind;
        root.positional = arm.positional;
        root.line = arm.line;
        for (const auto& field : arm.dataFields) {
            zl::MatchExpr::DataFieldPattern copied;
            copied.fieldName = field.fieldName;
            copied.line = field.line;
            copied.pattern = field.pattern ? std::make_unique<zl::MatchExpr::Pattern>(
                                                 cloneMatchPattern(*field.pattern))
                                           : nullptr;
            root.fields.push_back(std::move(copied));
        }
        for (const auto& child : arm.listElements) {
            root.elements.push_back(child ? std::make_unique<zl::MatchExpr::Pattern>(
                                                cloneMatchPattern(*child))
                                          : nullptr);
        }
        for (const auto& entry : arm.mapEntries) {
            zl::MatchExpr::MapEntryPattern copied;
            copied.line = entry.line;
            copied.key = entry.key ? std::make_unique<zl::MatchExpr::Pattern>(
                                         cloneMatchPattern(*entry.key))
                                   : nullptr;
            copied.value = entry.value ? std::make_unique<zl::MatchExpr::Pattern>(
                                             cloneMatchPattern(*entry.value))
                                       : nullptr;
            root.mapEntries.push_back(std::move(copied));
        }
        return root;
    }

    // `match` lowers to a chain of two-way branches, one test block per arm:
    // test the arm, run it on success, fall through to the next arm's test on
    // failure. That is the shape the bytecode compiler produces, kept here so
    // the two backends cannot drift apart on which arm wins. Structural
    // patterns nest the same shape: a field/element/value sub-pattern is its
    // own branch chain, and a failed sub-pattern falls through to the arm's
    // next sibling or the next arm's test.
    [[nodiscard]] Operand matchExpr(const zl::MatchExpr& node) {
        const SourceLocation loc = location(&node);
        // The subject is evaluated exactly once, before any arm runs. A guard
        // that reassigns the variable the subject was read from therefore
        // cannot change what a later arm compares against, and an arm cannot
        // observe a side effect of another arm's test.
        Operand subject = expression(node.subject.get());
        if (subject.isNone()) return Operand::none();

        const TypeId resultType = typeOfNode(&node);
        // A `match` in statement position has no value - every arm is a `log`,
        // an assignment, a `throw`. MIR has no value of type void, so such a
        // match owns no result slot at all: the arms run their bodies and jump
        // straight to the join block, which is then simply where the statement
        // ends. Declaring a void-typed slot (or storing the nil constant into
        // one) is exactly the contract violation the verifier reports.
        const bool returnsVoid = isVoid(resultType);
        // Arms produce their value in different blocks, so the result travels
        // through a slot - the same join mechanism `try` uses. There is no phi
        // in this IR, and inventing one for one construct would leave every
        // other join still going through a slot.
        // Mutable because every arm writes it, even though the source cannot
        // observe an intermediate value: the join block reads it only after
        // exactly one arm has stored.
        const SlotId resultSlot = returnsVoid
            ? kNoSlot
            : fb.addSlot("", resultType, true, zl::OwnershipKind::GC, {}, loc);
        const BlockId joinBlock = newBlock(const_cast<zl::MatchExpr*>(&node));
        // Where the last arm's failed test lands. The checker proves static
        // coverage, so only a value it could not classify reaches here; the
        // reference raises rather than yielding an undefined result, and so
        // does this.
        const BlockId noMatchBlock = newBlock(const_cast<zl::MatchExpr*>(&node));
        const TypeId boolType = ctx.builder.types().boolType();

        BlockId nextTest = newBlock(const_cast<zl::MatchExpr*>(&node));
        fb.emitJump(nextTest, loc);

        for (std::size_t i = 0; i < node.arms.size(); ++i) {
            const auto& arm = node.arms[i];
            SourceLocation armLoc = loc;
            armLoc.line = static_cast<std::uint32_t>(arm.line);
            const bool lastArm = i + 1 == node.arms.size();
            const BlockId bodyBlock = newBlock(const_cast<zl::MatchExpr*>(&node));
            const BlockId following = lastArm ? noMatchBlock : newBlock(const_cast<zl::MatchExpr*>(&node));
            const BlockId passedGuard = arm.guard ? newBlock(const_cast<zl::MatchExpr*>(&node)) : kNoBlock;
            const BlockId success = arm.guard ? passedGuard : bodyBlock;

            // Bindings named anywhere in the pattern are scoped to the arm, so
            // the scope opens before the test chain runs and closes after the
            // arm's body.
            pushScope();

            // --- the arm's test chain -------------------------------------
            gotoBlock(nextTest);
            if (!lowerMatchPattern(armPattern(arm), subject, success, following, arm, armLoc)) {
                popScope();
                return Operand::none();
            }

            // A type, data, or enum pattern narrows the *subject* identifier
            // too, so the arm body may keep spelling the subject's name and
            // mean the matched member. Only a subject whose static type the
            // pattern actually narrows gets the refine.
            if (node.subject && node.subject->kind == zl::NodeKind::Identifier) {
                const auto& subjectId = static_cast<const zl::Identifier&>(*node.subject);
                TypeId narrowed = subject.type;
                if (arm.patternKind == zl::MatchExpr::PatternKind::Type) {
                    narrowed = types.fromAnnotation(arm.typePattern);
                } else if (arm.patternKind == zl::MatchExpr::PatternKind::Data) {
                    narrowed = ctx.builder.types().objectType(arm.typePattern.name);
                } else if (arm.patternKind == zl::MatchExpr::PatternKind::EnumMember) {
                    narrowed = ctx.builder.types().objectType(arm.enumTypeName);
                }
                if (narrowed != 0 && narrowed != subject.type) {
                    bindLocalBoth(subjectId.name, subjectId.storageName, LocalRef::valueRef(
                        Operand::temp(fb.emitRefine(subject, narrowed, armLoc), narrowed)));
                }
            }

            // --- the arm's guard ------------------------------------------
            if (arm.guard) {
                gotoBlock(passedGuard);
                Operand guard = asCondition(expression(arm.guard.get()), arm.guard.get());
                if (failed || guard.isNone()) { popScope(); return Operand::none(); }
                fb.emitBranch(guard, bodyBlock, following, armLoc);
            }

            // --- the arm's body -------------------------------------------
            gotoBlock(bodyBlock);
            Operand value = expression(arm.result.get());
            // A void arm legitimately produces no operand - the same `none` a
            // void-returning method call yields - so only a failed lowering
            // aborts here. A match with a result still requires the operand.
            if (failed || (!returnsVoid && value.isNone())) { popScope(); return Operand::none(); }
            if (!returnsVoid) fb.emitStore(resultSlot, coerce(value, resultType, armLoc), armLoc);
            fb.emitJump(joinBlock, armLoc);
            popScope();

            gotoBlock(following);
            nextTest = following;
        }

        // No arm matched.
        const TypeId exceptionType = ctx.builder.types().objectType("Exception");
        const Operand instance = Operand::temp(fb.emitAlloc("Exception", {}, exceptionType, loc), exceptionType);
        const FunctionId constructor = lookupFunctionIn("Exception", "Exception(string)");
        if (constructor != kNoFunction) {
            Operand message = Operand::constant(ctx.builder.constantString("non-exhaustive match"),
                                                ctx.builder.types().stringType());
            (void)fb.emitCall(constructor, {instance, message}, 0, loc, {});
        }
        fb.emitThrow(instance, loc);

        gotoBlock(joinBlock);
        if (returnsVoid) return Operand::constant(ctx.builder.constantNil(), ctx.builder.types().voidType());
        return Operand::temp(fb.emitLoad(resultSlot, loc), resultType);
    }

    [[nodiscard]] Operand asCondition(Operand value, const zl::AstNode* node) {
        if (value.isNone()) return Operand::none();
        const Type* type = ctx.builder.types().find(value.type);
        if (!type) return value;
        if (type->kind == TypeKind::Bool) return value;
        if (type->kind == TypeKind::Unknown) {
            return Operand::temp(fb.emitRefine(value, ctx.builder.types().boolType(), location(node)),
                                 ctx.builder.types().boolType());
        }
        unsupported(node, "condition of type " + ctx.builder.types().render(value.type) + " (expected bool)");
        return Operand::none();
    }

    // Inserts the conversions the language performs implicitly, so MIR never
    // carries a boundary it does not name:
    //
    //   * int -> double widening, so arithmetic is homogeneous;
    //   * nil into any nullable type, which is a plain assignment in ZL;
    //   * unknown -> concrete, the dynamic-to-static boundary. This is NOT a
    //     silent retype: it emits Refine, the runtime type assertion, so the
    //     graph shows `dynamic value -> refine -> statically typed value` and
    //     a value whose shape does not match the destination fails loudly
    //     instead of being trusted. Every consumer of MIR - optimiser,
    //     backend, analyser - sees exactly where the checked boundaries are.
    // True when every type-parameter token inside `type` is one of this
    // function's own type parameters. The VM's AssertType resolves tokens
    // lexically against the *frame's* bindings, so an assertion is only
    // meaningful when the frame binds each token: a dynamic store into a
    // `T`-typed local inside a `Cell<T>` method checks against the
    // instantiation's concrete T, but a generic native's declared parameters
    // (Collection.push's `list<T>`/`T`) seen from a non-generic caller name a
    // token that frame has no binding for - asserting against it would test
    // the literal name "T" (matching nothing) or pin a container's storage
    // contract to it.
    [[nodiscard]] bool tokensBoundHere(const Type* type, std::size_t depth) const {
        if (!type || depth > 16) return true;
        if (type->kind == TypeKind::TypeParam) {
            return std::find(typeParams.begin(), typeParams.end(), type->name) != typeParams.end();
        }
        if (type->kind == TypeKind::Object && !type->name.empty() && type->name != "object" &&
            type->arguments.empty() && !ctx.builder.module().classLayout(type->name)) {
            // fromName renders an unbound catalog token as a bare object type
            // (`T` -> object "T"), so a bare identifier naming no class in the
            // module is the same unbound-token case. "object" is the builtin
            // dynamic top type (and an empty name is the anonymous object),
            // neither of which is a token.
            return std::find(typeParams.begin(), typeParams.end(), type->name) != typeParams.end();
        }
        for (const std::uint32_t argument : type->arguments) {
            if (!tokensBoundHere(ctx.builder.types().find(argument), depth + 1)) return false;
        }
        return true;
    }

    [[nodiscard]] Operand coerce(Operand value, TypeId target, SourceLocation loc) {
        if (value.isNone() || value.type == target) return value;
        const Type* from = ctx.builder.types().find(value.type);
        const Type* to = ctx.builder.types().find(target);
        if (!from || !to) return value;
        if (from->kind == TypeKind::Int && to->kind == TypeKind::Double) {
            return Operand::temp(fb.emitWiden(value, loc), target);
        }
        if (from->kind == TypeKind::Nil && isNullableKind(to->kind)) return value;
        if (from->kind == TypeKind::Unknown && to->kind != TypeKind::Unknown &&
            to->kind != TypeKind::Void && tokensBoundHere(to, 0)) {
            // The target may name this function's own type parameters: the VM
            // substitutes them from the frame at the assert, which is exactly
            // the language's `try { t = dynamic } catch ...` contract for
            // generic methods.
            return Operand::temp(fb.emitRefine(value, target, loc), target);
        }
        // Anything else is the checker's business, not the lowerer's: it already
        // accepted the program, so the value is usable where the target expects.
        return value;
    }

    // Refines call arguments against the callee's declared parameter types.
    // Template parameters are skipped: a TypeParam stands for a type only the
    // instantiation knows, and asserting one at the template would be a lie
    // the verifier rejects. Mutates `arguments` in place.
    void refineArguments(std::vector<Operand>& arguments, const Function& callee,
                         std::size_t skipLeading, SourceLocation loc) {
        // skipLeading operand slots (a constructor's receiver) are skipped on
        // BOTH sides: arguments[i + skipLeading] is the value for
        // parameters[i + skipLeading]. Indexing parameters by i alone paired
        // the first real argument with the receiver's own `this: C<T>` type,
        // and a generic constructor's unknown argument was then "refined" to
        // the class type - an assertion the language never makes
        // (`new Box<int>(transform(...))` asserted Box<int> against an int).
        for (std::size_t i = 0; i + skipLeading < arguments.size() && i + skipLeading < callee.parameters.size(); ++i) {
            const TypeId parameterType = callee.parameters[i + skipLeading].type;
            const Type* parameter = ctx.builder.types().find(parameterType);
            if (!parameter) continue;
            // The parameter ASSERTION is not the caller's: the callee's call
            // frame checks every argument against the declared parameter
            // types in one transaction (committing the element contracts the
            // checks pin only when the whole argument batch passes - a failed
            // batch must leave its arguments unbound), and it throws the
            // caller's `catch` sees. Emitting one assert per argument at the
            // call site would commit each one as it passes, so a batch that
            // fails on argument N leaves arguments 1..N-1 pinned - the
            // rollback the language's contract requires. What the caller still
            // owes is value conversion, not assertion: int -> double.
            const Type* from = ctx.builder.types().find(arguments[i + skipLeading].type);
            if (from && from->kind == TypeKind::Int && parameter->kind == TypeKind::Double) {
                arguments[i + skipLeading] =
                    Operand::temp(fb.emitWiden(arguments[i + skipLeading], loc), parameterType);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Program-level lowering
// ---------------------------------------------------------------------------

// True when `needle` occurs anywhere inside `haystack`. Used to attribute a
// lambda to the class whose method body contains it.
zl::MatchExpr::Pattern cloneMatchPattern(const zl::MatchExpr::Pattern& source) {
    zl::MatchExpr::Pattern out;
    out.kind = source.kind;
    out.raw = source.raw;
    out.literalType = source.literalType;
    out.enumTypeName = source.enumTypeName;
    out.enumMemberName = source.enumMemberName;
    out.typePattern = source.typePattern;
    out.bindingName = source.bindingName;
    out.containerKind = source.containerKind;
    out.positional = source.positional;
    out.line = source.line;
    for (const auto& child : source.elements) {
        out.elements.push_back(child ? std::make_unique<zl::MatchExpr::Pattern>(cloneMatchPattern(*child))
                                     : nullptr);
    }
    for (const auto& entry : source.mapEntries) {
        zl::MatchExpr::MapEntryPattern copied;
        copied.line = entry.line;
        copied.key = entry.key ? std::make_unique<zl::MatchExpr::Pattern>(cloneMatchPattern(*entry.key))
                               : nullptr;
        copied.value = entry.value ? std::make_unique<zl::MatchExpr::Pattern>(cloneMatchPattern(*entry.value))
                                   : nullptr;
        out.mapEntries.push_back(std::move(copied));
    }
    for (const auto& field : source.fields) {
        zl::MatchExpr::DataFieldPattern copied;
        copied.fieldName = field.fieldName;
        copied.line = field.line;
        copied.pattern = field.pattern
                             ? std::make_unique<zl::MatchExpr::Pattern>(cloneMatchPattern(*field.pattern))
                             : nullptr;
        out.fields.push_back(std::move(copied));
    }
    return out;
}

// True when `needle` occurs anywhere inside `haystack`. Used to attribute a
// lambda to the class whose method body contains it.
bool containsNode(const zl::AstNode* haystack, const zl::AstNode* needle) {
    if (!haystack) return false;
    if (haystack == needle) return true;
    switch (haystack->kind) {
        case zl::NodeKind::BlockStmt: {
            const auto& block = static_cast<const zl::BlockStmt&>(*haystack);
            for (const auto& child : block.statements) {
                if (containsNode(child.get(), needle)) return true;
            }
            return false;
        }
        case zl::NodeKind::LambdaExpr: {
            const auto& lambda = static_cast<const zl::LambdaExpr&>(*haystack);
            return containsNode(lambda.hasExprBody ? lambda.exprBody.get() : lambda.blockBody.get(), needle);
        }
        case zl::NodeKind::FunctionDecl: {
            const auto& function = static_cast<const zl::FunctionDecl&>(*haystack);
            return containsNode(function.body.get(), needle);
        }
        default:
            return false;
    }
}

void collectLambdas(const zl::AstNode* node, LoweringContext& ctx, const std::string& ownerClass,
                    const std::vector<std::string>& extraParams = {}) {
    if (!node) return;
    if (node->kind == zl::NodeKind::LambdaExpr) {
        const auto& lambda = static_cast<const zl::LambdaExpr&>(*node);
        if (!ctx.lambdaIds.count(&lambda)) {
            std::ostringstream name;
            name << (ownerClass.empty() ? std::string("$") : ownerClass + ".") << "$lambda" << ctx.lambdaCounter++;
            const FunctionId id = ctx.builder.addFunction(name.str()).function().id;
            ctx.lambdaIds[&lambda] = id;
            ctx.lambdaOrder.push_back(&lambda);
            auto params = ctx.typeParamsFor(ownerClass);
            params.insert(params.end(), extraParams.begin(), extraParams.end());
            ctx.lambdaTypeParams[&lambda] = std::move(params);
        }
    }
    // Every container kind, at every depth: a lambda passed straight into a call
    // (`Thread.start(func() => ...)`) sits inside a CallExpr, not inside a
    // BlockStmt, and a closure body can contain further closures.
    zl::forEachChild(node, [&](const zl::AstNode* child) {
        collectLambdas(child, ctx, ownerClass, extraParams);
    });
}

void declareLayouts(LoweringContext& ctx) {
    for (const auto& declaration : ctx.program.declarations) {
        if (declaration->kind == zl::NodeKind::InterfaceDecl) {
            // Record the interface hierarchy and nothing else. An interface has
            // no fields, no dispatch rows and no instance layout, so giving it a
            // ClassLayout would put a phantom class in the backend's reflection
            // and dispatch tables. The one fact the rest of the IR needs is
            // which contract entails which other one, because that decides
            // whether a value of one interface type may be used where another is
            // expected: `Named n = shape` for `interface Shape extends Named`.
            const auto& iface = static_cast<const zl::InterfaceDecl&>(*declaration);
            InterfaceInfo& info = ctx.builder.addInterface(iface.name);
            info.bases = iface.extendsNames;
            info.location = SourceLocation{iface.sourceFile, static_cast<std::uint32_t>(iface.line), 0};
            // An interface declares signatures, not bodies. They are recorded so
            // a call through an interface-typed receiver can be dispatched from
            // the interface's own declaration.
            TypeConverter ifaceConverter{ctx.builder.types(), kNoTypeParams};
            for (const auto& signature : iface.methods) {
                InterfaceMethod method;
                method.name = signature.name;
                for (const auto& param : signature.params) {
                    method.parameterTypes.push_back(ifaceConverter.fromAnnotation(param.type));
                }
                method.returnType = ifaceConverter.fromAnnotation(signature.returnType);
                info.methods.push_back(std::move(method));
            }
        } else if (declaration->kind == zl::NodeKind::ClassDecl) {
            const auto& cls = static_cast<const zl::ClassDecl&>(*declaration);
            ClassLayout& layout = ctx.builder.addClassLayout(cls.name);
            layout.typeParameters = cls.typeParams;
            layout.parent = cls.extendsName;
            if (!cls.extendsName.empty()) {
                // The extends clause with the parent's generic arguments, in
                // the canonical source rendering - reflection's baseTypeName.
                zl::TypeAnnotation baseType;
                baseType.name = cls.extendsName;
                baseType.typeArgs = cls.extendsTypeArgs;
                layout.parentTypeName = zl::describeTypeAnnotation(baseType);
            }
            layout.interfaces = cls.implementsNames;
            layout.location = SourceLocation{cls.sourceFile, static_cast<std::uint32_t>(cls.line), 0};
            TypeConverter converter{ctx.builder.types(), cls.typeParams};
            for (const auto& member : cls.members) {
                if (member->kind != zl::NodeKind::VarDecl) continue;
                const auto& field = static_cast<const zl::VarDecl&>(*member);
                FieldLayout fieldLayout;
                fieldLayout.name = field.name;
                fieldLayout.type = field.hasExplicitType ? converter.fromAnnotation(field.type)
                                                         : ctx.builder.types().unknownType();
                fieldLayout.ownership = field.ownership;
                fieldLayout.isStatic = field.isStatic;
                fieldLayout.access = field.access == zl::AccessModifier::PRIVATE    ? MemberAccess::Private
                                     : field.access == zl::AccessModifier::PROTECTED ? MemberAccess::Protected
                                                                                     : MemberAccess::Public;
                layout.fields.push_back(fieldLayout);
                // A `static` member is module-level storage, not per-instance
                // state, so it is also declared as a MIR static. Without this the
                // verifier can only warn that a static_load names a field the
                // module never declared.
                if (field.isStatic) {
                    (void)ctx.builder.addStatic(cls.name, field.name, fieldLayout.type, kNoFunction,
                                                SourceLocation{field.sourceFile, static_cast<std::uint32_t>(field.line), 0});
                }
            }
        } else if (declaration->kind == zl::NodeKind::DataDecl) {
            const auto& data = static_cast<const zl::DataDecl&>(*declaration);
            ClassLayout& layout = ctx.builder.addClassLayout(data.name);
            layout.parent = data.extendsName;
            layout.isData = true;
            layout.location = SourceLocation{data.sourceFile, static_cast<std::uint32_t>(data.line), 0};
            TypeConverter converter{ctx.builder.types(), kNoTypeParams};
            for (const auto& field : data.fields) {
                FieldLayout fieldLayout;
                fieldLayout.name = field.name;
                fieldLayout.type = converter.fromAnnotation(field.type);
                fieldLayout.ownership = field.ownership;
                layout.fields.push_back(std::move(fieldLayout));
            }
        } else if (declaration->kind == zl::NodeKind::EnumDecl) {
            const auto& enumeration = static_cast<const zl::EnumDecl&>(*declaration);
            ClassLayout& layout = ctx.builder.addClassLayout(enumeration.name);
            layout.isEnum = true;
            layout.enumMembers = enumeration.members;
            layout.location = SourceLocation{enumeration.sourceFile, static_cast<std::uint32_t>(enumeration.line), 0};
        }
    }
}

void declareFunction(LoweringContext& ctx, const zl::FunctionDecl& function, const std::string& ownerClass) {
    if (!ctx.options.lowerGenericTemplates && !ctx.typeParamsFor(ownerClass).empty()) return;

    FunctionBuilder fb = ctx.builder.addFunction(ctx.qualifiedName(function));
    const std::vector<std::string> classParams = ctx.typeParamsFor(ownerClass);
    std::vector<std::string> typeParams = classParams;
    typeParams.insert(typeParams.end(), function.typeParams.begin(), function.typeParams.end());
    TypeConverter converter{ctx.builder.types(), typeParams};

    fb.setAsync(function.isAsync);
    fb.setConstructor(function.isConstructor);
    fb.setStatic(function.isStatic);
    fb.setOperator(function.isOperator);
    // Visibility is part of the member's identity for reflection, so it is
    // recorded rather than defaulted: `DEFAULT` (no modifier) is public.
    fb.setAccess(function.access == zl::AccessModifier::PRIVATE    ? MemberAccess::Private
                 : function.access == zl::AccessModifier::PROTECTED ? MemberAccess::Protected
                                                                    : MemberAccess::Public);
    fb.setLocation(SourceLocation{function.sourceFile, static_cast<std::uint32_t>(function.line), 0});
    if (!classParams.empty()) fb.setGenericTemplate(classParams);
    if (!function.typeParams.empty()) {
        fb.function().isGenericTemplate = true;
        fb.function().methodTypeParameters = function.typeParams;
    }
    for (const auto& annotation : function.annotations) {
        if (annotation.name == "native") fb.setNative(true);
    }
    fb.setReturnType(converter.fromAnnotation(function.returnType));

    const bool instanceContext = !function.isStatic && !ownerClass.empty();
    if (instanceContext) {
        // `this` is parameter 0, so a direct call's arity is just the parameter
        // count and no call convention is implicit.
        //
        // Inside `class Set<T>`, `this` is `Set<T>` - the self-parameterized
        // form - not a bare `Set`. Typing it bare would lose the instantiation
        // and make every `this`-passing self-call look like a type error.
        //
        // The name is rendered and re-parsed through the shared converter rather
        // than assembled with objectType() directly, because not every class is
        // a plain Object: `Shared<T>` and `Task<T>` have their own kinds, and
        // building `this` as Object "Shared" would make it unassignable to the
        // Shared<int> a call site actually passes.
        std::string selfName = ownerClass;
        if (!classParams.empty()) {
            selfName += "<";
            for (std::size_t i = 0; i < classParams.size(); ++i) {
                if (i) selfName += ",";
                selfName += classParams[i];
            }
            selfName += ">";
        }
        (void)fb.addParameter("this", converter.fromRendered(selfName), zl::OwnershipKind::GC, {},
                              SourceLocation{function.sourceFile, static_cast<std::uint32_t>(function.line), 0});
        fb.function().hasThisParameter = true;
    }
    for (const auto& parameter : function.params) {
        (void)fb.addParameter(parameter.name, converter.fromAnnotation(parameter.type), parameter.ownership,
                              {}, SourceLocation{function.sourceFile, static_cast<std::uint32_t>(parameter.type.line), 0});
    }

    ctx.functionIds[fb.function().name] = fb.function().id;
    if (function.body) collectLambdas(function.body.get(), ctx, ownerClass, function.typeParams);
}

// Declares a lambda's MIR function: its flags and its capture list.
//
// Parameters are deliberately NOT added here. Captures become the closure body's
// leading parameters, and their types are only knowable once the enclosing body
// has been lowered (that is where the captured locals, and so their types, are
// in scope). Declaring them too early would mean declaring them as unknown and
// throwing the information away.
void lowerLambda(LoweringContext& ctx, const zl::LambdaExpr& lambda) {
    const FunctionId id = ctx.lambdaIds.at(&lambda);
    FunctionBuilder fb = ctx.builder.functionBuilder(id);
    fb.setLambda(true);
    fb.setAsync(lambda.isAsync);
    fb.setLocation(SourceLocation{lambda.sourceFile, static_cast<std::uint32_t>(lambda.line), 0});
    // A lambda written inside a generic class is generic over that class's
    // parameters: it closes over `this` and over locals whose types mention
    // them. Declaring the closure as a template is what makes those types legal
    // inside it rather than an unsubstituted parameter in a concrete function.
    const auto typeParams = ctx.lambdaTypeParams.find(&lambda);
    if (typeParams != ctx.lambdaTypeParams.end() && !typeParams->second.empty()) {
        fb.setGenericTemplate(typeParams->second);
    }
    for (std::size_t i = 0; i < lambda.captureNames.size(); ++i) {
        const std::string& name = lambda.captureNames[i];
        // The storage name is what the runtime snapshot is keyed by; fall back
        // to the source name when analysis recorded no storage name (the
        // receiver - "this" is already scope-keyed).
        const std::string storage = i < lambda.captureStorageNames.size() && !lambda.captureStorageNames[i].empty()
                                        ? lambda.captureStorageNames[i] : name;
        fb.addCapture(name, ctx.builder.types().unknownType(), lambda.usesThis, storage);
    }
}

// Adds a lambda's parameters (captures first, then the declared ones), fixes its
// return type, and lowers its body.
void lowerLambdaBody(LoweringContext& ctx, const zl::LambdaExpr& lambda) {
    const FunctionId id = ctx.lambdaIds.at(&lambda);
    const auto typeParamsEntry = ctx.lambdaTypeParams.find(&lambda);
    const std::vector<std::string> lambdaTypeParams =
        typeParamsEntry == ctx.lambdaTypeParams.end() ? std::vector<std::string>{} : typeParamsEntry->second;
    FunctionLowerer lowerer(ctx, id, lambdaTypeParams);
    // One builder handle for the whole body. A FunctionBuilder is a lightweight
    // handle over the shared Function, but "which block am I appending to" is
    // per-handle state - so a second handle obtained from the module builder
    // starts with no current block, and anything emitted through it either
    // trips the builder's assertion or, worse, reads a block that is not there.
    FunctionBuilder& fb = lowerer.fb;
    // The lambda's own generic parameters, so an annotation like `T item`
    // resolves to the type parameter rather than to a class named "T".
    TypeConverter converter{ctx.builder.types(), lambdaTypeParams};
    const SourceLocation loc{{}, static_cast<std::uint32_t>(lambda.line), 0};

    // Captures are leading parameters, so the callable's own arity is the
    // parameter count minus the capture count. This keeps a closure a real
    // function a backend can call, with its environment made explicit rather
    // than implicit.
    for (const auto& capture : fb.function().captures) {
        (void)fb.addParameter(capture.usesThis && capture.name == "this" ? "this" : capture.name,
                              capture.type, zl::OwnershipKind::GC, {}, loc);
    }
    const std::size_t captureCount = fb.function().captures.size();
    for (std::size_t i = 0; i < lambda.params.size(); ++i) {
        TypeId type = converter.fromAnnotation(lambda.params[i].type);
        if (i < lambda.inferredParameterTypeNames.size() && !lambda.inferredParameterTypeNames[i].empty()) {
            type = converter.fromRendered(lambda.inferredParameterTypeNames[i]);
        }
        (void)fb.addParameter(lambda.params[i].name.empty() ? "arg" + std::to_string(i) : lambda.params[i].name,
                              type, lambda.params[i].ownership, {}, loc);
    }
    TypeId returnType = ctx.builder.types().unknownType();
    if (lambda.hasDeclaredReturnType) returnType = converter.fromAnnotation(lambda.declaredReturnType);
    else if (!lambda.inferredReturnTypeName.empty()) returnType = converter.fromRendered(lambda.inferredReturnTypeName);
    // A lambda with no `return` infers a nil result. ZL has no nil-returning
    // function - the spelling for "produces nothing" is void - so a nil return
    // type is the checker's way of saying the same thing and is recorded as
    // void here.
    if (const Type* resolved = ctx.builder.types().find(returnType);
        resolved && resolved->kind == TypeKind::Nil) {
        returnType = ctx.builder.types().voidType();
    }
    fb.setReturnType(returnType);

    (void)fb.addBlock(BlockKind::Normal, loc);

    // Captured values arrive as parameters and are copied into slots so the body
    // can treat them like any other local.
    for (std::size_t i = 0; i < captureCount; ++i) {
        const Parameter* parameter = fb.function().parameter(static_cast<ParamId>(i));
        if (!parameter) continue;
        const SlotId slot = fb.addSlot(parameter->name, parameter->type, true, zl::OwnershipKind::GC, {}, loc);
        fb.emitStore(slot, fb.parameterOperand(static_cast<ParamId>(i)), loc);
        const std::string sourceName = i < lambda.captureNames.size() ? lambda.captureNames[i] : parameter->name;
        const std::string storageName = i < lambda.captureStorageNames.size() ? lambda.captureStorageNames[i]
                                                                              : sourceName;
        lowerer.bindLocalBoth(sourceName, storageName, LocalRef::slotRef(slot));
        // A lambda that closed over `this` gets the receiver back as its
        // receiver. Inside the body `this` is not an ordinary identifier - the
        // AST spells it ThisExpr, and ThisExpr is answered from thisRef, not
        // from the scope - so capturing it is not enough on its own: the
        // captured slot has to become the body's `this` as well.
        if (sourceName == "this") {
            lowerer.thisType = parameter->type;
            lowerer.thisRef = LocalRef::slotRef(slot);
        }
    }
    for (std::size_t i = 0; i < lambda.params.size(); ++i) {
        const ParamId paramId = static_cast<ParamId>(captureCount + i);
        const Parameter* parameter = fb.function().parameter(paramId);
        if (!parameter) continue;
        // Bind the source name and the storage name, exactly as a named
        // function's parameters are bound: the body's Identifier nodes carry the
        // storage name, and binding only the source name reads `x` as unbound.
        lowerer.bindLocalBoth(lambda.params[i].name, lambda.params[i].storageName,
                              LocalRef::parameterRef(paramId));
    }

    if (lambda.hasExprBody) {
        Operand value = lowerer.expression(lambda.exprBody.get());
        const Type* declared = ctx.builder.types().find(fb.function().returnType);
        const bool returnsNothing = !declared || declared->kind == TypeKind::Void;
        if (returnsNothing) {
            // `func(x) => log(x)` has an expression body that produces nothing.
            // The value is still evaluated for its side effect, but the block
            // ends in a bare return - returning a value from a void function is
            // not a thing the MIR allows.
            if (!lowerer.isDead()) {
                lowerer.emitOwnedCleanup(loc);
                fb.emitReturn(Operand::none(), loc);
            }
        } else if (!value.isNone()) {
            value = lowerer.coerce(value, fb.function().returnType, loc);
            if (!value.isNone() && !lowerer.isDead()) {
                lowerer.emitOwnedCleanup(loc);
                fb.emitReturn(value, loc);
            }
        }
    } else if (lambda.blockBody) {
        lowerer.statement(lambda.blockBody.get());
    }

    // Falling off the end of a lambda body. When the body was not lowered - an
    // unsupported construct inside it - there is no honest terminator to
    // synthesise, so the block is left unterminated and FunctionBuilder::finish
    // ends it in `unreachable`. Synthesising a `return` there would describe
    // control flow the source does not have.
    if (!lowerer.isDead() && !lowerer.failed) {
        const Type* returnTypePtr = ctx.builder.types().find(fb.function().returnType);
        if (returnTypePtr && returnTypePtr->kind == TypeKind::Void) {
            lowerer.emitOwnedCleanup(loc);
            fb.emitReturn(Operand::none(), loc);
        } else {
            fb.emitUnreachable(loc);
        }
    }
    fb.finish();
    if (lowerer.failed) ctx.result.incompleteFunctions.push_back(fb.function().name);
}

// Collects the storage names a body writes to, or moves out of. A parameter
// needs a mutable slot only when one of these happens; otherwise it stays an
// SSA value.
//
// The walk covers the whole body, because a write is only reachable from the
// top-level statements once you follow every container node: `if (a < 0)
// { a = -a }` writes `a` from inside an IfStmt, and a walker that stops at
// BlockStmt never sees it - which then reads the assignment as one to a binding
// that does not exist.
//
// LambdaExpr is a hard stop: a lambda's parameters and locals are its own
// bindings, not the enclosing function's, and lambda bodies are lowered
// separately with their own parameter list.
void collectWrittenLocals(const zl::AstNode* node, std::unordered_set<std::string>& out) {
    if (!node) return;
    switch (node->kind) {
        case zl::NodeKind::AssignExpr: {
            const auto& assign = static_cast<const zl::AssignExpr&>(*node);
            out.insert(assign.storageName.empty() ? assign.name : assign.storageName);
            break;
        }
        case zl::NodeKind::MoveExpr: {
            const auto& move = static_cast<const zl::MoveExpr&>(*node);
            out.insert(move.storageName.empty() ? move.name : move.storageName);
            break;
        }
        // A lambda body is a separate function with its own parameter list.
        case zl::NodeKind::LambdaExpr:
            return;
        default:
            break;
    }
    zl::forEachChild(node, [&](const zl::AstNode* child) { collectWrittenLocals(child, out); });
}

void lowerFunctionBody(LoweringContext& ctx, const zl::FunctionDecl& function, const std::string& ownerClass) {
    const auto found = ctx.functionIds.find(ctx.qualifiedName(function));
    if (found == ctx.functionIds.end()) return;
    std::vector<std::string> typeParams = ctx.typeParamsFor(ownerClass);
    typeParams.insert(typeParams.end(), function.typeParams.begin(), function.typeParams.end());

    FunctionLowerer lowerer(ctx, found->second, typeParams);
    lowerer.ownerClass = ownerClass;
    lowerer.isStaticContext = function.isStatic;
    FunctionBuilder& fb = lowerer.fb;

    collectWrittenLocals(function.body.get(), lowerer.writtenLocals);

    (void)fb.addBlock(BlockKind::Normal, fb.function().location);

    if (fb.function().hasThisParameter) {
        // `this` is never reassigned in ZL, so it stays the parameter it already
        // is rather than being copied into a slot that would have to be
        // writable to exist at all.
        lowerer.thisType = fb.function().parameters[0].type;
        lowerer.thisRef = LocalRef::parameterRef(0);
        // `this` is also bound under its own name. A lambda written inside this
        // method captures it as an ordinary name - semantic analysis puts
        // "this" in captureStorageNames - and a capture is resolved by looking
        // the name up in scope, not by asking whether the enclosing body happens
        // to have a receiver. Keeping `this` only in thisRef would read every
        // such capture as "not in scope".
        lowerer.bindLocalBoth("this", "this", LocalRef::parameterRef(0));
    }

    const std::size_t firstParameter = fb.function().hasThisParameter ? 1 : 0;
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        const ParamId paramId = static_cast<ParamId>(firstParameter + i);
        const Parameter* parameter = fb.function().parameter(paramId);
        if (!parameter) continue;
        const std::string key = function.params[i].storageName.empty() ? function.params[i].name
                                                                       : function.params[i].storageName;
        const bool needsSlot =
            function.params[i].ownership == zl::OwnershipKind::BORROW || lowerer.writtenLocals.count(key) != 0;
        if (!needsSlot) {
            lowerer.bindLocalBoth(function.params[i].name, function.params[i].storageName,
                                  LocalRef::parameterRef(paramId));
            continue;
        }
        const SlotId slot = fb.addSlot(key, parameter->type, true, function.params[i].ownership, {},
                                       fb.function().location);
        if (function.params[i].ownership == zl::OwnershipKind::BORROW) {
            fb.emitBorrow(slot, fb.parameterOperand(paramId), fb.function().location);
        } else {
            fb.emitStore(slot, fb.parameterOperand(paramId), fb.function().location);
        }
        lowerer.bindLocalBoth(function.params[i].name, function.params[i].storageName,
                              LocalRef::slotRef(slot));
    }

    if (function.body) lowerer.statement(function.body.get());

    // A block that falls off the end needs an explicit terminator. A void
    // function returns nothing; anything else is a lowering bug the verifier
    // would catch, so end it in `unreachable` rather than inventing a value.
    if (!lowerer.isDead()) {
        const Type* returnType = ctx.builder.types().find(fb.function().returnType);
        if (returnType && returnType->kind == TypeKind::Void) {
            lowerer.emitOwnedCleanup(fb.function().location);
            fb.emitReturn(Operand::none(), fb.function().location);
        } else {
            fb.emitUnreachable(fb.function().location);
        }
    }
    fb.finish();

    if (lowerer.failed) ctx.result.incompleteFunctions.push_back(fb.function().name);
}

} // namespace

// Declares and lowers one zero-parameter function per static field, and points
// the field's StaticField entry at it. The body is the field's initialiser
// expression (or nil for a field declared without one), returned as-is - the
// same lazy, on-first-access shape the reference compiler emits. Failures mark
// the generated function incomplete like any other body.
void lowerStaticInitializers(LoweringContext& ctx) {
    for (const auto& declaration : ctx.program.declarations) {
        if (declaration->kind != zl::NodeKind::ClassDecl) continue;
        const auto& cls = static_cast<const zl::ClassDecl&>(*declaration);
        const std::vector<std::string> typeParams = ctx.typeParamsFor(cls.name);
        if (!ctx.options.lowerGenericTemplates && !typeParams.empty()) continue;
        TypeConverter converter{ctx.builder.types(), typeParams};

        for (const auto& member : cls.members) {
            if (member->kind != zl::NodeKind::VarDecl) continue;
            const auto& field = static_cast<const zl::VarDecl&>(*member);
            if (!field.isStatic) continue;

            const SourceLocation loc{{}, static_cast<std::uint32_t>(field.line), 0};
            FunctionBuilder fb = ctx.builder.addFunction(cls.name + ".<static-init:" + field.name + ">");
            fb.setStatic(true);
            fb.setLocation(loc);
            const TypeId fieldType = field.hasExplicitType
                                         ? converter.fromAnnotation(field.type)
                                         : ctx.builder.types().unknownType();
            // A field declared without an initialiser holds nil, and the
            // reference's generated initialiser returns nil for it whatever the
            // field's declared type - the VM never asserts this function's
            // result. Typing the function `unknown` says exactly that; the
            // field's own type still governs stores through StaticStore.
            fb.setReturnType(field.initializer ? fieldType : ctx.builder.types().unknownType());
            for (auto& entry : ctx.builder.mutableModule().statics) {
                if (entry.className == cls.name && entry.name == field.name) entry.initializer = fb.function().id;
            }

            FunctionLowerer lowerer(ctx, fb.function().id, typeParams);
            lowerer.ownerClass = cls.name;
            lowerer.isStaticContext = true;
            (void)lowerer.fb.addBlock(BlockKind::Normal, loc);
            Operand value;
            if (field.initializer) value = lowerer.expression(field.initializer.get());
            else value = Operand::constant(ctx.builder.constantNil(), ctx.builder.types().nilType());
            if (value.isNone()) continue; // the initialiser failed; the function is marked incomplete
            value = lowerer.coerce(value, fieldType, loc);
            if (value.isNone()) continue;
            lowerer.fb.emitReturn(value, loc);
        }
    }
}

LoweringResult lowerProgram(const zl::Program& program, const zl::TypeChecker& checker,
                            const LoweringOptions& options) {
    LoweringContext ctx(program, checker, options);

    // Pass 0: class shape, so field access and inheritance resolve.
    for (const auto& declaration : program.declarations) {
        if (declaration->kind == zl::NodeKind::ClassDecl) {
            const auto& cls = static_cast<const zl::ClassDecl&>(*declaration);
            ctx.classTypeParams[cls.name] = cls.typeParams;
            ctx.classParents[cls.name] = cls.extendsName;
        }
    }
    if (options.emitLayouts) declareLayouts(ctx);

    // Pass 1: declare every function so a call can name its target regardless of
    // declaration order. Lambdas are discovered here too, because a closure body
    // is a real MIR function that other instructions reference by id.
    for (const auto& declaration : program.declarations) {
        std::vector<const zl::FunctionDecl*> members;
        std::string ownerClass;
        if (declaration->kind == zl::NodeKind::ClassDecl) {
            const auto& cls = static_cast<const zl::ClassDecl&>(*declaration);
            ownerClass = cls.name;
            for (const auto& member : cls.members) {
                if (member->kind == zl::NodeKind::FunctionDecl) {
                    members.push_back(static_cast<const zl::FunctionDecl*>(member.get()));
                }
            }
        } else if (declaration->kind == zl::NodeKind::DataDecl) {
            const auto& data = static_cast<const zl::DataDecl&>(*declaration);
            ownerClass = data.name;
            for (const auto& member : data.members) {
                if (member->kind == zl::NodeKind::FunctionDecl) {
                    members.push_back(static_cast<const zl::FunctionDecl*>(member.get()));
                }
            }
        } else {
            continue;
        }
        for (const zl::FunctionDecl* function : members) {
            declareFunction(ctx, *function, ownerClass);
            if (!ctx.mainFunction && function->name == "main") ctx.mainFunction = function;
        }
    }

    // Pass 2: lambda function declarations. Bodies come in pass 4 so a closure
    // can call a function declared after it, and so its capture types are
    // already known by the time its parameters are added.
    for (const zl::LambdaExpr* lambda : ctx.lambdaOrder) {
        lowerLambda(ctx, *lambda);
    }

    // Pass 2.5: static-field initializers. A ZL static is lazily initialised
    // shared state, so its initialiser becomes a zero-parameter MIR function
    // the backend (like the reference compiler) wires into the field's static
    // metadata and the runtime invokes on first access. Declared here so
    // every named function already exists - an initialiser may call anything -
    // and so the ids of the functions pass 1 declared stay untouched.
    lowerStaticInitializers(ctx);

    // Pass 3: bodies.
    for (const auto& declaration : program.declarations) {
        std::string ownerClass;
        std::vector<const zl::FunctionDecl*> members;
        if (declaration->kind == zl::NodeKind::ClassDecl) {
            const auto& cls = static_cast<const zl::ClassDecl&>(*declaration);
            ownerClass = cls.name;
            for (const auto& member : cls.members) {
                if (member->kind == zl::NodeKind::FunctionDecl) {
                    members.push_back(static_cast<const zl::FunctionDecl*>(member.get()));
                }
            }
        } else if (declaration->kind == zl::NodeKind::DataDecl) {
            const auto& data = static_cast<const zl::DataDecl&>(*declaration);
            ownerClass = data.name;
            for (const auto& member : data.members) {
                if (member->kind == zl::NodeKind::FunctionDecl) {
                    members.push_back(static_cast<const zl::FunctionDecl*>(member.get()));
                }
            }
        } else {
            continue;
        }
        for (const zl::FunctionDecl* function : members) lowerFunctionBody(ctx, *function, ownerClass);
    }

    // Pass 4: lambda bodies, in declaration order. An enclosing lambda is
    // discovered before the lambdas nested inside it, so this order lets an
    // outer body fill in an inner closure's capture types before that inner
    // closure's parameters are added.
    for (const zl::LambdaExpr* lambda : ctx.lambdaOrder) {
        lowerLambdaBody(ctx, *lambda);
    }

    if (ctx.mainFunction) {
        const auto entry = ctx.functionIds.find(ctx.qualifiedName(*ctx.mainFunction));
        if (entry != ctx.functionIds.end()) ctx.builder.setEntryPoint(entry->second);
    }

    ctx.result.module = ctx.builder.take();
    ctx.result.success = true;
    return std::move(ctx.result);
}

} // namespace zl::mir
