#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "zl/native/emit.hpp"
#include "zl/native/gc_bridge.hpp"
#include "zl/native/lir.hpp"

// ---------------------------------------------------------------------------
// Native execution driver
// ---------------------------------------------------------------------------
//
// The last piece of "native" that was missing is not code generation - the
// emitter has produced correct bytes since the arithmetic-contract work - but
// something to put those bytes in memory a CPU will execute, bind their call
// sites, and hand back a result. This file is that driver: it maps an emitted
// module executable, resolves direct calls among the module's own functions,
// binds explicit runtime helpers, and offers typed entry for all-integer,
// all-double, and borrowed all-reference signatures under SysV. Unsupported
// shapes are refused *before* any code runs, by name and with a reason:
//
//   * a call relocation that is not bound (a runtime import, or a callee the
//     selection left out) - binding would point at nothing;
//   * a function whose signature mixes register files, or whose result is not
//     a scalar the typed entrypoints cover - a double arrives in an XMM
//     register; reference entry is supported only through the borrowed-pointer
//     API, not from command-line arguments;
//   * a host this code does not target: execution is x86-64 Linux (the same
//     guard the executable-native tests use); every other build constructs a
//     NativeExecutable that reports `unsupported platform` rather than
//     pretending.
//
// The refusal list is the current subset boundary made concrete. Reference
// carry, safepoint root publication, non-generic data records, and simple
// non-inherited classes with trivial constructors support allocation/fields;
// nontrivial class initialization and dispatch remain open P1-6 steps.

namespace zl::native {

// The driver speaks all-Integer and all-Float shapes through `callInt64` and
// `callDouble`, plus a borrowed all-reference shape through `callReference`.
// Mixed register-file signatures are refused with a reason; each C++ cast
// describes one register-file shape, and general marshalling is out of scope.

using NativeRuntimeBindings = std::unordered_map<std::string, std::uintptr_t>;

class NativeExecutable {
public:
    // Takes emitted functions plus LIR signatures, binds direct calls and
    // the internal safepoint bridge. Runtime calls bind only when supplied in
    // runtimeBindings; unknown symbols still fail closed.
    NativeExecutable(const std::vector<EmittedFunction>& functions, const LirModule& lir,
                     const NativeRuntimeBindings& runtimeBindings = {});
    ~NativeExecutable();
    NativeExecutable(const NativeExecutable&) = delete;
    NativeExecutable& operator=(const NativeExecutable&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Emitted names, in module order.
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }

    // Resolve a call target by name. Accepts the exact LIR function name, or
    // a bare method token (the part after the last '.' and before '('), when
    // that identifies exactly one function. On ambiguity or absence, fills
    // `error` and returns kNoEntry.
    static constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);
    std::size_t resolve(const std::string& spec, std::string& error) const;

    // The entry's call shape: homogeneous Integer, Float, or Reference values;
    // other combinations are kUnsupported with a reason. Entries are what
    // `resolve` returns.
    enum class Shape { kInt64, kDouble, kReference, kUnsupported };
    Shape shape(std::size_t e, std::string& reason) const;

    // Call entry e with integer arguments (up to six, SysV integer ABI).
    // Requires shape() == kInt64; false with a reason otherwise.
    bool callInt64(std::size_t e, const std::vector<std::int64_t>& args,
                   std::int64_t& result, std::string& error) const;

    // Call entry e with double arguments (up to six, SysV XMM ABI).
    // Requires shape() == kDouble; false with a reason otherwise.
    bool callDouble(std::size_t e, const std::vector<double>& args,
                    double& result, std::string& error) const;

    // Typed reference entry used by the native runtime bridge and tests.
    // References are borrowed raw managed addresses; the caller must keep
    // them rooted until the generated prologue has spilled them.
    bool callReference(std::size_t e, const std::vector<std::uintptr_t>& args,
                       std::uintptr_t& result, std::string& error) const;

private:
    struct FrameMetadata {
        std::vector<EmittedFrameLocation> locations;
        std::vector<SafepointMap> safepoints;
    };
    static std::vector<const void*> nativeFrameAddresses(
        void* userData, const std::vector<NativeFrameRecord>& frames);
    static const char* nativeRuntimeString(void* userData,
                                           std::uint64_t stringIndex) noexcept;
    bool ok_{false};
    std::string error_;
    void* base_{nullptr};
    std::size_t size_{0};
    std::vector<std::string> names_;
    std::vector<std::uintptr_t> entries_;
    std::vector<std::vector<ValueClass>> parameterClasses_;
    std::vector<ValueClass> returnClasses_;
    std::vector<FrameMetadata> frameMetadata_;
    std::vector<std::string> runtimeStrings_;
};

} // namespace zl::native
