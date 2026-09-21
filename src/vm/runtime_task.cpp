#include "zl/vm/runtime_task.hpp"
#include "zl/vm/gc_roots.hpp"

#include <stdexcept>
#include <iostream>
#include <string>

namespace zl {

namespace {
bool isTerminalStatus(TaskStatus status) {
    return status == TaskStatus::Succeeded ||
           status == TaskStatus::Failed ||
           status == TaskStatus::Cancelled;
}
}


RuntimeTaskState::~RuntimeTaskState() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ != TaskStatus::Failed || failureObserved_ || !error_) return;
        std::cerr << "unobserved task failure: " << error_.message() << "\n";
    } catch (...) {
        // Reporting must never turn task destruction into a new runtime failure.
    }
}

TaskStatus RuntimeTaskState::status() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool RuntimeTaskState::cancellationRequested() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancellationRequested_;
}

bool RuntimeTaskState::failureObserved() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return failureObserved_;
}

std::string RuntimeTaskState::valueTypeName() const {
    // Signature metadata is immutable; type validation must not wait for task
    // completion/destruction while holding a container contract transaction.
    return valueTypeName_;
}

void RuntimeTaskState::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != TaskStatus::Pending) {
        throw std::logic_error("task can only start from Pending state");
    }
    status_ = TaskStatus::Running;
}

void RuntimeTaskState::succeed(Value value) {
    std::vector<Value> releasedRoots;
    std::vector<std::function<void()>> continuations;
    std::vector<std::function<void()>> cancelledObservers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        result_ = std::move(value);
        status_ = TaskStatus::Succeeded;
        continuations.swap(continuations_);
        cancelledObservers.swap(cancellationContinuations_);
        releasedRoots.swap(operationRoots_);
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::fail(std::exception_ptr error) {
    if (!error) {
        throw std::invalid_argument("failed task requires an exception");
    }
    StoredException captured(error);
    std::vector<Value> releasedRoots;
    std::vector<std::function<void()>> continuations;
    std::vector<std::function<void()>> cancelledObservers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        error_ = std::move(captured);
        status_ = TaskStatus::Failed;
        continuations.swap(continuations_);
        cancelledObservers.swap(cancellationContinuations_);
        releasedRoots.swap(operationRoots_);
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::cancel() {
    std::vector<Value> releasedRoots;
    std::vector<std::function<void()>> continuations;
    std::vector<std::function<void()>> cancellationContinuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) {
            throw std::logic_error("task is already terminal");
        }
        status_ = TaskStatus::Cancelled;
        cancellationRequested_ = true;
        continuations.swap(continuations_);
        cancellationContinuations.swap(cancellationContinuations_);
        releasedRoots.swap(operationRoots_);
    }
    condition_.notify_all();
    std::exception_ptr firstError;
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    for (auto& continuation : cancellationContinuations) {
        try {
            continuation();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    // A terminal cancellation still cascades: children the task spawned
    // before it settled were never given the request otherwise.
    propagateCancellationToChildren();
    if (firstError) std::rethrow_exception(firstError);
}

void RuntimeTaskState::requestCancellation() noexcept {
    std::vector<std::function<void()>> continuations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_) || cancellationRequested_) return;
        cancellationRequested_ = true;
        continuations.swap(cancellationContinuations_);
    }
    condition_.notify_all();
    for (auto& continuation : continuations) {
        try {
            continuation();
        } catch (...) {
            // Cancellation requests are noexcept by contract. One failing
            // observer must not prevent remaining observers from running.
        }
    }
    // Propagated after the observers so an awaiting parent's wake-up does not
    // run behind a child cascade that itself wants to wake the same parent.
    propagateCancellationToChildren();
}

void RuntimeTaskState::ignore() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    failureObserved_ = true;
}

void RuntimeTaskState::addSpawnedChild(const std::shared_ptr<RuntimeTaskState>& child) {
    if (!child || child.get() == this) return;
    bool cancelNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A parent whose cancellation is already in flight (requested or
        // terminal Cancelled) hands the request straight to the child: the
        // parent may have raced through one more spawn before its next
        // suspension point noticed.
        cancelNow = cancellationRequested_ || status_ == TaskStatus::Cancelled;
        spawnedChildren_.push_back(child);
    }
    if (cancelNow) child->requestCancellation();
}

void RuntimeTaskState::propagateCancellationToChildren() {
    std::vector<std::shared_ptr<RuntimeTaskState>> children;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // No dedup and no pruning: each entry is at most one lock away from
        // "terminal, skip", and the list is bounded by the task's own spawns.
        for (auto& weak : spawnedChildren_)
            if (auto child = weak.lock()) children.push_back(std::move(child));
    }
    for (auto& child : children) {
        if (!child->isTerminal()) child->requestCancellation();
    }
}

void RuntimeTaskState::appendGCRoots(std::vector<Value>& roots) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_) roots.push_back(*result_);
    roots.insert(roots.end(), operationRoots_.begin(), operationRoots_.end());
    error_.appendGCRoots(roots);
}


void RuntimeTaskState::onCancellation(std::function<void()> continuation) {
    if (!continuation) return;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == TaskStatus::Cancelled || cancellationRequested_) runNow = true;
        else if (!isTerminalStatus(status_)) cancellationContinuations_.push_back(std::move(continuation));
    }
    if (runNow) continuation();
}
void RuntimeTaskState::then(std::function<void()> continuation) {
    if (!continuation) return;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isTerminalStatus(status_)) runNow = true;
        else continuations_.push_back(std::move(continuation));
    }
    if (runNow) continuation();
}

bool RuntimeTaskState::isTerminal() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return isTerminalStatus(status_);
}

Value RuntimeTaskState::observe() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return isTerminalStatus(status_); });

    switch (status_) {
        case TaskStatus::Succeeded:
            return *result_;
        case TaskStatus::Failed:
            failureObserved_ = true;
            error_.rethrow();
        case TaskStatus::Cancelled: {
            failureObserved_ = true;
            // No chunk is reachable from task state, so the box carries no
            // runtime type and the message lands in extraFields; every reader
            // (StoredException, the catch binding, main) resolves it there.
            Value object = makeEmptyObject("CancellationException");
            auto ex = std::get<ObjectRef>(object);
            objectFieldAccess(*ex, "message") = std::string("task was cancelled");
            throw ZlThrownException(std::move(ex));
        }
        case TaskStatus::Pending:
        case TaskStatus::Running:
            break;
    }

    throw std::logic_error("invalid task state");
}

} // namespace zl
