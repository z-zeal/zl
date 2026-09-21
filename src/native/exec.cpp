#include "zl/native/exec.hpp"

#include <cstring>

#if defined(__unix__) && !defined(__APPLE__) && defined(__x86_64__)
#include <sys/mman.h>
#define ZL_NATIVE_EXEC_POSIX_MMAP 1
#endif

namespace zl::native {

namespace {

// The one calling shape the driver speaks: up to six int64 parameters, an
// int64 result, SysV. A cast to a concrete function type is honest here
// because selection has already guaranteed every other shape is refused before
// the cast is ever taken.
using EntryFn = std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                std::int64_t, std::int64_t, std::int64_t);

// The all-double counterpart: six XMM argument registers, result in xmm0.
using DoubleFn = double (*)(double, double, double, double, double, double);

std::string describeClass(ValueClass cls) {
    switch (cls) {
        case ValueClass::Integer: return "int";
        case ValueClass::Float: return "double";
        case ValueClass::Void: return "void";
        default: return "ref";
    }
}

std::size_t alignUp(std::size_t v) { return (v + 4095) / 4096 * 4096; }

} // namespace

NativeExecutable::NativeExecutable(const std::vector<EmittedFunction>& functions, const LirModule& lir) {
#if !defined(ZL_NATIVE_EXEC_POSIX_MMAP)
    (void)functions;
    (void)lir;
    error_ = "native execution driver: unsupported platform (needs x86-64 Linux with mmap)";
    return;
#else
    if (functions.empty()) {
        error_ = "native execution driver: the module emitted no functions";
        return;
    }

    // LIR signatures first, so a bad shape is reported before any mapping.
    // Every emitted function must have a same-named LIR function; the pairing
    // is by name, not position, because `code` is per-function output.
    for (const auto& fn : functions) {
        const LirFunction* sig = nullptr;
        for (const auto& lf : lir.functions)
            if (lf.name == fn.name) { sig = &lf; break; }
        if (sig == nullptr) {
            error_ = "native execution driver: no LIR signature for emitted function '" + fn.name + "'";
            return;
        }
        names_.push_back(fn.name);
        parameterClasses_.push_back(sig->parameterClasses);
        returnClasses_.push_back(fn.returnClass);
    }

    // One page-sized run per function keeps every entry point page-aligned and
    // every inter-function rel32 reachable through the arena (same layout the
    // native backend tests use, for the same reason).
    std::size_t total = 0;
    std::vector<std::size_t> offsets;
    for (const auto& fn : functions) {
        offsets.push_back(total);
        total += alignUp(fn.code.size());
    }
    if (total == 0) {
        error_ = "native execution driver: all emitted functions are empty";
        return;
    }
    void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        error_ = "native execution driver: mmap failed";
        return;
    }
    base_ = base;
    size_ = total;
    auto* bytes = static_cast<std::uint8_t*>(base);

    for (std::size_t i = 0; i < functions.size(); ++i) {
        std::memcpy(bytes + offsets[i], functions[i].code.data(), functions[i].code.size());
        entries_.push_back(reinterpret_cast<std::uintptr_t>(bytes + offsets[i]));
    }

    // Bind direct calls; refuse the module if any site cannot be bound.
    auto find = [&](const std::string& symbol) -> std::uintptr_t {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == symbol) return entries_[i];
        return 0;
    };
    std::vector<std::string> unbound;
    for (std::size_t i = 0; i < functions.size(); ++i) {
        for (const auto& reloc : functions[i].relocations) {
            const std::uintptr_t target = reloc.isRuntimeCall ? 0 : find(reloc.symbol);
            if (target == 0) {
                unbound.push_back(functions[i].name + " -> " + (reloc.isRuntimeCall ? "runtime:" : "") + reloc.symbol);
                continue;
            }
            auto* site = bytes + offsets[i] + reloc.offset;
            const std::int32_t delta = static_cast<std::int32_t>(
                target - (reinterpret_cast<std::uintptr_t>(site) + 4));
            std::memcpy(site, &delta, 4);
        }
    }
    if (!unbound.empty()) {
        std::string list;
        for (std::size_t i = 0; i < unbound.size() && i < 6; ++i) list += (i ? "; " : "") + unbound[i];
        if (unbound.size() > 6) list += "; +" + std::to_string(unbound.size() - 6) + " more";
        error_ = "native execution driver: unbound call site(s): " + list;
        return;
    }

    if (::mprotect(base_, size_, PROT_READ | PROT_EXEC) != 0) {
        error_ = "native execution driver: mprotect(PROT_EXEC) failed";
        return;
    }
    ok_ = true;
#endif
}

NativeExecutable::~NativeExecutable() {
#if defined(ZL_NATIVE_EXEC_POSIX_MMAP)
    if (base_ != nullptr) ::munmap(base_, size_);
#endif
}

std::size_t NativeExecutable::resolve(const std::string& spec, std::string& error) const {
    for (std::size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == spec) return i;
    // Bare method token: "sumSquares" matches "NumericKernel.sumSquares(int)".
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < names_.size(); ++i) {
        const std::string& n = names_[i];
        const std::size_t paren = n.find('(');
        const std::size_t dot = n.rfind('.', paren == std::string::npos ? n.size() : paren);
        const std::size_t begin = dot == std::string::npos ? 0 : dot + 1;
        if (paren != std::string::npos && n.compare(begin, paren - begin, spec) == 0)
            matches.push_back(i);
    }
    if (matches.size() == 1) return matches[0];
    if (matches.empty()) {
        error = "native execution driver: no native function matches '" + spec + "'; compiled: ";
        for (std::size_t i = 0; i < names_.size(); ++i) error += (i ? ", " : "") + names_[i];
    } else {
        error = "native execution driver: '" + spec + "' is ambiguous among ";
        for (std::size_t i = 0; i < matches.size(); ++i) error += (i ? ", " : "") + names_[matches[i]];
    }
    return kNoEntry;
}

// The one calling shape each entry can honestly be called through, decided
// from the LIR signature list the constructor recorded. A cast can only
// describe one register file, so a mixed signature is a refusal, not a
// marshalling puzzle; `Void` in the parameter list is impossible (selection
// refuses void parameters), so only the result needs the extra case.
NativeExecutable::Shape NativeExecutable::shape(std::size_t e, std::string& reason) const {
    if (e >= names_.size()) { reason = "bad entry index"; return Shape::kUnsupported; }
    const auto& params = parameterClasses_[e];
    if (params.size() > 6) {
        reason = "native execution driver: '" + names_[e] + "' takes " +
                 std::to_string(params.size()) + " arguments; the SysV shapes the driver casts go to six";
        return Shape::kUnsupported;
    }
    bool all_int = returnClasses_[e] == ValueClass::Integer;
    bool all_float = returnClasses_[e] == ValueClass::Float;
    for (const auto cls : params) {
        if (cls != ValueClass::Integer) all_int = false;
        if (cls != ValueClass::Float) all_float = false;
    }
    if (all_int) return Shape::kInt64;
    if (all_float) return Shape::kDouble;
    std::string sig;
    for (std::size_t i = 0; i < params.size(); ++i) sig += (i ? ", " : "") + describeClass(params[i]);
    reason = "native execution driver: '" + names_[e] + "' has signature (" + sig + ") -> " +
             describeClass(returnClasses_[e]) + "; the driver calls all-integer or all-double "
             "signatures, because one cast describes exactly one register file";
    return Shape::kUnsupported;
}

bool NativeExecutable::callInt64(std::size_t e, const std::vector<std::int64_t>& args,
                                 std::int64_t& result, std::string& error) const {
    if (!ok_) { error = error_; return false; }
#if defined(ZL_NATIVE_EXEC_POSIX_MMAP)
    if (shape(e, error) != Shape::kInt64) {
        if (!error.empty() && error.rfind("native execution driver:", 0) == 0) return false;
        error = "native execution driver: '" + names_[e] +
                "' does not have an all-integer int64 signature; see --run-native listing";
        return false;
    }
    if (args.size() != parameterClasses_[e].size()) {
        error = "native execution driver: '" + names_[e] + "' takes " +
                std::to_string(parameterClasses_[e].size()) + " argument(s), got " + std::to_string(args.size());
        return false;
    }
    std::int64_t a[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < args.size(); ++i) a[i] = args[i];
    result = reinterpret_cast<EntryFn>(entries_[e])(a[0], a[1], a[2], a[3], a[4], a[5]);
    return true;
#else
    (void)e; (void)args; (void)result; (void)error;
    error = error_;
    return false;
#endif
}

bool NativeExecutable::callDouble(std::size_t e, const std::vector<double>& args,
                                  double& result, std::string& error) const {
    if (!ok_) { error = error_; return false; }
#if defined(ZL_NATIVE_EXEC_POSIX_MMAP)
    if (shape(e, error) != Shape::kDouble) {
        if (!error.empty() && error.rfind("native execution driver:", 0) == 0) return false;
        error = "native execution driver: '" + names_[e] + "' is not an all-double signature";
        return false;
    }
    if (args.size() != parameterClasses_[e].size()) {
        error = "native execution driver: '" + names_[e] + "' takes " +
                std::to_string(parameterClasses_[e].size()) + " argument(s), got " + std::to_string(args.size());
        return false;
    }
    // Six XMM argument registers exist; passing padding in unused ones is
    // exactly what the SysV calling convention allows.
    double a[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < args.size(); ++i) a[i] = args[i];
    result = reinterpret_cast<DoubleFn>(entries_[e])(a[0], a[1], a[2], a[3], a[4], a[5]);
    return true;
#else
    (void)e; (void)args; (void)result; (void)error;
    error = error_;
    return false;
#endif
}

} // namespace zl::native
