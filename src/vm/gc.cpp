#include "zl/vm/gc.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/compiler/bytecode.hpp"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <type_traits>
#include <algorithm>
#include <chrono>
#include <limits>

namespace zl {
namespace {

// ---------------------------------------------------------------------------
// Phase 0 box recycling: a retired box is returned to the free list only in a
// default state, so a pooled box keeps nothing alive - a box that held a list
// of live values must not pin those values (transitively) until it is reused,
// and one that held a ChannelState or a Thread-joined mutex must have that
// state destroyed now, exactly as `retired_.clear()` used to.
// The mutexes inside a box (MapBox::mutex, ObjectBox::stateInitMutex) are not
// reset: a retired box has no concurrent users by definition, and a live mutex
// is the correct starting state for the next incarnation.
// ---------------------------------------------------------------------------
void resetListBox(ListBox& box) noexcept {
    box.storageType.reset();
    box.items.clear();
    box.frontIndex = 0;
}

void resetMapBox(MapBox& box) noexcept {
    box.storageType.reset();
    box.entries.clear();
    box.stringKeyIndexValid = false;
    box.stringKeyIndex.clear();
}

void resetObjectBox(ObjectBox& box) noexcept {
    box.genericTypeName.clear();
    box.mutexState.reset();
    box.rwLockState.reset();
    box.atomicState.reset();
    box.atomicBoolState.reset();
    box.atomicDoubleState.reset();
    box.atomicRefMutex.reset();
    box.sharedValueMutex.reset();
    box.atomicRefState.reset();
    box.semaphoreState.reset();
    box.conditionState.reset();
    box.channelState.reset();
    box.className.clear();
    box.runtimeType.reset();
    box.fields.clear();
    box.extraFields.clear();
}

void resetClosureBox(ClosureBox& box) noexcept {
    box.functionName.clear();
    box.paramNames.clear();
    box.parameterTypeNames.clear();
    box.returnTypeName.clear();
    box.isAsync = false;
    box.isNative = false;
    box.entryAddress = 0;
    box.functionIndex = 0;
    box.chunk.reset();
    box.captured.clear();
    box.typeBindings.clear();
}

} // namespace
namespace {

void appendValueChildren(const Value& value, std::vector<Value>& work, std::vector<const Chunk*>& programs) {
    std::visit([&](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef>) {
            if (!held) return;
            for (const auto& item : held->items) work.push_back(item);
        } else if constexpr (std::is_same_v<T, MapRef>) {
            if (!held) return;
            for (const auto& entry : held->entries) {
                work.push_back(entry.first);
                work.push_back(entry.second);
            }
        } else if constexpr (std::is_same_v<T, ObjectRef>) {
            if (!held) return;
            objectFieldForEach(*held, [&work](const std::string&, const Value& field) { work.push_back(field); });
            if (held->atomicRefState) work.push_back(held->atomicRefState->value);
            if (held->channelState) {
                for (const auto& item : held->channelState->items) work.push_back(item);
                for (const auto& pending : held->channelState->pendingSends) {
                    work.push_back(pending.value);
                    if (pending.task) work.emplace_back(pending.task);
                }
                for (const auto& pending : held->channelState->pendingReceives)
                    if (pending.task) work.emplace_back(pending.task);
            }
        } else if constexpr (std::is_same_v<T, ClosureRef>) {
            if (!held) return;
            for (const auto& capture : held->captured) work.push_back(capture.second);
            if (held->chunk) programs.push_back(held->chunk.get());
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            if (!held) return;
            held->appendGCRoots(work);
        }
    }, value);
}

const void* identity(const Value& value) {
    return std::visit([](const auto& held) -> const void* {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef> ||
                      std::is_same_v<T, MapRef> ||
                      std::is_same_v<T, ObjectRef> ||
                      std::is_same_v<T, ClosureRef> ||
                      std::is_same_v<T, TaskRef>) {
            return held.get();
        } else {
            return nullptr;
        }
    }, value);
}

class Marker {
public:
    std::unordered_set<const void*> marked;

    void trace(const GCRoots& roots) {
        auto work = roots.values;
        auto programs = roots.programs;
        while (!work.empty() || !programs.empty()) {
            if (!work.empty()) {
                Value value = std::move(work.back());
                work.pop_back();
                const auto* id = identity(value);
                if (id && marked.insert(id).second) appendValueChildren(value, work, programs);
                continue;
            }
            const auto* program = programs.back();
            programs.pop_back();
            if (!program || !programs_.insert(program).second) continue;
            work.insert(work.end(), program->constants.begin(), program->constants.end());
            const auto& storage = program->staticStorage;
            if (!storage || !statics_.insert(storage.get()).second) continue;
            // Every managed writer is stopped. Do not acquire initialization
            // locks: a parked native waiter may already have reacquired one
            // and be waiting for this trace to finish before it can resume.
            for (const auto& entry : storage->fields) {
                if (!entry.second) continue;
                work.push_back(entry.second->value);
                entry.second->failure.appendGCRoots(work);
            }
        }
    }
private:
    std::unordered_set<const Chunk*> programs_;
    std::unordered_set<const StaticRuntimeStorage*> statics_;
};

} // namespace

void appendExceptionRoots(const std::exception_ptr& error, std::vector<Value>& roots) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const ZlThrownException& exception) {
        if (exception.value()) roots.emplace_back(exception.value());
    } catch (...) {
        // Native exception objects do not contain managed ZL payloads.
    }
}

// Every runtime allocation goes through one of these four, which is why each
// of them can consult the free list: a recycled box is byte-for-byte the same
// allocation as a fresh one, already in a default state, so callers never see
// the difference except in Counters::reused.
ListRef makeGCList() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    // Pre-reserve the pool so reclaim()'s nothrow handoff cannot reallocate;
    // a no-op once the capacity exists (see kFreeListCap).
    gc.freeLists_.reserve(TracingGC::kFreeListCap);
    std::unique_ptr<ListBox> box;
    if (!gc.freeLists_.empty()) {
        box = std::move(gc.freeLists_.back());
        gc.freeLists_.pop_back();
        ++gc.totalReused_;
    } else {
        box = std::make_unique<ListBox>();
    }
    ListRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::List, std::move(box), {}, {}, {}});
    ++gc.allocationsSinceCollection_;
    ++gc.totalAllocations_;
    return ref;
}

MapRef makeGCMap() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    gc.freeMaps_.reserve(TracingGC::kFreeListCap);
    std::unique_ptr<MapBox> box;
    if (!gc.freeMaps_.empty()) {
        box = std::move(gc.freeMaps_.back());
        gc.freeMaps_.pop_back();
        ++gc.totalReused_;
    } else {
        box = std::make_unique<MapBox>();
    }
    MapRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Map, {}, std::move(box), {}, {}});
    ++gc.allocationsSinceCollection_;
    ++gc.totalAllocations_;
    return ref;
}

ObjectRef makeGCObject() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    gc.freeObjects_.reserve(TracingGC::kFreeListCap);
    std::unique_ptr<ObjectBox> box;
    if (!gc.freeObjects_.empty()) {
        box = std::move(gc.freeObjects_.back());
        gc.freeObjects_.pop_back();
        ++gc.totalReused_;
    } else {
        box = std::make_unique<ObjectBox>();
    }
    ObjectRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Object, {}, {}, std::move(box), {}});
    ++gc.allocationsSinceCollection_;
    ++gc.totalAllocations_;
    return ref;
}

ClosureRef makeGCClosure() {
    auto& gc = TracingGC::instance();
    std::lock_guard<std::mutex> lock(gc.mutex_);
    gc.freeClosures_.reserve(TracingGC::kFreeListCap);
    std::unique_ptr<ClosureBox> box;
    if (!gc.freeClosures_.empty()) {
        box = std::move(gc.freeClosures_.back());
        gc.freeClosures_.pop_back();
        ++gc.totalReused_;
    } else {
        box = std::make_unique<ClosureBox>();
    }
    ClosureRef ref(box.get());
    gc.entries_.push_back(TracingGC::Entry{TracingGC::Entry::Kind::Closure, {}, {}, {}, std::move(box)});
    ++gc.allocationsSinceCollection_;
    ++gc.totalAllocations_;
    return ref;
}

TracingGC& TracingGC::instance() {
    static TracingGC gc;
    return gc;
}

// These registration methods remain as a compatibility boundary for embedders.
// Managed runtime allocation must use makeGC* so the collector retains physical
// ownership. A handle already owned by this collector is simply observed here.
void TracingGC::track(const ListRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::List && entry.list.get() == value.get()) return;
}

void TracingGC::track(const MapRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Map && entry.map.get() == value.get()) return;
}

void TracingGC::track(const ObjectRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Object && entry.object.get() == value.get()) return;
}

void TracingGC::track(const ClosureRef& value) {
    if (!value) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) if (entry.kind == Entry::Kind::Closure && entry.closure.get() == value.get()) return;
}

void TracingGC::track(const Value& value) {
    std::visit([this](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, ListRef>) track(held);
        else if constexpr (std::is_same_v<T, MapRef>) track(held);
        else if constexpr (std::is_same_v<T, ObjectRef>) track(held);
        else if constexpr (std::is_same_v<T, ClosureRef>) track(held);
        else if constexpr (std::is_same_v<T, NativeBufferView> || std::is_same_v<T, NativeStructView> || std::is_same_v<T, NativeHandleRef> || std::is_same_v<T, NativeCallbackRef>) { /* external resources are not GC objects */ }
    }, value);
}

std::vector<Value> TracingGC::rootsForNativeAddresses(
    const std::vector<const void*>& addresses) const {
    std::vector<Value> roots;
    if (addresses.empty()) return roots;

    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<const void*, Entry::Kind> liveByPointer;
    liveByPointer.reserve(entries_.size());
    for (const auto& entry : entries_) {
        const void* pointer = nullptr;
        switch (entry.kind) {
            case Entry::Kind::List: pointer = entry.list.get(); break;
            case Entry::Kind::Map: pointer = entry.map.get(); break;
            case Entry::Kind::Object: pointer = entry.object.get(); break;
            case Entry::Kind::Closure: pointer = entry.closure.get(); break;
        }
        liveByPointer.emplace(pointer, entry.kind);
    }

    roots.reserve(addresses.size());
    for (const void* address : addresses) {
        const auto found = liveByPointer.find(address);
        if (found == liveByPointer.end()) continue;
        switch (found->second) {
            case Entry::Kind::List:
                roots.emplace_back(ListRef(static_cast<ListBox*>(const_cast<void*>(address))));
                break;
            case Entry::Kind::Map:
                roots.emplace_back(MapRef(static_cast<MapBox*>(const_cast<void*>(address))));
                break;
            case Entry::Kind::Object:
                roots.emplace_back(ObjectRef(static_cast<ObjectBox*>(const_cast<void*>(address))));
                break;
            case Entry::Kind::Closure:
                roots.emplace_back(ClosureRef(static_cast<ClosureBox*>(const_cast<void*>(address))));
                break;
        }
    }
    return roots;
}

TracingGC::Collection TracingGC::collect(const GCRoots& roots) {
    // The coordinator must stop all mutators before entering this method.
    // Serializing collectors alone does not make tracing concurrent-safe.
    std::lock_guard<std::mutex> collectLock(collectMutex_);
    // Phase 0 measures "% of run time in collect" as this span: trace +
    // protected-root resolution + the sweep, not lock waits.
    const auto started = std::chrono::steady_clock::now();

    std::unordered_set<const void*> protectedIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : protectedRoots_) protectedIds.insert(entry.first);
    }

    Marker marker;
    marker.trace(roots);

    // Convert protected identities into temporary non-owning Value roots while
    // the registry is stable. Marking itself happens outside the registry lock
    // to avoid lock-order inversions with runtime state such as Tasks.
    std::vector<Value> protectedValues;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // One pass over the registry, not one pass per protected id: build a
        // pointer -> kind index so protected identities resolve in O(1) each
        // (the old nested loop was O(P*N) under the registry lock).
        std::unordered_map<const void*, Entry::Kind> liveByPointer;
        liveByPointer.reserve(entries_.size());
        for (const auto& entry : entries_) {
            const void* id = nullptr;
            switch (entry.kind) {
                case Entry::Kind::List: id = entry.list.get(); break;
                case Entry::Kind::Map: id = entry.map.get(); break;
                case Entry::Kind::Object: id = entry.object.get(); break;
                case Entry::Kind::Closure: id = entry.closure.get(); break;
            }
            liveByPointer.emplace(id, entry.kind);
        }
        for (const void* protectedId : protectedIds) {
            const auto found = liveByPointer.find(protectedId);
            if (found == liveByPointer.end()) continue;
            switch (found->second) {
                case Entry::Kind::List:
                    protectedValues.emplace_back(ListRef(static_cast<ListBox*>(const_cast<void*>(protectedId))));
                    break;
                case Entry::Kind::Map:
                    protectedValues.emplace_back(MapRef(static_cast<MapBox*>(const_cast<void*>(protectedId))));
                    break;
                case Entry::Kind::Object:
                    protectedValues.emplace_back(ObjectRef(static_cast<ObjectBox*>(const_cast<void*>(protectedId))));
                    break;
                case Entry::Kind::Closure:
                    protectedValues.emplace_back(ClosureRef(static_cast<ClosureBox*>(const_cast<void*>(protectedId))));
                    break;
            }
        }
    }
    marker.trace(GCRoots{std::move(protectedValues)});

    std::lock_guard<std::mutex> lock(mutex_);
    Collection result;
    auto& stats = result.stats;
    stats.tracked = entries_.size();

    std::vector<Entry> survivors;
    survivors.reserve(entries_.size());
    result.retired_.reserve(entries_.size());
    // Complete allocations before mutating the registry. On failure the
    // coordinator can safely resume without a partially detached heap.
    static_assert(std::is_nothrow_move_constructible<Entry>::value, "GC retirement must not throw");
    for (auto& entry : entries_) {
        const void* id = nullptr;
        switch (entry.kind) {
            case Entry::Kind::List: id = entry.list.get(); break;
            case Entry::Kind::Map: id = entry.map.get(); break;
            case Entry::Kind::Object: id = entry.object.get(); break;
            case Entry::Kind::Closure: id = entry.closure.get(); break;
        }
        if (marker.marked.count(id)) {
            ++stats.reachable;
            survivors.push_back(std::move(entry));
            continue;
        }

        ++stats.unreachable;
        result.retired_.push_back(std::move(entry));
        ++stats.reclaimed;
    }
    entries_.swap(survivors);
    stats.tracked = entries_.size();
    stats.deferred = 0;

    // Adapt the next collection threshold to the live heap instead of using
    // only a fixed allocation count. A small floor avoids overly frequent
    // collections for tiny heaps, while allowing stable larger heaps to grow
    // proportionally between collections. This is the Phase 0 growth policy:
    // the trigger is 2x the live heap after every collection (floor 128), so
    // a stable heap collects roughly once per heap-size of new allocations
    // instead of once per 128 regardless of size.
    constexpr std::size_t kMinimumAllocationThreshold = 128;
    const std::size_t liveHeapBudget = entries_.size() > (std::numeric_limits<std::size_t>::max() / 2)
        ? std::numeric_limits<std::size_t>::max()
        : entries_.size() * 2;
    allocationThreshold_ = std::max(kMinimumAllocationThreshold, liveHeapBudget);
    allocationsSinceCollection_ = 0;
    ++totalCollections_;
    totalCollectNs_ += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    if (entries_.size() > peakTracked_) peakTracked_ = entries_.size();
    return result;
}

TracingGC::Counters TracingGC::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Counters c;
    c.allocations = totalAllocations_;
    c.reused = totalReused_;
    c.collections = totalCollections_;
    c.collectNs = totalCollectNs_;
    c.tracked = entries_.size();
    c.peakTracked = peakTracked_;
    c.threshold = allocationThreshold_;
    return c;
}

void TracingGC::Collection::reclaim() noexcept {
    // Retired boxes are unreachable - no root, live or parked, names them -
    // and retired_ is owned by this Collection alone, so neither the walk
    // nor the reset needs the registry lock. The reset MUST run without that
    // lock: a payload destructor can join a thread (a ThreadRef, whose
    // deleter re-enters the runtime) or allocate, and both would deadlock
    // against a held mutex_ - this is the same reason reclamation is
    // deferred out of the trace in the first place. Resetting also guarantees
    // a pooled box keeps nothing alive and that the overflow's destruction
    // below happens in a known state.
    for (auto& entry : retired_) {
        switch (entry.kind) {
            case TracingGC::Entry::Kind::List: resetListBox(*entry.list); break;
            case TracingGC::Entry::Kind::Map: resetMapBox(*entry.map); break;
            case TracingGC::Entry::Kind::Object: resetObjectBox(*entry.object); break;
            case TracingGC::Entry::Kind::Closure: resetClosureBox(*entry.closure); break;
        }
    }
    // Hand the reset boxes to the free list under the lock so makeGC* never
    // sees a half-pooled box. The pools are pre-reserved to their cap in
    // TracingGC's constructor, so these push_backs cannot reallocate and
    // this noexcept path cannot throw. The cap keeps a churn burst from
    // reserving an unbounded pool; overflow is destroyed after the loop,
    // still with no lock held.
    auto& gc = TracingGC::instance();
    {
        std::lock_guard<std::mutex> lock(gc.mutex_);
        for (auto& entry : retired_) {
            switch (entry.kind) {
                case TracingGC::Entry::Kind::List:
                    if (gc.freeLists_.size() < TracingGC::kFreeListCap)
                        gc.freeLists_.push_back(std::move(entry.list));
                    break;
                case TracingGC::Entry::Kind::Map:
                    if (gc.freeMaps_.size() < TracingGC::kFreeListCap)
                        gc.freeMaps_.push_back(std::move(entry.map));
                    break;
                case TracingGC::Entry::Kind::Object:
                    if (gc.freeObjects_.size() < TracingGC::kFreeListCap)
                        gc.freeObjects_.push_back(std::move(entry.object));
                    break;
                case TracingGC::Entry::Kind::Closure:
                    if (gc.freeClosures_.size() < TracingGC::kFreeListCap)
                        gc.freeClosures_.push_back(std::move(entry.closure));
                    break;
            }
        }
    }
    retired_.clear();
}

void TracingGC::protect(const void* identity) {
    if (!identity) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++protectedRoots_[identity];
}

void TracingGC::unprotect(const void* identity) {
    if (!identity) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = protectedRoots_.find(identity);
    if (it != protectedRoots_.end() && --it->second == 0) protectedRoots_.erase(it);
}

bool TracingGC::shouldCollect() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocationsSinceCollection_ >= allocationThreshold_;
}

std::size_t TracingGC::trackedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void appendGCRoots(const std::vector<Value>& values, std::vector<Value>& roots) {
    roots.insert(roots.end(), values.begin(), values.end());
}

} // namespace zl
