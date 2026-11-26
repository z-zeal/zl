#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zl::native {

struct NativeFrameRecord {
    std::uintptr_t framePointer{0};
    std::size_t functionIndex{0};
    std::size_t callOffset{0};
};

// Runtime-side adapter asks the emitter/driver for raw root addresses without
// making the native backend depend on VM Value or GC types.
using NativeFrameAddressProvider = std::vector<const void*> (*)(
    void* userData, const std::vector<NativeFrameRecord>& frames);
using NativeRuntimeStringProvider = const char* (*)(void* userData,
                                                    std::uint64_t stringIndex) noexcept;
using NativeDispatchResolver = std::uintptr_t (*)(void* userData,
                                                   const char* runtimeClass,
                                                   std::uint64_t dispatchId) noexcept;

struct NativeGcSession;

[[nodiscard]] NativeGcSession* beginNativeGcSession(
    void* userData, NativeFrameAddressProvider provider,
    NativeRuntimeStringProvider stringProvider,
    NativeDispatchResolver dispatchResolver = nullptr);
[[nodiscard]] bool nativeGcSessionFailed(const NativeGcSession* session) noexcept;
void endNativeGcSession(NativeGcSession* session) noexcept;

namespace detail {
[[nodiscard]] std::uintptr_t safepointEnterAddress() noexcept;
[[nodiscard]] std::uintptr_t safepointLeaveAddress() noexcept;
}

} // namespace zl::native
