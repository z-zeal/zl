#pragma once

#include <cstdint>

// Fixture header for tools/zl-bind/test_zl_bind.sh.
//
// Exercises the restricted binding surface: a top-level borrowed function and
// a unique-ownership native class with a constructor, destructor, const and
// mutating methods, and a method that throws.
//
// Annotations use the `// zl:` comment protocol understood by the parser. The
// class needs no annotation: the parser's default (borrowed) maps a class to
// unique ownership with exception error translation.

// zl: ownership=borrowed
// zl: errors=exception
extern "C" int zl_add(int a, int b);

class Counter {
public:
    Counter(int64_t start);
    ~Counter();

    int64_t value() const;
    void increment(int64_t by);
    int64_t fail() const;

private:
    // Data members are skipped by the binding generator (methods only).
    int64_t value_;
};

// Plain-data C struct: `zl-bind` binds this one field by field against a
// typed schema - zero-initialized storage, one get/set pair per field, and
// an offsetof/sizeof table the target compiler validates at compile time.
struct DemoConfig {
    bool flag;
    int32_t count;
    double ratio;
    uint16_t mode;
};
