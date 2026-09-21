#include "zl/mir/vm_backend.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "zl/mir/backend_edges.hpp"
#include "zl/vm/value.hpp"
#include "zl/vm/native.hpp"
#include "zl/vm/runtime_type.hpp"

namespace zl::mir {
namespace {

// Both MIR and the bytecode backend define a struct named `Instruction`;
// inside this file, unqualified `Instruction` is the bytecode one.
using BcInstruction = ::zl::Instruction;

// ---------------------------------------------------------------------------
// Value / type helpers
// ---------------------------------------------------------------------------

Value toVmConstant(const Constant& c) {
    switch (c.kind) {
        case ConstKind::Bool: return Value{c.boolValue};
        case ConstKind::Int: return Value{c.intValue};
        case ConstKind::Double: return Value{c.doubleValue};
        case ConstKind::String: return Value{c.stringValue};
        case ConstKind::Nil: return Value{};
        case ConstKind::EnumMember: return Value{c.stringValue};
    }
    return Value{};
}

// Runtime type-assertion spelling for a MIR type. These strings are handed to
// the VM's RuntimeTypeCheck at field boundaries; primitives and plain object
// names are the well-trodden path. The canonical render is a safe fallback.
const char* primitiveTypeName(TypeKind kind) {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Nil: return "nil";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return "int";
        case TypeKind::Double: return "double";
        case TypeKind::String: return "string";
        default: return nullptr;
    }
}

std::string runtimeTypeName(const TypeArena& types, TypeId id) {
    const Type* type = types.find(id);
    if (!type) return "unknown";
    if (const char* p = primitiveTypeName(type->kind)) return p;
    if (type->kind == TypeKind::Object && !type->name.empty()) return type->name;
    return types.render(id);
}

// Dispatch key for a method / constructor, consistent across classes so that
// an override and its base share a slot. Includes the declaring names after
// the receiver (this), matching how overload resolution distinguishes them.
std::string dispatchKeyFor(const Function& fn, const TypeArena& types) {
    const bool hasThis = fn.hasThisParameter;
    std::ostringstream out;
    out << (fn.isConstructor && !fn.ownerClass.empty() ? fn.ownerClass : fn.simpleName) << '(';
    for (std::size_t i = (hasThis ? 1 : 0); i < fn.parameters.size(); ++i) {
        if (i > (hasThis ? 1 : 0)) out << ',';
        out << runtimeTypeName(types, fn.parameters[i].type);
    }
    out << ')';
    return out.str();
}

// Visibility as reflection spells it. The reference path maps an omitted
// modifier to "public", so the mirror-image mapping lives here.
std::string accessName(MemberAccess access) {
    switch (access) {
        case MemberAccess::Private: return "private";
        case MemberAccess::Protected: return "protected";
        case MemberAccess::Public: break;
    }
    return "public";
}

// ---------------------------------------------------------------------------
// The translator
// ---------------------------------------------------------------------------

struct PerFunctionInfo {
    bool supported{false};
    std::string unsupportedReason;
    // Register-local names are function scoped and unique per slot / temp.
};

class MirBytecodeTranslator {
public:
    explicit MirBytecodeTranslator(const Module& module) : module_(module) {}

    BytecodeResult translate() {
        BytecodeResult result;
        try {
            if (!module_.entryPoint || module_.entryPoint > module_.functions.size()) {
                result.errors.push_back("module has no valid entry point");
                return result;
            }
            registerFunctions();
            if (!result.errors.empty()) return result;
            buildDispatchers();
            buildClassReflection();
            buildConstants();
            buildStatics();

            // Skip over function bodies (and the unsupported stub) to the real
            // program start, mirroring the reference compiler.
            const std::size_t skipJump = chunk_.code.size();
            emitGlobal(OpCode::Jump, 0);

            // Compile every function this backend can, catching the ones whose
            // bodies turn out not to be translatable (a construct missed by the
            // cheap opcode gate, e.g. a method whose overload cannot be pinned
            // down). Those become stubs; because the gate already rejects the
            // obviously-unsupported constructs, a stub is only ever *reached*
            // by a program that genuinely needs an untranslatable function, at
            // which point it raises loudly instead of misbehaving silently.
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (!fnInfo_[i].supported) continue;
                const std::size_t entry = chunk_.code.size();
                try {
                    compileFunctionBody(i);
                    chunk_.functions[i].entryAddress = entry;
                } catch (const std::exception& e) {
                    // Nothing was appended: compileFunctionBody buffers its
                    // whole body and only splices it on success.
                    fnInfo_[i].supported = false;
                    fnInfo_[i].unsupportedReason = e.what();
                }
            }

            // A single shared stub for every function this backend could not
            // translate. Reaching it is a loud runtime error, never silence.
            const std::size_t stubAddress = chunk_.code.size();
            emitStub();
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (!fnInfo_[i].supported) chunk_.functions[i].entryAddress = stubAddress;
            }

            // Program entry.
            compileEntry(skipJump);

            result.chunk = std::move(chunk_);
            result.stubbed = std::count_if(fnInfo_.begin(), fnInfo_.end(),
                                           [](const PerFunctionInfo& i) { return !i.supported; });
            for (std::size_t i = 0; i < module_.functions.size(); ++i) {
                if (fnInfo_[i].supported) continue;
                result.stubbedFunctions.push_back(module_.functions[i].name);
                result.stubbedReasons.push_back(fnInfo_[i].unsupportedReason);
            }
            return result;
        } catch (const std::exception& e) {
            result.errors.push_back(e.what());
            return result;
        }
    }

private:
    const Module& module_;
    Chunk chunk_;
    std::vector<PerFunctionInfo> fnInfo_;

    // ConstId (1-based MIR) -> Chunk constant index.
    std::vector<std::size_t> constIndex_;
    std::unordered_map<std::string, std::size_t> slotByDispatchKey_;
    // class name -> vector<functionIndex-or-INVALID> indexed by slot.
    std::unordered_map<std::string, std::vector<std::size_t>> vtables_;
    std::vector<std::string> slotKeyByIndex_;

    // --- chunk emission (global code) -------------------------------------
    std::size_t emitGlobal(OpCode op, std::size_t operand = 0, std::size_t line = 0,
                           std::size_t operand2 = 0, std::size_t operand3 = 0) {
        chunk_.code.push_back(BcInstruction{op, operand, line, operand2, operand3});
        return chunk_.code.size() - 1;
    }
    std::size_t addName(const std::string& name) { return chunk_.addName(name); }
    std::size_t addConst(const Value& value) { return chunk_.addConstant(value); }
    std::size_t methodTypeArgOperand(const std::string& name) {
        if (name.empty()) return 0;
        return addName(name) + 1;
    }

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------
    void registerFunctions() {
        fnInfo_.assign(module_.functions.size(), PerFunctionInfo{});
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            const Function& fn = module_.functions[i];
            const bool hasThis = fn.hasThisParameter;
            FunctionInfo info;
            info.name = fn.name;
            info.ownerClassName = fn.ownerClass;
            // A function with no `this` parameter is called without a receiver.
            // Native functions are dispatched through the native catalog
            // (CallNative), never through a bytecode `Call`, so they need no
            // runnable body here.
            info.isStatic = fn.isStatic;
            info.isAsync = fn.isAsync;
            info.isNative = fn.isNative;
            // A lambda body's leading parameters are its captured values, not
            // call arguments: the runtime seeds them into the frame from the
            // closure box, keyed by capture name, and `CallValue` passes only
            // the callable's own arity. paramNames therefore starts past the
            // captures - otherwise the arity check would demand capture-count
            // extra arguments and the binding loop would overwrite the very
            // values the closure captured. Capture names are the storage
            // names, exactly like the reference registers them: the snapshot
            // is keyed by storage name, so same-named locals in different
            // scopes capture independently.
            const std::size_t firstParameter = fn.isLambda ? fn.captures.size() : (hasThis ? 1 : 0);
            info.capturesEvaluationScope = fn.isLambda;
            if (fn.isLambda) {
                info.isStatic = false; // the reference registers closures as non-static
                for (const auto& capture : fn.captures) {
                    info.captureNames.push_back(capture.storage.empty() ? capture.name : capture.storage);
                }
            } else if (hasThis) {
                info.isStatic = false; // instance methods get `this` from the receiver
            }
            for (std::size_t p = firstParameter; p < fn.parameters.size(); ++p) {
                info.paramNames.push_back(fn.parameters[p].name);
                // Register the declared parameter type names exactly like the
                // reference does (describeTypeAnnotation of the source type).
                // The VM substitutes the receiver's concrete generic bindings
                // at the call and asserts the argument - and reflection
                // (Method.invoke) runs the same check on the raw names, where
                // an empty entry is a mismatch rather than a skip, so eliding
                // the names broke every reflective call ("argument type
                // mismatch at index 0"). render() is the MIR side of that
                // spelling: it keeps generic arguments ("Box<int>") where
                // runtimeTypeName would collapse an Object kind to the bare
                // class name ("Box"), and an unchecked assert against "Box"
                // rejected every boxed argument.
                info.parameterTypeNames.push_back(module_.types.render(fn.parameters[p].type));
            }
            for (std::size_t slotIndex = 0; slotIndex < fn.slots.size(); ++slotIndex) {
                // Slots are 1-based; the local a Store writes is slotLocal(id).
                if (fn.slots[slotIndex].ownership != zl::OwnershipKind::OWNED) continue;
                // An owned local's storage belongs to this call: the VM
                // releases it when the frame exits - on the ordinary return,
                // an early return, and an exception alike - matching the
                // reference compiler's owned-local cleanup.
                info.ownedLocalNames.push_back(slotLocal(static_cast<SlotId>(slotIndex + 1)));
            }
            for (std::size_t p = 0; p < fn.parameters.size(); ++p) {
                if (fn.parameters[p].ownership != zl::OwnershipKind::OWNED) continue;
                info.ownedLocalNames.push_back(paramLocal(fn, static_cast<ParamId>(p)));
            }
            // The rendered return type ("Box<int>", "func():T" bodies' "void"),
            // exactly like the reference's describeTypeAnnotation spelling: a
            // callable's OWN registered returnTypeName is what the func-type
            // argument check compares against, so "" (skip) made every
            // higher-order call reject its lambda as returning the wrong type.
            info.returnTypeName = module_.types.render(fn.returnType);
            info.typeParameters = fn.methodTypeParameters;
            info.dispatchSignature.name = fn.simpleName;
            if (fn.isNative) {
                fnInfo_[i].supported = false;
                fnInfo_[i].unsupportedReason = "native function (dispatched via catalog)";
            } else {
                fnInfo_[i].supported = isSupported(fn, fnInfo_[i].unsupportedReason);
            }
            chunk_.functions.push_back(std::move(info));
        }
    }

    // A function is translatable when every block is a normal block with no
    // dynamic exception chain, and every opcode/terminator has a faithful
    // bytecode spelling.
    bool isSupported(const Function& fn, std::string& reason) {
        // The lowering marks a function incomplete when it meets a construct
        // it cannot represent, and SKIPS THE REST OF THE BODY from that point
        // on (FunctionLowerer::failed). Compiling that prefix as a whole
        // function silently misbehaves (a truncated `return a + b` becomes a
        // void return, faulting some far-away caller contract instead). The
        // incomplete flag exists precisely to stop a backend from treating a
        // partial translation as a whole one (lowering.hpp), so refuse it
        // here: the shared stub raises loudly if the function is reached.
        if (fn.incomplete) {
            const std::string why = fn.incompleteReasons.empty() ? std::string("unspecified")
                                                                 : fn.incompleteReasons.front();
            reason = "incomplete translation of the source body (" + why + ")";
            return false;
        }
        if (!fn.blocks.empty() && !fn.blocks.front().exceptionHandlers.empty()) {
            // The entry block has no incoming edge, so nothing would ever
            // install its handlers; the lowering never produces this.
            reason = "entry block installs exception handlers";
            return false;
        }
        for (const BasicBlock& block : fn.blocks) {
            for (const Instruction& ins : block.instructions) {
                if (!isSupportedOpcode(ins.opcode)) {
                    reason = "opcode " + std::string(opcodeName(ins.opcode));
                    return false;
                }
            }
            if (!isSupportedTerminator(block.terminator.kind)) {
                reason = "terminator " + std::string(terminatorKindName(block.terminator.kind));
                return false;
            }
        }
        if (fn.isNative) return true; // body never runs; calls go through CallNative
        return true;
    }

    bool isSupportedOpcode(Opcode op) {
        switch (op) {
            case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
            case Opcode::Div: case Opcode::Mod: case Opcode::Pow: case Opcode::Neg:
            case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
            case Opcode::BitNot: case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
            case Opcode::Eq: case Opcode::Ne: case Opcode::Lt: case Opcode::Le:
            case Opcode::Gt: case Opcode::Ge:
            case Opcode::Not:
            case Opcode::Widen: case Opcode::Refine: case Opcode::TypeTest:
            case Opcode::IsNull:
            case Opcode::Move: case Opcode::Drop: case Opcode::Borrow:
            case Opcode::EndBorrow:
            case Opcode::Load: case Opcode::Store:
            case Opcode::FieldLoad: case Opcode::FieldStore:
            case Opcode::IndexLoad: case Opcode::IndexStore:
            case Opcode::NewCollection:
            case Opcode::Alloc:
            case Opcode::Call: case Opcode::InvokeMethod: case Opcode::InvokeSuper:
            case Opcode::InvokeStatic: case Opcode::CallIndirect: case Opcode::CallNative:
            case Opcode::MakeClosure:
            case Opcode::StaticLoad: case Opcode::StaticStore:
            case Opcode::Await:
            case Opcode::TaskSpawn: case Opcode::TaskBlock: case Opcode::TaskIgnore: case Opcode::TaskCancel:
            case Opcode::ThreadStart: case Opcode::ThreadJoin: case Opcode::ThreadIsAlive:
            case Opcode::ChannelCreate: case Opcode::ChannelSend: case Opcode::ChannelReceive:
            case Opcode::ChannelSize: case Opcode::ChannelSendAsync: case Opcode::ChannelReceiveAsync:
            case Opcode::MutexWithLock: case Opcode::RwLockWithRead: case Opcode::RwLockWithWrite:
            case Opcode::AtomicLoad: case Opcode::AtomicStore: case Opcode::AtomicAdd:
            case Opcode::AtomicLoadBool: case Opcode::AtomicStoreBool:
            case Opcode::AtomicLoadDouble: case Opcode::AtomicStoreDouble:
            case Opcode::AtomicLoadRef: case Opcode::AtomicStoreRef:
            case Opcode::SemaphoreAcquire: case Opcode::SemaphoreRelease: case Opcode::SemaphoreAvailable:
            case Opcode::SemaphoreSetPermits: case Opcode::SemaphoreTryAcquire:
            case Opcode::SemaphoreReleaseMany:
            case Opcode::ConditionWait: case Opcode::ConditionWaitFor:
            case Opcode::ConditionNotifyOne: case Opcode::ConditionNotifyAll:
            case Opcode::SharedCreate: case Opcode::SharedGet: case Opcode::SharedSet:
            case Opcode::SharedWithLock:
            case Opcode::RangeInBounds: case Opcode::Log:
                return true;
            // The FFI boundary (FfiCall plus handle/callback lifecycle) is the
            // native-codegen boundary: this backend fails closed on it, exactly
            // as it does for TaskCreate and the other unlowered spellings.
            default:
                return false;
        }
    }

    bool isSupportedTerminator(TerminatorKind kind) {
        switch (kind) {
            case TerminatorKind::Return: case TerminatorKind::Jump:
            case TerminatorKind::Branch: case TerminatorKind::Throw:
            // Statically unreachable control needs no emitted transfer; the
            // terminator emitter leaves such blocks without one.
            case TerminatorKind::Unreachable:
                return true;
            default:
                return false;
        }
    }

    // -----------------------------------------------------------------------
    // Dispatch slots and vtables
    // -----------------------------------------------------------------------
    void buildDispatchers() {
        // Assign one slot per distinct dispatch key over all instance methods
        // and constructors, in module order.
        for (const Function& fn : module_.functions) {
            if (fn.isLambda || fn.ownerClass.empty()) continue;
            if (!(fn.hasThisParameter)) continue; // static members are plain Call targets
            const std::string key = dispatchKeyFor(fn, module_.types);
            if (!slotByDispatchKey_.count(key)) {
                slotByDispatchKey_[key] = slotKeyByIndex_.size();
                slotKeyByIndex_.push_back(key);
            }
        }
        const std::size_t slotCount = slotKeyByIndex_.size();

        // Per-class declared function index by dispatch key.
        std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> declByClass;
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            const Function& fn = module_.functions[i];
            if (fn.ownerClass.empty() || !fn.hasThisParameter) continue;
            declByClass[fn.ownerClass][dispatchKeyFor(fn, module_.types)] = i;
        }

        // Class parent chain (from layouts).
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;

        // Collect every class name that has instance members or appears as a
        // declared receiver.
        std::vector<std::string> classNames;
        std::unordered_map<std::string, bool> seen;
        for (const ClassLayout& cls : module_.classes) {
            if (!seen.count(cls.name)) { classNames.push_back(cls.name); seen[cls.name] = true; }
        }
        for (const Function& fn : module_.functions) {
            if (!fn.ownerClass.empty() && !seen.count(fn.ownerClass)) {
                classNames.push_back(fn.ownerClass);
                seen[fn.ownerClass] = true;
            }
        }

        for (const std::string& name : classNames) {
            std::vector<std::size_t> row(slotCount, Chunk::INVALID_FUNCTION_INDEX);
            // Inherited entries first (base -> derived) so a derived override wins.
            std::vector<std::string> chain;
            std::string cur = name;
            for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
                chain.push_back(cur);
                auto it = parents.find(cur);
                cur = (it != parents.end()) ? it->second : std::string();
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const auto found = declByClass.find(*it);
                if (found == declByClass.end()) continue;
                for (const auto& entry : found->second) {
                    const auto slotIt = slotByDispatchKey_.find(entry.first);
                    if (slotIt != slotByDispatchKey_.end()) row[slotIt->second] = entry.second;
                }
            }
            vtables_[name] = std::move(row);
        }
    }

    // -----------------------------------------------------------------------
    // Class reflection (needed by the VM for instance field stores)
    // -----------------------------------------------------------------------
    void buildClassReflection() {
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;
        for (const ClassLayout& cls : module_.classes) {
            ClassReflectionInfo meta;
            // Reflection wants the bare parent name in baseClassName
            // (hierarchy walks key on it) and the full extends clause in
            // baseTypeName (generic rebinding through projections:
            // Proj<A,B> extends Base<B> must map Base's T through B=int).
            meta.baseClassName = cls.parent;
            meta.baseTypeName = cls.parentTypeName.empty() ? cls.parent : cls.parentTypeName;
            meta.interfaces = cls.interfaces;
            meta.typeParameters = cls.typeParameters;
            meta.isDataType = cls.isData;
            meta.isEnumType = cls.isEnum;
            meta.enumMembers = cls.enumMembers;

            // Fields: this class plus ancestors (the VM looks fields up on the
            // receiver's runtime class).
            std::vector<const ClassLayout*> chain;
            std::string cur = cls.name;
            for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
                const ClassLayout* layout = module_.classLayout(cur);
                if (!layout) break;
                chain.push_back(layout);
                auto pit = parents.find(cur);
                cur = (pit != parents.end()) ? pit->second : std::string();
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                for (const FieldLayout& field : (*it)->fields) {
                    RuntimeFieldInfo info;
                    info.name = field.name;
                    info.ownerClassName = (*it)->name;
                    // render() keeps generic arguments, like the reference's
                    // describeTypeAnnotation - "Box<int>", not "Box".
                    info.typeName = module_.types.render(field.type);
                    info.access = accessName(field.access);
                    info.isStatic = field.isStatic;
                    info.ownership = field.ownership;
                    // Static fields are listed too, exactly like the reference:
                    // the static-field opcodes type-check their accesses by
                    // looking the field up here with no receiver, and a list
                    // without the statics turns every one of those lookups
                    // into "unknown field".
                    meta.fields.push_back(std::move(info));
                }
                // Methods and constructors: base classes first, so a derived
                // declaration replaces an inherited one with the same name and
                // parameter list - the same merge the reference path performs.
                //
                // This used to be left empty, which was not a harmless omission:
                // `Type.methods()` is observable, so a program that lists a
                // class's methods printed "method count 0" here and the real
                // count on the reference path. Reflection metadata is program
                // output, so omitting a part of it is a miscompile.
                for (const Function& fn : module_.functions) {
                    if (fn.ownerClass != (*it)->name) continue;
                    if (fn.isLambda) continue;
                    RuntimeMethodInfo info;
                    info.ownerClassName = (*it)->name;
                    for (std::size_t i = fn.hasThisParameter ? 1 : 0; i < fn.parameters.size(); ++i) {
                        info.parameterTypes.push_back(module_.types.render(fn.parameters[i].type));
                    }
                    info.dispatchSignature = dispatchKeyFor(fn, module_.types);
                    // `describe()` carries the name as a prefix; reflection stores
                    // only the parameter suffix.
                    const std::string prefix = fn.isConstructor ? fn.ownerClass : fn.simpleName;
                    if (info.dispatchSignature.rfind(prefix, 0) == 0) {
                        info.dispatchSignature.erase(0, prefix.size());
                    }
                    info.access = accessName(fn.access);
                    info.isStatic = fn.isStatic;
                    info.isAsync = fn.isAsync;
                    info.functionIndex = fn.id - 1;
                    if (fn.isConstructor) {
                        RuntimeConstructorInfo ctor;
                        ctor.ownerClassName = (*it)->name;
                        ctor.access = info.access;
                        ctor.parameterTypes = info.parameterTypes;
                        ctor.dispatchSignature = info.dispatchSignature;
                        meta.constructors.push_back(std::move(ctor));
                        continue;
                    }
                    // MIR's `simpleName` carries the parameter list ("area()");
                    // reflection reports the bare name.
                    info.name = methodToken(fn.simpleName);
                    info.returnType = module_.types.render(fn.returnType);
                    const auto duplicate = std::find_if(
                        meta.methods.begin(), meta.methods.end(),
                        [&](const RuntimeMethodInfo& existing) {
                            return existing.name == info.name && existing.parameterTypes == info.parameterTypes;
                        });
                    if (duplicate == meta.methods.end()) meta.methods.push_back(std::move(info));
                }
            }
            RuntimeTypeInfo runtimeType;
            runtimeType.name = cls.name;
            runtimeType.baseClassName = cls.parent;
            runtimeType.interfaces = cls.interfaces;
            runtimeType.typeParameters = cls.typeParameters;
            runtimeType.isDataType = cls.isData;
            runtimeType.isEnumType = cls.isEnum;
            runtimeType.enumMembers = cls.enumMembers;
            runtimeType.fields = meta.fields;
            runtimeType.methods = meta.methods;
            runtimeType.constructors = meta.constructors;
            buildRuntimeFieldIndex(runtimeType);
            meta.runtimeType = std::make_shared<RuntimeTypeInfo>(std::move(runtimeType));
            chunk_.classReflection[cls.name] = std::move(meta);
        }
        chunk_.classVTables = vtables_;
    }

    // -----------------------------------------------------------------------
    // Constant pool
    // -----------------------------------------------------------------------
    void buildConstants() {
        constIndex_.assign(module_.constants.size() + 1,
                           std::numeric_limits<std::size_t>::max());
        for (std::size_t i = 0; i < module_.constants.size(); ++i) {
            constIndex_[i + 1] = addConst(toVmConstant(module_.constants[i]));
        }
    }

    // Statics: unsupported for now; nothing must reference them in a supported
    // function (StaticLoad/Store are not in the supported opcode set).
    // Static fields are lazily initialised shared state: the metadata maps
    // "Class.field" to the zero-parameter MIR function that computes the
    // initial value, and the VM invokes it on first access. A field with no
    // initialiser function gets none - reaching a load of it is a loud
    // "unknown static field" instead of a silent nil.
    void buildStatics() {
        for (const auto& field : module_.statics) {
            if (field.initializer == kNoFunction ||
                field.initializer > module_.functions.size()) {
                continue;
            }
            chunk_.staticFields[field.className + "." + field.name] =
                StaticFieldInfo{field.className, field.name, field.initializer - 1};
        }
    }

    // -----------------------------------------------------------------------
    // Function body compilation
    // -----------------------------------------------------------------------
    // Per-function emission buffer. Local addresses until finalised (offset by
    // the global base at append time).
    struct Body {
        std::vector<BcInstruction> code;
        std::unordered_map<BlockId, std::size_t> label;
        // Instruction index in `code` whose Jump/Branch operand needs patching.
        std::vector<std::pair<std::size_t, BlockId>> patches;
        std::size_t base{0};
        std::size_t emit(::zl::OpCode op, std::size_t operand = 0, std::size_t line = 0,
                         std::size_t operand2 = 0, std::size_t operand3 = 0) {
            code.push_back(BcInstruction{op, operand, line, operand2, operand3});
            return code.size() - 1;
        }
    };

    void compileFunctionBody(std::size_t fIndex) {
        const Function& fn = module_.functions[fIndex];
        if (fn.isNative) return; // entry set; body is never run
        Body body;
        body.base = chunk_.code.size();

        // Temps that are never read are not materialised: their producing
        // instruction's result is popped immediately instead of being bound to
        // a local that would keep the value alive until the frame exits. This
        // is observable - a discarded Thread.start result must die NOW so its
        // destructor registers the implicit-join node, exactly like the
        // reference path's Pop - not merely a tidiness question.
        readTemps_ = collectReadTemps(fn);

        // Reserve locals: slot and temp names are just indexed names; a slot is
        // materialised on first store (its defining Store), a temp on its
        // producing instruction. Nothing needs declaring up front.

        const std::unordered_map<BlockId, std::size_t> groups = handlerGroups(fn);

        for (const BasicBlock& block : fn.blocks) {
            if (!body.label.count(block.id)) body.label[block.id] = body.code.size();
            // A catch block is entered by the unwinder, which leaves the
            // caught value on the stack - the exception object for a typed
            // catch, the stringified message for a catch-all. Binding it to
            // the clause's slot is the first thing the block does, exactly
            // like the reference's DefineVar at each handler. A cleanup (finally)
            // block is entered the same way, but the value is always the thrown
            // object, and the block rethrows it at the end.
            if (block.kind == BlockKind::Catch || block.kind == BlockKind::Cleanup) {
                emitCatchBinding(fn, block, body);
            }
            // Block parameters are the MIR's phi nodes. The VM has no phi, but
            // block parameters and slots are interchangeable in meaning, so the
            // lowering is exactly the memory form: each predecessor stores the
            // value it would have passed into the parameter's own local (see
            // emitEdgeArguments) and the block simply reads it. Every incoming
            // edge writes that local before the block runs, so a read inside
            // the block sees the value its predecessor supplied, which is what
            // the parameter means. Nothing is emitted here on entry.
            for (const Instruction& ins : block.instructions) {
                emitInstruction(fn, block.id, ins, body);
            }
            emitTerminator(fn, block, body, groups);
        }

        // Patch unresolved forward references.
        for (const auto& patch : body.patches) {
            const auto it = body.label.find(patch.second);
            if (it != body.label.end()) body.code[patch.first].operand = it->second;
        }

        // Append, translating local addresses to global ones. A finally
        // handler's target is a block address like a jump's or a catch
        // handler's, so it needs the same base adjustment.
        for (auto& ins : body.code) {
            if (ins.op == OpCode::Jump || ins.op == OpCode::JumpIfFalse ||
                ins.op == OpCode::PushHandler || ins.op == OpCode::PushFinallyHandler) {
                ins.operand += body.base;
            }
        }
        chunk_.code.insert(chunk_.code.end(), body.code.begin(), body.code.end());
    }

    // ---- local operand / register naming ---------------------------------
    bool hasThis(const Function& fn) const { return fn.hasThisParameter; }
    std::string paramLocal(const Function& fn, ParamId p) const {
        // A lambda body's leading parameters are its captures, and the runtime
        // seeds them into the frame under their capture *storage* names (the
        // same names the MakeClosure site bound) - so reads must use those,
        // not the source names, or the body would read an empty local.
        if (fn.isLambda && p < fn.captures.size()) {
            const CaptureSpec& capture = fn.captures[p];
            return capture.storage.empty() ? capture.name : capture.storage;
        }
        if (hasThis(fn)) {
            if (p == 0) return "this";
            return fn.parameters[p].name;
        }
        return fn.parameters[p].name;
    }
    std::string slotLocal(SlotId s) const { return "@m_slot_" + std::to_string(s); }
    // In a closure body, the slots holding captured values ARE the captured
    // variables: the runtime seeds the frame from the closure box's snapshot
    // and writes the whole frame back to the box at exit, which is how a
    // counter closure's `count = count + 1` persists across calls. A read or
    // write of a capture's slot must therefore land on the local the snapshot
    // is keyed by - the capture's storage name - or the mutation would die
    // with the frame instead of outliving it.
    std::string slotLocal(const Function& fn, SlotId s) const {
        if (fn.isLambda) {
            if (const Slot* slot = fn.slot(s)) {
                for (const CaptureSpec& capture : fn.captures) {
                    if (capture.name == slot->name) {
                        return capture.storage.empty() ? capture.name : capture.storage;
                    }
                }
            }
        }
        return slotLocal(s);
    }
    std::string tempLocal(TempId t) const { return "@m_tmp_" + std::to_string(t); }
    std::string blockParamLocal(BlockParamId p) const { return "@m_bparam_" + std::to_string(p); }

    void pushOperand(const Function& fn, Operand op, Body& body, std::size_t line) {
        switch (op.kind) {
            case OperandKind::Const:
                if (op.index >= constIndex_.size() ||
                    constIndex_[op.index] == std::numeric_limits<std::size_t>::max())
                    throw std::runtime_error("MIR backend: missing constant #" + std::to_string(op.index));
                body.emit(OpCode::PushConst, constIndex_[op.index], line);
                return;
            case OperandKind::Param:
                body.emit(OpCode::LoadVar, addName(paramLocal(fn, op.index)), line);
                return;
            case OperandKind::Temp:
                body.emit(OpCode::LoadVar, addName(tempLocal(op.index)), line);
                return;
            case OperandKind::BlockParam:
                // A block parameter's local was written by the predecessor that
                // transferred here; reading it is the phi.
                body.emit(OpCode::LoadVar, addName(blockParamLocal(op.index)), line);
                return;
            case OperandKind::None:
                throw std::runtime_error("MIR backend: cannot push a None operand");
        }
    }

    void defineTemp(TempId temp, Body& body, std::size_t line) {
        if (temp == kNoTemp) return;
        if (!readTemps_.count(temp)) {
            // A dead result: pop it now. The value's destructor runs at this
            // exact point, which is what the reference's Pop achieves for a
            // discarded call result.
            body.emit(OpCode::Pop, 0, line);
            return;
        }
        body.emit(OpCode::DefineVar, addName(tempLocal(temp)), line);
    }

    [[nodiscard]] static std::unordered_set<TempId> collectReadTemps(const Function& fn) {
        std::unordered_set<TempId> read;
        auto noteOperand = [&read](const Operand& operand) {
            if (operand.kind == OperandKind::Temp) read.insert(operand.index);
        };
        for (const BasicBlock& block : fn.blocks) {
            for (const Instruction& ins : block.instructions) {
                for (const Operand& operand : ins.operands) noteOperand(operand);
            }
            noteOperand(block.terminator.value);
            for (const auto& edge : block.terminator.edgeArguments) {
                for (const Operand& operand : edge) noteOperand(operand);
            }
        }
        return read;
    }

    std::unordered_set<TempId> readTemps_;

    // ---- instructions -----------------------------------------------------
    void emitInstruction(const Function& fn, BlockId blockId, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        (void)blockId;
        switch (ins.opcode) {
            case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
            case Opcode::Div: case Opcode::Mod: case Opcode::Pow:
            case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
            case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
            case Opcode::Eq: case Opcode::Ne: case Opcode::Lt:
            case Opcode::Le: case Opcode::Gt: case Opcode::Ge: {
                pushOperand(fn, ins.operands[0], body, line);
                pushOperand(fn, ins.operands[1], body, line);
                body.emit(vmBinaryOp(ins.opcode), 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Neg: case Opcode::Not: case Opcode::BitNot: {
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(vmUnaryOp(ins.opcode), 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Widen: {
                // The VM stores numbers without a static type, so a widening is
                // the identity; the value just passes through.
                pushOperand(fn, ins.operands[0], body, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Refine: {
                // A refine is the runtime type assertion at a dynamic-to-static
                // boundary, so this backend emits a real AssertType when the
                // operand is not already statically the asserted type. An
                // identity refinement on a plain value stays a no-op: there the
                // MIR checker proved the value already has the type, and
                // re-checking it would burn time re-proving a static fact. The
                // one exception is a collection: there the identity assert is
                // what pins the container's storage contract (the assert
                // descends into the elements and commits their contracts),
                // which is how a literal's declared element type is enforced
                // on later writes. Dropping it silently un-types the container.
                const Type* refined = module_.types.find(ins.resultType);
                const bool pinsCollectionContract = refined && isCollectionType(*refined);
                if (ins.operands[0].type != ins.resultType || pinsCollectionContract) {
                    pushOperand(fn, ins.operands[0], body, line);
                    body.emit(OpCode::AssertType, addName(module_.types.render(ins.resultType)), line);
                    defineTemp(ins.result, body, line);
                    return;
                }
                pushOperand(fn, ins.operands[0], body, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::TypeTest: {
                // `value MatchType "T"` - the non-raising partner of
                // AssertType, producing the bool a match arm branches on.
                pushOperand(fn, ins.operands[0], body, line);
                const std::size_t nameConst = addConst(Value{module_.types.render(ins.testedType)});
                body.emit(OpCode::PushConst, nameConst, line);
                body.emit(OpCode::MatchType, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::IsNull: {
                // `value == null` in the reference's own spelling.
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::PushConst, addConst(Value{}), line);
                body.emit(OpCode::Eq, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Load:
                body.emit(OpCode::LoadVar, addName(slotLocal(fn, ins.slot)), line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::Store:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::DefineVar, addName(slotLocal(fn, ins.slot)), line);
                return;
            case Opcode::Move:
                // `MoveVar` loads the local, clears it, and leaves the value
                // on the stack: the source local is gone from this point on,
                // so a later read fails the same way the reference path's
                // does. This is the ownership transfer, not a copy.
                body.emit(OpCode::MoveVar, addName(slotLocal(fn, ins.slot)), line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::Drop: {
                // Deterministic release. The slot form names the storage to
                // release; the operand form releases the parameter or temp
                // local its value lives in. The VM's frame teardown also
                // erases every owned local on any exit, so an early return or
                // an exception releases exactly what the reference path
                // releases.
                std::string local;
                if (ins.slot != 0) {
                    local = slotLocal(fn, ins.slot);
                } else if (!ins.operands.empty() && ins.operands[0].kind == OperandKind::Param) {
                    local = paramLocal(fn, ins.operands[0].index);
                } else if (!ins.operands.empty() && ins.operands[0].kind == OperandKind::Temp) {
                    local = tempLocal(ins.operands[0].index);
                } else {
                    throw std::runtime_error("MIR backend: drop names no local");
                }
                body.emit(OpCode::DropVar, addName(local), line);
                return;
            }
            case Opcode::Borrow:
                // A ZL borrow is a read-only view of one owner, and the
                // runtime represents it the way the reference compiler does:
                // as the owner's value bound to the borrow's own local. There
                // is no aliasing at the bytecode level to maintain, so the
                // lifetime rules are enforced by the MIR verifier, not here.
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::DefineVar, addName(slotLocal(fn, ins.slot)), line);
                return;
            case Opcode::EndBorrow:
                // Releases the borrow's local. For a native borrow view this
                // is the deterministic release the construct exists for; for
                // ordinary values it only retires the name, which is
                // unobservable.
                body.emit(OpCode::DropVar, addName(slotLocal(fn, ins.slot)), line);
                return;
            case Opcode::FieldLoad:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::GetField, addName(ins.name), line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::FieldStore:
                pushOperand(fn, ins.operands[0], body, line);   // base
                pushOperand(fn, ins.operands[1], body, line);   // value
                body.emit(OpCode::SetField, addName(ins.name), line); // leaves value
                body.emit(OpCode::Pop, 0, line);
                return;
            case Opcode::IndexLoad:
                // Raw native collection access: list/array use Collection.get,
                // map uses Collection.mapGet. (A typed List/Map *object* is
                // always accessed through its methods, so it never reaches an
                // Index opcode.)
                emitCollectionIndexLoad(fn, ins, body);
                return;
            case Opcode::IndexStore:
                emitCollectionIndexStore(fn, ins, body);
                return;
            case Opcode::NewCollection:
                emitNewCollection(fn, ins, body);
                return;
            case Opcode::Alloc: {
                body.emit(OpCode::NewObject, addName(ins.className), line, 0, concreteTypeNameIndex(ins));
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::Call: emitCall(fn, ins, body); return;
            case Opcode::MakeClosure: emitMakeClosure(fn, ins, body); return;
            case Opcode::StaticLoad: emitStaticLoad(fn, ins, body); return;
            case Opcode::StaticStore: emitStaticStore(fn, ins, body); return;
            case Opcode::InvokeMethod: emitInvokeMethod(fn, ins, body); return;
            case Opcode::Await: {
                // `await task`: the VM pops the task, suspends this async frame
                // on it, and the scheduler resumes with the payload pushed.
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::Await, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            // --- task lifecycle -------------------------------------------------
            // Task.block/ignore/cancel are runtime task operations, not methods;
            // this is the same spelling the InvokeMethod Task path below uses.
            case Opcode::TaskBlock: {
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::TaskBlock, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::TaskIgnore: {
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::TaskIgnore, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            case Opcode::TaskCancel: {
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::TaskCancel, 0, line);
                defineTemp(ins.result, body, line);
                return;
            }
            // --- catalog-backed primitives --------------------------------------
            // Every operation below is the dedicated MIR spelling of exactly one
            // native catalog entry: the backend translates it back to that same
            // entry, so the bytecode is identical to what the generic CallNative
            // lowering produced. Only the MIR is more precise.
            case Opcode::TaskSpawn:
                emitCallNativeById(fn, ins, body, NativeId::TASK_SPAWN);
                return;
            case Opcode::ThreadStart:
                emitCallNativeById(fn, ins, body, NativeId::THREAD_START);
                return;
            case Opcode::ThreadJoin:
                emitCallNativeById(fn, ins, body, NativeId::THREAD_JOIN);
                return;
            case Opcode::ThreadIsAlive:
                emitCallNativeById(fn, ins, body, NativeId::THREAD_ISALIVE);
                return;
            case Opcode::ChannelCreate:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_CREATE);
                return;
            case Opcode::ChannelSend:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_SEND);
                return;
            case Opcode::ChannelReceive:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_RECEIVE);
                return;
            case Opcode::ChannelSize:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_SIZE);
                return;
            case Opcode::ChannelSendAsync:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_SEND_ASYNC);
                return;
            case Opcode::ChannelReceiveAsync:
                emitCallNativeById(fn, ins, body, NativeId::CHANNEL_RECEIVE_ASYNC);
                return;
            case Opcode::MutexWithLock:
                emitCallNativeById(fn, ins, body, NativeId::MUTEX_WITHLOCK);
                return;
            case Opcode::RwLockWithRead:
                emitCallNativeById(fn, ins, body, NativeId::RWLOCK_WITHREAD);
                return;
            case Opcode::RwLockWithWrite:
                emitCallNativeById(fn, ins, body, NativeId::RWLOCK_WITHWRITE);
                return;
            case Opcode::AtomicLoad:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_LOAD);
                return;
            case Opcode::AtomicStore:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_STORE);
                return;
            case Opcode::AtomicAdd:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_ADD);
                return;
            case Opcode::AtomicLoadBool:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_LOAD_BOOL);
                return;
            case Opcode::AtomicStoreBool:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_STORE_BOOL);
                return;
            case Opcode::AtomicLoadDouble:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_LOAD_DOUBLE);
                return;
            case Opcode::AtomicStoreDouble:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_STORE_DOUBLE);
                return;
            case Opcode::AtomicLoadRef:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_LOAD_REF);
                return;
            case Opcode::AtomicStoreRef:
                emitCallNativeById(fn, ins, body, NativeId::ATOMIC_STORE_REF);
                return;
            case Opcode::SemaphoreAcquire:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_ACQUIRE);
                return;
            case Opcode::SemaphoreRelease:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_RELEASE);
                return;
            case Opcode::SemaphoreAvailable:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_AVAILABLE);
                return;
            case Opcode::SemaphoreSetPermits:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_SETPERMITS);
                return;
            case Opcode::SemaphoreTryAcquire:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_TRYACQUIRE);
                return;
            case Opcode::SemaphoreReleaseMany:
                emitCallNativeById(fn, ins, body, NativeId::SEMAPHORE_RELEASEMANY);
                return;
            case Opcode::ConditionWait:
                emitCallNativeById(fn, ins, body, NativeId::CONDITION_WAIT);
                return;
            case Opcode::ConditionWaitFor:
                emitCallNativeById(fn, ins, body, NativeId::CONDITION_WAITFOR);
                return;
            case Opcode::ConditionNotifyOne:
                emitCallNativeById(fn, ins, body, NativeId::CONDITION_NOTIFYONE);
                return;
            case Opcode::ConditionNotifyAll:
                emitCallNativeById(fn, ins, body, NativeId::CONDITION_NOTIFYALL);
                return;
            // --- shared state ---------------------------------------------------
            case Opcode::SharedCreate:
                // `share` tags its Shared box through the CallNative factory
                // type; shared_create carries the same result type, so the tag
                // is identical to the generic path's.
                emitCallNativeById(fn, ins, body, NativeId::SHARED_SHARE,
                                   ins.result != kNoTemp
                                       ? addName(module_.types.render(ins.resultType)) + 1
                                       : 0);
                return;
            case Opcode::SharedGet:
            case Opcode::SharedSet:
            case Opcode::SharedWithLock:
                // One table for all three: `sharedMethodFor` is the same list
                // the reachability analysis consults, so the callee it keeps
                // alive and the callee this emits cannot drift.
                emitSharedMethod(fn, ins, body, sharedMethodFor(ins.opcode));
                return;
            case Opcode::InvokeSuper: emitInvokeSuper(fn, ins, body); return;
            case Opcode::InvokeStatic: emitInvokeStatic(fn, ins, body); return;
            case Opcode::CallIndirect: emitCallIndirect(fn, ins, body); return;
            case Opcode::CallNative: emitCallNative(fn, ins, body); return;
            case Opcode::RangeInBounds:
                pushOperand(fn, ins.operands[0], body, line); // current
                pushOperand(fn, ins.operands[1], body, line); // end
                pushOperand(fn, ins.operands[2], body, line); // step
                body.emit(OpCode::RangeContinue, 0, line);
                defineTemp(ins.result, body, line);
                return;
            case Opcode::Log:
                pushOperand(fn, ins.operands[0], body, line);
                body.emit(OpCode::Log, 0, line);
                return;
            default:
                throw std::runtime_error("MIR backend: unsupported opcode reached emission");
        }
    }

    // If an Alloc names a generic class, its Chunk name table index (+1, the
    // VM's NewObject convention) is the concrete instantiated type name (e.g.
    // "List<string>"). The VM parses that to record the object's runtime type
    // bindings, which is what lets a generic method body substitute its type
    // parameter when later type-checked. Plain classes need no type name
    // (operand3 = 0).
    std::size_t concreteTypeNameIndex(const Instruction& ins) {
        if (ins.typeArguments.empty()) return 0;
        const std::string concreteName = module_.types.render(ins.resultType);
        return addName(concreteName) + 1;
    }

    OpCode vmBinaryOp(Opcode op) const {
        switch (op) {
            case Opcode::Add: return OpCode::Add;
            case Opcode::Sub: return OpCode::Sub;
            case Opcode::Mul: return OpCode::Mul;
            case Opcode::Div: return OpCode::Div;
            case Opcode::Mod: return OpCode::Mod;
            case Opcode::Pow: return OpCode::Pow;
            case Opcode::BitAnd: return OpCode::BitAnd;
            case Opcode::BitOr: return OpCode::BitOr;
            case Opcode::BitXor: return OpCode::BitXor;
            case Opcode::Shl: return OpCode::Shl;
            case Opcode::Shr: return OpCode::Shr;
            case Opcode::Ushr: return OpCode::Ushr;
            case Opcode::Eq: return OpCode::Eq;
            case Opcode::Ne: return OpCode::Neq;
            case Opcode::Lt: return OpCode::Lt;
            case Opcode::Le: return OpCode::Lte;
            case Opcode::Gt: return OpCode::Gt;
            case Opcode::Ge: return OpCode::Gte;
            default: throw std::runtime_error("not a binary op");
        }
    }
    OpCode vmUnaryOp(Opcode op) const {
        switch (op) {
            case Opcode::Neg: return OpCode::Neg;
            case Opcode::Not: return OpCode::Not;
            case Opcode::BitNot: return OpCode::BitNot;
            default: throw std::runtime_error("not a unary op");
        }
    }

    // Direct calls: the MIR `Call` carries a receiver as operand 0 when the
    // callee has a `this` parameter.
    void emitCall(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const FunctionId calleeId = ins.target.function;
        const Function* callee = module_.function(calleeId);
        if (!callee) throw std::runtime_error("MIR backend: bad direct call target");
        const bool calleeHasThis = callee->hasThisParameter;
        const std::size_t argBase = calleeHasThis ? 1 : 0;
        // Constructors are dispatched through the constructor slot on the
        // freshly-allocated receiver object (the VM has no direct-with-receiver
        // opcode).
        if (callee->isConstructor && calleeHasThis) {
            pushOperand(fn, ins.operands[0], body, line); // receiver (new object)
            for (std::size_t i = 1; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            const std::string key = dispatchKeyFor(*callee, module_.types);
            const std::size_t slot = requireSlot(key, ins.location);
            body.emit(OpCode::InvokeMethod, slot, line, ins.operands.size() - 1);
        } else if (calleeHasThis) {
            // A self-call: the VM injects `this` from the current frame, and the
            // operand 0 receiver is that same receiver. Assert we are the caller's
            // own receiver would require frame context; instead push the receiver
            // explicitly is not possible for plain Call, so we require operand 0 to
            // be this function's `this` parameter and drop it (the VM re-adds it).
            if (!isSelfReceiver(fn, ins.operands[0])) {
                throw std::runtime_error("MIR backend: direct instance call whose receiver "
                                         "is not `this` is not translatable");
            }
            for (std::size_t i = 1; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            body.emit(OpCode::Call, calleeId - 1, line, methodTypeArgOperand(ins.name));
        } else {
            for (std::size_t i = 0; i < ins.operands.size(); ++i)
                pushOperand(fn, ins.operands[i], body, line);
            body.emit(OpCode::Call, calleeId - 1, line, methodTypeArgOperand(ins.name));
        }
        // Result (the callee's return value is always pushed by the VM).
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    bool isSelfReceiver(const Function& fn, const Operand& op) const {
        if (!fn.hasThisParameter) return false;
        return op.kind == OperandKind::Param && op.index == 0;
    }

    void emitInvokeMethod(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        // Task.block/ignore/cancel are runtime task operations, not methods:
        // the stdlib's Task class is a marker with no members, so there is no
        // dispatch slot to resolve, and the reference compiler emits the
        // dedicated opcodes for exactly these calls. The receiver pops; the
        // result (Task.block's payload) is the instruction's temp.
        if (ins.target.className == "Task") {
            OpCode taskOp;
            if (ins.target.methodName == "block") taskOp = OpCode::TaskBlock;
            else if (ins.target.methodName == "ignore") taskOp = OpCode::TaskIgnore;
            else if (ins.target.methodName == "cancel") taskOp = OpCode::TaskCancel;
            else throw std::runtime_error("MIR backend: unknown task operation 'Task." +
                                          ins.target.methodName + "'");
            pushOperand(fn, ins.operands[0], body, line);
            body.emit(taskOp, 0, line);
            defineTemp(ins.result, body, line);
            return;
        }
        // operands: [receiver, args...]
        pushOperand(fn, ins.operands[0], body, line);
        for (std::size_t i = 1; i < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line);
        const std::string className = ins.target.className.empty()
                                          ? runtimeTypeName(module_.types, ins.operands[0].type)
                                          : ins.target.className;
        const std::size_t slot =
            resolveMethodSlot(className, ins.target.methodName, ins.operands, ins.location);
        body.emit(OpCode::InvokeMethod, slot, line, ins.operands.size() - 1, methodTypeArgOperand(ins.name));
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    void emitInvokeSuper(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        pushOperand(fn, ins.operands[0], body, line); // receiver
        for (std::size_t i = 1; i < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line);
        body.emit(OpCode::InvokeSuper, ins.target.function - 1, line, ins.operands.size() - 1,
                  methodTypeArgOperand(ins.name));
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    void emitInvokeStatic(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        body.emit(OpCode::Call, ins.target.function - 1, line, methodTypeArgOperand(ins.name));
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    // Builds a closure value. The runtime's MakeClosure snapshots the *current
    // scope* for the names in the closure's captureNames, so every captured
    // value must sit in a local spelled exactly as its capture storage name
    // before the instruction runs. This backend keeps values in
    // compiler-named locals, so the site binds them explicitly; that cannot
    // collide with a real local because source-level storage is either a
    // parameter's own name (the same value) or an `@m_slot_N`/`@m_tmp_N`
    // synthetic. The receiver needs no binding - an instance method's frame
    // already holds it as `this`.
    void emitMakeClosure(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const Function* closureBody = ins.target.function != 0 && ins.target.function <= module_.functions.size()
                                          ? &module_.functions[ins.target.function - 1] : nullptr;
        if (!closureBody) throw std::runtime_error("MIR backend: make_closure names no function");
        if (ins.operands.size() != closureBody->captures.size()) {
            throw std::runtime_error("MIR backend: make_closure capture count does not match its body");
        }
        for (std::size_t i = 0; i < ins.operands.size(); ++i) {
            const CaptureSpec& capture = closureBody->captures[i];
            if (capture.name == "this") continue;
            const std::string& binding = capture.storage.empty() ? capture.name : capture.storage;
            pushOperand(fn, ins.operands[i], body, line);
            body.emit(OpCode::DefineVar, addName(binding), line);
        }
        body.emit(OpCode::MakeClosure, ins.target.function - 1, line);
        defineTemp(ins.result, body, line);
    }

    // Static access goes through the VM's lazy, thread-safe static machinery,
    // which needs the "Class.field" metadata buildStatics() recorded.
    void emitStaticLoad(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        (void)fn;
        body.emit(OpCode::GetStaticField, addName(ins.target.className), line, addName(ins.name));
        defineTemp(ins.result, body, line);
    }

    void emitStaticStore(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        (void)fn;
        pushOperand(fn, ins.operands[0], body, line);
        body.emit(OpCode::SetStaticField, addName(ins.target.className), line, addName(ins.name));
    }

    void emitCallIndirect(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        const std::size_t argCount = ins.operands.empty() ? 0 : ins.operands.size() - 1;
        // operands[0] is the callee, operands[1..] the arguments. The VM's
        // CallValue wants the arguments pushed first with the callee value on
        // top. (This used to walk `i + 1 < size` from 0, which pushed the
        // callee first and then called the last argument - unreachable until
        // closures could actually be built, and instantly fatal once they
        // could. Getting it wrong again in the other direction drops the sole
        // argument of a one-argument call, so the bounds are spelled plainly:
        // every operand after the first is an argument.)
        for (std::size_t i = 1; i < ins.operands.size(); ++i)
            pushOperand(fn, ins.operands[i], body, line); // arguments
        pushOperand(fn, ins.operands[0], body, line);     // callee value on top
        body.emit(OpCode::CallValue, 0, line, argCount);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    std::size_t nativeIndexByName(const std::string& name) const {
        auto byName = findNativeFunction(name);
        if (!byName) throw std::runtime_error("MIR backend: unknown native '" + name + "'");
        return *byName;
    }

    // list/array vs map vs set for a native collection typed operand.
    enum class NativeCollKind { List, Map, Set };
    NativeCollKind collKind(std::uint32_t typeId) const {
        // Read the type structurally. ZL spells a collection two ways - the
        // lowercase keywords arrive as their own kinds and the capitalised
        // classes as an Object with that name - and both must classify the
        // same, or `new List<string>()` and `var l: list<string>` would take
        // different access paths. Matching on the rendered string prefix used
        // to send every class-spelled collection down the raw-native path.
        const Type* type = module_.types.find(typeId);
        if (!type) return NativeCollKind::List;
        const bool isMap = type->kind == TypeKind::Map || (type->kind == TypeKind::Object && type->name == "Map");
        if (isMap) return NativeCollKind::Map;
        const bool isSet = type->kind == TypeKind::Set || (type->kind == TypeKind::Object && type->name == "Set");
        if (isSet) return NativeCollKind::Set;
        return NativeCollKind::List;
    }
    // The base collection class name ("List"/"Map"/"Set") when `typeId` is the
    // CLASS layer of a collection - a generic instantiation like `List<int>` -
    // or empty when it is a raw native collection (`list<T>` unwrapped from a
    // `__native` field). The two layers must not be conflated: a class-layer
    // literal has to produce the same tagged `List<int>` object the reference
    // produces, or typed assignments, generic dispatch and reflection all see
    // an untyped raw list where the language promised a real object.
    std::string classCollectionBase(std::uint32_t typeId) const {
        // Delegated: `backend_edges.hpp` holds the one copy of this mapping,
        // shared with the reachability analysis that keeps its callees alive.
        // Qualified to namespace scope so this member resolves to the free
        // function rather than recursing into itself.
        return ::zl::mir::classCollectionBase(module_.types, typeId);
    }

    void emitNewCollection(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        if (const std::string base = classCollectionBase(ins.resultType); !base.empty()) {
            // A typed literal is a real ZL collection object, equivalent to
            // constructing the collection and filling it through its public
            // methods - exactly what the reference compiler emits for
            // `[...]`/`[k: v]`/`set{...}` literals spelled with the class
            // layer: NewObject tagged with the concrete instantiation
            // (operand3 = concrete name index + 1, as with Alloc), then the
            // empty constructor invoked on a duplicate. The raw-native
            // collection would be an untagged value the runtime cannot type.
            // The rendered instantiation ("List<int>"), not the bare class
            // name: runtimeTypeName returns type->name for Object kinds, and
            // the arena carries the arguments separately - so render() is what
            // spells the instantiation. NewObject's operand3 is what the
            // runtime parses into the receiver's generic bindings, so the
            // element type must be spelled out here.
            const std::string concrete = module_.types.render(ins.resultType);
            const Function* ctor = nullptr;
            for (const Function& candidate : module_.functions) {
                if (isCollectionConstructor(candidate, base)) { // `this` is the only parameter
                    ctor = &candidate;
                    break;
                }
            }
            if (!ctor)
                throw std::runtime_error("MIR backend: collection class '" + base +
                                         "' has no empty constructor for a literal");
            body.emit(OpCode::NewObject, addName(base), line, 0, addName(concrete) + 1);
            body.emit(OpCode::Dup, 0, line);
            body.emit(OpCode::InvokeMethod, requireSlot(dispatchKeyFor(*ctor, module_.types), ins.location),
                      line, 0);
            body.emit(OpCode::Pop, 0, line);
            defineTemp(ins.result, body, line);
            return;
        }
        const char* name = "Collection.newList";
        switch (collKind(ins.resultType)) {
            case NativeCollKind::Map: name = "Collection.newMap"; break;
            case NativeCollKind::Set: name = "Collection.newSet"; break;
            case NativeCollKind::List: name = "Collection.newList"; break;
        }
        body.emit(OpCode::CallNative, nativeIndexByName(name), line, 0);
        defineTemp(ins.result, body, line);
    }

    // IndexLoad: push (collection, key), call the getter, keep the result.
    //
    // A map reads through Collection.mapGet. Everything else reads through
    // the VM's GetIndex, which is what the reference compiler emits for `c[i]`:
    // it accepts a raw native list and a typed List/Set object (unwrapping the
    // object's `__native` storage) alike, and it enforces bounds. Going through
    // Collection.get instead required the operand to be a raw list, so an
    // index read on a class-spelled collection failed at runtime even though
    // its MIR type was exactly right.
    void emitCollectionIndexLoad(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        pushOperand(fn, ins.operands[0], body, line);
        pushOperand(fn, ins.operands[1], body, line);
        // Class-layer collections read through GetIndex - the opcode the
        // reference emits for every `c[k]`, and it accepts typed List/Set/Map
        // objects. Only a RAW native map has no object to unwrap, so it reads
        // through Collection.mapGet.
        if (collKind(ins.operands[0].type) == NativeCollKind::Map && classCollectionBase(ins.operands[0].type).empty()) {
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.mapGet"), line, 0);
        } else {
            body.emit(OpCode::GetIndex, 0, line);
        }
        defineTemp(ins.result, body, line);
    }

    // IndexStore writes element `key` of `collection`. In the MIR, a list is
    // only ever grown through IndexStore as part of building a literal (ZL has
    // no out-of-range list assignment; that goes through Collection.set
    // explicitly), so a list IndexStore appends with Collection.push. A map's
    // key/value write grows naturally, so it maps to Collection.mapSet.
    void emitCollectionIndexStore(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        // Class-layer collections grow through their public methods - the same
        // typed boundary the reference's literal building uses (`push` appends,
        // `add` for sets, `put` for maps), so the element assertion runs exactly
        // where the language runs it. Only raw native collections go through
        // the Collection.* natives.
        if (const std::string base = classCollectionBase(ins.operands[0].type); !base.empty()) {
            std::vector<Operand> callArgs;
            callArgs.push_back(ins.operands[0]); // receiver
            if (base == "Map") {
                callArgs.push_back(ins.operands[1]); // key
                callArgs.push_back(ins.operands[2]); // value
            } else {
                callArgs.push_back(ins.operands[2]); // value (the literal's growing index is append-only)
            }
            const std::string method = collectionAppendMethod(base);
            const std::size_t slot = resolveMethodSlot(base, method, callArgs, ins.location);
            for (const Operand& operand : callArgs) pushOperand(fn, operand, body, line);
            body.emit(OpCode::InvokeMethod, slot, line, callArgs.size() - 1); // operand2 = argument count
            body.emit(OpCode::Pop, 0, line);
            return;
        }
        const NativeCollKind kind = collKind(ins.operands[0].type);
        if (kind == NativeCollKind::Map) {
            pushOperand(fn, ins.operands[0], body, line);
            pushOperand(fn, ins.operands[1], body, line);
            pushOperand(fn, ins.operands[2], body, line);
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.mapSet"), line, 0);
        } else if (kind == NativeCollKind::Set) {
            // Sets grow with setAdd, never a positional append: a literal like
            // `set<string> {"a", "b", "a"}` must dedup to two elements, exactly
            // as the reference's newSet/setAdd sequence does. Push would keep
            // the duplicate and store list-style entries.
            pushOperand(fn, ins.operands[0], body, line);
            pushOperand(fn, ins.operands[2], body, line);
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.setAdd"), line, 0);
        } else {
            // list/array: append value (ignore the growing index).
            pushOperand(fn, ins.operands[0], body, line);
            pushOperand(fn, ins.operands[2], body, line);
            body.emit(OpCode::CallNative, nativeIndexByName("Collection.push"), line, 0);
        }
        body.emit(OpCode::Pop, 0, line);
    }

    void emitCallNative(const Function& fn, const Instruction& ins, Body& body) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        std::size_t nativeIndex = 0;
        if (ins.target.nativeId >= 0) {
            auto byId = findNativeFunction(static_cast<NativeId>(ins.target.nativeId));
            if (byId) nativeIndex = *byId;
            else {
                auto byName = findNativeFunction(ins.target.nativeName);
                if (!byName)
                    throw std::runtime_error("MIR backend: unknown native '" + ins.target.nativeName + "'");
                nativeIndex = *byName;
            }
        } else {
            auto byName = findNativeFunction(ins.target.nativeName);
            if (!byName)
                throw std::runtime_error("MIR backend: unknown native '" + ins.target.nativeName + "'");
            nativeIndex = *byName;
        }
        // `share` tags its Shared box through CallNative's operand2 factory
        // type (the reference emits Shared<T> here; the VM runs
        // initializeObjectType with it). Without the tag the box has no
        // genericTypeName, so every later Shared<T> boundary - get()'s return,
        // setValue's parameter - substitutes T to nothing and fails.
        std::size_t factoryType = 0;
        if (ins.target.nativeId >= 0 && static_cast<NativeId>(ins.target.nativeId) == NativeId::SHARED_SHARE &&
            ins.result != kNoTemp) {
            factoryType = addName(module_.types.render(ins.resultType)) + 1;
        }
        body.emit(OpCode::CallNative, nativeIndex, line, factoryType);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    // A dedicated MIR spelling resolved back to its catalog entry: pushes the
    // operands and emits the same CallNative the generic lowering produced.
    void emitCallNativeById(const Function& fn, const Instruction& ins, Body& body, NativeId id,
                            std::size_t factoryType = 0) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        auto byId = findNativeFunction(id);
        if (!byId) throw std::runtime_error("MIR backend: native catalog has no entry for " +
                                            std::string(opcodeName(ins.opcode)));
        body.emit(OpCode::CallNative, *byId, line, factoryType);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    // A Shared cell access dispatched to the builtin Shared method, exactly as
    // the generic InvokeMethod lowering did: same receiver, same arguments,
    // same dispatch slot, same result handling.
    void emitSharedMethod(const Function& fn, const Instruction& ins, Body& body,
                          const std::string& methodName) {
        const std::size_t line = ins.location.line;
        for (const Operand& op : ins.operands) pushOperand(fn, op, body, line);
        const std::size_t slot = resolveMethodSlot("Shared", methodName, ins.operands, ins.location);
        body.emit(OpCode::InvokeMethod, slot, line, ins.operands.size() - 1);
        if (ins.result != kNoTemp) defineTemp(ins.result, body, line);
        else body.emit(OpCode::Pop, 0, line);
    }

    // ---- terminators ------------------------------------------------------
    //
    // Block-parameter arguments are stored into the parameter's own local before
    // control transfers, one store per argument, in successor order. The VM has
    // no phi node, so the parameter's value is materialised as a local the
    // predecessor writes and the block reads - the memory form of the same
    // merge. Storing before the branch keeps the values the parameters name
    // visible in the target block no matter which edge is taken; the arguments
    // are already-computed operands, so evaluating them ahead of the branch
    // cannot reorder any effect the block itself performs.
    void emitEdgeArguments(const Function& fn, const Terminator& term, std::size_t successorIndex,
                           Body& body, std::size_t line) {
        const auto successors = term.successors();
        if (successorIndex >= successors.size()) return;
        const BasicBlock* target = fn.block(successors[successorIndex]);
        if (!target || target->parameters.empty()) return;
        const std::vector<Operand>& arguments = term.argumentsFor(successorIndex);
        if (arguments.size() != target->parameters.size())
            throw std::runtime_error("MIR backend: block b" + std::to_string(target->id) +
                                     " has " + std::to_string(target->parameters.size()) +
                                     " block parameter(s) but its incoming edge supplies " +
                                     std::to_string(arguments.size()));
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            pushOperand(fn, arguments[i], body, line);
            body.emit(OpCode::DefineVar, addName(blockParamLocal(target->parameters[i].id)), line);
        }
    }

    void emitTerminator(const Function& fn, const BasicBlock& block, Body& body,
                        const std::unordered_map<BlockId, std::size_t>& groups) {
        const Terminator& term = block.terminator;
        const std::size_t line = term.location.line;
        switch (term.kind) {
            case TerminatorKind::Return: {
                if (term.value.isNone()) {
                    const std::size_t nilIdx = addConst(Value{});
                    body.emit(OpCode::PushConst, nilIdx, line);
                } else {
                    pushOperand(fn, term.value, body, line);
                }
                body.emit(OpCode::Return, 0, line);
                return;
            }
            case TerminatorKind::Jump: {
                emitEdgeArguments(fn, term, 0, body, line);
                syncHandlers(fn, block, term.target, groups, body, line);
                emitJumpTo(term.target, body, line);
                return;
            }
            case TerminatorKind::Branch: {
                // Both arms continue within the same handler nesting (an if or
                // a loop body never opens or closes a try region on one arm
                // only), so no stack adjustment belongs here - but a branch
                // that *would* cross one has no faithful translation, since a
                // conditional cannot adjust the handler stack per arm. Fail
                // closed rather than miscompile.
                if (!sameChain(fn, block, term.target) || !sameChain(fn, block, term.elseBlock)) {
                    throw std::runtime_error(
                        "MIR backend: branch crosses an exception-handler region");
                }
                // Arguments first, then the condition: the condition has to end
                // up on top of the stack for the jump, and the arguments are
                // pure reads so their order relative to it does not matter.
                emitEdgeArguments(fn, term, 0, body, line);
                emitEdgeArguments(fn, term, 1, body, line);
                pushOperand(fn, term.value, body, line);
                emitJumpIfFalse(term.elseBlock, body, line);
                emitJumpTo(term.target, body, line);
                return;
            }
            case TerminatorKind::Switch: {
                // A multi-way terminator is not translatable to this VM's
                // single jump-if-false; the opcode gate rejects it before we
                // get here, so reaching this is a translator bug, not user
                // input. Fail loudly rather than emit a wrong transfer.
                throw std::runtime_error(
                    "MIR backend: switch terminator is not translatable to this bytecode");
            }
            case TerminatorKind::Throw: {
                pushOperand(fn, term.value, body, line);
                body.emit(OpCode::Throw, 0, line);
                return;
            }
            default:
                // Unreachable / switch: no bytecode transfer; a block that is
                // statically unreachable needs no emitted transfer.
                return;
        }
    }

    // ---- exception handler bookkeeping ------------------------------------
    // The MIR records, per block, the handler chain in effect there - outer
    // handlers first, then each nested try's own catches in source order
    // (innermost last). The runtime keeps the same handlers on a LIFO stack
    // and dispatch pops from the top, so the first source catch of the
    // innermost try must sit on top. Every normal edge is explicit in the
    // MIR, so the difference between the source block's chain and the
    // target's is exactly the stack adjustment that edge performs: pop the
    // handlers only the source knows (innermost, i.e. latest installed,
    // first) and push the ones only the target knows (in reverse vector
    // order, so the vector's first entry lands on top). Returns and throws
    // need nothing: a return tears the frame down with its handlers, and a
    // throw is dispatched dynamically against whatever is installed.

    static bool sameHandler(const ExceptionHandler& a, const ExceptionHandler& b) {
        return a.block == b.block && a.catchType == b.catchType && a.isFinally == b.isFinally;
    }

    static std::size_t commonPrefix(const std::vector<ExceptionHandler>& a,
                                    const std::vector<ExceptionHandler>& b) {
        std::size_t i = 0;
        while (i < a.size() && i < b.size() && sameHandler(a[i], b[i])) ++i;
        return i;
    }

    [[nodiscard]] const std::vector<ExceptionHandler>& chainOf(const Function& fn, BlockId id) const {
        const BasicBlock* block = fn.block(id);
        if (!block) throw std::runtime_error("MIR backend: edge names no block");
        return block->exceptionHandlers;
    }

    bool sameChain(const Function& fn, const BasicBlock& from, BlockId to) const {
        const std::vector<ExceptionHandler>& a = from.exceptionHandlers;
        const std::vector<ExceptionHandler>& b = chainOf(fn, to);
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!sameHandler(a[i], b[i])) return false;
        }
        return true;
    }

    void syncHandlers(const Function& fn, const BasicBlock& from, BlockId to,
                      const std::unordered_map<BlockId, std::size_t>& groups, Body& body,
                      std::size_t line) {
        const std::vector<ExceptionHandler>& source = from.exceptionHandlers;
        const std::vector<ExceptionHandler>& target = chainOf(fn, to);
        const std::size_t shared = commonPrefix(source, target);
        for (std::size_t i = source.size(); i > shared; --i) {
            body.emit(OpCode::PopHandler, 0, line);
        }
        for (std::size_t k = target.size(); k > shared; --k) {
            const ExceptionHandler& handler = target[k - 1];
            // A finally handler is installed as a catch-all that rethrows
            // after the cleanup block runs; the runtime pushes the thrown
            // object (not a stringified message) and the rethrow is the
            // cleanup block's own terminator. A typed catch carries the class
            // name (bare, so the runtime can match it against the thrown
            // object's class and walk subclasses); operand2 is 1-based so 0
            // can mean catch-all. The group id is what makes a throw inside a
            // catch body skip this try's other handlers: the runtime removes
            // the whole group on dispatch.
            if (handler.isFinally) {
                const std::size_t index = body.emit(OpCode::PushFinallyHandler, 0, line);
                body.patches.emplace_back(index, handler.block);
                continue;
            }
            const std::size_t typeIndex =
                handler.catchType != 0
                    ? addName(bareTypeName(handler.catchType)) + 1
                    : 0;
            const std::size_t group = groups.count(handler.block) ? groups.at(handler.block) : 0;
            const std::size_t index = body.emit(OpCode::PushHandler, 0, line, typeIndex, group);
            body.patches.emplace_back(index, handler.block);
        }
    }

    // The bare class name of a catch type. Catch matching compares the thrown
    // object's class against this name, so a generic catch spelling must
    // contribute only its base name, never its arguments.
    std::string bareTypeName(TypeId type) const {
        const Type* resolved = module_.types.find(type);
        if (resolved && !resolved->name.empty()) return resolved->name;
        return runtimeTypeName(module_.types, type);
    }

    // A catch (or finally cleanup) block's bound value lands in the slot its
    // handler names; find that slot by locating the handler that targets this
    // block. For a cleanup block the value is the thrown object itself.
    void emitCatchBinding(const Function& fn, const BasicBlock& block, Body& body) {
        for (const BasicBlock& b : fn.blocks) {
            for (const ExceptionHandler& handler : b.exceptionHandlers) {
                if (handler.block != block.id) continue;
                if (handler.catchSlot == 0) {
                    throw std::runtime_error("MIR backend: handler target block without a binding slot");
                }
                body.emit(OpCode::DefineVar, addName(slotLocal(handler.catchSlot)),
                          block.location.line);
                return;
            }
        }
        throw std::runtime_error("MIR backend: handler target block named by no handler");
    }

    // All handlers of one try statement share a group id. Two adjacent
    // handlers belong to the same try exactly when they are adjacent in every
    // chain that contains them both: a nested try's handler separates them in
    // some chain, and the nested try's own catch blocks drop the inner handler
    // entirely. Union-find over handler blocks, then ids in first-appearance
    // order.
    [[nodiscard]] std::unordered_map<BlockId, std::size_t> handlerGroups(const Function& fn) const {
        std::vector<const BasicBlock*> chains;
        for (const BasicBlock& block : fn.blocks) {
            if (!block.exceptionHandlers.empty()) chains.push_back(&block);
        }
        std::unordered_map<BlockId, BlockId> parent;
        auto find = [&parent](BlockId x) {
            while (parent.count(x) && parent.at(x) != x) x = parent.at(x);
            return x;
        };
        auto indexOf = [](const BasicBlock& block, BlockId handlerBlock) -> std::size_t {
            for (std::size_t i = 0; i < block.exceptionHandlers.size(); ++i) {
                if (block.exceptionHandlers[i].block == handlerBlock) return i;
            }
            return static_cast<std::size_t>(-1);
        };
        for (const BasicBlock* block : chains) {
            for (std::size_t i = 0; i + 1 < block->exceptionHandlers.size(); ++i) {
                const BlockId x = block->exceptionHandlers[i].block;
                const BlockId y = block->exceptionHandlers[i + 1].block;
                bool siblings = true;
                for (const BasicBlock* other : chains) {
                    const std::size_t pos = indexOf(*other, x);
                    if (pos == static_cast<std::size_t>(-1)) continue;
                    if (pos + 1 >= other->exceptionHandlers.size() ||
                        other->exceptionHandlers[pos + 1].block != y) {
                        siblings = false;
                        break;
                    }
                }
                if (siblings) {
                    const BlockId rootX = find(x);
                    const BlockId rootY = find(y);
                    if (rootX != rootY) parent[rootX] = rootY;
                }
            }
        }
        std::unordered_map<BlockId, std::size_t> groups;
        std::size_t next = 1;
        for (const BasicBlock* block : chains) {
            for (const ExceptionHandler& handler : block->exceptionHandlers) {
                const BlockId root = find(handler.block);
                auto existing = groups.find(root);
                if (existing == groups.end()) {
                    existing = groups.emplace(root, next++).first;
                }
                groups[handler.block] = existing->second;
            }
        }
        return groups;
    }

    void emitJumpTo(BlockId target, Body& body, std::size_t line) {
        const auto it = body.label.find(target);
        if (it != body.label.end()) {
            body.emit(OpCode::Jump, it->second, line);
        } else {
            const std::size_t index = body.emit(OpCode::Jump, 0, line);
            body.patches.emplace_back(index, target);
        }
    }
    void emitJumpIfFalse(BlockId target, Body& body, std::size_t line) {
        const auto it = body.label.find(target);
        if (it != body.label.end()) {
            body.emit(OpCode::JumpIfFalse, it->second, line);
        } else {
            const std::size_t index = body.emit(OpCode::JumpIfFalse, 0, line);
            body.patches.emplace_back(index, target);
        }
    }

    // ---- slot helpers -----------------------------------------------------
    std::size_t requireSlot(const std::string& key, const SourceLocation&) const {
        const auto it = slotByDispatchKey_.find(key);
        if (it == slotByDispatchKey_.end())
            throw std::runtime_error("MIR backend: no dispatch slot for '" + key + "'");
        return it->second;
    }

    // MIR records an instance method's full declared signature as its
    // `simpleName` (e.g. "push(object)") but the InvokeMethod site only names
    // the unqualified token ("push"). Extract that token for matching.
    static std::string methodToken(const std::string& simpleName) {
        const std::size_t paren = simpleName.find('(');
        return paren == std::string::npos ? simpleName : simpleName.substr(0, paren);
    }

    // Resolves the method's dispatch slot by finding the method declared on
    // `className` (or an ancestor) whose name-token and parameter count match
    // the call site, then returns its slot. Overloads differing only in the
    // static types of the arguments are disambiguated by comparing the MIR
    // parameter type ids to the actual operand type ids when that is decisive.
    std::size_t resolveMethodSlot(const std::string& className, const std::string& methodName,
                                  const std::vector<Operand>& args, const SourceLocation& loc) const {
        const std::size_t argCount = args.empty() ? 0 : args.size() - 1; // args[0] is the receiver
        std::string cur = className;
        std::unordered_map<std::string, std::string> parents;
        for (const ClassLayout& cls : module_.classes) parents[cls.name] = cls.parent;
        for (std::size_t guard = 0; guard < 64 && !cur.empty(); ++guard) {
            const Function* found = findDeclaredMethod(cur, methodName, argCount, args);
            if (found) return requireSlot(dispatchKeyFor(*found, module_.types), loc);
            const auto pit = parents.find(cur);
            cur = (pit != parents.end()) ? pit->second : std::string();
        }

        // The declared receiver may be an interface, which declares signatures
        // and no bodies, so nothing above can match it. Its slot is still
        // perfectly well defined: dispatch slots are keyed globally by
        // signature, and every implementer of the interface declares that
        // signature, so the interface's own declaration fixes the key. Reading
        // it from the interface is the exact answer; inferring it from whichever
        // class happens to implement the interface would be a guess.
        std::string ifaceName = className;
        for (std::size_t guard = 0; guard < 64 && !ifaceName.empty(); ++guard) {
            if (const InterfaceInfo* info = module_.interfaceInfo(ifaceName)) {
                if (const InterfaceMethod* method = info->method(methodName)) {
                    if (method->parameterTypes.size() == argCount) {
                        // The key the implementing methods were registered under,
                        // rebuilt exactly as dispatchKeyFor builds it from a real
                        // declaration: a MIR `simpleName` already carries the
                        // parameter list ("name()"), and the key appends the
                        // parameter list again. Mirroring that convention is
                        // what makes the interface's key equal the implementer's
                        // key - deriving a "prettier" key here would silently
                        // fail to find the slot.
                        std::string parameters;
                        for (std::size_t i = 0; i < method->parameterTypes.size(); ++i) {
                            if (i) parameters += ',';
                            parameters += runtimeTypeName(module_.types, method->parameterTypes[i]);
                        }
                        const std::string simpleName = methodName + "(" + parameters + ")";
                        return requireSlot(simpleName + "(" + parameters + ")", loc);
                    }
                }
                // A method the interface inherits from a base interface.
                if (info->bases.empty()) break;
                ifaceName = info->bases.front();
                continue;
            }
            break;
        }
        throw std::runtime_error("MIR backend: cannot resolve method '" + className + "." +
                                 methodName + "' to a dispatch slot");
    }

    // Candidates on `className` with this method-token and `argCount` explicit
    // parameters. When several share the token (genuine overloads) but only one
    // also agrees on the static operand types, that one is chosen.
    const Function* findDeclaredMethod(const std::string& className, const std::string& methodName,
                                       std::size_t argCount,
                                       const std::vector<Operand>& args) const {
        const Function* chosen = nullptr;
        std::size_t arityMatch = 0;
        for (const Function& fn : module_.functions) {
            if (fn.ownerClass != className || !fn.hasThisParameter || fn.isConstructor) continue;
            if (methodToken(fn.simpleName) != methodName) continue;
            const std::size_t explicitParams = fn.parameters.size() - 1; // drop `this`
            if (explicitParams != argCount) continue;                    // arity must agree
            ++arityMatch;
            if (arityMatch == 1) chosen = &fn; // first arity match is provisional
        }
        if (arityMatch == 0) return nullptr;
        if (arityMatch == 1) return chosen;

        // Several overloads share the arity. Narrow by static operand types:
        // a parameter matches the corresponding call operand when their MIR
        // type ids are equal (operands carry their static type).
        for (const Function& fn : module_.functions) {
            if (fn.ownerClass != className || !fn.hasThisParameter || fn.isConstructor) continue;
            if (methodToken(fn.simpleName) != methodName) continue;
            if (fn.parameters.size() - 1 != argCount) continue;
            bool allMatch = true;
            for (std::size_t i = 0; i < argCount; ++i) {
                const std::uint32_t paramType = fn.parameters[i + 1].type;
                const std::uint32_t argType = args[i + 1].type;
                if (paramType != argType) { allMatch = false; break; }
            }
            if (allMatch) return &fn;
        }
        return nullptr; // ambiguous overload -> caller fails closed
    }

    // ---- stub + entry -----------------------------------------------------
    void emitStub() {
        const std::size_t line = 0;
        const std::size_t msg = addConst(Value(std::string(
            "MIR backend: function not translatable by this backend (unsupported construct)")));
        emitGlobal(OpCode::PushConst, msg, line);
        // Throwing a non-object is a loud runtime error, which is what we want
        // if a stubbed function is ever actually reached.
        emitGlobal(OpCode::Throw, 0, line);
    }

    void compileEntry(std::size_t skipJump) {
        const Function& main = module_.functions[module_.entryPoint - 1];
        const std::size_t mainIndex = module_.entryPoint - 1;
        const std::size_t line = main.location.line;
        const bool hasThis = main.hasThisParameter;
        const std::size_t declared = main.parameters.size() - (hasThis ? 1 : 0);
        if (declared > 1)
            throw std::runtime_error("MIR backend: entry function can take at most one parameter");
        const std::size_t entryStart = chunk_.code.size();
        if (declared == 1) emitGlobal(OpCode::PushProgramArgs, 0, line);
        emitGlobal(OpCode::Call, mainIndex, line);
        emitGlobal(OpCode::Pop, 0, line);
        emitGlobal(OpCode::Halt);
        // Patch the leading skip jump to land here.
        chunk_.code[skipJump].operand = entryStart;
    }
};

} // namespace

BytecodeResult compileModuleToBytecode(const Module& module) {
    MirBytecodeTranslator translator(module);
    return translator.translate();
}

} // namespace zl::mir
