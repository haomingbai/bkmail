/**
 * @file tests/support/io_runner.h
 * @brief Dedicated-thread bnio::io_context runner plus test sync helpers.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * `io_runner` is the helper from usage.md §1.3: a bkmail sender only makes
 * progress while some thread runs the borrowed `bnio::io_context`, so every
 * test that consumes senders with `bexec::this_thread::sync_wait` keeps one
 * of these alive for the duration of the session.
 *
 * `signal_event` and `poll_until` cover the other direction: handlers and
 * unsolicited callbacks fire on the io thread, and the test thread needs a
 * bounded wait for them (`signal_event`) or for a condition that offers no
 * notification hook (`poll_until`, e.g. `imap_context::is_alive()`).
 */

#pragma once
#ifndef BKMAIL_TESTS_SUPPORT_IO_RUNNER_H_
#define BKMAIL_TESTS_SUPPORT_IO_RUNNER_H_

#include <bnio/io_context.h>

#include <bexec/bexec.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace bkmail::test {

/**
 * Runs a bnio::io_context on a dedicated thread for the object's lifetime.
 * Mirrors the helper in usage.md §1.3.
 */
class io_runner {
 public:
  io_runner() : thread_([this] { (void)ioc_.run(); }) {}

  io_runner(const io_runner&) = delete;
  io_runner& operator=(const io_runner&) = delete;

  ~io_runner() {
    ioc_.stop();
    thread_.join();
  }

  /// The context borrowed by sessions under test.
  [[nodiscard]] bnio::io_context& get() noexcept { return ioc_; }

  /**
   * Runs one empty task through the io_context and waits for it on the
   * calling thread: when this returns, the worker has finished executing
   * every completion that was in flight before the call — a happens-before
   * barrier proving the worker is outside any receiver call chain (used
   * before destroying test-stack objects those receivers reference).
   * Defined after signal_event below.
   */
  void quiesce();

  /**
   * Runs @p f once ON the io worker thread and waits for it on the calling
   * thread (same skeleton as quiesce(): schedule() through the post
   * scheduler, a receiver that invokes @p f and then arrives; bounded
   * wait). Because the task is queued behind everything posted before it,
   * work @p f triggers (e.g. a cancel pass posting a pump kick) lands
   * strictly BEHIND the running task — the queue-order pin that keeps a
   * scripted interleaving deterministic.
   * Defined after the detail receiver below.
   */
  template <class F>
  void run_on_worker(F&& f);

 private:
  bnio::io_context ioc_;
  std::thread thread_;
};

/**
 * RAII teardown barrier for the same happens-before guarantee as
 * quiesce(), applied to every exit path of a test body: declared
 * immediately after the objects a receiver references (the started
 * operation state, or the locals handlers capture), its destructor
 * quiesces the runner BEFORE those objects are destroyed — so an early
 * ASSERT failure unwinding the test cannot destroy them while an io
 * worker is still inside a receiver.
 */
class quiesce_guard {
 public:
  explicit quiesce_guard(io_runner& runner) noexcept : runner_(runner) {}

  quiesce_guard(const quiesce_guard&) = delete;
  quiesce_guard& operator=(const quiesce_guard&) = delete;

  ~quiesce_guard() { runner_.quiesce(); }

 private:
  io_runner& runner_;
};

/**
 * One-shot, count-up cross-thread signal with a bounded wait. Handlers call
 * arrive() (possibly several times); the test thread blocks in
 * wait()/wait_for() until at least one arrival was observed.
 */
class signal_event {
 public:
  /// Records one arrival; never blocks.
  ///
  /// Notifies WHILE holding the mutex: a waiter returns from wait() with
  /// the mutex held, so the notifier's notify_all has already returned —
  /// destroying the event after a satisfied wait is then race-free.
  void arrive() {
    std::lock_guard lock(mutex_);
    ++count_;
    cv_.notify_all();
  }

  /// Blocks until at least one arrival was observed.
  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return count_ != 0U; });
  }

  /// Blocks until at least one arrival was observed or the timeout expires;
  /// returns false on timeout (the test then fails with context).
  template <class Rep, class Period>
  [[nodiscard]] bool wait_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return count_ != 0U; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t count_ = 0;
};

/**
 * Bounded busy-wait for conditions that expose no notification (e.g.
 * is_alive() flipping after a BYE). Returns the predicate's final value.
 */
template <class Predicate, class Rep, class Period>
[[nodiscard]] bool poll_until(Predicate predicate,
                              std::chrono::duration<Rep, Period> timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return predicate();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

/// Default bound for the scripted exchanges in this test suite; generous so
/// CI stalls surface as readable failures instead of flaky timeouts.
inline constexpr std::chrono::milliseconds kDefaultTimeout{10000};

namespace detail {

/// Receiver completing io_runner::quiesce()'s empty barrier task.
class quiesce_receiver {
 public:
  explicit quiesce_receiver(signal_event* done) noexcept : done_(done) {}

  void set_value(std::error_code) noexcept { done_->arrive(); }
  void set_stopped() noexcept { done_->arrive(); }

 private:
  signal_event* done_;
};

/// Receiver completing io_runner::run_on_worker()'s task: invokes the
/// stored callable on the io worker, then arrives. The callable is held by
/// value so the caller's locals need not outlive the queue wait.
template <class F>
class run_on_worker_receiver {
 public:
  run_on_worker_receiver(F f, signal_event* done) noexcept
      : f_(std::move(f)), done_(done) {}

  void set_value(std::error_code ec) noexcept {
    if (!ec) {
      f_();
    }
    done_->arrive();
  }

  void set_stopped() noexcept { done_->arrive(); }

 private:
  F f_;
  signal_event* done_;
};

}  // namespace detail

inline void io_runner::quiesce() {
  signal_event done;
  auto operation = bexec::connect(ioc_.get_post_scheduler().schedule(),
                                  detail::quiesce_receiver{&done});
  bexec::start(operation);
  done.wait();
}

template <class F>
void io_runner::run_on_worker(F&& f) {
  signal_event done;
  auto operation =
      bexec::connect(ioc_.get_post_scheduler().schedule(),
                     detail::run_on_worker_receiver<std::decay_t<F>>{
                         std::forward<F>(f), &done});
  bexec::start(operation);
  (void)done.wait_for(kDefaultTimeout);
}

}  // namespace bkmail::test

#endif  // BKMAIL_TESTS_SUPPORT_IO_RUNNER_H_
