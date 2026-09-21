#include "zl/compiler/native_catalog.hpp"
#include "zl/common/type_name.hpp"
#include "zl/compiler/semantic_types.hpp"

namespace zl {

// ---------------------------------------------------------------------------
// NativeSignature table
// ---------------------------------------------------------------------------
// This mirrors the runtime nativeFunctionTable() in native.cpp but carries
// compile-time type information instead of C++ callbacks.
// ---------------------------------------------------------------------------

namespace {

std::vector<NativeSignature> buildSignatureTable() {
    std::vector<NativeSignature> sigs;

    // Declare native type relationships once. Coarse kinds still drive native
    // dispatch; compiler inference and VM result validation share the templates.
    const auto generic = [&](NativeId id, const char* name, std::vector<std::string> parameters,
                             const char* result, const char* package = "zl.lang") {
        const auto first = parseTypeName(parameters.front());
        const auto kind = [&](const std::string& type) {
            const auto parsed = parseTypeName(type);
            for (const auto& parameter : first.args) if (parameter.name == parsed.name) return ZlType::UNKNOWN;
            return zlTypeFromBaseName(parsed.name);
        };
        std::vector<ZlType> kinds;
        for (const auto& parameter : parameters) kinds.push_back(kind(parameter));
        NativeSignature signature{id, name, std::move(kinds), kind(result), package, {},
                                  NativeReturnTypeRule::FIRST_ARGUMENT_TYPES, ZlType::UNKNOWN, result};
        signature.parameterTypeNames = std::move(parameters);
        if (first.name == "list" || first.name == "set" || first.name == "array")
            signature.acceptedParamTypes = {{ZlType::LIST, ZlType::ARRAY, ZlType::SET, ZlType::UNKNOWN}};
        sigs.push_back(std::move(signature));
    };

    // --- Reflection / Type ---
    // The catalog owns language metadata; VM callbacks bind by NativeId.
    sigs.push_back({NativeId::TYPE_NAME, "Type.name",    {ZlType::UNKNOWN}, ZlType::STRING});
    sigs.push_back({NativeId::TYPE_FIELDS, "Type.fields",  {ZlType::OBJECT},  ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<Field>"});
    sigs.push_back({NativeId::TYPE_METHODS, "Type.methods", {ZlType::OBJECT},  ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<Method>"});
    sigs.push_back({NativeId::TYPE_BASE, "Type.base",    {ZlType::OBJECT},  ZlType::STRING});
    sigs.push_back({NativeId::TYPE_CALLABLE, "Type.callable", {ZlType::OBJECT}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Function"});
    sigs.push_back({NativeId::TYPE_KIND, "Type.kind", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::TYPE_IS_DATA, "Type.isData", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::TYPE_IS_ENUM, "Type.isEnum", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::TYPE_ENUM_MEMBERS, "Type.enumMembers", {ZlType::OBJECT}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::TYPE_OF, "Type.of", {ZlType::UNKNOWN}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Type"});
    sigs.push_back({NativeId::SHARED_SHARE, "share", {ZlType::UNKNOWN}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Shared"});
    sigs.push_back({NativeId::SHARED_GET, "Shared.__get", {ZlType::OBJECT}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::SHARED_SET, "Shared.__set", {ZlType::OBJECT, ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SHARED_WITHLOCK, "Shared.__withLock", {ZlType::OBJECT, ZlType::FUNCTION}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::REFLECTION_NAME, "Reflection.name", {ZlType::UNKNOWN}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_KIND, "Reflection.kind", {ZlType::UNKNOWN}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_IS_DATA, "Reflection.isData", {ZlType::UNKNOWN}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_IS_ENUM, "Reflection.isEnum", {ZlType::UNKNOWN}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_ENUM_MEMBERS, "Reflection.enumMembers", {ZlType::UNKNOWN}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_FIELDS, "Reflection.fields", {ZlType::UNKNOWN}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<Field>"});
    sigs.push_back({NativeId::REFLECTION_METHODS, "Reflection.methods", {ZlType::UNKNOWN}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<Method>"});
    sigs.push_back({NativeId::REFLECTION_BASE, "Reflection.base", {ZlType::UNKNOWN}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_INTERFACES, "Reflection.interfaces", {ZlType::UNKNOWN}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_TYPE_PARAMETERS, "Reflection.typeParameters", {ZlType::UNKNOWN}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_TYPE_CONSTRUCTORS, "Reflection.constructors", {ZlType::OBJECT}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<Constructor>"});
    sigs.push_back({NativeId::REFLECTION_FIELD, "Reflection.field", {ZlType::OBJECT, ZlType::STRING}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Field"});
    sigs.push_back({NativeId::REFLECTION_METHOD, "Reflection.method", {ZlType::OBJECT, ZlType::STRING}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Method"});
    sigs.push_back({NativeId::REFLECTION_CONSTRUCTOR, "Reflection.constructor", {ZlType::OBJECT, ZlType::INT}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Constructor"});
    sigs.push_back({NativeId::REFLECTION_METHOD_INVOKE, "Reflection.methodInvoke", {ZlType::OBJECT, ZlType::OBJECT, ZlType::LIST}, ZlType::OBJECT});
    sigs.push_back({NativeId::REFLECTION_CONSTRUCTOR_INVOKE, "Reflection.constructorInvoke", {ZlType::OBJECT, ZlType::LIST}, ZlType::OBJECT});
    sigs.push_back({NativeId::REFLECTION_FIELD_NAME, "Reflection.fieldName", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_FIELD_TYPE, "Reflection.fieldType", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_FIELD_ACCESS, "Reflection.fieldAccess", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_METHOD_NAME, "Reflection.methodName", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_METHOD_RETURN_TYPE, "Reflection.methodReturnType", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_METHOD_ACCESS, "Reflection.methodAccess", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_METHOD_STATIC, "Reflection.methodIsStatic", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_METHOD_ASYNC, "Reflection.methodIsAsync", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_METHOD_PARAMETERS, "Reflection.methodParameters", {ZlType::OBJECT}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_CONSTRUCTOR_PARAMETERS, "Reflection.constructorParameters", {ZlType::OBJECT}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_FUNCTION, "Reflection.function", {ZlType::OBJECT}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Function"});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_NAME, "Reflection.functionName", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_PARAMETERS, "Reflection.functionParameters", {ZlType::OBJECT}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_RETURN_TYPE, "Reflection.functionReturnType", {ZlType::OBJECT}, ZlType::STRING});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_ASYNC, "Reflection.functionIsAsync", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_IS_NATIVE, "Reflection.functionIsNative", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::REFLECTION_FUNCTION_INVOKE, "Reflection.functionInvoke", {ZlType::OBJECT, ZlType::LIST}, ZlType::OBJECT});
    sigs.push_back({NativeId::MATH_SQRT, "Math.sqrt",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ABS, "Math.abs",   {ZlType::DOUBLE}, ZlType::DOUBLE, "zl.lang", {{ZlType::INT, ZlType::DOUBLE}}, NativeReturnTypeRule::SAME_NUMERIC_KIND});
    sigs.push_back({NativeId::MATH_POW, "Math.pow",   {ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_FLOOR, "Math.floor", {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_CEIL, "Math.ceil",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ROUND, "Math.round", {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_MIN, "Math.min",   {ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::DOUBLE, "zl.lang", {{ZlType::INT, ZlType::DOUBLE}, {ZlType::INT, ZlType::DOUBLE}}, NativeReturnTypeRule::SAME_NUMERIC_KIND});
    sigs.push_back({NativeId::MATH_MAX, "Math.max",   {ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::DOUBLE, "zl.lang", {{ZlType::INT, ZlType::DOUBLE}, {ZlType::INT, ZlType::DOUBLE}}, NativeReturnTypeRule::SAME_NUMERIC_KIND});
    sigs.push_back({NativeId::MATH_CBRT, "Math.cbrt",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_TRUNC, "Math.trunc", {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_SIN, "Math.sin",   {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_COS, "Math.cos",   {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_TAN, "Math.tan",   {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ASIN, "Math.asin",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ACOS, "Math.acos",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ATAN, "Math.atan",  {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_ATAN2, "Math.atan2", {ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_LOG, "Math.log",   {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_LOG10, "Math.log10", {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_EXP, "Math.exp",   {ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_RANDOM, "Math.random", {}, ZlType::DOUBLE});
    sigs.push_back({NativeId::MATH_RANDOMINT, "Math.randomInt", {ZlType::INT, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::MATH_RANDOMFLOAT, "Math.randomFloat", {ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::DOUBLE});
    sigs.push_back({NativeId::IO_PRINT, "IO.print",      {ZlType::UNKNOWN}, ZlType::VOID_TYPE});  // accepts any type
    sigs.push_back({NativeId::IO_PRINTLN, "IO.println",    {ZlType::UNKNOWN}, ZlType::VOID_TYPE});  // accepts any type
    sigs.push_back({NativeId::IO_READLINE, "IO.readLine", {},                   ZlType::STRING});
    sigs.push_back({NativeId::IO_CLEAR, "IO.clear",     {},                   ZlType::VOID_TYPE});
    sigs.push_back({NativeId::COLLECTION_NEWLIST, "Collection.newList",   {},                                     ZlType::LIST});
    generic(NativeId::COLLECTION_PUSH, "Collection.push", {"list<T>", "T"}, "void");
    generic(NativeId::COLLECTION_POP, "Collection.pop", {"list<T>"}, "T");
    generic(NativeId::COLLECTION_GET, "Collection.get", {"list<T>", "int"}, "T");
    generic(NativeId::COLLECTION_SET, "Collection.set", {"list<T>", "int", "T"}, "void");
    sigs.push_back({NativeId::COLLECTION_LENGTH, "Collection.length",    {ZlType::UNKNOWN},                      ZlType::INT, "zl.lang", {{ZlType::LIST, ZlType::ARRAY, ZlType::SET, ZlType::MAP, ZlType::STRING, ZlType::UNKNOWN}}});
    sigs.push_back({NativeId::COLLECTION_NEWMAP, "Collection.newMap",    {},                                     ZlType::MAP});
    generic(NativeId::COLLECTION_MAPSET, "Collection.mapSet", {"map<K,V>", "K", "V"}, "void");
    generic(NativeId::COLLECTION_MAPGET, "Collection.mapGet", {"map<K,V>", "K"}, "V");
    generic(NativeId::COLLECTION_MAPHAS, "Collection.mapHas", {"map<K,V>", "K"}, "bool");
    generic(NativeId::COLLECTION_MAPREMOVE, "Collection.mapRemove", {"map<K,V>", "K"}, "void");
    generic(NativeId::COLLECTION_MAPKEYS, "Collection.mapKeys", {"map<K,V>"}, "list<K>");
    generic(NativeId::COLLECTION_MAPVALUES, "Collection.mapValues", {"map<K,V>"}, "list<V>");
    sigs.push_back({NativeId::COLLECTION_NEWSET, "Collection.newSet",    {},                                     ZlType::SET});
    generic(NativeId::COLLECTION_SETADD, "Collection.setAdd", {"set<T>", "T"}, "void");
    generic(NativeId::COLLECTION_SETHAS, "Collection.setHas", {"set<T>", "T"}, "bool");
    generic(NativeId::COLLECTION_SETREMOVE, "Collection.setRemove", {"set<T>", "T"}, "void");
    generic(NativeId::COLLECTION_SETITEMS, "Collection.setItems", {"set<T>"}, "list<T>");
    sigs.push_back({NativeId::QUEUE_NEWQUEUE, "Queue.newQueue", {},                             ZlType::LIST, "zl.util.Queue"});
    generic(NativeId::QUEUE_ENQUEUE, "Queue.enqueue", {"list<T>", "T"}, "void", "zl.util.Queue");
    generic(NativeId::QUEUE_DEQUEUE, "Queue.dequeue", {"list<T>"}, "T", "zl.util.Queue");
    generic(NativeId::QUEUE_PEEK, "Queue.peek", {"list<T>"}, "T", "zl.util.Queue");
    sigs.push_back({NativeId::QUEUE_ISEMPTY, "Queue.isEmpty",  {ZlType::LIST},                 ZlType::BOOL, "zl.util.Queue"});
    sigs.push_back({NativeId::QUEUE_SIZE, "Queue.size", {ZlType::LIST}, ZlType::INT, "zl.util.Queue"});
    sigs.push_back({NativeId::STACK_NEWSTACK, "Stack.newStack", {},                             ZlType::LIST, "zl.util.Stack"});
    generic(NativeId::STACK_PUSH, "Stack.push", {"list<T>", "T"}, "void", "zl.util.Stack");
    generic(NativeId::STACK_POP, "Stack.pop", {"list<T>"}, "T", "zl.util.Stack");
    generic(NativeId::STACK_PEEK, "Stack.peek", {"list<T>"}, "T", "zl.util.Stack");
    sigs.push_back({NativeId::STACK_ISEMPTY, "Stack.isEmpty",  {ZlType::LIST},                 ZlType::BOOL, "zl.util.Stack"});
    sigs.push_back({NativeId::STACK_SIZE, "Stack.size", {ZlType::LIST}, ZlType::INT, "zl.util.Stack"});
    sigs.push_back({NativeId::STRING_LENGTH, "String.length",    {ZlType::STRING},                           ZlType::INT});
    sigs.push_back({NativeId::STRING_UPPER, "String.upper",     {ZlType::STRING},                           ZlType::STRING});
    sigs.push_back({NativeId::STRING_LOWER, "String.lower",     {ZlType::STRING},                           ZlType::STRING});
    sigs.push_back({NativeId::STRING_TRIM, "String.trim",      {ZlType::STRING},                           ZlType::STRING});
    sigs.push_back({NativeId::STRING_CONTAINS, "String.contains",  {ZlType::STRING, ZlType::STRING},           ZlType::BOOL});
    // startsWith/endsWith are the same byte-prefix/suffix test the string
    // method surface exposes as `s.startsWith(p)`; Text.startsWith and
    // Text.endsWith delegate here rather than carrying a second ZL copy.
    sigs.push_back({NativeId::STRING_STARTSWITH, "String.startsWith", {ZlType::STRING, ZlType::STRING},        ZlType::BOOL});
    sigs.push_back({NativeId::STRING_ENDSWITH, "String.endsWith",  {ZlType::STRING, ZlType::STRING},           ZlType::BOOL});
    sigs.push_back({NativeId::STRING_INDEXOF, "String.indexOf",   {ZlType::STRING, ZlType::STRING},           ZlType::INT});
    sigs.push_back({NativeId::STRING_CHARAT, "String.charAt",    {ZlType::STRING, ZlType::INT},              ZlType::STRING});
    sigs.push_back({NativeId::STRING_SUBSTRING, "String.substring", {ZlType::STRING, ZlType::INT, ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_REPLACE, "String.replace",   {ZlType::STRING, ZlType::STRING, ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_SPLIT, "String.split", {ZlType::STRING, ZlType::STRING}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::STRING_TOINT, "String.toInt",     {ZlType::STRING},                           ZlType::INT});
    sigs.push_back({NativeId::STRING_TOFLOAT, "String.toFloat",   {ZlType::STRING},                           ZlType::DOUBLE});
    sigs.push_back({NativeId::STRING_LASTINDEXOF, "String.lastIndexOf", {ZlType::STRING, ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::STRING_INDEXOFFROM, "String.indexOfFrom", {ZlType::STRING, ZlType::STRING, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::STRING_TRIMSTART, "String.trimStart", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_TRIMEND, "String.trimEnd", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_COMPARE, "String.compare", {ZlType::STRING, ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::STRING_COMPAREIGNORECASE, "String.compareIgnoreCase", {ZlType::STRING, ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::STRING_CODEPOINTAT, "String.codePointAt", {ZlType::STRING, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::STRING_FROMCODEPOINT, "String.fromCodePoint", {ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_REPEAT, "String.repeatText", {ZlType::STRING, ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_UTF8LENGTH, "String.utf8Length", {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::STRING_UTF8CHARAT, "String.utf8CharAt", {ZlType::STRING, ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_UTF8SUBSTRING, "String.utf8Substring", {ZlType::STRING, ZlType::INT, ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_UTF8REVERSE, "String.utf8Reverse", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_UTF8CODEPOINTAT, "String.utf8CodePointAt", {ZlType::STRING, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::STRING_UTF8FROMCODEPOINT, "String.utf8FromCodePoint", {ZlType::INT}, ZlType::STRING});
    sigs.push_back({NativeId::STRING_UTF8BYTEINDEX, "String.utf8ByteIndex", {ZlType::STRING, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::STRING_UTF8INDEXFROMBYTE, "String.utf8IndexFromByte", {ZlType::STRING, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::FILESYSTEM_READFILE, "FileSystem.readFile",   {ZlType::STRING},                      ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_WRITEFILE, "FileSystem.writeFile",  {ZlType::STRING, ZlType::STRING},      ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_APPENDFILE, "FileSystem.appendFile", {ZlType::STRING, ZlType::STRING},      ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_EXISTS, "FileSystem.exists",     {ZlType::STRING},                      ZlType::BOOL});
    sigs.push_back({NativeId::FILESYSTEM_DELETEFILE, "FileSystem.deleteFile", {ZlType::STRING},                      ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_LISTDIR, "FileSystem.listDir", {ZlType::STRING}, ZlType::LIST, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::FILESYSTEM_RENAME, "FileSystem.rename", {ZlType::STRING, ZlType::STRING}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_COPYFILE, "FileSystem.copyFile", {ZlType::STRING, ZlType::STRING}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_SIZE, "FileSystem.size", {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::FILESYSTEM_ISFILE, "FileSystem.isFile", {ZlType::STRING}, ZlType::BOOL});
    sigs.push_back({NativeId::FILESYSTEM_ISDIRECTORY, "FileSystem.isDirectory", {ZlType::STRING}, ZlType::BOOL});
    sigs.push_back({NativeId::FILESYSTEM_CREATEDIR, "FileSystem.createDir", {ZlType::STRING}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::FILESYSTEM_REMOVEDIR, "FileSystem.removeDir", {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::FILESYSTEM_MODIFIEDTIME, "FileSystem.modifiedTime", {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::FILESYSTEM_ABSOLUTEPATH, "FileSystem.absolutePath", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_PARENTPATH, "FileSystem.parentPath", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_FILENAME, "FileSystem.fileName", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_EXTENSION, "FileSystem.extension", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_STEM, "FileSystem.stem", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_JOINPATH, "FileSystem.joinPath", {ZlType::STRING, ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_CURRENTDIR, "FileSystem.currentDir", {}, ZlType::STRING});
    sigs.push_back({NativeId::FILESYSTEM_TEMPDIR, "FileSystem.tempDir", {}, ZlType::STRING});
    sigs.push_back({NativeId::NETWORK_RESOLVE, "Network.resolve", {ZlType::STRING}, ZlType::STRING, "zl.net.Network"});
    sigs.push_back({NativeId::NETWORK_RESOLVEALL, "Network.resolveAll", {ZlType::STRING}, ZlType::LIST, "zl.net.Network", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::NETWORK_HOSTNAME, "Network.hostname", {}, ZlType::STRING, "zl.net.Network"});
    sigs.push_back({NativeId::NETWORK_ISVALIDIP, "Network.isValidIp", {ZlType::STRING}, ZlType::BOOL, "zl.net.Network"});
    sigs.push_back({NativeId::TIME_NOW, "Time.now",        {},              ZlType::INT});
    sigs.push_back({NativeId::TIME_NOWMILLIS, "Time.nowMillis",  {},              ZlType::INT});
    sigs.push_back({NativeId::TIME_SLEEP, "Time.sleep",      {ZlType::DOUBLE}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TIME_SLEEP_ASYNC, "Time.sleepAsync", {ZlType::DOUBLE}, ZlType::VOID_TYPE, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TIME_YEAR, "Time.year",       {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_MONTH, "Time.month",      {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_DAY, "Time.day",        {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_HOUR, "Time.hour",       {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_MINUTE, "Time.minute",     {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_SECOND, "Time.second",     {ZlType::INT},   ZlType::INT});
    sigs.push_back({NativeId::TIME_FORMAT, "Time.format",     {ZlType::INT, ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::TIME_MONOTONICMILLIS, "Time.monotonicMillis", {}, ZlType::INT});
    sigs.push_back({NativeId::TIME_DAYOFWEEK, "Time.dayOfWeek", {ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::TIME_DAYOFYEAR, "Time.dayOfYear", {ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::TIME_ISLEAPYEAR, "Time.isLeapYear", {ZlType::INT}, ZlType::BOOL});
    sigs.push_back({NativeId::TIME_FROMPARTS, "Time.fromParts", {ZlType::INT, ZlType::INT, ZlType::INT, ZlType::INT, ZlType::INT, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::TIME_UTCFORMAT, "Time.utcFormat", {ZlType::INT, ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::THREAD_START, "Thread.start", {ZlType::FUNCTION}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Thread"});
    sigs.push_back({NativeId::THREAD_JOIN, "Thread.join", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::THREAD_ISALIVE, "Thread.isAlive", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::TASK_SPAWN, "Task.spawn", {ZlType::FUNCTION}, ZlType::UNKNOWN, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::VOID_TYPE, "Task"});
    sigs.push_back({NativeId::MUTEX_WITHLOCK, "Mutex.withLock", {ZlType::OBJECT, ZlType::FUNCTION}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::RWLOCK_WITHREAD, "RwLock.withRead", {ZlType::OBJECT, ZlType::FUNCTION}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::RWLOCK_WITHWRITE, "RwLock.withWrite", {ZlType::OBJECT, ZlType::FUNCTION}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::ATOMIC_LOAD, "Atomic.load", {ZlType::OBJECT}, ZlType::INT});
    sigs.push_back({NativeId::ATOMIC_STORE, "Atomic.store", {ZlType::OBJECT, ZlType::INT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::ATOMIC_ADD, "Atomic.add", {ZlType::OBJECT, ZlType::INT}, ZlType::INT});
    sigs.push_back({NativeId::ATOMIC_LOAD_BOOL, "Atomic.loadBool", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::ATOMIC_STORE_BOOL, "Atomic.storeBool", {ZlType::OBJECT, ZlType::BOOL}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::ATOMIC_LOAD_DOUBLE, "Atomic.loadDouble", {ZlType::OBJECT}, ZlType::DOUBLE});
    sigs.push_back({NativeId::ATOMIC_STORE_DOUBLE, "Atomic.storeDouble", {ZlType::OBJECT, ZlType::DOUBLE}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::ATOMIC_LOAD_REF, "Atomic.loadRef", {ZlType::OBJECT}, ZlType::OBJECT});
    sigs.push_back({NativeId::ATOMIC_STORE_REF, "Atomic.storeRef", {ZlType::OBJECT, ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SEMAPHORE_ACQUIRE, "Semaphore.acquire", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SEMAPHORE_RELEASE, "Semaphore.release", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SEMAPHORE_AVAILABLE, "Semaphore.available", {ZlType::OBJECT}, ZlType::INT});
    sigs.push_back({NativeId::SEMAPHORE_SETPERMITS, "Semaphore.setPermits", {ZlType::OBJECT, ZlType::INT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SEMAPHORE_TRYACQUIRE, "Semaphore.tryAcquire", {ZlType::OBJECT}, ZlType::BOOL});
    sigs.push_back({NativeId::SEMAPHORE_RELEASEMANY, "Semaphore.releaseMany", {ZlType::OBJECT, ZlType::INT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::CONDITION_WAIT, "Condition.wait", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::CONDITION_NOTIFYONE, "Condition.notifyOne", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::CONDITION_NOTIFYALL, "Condition.notifyAll", {ZlType::OBJECT}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::CONDITION_WAITFOR, "Condition.waitFor", {ZlType::OBJECT, ZlType::DOUBLE}, ZlType::BOOL});
    sigs.push_back({NativeId::CHANNEL_CREATE, "Channel.create", {ZlType::INT}, ZlType::OBJECT, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "Channel"});
    sigs.push_back({NativeId::CHANNEL_SEND, "Channel.send", {ZlType::OBJECT, ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::CHANNEL_RECEIVE, "Channel.receive", {ZlType::OBJECT}, ZlType::UNKNOWN});
    sigs.push_back({NativeId::CHANNEL_SIZE, "Channel.size", {ZlType::OBJECT}, ZlType::INT});
    sigs.push_back({NativeId::CHANNEL_SEND_ASYNC, "Channel.sendAsync", {ZlType::OBJECT, ZlType::UNKNOWN}, ZlType::TASK});
    sigs.push_back({NativeId::CHANNEL_RECEIVE_ASYNC, "Channel.receiveAsync", {ZlType::OBJECT}, ZlType::TASK});
    sigs.push_back({NativeId::TEST_FAIL, "Test.fail",         {ZlType::STRING}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TEST_ASSERTTRUE, "Test.assertTrue",   {ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TEST_ASSERTFALSE, "Test.assertFalse",  {ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TEST_ASSERTEQUAL, "Test.assertEqual",  {ZlType::UNKNOWN, ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TEST_ASSERTNOTEQUAL, "Test.assertNotEqual", {ZlType::UNKNOWN, ZlType::UNKNOWN}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::TEST_ASSERTNEAR, "Test.assertNear",   {ZlType::DOUBLE, ZlType::DOUBLE, ZlType::DOUBLE}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::LOG_INFO, "Log.info", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_WARN, "Log.warn", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_ERROR, "Log.error", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_DEBUG, "Log.debug", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_TRACE, "Log.trace", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_FATAL, "Log.fatal", {ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::LOG_AT, "Log.at", {ZlType::STRING, ZlType::UNKNOWN}, ZlType::VOID_TYPE, "zl.logging.Log"});
    sigs.push_back({NativeId::TEXT_FORMAT, "Text.format", {ZlType::STRING, ZlType::LIST}, ZlType::STRING, "zl.text.Text"});
    sigs.push_back({NativeId::TEXT_REGEXMATCHES, "Text.regexMatches", {ZlType::STRING, ZlType::STRING}, ZlType::BOOL, "zl.text.Text"});
    sigs.push_back({NativeId::TEXT_REGEXFULLMATCHES, "Text.regexFullMatches", {ZlType::STRING, ZlType::STRING}, ZlType::BOOL, "zl.text.Text"});
    sigs.push_back({NativeId::TEXT_REGEXFINDALL, "Text.regexFindAll", {ZlType::STRING, ZlType::STRING}, ZlType::LIST, "zl.text.Text", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<string>"});
    sigs.push_back({NativeId::TEXT_REGEXREPLACE, "Text.regexReplace", {ZlType::STRING, ZlType::STRING, ZlType::STRING}, ZlType::STRING, "zl.text.Text"});
    sigs.push_back({NativeId::TEXT_REGEXFIND, "Text.regexFind", {ZlType::STRING, ZlType::STRING}, ZlType::OBJECT, "zl.text.Text", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "RegexMatch"});
    sigs.push_back({NativeId::TEXT_REGEXFINDMATCHES, "Text.regexFindMatches", {ZlType::STRING, ZlType::STRING}, ZlType::LIST, "zl.text.Text", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "list<RegexMatch>"});
    sigs.push_back({NativeId::SERIALIZE_ENCODE, "Serialize.encode", {ZlType::UNKNOWN}, ZlType::STRING, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::SERIALIZE_DECODE, "Serialize.decode", {ZlType::STRING}, ZlType::UNKNOWN, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::SERIALIZE_ASSTRING, "Serialize.asString", {ZlType::UNKNOWN}, ZlType::STRING, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::SERIALIZE_ASINT, "Serialize.asInt", {ZlType::UNKNOWN}, ZlType::INT, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::SERIALIZE_ASDOUBLE, "Serialize.asDouble", {ZlType::UNKNOWN}, ZlType::DOUBLE, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::SERIALIZE_ASBOOL, "Serialize.asBool", {ZlType::UNKNOWN}, ZlType::BOOL, "zl.serialize.Serialize"});
    sigs.push_back({NativeId::CRYPTO_CRC32, "Crypto.crc32", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_SHA256, "Crypto.sha256", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_SHA1, "Crypto.sha1", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_MD5, "Crypto.md5", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_FNV1A64, "Crypto.fnv1a64", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_HEXENCODE, "Crypto.hexEncode", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_HEXDECODE, "Crypto.hexDecode", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_BASE64ENCODE, "Crypto.base64Encode", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::CRYPTO_BASE64DECODE, "Crypto.base64Decode", {ZlType::STRING}, ZlType::STRING, "zl.crypto.Crypto"});
    sigs.push_back({NativeId::HASH_CODE, "Hash.code", {ZlType::UNKNOWN}, ZlType::INT, "zl.lang"});
    sigs.push_back({NativeId::SYSTEM_EXIT, "System.exit",   {ZlType::INT},    ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SYSTEM_GETENV, "System.getEnv", {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::SYSTEM_EXEC, "System.exec",   {ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::SYSTEM_EXECSTATUS, "System.execStatus", {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::SYSTEM_SETENV, "System.setEnv", {ZlType::STRING, ZlType::STRING}, ZlType::VOID_TYPE});
    sigs.push_back({NativeId::SYSTEM_HASENV, "System.hasEnv", {ZlType::STRING}, ZlType::BOOL});
    sigs.push_back({NativeId::SYSTEM_ENVOR, "System.envOr", {ZlType::STRING, ZlType::STRING}, ZlType::STRING});
    sigs.push_back({NativeId::SYSTEM_PLATFORM, "System.platform", {}, ZlType::STRING});
    sigs.push_back({NativeId::INT_PARSE, "Int.parse",    {ZlType::STRING}, ZlType::INT});
    sigs.push_back({NativeId::DOUBLE_PARSE, "Double.parse", {ZlType::STRING}, ZlType::DOUBLE});
    sigs.push_back({NativeId::BOOL_PARSE, "Bool.parse",   {ZlType::STRING}, ZlType::BOOL});
    // Phase 0 benchmark measurement surface (memory-domains.md §9/§10): the
    // process-wide collector counters as a plain string-keyed map of ints.
    // Returns a bare map so the caller can read any counter by name without a
    // fixed shape; the keys are stable and documented in gcStats().
    sigs.push_back({NativeId::GC_STATS, "GC.stats", {}, ZlType::MAP, "zl.lang", {}, NativeReturnTypeRule::NONE, ZlType::UNKNOWN, "map<string,int>"});

    return sigs;
}

} // namespace

const std::vector<NativeSignature>& nativeSignatureTable() {
    static const std::vector<NativeSignature> table = buildSignatureTable();
    return table;
}

std::optional<const NativeSignature*>
findNativeSignature(const std::string& qualifiedName) {
    const auto& table = nativeSignatureTable();
    for (const auto& sig : table) {
        if (sig.qualifiedName == qualifiedName) return &sig;
    }
    return std::nullopt;
}


bool nativeIsReflective(NativeId id) noexcept {
    // The reflection family is contiguous in `NativeId` - every enumerator
    // between REFLECTION_NAME and REFLECTION_CONSTRUCTOR_INVOKE reads,
    // writes, or invokes the program's function table through a name or a
    // descriptor value. New reflection natives join the family in place;
    // anything outside it that starts inspecting the table (a plugin
    // registry, say) extends both ends here, in the catalog, where the list
    // of what each native does lives.
    return id >= NativeId::REFLECTION_NAME && id <= NativeId::REFLECTION_CONSTRUCTOR_INVOKE;
}

std::unordered_map<std::string, std::string> nativeTypeBindings(const NativeSignature& signature,
                                                               const std::string& firstArgumentType) {
    std::unordered_map<std::string, std::string> bindings;
    if (signature.returnTypeRule != NativeReturnTypeRule::FIRST_ARGUMENT_TYPES) return bindings;
    const auto declared = parseTypeName(signature.parameterTypeNames.front());
    const auto actual = parseTypeName(firstArgumentType);
    for (std::size_t i = 0; i < declared.args.size(); ++i)
        bindings.emplace(declared.args[i].name, i < actual.args.size() ? describeTypeName(actual.args[i]) : "unknown");
    return bindings;
}

} // namespace zl
