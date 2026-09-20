/**
 * @file include/bkmail/imap/detail/read_pump.h
 * @brief Read-path algorithm: permanent async_read_some loop, lexer
 *        drive, dispatch into registry + unsolicited table.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details [self-built] friend-scoped algorithm carrier for
 * `imap_context` (code_layout §4): the pump owns no state — the read
 * buffer, lexer and registry live in `detail::context_core` — it owns
 * the read-path algorithm. The pump's receivers hold a shared_ptr to the
 * core, so the core (stream, buffer, registry) stays alive while a read
 * is in flight even when the imap_context shell was already destroyed
 * (usage.md §7.4); the core's `abandoned_` flag then switches this pump
 * into its quiet teardown form (no dispatch, no handler invocation).
 *
 * Model (architecture §3.5): exactly one `async_read_some` is armed while
 * the connection is alive (bnio's `async_read` is read-all and must not
 * be used for the permanent loop). Each completion commits the bytes,
 * drives the two-mode `response_lexer` over the committed range, parses
 * and dispatches every complete response, then re-arms — unless the
 * context is draining, closed, abandoned, or suspended for relocation.
 *
 * Dispatch rules (architecture §3.5):
 *
 *  - tagged: ownership of the operation cell is moved out of the queue
 *    and the registry BEFORE `on_tagged` runs, so the object outlives
 *    its own completion; an unknown tag is a detached (cancelled)
 *    operation whose reply is dropped on arrival.
 *  - untagged: first the unsolicited table (whole-variant event), then
 *    every registered operation's `on_untagged` under the context lock
 *    (command-internal code only, never user code).
 *  - continuation: routed to the single `continuation_target_` (which
 *    write_pump::kick registers at staging time, before any response to
 *    the staged bytes can be dispatched); a continuation with no waiter
 *    is a protocol violation and fails the connection. After the target
 *    renders its literal bytes the write pump is kicked.
 *
 * An untagged BYE is connection-fatal but not immediately: the server
 * may still deliver a pending tagged reply (LOGOUT). The pump records
 * the BYE and maps the inevitable EOF to `errc::server_bye`.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_READ_PUMP_H_
#define BKMAIL_IMAP_DETAIL_READ_PUMP_H_

#include <bkmail/common/error.h>
#include <bkmail/imap/detail/response_lexer.h>
#include <bkmail/imap/detail/response_parser.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bkmail/imap/unsolicited_event.h>
#include <bnio/buffer/dynamic_byte_vector.h>
#include <bnio/io_context.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace bkmail::imap::detail {

template <class Stream, class Allocator>
class context_core;

template <class Stream, class Allocator = std::allocator<std::byte>>
class read_pump {
 public:
  /// Bytes requested per armed read.
  static constexpr std::size_t kReadChunk = 8192;

  using core_type = context_core<Stream, Allocator>;
  using operation_type = operation_base<Allocator>;

  /**
   * Arms the single permanent read. Call only while no read is armed:
   * `prepare()` reallocates the read storage and must never race an
   * in-flight read (buffer stability, architecture §2.5).
   *
   * Runs entirely under the core mutex: the phase decision and the
   * registration of the armed operation (the scripted stream records the
   * armed read inside start()) form one critical section, so a
   * concurrent abandon() either observes the armed read in its teardown
   * delivery or wins the mutex first and this arm unwinds instead — the
   * teardown completion is delivered exactly once under every
   * interleaving, with no retries and no generation tracking. The mutex
   * is recursive exactly because an eagerly-completed arm re-enters
   * on_read on the same thread.
   */
  static void arm(std::shared_ptr<core_type> core) {
    std::lock_guard lock(core->mutex_);
    if (core->phase_ != core_type::phase::open ||
        core->abandoned_.load(std::memory_order_acquire) ||
        core->suspend_read_) {
      // Teardown won the race: wind the pump down instead of arming.
      core->read_running_ = false;
      core->maybe_finish_close();
      return;
    }
    // start_io_box materializes the sender before the receiver factory
    // runs (the sequencing hazard the wrapper exists for). The receiver
    // factory copy-captures `core` and moves at invocation time: an
    // init-capture move would already run at this call site, before
    // make_sender dereferences the pump's shared_ptr.
    start_io_box(
        core->get_allocator(),
        [&core] {
          return core->stream_.async_read_some(
              core->scheduler(), core->read_buffer_.prepare(kReadChunk), 0);
        },
        [core](io_box_base* self) mutable {
          return read_receiver(self, std::move(core));
        });
  }

 private:
  /// Receiver of the permanent read. The shared_ptr keeps the core (and
  /// with it the stream and the read storage) alive until the completion
  /// has been delivered, independent of the shell's lifetime.
  class read_receiver {
   public:
    read_receiver(io_box_base* box, std::shared_ptr<core_type> core) noexcept
        : box_(box), core_(std::move(core)) {}

    void set_value(std::error_code ec, std::size_t n) noexcept {
      io_box_base* box = box_;
      core_type* core = core_.get();
      // The teardown-vs-dispatch decision is made under the core mutex,
      // serialized with abandon()/close() exactly like the arm paths
      // (the recursive mutex re-enters fine when an eagerly-completed
      // read reaches this from arm()). Loading `abandoned_` unlocked
      // was a check-then-act race: the flag and the phase it gates are
      // only coherent together under the lock.
      bool dispatch;
      {
        std::lock_guard lock(core->mutex_);
        dispatch = !core->abandoned_.load(std::memory_order_acquire) &&
                   core->phase_ != core_type::phase::closed;
        if (!dispatch) {
          // Shell destroyed (usage.md §7.4): no dispatch, no handlers —
          // only the pump bookkeeping so the close protocol can finish.
          core->read_running_ = false;
        }
      }
      if (dispatch) {
        // on_read manages its own locking; a draining context still
        // reaches its error path, which is what fails the written
        // operations the close protocol promised to fail.
        on_read(*core, ec, n);
      } else {
        // (maybe_finish_close takes the mutex itself; call it unlocked.)
        core->maybe_finish_close();
      }
      core_.reset();
      box->dispose();
    }

    void set_stopped() noexcept {
      set_value(std::make_error_code(std::errc::operation_canceled), 0);
    }

   private:
    io_box_base* box_;
    std::shared_ptr<core_type> core_;
  };

  /// Read completion: feed the lexer, dispatch, re-arm or wind down.
  static void on_read(core_type& ctx, std::error_code ec,
                      std::size_t n) noexcept {
    if (ec || n == 0) {
      // n == 0 is EOF (architecture §2.3): an orderly close after BYE
      // maps to server_bye; an unannounced EOF is a protocol violation.
      // The mapping is decided under the lock: a recorded BYE is the
      // server's announced reason for the connection ending, so it wins
      // over any transport-level error (RST included).
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.read_running_ = false;
        if (ctx.bye_received_) {
          ec = make_error_code(errc::server_bye);
        } else if (!ec) {
          ec = make_error_code(errc::unexpected_response);
        }
      }
      ctx.connection_lost(ec);
      return;
    }

    ctx.read_buffer_.commit(n);
    bool dispatch_ok = true;
    for (;;) {
      // Two-call lexer protocol: the extracted view aliases the buffer
      // and is dropped by consume_response() after dispatch (the parsed
      // payload views share that lifetime).
      std::optional<std::string_view> text =
          ctx.lexer_.next_complete_response(ctx.read_buffer_);
      if (!text.has_value()) {
        break;
      }
      const bool ok = dispatch(ctx, *text);
      ctx.lexer_.consume_response(ctx.read_buffer_);
      if (!ok) {
        // Fatal protocol violation: connection_lost already ran.
        dispatch_ok = false;
        break;
      }
    }

    if (!dispatch_ok) {
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.read_running_ = false;
      }
      ctx.maybe_finish_close();
      return;
    }

    // Re-arm while open and not suspended for relocation.
    bool arm_again;
    {
      std::lock_guard lock(ctx.mutex_);
      arm_again = (ctx.phase_ == core_type::phase::open && !ctx.suspend_read_);
      if (!arm_again) {
        ctx.read_running_ = false;
      }
    }
    if (!arm_again) {
      ctx.maybe_finish_close();
      return;
    }
    try {
      arm(ctx.shared_from_this());
    } catch (...) {
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.read_running_ = false;
      }
      ctx.connection_lost(std::make_error_code(std::errc::not_enough_memory));
    }
  }

  /// Parses and dispatches one complete response text. Returns false on
  /// a connection-fatal protocol violation (connection_lost already run).
  static bool dispatch(core_type& ctx, std::string_view text) {
    std::optional<server_response<Allocator>> parsed = ctx.parser_.parse(text);
    if (!parsed.has_value()) {
      ctx.connection_lost(make_error_code(errc::unexpected_response));
      return false;
    }
    return std::visit(
        [&ctx](auto&& response) {
          return dispatch_one(ctx, std::forward<decltype(response)>(response));
        },
        std::move(*parsed));
  }

  /// tagged: route to the registered operation; detached tags drop. A
  /// tagged reply ALWAYS takes ownership of its cell first — the cell
  /// is extracted from the queue and the registry under the mutex,
  /// whether or not the command's own bytes are still being written —
  /// and `on_tagged` runs outside the lock. Safety: the in-flight write
  /// references only the staging BYTES (`write_staging_`), never the
  /// cell; `on_write_done` re-validates every staged pointer against
  /// the queue (`queue_contains_locked`) and skips extracted cells, so
  /// no flush callback touches an operation already handed to its
  /// completion, and `arm()`'s teardown path goes through
  /// `extract_from_queue_locked`, which returns null for
  /// already-extracted ops.
  static bool dispatch_one(core_type& ctx,
                           tagged_response<Allocator>&& response) {
    using cell_type = typename core_type::cell_type;
    cell_type cell;
    {
      std::lock_guard lock(ctx.mutex_);
      auto it = ctx.registry_.find(response.tag);
      if (it == ctx.registry_.end()) {
        // Detached (cancelled after write): drop on arrival.
        return true;
      }
      operation_type* op = it->second;
      ctx.registry_.erase(it);
      if (ctx.continuation_target_ == op) {
        ctx.continuation_target_ = nullptr;
      }
      cell = ctx.extract_from_queue_locked(op);
    }
    if (cell == nullptr) {
      // Registry/queue divergence is an internal invariant violation.
      ctx.connection_lost(make_error_code(errc::unexpected_response));
      return false;
    }
    // Ownership moved out first: the operation outlives its own
    // completion. The cell is disposed when this frame returns.
    cell->on_tagged(std::move(response));
    return true;
  }

  /// untagged: unsolicited table first; then command-scoped data kinds
  /// (FETCH/LIST/LSUB/STATUS/SEARCH/CAPABILITY/FLAGS) go to the
  /// earliest-queued operation that wants them, while connection-scoped
  /// events and status responses broadcast to every operation.
  static bool dispatch_one(core_type& ctx,
                           untagged_response<Allocator>&& response) {
    if (response.kind == untagged_kind::bye) {
      std::lock_guard lock(ctx.mutex_);
      ctx.bye_received_ = true;
    }
    if (std::optional<unsolicited_event<Allocator>> event =
            make_unsolicited_event(response, ctx.get_allocator())) {
      ctx.unsolicited_.notify(*event);
    }
    // Runs under the lock: on_untagged is command-internal code
    // (contract in operation_base.h), so no user code runs here and the
    // registry cannot be mutated mid-dispatch.
    std::lock_guard lock(ctx.mutex_);
    switch (response.kind) {
      case untagged_kind::fetch:
      case untagged_kind::list:
      case untagged_kind::lsub:
      case untagged_kind::status:
      case untagged_kind::search:
      case untagged_kind::capability:
      case untagged_kind::flags: {
        // Command-scoped data: FIFO attribution (see wants_untagged).
        // write_queue_ is in submission order; a completed operation has
        // already left it, so the earliest waiter is the honest owner.
        for (auto& cell : ctx.write_queue_) {
          if (cell->wants_untagged(response.kind)) {
            cell->on_untagged(response);
            break;
          }
        }
        return true;
      }
      default:
        for (auto& [tag, op] : ctx.registry_) {
          op->on_untagged(response);
        }
        return true;
    }
  }

  /// continuation: single route to the paused operation. The route is
  /// registered at STAGING time (write_pump::kick), so a `+` answering
  /// bytes whose write completion has not been reaped yet still finds
  /// its waiter; a missing waiter is a protocol violation.
  static bool dispatch_one(core_type& ctx,
                           continuation_request<Allocator>&& response) {
    operation_type* target;
    {
      std::lock_guard lock(ctx.mutex_);
      target = ctx.continuation_target_;
      ctx.continuation_target_ = nullptr;
      if (target != nullptr) {
        // Internal command code; runs under the lock so the cell cannot
        // be torn down concurrently by a write-side failure.
        target->on_continuation(response.text);
      }
    }
    if (target == nullptr) {
      // Continuation with no waiter: protocol violation (RFC 3501).
      ctx.connection_lost(make_error_code(errc::unexpected_response));
      return false;
    }
    // The literal/next segment is now renderable: kick the write pump.
    ctx.kick_write_pump();
    return true;
  }

  /**
   * Maps an untagged wire response to the user-facing unsolicited event
   * variant. Kinds with no event counterpart (command-scoped
   * FLAGS/LIST/STATUS/SEARCH data, ...) yield no event.
   */
  static std::optional<unsolicited_event<Allocator>> make_unsolicited_event(
      const untagged_response<Allocator>& response, const Allocator& alloc) {
    switch (response.kind) {
      case untagged_kind::exists:
        return unsolicited_event<Allocator>{
            exists_event{static_cast<std::uint32_t>(response.number)}};
      case untagged_kind::recent:
        return unsolicited_event<Allocator>{
            recent_event{static_cast<std::uint32_t>(response.number)}};
      case untagged_kind::expunge:
        return unsolicited_event<Allocator>{
            expunge_event{static_cast<std::uint32_t>(response.number)}};
      case untagged_kind::fetch:
        // The flag set itself is inside `payload`; the event carries the
        // sequence number, and interested commands/ watchers deep-parse
        // the attributes from the broadcast wire response.
        return unsolicited_event<Allocator>{
            flags_update_event{static_cast<std::uint32_t>(response.number)}};
      case untagged_kind::capability: {
        capability_event<Allocator> event(alloc);
        // `payload` holds the space-separated atom list.
        std::string_view atoms = response.payload;
        while (!atoms.empty()) {
          const auto space = atoms.find(' ');
          const auto atom = atoms.substr(0, space);
          if (!atom.empty()) {
            event.capabilities.insert(atom);
          }
          if (space == std::string_view::npos) {
            break;
          }
          atoms.remove_prefix(space + 1);
        }
        return unsolicited_event<Allocator>{std::move(event)};
      }
      case untagged_kind::bye: {
        bye_event<Allocator> event(alloc);
        event.text = response.text;
        event.code = response.code;
        return unsolicited_event<Allocator>{std::move(event)};
      }
      case untagged_kind::ok:
      case untagged_kind::preauth: {
        // The greeting (first OK/PREAUTH) and later resp-code-carrying
        // untagged OKs share this report; consumers that only care about
        // the greeting simply ignore later ones (usage.md §3.5).
        greeting_event<Allocator> event(alloc);
        event.status = response.status;
        event.text = response.text;
        event.code = response.code;
        return unsolicited_event<Allocator>{std::move(event)};
      }
      default:
        return std::nullopt;
    }
  }
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_READ_PUMP_H_
