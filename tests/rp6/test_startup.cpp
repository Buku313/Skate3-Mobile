// Includes the actual production method bodies verbatim. Test-only mutex and
// condition-variable adapters force the first state unlock scheduling boundary.
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

thread_local bool worker_thread = false;
class Gate {
 public:
  void open() { std::lock_guard lock(mutex_); open_ = true; cv_.notify_all(); }
  void wait() { std::unique_lock lock(mutex_); assert(cv_.wait_for(lock, std::chrono::seconds(3), [&]{return open_;})); }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};
class HookMutex {
 public:
  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); if (after_unlock) after_unlock(); }
  bool try_lock() { return mutex_.try_lock(); }
  std::function<void()> after_unlock;
 private:
  std::mutex mutex_;
};
class HookCV {
 public:
  void notify_all() { cv_.notify_all(); }
  template<class Lock,class Pred> void wait(Lock& lock,Pred predicate) {
    if (before_wait) before_wait();
    cv_.wait(lock,predicate);
  }
  std::function<void()> before_wait;
 private:
  std::condition_variable_any cv_;
};
namespace shim_std {
using mutex = HookMutex;
template<class T> using unique_lock = ::std::unique_lock<T>;
using ::std::move;
}
struct FakeSemaphore { std::atomic<unsigned> posts{0}; };
int fake_sem_post(FakeSemaphore* sem) { ++sem->posts; return 0; }
uint64_t fake_pthread_self() { return worker_thread ? 2 : 1; }
std::atomic<unsigned> delivered_signal_requests{0};
int fake_pthread_kill(uint64_t,int) { ++delivered_signal_requests; return 0; }
enum class SignalType { kThreadSuspend };
int GetSystemSignal(SignalType) { return 1; }
void set_current_thread_name(const char*) {}

namespace rex::thread {
class Thread { public: virtual ~Thread() = default; };
struct ThreadStartData { std::function<void()> start_routine; bool create_suspended; Thread* thread_obj; };
class PosixConditionBase {
 public:
  void NotifyMultiWaiters() { ++multi_notify_count; }
  void NotifyWaitAny() { ++multi_notify_count; }
  std::atomic<unsigned> multi_notify_count{0};
  HookMutex mutex_;
  HookCV cond_;
};
template<class T> class PosixCondition;
template<> class PosixCondition<Thread> : public PosixConditionBase {
 public:
  enum class State { kUninitialized,kRunning,kSuspended,kFinished };
  uint64_t thread_ = 2;
  bool signaled_ = false;
  int exit_code_ = 99;
  State state_ = State::kUninitialized;
  uint32_t suspend_count_ = 0;
  FakeSemaphore suspend_sem_;
  mutable HookMutex state_mutex_;
  mutable HookCV state_signal_;
  static void* ThreadStartRoutine(void*);
  void WaitSuspended() { assert(false && "Signal-based self suspend is outside this harness"); }
#define std shim_std
#define pthread_self fake_pthread_self
#define pthread_kill fake_pthread_kill
#define sem_post fake_sem_post
#include "resume-method.inc"
#include "suspend-method.inc"
#include "waitstarted-method.inc"
#undef sem_post
#undef pthread_kill
#undef pthread_self
#undef std
};
class PosixThread : public Thread { public: PosixCondition<Thread> handle_; };
thread_local PosixThread* current_thread_ = nullptr;
thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;
#define REX_PLATFORM_ANDROID 1
#define assert_always() assert(false)
#define assert_not_null(value) assert(value)
#define std shim_std
#ifdef BASELINE
#include "baseline-start.inc"
#else
#include "patched-start.inc"
#endif
#undef std
}

using Condition = rex::thread::PosixCondition<rex::thread::Thread>;
struct Case {
  rex::thread::PosixThread object;
  Gate gap, continue_start, initial_wait, entered;
  std::atomic<unsigned> body_calls{0};
  std::thread worker;
  bool gap_seen = false;  // Worker only.
  void start(bool suspended) {
    object.handle_.state_mutex_.after_unlock = [&] {
      if (worker_thread && !gap_seen) {
        gap_seen = true; gap.open(); continue_start.wait();
      }
    };
    object.handle_.state_signal_.before_wait = [&] { if(worker_thread) initial_wait.open(); };
    auto* data = new rex::thread::ThreadStartData{[&]{ ++body_calls; entered.open(); }, suspended, &object};
    worker = std::thread([data] {
      worker_thread = true;
      assert(Condition::ThreadStartRoutine(data) == nullptr);
      assert(rex::thread::current_thread_ == nullptr);
      assert(rex::thread::current_thread_condition_ == nullptr);
    });
    gap.wait();
  }
  void finish() {
    entered.wait(); worker.join();
    assert(body_calls == 1);
    assert(object.handle_.state_ == Condition::State::kFinished);
    assert(object.handle_.signaled_ && object.handle_.exit_code_ == 0);
    assert(object.handle_.multi_notify_count == 1);
  }
};

int main() {
  unsigned previous = 99;
#ifdef BASELINE
  Case test; test.start(true);
  assert(test.object.handle_.state_ == Condition::State::kSuspended);
  assert(test.object.handle_.suspend_count_ == 0);
  assert(!test.object.handle_.Resume(&previous));
  assert(previous == 0 && test.body_calls == 0);
  test.continue_start.open(); test.initial_wait.wait();
  {
    std::unique_lock lock(test.object.handle_.state_mutex_);
    assert(test.object.handle_.suspend_count_ == 1);
    assert(test.object.handle_.state_ == Condition::State::kSuspended);
    assert(test.body_calls == 0);
  }
  // Cleanup also proves that only an additional, otherwise unnecessary Resume
  // releases the baseline's lost-startup-resume condition.
  assert(test.object.handle_.Resume(&previous) && previous == 1);
  test.finish();
  std::cout << "REPRODUCED: first Resume lost at publication gap; second Resume required\n";
#else
  int cases = 0;
  {
    Case test; test.start(true);
    assert(test.object.handle_.suspend_count_ == 1);
    assert(test.object.handle_.Resume(&previous) && previous == 1);
    assert(test.object.handle_.suspend_count_ == 0 && test.body_calls == 0);
    assert(!test.object.handle_.Resume(&previous) && previous == 0);
    test.continue_start.open(); test.finish(); ++cases;
  }
  {
    Case test; test.start(true); test.continue_start.open(); test.initial_wait.wait();
    assert(test.object.handle_.Resume(&previous) && previous == 1);
    test.finish(); ++cases;
  }
  {
    Case test; test.start(false);
    assert(test.object.handle_.state_ == Condition::State::kRunning);
    assert(test.object.handle_.suspend_count_ == 0);
    assert(!test.object.handle_.Resume(&previous) && previous == 0);
    test.continue_start.open(); test.finish();
    assert(test.object.handle_.suspend_sem_.posts == 0); ++cases;
  }
  for (unsigned added : {1u,2u,3u}) {
    Case test; test.start(true);
    for (unsigned i=0;i<added;i++) {
      assert(test.object.handle_.Suspend(&previous) && previous == i+1);
    }
    for (unsigned i=0;i<added;i++) {
      assert(test.object.handle_.Resume(&previous) && previous == added+1-i);
      assert(test.object.handle_.suspend_count_ > 0 && test.body_calls == 0);
      assert(test.object.handle_.suspend_sem_.posts == 0);
    }
    test.continue_start.open(); test.initial_wait.wait();
    // A spurious notification must not bypass the production wait predicate.
    test.object.handle_.state_signal_.notify_all();
    {
      std::unique_lock lock(test.object.handle_.state_mutex_);
      assert(test.object.handle_.suspend_count_ == 1 && test.body_calls == 0);
    }
    assert(test.object.handle_.Resume(&previous) && previous == 1);
    test.finish();
    assert(test.object.handle_.suspend_sem_.posts == 1); ++cases;
  }
  std::cout << "PASS: " << cases << " deterministic production-method scenarios; startup/count transitions; no sanitizer findings\n";
#endif
}
