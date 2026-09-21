#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zl/native/emit.hpp"
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
// and offers typed entry for the calling shapes it can honestly speak today -
// all-integer or all-double signatures, SysV. Everything else is refused
// *before* any code runs, by name and with a reason:
//
//   * a call relocation that is not bound (a runtime import, or a callee the
//     selection left out) - binding would point at nothing;
//   * a function whose signature mixes register files, or whose result is not
//     a scalar the casts cover - a double arrives in an XMM register, and a
//     reference carries GC obligations the driver does not model;
//   * a host this code does not target: execution is x86-64 Linux (the same
//     guard the executable-native tests use); every other build constructs a
//     NativeExecutable that reports `unsupported platform` rather than
//     pretending.
//
// That refusal list is the current subset boundary made concrete. Growing it -
// refs and objects with GC maps - is P1-6's remaining half; the driver is the
// half that makes the bytes reachable at all.

namespace zl::native {

// The driver speaks two calling shapes, chosen by the signature it finds:
// every-Integer in and out through `callInt64`, every-Float in and out
// through `callDouble`. A signature that mixes the two is refused with a
// reason - one C++ cast can only describe one register-file shape, and
// hand-written thunks are not this file's business.

class NativeExecutable {
public:
    // Takes the emitted functions plus their LIR signatures (parameter
    // classes); binds inter-function calls; never binds runtime calls.
    NativeExecutable(const std::vector<EmittedFunction>& functions, const LirModule& lir);
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

    // The entry's call shape: everything Integer (kInt64), everything Float
    // with a Float result (kDouble), or neither (kUnsupported, with the
    // reason). Entries are what `resolve` returns.
    enum class Shape { kInt64, kDouble, kUnsupported };
    Shape shape(std::size_t e, std::string& reason) const;

    // Call entry e with integer arguments (up to six, SysV integer ABI).
    // Requires shape() == kInt64; false with a reason otherwise.
    bool callInt64(std::size_t e, const std::vector<std::int64_t>& args,
                   std::int64_t& result, std::string& error) const;

    // Call entry e with double arguments (up to six, SysV XMM ABI).
    // Requires shape() == kDouble; false with a reason otherwise.
    bool callDouble(std::size_t e, const std::vector<double>& args,
                    double& result, std::string& error) const;

private:
    bool ok_{false};
    std::string error_;
    void* base_{nullptr};
    std::size_t size_{0};
    std::vector<std::string> names_;
    std::vector<std::uintptr_t> entries_;
    std::vector<std::vector<ValueClass>> parameterClasses_;
    std::vector<ValueClass> returnClasses_;
};

} // namespace zl::native
