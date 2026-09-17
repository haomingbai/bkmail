/**
 * @file include/bkmail/imap/operation_base.h
 * @brief Type-erasure base for queued IMAP commands.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details This header is the single internal contract between
 * `imap_context` (which owns the machinery) and the command files of
 * `imap/command/` (which own the protocol knowledge). It provides:
 *
 *  - `operation_base<Allocator>`: the pinned, virtual-entry-point base of
 *    every queued operation, modeled on bexec's intrusive
 *    `run_loop_operation_base`. One heap allocation per submission, made
 *    through the context's rebound allocator; the cell is never moved
 *    afterwards (buffer stability, architecture §2.5/§3.4).
 *  - `operation_sink<Allocator>`: the narrow interface `imap_context`
 *    implements so an operation can report stop-token cancellation without
 *    naming the (Stream-templated) context type.
 *  - `operation_model<Command, HandlerOrReceiver>`: the concrete cell.
 *    `HandlerOrReceiver` is either a plain callback `void(std::error_code[,
 *    result_type])` (callback path) or a bexec receiver (sender path); the
 *    model adapts with `if constexpr`.
 *  - `op_deleter<Allocator>`: stateless deleter that routes destruction
 *    through the virtual `dispose()` so the cell frees itself with the same
 *    rebound allocator it was allocated with.
 *
 * Command type contract (implemented by every command header class):
 *
 *  - `using result_type = ...;` and `using allocator_type = ...;`
 *  - Tag stamping, called exactly once at registration:
 *    `void render(std::string_view tag)`. Renders at least the first
 *    segment.
 *  - `next_segment() noexcept` -> `bnio::const_buffer`: the currently
 *    stageable byte chunk (empty when none). Pure query: repeated calls
 *    before `on_segment_flushed()` return the same range. A segment
 *    never crosses a `{n}` boundary.
 *  - `bool awaits_continuation() const noexcept`: the literal/SASL wall —
 *    true while a continuation request is needed before more bytes may be
 *    staged.
 *  - `void on_segment_flushed() noexcept`: advances past the staged
 *    segment once it has hit the wire.
 *  - `void on_continuation(std::string_view text)`: releases the next
 *    segment after `+ ...`.
 *  - `void on_untagged(const untagged_response<allocator_type>& r)`:
 *    absorb command-relevant untagged data. Runs under the context's
 *    internal lock on the read-dispatch path: must be fast, must not
 *    complete user code, must not call back into the context.
 *  - `on_tagged`: terminal mapping, returning the error code to deliver
 *    (`errc::command_rejected` on NO, `errc::bad_command` on BAD — both
 *    submission paths, code_layout D2). Void-result commands:
 *    `std::error_code on_tagged(const tagged_response<A>&)`; result
 *    commands: `std::error_code on_tagged(const tagged_response<A>&,
 *    result_type& out)`, filling `out` only on success.
 *  - Optional `bool blocks_pipeline() const noexcept`: advisory wall for
 *    the write pump (AUTHENTICATE/LOGOUT/STARTTLS return true); nothing
 *    behind such a command is staged in the same batch.
 *  - Optional `void done()` (IDLE only): cancel of a written IDLE is
 *    forwarded here; the command queues its `DONE` segment and stays
 *    registered until the tagged reply arrives, which then completes
 *    with stop semantics instead of a value.
 */

#pragma once
#ifndef BKMAIL_IMAP_OPERATION_BASE_H_
#define BKMAIL_IMAP_OPERATION_BASE_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/error.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bkmail::imap::detail {

using bkmail::detail::rebind_alloc_t;
using bkmail::detail::string_of;

/// Transparent hash for the tag registry (heterogeneous lookup by
/// `std::string_view`).
struct string_hash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

/**
 * Maps a tagged reply status to the error code contract (code_layout D2):
 * OK is success, NO/BAD are protocol outcomes reported as errc values on
 * the value channel, anything else is a protocol violation.
 */
[[nodiscard]] inline std::error_code map_status_to_ec(
    response_status status) noexcept {
  switch (status) {
    case response_status::ok:
      return {};
    case response_status::no:
      return make_error_code(errc::command_rejected);
    case response_status::bad:
      return make_error_code(errc::bad_command);
    default:
      return make_error_code(errc::unexpected_response);
  }
}

template <class Allocator>
class operation_base;

/**
 * Coordination interface implemented by `imap_context` so that operations
 * can report cancellation without naming the (Stream-templated) context
 * type. All methods are called with the operation pinned and registered.
 */
template <class Allocator = std::allocator<std::byte>>
class operation_sink {
 public:
  operation_sink() = default;
  operation_sink(const operation_sink&) = delete;
  operation_sink& operator=(const operation_sink&) = delete;
  virtual ~operation_sink() = default;

  /**
   * Reports that the operation's stop token fired. Called on the
   * requesting thread (architecture §6): implementations set flags and
   * erase queue cells under the context mutex, and complete the receiver
   * from the context's own dispatch path, never inline here.
   */
  virtual void on_stop_requested(operation_base<Allocator>* op) noexcept = 0;

  /// Kicks the write pump (through the scheduler) after a cancelled
  /// written operation produced new bytes (IDLE `DONE`).
  virtual void kick_write_pump() noexcept = 0;
};

/**
 * Type-erasure base for queued IMAP commands. Pinned: receivers, the
 * registry, and the write queue hold `operation_base*`, so the object
 * address must never change after construction.
 */
template <class Allocator = std::allocator<std::byte>>
class operation_base {
 public:
  /// Allocator of every parsed response handed to this operation.
  using allocator_type = Allocator;

  operation_base() = default;
  operation_base(const operation_base&) = delete;
  operation_base& operator=(const operation_base&) = delete;
  virtual ~operation_base() = default;

  // --- write side ----------------------------------------------------

  /// Next chunk of command bytes to stage, or an empty buffer when there
  /// is nothing to send right now. Never crosses a `{n}` boundary.
  [[nodiscard]] virtual bnio::const_buffer next_segment() noexcept = 0;

  /// True while the operation is paused on a server continuation request
  /// (literal / SASL wall).
  [[nodiscard]] virtual bool awaits_continuation() const noexcept = 0;

  /// Advisory pipelining wall (AUTHENTICATE / LOGOUT / STARTTLS /
  /// IDLE-style commands): nothing behind this operation is staged in
  /// the same batch.
  [[nodiscard]] virtual bool blocks_pipeline() const noexcept = 0;

  /// The write pump staged `next_segment()` into the batch. Until
  /// `on_segment_flushed()` arrives, `next_segment()` returns empty.
  virtual void on_segment_staged() noexcept = 0;

  /// The staged segment has been written to the wire.
  virtual void on_segment_flushed() noexcept = 0;

  /// True once every byte of the command has been written and the
  /// operation only awaits server replies ("in flight").
  [[nodiscard]] virtual bool is_written() const noexcept = 0;

  /// True while a segment is staged but not yet flushed. Cancellation
  /// must never extract such an operation from the queue: the in-flight
  /// write still references it.
  [[nodiscard]] virtual bool is_staged() const noexcept = 0;

  // --- read side -----------------------------------------------------

  /// Routes a continuation request to the paused operation.
  virtual void on_continuation(std::string_view text) noexcept = 0;

  /// Broadcast hook for untagged responses; most operations ignore most
  /// responses. Runs under the context lock: no user code, no re-entry.
  virtual void on_untagged(const untagged_response<Allocator>& r) noexcept = 0;

  /**
   * Ownership predicate for command-scoped untagged data (FETCH / LIST /
   * LSUB / STATUS / SEARCH / CAPABILITY data lines and FLAGS). IMAP gives
   * no tag on untagged data, so when several commands that consume the
   * same data kind are pipelined, the read pump delivers each such line
   * to the EARLIEST-queued operation whose predicate accepts it (the
   * server completes pipelined commands in submission order in practice,
   * which is the only honest attribution available). Events with
   * connection scope (EXISTS / RECENT / EXPUNGE) and untagged status
   * responses (OK / NO / BAD / BYE / PREAUTH) are broadcast to every
   * operation instead and do not consult this predicate.
   */
  [[nodiscard]] virtual bool wants_untagged(
      untagged_kind kind) const noexcept = 0;

  /// Terminal tagged reply. Ownership of the operation cell is moved out
  /// of the queue before this call, so the operation outlives its own
  /// completion.
  virtual void on_tagged(tagged_response<Allocator> r) noexcept = 0;

  // --- control -------------------------------------------------------

  /// Connection-level failure: completes with `ec` on the value channel.
  virtual void fail(std::error_code ec) noexcept = 0;

  /// Stop-token driven cancel entry (architecture §3.7). Records the
  /// request and forwards to the sink; never completes the receiver
  /// inline.
  virtual void request_stop() noexcept = 0;

  /// Completes the stored receiver/handler with `set_stopped()`
  /// semantics, from the context's dispatch path.
  virtual void complete_stopped() noexcept = 0;

  /// Registration race guard: returns true (once) when a stop request
  /// arrived before the operation became visible to the context. Checked
  /// by the context under its mutex right after queueing.
  virtual bool take_stop_request() noexcept = 0;

  /// Cancel request against an already-written operation. Returns true
  /// when the operation handled it (IDLE queued `DONE` and awaits its
  /// tagged reply); false selects detach/drop-on-arrival.
  [[nodiscard]] virtual bool cancel_written() noexcept = 0;

  /// Marks the operation detached: its tagged reply is dropped on
  /// arrival and the write pump retires the cell after the in-flight
  /// staged write completes.
  virtual void mark_detached() noexcept = 0;

  /// True after `mark_detached()`.
  [[nodiscard]] virtual bool is_detached() const noexcept = 0;

  /// Binds the context sink and stamps the command tag. Called exactly
  /// once, at registration, before the cell enters the queue.
  virtual void attach(operation_sink<Allocator>* sink,
                      std::string_view tag) = 0;

  /// The stamped tag (registry key). Valid after `attach()`.
  [[nodiscard]] virtual std::string_view tag() const noexcept = 0;

  /// Destroys and deallocates the cell with the allocator it was
  /// allocated with. Invoked through `op_deleter`.
  virtual void dispose() noexcept = 0;
};

/**
 * Stateless deleter for `std::unique_ptr<operation_base<Allocator>>`:
 * destruction is routed through the virtual `dispose()` so the concrete
 * cell frees itself with its own rebound allocator.
 */
template <class Allocator = std::allocator<std::byte>>
struct op_deleter {
  void operator()(operation_base<Allocator>* op) const noexcept {
    op->dispose();
  }
};

/**
 * Concrete operation cell: owns the command and the completion target.
 *
 * `HandlerOrReceiver` is either a callback invocable as
 * `f(std::error_code, result_type)` (or `f(std::error_code)` for `void`
 * results) — the callback path — or a bexec receiver accepting
 * `set_value(std::error_code[, result_type])` / `set_stopped()` — the
 * sender path. The callback path maps stop-cancellation to
 * `std::errc::operation_canceled`, since plain handlers have no stopped
 * channel.
 *
 * Exactly one terminal delivery (`on_tagged` / `fail` /
 * `complete_stopped`) wins, guarded by an atomic exchange; the stop
 * callback is unregistered before delivery and its destructor blocks
 * until an in-flight `request_stop()` returns, so no path can touch the
 * cell after delivery started.
 */
template <class Command, class HandlerOrReceiver>
class operation_model final
    : public operation_base<typename Command::allocator_type> {
 public:
  using command_type = Command;
  using result_type = typename Command::result_type;
  using allocator_type = typename Command::allocator_type;
  using base = operation_base<allocator_type>;
  using string_type = string_of<allocator_type>;

  static constexpr bool kVoidResult = std::is_void_v<result_type>;

  /// Callback path: target invocable with (ec[, result]).
  static constexpr bool kCallbackPath =
      kVoidResult
          ? std::invocable<HandlerOrReceiver, std::error_code>
          : std::invocable<HandlerOrReceiver, std::error_code, result_type>;

  static_assert(
      kCallbackPath ||
          (kVoidResult
               ? requires(HandlerOrReceiver r,
                          std::error_code ec) { std::move(r).set_value(ec); }
               : requires(HandlerOrReceiver r, std::error_code ec,
                          result_type
                              v) { std::move(r).set_value(ec, std::move(v)); }),
      "HandlerOrReceiver must be an invocable callback or a bexec receiver");

  operation_model(Command command, HandlerOrReceiver target,
                  const allocator_type& alloc = allocator_type{})
      : command_(std::move(command)),
        target_(std::move(target)),
        cell_alloc_(alloc),
        tag_(alloc) {}

  operation_model(const operation_model&) = delete;
  operation_model& operator=(const operation_model&) = delete;
  ~operation_model() override = default;

  // --- write side ----------------------------------------------------

  [[nodiscard]] bnio::const_buffer next_segment() noexcept override {
    if (segment_staged_) {
      return {};
    }
    return command_.next_segment();
  }

  [[nodiscard]] bool awaits_continuation() const noexcept override {
    return command_.awaits_continuation();
  }

  [[nodiscard]] bool blocks_pipeline() const noexcept override {
    if constexpr (requires { command_.blocks_pipeline(); }) {
      return command_.blocks_pipeline();
    } else {
      return false;
    }
  }

  void on_segment_staged() noexcept override { segment_staged_ = true; }

  void on_segment_flushed() noexcept override {
    segment_staged_ = false;
    command_.on_segment_flushed();
  }

  [[nodiscard]] bool is_written() const noexcept override {
    // Live check: fully written means nothing staged right now, not
    // parked on a continuation, and no bytes left to render. A fresh
    // operation (segment pending) and a mid-handshake operation
    // (awaiting a continuation) are both NOT written; an idling IDLE
    // is.
    return !segment_staged_ && !command_.awaits_continuation() &&
           command_.next_segment().size() == 0;
  }

  [[nodiscard]] bool is_staged() const noexcept override {
    return segment_staged_;
  }

  // --- read side -----------------------------------------------------

  void on_continuation(std::string_view text) noexcept override {
    command_.on_continuation(text);
  }

  void on_untagged(
      const untagged_response<allocator_type>& r) noexcept override {
    command_.on_untagged(r);
  }

  [[nodiscard]] bool wants_untagged(
      untagged_kind kind) const noexcept override {
    // Commands that consume command-scoped data declare their kinds with
    // a `wants_untagged(kind)` member; everything else declines, so a
    // pipelined data line is never absorbed by an unrelated command.
    if constexpr (requires {
                    {
                      command_.wants_untagged(kind)
                    } -> std::convertible_to<bool>;
                  }) {
      return command_.wants_untagged(kind);
    } else {
      return false;
    }
  }

  void on_tagged(tagged_response<allocator_type> r) noexcept override {
    // A cancelled IDLE-style operation still receives its tagged reply
    // after DONE; completion is then on the stopped channel.
    if (stopped_.load(std::memory_order_acquire)) {
      deliver_stopped();
      return;
    }
    if constexpr (kVoidResult) {
      deliver_value(command_.on_tagged(r));
    } else {
      // The command fills `out` only on success; on NO/BAD the default
      // result rides alongside the error code.
      result_type out{};
      deliver_value(command_.on_tagged(r, out), std::move(out));
    }
  }

  // --- control -------------------------------------------------------

  void fail(std::error_code ec) noexcept override {
    if constexpr (kVoidResult) {
      deliver_value(ec);
    } else {
      deliver_value(ec, result_type{});
    }
  }

  void request_stop() noexcept override {
    // Record first, then notify: a stop that races registration is
    // observed by take_stop_request() under the context mutex.
    stop_seen_.store(true, std::memory_order_release);
    if (finished_.load(std::memory_order_acquire)) {
      return;
    }
    if (sink_ != nullptr) {
      sink_->on_stop_requested(this);
    }
  }

  void complete_stopped() noexcept override { deliver_stopped(); }

  bool take_stop_request() noexcept override {
    return stop_seen_.exchange(false, std::memory_order_acq_rel);
  }

  [[nodiscard]] bool cancel_written() noexcept override {
    if constexpr (requires { command_.done(); }) {
      command_.done();
      stopped_.store(true, std::memory_order_release);
      return true;
    } else {
      return false;
    }
  }

  void mark_detached() noexcept override { detached_ = true; }

  [[nodiscard]] bool is_detached() const noexcept override { return detached_; }

  void attach(operation_sink<allocator_type>* sink,
              std::string_view tag) override {
    sink_ = sink;
    tag_.assign(tag.data(), tag.size());
    command_.render(tag_);
    // Sender path: honour the receiver's stop token end to end. The
    // callback path (plain handler) has no env, so the token is
    // never_stop_token and the registration is an inert no-op.
    stop_callback_.emplace(bexec::get_stop_token(bexec::get_env(target_)),
                           stop_forwarder{this});
  }

  [[nodiscard]] std::string_view tag() const noexcept override { return tag_; }

  void dispose() noexcept override {
    using model_alloc = rebind_alloc_t<allocator_type, operation_model>;
    model_alloc alloc(cell_alloc_);
    this->~operation_model();
    std::allocator_traits<model_alloc>::deallocate(alloc, this, 1);
  }

 private:
  // The stop callback invokes request_stop() on the requesting thread.
  struct stop_forwarder {
    operation_model* self;
    void operator()() const noexcept { self->request_stop(); }
  };

  using env_type =
      decltype(bexec::get_env(std::declval<const HandlerOrReceiver&>()));
  using stop_token_type = std::remove_cvref_t<decltype(bexec::get_stop_token(
      std::declval<env_type>()))>;
  using stop_callback_type =
      typename stop_token_type::template callback_type<stop_forwarder>;

  /// One-shot guard: exactly one terminal delivery wins, no matter how
  /// many paths (tagged reply, fail, stop, cancel) race for it.
  bool begin_delivery() noexcept {
    if (finished_.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    // Unregister the stop callback before completing; its destructor
    // blocks until an in-flight invocation returns, so no request_stop()
    // can reference this object after delivery starts.
    stop_callback_.reset();
    return true;
  }

  template <class... Args>
  void deliver_value(std::error_code ec, Args&&... args) noexcept {
    if (!begin_delivery()) {
      return;
    }
    if constexpr (kCallbackPath) {
      if constexpr (kVoidResult) {
        std::invoke(std::move(target_), ec);
      } else {
        std::invoke(std::move(target_), ec, std::forward<Args>(args)...);
      }
    } else {
      if constexpr (kVoidResult) {
        bexec::set_value(std::move(target_), ec);
      } else {
        bexec::set_value(std::move(target_), ec, std::forward<Args>(args)...);
      }
    }
  }

  void deliver_stopped() noexcept {
    if (!begin_delivery()) {
      return;
    }
    if constexpr (kCallbackPath) {
      // usage.md §3.3: "ctx.cancel(tag) withdraws a handler" — on the
      // callback path a cancelled operation NEVER invokes its handler
      // (there is no stopped channel to report through).
    } else {
      bexec::set_stopped(std::move(target_));
    }
  }

  Command command_;
  HandlerOrReceiver target_;
  operation_sink<allocator_type>* sink_ = nullptr;
  // Rebound copy of the construction allocator, used by dispose().
  [[no_unique_address]] allocator_type cell_alloc_;
  string_type tag_;
  // The model is pinned, so the immovable callback may live in place.
  std::optional<stop_callback_type> stop_callback_;
  // Accessed only under the context mutex (pump and cancel paths).
  bool segment_staged_ = false;
  bool detached_ = false;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> stop_seen_{false};
  std::atomic<bool> finished_{false};
};

/**
 * Allocates an operation cell through `alloc` (rebound to `Model`) and
 * returns it as the queue's owning unique_ptr. This is the one heap
 * allocation of the type-erasure boundary (architecture §3.3/§7).
 */
template <class Model, class Allocator, class... Args>
[[nodiscard]] std::unique_ptr<operation_base<typename Model::allocator_type>,
                              op_deleter<typename Model::allocator_type>>
allocate_operation(const Allocator& alloc, Args&&... args) {
  using allocator_type = typename Model::allocator_type;
  using model_alloc = rebind_alloc_t<allocator_type, Model>;
  model_alloc model_allocator(alloc);
  Model* cell =
      std::allocator_traits<model_alloc>::allocate(model_allocator, 1);
  try {
    std::allocator_traits<model_alloc>::construct(model_allocator, cell,
                                                  std::forward<Args>(args)...,
                                                  allocator_type(alloc));
  } catch (...) {
    std::allocator_traits<model_alloc>::deallocate(model_allocator, cell, 1);
    throw;
  }
  return std::unique_ptr<operation_base<allocator_type>,
                         op_deleter<allocator_type>>(cell);
}

// ---------------------------------------------------------------------
// Self-owning I/O operation boxes
//
// A connected bnio/bexec operation state is pinned and must stay alive
// until its completion has been delivered. The pumps therefore allocate
// one heap box per armed I/O; the receiver stored inside the state
// carries the box address and calls `dispose()` as the very last action
// of its terminal callback. This is safe because every bnio/bexec
// executor treats receiver delivery as the final use of an operation:
// workers unlink the intrusive node before `execute()`, and
// `execute()`'s last action is the receiver call (bnio posix io_context
// worker loop, bexec run_loop).

/// Polymorphic handle for a heap-allocated connected operation state.
class io_box_base {
 public:
  io_box_base() = default;
  io_box_base(const io_box_base&) = delete;
  io_box_base& operator=(const io_box_base&) = delete;
  virtual ~io_box_base() = default;

  /// Starts the held operation state (exactly once, right after
  /// construction).
  virtual void start() noexcept = 0;

  /// Destroys and deallocates the box. Must be the last action of the
  /// receiver's terminal callback.
  virtual void dispose() noexcept = 0;
};

/// Concrete self-owning box: the connected operation state lives in
/// place (pinned, never moved).
template <class OpState, class Allocator>
class io_box final : public io_box_base {
 public:
  template <class Sender, class Receiver>
  io_box(Sender&& sender, Receiver receiver, const Allocator& alloc)
      : state_(
            bexec::connect(std::forward<Sender>(sender), std::move(receiver))),
        alloc_(alloc) {}

  io_box(const io_box&) = delete;
  io_box& operator=(const io_box&) = delete;
  ~io_box() override = default;

  void start() noexcept override { bexec::start(state_); }

  void dispose() noexcept override {
    using box_alloc = rebind_alloc_t<Allocator, io_box>;
    box_alloc rebound(alloc_);
    this->~io_box();
    std::allocator_traits<box_alloc>::deallocate(rebound, this, 1);
  }

 private:
  OpState state_;
  [[no_unique_address]] Allocator alloc_;
};

/**
 * Allocates a self-owning I/O box for `connect(sender, receiver)`.
 * `make_receiver` is invoked with the (stable) box address to produce the
 * receiver stored inside the operation state. The receiver's terminal
 * callback must end with `box->dispose()`.
 */
template <class Allocator, class Sender, class MakeReceiver>
[[nodiscard]] io_box_base* make_io_box(const Allocator& alloc, Sender&& sender,
                                       MakeReceiver&& make_receiver) {
  using receiver_type = std::invoke_result_t<MakeReceiver, io_box_base*>;
  using state_type = decltype(bexec::connect(std::forward<Sender>(sender),
                                             std::declval<receiver_type>()));
  using box_type = io_box<state_type, Allocator>;
  using box_alloc = rebind_alloc_t<Allocator, box_type>;
  box_alloc rebound(alloc);
  box_type* mem = std::allocator_traits<box_alloc>::allocate(rebound, 1);
  try {
    std::allocator_traits<box_alloc>::construct(
        rebound, mem, std::forward<Sender>(sender),
        std::forward<MakeReceiver>(make_receiver)(
            static_cast<io_box_base*>(mem)),
        alloc);
  } catch (...) {
    std::allocator_traits<box_alloc>::deallocate(rebound, mem, 1);
    throw;
  }
  return mem;
}

/// Minimal compile-time contract check for command types; the full
/// requirement list is documented in the header banner.
template <class C>
concept command_like = requires {
  typename C::result_type;
  typename C::allocator_type;
};

/// Callback-path predicate: F is invocable as f(ec[, result]).
template <class F, class Command>
concept handler_for =
    command_like<Command> &&
    (std::is_void_v<typename Command::result_type>
         ? std::invocable<F, std::error_code>
         : std::invocable<F, std::error_code, typename Command::result_type>);

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_OPERATION_BASE_H_
