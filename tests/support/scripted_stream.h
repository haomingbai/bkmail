/**
 * @file tests/support/scripted_stream.h
 * @brief Scripted in-memory stream double modeling the bkmail stream concept.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * `bkmail::test::scripted_stream` is the foundation of every scripted test in
 * this project: it models the stream requirements of `imap_context`
 * (docs/architecture.md §3.1) without sockets, so Layer-1 tests replay a
 * fully deterministic byte script.
 *
 * The script is a list of steps consumed in order:
 *
 *   - `server_bytes{"..."}`   — one chunk the "server" sends. Each armed
 *                               `async_read_some` delivers at most one chunk
 *                               (fewer bytes when the caller's buffer is
 *                               smaller), which emulates fragmented TCP
 *                               delivery. Literal blocks are plain bytes here
 *                               and may freely contain CRLF.
 *   - `expect_write{"..."}`   — a gate. Delivery of every following step is
 *                               suspended until the bytes the client has
 *                               written so far contain the expected substring
 *                               (searched after the previously satisfied
 *                               gate). This is how tests assert wire
 *                               ordering, e.g. "the client wrote the {n}
 *                               literal header before the server sent the
 *                               continuation" or "DONE was written before the
 *                               tagged IDLE reply arrived".
 *   - `server_eof{}`          — the next read completes with
 *                               `set_value({}, 0)`; `n == 0` is bnio's EOF
 *                               signal (docs/architecture.md §2.3).
 *   - `server_error{ec}`      — the next read completes with
 *                               `set_value(ec, 0)`, injecting a transport
 *                               error.
 *
 * Once the script is exhausted, further reads pend forever (a quiet server),
 * which is the natural state at test teardown.
 *
 * Completion contract (identical to bnio sockets, docs/architecture.md §2.1):
 * every operation completes with `set_value(std::error_code, std::size_t)` or
 * `set_stopped()`; there is no error channel. Stop tokens are observed at the
 * same points bnio observes them (start/post points, §2.6) — and only there:
 * a read parked on a never-satisfied `expect_write` gate observes a stop
 * request no earlier than the next drive pass, so tests that cancel such a
 * read must follow the request with one more operation that triggers a drive
 * (any submission or flush). Completions are
 * always delivered from a task posted onto the scheduler passed to the async
 * call — never inline from `start()` — mirroring bnio's behaviour and keeping
 * handler threads exactly as documented in usage.md §7.
 *
 * Client writes are recorded: `script_recorder::writes()` holds one entry per
 * accepted `async_write` (so "N submits flush as one write" is directly
 * assertable) and `script_recorder::written()` is their concatenation. Obtain
 * the recorder through `scripted_stream::recorder()` *before* moving the
 * stream into `imap_context`; the recorder shares the stream's state and
 * stays valid for the whole test.
 *
 * `lowest_layer()` returns a socketpair-backed `bnio::tcp::socket` so that
 * `shutdown()`/`close()` calls from the context's close protocol behave like
 * they do on a real transport.
 *
 * Lifetime contract (mirrors usage.md §7.4): an operation state must not be
 * destroyed while the stream is delivering its completion. Tests satisfy this
 * by synchronizing with every handler before tearing the context down; a
 * still-pending operation detaches itself from the stream on destruction.
 */

#pragma once
#ifndef BKMAIL_TESTS_SUPPORT_SCRIPTED_STREAM_H_
#define BKMAIL_TESTS_SUPPORT_SCRIPTED_STREAM_H_

#include <bnio/buffer/basic.h>
#include <bnio/io_context.h>
#include <bnio/tcp/socket.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bexec/bexec.hpp>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bkmail::test {

/**
 * One script step of `scripted_stream`; see the file-level documentation.
 */
struct server_bytes {
  std::string data;
};

/**
 * Gate step: following steps are withheld until the client wrote `expected`.
 */
struct expect_write {
  std::string expected;
};

/**
 * Orderly server shutdown: the next read reports EOF (`set_value({}, 0)`).
 */
struct server_eof {};

/**
 * Transport-error injection: the next read reports `set_value(ec, 0)`.
 */
struct server_error {
  std::error_code ec;
};

using script_step =
    std::variant<server_bytes, expect_write, server_eof, server_error>;

/**
 * The full server script, consumed strictly in order.
 */
using script = std::vector<script_step>;

namespace detail {

/**
 * Type-erased view of one armed read operation, used by the script engine.
 * Completion member functions are never called under the state mutex.
 */
class scripted_read_op_base {
 public:
  scripted_read_op_base() = default;
  scripted_read_op_base(const scripted_read_op_base&) = delete;
  scripted_read_op_base& operator=(const scripted_read_op_base&) = delete;
  virtual ~scripted_read_op_base() = default;

  /// Destination buffer of the armed read.
  [[nodiscard]] virtual bnio::mutable_buffer buffer() const noexcept = 0;
  /// Whether the receiver's stop token has been requested (observed at
  /// start/post points, matching bnio's token-observation model).
  [[nodiscard]] virtual bool stop_requested() const noexcept = 0;
  /// Delivers the value completion to the stored receiver.
  virtual void complete(std::error_code ec, std::size_t n) noexcept = 0;
  /// Delivers the stopped completion to the stored receiver.
  virtual void complete_stopped() noexcept = 0;
};

/**
 * Type-erased view of one armed write operation.
 */
class scripted_write_op_base {
 public:
  scripted_write_op_base() = default;
  scripted_write_op_base(const scripted_write_op_base&) = delete;
  scripted_write_op_base& operator=(const scripted_write_op_base&) = delete;
  virtual ~scripted_write_op_base() = default;

  /// Byte count of the write (a bnio async_write is write-all).
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  /// Whether the receiver's stop token has been requested.
  [[nodiscard]] virtual bool stop_requested() const noexcept = 0;
  /// Delivers the value completion to the stored receiver.
  virtual void complete(std::error_code ec, std::size_t n) noexcept = 0;
  /// Delivers the stopped completion to the stored receiver.
  virtual void complete_stopped() noexcept = 0;
};

/**
 * Shared state behind a scripted_stream/script_recorder pair.
 */
struct script_state {
  std::mutex mutex;
  /// Signalled whenever a client write is recorded (recorder waits).
  std::condition_variable write_cv;

  script steps;
  std::size_t step_cursor = 0;
  /// Offset into the current server_bytes chunk (partial deliveries).
  std::size_t chunk_offset = 0;
  /// Consumed prefix of written_all; gates are matched from here on.
  std::size_t gate_cursor = 0;

  std::string written_all;
  std::vector<std::string> writes;

  std::optional<std::error_code> fail_next_write;

  scripted_read_op_base* pending_read = nullptr;
  scripted_write_op_base* pending_write = nullptr;
  std::error_code pending_write_ec;
  bool drive_posted = false;
};

/**
 * Runs one pass of the script engine on the scheduler thread: completes the
 * armed write (if any) and the armed read (if the next step is deliverable).
 */
inline void drive_script(const std::shared_ptr<script_state>& state) noexcept {  scripted_write_op_base* write_op = nullptr;
  scripted_read_op_base* read_op = nullptr;
  std::error_code write_ec;
  std::error_code read_ec;
  std::size_t write_n = 0;
  std::size_t read_n = 0;
  bool write_stopped = false;
  bool read_stopped = false;

  {
    std::lock_guard lock(state->mutex);
    state->drive_posted = false;

    if (state->pending_write != nullptr) {
      write_op = state->pending_write;
      state->pending_write = nullptr;
      if (write_op->stop_requested()) {
        write_stopped = true;
      } else {
        write_ec = state->pending_write_ec;
        write_n = write_op->size();
      }
    }

    if (state->pending_read != nullptr) {
      scripted_read_op_base* r = state->pending_read;
      if (r->stop_requested()) {
        state->pending_read = nullptr;
        read_op = r;
        read_stopped = true;
      } else {
        while (state->step_cursor < state->steps.size()) {
          auto& step = state->steps[state->step_cursor];
          if (auto* bytes = std::get_if<server_bytes>(&step)) {
            const std::size_t remaining =
                bytes->data.size() - state->chunk_offset;
            if (remaining == 0) {
              // Empty (or fully delivered) chunk: skip it.
              ++state->step_cursor;
              state->chunk_offset = 0;
              continue;
            }
            const bnio::mutable_buffer buf = r->buffer();
            const std::size_t n = (std::min)(remaining, buf.size());
            if (n == 0) {
              break;  // Degenerate zero-size buffer; wait for the next drive.
            }
            std::memcpy(buf.data(), bytes->data.data() + state->chunk_offset,
                        n);
            state->chunk_offset += n;
            if (state->chunk_offset == bytes->data.size()) {
              ++state->step_cursor;
              state->chunk_offset = 0;
            }
            state->pending_read = nullptr;
            read_op = r;
            read_n = n;
            read_ec = {};
            break;  // One chunk (or partial chunk) per read.
          }
          if (auto* gate = std::get_if<expect_write>(&step)) {
            const std::size_t pos =
                state->written_all.find(gate->expected, state->gate_cursor);
            if (pos == std::string::npos) {
              break;  // Gate closed: the read stays pending.
            }
            state->gate_cursor = pos + gate->expected.size();
            ++state->step_cursor;
            continue;
          }
          if (std::get_if<server_eof>(&step) != nullptr) {
            ++state->step_cursor;
            state->pending_read = nullptr;
            read_op = r;
            read_n = 0;  // n == 0 is bnio's EOF signal.
            read_ec = {};
            break;
          }
          if (auto* error = std::get_if<server_error>(&step)) {
            ++state->step_cursor;
            state->pending_read = nullptr;
            read_op = r;
            read_ec = error->ec;
            break;
          }
        }
      }
    }
  }

  // Completions run without the lock; receivers may start fresh operations
  // (which post their own drive) or run arbitrary user handlers.
  if (write_op != nullptr) {
    if (write_stopped) {
      write_op->complete_stopped();
    } else {
      write_op->complete(write_ec, write_n);
    }
  }
  if (read_op != nullptr) {
    if (read_stopped) {
      read_op->complete_stopped();
    } else {
      read_op->complete(read_ec, read_n);
    }
  }
}

/**
 * Heap-allocated, self-destroying schedule operation: runs @p F once on the
 * scheduler's thread. Receiver member bodies are compiled only after the
 * enclosing class is complete, which is why this lives at namespace scope.
 */
template <class Scheduler, class F>
class posted_task {
 public:
  class receiver {
   public:
    explicit receiver(posted_task* task) noexcept : task_(task) {}

    void set_value(std::error_code ec) noexcept {
      posted_task* task = task_;
      if (ec) {
        // The context is stopping; drop the task (teardown path).
        delete task;
        return;
      }
      task->run_and_delete();
    }

    void set_stopped() noexcept { delete task_; }

   private:
    posted_task* task_;
  };

  using operation_type = decltype(bexec::connect(
      std::declval<Scheduler&>().schedule(), std::declval<receiver>()));

  posted_task(Scheduler& scheduler, F fn)
      : fn_(std::move(fn)),
        operation_(bexec::connect(scheduler.schedule(), receiver{this})) {}

  void start() noexcept { bexec::start(operation_); }

  void run_and_delete() noexcept {
    F fn = std::move(fn_);
    fn();
    delete this;
  }

 private:
  F fn_;
  operation_type operation_;
};

/**
 * Posts @p f onto @p scheduler through a heap-allocated, self-destroying
 * schedule operation. Used so that every stream completion originates from
 * the scheduler's own thread, never inline from start().
 */
template <class Scheduler, class F>
void post_on_scheduler(Scheduler scheduler, F&& f) {
  using task_type = posted_task<Scheduler, std::decay_t<F>>;
  auto* box = new (std::nothrow) task_type(scheduler, std::forward<F>(f));
  if (box == nullptr) {
    // Allocation failure must not hang the test: fall back to inline run.
    f();
    return;
  }
  box->start();
}

/**
 * Arms one drive pass on the scheduler unless one is already in flight.
 * Callers must hold state->mutex.
 */
template <class Scheduler>
void post_drive_locked(const std::shared_ptr<script_state>& state,
                       Scheduler& scheduler) {
  if (state->drive_posted) {
    return;
  }
  state->drive_posted = true;
  post_on_scheduler(scheduler,
                    [weak = std::weak_ptr<script_state>(state)]() mutable {
                      if (auto locked = weak.lock()) {
                        drive_script(locked);
                      }
                    });
}

/**
 * Read operation state: delivers scripted chunks through the receiver.
 */
template <class Scheduler, class Receiver>
class scripted_read_operation final : public scripted_read_op_base {
 public:
  scripted_read_operation(std::shared_ptr<script_state> state,
                          Scheduler scheduler, bnio::mutable_buffer buffer,
                          Receiver receiver)
      : state_(std::move(state)),
        scheduler_(std::move(scheduler)),
        buffer_(buffer),
        receiver_(std::move(receiver)) {}

  scripted_read_operation(const scripted_read_operation&) = delete;
  scripted_read_operation& operator=(const scripted_read_operation&) = delete;
  scripted_read_operation(scripted_read_operation&&) = delete;
  scripted_read_operation& operator=(scripted_read_operation&&) = delete;

  /// Detaches a still-pending read (teardown path, see file documentation).
  ~scripted_read_operation() {
    std::lock_guard lock(state_->mutex);
    if (state_->pending_read == this) {
      state_->pending_read = nullptr;
    }
  }

  void start() noexcept {
    std::lock_guard lock(state_->mutex);
    // The read pump arms exactly one read at a time (architecture §3.5).
    assert(state_->pending_read == nullptr);
    state_->pending_read = this;
    post_drive_locked(state_, scheduler_);
  }

  [[nodiscard]] bnio::mutable_buffer buffer() const noexcept override {
    return buffer_;
  }

  [[nodiscard]] bool stop_requested() const noexcept override {
    return bexec::query(bexec::get_env(receiver_), bexec::get_stop_token)
        .stop_requested();
  }

  void complete(std::error_code ec, std::size_t n) noexcept override {
    bexec::set_value(std::move(receiver_), ec, n);
  }

  void complete_stopped() noexcept override {
    bexec::set_stopped(std::move(receiver_));
  }

 private:
  std::shared_ptr<script_state> state_;
  Scheduler scheduler_;
  bnio::mutable_buffer buffer_;
  Receiver receiver_;
};

/**
 * Write operation state: records the client bytes, then completes.
 */
template <class Scheduler, class Receiver>
class scripted_write_operation final : public scripted_write_op_base {
 public:
  scripted_write_operation(std::shared_ptr<script_state> state,
                           Scheduler scheduler, bnio::const_buffer buffer,
                           Receiver receiver)
      : state_(std::move(state)),
        scheduler_(std::move(scheduler)),
        buffer_(buffer),
        receiver_(std::move(receiver)) {}

  scripted_write_operation(const scripted_write_operation&) = delete;
  scripted_write_operation& operator=(const scripted_write_operation&) = delete;
  scripted_write_operation(scripted_write_operation&&) = delete;
  scripted_write_operation& operator=(scripted_write_operation&&) = delete;

  /// Detaches a still-pending write (teardown path).
  ~scripted_write_operation() {
    std::lock_guard lock(state_->mutex);
    if (state_->pending_write == this) {
      state_->pending_write = nullptr;
    }
  }

  void start() noexcept {
    {
      std::lock_guard lock(state_->mutex);
      // The write pump serializes writes: at most one is ever in flight.
      assert(state_->pending_write == nullptr);
      state_->pending_write_ec = {};
      if (!stop_requested()) {
        if (state_->fail_next_write.has_value()) {
          state_->pending_write_ec = *state_->fail_next_write;
          state_->fail_next_write.reset();
        } else {
          // Bytes are recorded when the write starts: from the peer's point
          // of view a started write-all operation transmits these bytes.
          const auto* data = static_cast<const char*>(buffer_.data());
          state_->writes.emplace_back(data, buffer_.size());
          state_->written_all.append(data, buffer_.size());
        }
      }
      state_->pending_write = this;
      post_drive_locked(state_, scheduler_);
      // Notified under the lock: a waiter returns from its wait holding
      // the mutex, so this notify has already returned before the
      // waiter's thread can destroy the cv (mirrors signal_event::arrive).
      state_->write_cv.notify_all();
    }
  }

  [[nodiscard]] std::size_t size() const noexcept override {
    return buffer_.size();
  }

  [[nodiscard]] bool stop_requested() const noexcept override {
    return bexec::query(bexec::get_env(receiver_), bexec::get_stop_token)
        .stop_requested();
  }

  void complete(std::error_code ec, std::size_t n) noexcept override {
    bexec::set_value(std::move(receiver_), ec, n);
  }

  void complete_stopped() noexcept override {
    bexec::set_stopped(std::move(receiver_));
  }

 private:
  std::shared_ptr<script_state> state_;
  Scheduler scheduler_;
  bnio::const_buffer buffer_;
  Receiver receiver_;
};

/**
 * Sender for one scripted read-some operation.
 */
template <class Scheduler>
class scripted_read_sender {
 public:
  /// bnio completion contract: value channel carries (ec, bytes read).
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  scripted_read_sender(std::shared_ptr<script_state> state, Scheduler scheduler,
                       bnio::mutable_buffer buffer)
      : state_(std::move(state)),
        scheduler_(std::move(scheduler)),
        buffer_(buffer) {}

  template <class Receiver>
  auto connect(Receiver receiver) {
    return scripted_read_operation<Scheduler, std::remove_cvref_t<Receiver>>{
        state_, scheduler_, buffer_, std::move(receiver)};
  }

 private:
  std::shared_ptr<script_state> state_;
  Scheduler scheduler_;
  bnio::mutable_buffer buffer_;
};

/**
 * Sender for one scripted write-all operation.
 */
template <class Scheduler>
class scripted_write_sender {
 public:
  /// bnio completion contract: value channel carries (ec, bytes written).
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  scripted_write_sender(std::shared_ptr<script_state> state,
                        Scheduler scheduler, bnio::const_buffer buffer)
      : state_(std::move(state)),
        scheduler_(std::move(scheduler)),
        buffer_(buffer) {}

  template <class Receiver>
  auto connect(Receiver receiver) {
    return scripted_write_operation<Scheduler, std::remove_cvref_t<Receiver>>{
        state_, scheduler_, buffer_, std::move(receiver)};
  }

 private:
  std::shared_ptr<script_state> state_;
  Scheduler scheduler_;
  bnio::const_buffer buffer_;
};

}  // namespace detail

/**
 * Inspection handle for a scripted_stream. Copyable; shares the stream's
 * state, so it stays usable after the stream was moved into an imap_context.
 */
class script_recorder {
 public:
  /// Concatenation of every accepted client write, in wire order.
  [[nodiscard]] std::string written() const {
    std::lock_guard lock(state_->mutex);
    return state_->written_all;
  }

  /// One entry per accepted async_write (asserts on write batching).
  [[nodiscard]] std::vector<std::string> writes() const {
    std::lock_guard lock(state_->mutex);
    return state_->writes;
  }

  /// Whether the client has written `needle` so far.
  [[nodiscard]] bool contains_written(std::string_view needle) const {
    std::lock_guard lock(state_->mutex);
    return state_->written_all.find(needle) != std::string::npos;
  }

  /// Waits until at least `count` client bytes were written.
  template <class Rep, class Period>
  bool wait_written_bytes(std::size_t count,
                          std::chrono::duration<Rep, Period> timeout) const {
    auto& state = *state_;
    std::unique_lock lock(state.mutex);
    return state.write_cv.wait_for(
        lock, timeout, [&] { return state.written_all.size() >= count; });
  }

  /// Waits until at least `count` async_write operations were accepted.
  template <class Rep, class Period>
  bool wait_write_count(std::size_t count,
                        std::chrono::duration<Rep, Period> timeout) const {
    auto& state = *state_;
    std::unique_lock lock(state.mutex);
    return state.write_cv.wait_for(
        lock, timeout, [&] { return state.writes.size() >= count; });
  }

  /// Waits until the client writes a byte string containing `needle`.
  template <class Rep, class Period>
  bool wait_written(std::string_view needle,
                    std::chrono::duration<Rep, Period> timeout) const {
    auto& state = *state_;
    std::unique_lock lock(state.mutex);
    return state.write_cv.wait_for(lock, timeout, [&] {
      return state.written_all.find(needle) != std::string::npos;
    });
  }

 private:
  friend class scripted_stream;

  explicit script_recorder(std::shared_ptr<detail::script_state> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::script_state> state_;
};

/**
 * Scripted in-memory stream modeling the imap_context stream requirements
 * (docs/architecture.md §3.1). See the file-level documentation for the
 * script vocabulary and the completion contract.
 */
class scripted_stream {
 public:
  /// Builds the stream over a fixed server script (see file documentation).
  explicit scripted_stream(script steps)
      : state_(std::make_shared<detail::script_state>()) {
    state_->steps = std::move(steps);
    // A socketpair end gives lowest_layer() a live descriptor so the
    // context's close protocol (shutdown/close) works as on a real socket.
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      socket_ = bnio::tcp::socket(fds[0]);
      peer_fd_ = fds[1];
    }
  }

  scripted_stream(const scripted_stream&) = delete;
  scripted_stream& operator=(const scripted_stream&) = delete;

  scripted_stream(scripted_stream&& other) noexcept
      : state_(std::move(other.state_)),
        socket_(std::move(other.socket_)),
        peer_fd_(std::exchange(other.peer_fd_, -1)) {}

  scripted_stream& operator=(scripted_stream&&) = delete;

  ~scripted_stream() {
    if (peer_fd_ >= 0) {
      ::close(peer_fd_);
    }
  }

  /// Returns the inspection handle; call before moving the stream away.
  [[nodiscard]] script_recorder recorder() const {
    return script_recorder(state_);
  }

  /// Makes the next accepted write complete with @p ec (and record nothing).
  void fail_next_write(std::error_code ec) {
    std::lock_guard lock(state_->mutex);
    state_->fail_next_write = ec;
  }

  /// Arms one read; each call delivers at most one scripted chunk.
  template <class Scheduler>
  [[nodiscard]] auto async_read_some(Scheduler scheduler,
                                     bnio::mutable_buffer buffer,
                                     int /*flags*/ = 0) {
    return detail::scripted_read_sender<Scheduler>{state_, std::move(scheduler),
                                                   buffer};
  }

  /// Arms one write-all; accepted bytes are appended to the write record.
  template <class Scheduler>
  [[nodiscard]] auto async_write(Scheduler scheduler, bnio::const_buffer buffer,
                                 int /*flags*/ = 0) {
    return detail::scripted_write_sender<Scheduler>{
        state_, std::move(scheduler), buffer};
  }

  /// Access to shutdown()/close(), as required by the stream concept.
  [[nodiscard]] bnio::tcp::socket& lowest_layer() noexcept { return socket_; }

  /// Access to shutdown()/close(), as required by the stream concept.
  [[nodiscard]] const bnio::tcp::socket& lowest_layer() const noexcept {
    return socket_;
  }

  /// Optional stream extension consumed by `context_core::abandon()` (via
  /// a requires-expression): a real transport completes the armed read
  /// with EOF as soon as the abandonment protocol shut the read side down
  /// (SHUT_RD), which retires the pump boxes and lets the context core
  /// destroy itself. The script engine cannot observe
  /// `lowest_layer().shutdown()`, so `abandon()` asks the stream to
  /// deliver the same effect explicitly.
  ///
  /// Runs inline on the caller's thread — the abandonment path is quiet
  /// (no handler runs), so no scheduler round-trip is needed; this also
  /// keeps working when the owning io_context is already gone (a shell
  /// that outlived its runner). Mirrors drive_script's stop-token
  /// arbitration.
  void complete_pending_io_for_teardown() noexcept {
    detail::scripted_write_op_base* write_op = nullptr;
    detail::scripted_read_op_base* read_op = nullptr;
    std::error_code write_ec;
    bool write_stopped = false;
    bool read_stopped = false;
    std::size_t write_n = 0;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->pending_write != nullptr) {
        write_op = state_->pending_write;
        state_->pending_write = nullptr;
        if (write_op->stop_requested()) {
          write_stopped = true;
        } else {
          write_ec = state_->pending_write_ec;
          write_n = write_op->size();
        }
      }
      if (state_->pending_read != nullptr) {
        read_op = state_->pending_read;
        state_->pending_read = nullptr;
        read_stopped = read_op->stop_requested();
      }
    }
    // Completions run without the lock; the abandonment path invokes no
    // user code, so inline delivery is safe (and required: the scheduler
    // may already be gone).
    if (write_op != nullptr) {
      if (write_stopped) {
        write_op->complete_stopped();
      } else {
        write_op->complete(write_ec, write_n);
      }
    }
    if (read_op != nullptr) {
      if (read_stopped) {
        read_op->complete_stopped();
      } else {
        read_op->complete(std::error_code{}, 0);  // n == 0 is EOF.
      }
    }
  }

  /// Convenience forwarder, see script_recorder::written(). Only valid
  /// while the stream has not been moved from.
  [[nodiscard]] std::string written() const {
    if (state_ == nullptr) {
      return {};
    }
    return recorder().written();
  }

  /// Convenience forwarder, see script_recorder::writes(). Only valid while
  /// the stream has not been moved from.
  [[nodiscard]] std::vector<std::string> writes() const {
    if (state_ == nullptr) {
      return {};
    }
    return recorder().writes();
  }

 private:
  std::shared_ptr<detail::script_state> state_;
  bnio::tcp::socket socket_;
  int peer_fd_ = -1;
};

/**
 * Compile-time mirror of the stream requirements in docs/architecture.md
 * §3.1; scripted_stream must model it to plug into imap_context.
 */
template <class S>
concept test_imap_stream =
    requires(S& s, bnio::io_context::post_scheduler sched,
             bnio::mutable_buffer mb, bnio::const_buffer cb) {
      { s.async_read_some(sched, mb, 0) };
      { s.async_write(sched, cb, 0) };
      { s.lowest_layer() } -> std::same_as<bnio::tcp::socket&>;
    };

static_assert(test_imap_stream<scripted_stream>);

namespace detail {

/**
 * Receiver used only by the compile-time contract checks below.
 */
class contract_check_receiver {
 public:
  void set_value(std::error_code, std::size_t) noexcept {}
  void set_stopped() noexcept {}
};

using contract_read_sender =
    decltype(std::declval<scripted_stream&>().async_read_some(
        std::declval<bnio::io_context::post_scheduler>(),
        bnio::mutable_buffer{}, 0));
using contract_write_sender =
    decltype(std::declval<scripted_stream&>().async_write(
        std::declval<bnio::io_context::post_scheduler>(), bnio::const_buffer{},
        0));

// The senders model the bexec sender concept and connect to any receiver of
// (std::error_code, std::size_t) / set_stopped — the bnio socket contract.
static_assert(bexec::sender<contract_read_sender>);
static_assert(bexec::sender<contract_write_sender>);
static_assert(bexec::sender_to<contract_read_sender, contract_check_receiver>);
static_assert(bexec::sender_to<contract_write_sender, contract_check_receiver>);
static_assert(
    std::same_as<bexec::completion_signatures_of_t<contract_read_sender>,
                 bexec::completion_signatures<
                     bexec::set_value_t(std::error_code, std::size_t),
                     bexec::set_stopped_t()>>);
static_assert(
    std::same_as<bexec::completion_signatures_of_t<contract_write_sender>,
                 bexec::completion_signatures<
                     bexec::set_value_t(std::error_code, std::size_t),
                     bexec::set_stopped_t()>>);

}  // namespace detail

}  // namespace bkmail::test

#endif  // BKMAIL_TESTS_SUPPORT_SCRIPTED_STREAM_H_
