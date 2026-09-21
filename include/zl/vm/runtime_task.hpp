#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <vector>
#include <string>

#include "value.hpp"
#include "runtime_exception.hpp"

namespace zl {

// Runtime representation shared by the future language-level Task<T> abstraction.
// It deliberately owns only lifecycle/result state; scheduling policy lives in Scheduler.
enum class TaskStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

class RuntimeTaskState {
public:
    explicit RuntimeTaskState(std::string valueTypeName = {}, std::vector<Value> operationRoots = {})
        : valueTypeName_(std::move(valueTypeName)), operationRoots_(std::move(operationRoots)) {}
    ~RuntimeTaskState() noexcept;
    RuntimeTaskState(const RuntimeTaskState&) = delete;
    RuntimeTaskState& operator=(const RuntimeTaskState&) = delete;

    [[nodiscard]] TaskStatus status() const noexcept;
    [[nodiscard]] bool cancellationRequested() const noexcept;
    [[nodiscard]] bool failureObserved() const noexcept;
    [[nodiscard]] std::string valueTypeName() const;

    // State transitions are idempotent only for cancellation requests. Terminal
    // completion/failure/cancellation are single-owner transitions and reject misuse.
    void start();
    void succeed(Value value);
    void fail(std::exception_ptr error);
    void cancel();
    void requestCancellation() noexcept;

    // Observing a task marks a failure as observed. Calling this before
    // completion waits until the task reaches a terminal state.
    Value observe();
    [[nodiscard]] bool isTerminal() const noexcept;
    void ignore() noexcept;
    void then(std::function<void()> continuation);
    void onCancellation(std::function<void()> continuation);

    // Spawned-task relation. A task created while another task's body was
    // running records that parent, and a cancellation request or terminal
    // cancellation cascades parent-first to every still-live child (which
    // cascades to its own children). A child added to an already-cancelled
    // parent is cancelled immediately, so a parent never races past its
    // cancellation into a fresh spawn.
    void addSpawnedChild(const std::shared_ptr<RuntimeTaskState>& child);

    // Append managed values retained by the task itself (currently its result
    // and the value carried by a ZL exception) to an active GC mark worklist.
    void appendGCRoots(std::vector<Value>& roots) const;

private:
    // Cascade a cancellation to every live child (lock must NOT be held).
    void propagateCancellationToChildren();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    TaskStatus status_{TaskStatus::Pending};
    std::optional<Value> result_;
    StoredException error_;
    bool cancellationRequested_{false};
    bool failureObserved_{false};
    const std::string valueTypeName_;
    std::vector<Value> operationRoots_;
    std::vector<std::function<void()>> continuations_;
    std::vector<std::function<void()>> cancellationContinuations_;
    // Weak by design: a task must not keep its children alive (a dropped
    // child still reports its own unobserved failure at teardown), and a
    // dead weak entry costs one lock when the parent is cancelled.
    std::vector<std::weak_ptr<RuntimeTaskState>> spawnedChildren_;
};

// The task whose body is currently executing on this thread, if any. Set for
// the duration of an async invocation's execution step and of a Task.spawn
// worker body, so a task created inside either records the parent it was
// spawned from. A raw pointer: the owning VM or executor lambda keeps the
// state alive across the guarded region.
inline RuntimeTaskState* gCurrentSpawningTask = nullptr;

struct CurrentSpawningTaskGuard {
    RuntimeTaskState* previous;
    explicit CurrentSpawningTaskGuard(RuntimeTaskState* task) noexcept
        : previous(gCurrentSpawningTask) {
        gCurrentSpawningTask = task;
    }
    ~CurrentSpawningTaskGuard() { gCurrentSpawningTask = previous; }
    CurrentSpawningTaskGuard(const CurrentSpawningTaskGuard&) = delete;
    CurrentSpawningTaskGuard& operator=(const CurrentSpawningTaskGuard&) = delete;
};

} // namespace zl
