#pragma once

#include "zl/vm/runtime_exception.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "zl/vm/value.hpp"
#include "zl/common/dispatch_signature.hpp"

namespace zl {

enum class OpCode : std::uint8_t {
    PushConst,   // operand = index into Chunk::constants; push that value
    Pop,         // discard top of stack (used after expression-statements)
    Dup,         // duplicate the top of stack (assignment-as-expression needs its value twice: once to store, once to keep as the result)

    DefineVar,   // operand = index into Chunk::names; pop value, bind name -> value
    LoadVar,     // operand = index into Chunk::names; push the named variable's value
    MoveVar,     // operand = index into Chunk::names; move the value out and clear the source slot
    DropVar,     // operand = index into Chunk::names; deterministically release/clear an owned local

    // arithmetic
    Add, Sub, Mul, Div, Mod, Pow,
    Neg,         // unary minus

    // bitwise (Java-style)
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr, Ushr,

    // comparison
    Eq, Neq, Lt, Gt, Lte, Gte,

    // logical
    And, Or, Not,

    // control flow - operand = target instruction index in Chunk::code
    Jump,          // unconditional jump
    JumpIfFalse,   // pop condition; jump only if it was falsy

    Call,          // operand = index into Chunk::functions; async functions return a Task and schedule execution.
                   // operand2 = name-pool index + 1 of a ';'-joined list of resolved
                   // method-level type arguments (0 = none).
    Return,        // pops the return value, restores the caller's frame, pushes the value back
    CallNative,    // operand = index into the native func registry; pops args, calls into C++, pushes result
    PushProgramArgs, // pushes a list<string> built from the VM's real CLI args (see main.cpp) - used to feed main(args: list<string>)

    // Lambda expressions (see LambdaExpr/ClosureBox). operand = index into
    // Chunk::functions (the lambda body's own compiled entry, registered
    // exactly like a normal func's - see Compiler::compileLambdaExpr).
    // Builds a ClosureRef capturing a snapshot (by VALUE - a copy) of every
    // local currently in scope (or every global, if executed at top level)
    // and pushes it.
    MakeClosure,
    // Indirect call through a VALUE on the stack, rather than a statically
    // resolved Chunk::functions index the way Call is - used for calling a
    // variable that holds a closure (see CallExpr::isValueCall). Stack
    // layout on entry: [arg0, arg1, ..., argN-1, closureValue] with
    // closureValue on top; operand2 = N (argument count). Pops the closure
    // value, then pops N args, binds them against the closure's OWN
    // captured-scope snapshot (not the caller's locals - this is what makes
    // capture-by-value actually stick), and pushes a call frame.
    CallValue,
    Await,       // pop a Task; if incomplete, suspend the current async frame and resume later
    TaskBlock,   // pop a Task; synchronously wait for completion and push its result
    TaskIgnore,  // pop a Task and mark its result/failure intentionally ignored; pushes no value
    TaskCancel,  // pop a Task and request cooperative cancellation; pushes no value
    MatchType,    // pop a value and push whether it matches a compile-time type name
    AssertType,    // pop a value, verify it matches the expected type, then push it back

    // Pops step, then end, then i (push order: i, end, step). Pushes a bool:
    // true if the for-loop should keep going. Direction-aware so both
    // ascending (step > 0) and descending (step < 0) ranges work correctly -
    // doing this comparison in the VM avoids needing separate bytecode for
    // each direction, which the compiler can't know until runtime anyway.
    RangeContinue,

    // Exception handling. PushHandler's operand is patched to the catch
    // block's address once it's known (same backpatching trick as jumps).
    // operand2 carries a name-pool index for a typed catch, or 0 for catch-all.
    // PopHandler removes the handler once its try-block finishes WITHOUT
    // throwing, so an exception from code AFTER the try/catch doesn't get
    // mistakenly caught by an expired handler.
    PushHandler,
    PushFinallyHandler, // catch-all handler used to run a finally block and rethrow
    PopHandler,
    Throw,   // pop a value and raise it as a real C++ exception. An object must be
             // Exception-derived (the type checker enforces this for source-level
             // `throw`); a bare string is raised as a RuntimeError carrying that
             // text, which is how a backend's fail-closed stub reports itself.

    // Object-oriented operations.
    NewObject,   // operand = index into Chunk::names (the class name); pushes a fresh ObjectRef
    CopyObject,  // pop an ObjectRef and push a shallow field-preserving immutable copy
    GetField,    // operand = index into Chunk::names (the field name); pops object, pushes field value
    GetIndex,    // pops index then list, pushes the indexed element
    GetStaticField, // operand = class-name index, operand2 = field-name index; lazy thread-safe access
    SetStaticField, // operand = class-name index, operand2 = field-name index; stores initialized static value
    SetField,    // operand = index into Chunk::names (the field name); pops value then object, sets field, pushes value back
    SetIndex,    // pops value, index, list; sets list element and pushes value
    // operand = compile-time dispatch slot; operand2 = argument count.
    // Pops operand2 args, then the receiver object, and resolves the slot
    // directly through the receiver class's compiled vtable. No runtime string
    // construction, class-parent walk, or FunctionInfo linear scan is needed.
    // operand3 = name-pool index + 1 of a ';'-joined list of resolved
    // method-level type arguments (0 = none).
    InvokeMethod,

    // operand = compile-time index into Chunk::functions for the exact
    // parent constructor/method selected by the type checker; operand2 =
    // argument count. Pops operand2 args, then pops the receiver ('this'),
    // and calls that func directly. `super` therefore has no runtime
    // string construction, parent-chain walk, or FunctionInfo linear scan.
    // Used for both `super(...)` (constructor) and `super.method(...)`.
    // operand3 = name-pool index + 1 of a ';'-joined list of resolved
    // method-level type arguments (0 = none), matching InvokeMethod.
    InvokeSuper,

    Log,   // pop and print
    Halt   // stop the VM
};

// One compiled func: enough for the VM to bind arguments and jump in.
// `entryAddress` points at the first instruction of the func's body,
// filled in AFTER the signature is registered (see Compiler::compile) so
// that functions can call each other regardless of declaration order.
struct FunctionInfo {
    std::string name;
    std::vector<std::string> paramNames;
    std::vector<std::string> parameterTypeNames;
    std::string returnTypeName;
    std::size_t entryAddress{0};
    bool isStatic{false};
    bool isAsync{false};
    bool isNative{false};
    std::string ownerClassName;
    DispatchSignature dispatchSignature;
    std::vector<std::string> captureNames;
    // Lambda closures capture the current evaluation scope when captureNames
    // is empty (legacy untyped-lambda behavior). Named function references
    // must not capture the caller scope at all.
    bool capturesEvaluationScope{true};
    // Locals with explicit owned semantics. The compiler records these on the
    // function so the VM can release them deterministically at frame exit.
    std::vector<std::string> ownedLocalNames;
    // Method-level type parameters only (`func firstOf<T>(...)`). Class-level
    // parameters arrive via the receiver's type bindings, not this list.
    std::vector<std::string> typeParameters;
};

// One bytecode instruction. `operand`'s meaning depends on `op` (see comments
// above) - e.g. for PushConst it's a constant-pool index, for Jump it's a
// target position in `code`.
// Runtime-visible class metadata used by the Type reflection library.
// This is deliberately small: names and inheritance first, with generic
// arguments left for a later type-system phase.
struct ClassReflectionInfo {
    RuntimeTypeId id{0};
    std::string baseClassName;
    // Unsubstituted parent type, e.g. Base<V,K> for class Derived<K,V>.
    std::string baseTypeName;
    std::vector<std::string> interfaces;
    std::vector<std::string> typeParameters;
    bool isDataType{false};
    bool isEnumType{false};
    std::vector<std::string> enumMembers;
    std::vector<RuntimeFieldInfo> fields;
    std::vector<RuntimeMethodInfo> methods;
    std::vector<RuntimeConstructorInfo> constructors;
    RuntimeTypeRef runtimeType;
};

struct Instruction {
    OpCode op;
    std::size_t operand{0};
    std::size_t line{0};
    // Second operand: argument counts for calls, catch-type name indices for
    // handlers, or a fresh native factory's type-name index + 1 (zero = none).
    std::size_t operand2{0};
    // Third operand: PushHandler catch-group id, or NewObject's concrete
    // type-name index + 1 (zero = use the unparameterized class name).
    std::size_t operand3{0};
};

// A fully-compiled program: the instructions plus the constant/name pools
// they reference by index. Handing the VM a Chunk gives it everything it
// needs to run, with no remaining pointers back into the AST.
struct StaticFieldState {
    enum class Status { Uninitialized, Initializing, Initialized, Failed };
    mutable std::mutex mutex;
    std::condition_variable cv;
    Status status{Status::Uninitialized};
    Value value{};
    StoredException failure;
    std::thread::id ownerThread{};
    std::size_t initializerFunction{static_cast<std::size_t>(-1)};
};

struct StaticRuntimeStorage {
    mutable std::mutex mapMutex;
    std::unordered_map<std::string, std::shared_ptr<StaticFieldState>> fields;
};

struct StaticFieldInfo {
    std::string className;
    std::string fieldName;
    std::size_t initializerFunction{static_cast<std::size_t>(-1)};
};

struct Chunk {
    static constexpr std::size_t INVALID_FUNCTION_INDEX = static_cast<std::size_t>(-1);

    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<std::string> names;
    // Indexes into the pools above (index = vector position), kept in step
    // by addConstant/addName, the only writers of either pool. Pool entries
    // are append-only and never reused, so entries stay valid for the
    // chunk's lifetime. Anything that bypasses addConstant/addName to touch
    // the vectors directly must update these maps to match.
    std::unordered_map<Value, std::size_t, ValueHash, ValueEqual> constantIndex;
    std::unordered_map<std::string, std::size_t> nameIndex;
    std::vector<FunctionInfo> functions;
    // class name -> vtable indexed by compile-time dispatch slot. Entries
    // contain FunctionInfo indices; INVALID_FUNCTION_INDEX means the class
    // does not implement that slot.
    std::unordered_map<std::string, std::vector<std::size_t>> classVTables;
    std::unordered_map<std::string, ClassReflectionInfo> classReflection;
    std::unordered_map<std::string, StaticFieldInfo> staticFields;
    std::shared_ptr<StaticRuntimeStorage> staticStorage{std::make_shared<StaticRuntimeStorage>()};

    // Adds a constant/name if not already present, returns its index either way.
    // Prevents the pool from growing every time the same literal or variable
    // name is referenced twice.
    std::size_t addConstant(const Value& value);
    std::size_t addName(const std::string& name);
};

} // namespace zl
