// Task executor and task-state regressions: executor submission/completion,
// worker failure containment, concurrent submission, task lifecycle
// (success/failure/cancellation and their continuations), failure observation,
// and thread-state failure capture on join.
#include "zl/vm/runtime_exception.hpp"
#include "zl/vm/runtime_task.hpp"
#include "zl/vm/runtime_task_executor.hpp"
#include "zl/vm/runtime_thread.hpp"
#include "zl/vm/value.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "task executor regression: " << message << '\n';
    ++failures;
}

using namespace std::chrono_literals;

// Polls `probe` until it reports true or the deadline passes. Keeps the suite
// fast when the executor is quick and honest about a hang instead of racing
// a fixed sleep.
template <typename Probe>
bool waitFor(Probe&& probe, int seconds = 15) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return probe();
}

void testExecutorCompletion() {
    std::atomic<int> done{0};
    auto& executor = zl::RuntimeTaskExecutor::instance();
    for (int i = 0; i < 256; ++i) executor.enqueue([&done] { ++done; });
    require(waitFor([&done] { return done.load() == 256; }), "every enqueued task runs");
    // Executor reuse: the same singleton serves the next batch.
    std::atomic<int> second{0};
    executor.enqueue([&second] { ++second; });
    require(waitFor([&second] { return second.load() == 1; }), "the executor serves a later batch");
}

void testExecutorFailureContainment() {
    std::atomic<int> after{0};
    auto& executor = zl::RuntimeTaskExecutor::instance();
    executor.enqueue([] { throw std::runtime_error("task exploded"); });
    // A crashing task must not kill its worker: the next task still runs.
    executor.enqueue([&after] { ++after; });
    require(waitFor([&after] { return after.load() == 1; }), "a worker survives a throwing task");
    bool rejectedEmpty = false;
    try {
        executor.enqueue(nullptr);
    } catch (const std::invalid_argument&) {
        rejectedEmpty = true;
    }
    require(rejectedEmpty, "an empty work item is rejected");
}

void testConcurrentSubmission() {
    std::atomic<int> total{0};
    auto& executor = zl::RuntimeTaskExecutor::instance();
    std::vector<std::thread> submitterThreads;
    for (int t = 0; t < 4; ++t) {
        submitterThreads.emplace_back([&executor, &total] {
            for (int i = 0; i < 100; ++i) executor.enqueue([&total] { ++total; });
        });
    }
    for (auto& worker : submitterThreads) worker.join();
    require(waitFor([&total] { return total.load() == 400; }), "all concurrently submitted tasks run");
}

void testTaskLifecycle() {
    auto task = std::make_shared<zl::RuntimeTaskState>("int");
    require(task->status() == zl::TaskStatus::Pending, "a fresh task is pending");

    bool ranOnSuccess = false;
    task->then([&ranOnSuccess] { ranOnSuccess = true; });

    task->start();
    require(task->status() == zl::TaskStatus::Running, "start moves the task to running");
    task->succeed(zl::Value(41));
    require(task->status() == zl::TaskStatus::Succeeded, "success is terminal");
    require(task->observe() == zl::Value(41), "observe returns the result");
    require(ranOnSuccess, "a continuation registered before success runs");

    bool lateContinuation = false;
    task->then([&lateContinuation] { lateContinuation = true; });
    require(lateContinuation, "a continuation after success runs immediately");
}

void testTaskFailureAndObservation() {
    auto task = std::make_shared<zl::RuntimeTaskState>("");
    task->start();
    try {
        throw std::runtime_error("task failed badly");
    } catch (...) {
        task->fail(std::current_exception());
    }
    require(task->status() == zl::TaskStatus::Failed, "failure is terminal");
    bool rethrown = false;
    try {
        (void)task->observe();
    } catch (const std::runtime_error& error) {
        rethrown = true;
        require(std::string(error.what()) == "task failed badly", "observe rethrows the original failure");
    }
    require(rethrown, "observing a failed task rethrows its failure");

    // Misuse is rejected, not silently absorbed.
    bool rejectedSecondTerminal = false;
    try {
        task->succeed(zl::Value(1));
    } catch (const std::logic_error&) {
        rejectedSecondTerminal = true;
    }
    require(rejectedSecondTerminal, "a terminal task cannot be completed again");
    bool rejectedNullFailure = false;
    auto fresh = std::make_shared<zl::RuntimeTaskState>("");
    try {
        fresh->fail(nullptr);
    } catch (const std::invalid_argument&) {
        rejectedNullFailure = true;
    }
    require(rejectedNullFailure, "failing a task requires an exception");
    bool rejectedStart = false;
    try {
        fresh->start();
        fresh->start();
    } catch (const std::logic_error&) {
        rejectedStart = true;
    }
    require(rejectedStart, "a task can only start from pending");
}

void testTaskCancellation() {
    auto task = std::make_shared<zl::RuntimeTaskState>("");
    int observerRuns = 0;
    task->onCancellation([&observerRuns] { ++observerRuns; });
    task->requestCancellation();
    require(task->cancellationRequested(), "cancellation is requested");
    require(observerRuns == 1, "the observer runs at request time (cooperative cancellation)");
    require(task->status() == zl::TaskStatus::Pending, "a request alone is not terminal");
    task->start();
    task->cancel();
    require(task->status() == zl::TaskStatus::Cancelled, "cancel is terminal");
    require(observerRuns == 1, "the observer is not run a second time by the terminal cancel");
    bool threw = false;
    try {
        (void)task->observe();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "observing a cancelled task surfaces the cancellation");
}

// A failed task nobody ever observes must report itself when it dies (the
// P1-7 unobserved-failure gap), and observing it must silence the report.
void testUnobservedFailureReport() {
    auto dropped = std::make_shared<zl::RuntimeTaskState>("int");
    dropped->start();
    dropped->fail(std::make_exception_ptr(std::runtime_error("nobody looked")));
    std::ostringstream capture;
    std::streambuf* previous = std::cerr.rdbuf(capture.rdbuf());
    dropped.reset(); // the destructor reports at the moment the task dies
    std::cerr.rdbuf(previous);
    require(capture.str().find("unobserved task failure") != std::string::npos,
            "a dropped failed task reports itself at teardown");
    require(capture.str().find("nobody looked") != std::string::npos,
            "the report names the failure it is about");

    auto observed = std::make_shared<zl::RuntimeTaskState>("int");
    observed->start();
    observed->fail(std::make_exception_ptr(std::runtime_error("seen")));
    try {
        (void)observed->observe();
    } catch (const std::exception&) {
    }
    capture.str("");
    std::cerr.rdbuf(capture.rdbuf());
    observed.reset();
    std::cerr.rdbuf(previous);
    require(capture.str().empty(), "an observed failure is not reported twice");

    auto ignored = std::make_shared<zl::RuntimeTaskState>("int");
    ignored->start();
    ignored->fail(std::make_exception_ptr(std::runtime_error("detached")));
    ignored->ignore();
    capture.str("");
    std::cerr.rdbuf(capture.rdbuf());
    ignored.reset();
    std::cerr.rdbuf(previous);
    require(capture.str().empty(), "ignore() detaches the failure deliberately");
}

// Cancellation cascades along the spawn edge, and only along it (P1-7):
// a task cancelled (requested or terminal) hands the request to the tasks
// spawned from its body, which hand it to their own children, and a spawn
// that races the cascade is cancelled on arrival.
void testCancellationCascade() {
    auto parent = std::make_shared<zl::RuntimeTaskState>("int");
    auto child = std::make_shared<zl::RuntimeTaskState>("int");
    auto grandchild = std::make_shared<zl::RuntimeTaskState>("int");
    child->start();
    grandchild->start();
    parent->addSpawnedChild(child);
    child->addSpawnedChild(grandchild);
    parent->requestCancellation();
    require(child->cancellationRequested(), "a running child takes its parent's cancellation");
    require(grandchild->cancellationRequested(), "the request cascades to a grandchild");
    require(child->status() == zl::TaskStatus::Running,
            "propagation stays cooperative: a request is not a terminal transition");

    auto late = std::make_shared<zl::RuntimeTaskState>("int");
    parent->addSpawnedChild(late);
    require(late->cancellationRequested(), "a spawn racing the cascade is cancelled on arrival");

    // A terminal cancellation of a parent still reaches live children.
    auto p2 = std::make_shared<zl::RuntimeTaskState>("int");
    auto c2 = std::make_shared<zl::RuntimeTaskState>("int");
    c2->start();
    p2->addSpawnedChild(c2);
    p2->cancel();
    require(c2->cancellationRequested(), "a parent's terminal cancellation still cascades");

    // Siblings and strangers are untouched: the relation is spawn, not scope.
    auto sibling = std::make_shared<zl::RuntimeTaskState>("int");
    auto stranger = std::make_shared<zl::RuntimeTaskState>("int");
    sibling->start();
    stranger->start();
    auto p3 = std::make_shared<zl::RuntimeTaskState>("int");
    p3->addSpawnedChild(sibling);
    p3->requestCancellation();
    require(sibling->cancellationRequested(), "the registered sibling is cancelled");
    require(!stranger->cancellationRequested(), "an unrelated task keeps running");

    // Dropping a parent must not drop a live child (weak, not owned): the
    // child still reports its own failure at its own teardown.
    auto dropParent = std::make_shared<zl::RuntimeTaskState>("int");
    auto dropChild = std::make_shared<zl::RuntimeTaskState>("int");
    dropParent->addSpawnedChild(dropChild);
    dropParent.reset();
    require(dropChild.use_count() == 1, "a parent does not keep its child alive");
}

void testConcurrentObserve() {
    auto task = std::make_shared<zl::RuntimeTaskState>("int");
    std::atomic<int> observers{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&task, &observers] {
            if (task->observe() == zl::Value(7)) ++observers;
        });
    }
    std::this_thread::sleep_for(20ms);
    task->start();
    task->succeed(zl::Value(7));
    for (auto& thread : threads) thread.join();
    require(observers.load() == 8, "every concurrent observer gets the result");
}

void testThreadState() {
    zl::RuntimeThreadState thread;
    std::atomic<int> ran{0};
    thread.startWith([&ran] { ++ran; });
    require(thread.isAlive(), "a started thread is alive");
    thread.join();
    require(ran.load() == 1, "the worker ran its entry");
    require(!thread.isAlive(), "a joined thread is no longer alive");

    bool doubleStartRejected = false;
    try {
        thread.startWith([] {});
    } catch (const std::logic_error&) {
        doubleStartRejected = true;
    }
    require(doubleStartRejected, "a thread cannot start twice");

    // A worker failure is captured and rethrown on join in the joining thread.
    zl::RuntimeThreadState failing;
    failing.startWith([] { throw std::runtime_error("worker boom"); });
    bool rethrown = false;
    try {
        failing.join();
    } catch (const std::runtime_error& error) {
        rethrown = true;
        require(std::string(error.what()) == "worker boom", "join rethrows the worker's failure");
    }
    require(rethrown, "join surfaces a worker failure instead of ending the process");
}

} // namespace

int main() {
    testExecutorCompletion();
    testExecutorFailureContainment();
    testConcurrentSubmission();
    testTaskLifecycle();
    testTaskFailureAndObservation();
    testTaskCancellation();
    testUnobservedFailureReport();
    testCancellationCascade();
    testConcurrentObserve();
    testThreadState();

    if (failures != 0) {
        std::cerr << failures << " task executor regression(s) failed\n";
        return 1;
    }
    std::cout << "all task executor regressions passed\n";
    return 0;
}
