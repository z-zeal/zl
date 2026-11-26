#include "zl/native/gc_bridge.hpp"

#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"

#include <new>
#include <utility>

namespace zl::native {

struct NativeGcSession {
    GCSafepointCoordinator::ParticipantId participant{0};
    void* userData{nullptr};
    NativeFrameAddressProvider provider{nullptr};
    NativeRuntimeStringProvider stringProvider{nullptr};
    std::vector<NativeFrameRecord> frames;
    bool failed{false};
    NativeGcSession* parent{nullptr};
    bool ownsParticipant{false};
};

namespace {
thread_local NativeGcSession* activeSession = nullptr;

::zl::GCRoots rootsForActiveFrames() {
    ::zl::GCRoots allRoots;
    for (NativeGcSession* session = activeSession; session != nullptr; session = session->parent) {
        if (session->provider == nullptr) continue;
        const auto addresses = session->provider(session->userData, session->frames);
        auto values = ::zl::TracingGC::instance().rootsForNativeAddresses(addresses);
        allRoots.values.insert(allRoots.values.end(), values.begin(), values.end());
    }
    return allRoots;
}

extern "C" void nativeSafepointEnter(void* framePointer, std::uint64_t functionIndex,
                                     std::uint64_t callOffset) noexcept {
    NativeGcSession* session = activeSession;
    if (session == nullptr) return; // Raw executable-buffer tests have no driver session.
    try {
        session->frames.push_back(NativeFrameRecord{
            reinterpret_cast<std::uintptr_t>(framePointer),
            static_cast<std::size_t>(functionIndex), static_cast<std::size_t>(callOffset)});
        ::zl::GCSafepointCoordinator::instance().poll(session->participant, [] {
            return rootsForActiveFrames();
        });
    } catch (...) {
        session->failed = true;
    }
}

extern "C" void nativeSafepointLeave() noexcept {
    NativeGcSession* session = activeSession;
    if (session == nullptr) return;
    if (session->frames.empty()) {
        session->failed = true;
        return;
    }
    session->frames.pop_back();
}
} // namespace

NativeGcSession* beginNativeGcSession(void* userData, NativeFrameAddressProvider provider,
                                      NativeRuntimeStringProvider stringProvider) {
    auto* session = new (std::nothrow) NativeGcSession();
    if (session == nullptr) return nullptr;
    session->userData = userData;
    session->provider = provider;
    session->stringProvider = stringProvider;
    session->parent = activeSession;
    try {
        if (session->parent != nullptr) {
            session->participant = session->parent->participant;
        } else {
            session->participant = ::zl::GCSafepointCoordinator::instance().registerParticipant();
            session->ownsParticipant = true;
        }
    } catch (...) {
        delete session;
        return nullptr;
    }
    activeSession = session;
    return session;
}

bool nativeGcSessionFailed(const NativeGcSession* session) noexcept {
    return session != nullptr && session->failed;
}

void endNativeGcSession(NativeGcSession* session) noexcept {
    if (session == nullptr) return;
    // Sessions are strictly nested on a thread. Refuse to corrupt the active
    // stack if a caller violates that lifetime contract.
    if (activeSession != session) {
        if (activeSession != nullptr) activeSession->failed = true;
        return;
    }
    activeSession = session->parent;
    if (session->failed && session->parent) session->parent->failed = true;
    if (session->ownsParticipant)
        ::zl::GCSafepointCoordinator::instance().unregisterParticipant(session->participant);
    delete session;
}

namespace detail {
std::uintptr_t safepointEnterAddress() noexcept {
    return reinterpret_cast<std::uintptr_t>(&nativeSafepointEnter);
}
std::uintptr_t safepointLeaveAddress() noexcept {
    return reinterpret_cast<std::uintptr_t>(&nativeSafepointLeave);
}
} // namespace detail

namespace {
const char* activeRuntimeString(std::uint64_t index) noexcept {
    if (activeSession == nullptr || activeSession->stringProvider == nullptr) return nullptr;
    return activeSession->stringProvider(activeSession->userData, index);
}

void failActiveRuntimeHelper() noexcept {
    if (activeSession != nullptr) activeSession->failed = true;
}

bool objectForAddress(void* address, ::zl::ObjectRef& object) {
    if (address == nullptr) return false;
    const auto roots = ::zl::TracingGC::instance().rootsForNativeAddresses({address});
    if (roots.size() != 1) return false;
    const auto* found = std::get_if<::zl::ObjectRef>(&roots.front());
    if (found == nullptr) return false;
    object = *found;
    return true;
}

bool objectValueFromAddress(void* address, ::zl::Value& value) {
    if (address == nullptr) {
        value = std::monostate{};
        return true;
    }
    const auto roots = ::zl::TracingGC::instance().rootsForNativeAddresses({address});
    if (roots.size() != 1 || std::get_if<::zl::ObjectRef>(&roots.front()) == nullptr) return false;
    value = roots.front();
    return true;
}

void* objectAddress(const ::zl::Value* value, bool& valid) {
    if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
        valid = true;
        return nullptr;
    }
    const auto* object = std::get_if<::zl::ObjectRef>(value);
    valid = object != nullptr;
    return object == nullptr ? nullptr : object->get();
}
} // namespace

extern "C" void* zl_native_alloc_object(std::uint64_t stringIndex) noexcept {
    try {
        const char* className = activeRuntimeString(stringIndex);
        if (className == nullptr || activeSession == nullptr) {
            failActiveRuntimeHelper();
            return nullptr;
        }
        ::zl::ObjectRef object = ::zl::makeGCObject();
        object->className = className;
        object->genericTypeName = className;
        return object.get();
    } catch (...) {
        failActiveRuntimeHelper();
        return nullptr;
    }
}

extern "C" std::int64_t zl_native_field_load_i(void* address,
                                                 std::uint64_t stringIndex) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        if (fieldName == nullptr || !objectForAddress(address, object)) {
            failActiveRuntimeHelper();
            return 0;
        }
        const ::zl::Value* value = ::zl::objectFieldLookup(*object, fieldName);
        if (value == nullptr) return 0;
        if (const auto* integer = std::get_if<std::int64_t>(value)) return *integer;
        if (const auto* boolean = std::get_if<bool>(value)) return *boolean ? 1 : 0;
        failActiveRuntimeHelper();
        return 0;
    } catch (...) {
        failActiveRuntimeHelper();
        return 0;
    }
}

extern "C" double zl_native_field_load_f(void* address,
                                           std::uint64_t stringIndex) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        if (fieldName == nullptr || !objectForAddress(address, object)) {
            failActiveRuntimeHelper();
            return 0.0;
        }
        const ::zl::Value* value = ::zl::objectFieldLookup(*object, fieldName);
        if (value == nullptr) return 0.0;
        if (const auto* number = std::get_if<double>(value)) return *number;
        failActiveRuntimeHelper();
        return 0.0;
    } catch (...) {
        failActiveRuntimeHelper();
        return 0.0;
    }
}

extern "C" void* zl_native_field_load_r(void* address,
                                           std::uint64_t stringIndex) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        if (fieldName == nullptr || !objectForAddress(address, object)) {
            failActiveRuntimeHelper();
            return nullptr;
        }
        bool valid = false;
        void* value = objectAddress(::zl::objectFieldLookup(*object, fieldName), valid);
        if (!valid) failActiveRuntimeHelper();
        return value;
    } catch (...) {
        failActiveRuntimeHelper();
        return nullptr;
    }
}

extern "C" void zl_native_field_store_i(void* address, std::uint64_t stringIndex,
                                          std::uint64_t isBoolean,
                                          std::int64_t value) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        if (fieldName == nullptr || !objectForAddress(address, object)) {
            failActiveRuntimeHelper();
            return;
        }
        ::zl::objectFieldAccess(*object, fieldName) = isBoolean != 0
            ? ::zl::Value(value != 0) : ::zl::Value(value);
    } catch (...) {
        failActiveRuntimeHelper();
    }
}

extern "C" void zl_native_field_store_f(void* address, std::uint64_t stringIndex,
                                          double value) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        if (fieldName == nullptr || !objectForAddress(address, object)) {
            failActiveRuntimeHelper();
            return;
        }
        ::zl::objectFieldAccess(*object, fieldName) = ::zl::Value(value);
    } catch (...) {
        failActiveRuntimeHelper();
    }
}

extern "C" void zl_native_field_store_r(void* address, std::uint64_t stringIndex,
                                          void* valueAddress) noexcept {
    try {
        const char* fieldName = activeRuntimeString(stringIndex);
        ::zl::ObjectRef object;
        ::zl::Value value;
        if (fieldName == nullptr || !objectForAddress(address, object) ||
            !objectValueFromAddress(valueAddress, value)) {
            failActiveRuntimeHelper();
            return;
        }
        ::zl::objectFieldAccess(*object, fieldName) = std::move(value);
    } catch (...) {
        failActiveRuntimeHelper();
    }
}

} // namespace zl::native
