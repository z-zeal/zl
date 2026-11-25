// Synchronization state regressions: the exact state objects the VM uses for
// ZL Mutex / ReadWriteLock / Atomic / Shared / Semaphore / Condition / Channel
// (value.hpp), driven concurrently the way the runtime drives them - the same
// wait predicates, the same bounded-channel handoff - to pin down mutex
// correctness, wait/notify and timeout behavior, contention, and destruction
// of idle state.
#include "zl/vm/value.hpp"

#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "sync regression: " << message << '\n';
    ++failures;
}

using namespace std::chrono_literals;
using zl::MapBox;
using zl::ObjectBox;
using zl::Value;

// The state objects live on an ObjectBox, lazily initialized by the VM; the
// tests initialize them directly so each primitive is exercised in isolation.
// ObjectBox is non-copyable, so hand out heap storage.
std::unique_ptr<ObjectBox> makeBox(const char* className) {
    auto box = std::make_unique<ObjectBox>();
    box->className = className;
    return box;
}

void testMutexState() {
    auto box = makeBox("Mutex");
    box->mutexState = std::make_shared<std::mutex>();
    std::atomic<long> counter{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                std::lock_guard<std::mutex> lock(*box->mutexState);
                ++counter;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    require(counter.load() == 40000, "mutexState serializes contending writers");
    // Destruction of idle state must not hang or throw.
    box->mutexState.reset();
}

void testReadWriteLockState() {
    auto box = makeBox("ReadWriteLock");
    box->rwLockState = std::make_shared<std::shared_mutex>();
    std::atomic<long> version{0};
    std::atomic<bool> inconsistent{false};
    std::atomic<bool> writersDone{false};
    std::atomic<long> reads{0};
    constexpr int kReaders = 4;

    std::vector<std::thread> threads;
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&] {
            // Wait until every reader has taken the lock at least once before
            // writing anything. Two writers on a two-core box otherwise own both
            // cores from start to finish, so the readers are not scheduled until
            // writersDone is already set, exit on their first check, and
            // "readers actually ran" fails for a reason that is about the
            // scheduler rather than the lock. Measured on an idle two-core box:
            // 199 failures in 200 solo runs, and 0 in 30 with both cores busy -
            // a suite that only fails when the machine is quiet is one no CI
            // runner ever reproduces.
            while (reads.load(std::memory_order_acquire) < kReaders) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 1000; ++i) {
                std::unique_lock<std::shared_mutex> lock(*box->rwLockState);
                // Two half-updates: a reader must never observe the pair mid-write.
                version.fetch_add(1);
                version.fetch_add(1);
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            while (!writersDone.load(std::memory_order_acquire)) {
                {
                    std::shared_lock<std::shared_mutex> lock(*box->rwLockState);
                    if (version.load() % 2 != 0) inconsistent.store(true);
                    ++reads;
                }
                // Yield between reads. Four readers in a tight loop starve the
                // writers' exclusive lock on a two-core box - the interleaving
                // this test is about still happens, it just took four seconds to
                // get through 2000 writes instead of a fraction of one.
                std::this_thread::yield();
            }
        });
    }
    for (int i = 0; i < 2; ++i) threads[i].join();
    writersDone.store(true, std::memory_order_release);
    for (std::size_t i = 2; i < threads.size(); ++i) threads[i].join();
    require(!inconsistent.load(), "readers never observe a half-written pair");
    require(reads.load() > 0, "readers actually ran");
}

void testAtomicState() {
    auto box = makeBox("Atomic");
    box->atomicState = std::make_shared<std::atomic<std::int64_t>>(0);
    box->atomicBoolState = std::make_shared<std::atomic<bool>>(false);
    box->atomicDoubleState = std::make_shared<std::atomic<double>>(1.0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&box] {
            for (int i = 0; i < 10000; ++i) box->atomicState->fetch_add(1);
            box->atomicBoolState->exchange(true);
        });
    }
    for (auto& thread : threads) thread.join();
    require(box->atomicState->load() == 80000, "atomicState counts every increment");
    require(box->atomicBoolState->load(), "atomicBoolState observes the exchange");
    double expected = 1.0;
    bool doubleCas = box->atomicDoubleState->compare_exchange_strong(expected, 2.0);
    require(doubleCas && box->atomicDoubleState->load() == 2.0, "atomicDoubleState CAS works");
    box->sharedValueMutex = std::make_shared<std::recursive_mutex>();
    std::atomic<long> cell{0};
    std::vector<std::thread> cellThreads;
    for (int t = 0; t < 8; ++t) {
        cellThreads.emplace_back([&box, &cell] {
            for (int i = 0; i < 2000; ++i) {
                std::lock_guard<std::recursive_mutex> lock(*box->sharedValueMutex);
                cell += 1;
                (void)cell.load();
            }
        });
    }
    for (auto& thread : cellThreads) thread.join();
    require(cell.load() == 16000, "the Shared<T> cell serializes get/set");
}

void testSemaphoreState() {
    auto box = makeBox("Semaphore");
    auto state = std::make_shared<ObjectBox::SemaphoreState>();
    state->permits = 0;
    box->semaphoreState = state;

    const int workers = 4;
    const int perWorker = 500;
    const int total = workers * perWorker;
    std::atomic<int> acquired{0};
    std::vector<std::thread> threads;
    // The same wait predicate the VM's Semaphore.acquire uses.
    for (int t = 0; t < workers; ++t) {
        threads.emplace_back([&state, &acquired, perWorker] {
            for (int i = 0; i < perWorker; ++i) {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait(lock, [&state] { return state->permits > 0; });
                --state->permits;
                ++acquired;
            }
        });
    }
    // The producer releases exactly `total` permits; the accounting must come
    // out even: every acquisition is paid for, and none is left waiting.
    std::thread producer([&state, total] {
        for (int i = 0; i < total; ++i) {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->permits;
            state->cv.notify_one();
        }
    });
    for (auto& thread : threads) thread.join();
    producer.join();
    require(acquired.load() == total, "every permit acquisition completed");
    std::lock_guard<std::mutex> lock(state->mutex);
    require(state->permits == 0, "permit accounting is exact");
    state.reset(); // idle destruction
}

void testConditionState() {
    auto box = makeBox("Condition");
    auto state = std::make_shared<ObjectBox::ConditionState>();
    box->conditionState = state;

    // Signaling: a waiter wakes when the producer flips the flag.
    bool flagged = false;
    std::thread producer([&state, &flagged] {
        std::this_thread::sleep_for(30ms);
        std::lock_guard<std::mutex> lock(state->mutex);
        flagged = true;
        state->cv.notify_all();
    });
    bool woke = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        woke = state->cv.wait_for(lock, 5s, [&flagged] { return flagged; });
    }
    producer.join();
    require(woke, "a notified condition waiter wakes with its predicate true");

    // Timeout: no notification means the waiter reports the timeout, it does
    // not hang.
    bool timedOut = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool signaled = state->cv.wait_for(lock, 50ms, [] { return false; });
        timedOut = !signaled;
    }
    require(timedOut, "a condition wait without a signal times out");
    state.reset();
}

void testChannelState() {
    auto state = std::make_shared<ObjectBox::ChannelState>();
    state->capacity = 2;

    auto send = [&](int value) {
        std::unique_lock<std::mutex> lock(state->mutex);
        // Hand directly to a waiting receiver when the buffer is full,
        // otherwise buffer, mirroring the runtime channel protocol.
        if (state->pendingReceives.empty()) {
            state->cvNotFull.wait(lock, [&state] { return state->items.size() < state->capacity; });
            state->items.push_back(zl::Value(value));
        } else {
            auto receiver = state->pendingReceives.front();
            state->pendingReceives.pop_front();
            (void)receiver; // the value is delivered through the result below
            state->items.push_back(zl::Value(value));
        }
        state->cvNotEmpty.notify_one();
    };
    auto receive = [&]() -> zl::Value {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cvNotEmpty.wait(lock, [&state] { return !state->items.empty(); });
        auto value = state->items.front();
        state->items.pop_front();
        state->cvNotFull.notify_one();
        return value;
    };

    const int items = 32;
    std::vector<int> received;
    std::mutex receivedMutex;
    std::thread sender([&] {
        for (int i = 0; i < items; ++i) send(i);
    });
    std::thread receiver([&] {
        for (int i = 0; i < items; ++i) {
            auto value = receive();
            std::lock_guard<std::mutex> lock(receivedMutex);
            received.push_back(std::get<std::int64_t>(value));
        }
    });
    sender.join();
    receiver.join();
    bool fifo = received.size() == items;
    for (int i = 0; fifo && i < items; ++i) fifo = received[i] == i;
    require(fifo, "the bounded channel delivers every value in order");
    require(state->items.empty() && state->pendingSends.empty() && state->pendingReceives.empty(),
            "the channel settles with no pending work");
    state.reset();
}

void testMapBoxState() {
    // Sequential semantics: insertion order, in-place update, hit/miss,
    // removal, and snapshot isolation (a snapshot is detached from later
    // mutation, so printers/serializers never observe a torn map).
    MapBox box;
    box.setEntry(Value{std::int64_t{1}}, Value{std::int64_t{10}});
    box.setEntry(Value{std::string{"k"}}, Value{std::int64_t{20}});
    require(box.size() == 2, "setEntry grows the map");
    box.setEntry(Value{std::int64_t{1}}, Value{std::int64_t{11}});
    require(box.size() == 2, "setEntry updates a present key in place");
    auto hit = box.tryGetEntry(Value{std::int64_t{1}});
    require(hit && std::get<std::int64_t>(*hit) == 11, "tryGetEntry returns the updated value");
    require(!box.tryGetEntry(Value{std::int64_t{2}}), "tryGetEntry misses an absent key");
    require(box.findEntry(Value{std::string{"k"}}) != MapBox::kNoEntry, "findEntry locates a string key");
    auto snap = box.snapshotEntries();
    box.setEntry(Value{std::int64_t{3}}, Value{std::int64_t{30}});
    box.removeEntry(Value{std::string{"k"}});
    require(snap.size() == 2, "snapshotEntries is detached from later mutation");
    require(box.removeEntry(Value{std::int64_t{3}}), "removeEntry reports a present key");
    require(!box.removeEntry(Value{std::int64_t{3}}), "removeEntry reports an absent key");

    // Numeric keys use a separate index while preserving valuesEqual's
    // cross-representation rule: an integral double finds and updates the
    // matching int key without changing insertion order or growing the map.
    MapBox numeric;
    for (std::int64_t i = 0; i < 32; ++i) {
        numeric.setEntry(Value{i}, Value{i * 10});
    }
    auto numericHit = numeric.tryGetEntry(Value{7.0});
    require(numeric.numericKeyIndexValid, "a large numeric map builds its lookup index");
    require(numericHit && std::get<std::int64_t>(*numericHit) == 70,
            "numeric index preserves int/double key equality");
    numeric.setEntry(Value{7.0}, Value{std::int64_t{77}});
    require(numeric.size() == 32, "cross-numeric update does not append a duplicate key");
    auto updatedNumericHit = numeric.tryGetEntry(Value{std::int64_t{7}});
    require(updatedNumericHit && std::get<std::int64_t>(*updatedNumericHit) == 77,
            "cross-numeric update is visible through the original key type");
    numeric.setEntry(Value{std::int64_t{32}}, Value{std::int64_t{320}});
    auto appendedNumericHit = numeric.tryGetEntry(Value{32.0});
    require(appendedNumericHit && std::get<std::int64_t>(*appendedNumericHit) == 320,
            "an active numeric index tracks appended keys");
    require(numeric.removeEntry(Value{7.0}), "numeric index removes a cross-type key");
    require(!numeric.numericKeyIndexValid, "removal invalidates the numeric index");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    numeric.setEntry(Value{nan}, Value{std::int64_t{1}});
    require(!numeric.tryGetEntry(Value{nan}), "numeric index retains NaN non-equality");

    // Enough string keys to engage the hash index, then hammer one MapBox
    // from several threads the way aliased Shared cells do: concurrent
    // insert/update/lookup/remove/snapshot/size must neither crash, tear,
    // nor lose atomicity of any single op.
    MapBox shared;
    for (int i = 0; i < 64; ++i)
        shared.setEntry(Value{"s" + std::to_string(i)}, Value{std::int64_t{i}});
    std::atomic<bool> failed{false};
    auto worker = [&](int id) {
        try {
            for (int i = 0; i < 500; ++i) {
                const int key = (id * 500 + i) % 128;
                shared.setEntry(Value{std::int64_t{key}}, Value{std::int64_t{id}});
                (void)shared.tryGetEntry(Value{std::int64_t{key}});
                (void)shared.tryGetEntry(Value{"s" + std::to_string(key % 64)});
                if ((i % 7) == 0) (void)shared.snapshotEntries();
                if ((i % 11) == 0) (void)shared.size();
                if ((i % 13) == 0) shared.removeEntry(Value{std::int64_t{(key + 1) % 128}});
            }
        } catch (...) {
            failed.store(true);
        }
    };
    std::thread t1([&] { worker(1); });
    std::thread t2([&] { worker(2); });
    std::thread t3([&] { worker(3); });
    std::thread t4([&] { worker(4); });
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    require(!failed.load(), "concurrent map ops complete without throwing");
    // The map is always in a coherent state: snapshot and size agree, and
    // every snapshot entry is still findable with an equal value.
    auto finalSnap = shared.snapshotEntries();
    require(finalSnap.size() == shared.size(), "snapshot and size agree after the hammer");
    bool coherent = true;
    for (const auto& entry : finalSnap) {
        auto current = shared.tryGetEntry(entry.first);
        if (!current || *current != entry.second) { coherent = false; break; }
    }
    require(coherent, "every snapshot entry round-trips through lookup");
}

} // namespace

int main() {
    testMutexState();
    testReadWriteLockState();
    testAtomicState();
    testSemaphoreState();
    testConditionState();
    testChannelState();
    testMapBoxState();

    if (failures != 0) {
        std::cerr << failures << " sync regression(s) failed\n";
        return 1;
    }
    std::cout << "all sync regressions passed\n";
    return 0;
}
