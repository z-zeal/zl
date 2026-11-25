#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include <unordered_map>

#include "gc_roots.hpp"

namespace zl {

// Single allocation gateway for GC-managed runtime boxes. Every runtime
// allocation of a ListBox, MapBox, ObjectBox, or ClosureBox should enter
// through one of these helpers so registration cannot be accidentally omitted.
[[nodiscard]] ListRef makeGCList();
[[nodiscard]] MapRef makeGCMap();
[[nodiscard]] ObjectRef makeGCObject();
[[nodiscard]] ClosureRef makeGCClosure();


// Tracing GC boundary for ZL's hybrid memory model.
//
// Runtime GC allocation is centralized through the makeGC* gateway helpers.
// The collector holds one strong registry reference for each managed box,
// discovers roots explicitly, marks the managed graph, and performs the
// current tracing sweep. Physical storage is collector-owned and released from
// the registry when an allocation is unreachable.
class TracingGC {
    struct Entry {
        enum class Kind : std::uint8_t { List, Map, Object, Closure };
        Kind kind;
        std::unique_ptr<ListBox> list;
        std::unique_ptr<MapBox> map;
        std::unique_ptr<ObjectBox> object;
        std::unique_ptr<ClosureBox> closure;
    };

public:
    struct Stats {
        std::size_t tracked{0};
        std::size_t reachable{0};
        std::size_t unreachable{0};
        std::size_t reclaimed{0};
        std::size_t deferred{0};
    };

    // Process-wide lifetime counters, independent of any single collection.
    // `allocations` counts every box that entered the registry (fresh or
    // recycled); `reused` counts the subset that came from the free list, so
    // fresh = allocations - reused. These are the Phase 0 benchmark inputs:
    // throughput per allocation and the free list's hit rate.
    struct Counters {
        std::size_t allocations{0};
        std::size_t reused{0};
        std::size_t collections{0};
        std::uint64_t collectNs{0};
        std::size_t tracked{0};
        std::size_t peakTracked{0};
        std::size_t threshold{0};
    };
    [[nodiscard]] Counters counters() const;

    // Trace/sweep only detaches garbage. Keep this result until mutators have
    // resumed, then reclaim: C++ ownership inside an unreachable box can join
    // threads, unregister VMs, or destroy a program's static storage.
    //
    // reclaim() runs after the trace but with peers runnable (the collector
    // drops the coordinator lock first), which is why destruction is deferred
    // out of the trace at all: a destructor may join a thread or allocate.
    // The recycled-box path returns storage to the free list here instead of
    // destroying it; the free list has its own lock, and a retired box is
    // unreachable by definition, so pooling is safe with mutators running.
    class Collection {
    public:
        Collection() = default;
        Collection(Collection&&) noexcept = default;
        Collection& operator=(Collection&&) noexcept = default;
        Collection(const Collection&) = delete;
        Collection& operator=(const Collection&) = delete;
        Stats stats;
        void reclaim() noexcept;
    private:
        friend class TracingGC;
        std::vector<Entry> retired_;
    };

    TracingGC() = default;
    TracingGC(const TracingGC&) = delete;
    TracingGC& operator=(const TracingGC&) = delete;

    static TracingGC& instance();

    void track(const ListRef& value);
    void track(const MapRef& value);
    void track(const ObjectRef& value);
    void track(const ClosureRef& value);
    void track(const Value& value);

    [[nodiscard]] Collection collect(const GCRoots& roots);
    // Convert raw native-frame addresses into typed, non-owning GC roots.
    // Unknown/null values are ignored; the registry lock protects the lookup.
    [[nodiscard]] std::vector<Value> rootsForNativeAddresses(
        const std::vector<const void*>& addresses) const;
    [[nodiscard]] std::size_t trackedCount() const;
    [[nodiscard]] bool shouldCollect() const;

    // Protect an externally-managed runtime object (such as a raw Thread
    // closure) from reclamation while code outside ExecutionState is using it.
    // Protections are counted: several workers may retain the same closure.
    void protect(const void* identity);
    void unprotect(const void* identity);

private:
    friend ListRef makeGCList();
    friend MapRef makeGCMap();
    friend ObjectRef makeGCObject();
    friend ClosureRef makeGCClosure();

    // Phase 0 box recycling: retired boxes are reset to a default state and
    // handed back to a per-kind free list instead of destroyed, so a churn
    // workload reuses storage instead of paying a fresh allocation each time.
    // The cap bounds the pool: a benchmark that allocates a million boxes and
    // drops them must not keep a million boxes resident "for later". A pool
    // that exceeds the cap destroys the overflow; the hit-rate counter in
    // Counters::reused is how the benchmark sees the trade.
    static constexpr std::size_t kFreeListCap = 8192;

    mutable std::mutex mutex_;
    mutable std::mutex collectMutex_;
    std::vector<Entry> entries_;
    std::vector<std::unique_ptr<ListBox>> freeLists_;
    std::vector<std::unique_ptr<MapBox>> freeMaps_;
    std::vector<std::unique_ptr<ObjectBox>> freeObjects_;
    std::vector<std::unique_ptr<ClosureBox>> freeClosures_;
    std::unordered_map<const void*, std::size_t> protectedRoots_;
    std::size_t allocationsSinceCollection_{0};
    std::size_t allocationThreshold_{128};
    // Lifetime counters (see Counters). Guarded by mutex_.
    std::size_t totalAllocations_{0};
    std::size_t totalReused_{0};
    std::size_t totalCollections_{0};
    std::uint64_t totalCollectNs_{0};
    std::size_t peakTracked_{0};
};

// Retain a root across a queued/running native callback, including enqueue or
// thread-start failure. Share this token when the callback must be copyable.
class ProtectedGCRoot {
public:
    explicit ProtectedGCRoot(const void* identity) : identity_(identity) {
        TracingGC::instance().protect(identity_);
    }
    ~ProtectedGCRoot() { TracingGC::instance().unprotect(identity_); }
    ProtectedGCRoot(const ProtectedGCRoot&) = delete;
    ProtectedGCRoot& operator=(const ProtectedGCRoot&) = delete;
private:
    const void* identity_;
};

// Explicit root collection for a VM execution state. The VM calls this at
// safe points; callers must provide additional roots owned outside the
// ExecutionState (for example suspended async invocation arguments/results).
void appendGCRoots(const std::vector<Value>& values, std::vector<Value>& roots);

} // namespace zl
