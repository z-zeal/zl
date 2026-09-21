#pragma once

#include <string>
#include <vector>

namespace zl_bind {

struct Param { std::string type; std::string name; };
struct Fn {
    std::string ret, name;
    std::vector<Param> params;
    std::string ownership = "borrowed";
    std::string errors = "exception";
};
struct Method {
    std::string ret, name;
    std::vector<Param> params;
    bool isConst{false};
    bool isDestructor{false};
};

// A data member of a plain C struct: the typed schema is a field list with
// exactly this shape (C type + member name); every field type must map to a
// supported scalar.
struct Field { std::string type; std::string name; };

// A plain-data `struct` (fields only: no constructor, destructor or
// methods). `zl-bind` emits a typed field-by-field binding for these:
// alloc/free plus a get/set pair per field, with a C++ layout schema table
// (name, offset, size) guarded by compile-time asserts.
struct NativeStruct {
    std::string name;
    std::vector<Field> fields;
};
struct NativeClass {
    std::string name;
    std::string kind;
    std::vector<Param> constructorParams;
    bool hasConstructor{false};
    bool hasDestructor{false};
    std::string ownership = "unique";
    std::string errors = "exception";
    std::vector<Method> methods;
    // Data members seen while parsing; consumed only when the declaration
    // turns out to be a plain data struct (no constructor, no methods).
    std::vector<Field> fields;
};

struct ParsedHeader {
    std::vector<Fn> functions;
    std::vector<NativeClass> classes;
    std::vector<NativeStruct> structs;
};

ParsedHeader parseHeaderFile(const std::string& path);
std::string mapNativeType(const std::string& type);

} // namespace zl_bind
