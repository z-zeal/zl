#include "zl/native/select.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "zl/mir/type.hpp"

namespace zl::native {
namespace {

using namespace zl::mir;

// A rejection is a normal outcome, not an error, so it is thrown as its own
// type and caught per function: the alternative is threading a bool through
// every helper, which makes the "why" get dropped at the first careless call.
struct Refuse {
    std::string reason;
    std::uint32_t line{0};
};

[[noreturn]] void refuse(const std::string& reason, const SourceLocation& loc) {
    throw Refuse{reason, static_cast<std::uint32_t>(loc.line)};
}

[[noreturn]] void refuse(const std::string& reason) { throw Refuse{reason, 0}; }

class FunctionSelector {
public:
    FunctionSelector(const Module& mirModule, const Function& mirFunction, const TargetMachine& target,
                     const SelectionOptions& options, LirModule& out)
        : mir_(mirModule), fn_(mirFunction), target_(target), options_(options), outModule_(out) {}

    LirFunction run() {
        lir_.name = fn_.name;

        if (options_.requireCompleteMir && fn_.incomplete) {
            std::string why = fn_.incompleteReasons.empty() ? std::string("unspecified")
                                                            : fn_.incompleteReasons.front();
            refuse("MIR is an incomplete translation of the source body (" + why + ")", fn_.location);
        }
        if (fn_.isAsync) refuse("async functions need a scheduler frame the native tier has no shape for", fn_.location);
        if (fn_.isLambda || !fn_.captures.empty()) refuse("closures need a capture environment layout", fn_.location);
        if (fn_.isGenericTemplate) refuse("generic templates are not instantiated by the native tier", fn_.location);
        if (fn_.isConstructor && !supportedNativeConstructor(fn_))
            refuse("constructor requires a non-generic, non-inherited class layout", fn_.location);

        // The whole point of the pipeline: every fact below is read out of the
        // verified MIR. Nothing consults the AST or the type checker.
        lir_.returnClass = classify(fn_.returnType);

        for (const auto& block : fn_.blocks)
            if (!block.exceptionHandlers.empty())
                refuse("exception handlers require unwind tables", block.location);

        declareParameters();
        declareSlots();
        declareBlocks();

        for (const auto& block : fn_.blocks) selectBlock(block);

        lir_.entry = blockMap_.at(fn_.entryBlock);
        lir_.rebuildCfg();
        return std::move(lir_);
    }

private:
    // --- classification ---------------------------------------------------

    ValueClass classify(std::uint32_t typeId) const { return classifyType(mir_, typeId); }

    bool supportedNativeConstructor(const Function& function) const {
        if (!function.isConstructor || function.ownerClass.empty()) return false;
        const ClassLayout* layout = mir_.classLayout(function.ownerClass);
        return layout != nullptr && !layout->isData && layout->parent.empty() &&
               layout->typeParameters.empty();
    }

    // References may be carried through values and used only in the narrow
    // managed-object operations selected below. Every other reference use is
    // refused (arithmetic, equality, strings, collections, and dynamic calls).
    // Allocation and field access go through the runtime bridge, so their
    // emitted calls publish native-frame roots at safepoints.

    // --- declarations -----------------------------------------------------

    VReg newVReg(ValueClass cls, std::string name) {
        if (lir_.vregs.empty()) lir_.vregs.push_back(VRegInfo{}); // index 0 unused
        VRegInfo info;
        info.id = static_cast<VReg>(lir_.vregs.size());
        info.cls = cls;
        info.name = std::move(name);
        lir_.vregs.push_back(info);
        return info.id;
    }

    FrameSlot newSlot(ValueClass cls, std::string name) {
        if (lir_.slots.empty()) lir_.slots.push_back(FrameSlotInfo{});
        FrameSlotInfo info;
        info.id = static_cast<FrameSlot>(lir_.slots.size());
        info.cls = cls;
        info.size = target_.slotSizeFor(cls);
        info.name = std::move(name);
        info.isReference = cls == ValueClass::Reference;
        lir_.slots.push_back(info);
        return info.id;
    }

    void declareParameters() {
        const auto& cc = target_.cc;
        std::size_t intUsed = 0;
        std::size_t floatUsed = 0;
        for (std::size_t i = 0; i < fn_.parameters.size(); ++i) {
            const Parameter& p = fn_.parameters[i];
            const ValueClass cls = classify(p.type);
            if (cls == ValueClass::Void) refuse("void parameter", p.location);
            if (p.ownership != zl::OwnershipKind::GC)
                refuse("parameter '" + p.name + "' has a non-GC ownership contract the native tier cannot honour",
                       p.location);

            const VReg v = newVReg(cls, p.name);
            // Fix the parameter to its ABI register. Arguments beyond the
            // register set are a stack-passing rule the emitter does not
            // implement yet, so they are refused rather than mis-placed.
            if (cls == ValueClass::Float) {
                if (floatUsed >= cc.floatArgRegs.size())
                    refuse("more float arguments than the convention passes in registers", p.location);
                lir_.vregs[v].fixed = cc.floatArgRegs[floatUsed++];
            } else {
                if (intUsed >= cc.intArgRegs.size())
                    refuse("more integer arguments than the convention passes in registers", p.location);
                lir_.vregs[v].fixed = cc.intArgRegs[intUsed++];
            }
            lir_.parameters.push_back(v);
            lir_.parameterClasses.push_back(cls);
            paramVReg_[static_cast<ParamId>(i)] = v;
        }
    }

    void declareSlots() {
        for (std::size_t i = 0; i < fn_.slots.size(); ++i) {
            const Slot& s = fn_.slots[i];
            const ValueClass cls = classify(s.type);
            if (s.ownership != zl::OwnershipKind::GC)
                refuse("local '" + s.name + "' has a non-GC ownership contract", s.location);
            // MIR slot ids are 1-based and positional.
            slotMap_[static_cast<SlotId>(i + 1)] = newSlot(cls, s.name);
        }
    }

    void declareBlocks() {
        for (const auto& b : fn_.blocks) {
            if (b.kind != BlockKind::Normal) refuse("catch/cleanup blocks need unwind support", b.location);
            LirBlock lb;
            lb.id = static_cast<LirBlockId>(lir_.blocks.size() + 1);
            lb.label = "mir" + std::to_string(b.id);
            blockMap_[b.id] = lb.id;
            lir_.blocks.push_back(std::move(lb));

            // A MIR block parameter is a phi. It becomes a vreg that the
            // predecessors copy into on their edges - the standard
            // out-of-SSA form, done here rather than in the emitter because
            // the copies are real instructions that belong in the CFG.
            for (const auto& p : b.parameters) {
                const ValueClass cls = classify(p.type);
                blockParamVReg_[p.id] = newVReg(cls, p.name.empty() ? "phi" : p.name);
            }
        }
    }

    LirBlock& lirBlockFor(BlockId mirBlock) {
        return lir_.blocks[blockMap_.at(mirBlock) - 1];
    }

    // --- operands ---------------------------------------------------------

    // Materialises a MIR operand as a LIR operand usable by an instruction.
    // Constants become immediates; every other operand is already a vreg.
    LirOperand use(const Operand& operand, const SourceLocation& loc) {
        switch (operand.kind) {
            case OperandKind::None: refuse("missing operand", loc);
            case OperandKind::Const: return useConstant(operand, loc);
            case OperandKind::Temp: {
                auto it = tempVReg_.find(operand.index);
                if (it == tempVReg_.end()) refuse("use of a temp with no definition in this function", loc);
                return LirOperand::reg(it->second);
            }
            case OperandKind::Param: {
                auto it = paramVReg_.find(operand.index);
                if (it == paramVReg_.end()) refuse("use of an unknown parameter", loc);
                return LirOperand::reg(it->second);
            }
            case OperandKind::BlockParam: {
                auto it = blockParamVReg_.find(operand.index);
                if (it == blockParamVReg_.end()) refuse("use of an unknown block parameter", loc);
                return LirOperand::reg(it->second);
            }
            case OperandKind::Static:
                refuse("static fields need a global data section", loc);
        }
        refuse("unhandled operand kind", loc);
    }

    LirOperand useConstant(const Operand& operand, const SourceLocation& loc) {
        // Constant ids are 1-based indices into the module's pool.
        if (operand.index == kNoConst || operand.index > mir_.constants.size())
            refuse("constant index out of range", loc);
        const Constant& k = mir_.constants[operand.index - 1];
        switch (k.kind) {
            case ConstKind::Int: return LirOperand::immediate(k.intValue);
            case ConstKind::Bool: return LirOperand::immediate(k.boolValue ? 1 : 0);
            case ConstKind::Double: return LirOperand::immediateF(k.doubleValue);
            case ConstKind::String: refuse("string constants need a data section and a heap string", loc);
            case ConstKind::Nil: refuse("null constants need a reference representation", loc);
            case ConstKind::EnumMember: refuse("enum members are represented as heap strings", loc);
        }
        refuse("unknown constant kind", loc);
    }

    // Forces an operand into a register, materialising an immediate when it is
    // one. Instructions whose target form cannot take an immediate use this.
    LirOperand inRegister(LirBlock& block, const LirOperand& op, ValueClass cls, std::uint32_t line) {
        if (op.kind == LirOperandKind::VirtualReg) return op;
        LirInstruction ins;
        ins.opcode = cls == ValueClass::Float ? LirOpcode::LoadImmF : LirOpcode::LoadImmI;
        ins.result = newVReg(cls, "k");
        ins.resultClass = cls;
        ins.operands = {op};
        ins.line = line;
        block.instructions.push_back(ins);
        return LirOperand::reg(ins.result);
    }

    // The class of a MIR operand, read from the operand's own recorded type.
    ValueClass classOf(const Operand& operand) const { return classify(operand.type); }

    // --- instruction selection --------------------------------------------

    void selectBlock(const BasicBlock& block) {
        LirBlock& out = lirBlockFor(block.id);
        for (const auto& ins : block.instructions) selectInstruction(out, ins);
        selectTerminator(out, block);
    }

    void define(LirBlock& block, const Instruction& mirIns, LirOpcode op, std::vector<LirOperand> operands,
                ValueClass cls) {
        LirInstruction ins;
        ins.opcode = op;
        ins.operands = std::move(operands);
        ins.line = static_cast<std::uint32_t>(mirIns.location.line);
        if (mirIns.result != kNoTemp) {
            ins.result = newVReg(cls, "t" + std::to_string(mirIns.result));
            ins.resultClass = cls;
            tempVReg_[mirIns.result] = ins.result;
        }
        block.instructions.push_back(std::move(ins));
    }

    // Binary arithmetic/comparison. The operand classes decide the machine
    // opcode - this is the exact point at which MIR's type-directed `Add`
    // becomes a machine `IAdd` or `FAdd` and never has to be asked again.
    void selectBinary(LirBlock& block, const Instruction& ins) {
        const ValueClass lhs = classOf(ins.operands[0]);
        const ValueClass rhs = classOf(ins.operands[1]);
        if (lhs != rhs)
            refuse(std::string("mixed operand classes for ") + opcodeName(ins.opcode) + " (" +
                       valueClassName(lhs) + ", " + valueClassName(rhs) + ")",
                   ins.location);
        if (lhs == ValueClass::Reference)
            refuse(std::string(opcodeName(ins.opcode)) + " on references (string concatenation, object equality) "
                   "belongs to the runtime, not this tier",
                   ins.location);

        const bool isFloat = lhs == ValueClass::Float;
        LirOpcode op = LirOpcode::Nop;
        ValueClass resultClass = lhs;

        switch (ins.opcode) {
            case Opcode::Add: op = isFloat ? LirOpcode::FAdd : LirOpcode::IAdd; break;
            case Opcode::Sub: op = isFloat ? LirOpcode::FSub : LirOpcode::ISub; break;
            case Opcode::Mul: op = isFloat ? LirOpcode::FMul : LirOpcode::IMul; break;
            case Opcode::Div: op = isFloat ? LirOpcode::FDiv : LirOpcode::IDiv; break;
            case Opcode::Mod:
                if (isFloat) refuse("floating-point remainder is a runtime call (fmod)", ins.location);
                op = LirOpcode::IMod;
                break;
            case Opcode::Eq: op = isFloat ? LirOpcode::FCmpEq : LirOpcode::ICmpEq; resultClass = ValueClass::Integer; break;
            case Opcode::Ne: op = isFloat ? LirOpcode::FCmpNe : LirOpcode::ICmpNe; resultClass = ValueClass::Integer; break;
            case Opcode::Lt: op = isFloat ? LirOpcode::FCmpLt : LirOpcode::ICmpLt; resultClass = ValueClass::Integer; break;
            case Opcode::Le: op = isFloat ? LirOpcode::FCmpLe : LirOpcode::ICmpLe; resultClass = ValueClass::Integer; break;
            case Opcode::Gt: op = isFloat ? LirOpcode::FCmpGt : LirOpcode::ICmpGt; resultClass = ValueClass::Integer; break;
            case Opcode::Ge: op = isFloat ? LirOpcode::FCmpGe : LirOpcode::ICmpGe; resultClass = ValueClass::Integer; break;
            case Opcode::And: op = LirOpcode::IAnd; break;
            case Opcode::Or: op = LirOpcode::IOr; break;
            case Opcode::BitAnd: op = LirOpcode::IAnd; break;
            case Opcode::BitOr: op = LirOpcode::IOr; break;
            case Opcode::BitXor: op = LirOpcode::IXor; break;
            case Opcode::Shl: op = LirOpcode::IShl; break;
            case Opcode::Shr: op = LirOpcode::IShr; break;
            case Opcode::Ushr: op = LirOpcode::IUshr; break;
            default: refuse(std::string("unsupported binary opcode ") + opcodeName(ins.opcode), ins.location);
        }
        if (isFloat && (op == LirOpcode::IAnd || op == LirOpcode::IOr))
            refuse("bitwise/logical operations on doubles", ins.location);

        const std::uint32_t line = static_cast<std::uint32_t>(ins.location.line);
        LirOperand a = inRegister(block, use(ins.operands[0], ins.location), lhs, line);
        LirOperand b = use(ins.operands[1], ins.location);
        if (isFloat) b = inRegister(block, b, rhs, line);
        define(block, ins, op, {a, b}, resultClass);
    }

    std::size_t internRuntimeString(const std::string& value) {
        const auto found = std::find(outModule_.runtimeStrings.begin(), outModule_.runtimeStrings.end(), value);
        if (found != outModule_.runtimeStrings.end())
            return static_cast<std::size_t>(found - outModule_.runtimeStrings.begin());
        outModule_.runtimeStrings.push_back(value);
        return outModule_.runtimeStrings.size() - 1;
    }

    bool supportedNativeObjectType(std::uint32_t typeId) const {
        const Type* type = mir_.types.find(typeId);
        if (type == nullptr || type->kind != TypeKind::Object || !type->arguments.empty()) return false;
        const ClassLayout* layout = mir_.classLayout(type->name);
        if (layout == nullptr || !layout->typeParameters.empty() || layout->isEnum) return false;
        if (layout->isData) return true;
        if (!layout->parent.empty()) return false;
        const auto constructor = std::find_if(mir_.functions.begin(), mir_.functions.end(),
            [&](const Function& function) {
                return function.isConstructor && function.ownerClass == layout->name;
            });
        return constructor != mir_.functions.end() && supportedNativeConstructor(*constructor);
    }

    void requireNativeObject(std::uint32_t typeId, const SourceLocation& location,
                             const std::string& operation) const {
        if (!supportedNativeObjectType(typeId))
            refuse(operation + " requires a non-generic data record or a non-inherited class",
                   location);
    }

    bool supportedNativeFieldType(std::uint32_t typeId) const {
        const Type* type = mir_.types.find(typeId);
        if (type == nullptr) return false;
        if (type->kind == TypeKind::Int || type->kind == TypeKind::Bool ||
            type->kind == TypeKind::Double) return true;
        return supportedNativeObjectType(typeId);
    }

    void appendFieldStore(LirBlock& block, LirOperand object, const std::string& fieldName,
                          ValueClass valueClass, LirOperand value, bool isBoolean,
                          std::uint32_t line) {
        const char* suffix = valueClass == ValueClass::Float ? "f" :
                             valueClass == ValueClass::Reference ? "r" : "i";
        LirInstruction call;
        call.opcode = LirOpcode::CallRuntime;
        call.callee = std::string("__zl_native_field_store_") + suffix;
        call.operands = {object,
            LirOperand::immediate(static_cast<std::int64_t>(internRuntimeString(fieldName)))};
        call.argClasses = {ValueClass::Reference, ValueClass::Integer};
        if (valueClass == ValueClass::Integer) {
            call.operands.push_back(LirOperand::immediate(isBoolean ? 1 : 0));
            call.argClasses.push_back(ValueClass::Integer);
        }
        call.operands.push_back(value);
        call.argClasses.push_back(valueClass);
        call.line = line;
        block.instructions.push_back(std::move(call));
    }

    void selectAlloc(LirBlock& block, const Instruction& mirIns) {
        if (!options_.allowRuntimeCalls)
            refuse("managed object allocation requires runtime calls", mirIns.location);
        requireNativeObject(mirIns.resultType, mirIns.location, "allocation");
        if (mirIns.className.empty()) refuse("allocation has no class name", mirIns.location);
        if (!mirIns.typeArguments.empty())
            refuse("generic object allocation is not in the native subset", mirIns.location);
        const ClassLayout* layout = mir_.classLayout(mirIns.className);
        if (layout == nullptr) refuse("allocation has no class layout", mirIns.location);
        const bool isPlainClass = !layout->isData;
        const std::uint32_t line = static_cast<std::uint32_t>(mirIns.location.line);

        if (isPlainClass) {
            for (const FieldLayout& field : layout->fields) {
                if (field.isStatic) continue;
                if (!supportedNativeFieldType(field.type))
                    refuse("class field '" + field.name + "' has a type outside the native object subset",
                           layout->location);
            }
        }

        LirInstruction call;
        call.opcode = LirOpcode::CallRuntime;
        call.callee = "__zl_native_alloc_object";
        call.operands = {LirOperand::immediate(static_cast<std::int64_t>(
            internRuntimeString(mirIns.className)))};
        call.argClasses = {ValueClass::Integer};
        call.result = newVReg(ValueClass::Reference, "t" + std::to_string(mirIns.result));
        call.resultClass = ValueClass::Reference;
        call.line = line;
        const VReg object = call.result;
        tempVReg_[mirIns.result] = object;
        block.instructions.push_back(std::move(call));

        // Mirror the VM's zero-value defaults before the constructor runs.
        // These stores are runtime calls too: the just-allocated object remains
        // in the native frame map across every helper/safepoint.
        if (isPlainClass) {
            for (const FieldLayout& field : layout->fields) {
                if (field.isStatic) continue;
                const Type* type = mir_.types.find(field.type);
                const ValueClass cls = classify(field.type);
                const LirOperand defaultValue = cls == ValueClass::Float ? LirOperand::immediateF(0.0)
                    : cls == ValueClass::Reference ? LirOperand::immediate(0)
                    : LirOperand::immediate(0);
                appendFieldStore(block, LirOperand::reg(object), field.name, cls, defaultValue,
                                 type != nullptr && type->kind == TypeKind::Bool, line);
            }
        }
    }

    void selectFieldLoad(LirBlock& block, const Instruction& mirIns) {
        if (!options_.allowRuntimeCalls)
            refuse("managed-object field access requires runtime calls", mirIns.location);
        if (mirIns.operands.empty()) refuse("field load has no receiver", mirIns.location);
        requireNativeObject(mirIns.operands[0].type, mirIns.location, "field access");
        if (!supportedNativeFieldType(mirIns.resultType))
            refuse("field type is outside the native object subset", mirIns.location);
        const ValueClass cls = classify(mirIns.resultType);
        const char* suffix = cls == ValueClass::Float ? "f" :
                             cls == ValueClass::Reference ? "r" : "i";
        LirInstruction call;
        call.opcode = LirOpcode::CallRuntime;
        call.callee = std::string("__zl_native_field_load_") + suffix;
        call.operands = {
            inRegister(block, use(mirIns.operands[0], mirIns.location), ValueClass::Reference,
                       static_cast<std::uint32_t>(mirIns.location.line)),
            LirOperand::immediate(static_cast<std::int64_t>(internRuntimeString(mirIns.name)))
        };
        call.argClasses = {ValueClass::Reference, ValueClass::Integer};
        call.result = newVReg(cls, "t" + std::to_string(mirIns.result));
        call.resultClass = cls;
        call.line = static_cast<std::uint32_t>(mirIns.location.line);
        tempVReg_[mirIns.result] = call.result;
        block.instructions.push_back(std::move(call));
    }

    void selectFieldStore(LirBlock& block, const Instruction& mirIns) {
        if (!options_.allowRuntimeCalls)
            refuse("managed-object field access requires runtime calls", mirIns.location);
        if (mirIns.operands.size() < 2) refuse("field store has incomplete operands", mirIns.location);
        requireNativeObject(mirIns.operands[0].type, mirIns.location, "field access");
        const std::uint32_t valueType = mirIns.operands[1].type;
        if (!supportedNativeFieldType(valueType))
            refuse("field type is outside the native object subset", mirIns.location);
        const ValueClass cls = classify(valueType);
        const char* suffix = cls == ValueClass::Float ? "f" :
                             cls == ValueClass::Reference ? "r" : "i";
        const std::uint32_t line = static_cast<std::uint32_t>(mirIns.location.line);
        LirInstruction call;
        call.opcode = LirOpcode::CallRuntime;
        call.callee = std::string("__zl_native_field_store_") + suffix;
        call.operands = {
            inRegister(block, use(mirIns.operands[0], mirIns.location), ValueClass::Reference, line),
            LirOperand::immediate(static_cast<std::int64_t>(internRuntimeString(mirIns.name)))
        };
        call.argClasses = {ValueClass::Reference, ValueClass::Integer};
        if (cls == ValueClass::Integer) {
            const Type* valueTypeInfo = mir_.types.find(valueType);
            const std::int64_t isBoolean = valueTypeInfo != nullptr && valueTypeInfo->kind == TypeKind::Bool ? 1 : 0;
            call.operands.push_back(LirOperand::immediate(isBoolean));
            call.argClasses.push_back(ValueClass::Integer);
        }
        call.operands.push_back(inRegister(block, use(mirIns.operands[1], mirIns.location), cls, line));
        call.argClasses.push_back(cls);
        call.line = line;
        block.instructions.push_back(std::move(call));
    }

    void selectInstruction(LirBlock& block, const Instruction& ins) {
        const std::uint32_t line = static_cast<std::uint32_t>(ins.location.line);
        switch (ins.opcode) {
            case Opcode::Nop:
                return;

            case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div:
            case Opcode::Mod:
            case Opcode::Eq: case Opcode::Ne: case Opcode::Lt: case Opcode::Le:
            case Opcode::Gt: case Opcode::Ge:
            case Opcode::And: case Opcode::Or:
            case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor:
            case Opcode::Shl: case Opcode::Shr: case Opcode::Ushr:
                selectBinary(block, ins);
                return;

            case Opcode::Neg: {
                const ValueClass cls = classOf(ins.operands[0]);
                if (cls == ValueClass::Reference) refuse("negation of a reference", ins.location);
                LirOperand a = inRegister(block, use(ins.operands[0], ins.location), cls, line);
                define(block, ins, cls == ValueClass::Float ? LirOpcode::FNeg : LirOpcode::INeg, {a}, cls);
                return;
            }
            case Opcode::Not: {
                LirOperand a = inRegister(block, use(ins.operands[0], ins.location), ValueClass::Integer, line);
                define(block, ins, LirOpcode::INot, {a}, ValueClass::Integer);
                return;
            }
            case Opcode::BitNot: {
                LirOperand a = inRegister(block, use(ins.operands[0], ins.location), ValueClass::Integer, line);
                define(block, ins, LirOpcode::IBitNot, {a}, ValueClass::Integer);
                return;
            }
            case Opcode::Widen: {
                LirOperand a = inRegister(block, use(ins.operands[0], ins.location), ValueClass::Integer, line);
                define(block, ins, LirOpcode::IntToFloat, {a}, ValueClass::Float);
                return;
            }

            case Opcode::Load: {
                auto it = slotMap_.find(ins.slot);
                if (it == slotMap_.end()) refuse("load from an unknown slot", ins.location);
                const ValueClass cls = lir_.slots[it->second].cls;
                define(block, ins, LirOpcode::Load, {LirOperand::frame(it->second)}, cls);
                return;
            }
            case Opcode::Store: {
                auto it = slotMap_.find(ins.slot);
                if (it == slotMap_.end()) refuse("store to an unknown slot", ins.location);
                const ValueClass cls = lir_.slots[it->second].cls;
                LirOperand value = inRegister(block, use(ins.operands[0], ins.location), cls, line);
                LirInstruction store;
                store.opcode = LirOpcode::Store;
                store.operands = {LirOperand::frame(it->second), value};
                store.line = line;
                block.instructions.push_back(std::move(store));
                return;
            }

            case Opcode::FieldLoad:
                selectFieldLoad(block, ins);
                return;

            case Opcode::FieldStore:
                selectFieldStore(block, ins);
                return;

            case Opcode::Alloc:
                selectAlloc(block, ins);
                return;

            case Opcode::Call: {
                selectCall(block, ins);
                return;
            }

            default:
                refuse(std::string("MIR opcode ") + opcodeName(ins.opcode) +
                           " is outside the native subset (runs on the VM)",
                       ins.location);
        }
    }

    void selectCall(LirBlock& block, const Instruction& ins) {
        const FunctionId callee = ins.target.function;
        if (callee == kNoFunction || callee > mir_.functions.size())
            refuse("call to an unresolved function", ins.location);
        const Function& target = mir_.functions[callee - 1];
        if (target.isAsync) refuse("call to an async function returns a Task", ins.location);

        LirInstruction call;
        call.opcode = LirOpcode::Call;
        call.callee = target.name;
        call.line = static_cast<std::uint32_t>(ins.location.line);

        const auto& cc = target_.cc;
        std::size_t intUsed = 0, floatUsed = 0;
        for (const auto& arg : ins.operands) {
            const ValueClass cls = classOf(arg);
            if (cls == ValueClass::Float) {
                if (++floatUsed > cc.floatArgRegs.size()) refuse("too many float arguments", ins.location);
            } else {
                if (++intUsed > cc.intArgRegs.size()) refuse("too many integer arguments", ins.location);
            }
            call.operands.push_back(inRegister(block, use(arg, ins.location), cls, call.line));
            call.argClasses.push_back(cls);
        }

        const ValueClass resultClass = classify(ins.resultType);
        if (ins.result != kNoTemp) {
            call.result = newVReg(resultClass, "t" + std::to_string(ins.result));
            call.resultClass = resultClass;
            tempVReg_[ins.result] = call.result;
        }
        block.instructions.push_back(std::move(call));
    }

    // --- terminators ------------------------------------------------------
    //
    // Block parameters are destroyed here: each edge's arguments become copies
    // into the successor's parameter vregs, emitted before the transfer. The
    // copies are sequential, which is only safe when no parameter is both read
    // and written on the same edge; the check below refuses that case rather
    // than emitting a swap it has not implemented.
    void emitEdgeCopies(LirBlock& out, const BasicBlock& block, std::size_t successorIndex,
                        BlockId successor) {
        const auto& args = block.terminator.argumentsFor(successorIndex);
        const BasicBlock* target = fn_.block(successor);
        if (target == nullptr) refuse("terminator names a block that does not exist", block.terminator.location);
        if (args.size() != target->parameters.size())
            refuse("edge argument count does not match the successor's parameters", block.terminator.location);

        const std::uint32_t line = static_cast<std::uint32_t>(block.terminator.location.line);

        std::vector<VReg> destinations;
        std::vector<LirOperand> sources;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const ValueClass cls = classify(target->parameters[i].type);
            destinations.push_back(blockParamVReg_.at(target->parameters[i].id));
            sources.push_back(inRegister(out, use(args[i], block.terminator.location), cls, line));
        }
        for (std::size_t i = 0; i < destinations.size(); ++i)
            for (std::size_t j = i + 1; j < sources.size(); ++j)
                if (sources[j].kind == LirOperandKind::VirtualReg && sources[j].vreg == destinations[i])
                    refuse("edge needs a parallel copy with a cycle; not implemented",
                           block.terminator.location);

        for (std::size_t i = 0; i < destinations.size(); ++i) {
            LirInstruction move;
            move.opcode = LirOpcode::Move;
            move.result = destinations[i];
            move.resultClass = lir_.vregs[destinations[i]].cls;
            move.operands = {sources[i]};
            move.line = line;
            move.comment = "phi copy";
            out.instructions.push_back(std::move(move));
        }
    }

    void selectTerminator(LirBlock& out, const BasicBlock& block) {
        const Terminator& term = block.terminator;
        const std::uint32_t line = static_cast<std::uint32_t>(term.location.line);

        switch (term.kind) {
            case TerminatorKind::Return: {
                LirInstruction ret;
                ret.opcode = LirOpcode::Return;
                ret.line = line;
                if (!term.value.isNone()) {
                    const ValueClass cls = classOf(term.value);
                    ret.operands = {inRegister(out, use(term.value, term.location), cls, line)};
                }
                out.instructions.push_back(std::move(ret));
                return;
            }
            case TerminatorKind::Jump: {
                emitEdgeCopies(out, block, 0, term.target);
                LirInstruction jump;
                jump.opcode = LirOpcode::Jump;
                jump.operands = {LirOperand::target(blockMap_.at(term.target))};
                jump.line = line;
                out.instructions.push_back(std::move(jump));
                return;
            }
            case TerminatorKind::Branch: {
                const ValueClass cls = classOf(term.value);
                if (cls != ValueClass::Integer) refuse("branch condition is not a bool", term.location);
                LirOperand cond = inRegister(out, use(term.value, term.location), ValueClass::Integer, line);
                // Both edges may carry arguments. Emitting both sets of copies
                // before a two-way branch would be wrong, so a branch whose
                // successors take parameters is refused until critical-edge
                // splitting exists.
                for (std::size_t i = 0; i < 2; ++i) {
                    const BlockId succ = i == 0 ? term.target : term.elseBlock;
                    const BasicBlock* target = fn_.block(succ);
                    if (target != nullptr && !target->parameters.empty())
                        refuse("branch to a block with parameters needs a split critical edge",
                               term.location);
                }
                LirInstruction br;
                br.opcode = LirOpcode::BranchIf;
                br.operands = {cond, LirOperand::target(blockMap_.at(term.target)),
                               LirOperand::target(blockMap_.at(term.elseBlock))};
                br.line = line;
                out.instructions.push_back(std::move(br));
                return;
            }
            case TerminatorKind::Unreachable: {
                LirInstruction u;
                u.opcode = LirOpcode::Unreachable;
                u.line = line;
                out.instructions.push_back(std::move(u));
                return;
            }
            case TerminatorKind::Switch:
                refuse("switch needs a jump table or a comparison chain", term.location);
            case TerminatorKind::Throw:
                refuse("throw needs unwind support", term.location);
            case TerminatorKind::None:
                refuse("block has no terminator (invalid MIR reached the backend)", block.location);
        }
        refuse("unknown terminator", term.location);
    }

    const Module& mir_;
    const Function& fn_;
    const TargetMachine& target_;
    const SelectionOptions& options_;
    LirModule& outModule_;
    LirFunction lir_;

    std::unordered_map<TempId, VReg> tempVReg_;
    std::unordered_map<ParamId, VReg> paramVReg_;
    std::unordered_map<BlockParamId, VReg> blockParamVReg_;
    std::unordered_map<SlotId, FrameSlot> slotMap_;
    std::unordered_map<BlockId, LirBlockId> blockMap_;
};

} // namespace

ValueClass classifyType(const zl::mir::Module& module, std::uint32_t typeId) {
    const zl::mir::Type* type = module.types.find(typeId);
    if (type == nullptr) return typeId == zl::mir::kNoType ? ValueClass::Void : ValueClass::Reference;
    switch (type->kind) {
        case zl::mir::TypeKind::Void: return ValueClass::Void;
        case zl::mir::TypeKind::Bool:
        case zl::mir::TypeKind::Int: return ValueClass::Integer;
        case zl::mir::TypeKind::Double: return ValueClass::Float;
        default:
            // Everything else - strings, collections, objects, sums, tasks,
            // unions, type parameters, unknown - is reference-shaped or not
            // understood, and both must fail towards the conservative class.
            return ValueClass::Reference;
    }
}

SelectionResult selectModule(const zl::mir::Module& module, const TargetMachine& target,
                             const SelectionOptions& options) {
    SelectionResult result;
    result.module.name = module.name;

    if (target.arch == Arch::Unknown) {
        result.errors.push_back("no native target description for this host");
        return result;
    }

    for (const auto& fn : module.functions) {
        try {
            FunctionSelector selector(module, fn, target, options, result.module);
            LirFunction lowered = selector.run();
            result.lowered.push_back(fn.name);
            result.module.functions.push_back(std::move(lowered));
        } catch (const Refuse& refusal) {
            result.rejected.push_back(SelectionRejection{fn.name, refusal.reason, refusal.line});
        }
    }

    // Any function the subset accepted must not call one it refused: the
    // callee would have no native body to jump to. Dropping such callers is
    // the fail-closed answer; the caller then runs on the VM like its callee.
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<std::string> available;
        for (const auto& fn : result.module.functions) available.push_back(fn.name);
        for (std::size_t i = 0; i < result.module.functions.size();) {
            const LirFunction& fn = result.module.functions[i];
            std::string missing;
            for (const auto& block : fn.blocks)
                for (const auto& ins : block.instructions)
                    if (ins.opcode == LirOpcode::Call &&
                        std::find(available.begin(), available.end(), ins.callee) == available.end())
                        missing = ins.callee;
            if (missing.empty()) { ++i; continue; }
            result.rejected.push_back(
                SelectionRejection{fn.name, "calls '" + missing + "', which is not in the native subset", 0});
            result.lowered.erase(std::remove(result.lowered.begin(), result.lowered.end(), fn.name),
                                 result.lowered.end());
            result.module.functions.erase(result.module.functions.begin() + static_cast<long>(i));
            changed = true;
        }
    }

    for (const auto& fn : result.module.functions)
        for (const auto& block : fn.blocks)
            for (const auto& ins : block.instructions)
                if (ins.opcode == LirOpcode::CallRuntime &&
                    std::find(result.module.runtimeImports.begin(), result.module.runtimeImports.end(),
                              ins.callee) == result.module.runtimeImports.end())
                    result.module.runtimeImports.push_back(ins.callee);

    return result;
}

} // namespace zl::native
