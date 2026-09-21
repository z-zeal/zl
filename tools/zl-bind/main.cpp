#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <memory>
#include <set>
#include <vector>
#include <limits>

#include "header_parser.hpp"

using zl_bind::Fn;
using zl_bind::Param;
using zl_bind::Method;
using zl_bind::NativeClass;

static std::string mapType(const std::string& t) { return zl_bind::mapNativeType(t); }

static std::string wrapResult(const std::string& type, const std::string& expr) {
    auto t = mapType(type);
    if (t == "void") return expr + "; return Value{};";
    if (t == "int") return "return static_cast<int64_t>(" + expr + ");";
    if (t == "double") return "return static_cast<double>(" + expr + ");";
    if (t == "bool") return "return static_cast<bool>(" + expr + ");";
    if (t == "string") return "return std::string(" + expr + ");";
    throw std::runtime_error("unsupported result");
}

static void emitParamConversions(std::ofstream& o, const std::vector<Param>& params, std::size_t offset) {
    for (size_t i=0;i<params.size();++i) {
        auto mt = mapType(params[i].type);
        o << "    " << params[i].type << " a" << i << " = ";
        if (mt == "int") o << "static_cast<" << params[i].type << ">(std::get<int64_t>(args[" << (i+offset) << "]))";
        else if (mt == "double") o << "std::holds_alternative<int64_t>(args[" << (i+offset) << "]) ? static_cast<double>(std::get<int64_t>(args[" << (i+offset) << "])) : std::get<double>(args[" << (i+offset) << "])";
        else if (mt == "bool") o << "std::get<bool>(args[" << (i+offset) << "])";
        else o << "std::get<std::string>(args[" << (i+offset) << "]).c_str()";
        o << ";\n";
    }
}

static std::string callArgs(const std::vector<Param>& params, std::size_t offset = 0) {
    std::string s;
    for (size_t i=0;i<params.size();++i) { if(i) s += ", "; s += "a"+std::to_string(i); }
    (void)offset; return s;
}

static void emitFreeBindings(std::ofstream& o, const std::string&, const std::vector<Fn>& fns) {
    for (const auto& f : fns) {
        o << "Value " << f.name << "_binding(const std::vector<Value>& args) {\n";
        o << "    if (args.size() != " << f.params.size() << ") throw std::runtime_error(\"zl-bind: arity mismatch for " << f.name << "\");\n";
        o << "    try {\n";
        emitParamConversions(o, f.params, 0);
        o << "    " << wrapResult(f.ret, f.name + "(" + callArgs(f.params) + ")") << "\n";
        o << "    } catch (const std::exception& e) { throw std::runtime_error(std::string(\"zl-bind: " + f.name + " native exception: \") + e.what()); } catch (...) { throw std::runtime_error(\"zl-bind: " + f.name + " native exception: unknown exception\"); }\n}\n\n";
    }
}

// The slot map shared by class handles and struct handles: an opaque integer
// id in ZL, the owner pointer behind it in C++, and a type tag so a handle
// cannot be loaded as the wrong struct/class.
static void emitSlotHelpers(std::ofstream& o) {
    o << "namespace {\n";
    o << "struct NativeSlot { void* ptr{}; std::string type; std::shared_ptr<void> owner; };\n";
    o << "std::mutex nativeSlotsMutex; std::unordered_map<uint64_t, NativeSlot> nativeSlots; uint64_t nextNativeSlot = 1;\n\n";
    o << "template<class T> uint64_t storeNative(std::shared_ptr<T> owner, const char* type) { std::lock_guard<std::mutex> lock(nativeSlotsMutex); uint64_t id=nextNativeSlot++; if(id==0) id=nextNativeSlot++; nativeSlots[id]={owner.get(),type,std::move(owner)}; return id; }\n";
    o << "template<class T> T* loadNative(uint64_t id, const char* type) { std::lock_guard<std::mutex> lock(nativeSlotsMutex); auto it=nativeSlots.find(id); if(it==nativeSlots.end() || it->second.type != type) throw std::runtime_error(std::string(\"zl-bind: invalid native handle for \")+type); return static_cast<T*>(it->second.ptr); }\n";
    o << "template<class T> void eraseNative(uint64_t id, const char* type) { std::lock_guard<std::mutex> lock(nativeSlotsMutex); auto it=nativeSlots.find(id); if(it==nativeSlots.end()) throw std::runtime_error(\"zl-bind: native handle already closed\"); if(it->second.type != type) throw std::runtime_error(std::string(\"zl-bind: native handle type mismatch for \")+type); nativeSlots.erase(it); }\n";
    o << "uint64_t retainNative(uint64_t id, const char* type) { std::lock_guard<std::mutex> lock(nativeSlotsMutex); auto it=nativeSlots.find(id); if(it==nativeSlots.end() || it->second.type != type) throw std::runtime_error(std::string(\"zl-bind: invalid native handle for \")+type); uint64_t next=nextNativeSlot++; if(next==0) next=nextNativeSlot++; nativeSlots[next]={it->second.ptr,it->second.type,it->second.owner}; return next; }\n";
    o << "}\n\n";
}

static void emitStructSchema(std::ofstream& o, const zl_bind::NativeStruct& s) {
    o << "namespace {\n";
    o << "const ZlFieldSchema " << s.name << "_schema[] = {";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        o << (i ? ", " : " ");
        o << "{\"" << s.fields[i].name << "\", \"" << mapType(s.fields[i].type) << "\", offsetof("
          << s.name << ", " << s.fields[i].name << "), sizeof(" << s.fields[i].type << ")}";
    }
    o << " };\n}\n";
    for (const auto& f : s.fields)
        o << "static_assert(offsetof(" << s.name << ", " << f.name << ") + sizeof(" << f.type
          << ") <= sizeof(" << s.name << "), \"zl-bind: struct schema field lies outside the struct " << s.name << "\");\n";
    o << "static_assert(alignof(" << s.name << ") >= 1, \"zl-bind: struct " << s.name << " has no usable alignment\");\n";
    o << "static const std::size_t " << s.name << "_schema_count = " << s.fields.size() << ";\n";
    o << "static_assert(sizeof(" << s.name << "_schema)/sizeof(ZlFieldSchema) == " << s.name << "_schema_count, \"zl-bind: struct schema arity drifted\");\n\n";
}

static void emitStructBindings(std::ofstream& o, const std::string& ns, const std::vector<zl_bind::NativeStruct>& structs) {
    o << "// The typed field schema the struct bindings are generated against; the\n";
    o << "// offsetof/sizeof entries are evaluated by the target compiler, so the\n";
    o << "// table describes the ABI the generated code was built for.\n";
    o << "struct ZlFieldSchema { const char* name; const char* type; std::size_t offset; std::size_t size; };\n\n";
    for (const auto& s : structs) {
        const std::string catchBlock =
            " catch (const std::exception& e) { throw std::runtime_error(std::string(\"zl-bind: \" + std::string(\"" + s.name + "\") + \" native exception: \") + e.what()); } catch (...) { throw std::runtime_error(\"zl-bind: " + s.name + " native exception: unknown exception\"); }";
        (void)ns;
        emitStructSchema(o, s);
        o << "Value " << s.name << "_new_binding(const std::vector<Value>& args) {\n";
        o << "    if (!args.empty()) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_new\");\n";
        o << "    auto owner = std::make_shared<" << s.name << ">(); // value-initialized: every field starts zeroed\n";
        o << "    return static_cast<int64_t>(storeNative<" << s.name << ">(std::move(owner), \"" << s.name << "\"));\n}\n\n";
        o << "Value " << s.name << "_close_binding(const std::vector<Value>& args) {\n";
        o << "    if(args.size()!=1) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_close\");\n";
        o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0])); if(id==0) return Value{};\n";
        o << "    eraseNative<" << s.name << ">(id, \"" << s.name << "\");\n    return Value{};\n}\n\n";
        o << "Value " << s.name << "_size_binding(const std::vector<Value>& args) {\n";
        o << "    if(!args.empty()) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_size\");\n";
        o << "    return static_cast<int64_t>(sizeof(" << s.name << "));\n}\n\n";
        for (const auto& f : s.fields) {
            const auto ft = mapType(f.type);
            o << "Value " << s.name << "_offset_" << f.name << "_binding(const std::vector<Value>& args) {\n";
            o << "    if(!args.empty()) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_offset_" << f.name << "\");\n";
            o << "    return static_cast<int64_t>(offsetof(" << s.name << ", " << f.name << "));\n}\n\n";
            o << "Value " << s.name << "_get_" << f.name << "_binding(const std::vector<Value>& args) {\n";
            o << "    if (args.size() != 1) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_get_" << f.name << "\");\n";
            o << "    try {\n";
            o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0])); if(id==0) throw std::runtime_error(\"zl-bind: null native handle for " << s.name << "\");\n";
            o << "    auto* self=loadNative<" << s.name << ">(id, \"" << s.name << "\");\n";
            if (ft == "int") o << "    return static_cast<int64_t>(self->" << f.name << ");\n";
            else if (ft == "double") o << "    return static_cast<double>(self->" << f.name << ");\n";
            else o << "    return static_cast<bool>(self->" << f.name << ");\n";
            o << "    }" << catchBlock << "\n}\n\n";
            o << "Value " << s.name << "_set_" << f.name << "_binding(const std::vector<Value>& args) {\n";
            o << "    if (args.size() != 2) throw std::runtime_error(\"zl-bind: arity mismatch for " << s.name << "_set_" << f.name << "\");\n";
            o << "    try {\n";
            o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0])); if(id==0) throw std::runtime_error(\"zl-bind: null native handle for " << s.name << "\");\n";
            o << "    auto* self=loadNative<" << s.name << ">(id, \"" << s.name << "\");\n";
            if (ft == "int") o << "    self->" << f.name << " = static_cast<" << f.type << ">(std::get<int64_t>(args[1]));\n";
            else if (ft == "double") o << "    self->" << f.name << " = static_cast<" << f.type << ">(std::holds_alternative<int64_t>(args[1]) ? static_cast<double>(std::get<int64_t>(args[1])) : std::get<double>(args[1]));\n";
            else o << "    self->" << f.name << " = static_cast<" << f.type << ">(std::get<bool>(args[1]));\n";
            o << "    return Value{};\n";
            o << "    }" << catchBlock << "\n}\n\n";
        }
    }
}

static void emitClassBindings(std::ofstream& o, const std::string&, const std::vector<NativeClass>& classes) {
    for (const auto& c : classes) {
        const std::string catchBlock =
            " catch (const std::exception& e) { throw std::runtime_error(std::string(\"zl-bind: " + c.name + " native exception: \") + e.what()); } catch (...) { throw std::runtime_error(\"zl-bind: " + c.name + " native exception: unknown exception\"); }";
        o << "Value " << c.name << "_new_binding(const std::vector<Value>& args) {\n";
        o << "    if (args.size() != " << c.constructorParams.size() << ") throw std::runtime_error(\"zl-bind: arity mismatch for " << c.name << "_new\");\n";
        o << "    try {\n";
        emitParamConversions(o, c.constructorParams, 0);
        o << "    auto owner = std::make_shared<" << c.name << ">(" << callArgs(c.constructorParams) << ");\n";
        o << "    return static_cast<int64_t>(storeNative<" << c.name << ">(std::move(owner), \"" << c.name << "\"));\n";
        o << "    }" << catchBlock << "\n}\n\n";

        if (c.ownership == "shared") {
            o << "Value " << c.name << "_retain_binding(const std::vector<Value>& args) {\n";
            o << "    if(args.size()!=1) throw std::runtime_error(\"zl-bind: arity mismatch for " << c.name << "_retain\");\n";
            o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0]));\n";
            o << "    return static_cast<int64_t>(retainNative(id, \"" << c.name << "\"));\n";
            o << "}\n\n";
        }
        o << "Value " << c.name << "_close_binding(const std::vector<Value>& args) {\n";
        o << "    if(args.size()!=1) throw std::runtime_error(\"zl-bind: arity mismatch for " << c.name << "_close\");\n";
        o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0])); if(id==0) return Value{};\n";
        o << "    eraseNative<" << c.name << ">(id, \"" << c.name << "\");\n";
        o << "    return Value{};\n}\n\n";

        for (const auto& m : c.methods) {
            o << "Value " << c.name << "_" << m.name << "_binding(const std::vector<Value>& args) {\n";
            o << "    if (args.size() != " << (m.params.size()+1) << ") throw std::runtime_error(\"zl-bind: arity mismatch for " << c.name << "_" << m.name << "\");\n";
            o << "    auto id=static_cast<uint64_t>(std::get<int64_t>(args[0])); if(id==0) throw std::runtime_error(\"zl-bind: null native handle for " << c.name << "\");\n";
            o << "    try {\n";
            o << "    auto* self=loadNative<" << c.name << ">(id, \"" << c.name << "\");\n";
            emitParamConversions(o, m.params, 1);
            std::string argsList = callArgs(m.params);
            o << "    " << wrapResult(m.ret, "self->" + m.name + "(" + argsList + ")") << "\n";
            o << "    }" << catchBlock << "\n}\n\n";
        }
    }
}

static void emitAbiGuards(std::ofstream& o) {
    o << "static_assert(sizeof(std::int64_t) == 8, \"zl-bind requires 64-bit int64_t\");\n";
    o << "static_assert(sizeof(double) == 8, \"zl-bind requires IEEE-compatible 64-bit double storage\");\n";
    o << "static_assert(sizeof(void*) == 4 || sizeof(void*) == 8, \"zl-bind requires a 32-bit or 64-bit pointer ABI\");\n";
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
    o << "#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ && __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__\n";
    o << "#error \"zl-bind requires a known byte order\"\n";
    o << "#endif\n";
#endif
}


static void emitCpp(const std::filesystem::path& header, const std::filesystem::path& out, const std::string& ns, const std::vector<Fn>& fns, const std::vector<NativeClass>& classes, const std::vector<zl_bind::NativeStruct>& structs) {
    std::ofstream o(out);
    o << "#include <cstddef>\n#include <cstdint>\n#include <mutex>\n#include <string>\n#include <unordered_map>\n#include <vector>\n#include <stdexcept>\n#include <utility>\n#include \"zl/vm/native.hpp\"\n#include \"" << header.filename().string() << "\"\n\n";
    o << "// Generated by zl-bind. Do not edit manually.\n";
    emitAbiGuards(o);
    o << "using zl::Value; using zl::NativeFunction; using zl::NativeId;\n";
    o << "namespace zl { namespace generated_bindings {\n";
    emitFreeBindings(o, ns, fns);
    if (!classes.empty() || !structs.empty()) emitSlotHelpers(o);
    if (!classes.empty()) emitClassBindings(o, ns, classes);
    if (!structs.empty()) emitStructBindings(o, ns, structs);
    o << "\nstd::vector<NativeFunction> table() { std::vector<NativeFunction> t;\n";
    for (const auto& f : fns) o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << f.name << "\", " << f.name << "_binding, " << f.params.size() << "});\n";
    for (const auto& c : classes) {
        o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << c.name << "_new\", " << c.name << "_new_binding, " << c.constructorParams.size() << "});\n";
        o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << c.name << "_close\", " << c.name << "_close_binding, 1});\n";
        if (c.ownership == "shared") o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << c.name << "_retain\", " << c.name << "_retain_binding, 1});\n";
        for (const auto& m : c.methods) o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << c.name << "_" << m.name << "\", " << c.name << "_" << m.name << "_binding, " << (m.params.size()+1) << "});\n";
    }
    for (const auto& s : structs) {
        o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_new\", " << s.name << "_new_binding, 0});\n";
        o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_close\", " << s.name << "_close_binding, 1});\n";
        o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_size\", " << s.name << "_size_binding, 0});\n";
        for (const auto& f : s.fields) {
            o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_offset_" << f.name << "\", " << s.name << "_offset_" << f.name << "_binding, 0});\n";
            o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_get_" << f.name << "\", " << s.name << "_get_" << f.name << "_binding, 1});\n";
            o << " t.push_back(NativeFunction{NativeId::EXTENSION, \"" << ns << "." << s.name << "_set_" << f.name << "\", " << s.name << "_set_" << f.name << "_binding, 2});\n";
        }
    }
    o << " return t; }\n\nstatic NativeFunctionRegistrar registrar(table());\n\n} }\n";
}

static std::string zlParamList(const std::vector<Param>& params) {
    std::string s;
    for (size_t i=0;i<params.size();++i) { if(i)s += ", "; s += mapType(params[i].type) + " " + params[i].name; }
    return s;
}

static std::string zlFieldType(const std::string& t) { return mapType(t); }

static void emitZl(const std::filesystem::path& out, const std::string& ns, const std::vector<Fn>& fns, const std::vector<NativeClass>& classes, const std::vector<zl_bind::NativeStruct>& structs) {
    std::ofstream o(out);
    o << "// Generated by zl-bind. Native wrappers are registered automatically when the generated C++ is linked.\n\n";
    for (const auto& f : fns) o << "// Native func: " << ns << "." << f.name << "(" << zlParamList(f.params) << "): " << mapType(f.ret) << "\n";
    for (const auto& s : structs) {
        o << "// Struct schema " << s.name << ": " << s.fields.size() << " typed field(s); handles are opaque, storage is zero-initialized.\n";
        o << "class " << s.name << " {\n    int handle\n\n    func " << s.name << "(): void {\n        this.handle = " << ns << "." << s.name << "_new()\n    }\n\n";
        for (const auto& f : s.fields) {
            const auto ft = zlFieldType(f.type);
            o << "    public func " << f.name << "(): " << ft << " {\n        return " << ns << "." << s.name << "_get_" << f.name << "(this.handle)\n    }\n\n";
            o << "    public func set_" << f.name << "(" << ft << " value): void {\n        " << ns << "." << s.name << "_set_" << f.name << "(this.handle, value)\n    }\n\n";
        }
        o << "    public func close(): void {\n        if (this.handle != 0) {\n            " << ns << "." << s.name << "_close(this.handle)\n            this.handle = 0\n        }\n    }\n}\n\n";
    }
    for (const auto& c : classes) {
        o << "class " << c.name << " {\n    int handle\n\n    func " << c.name << "(" << zlParamList(c.constructorParams) << "): void {\n        this.handle = " << ns << "." << c.name << "_new(";
        for(size_t i=0;i<c.constructorParams.size();++i){ if(i)o<<", "; o<<c.constructorParams[i].name; }
        o << ")\n    }\n\n";
        for (const auto& m : c.methods) {
            o << "    public func " << m.name << "(" << zlParamList(m.params) << "): " << mapType(m.ret) << " {\n        ";
            if (mapType(m.ret) == "void") o << ns << "." << c.name << "_" << m.name << "(this.handle";
            else o << "return " << ns << "." << c.name << "_" << m.name << "(this.handle";
            for (const auto& p : m.params) o << ", " << p.name;
            o << ")\n    }\n\n";
        }
        o << "    public func close(): void {\n        if (this.handle != 0) {\n            " << ns << "." << c.name << "_close(this.handle)\n            this.handle = 0\n        }\n    }\n}\n\n";
    }
}


static std::string currentAbiTag() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#elif defined(__linux__)
    const char* os = "linux";
#else
    const char* os = "unknown-os";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* arch = "x86";
#else
    const char* arch = "unknown-arch";
#endif
    return std::string(os) + "-" + arch + "-ptr" + std::to_string(sizeof(void*) * 8);
}

static void emitDocs(const std::filesystem::path& out, const std::string& ns,
                     const std::vector<Fn>& fns, const std::vector<NativeClass>& classes,
                     const std::vector<zl_bind::NativeStruct>& structs) {
    std::ofstream o(out);
    o << "# " << ns << " native bindings\n\n";
    o << "> Generated by `zl-bind`. Do not edit manually.\n\n";
    o << "## ABI requirements\n\n";
    o << "- `int64_t` must be 8 bytes.\n";
    o << "- `double` must be 8 bytes.\n";
    o << "- Pointer size must be 32 or 64 bits.\n";
    o << "- The target must have a known byte order.\n\n";
    o << "## Functions\n\n";
    if (fns.empty()) o << "None.\n\n";
    for (const auto& f : fns) {
        o << "### `" << ns << "." << f.name << "`\n\n";
        o << "`" << mapType(f.ret) << " " << f.name << "(" << zlParamList(f.params) << ")`\n\n";
        o << "- Ownership: `" << f.ownership << "`\n";
        o << "- Error mode: `" << f.errors << "`\n\n";
    }
    o << "## Native classes\n\n";
    if (classes.empty()) o << "None.\n\n";
    for (const auto& c : classes) {
        o << "### `" << c.name << "`\n\n";
        o << "- Ownership: `" << c.ownership << "`\n";
        o << "- Error mode: `" << c.errors << "`\n";
        o << "- Handle: opaque integer\n";
        o << "- Constructor parameters: " << c.constructorParams.size() << "\n";
        o << "- Deterministic `close()`: yes\n";
        if (c.ownership == "shared") o << "- `retain()`: yes\n";
        o << "\n#### Methods\n\n";
        for (const auto& m : c.methods) {
            o << "- `" << m.name << "(" << zlParamList(m.params) << "): " << mapType(m.ret)
              << (m.isConst ? "` — const" : "`") << "\n";
        }
        o << "\n";
    }
    o << "## Struct schemas\n\n";
    if (structs.empty()) o << "None.\n\n";
    for (const auto& s : structs) {
        o << "### `" << s.name << "`\n\n";
        o << "- Layout: compile-time schema table (`offsetof`/`sizeof` evaluated by the target compiler, guarded by `static_assert`)\n";
        o << "- Storage: heap value-initialized (all fields start zeroed), freed by `close()`\n";
        o << "- Handles: opaque integer, type-checked per access\n";
        o << "- Field access: typed `get_<field>`/`set_<field>` bindings per field, plus `size` and `offset_<field>` queries\n\n";
        o << "| Field | C type | ZL type |\n| --- | --- | --- |\n";
        for (const auto& f : s.fields) o << "| `" << f.name << "` | `" << f.type << "` | `" << mapType(f.type) << "` |\n";
        o << "\n";
    }
    o << "## Compatibility\n\n";
    o << "The generated C++ contains compile-time ABI guards. Re-run `zl-bind` for each target "
         "architecture/OS and treat the generated manifest and documentation as target-specific artifacts.\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 4) { std::cerr << "usage: zl-bind <header> <namespace> <output-dir>\n"; return 2; }
        auto parsed = zl_bind::parseHeaderFile(argv[1]);
        auto fns = std::move(parsed.functions);
        auto classes = std::move(parsed.classes);
        auto parsed_structs = std::move(parsed.structs);
        {
            std::set<std::string> runtimeNames;
            const auto claim = [&](const std::string& name) {
                if (!runtimeNames.insert(name).second)
                    throw std::runtime_error("binding name collision: '" + name +
                        "' is generated twice; rename the C++ declaration");
            };
            for (const auto& f : fns) claim(std::string(argv[2]) + "." + f.name);
            for (const auto& c : classes) {
                const std::string prefix = std::string(argv[2]) + "." + c.name + "_";
                claim(prefix + "new");
                claim(prefix + "close");
                if (c.ownership == "shared") claim(prefix + "retain");
                for (const auto& m : c.methods) claim(prefix + m.name);
            }
            for (const auto& s : parsed_structs) {
                const std::string prefix = std::string(argv[2]) + "." + s.name + "_";
                claim(prefix + "new");
                claim(prefix + "close");
                claim(prefix + "size");
                for (const auto& f : s.fields) {
                    if (f.name == "handle" || f.name == "close")
                        throw std::runtime_error("struct field name collides with the generated facade: " + s.name + "." + f.name);
                    claim(prefix + "offset_" + f.name);
                    claim(prefix + "get_" + f.name);
                    claim(prefix + "set_" + f.name);
                }
            }
        }
        std::filesystem::path dir = argv[3]; std::filesystem::create_directories(dir);
        std::filesystem::path header = std::filesystem::absolute(argv[1]);
        emitCpp(header, dir / (std::string(argv[2]) + "_bindings.cpp"), argv[2], fns, classes, parsed_structs);
        emitZl(dir / (std::string(argv[2]) + ".zl"), argv[2], fns, classes, parsed_structs);
        emitDocs(dir / (std::string(argv[2]) + ".md"), argv[2], fns, classes, parsed_structs);
        std::ofstream manifest(dir / (std::string(argv[2]) + ".zlbind"));
        manifest << "version=3\nnamespace=" << argv[2] << "\n";
        manifest << "abi=" << currentAbiTag() << "\n";
        manifest << "int64_bits=64\n";
        manifest << "double_bits=64\n";
        manifest << "pointer_bits=" << (sizeof(void*) * 8) << "\n";
        for (const auto& f : fns) manifest << "func|" << f.name << "|" << mapType(f.ret) << "|" << f.params.size() << "|ownership=" << f.ownership << "|errors=" << f.errors << "\n";
        for (const auto& c : classes) {
            manifest << "class|" << c.name << "|constructor=" << c.constructorParams.size() << "|ownership=" << c.ownership << "|errors=" << c.errors << "|handles=integer"; if (c.ownership == "shared") manifest << "|retain=true"; manifest << "\n";
            for (const auto& m : c.methods) manifest << "method|" << c.name << "." << m.name << "|" << mapType(m.ret) << "|" << (m.params.size()+1) << "|const=" << (m.isConst?"true":"false") << "\n";
            manifest << "destructor|" << c.name << "|supported=" << (c.hasDestructor?"true":"generated") << "\n";
        }
        for (const auto& s : parsed_structs) {
            manifest << "struct|" << s.name << "|fields=" << s.fields.size() << "|storage=zero-initialized|handles=integer\n";
            for (const auto& f : s.fields)
                manifest << "field|" << s.name << "." << f.name << "|" << mapType(f.type) << "|ctype=" << f.type << "|access=getter+setter\n";
        }
        std::cout << "zl-bind: generated " << fns.size() << " func binding(s), " << classes.size() << " class binding(s) and " << parsed_structs.size() << " struct schema(s)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "zl-bind: " << e.what() << "\n"; return 1; }
}
