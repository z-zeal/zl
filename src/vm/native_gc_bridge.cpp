#include "zl/native/gc_bridge.hpp"

#include "zl/vm/gc.hpp"
#include "zl/vm/gc_safepoint.hpp"

#include <new>

namespace zl::native {

struct NativeGcSession {
    GCSafepointCoordinator::ParticipantId participant{0};
    void* userData{nullptr};
    NativeFrameAddressProvider provider{nullptr};
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

NativeGcSession* beginNativeGcSession(void* userData, NativeFrameAddressProvider provider) {
    auto* session = new (std::nothrow) NativeGcSession();
    if (session == nullptr) return nullptr;
    session->userData = userData;
    session->provider = provider;
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

} // namespace zl::native
