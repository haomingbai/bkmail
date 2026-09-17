/**
 * @file include/bkmail/imap/detail/write_pump.h
 * @brief Write-path algorithm: staging buffer, batching drain, literal
 *        wall, MSG_NOSIGNAL write chain.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details [self-built] friend-scoped algorithm carrier for
 * `imap_context` (code_layout §4): the pump owns no state — the queue,
 * staging buffer and flags live in `detail::context_core` — it owns the
 * write-path algorithm. The pump's receivers hold a shared_ptr to the
 * core, so the core (stream, staging buffer, queue) stays alive while a
 * write is in flight even when the imap_context shell was already
 * destroyed (usage.md §7.4); the core's `abandoned_` flag then switches
 * this pump into its quiet teardown form (cells retired silently, no
 * handler invocation).
 *
 * Algorithm (architecture §3.4):
 *
 *  - Registration of a write happens when an operation is added to an
 *    idle queue: the submit path posts `kick()` through the scheduler, so
 *    a batch of submissions made before the worker runs is staged into a
 *    single `async_write`. While a write is in flight, new arrivals are
 *    picked up by the completion handler chaining the next drain.
 *  - `kick()` copies one segment per queued operation into the
 *    contiguous staging buffer (bnio has no scatter/gather write) and
 *    arms one write-all `async_write` with `MSG_NOSIGNAL`.
 *  - The staging buffer is only mutated while no write is in flight, so
 *    the pointer captured by the in-flight sender stays valid; queue
 *    elements are `unique_ptr`-owned, so vector reallocation never moves
 *    an operation (buffer stability, architecture §2.5).
 *  - An operation paused on a continuation request (literal/SASL) is a
 *    wall: nothing behind it is staged (RFC 3501 §5.5 wire order).
 *  - A write error is connection-fatal: it funnels into
 *    `context_core::connection_lost`.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_WRITE_PUMP_H_
#define BKMAIL_IMAP_DETAIL_WRITE_PUMP_H_

#include <bkmail/imap/operation_base.h>
#include <bnio/buffer/basic.h>
#include <bnio/io_context.h>
#include <sys/socket.h>

#include <bexec/scheduler.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

namespace bkmail::imap::detail {

template <class Stream, class Allocator>
class context_core;

template <class Stream, class Allocator = std::allocator<std::byte>>
class write_pump {
 public:
  using core_type = context_core<Stream, Allocator>;
  using operation_type = operation_base<Allocator>;

  /**
   * Arms the write pump through the scheduler when the queue holds
   * unsubmitted work and no write/kick is outstanding. Batching hinge:
   * posting (instead of draining inline) lets co-arriving submissions
   * share one write syscall.
   */
  static void post_kick(std::shared_ptr<core_type> core) {
    {
      std::lock_guard lock(core->mutex_);
      if (core->phase_ != core_type::phase::open || core->write_in_flight_ ||
          core->kick_posted_ || core->write_queue_.empty()) {
        return;
      }
      core->kick_posted_ = true;
    }
    try {
      // Materialize the allocator and the sender before the receiver
      // factory: capturing `core` by move initializes the lambda as an
      // argument of the same call, and with an unspecified argument
      // evaluation order the other arguments would dereference a
      // moved-from (null) shared_ptr. The lambda copies the shared_ptr
      // so the catch path below still owns the core.
      auto alloc = core->get_allocator();
      auto sender = bexec::schedule(core->scheduler());
      auto* box = make_io_box(
          std::move(alloc), std::move(sender),
          [core](io_box_base* self) mutable {
            return kick_receiver(self, std::move(core));
          });
      box->start();
    } catch (...) {
      // Allocation failure: unlatch so a later submission re-kicks; the
      // queued operations stay pending.
      std::lock_guard lock(core->mutex_);
      core->kick_posted_ = false;
    }
  }

  /**
   * Drains the write queue into the staging buffer and arms one batched
   * write. Runs on the scheduler thread: from a posted kick, or chained
   * directly from a write completion.
   */
  static void kick(core_type& ctx) noexcept {
    std::vector<operation_type*, rebind_alloc_t<Allocator, operation_type*>>
        staged{rebind_alloc_t<Allocator, operation_type*>(ctx.get_allocator())};
    {
      std::lock_guard lock(ctx.mutex_);
      if (ctx.phase_ != core_type::phase::open || ctx.write_in_flight_) {
        return;
      }
      ctx.write_staging_.clear();
      for (auto& cell : ctx.write_queue_) {
        operation_type* op = cell.get();
        if (op->is_written()) {
          continue;
        }
        const bnio::const_buffer segment = op->next_segment();
        if (segment.size() == 0) {
          // Paused on a continuation request: hard wire-order wall.
          // (An operation with no bytes and no wall simply has nothing
          // to send this round.)
          if (op->awaits_continuation()) {
            break;
          }
          continue;
        }
        const auto* first = static_cast<const char*>(segment.data());
        ctx.write_staging_.insert(ctx.write_staging_.end(), first,
                                  first + segment.size());
        op->on_segment_staged();
        staged.push_back(op);
        // Staged the "{n}\r\n" line of a literal operation, or a
        // pipeline-blocking command (AUTHENTICATE / LOGOUT / STARTTLS):
        // the wall drops behind it.
        if (op->awaits_continuation() || op->blocks_pipeline()) {
          break;
        }
      }
      if (ctx.write_staging_.empty()) {
        return;
      }
      // Latch before arming: the staging buffer must not be touched
      // while the write is in flight.
      ctx.write_in_flight_ = true;
    }
    try {
      arm(ctx.shared_from_this(), std::move(staged));
    } catch (...) {
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.write_in_flight_ = false;
      }
      ctx.connection_lost(std::make_error_code(std::errc::not_enough_memory));
    }
  }

 private:
  /// Receiver of the posted schedule: entry point of a batched drain.
  class kick_receiver {
   public:
    kick_receiver(io_box_base* box, std::shared_ptr<core_type> core) noexcept
        : box_(box), core_(std::move(core)) {}

    void set_value(std::error_code ec) noexcept {
      io_box_base* box = box_;
      core_type* core = core_.get();
      {
        std::lock_guard lock(core->mutex_);
        core->kick_posted_ = false;
      }
      // A stopping io_context reports operation_canceled here; the close
      // protocol owns teardown in that case, so only a clean schedule
      // drains. An abandoned core never drains: the queue was dropped.
      if (!ec && !core->abandoned_.load(std::memory_order_acquire)) {
        kick(*core);
      }
      core_.reset();
      box->dispose();
    }

    void set_stopped() noexcept {
      {
        std::lock_guard lock(core_->mutex_);
        core_->kick_posted_ = false;
      }
      core_.reset();
      box_->dispose();
    }

   private:
    io_box_base* box_;
    std::shared_ptr<core_type> core_;
  };

  /// Receiver of the batched async_write: notifies staged operations and
  /// chains the next drain.
  class write_receiver {
   public:
    write_receiver(
        io_box_base* box, std::shared_ptr<core_type> core,
        std::vector<operation_type*, rebind_alloc_t<Allocator, operation_type*>>
            staged) noexcept
        : box_(box), core_(std::move(core)), staged_(std::move(staged)) {}

    void set_value(std::error_code ec, std::size_t n) noexcept {
      io_box_base* box = box_;
      core_type* core = core_.get();
      on_write_done(*core, ec, n, staged_);
      core_.reset();
      box->dispose();
    }

    void set_stopped() noexcept {
      set_value(std::make_error_code(std::errc::operation_canceled), 0);
    }

   private:
    io_box_base* box_;
    std::shared_ptr<core_type> core_;
    std::vector<operation_type*, rebind_alloc_t<Allocator, operation_type*>>
        staged_;
  };

  /// Arms the single write-all operation over the staging buffer.
  ///
  /// Runs entirely under the core mutex: the phase decision and the
  /// registration of the armed write (the scripted stream records the
  /// armed write inside start()) form one critical section, so a
  /// concurrent abandon() either observes the armed write in its
  /// teardown delivery or wins the mutex first and this arm unwinds
  /// instead — the teardown completion is delivered exactly once under
  /// every interleaving. The mutex is recursive because an
  /// eagerly-completed write re-enters on_write_done on the same thread.
  static void arm(
      std::shared_ptr<core_type> core,
      std::vector<operation_type*, rebind_alloc_t<Allocator, operation_type*>>
          staged) {
    using cell_type = typename core_type::cell_type;
    std::lock_guard lock(core->mutex_);
    if (core->phase_ != core_type::phase::open ||
        core->abandoned_.load(std::memory_order_acquire)) {
      // Teardown won the race between kick()'s decision and this arm:
      // the staged cells were kept in the queue for this write's
      // completion, which will now never happen — retire them the way
      // the write completion would have. During abandonment they are
      // destroyed silently (handlers dropped, not invoked); during a
      // close() drain they are failed with the cancellation code, like
      // every other queued operation.
      const bool abandoned =
          core->abandoned_.load(std::memory_order_acquire);
      std::vector<cell_type, rebind_alloc_t<Allocator, cell_type>> retired{
          rebind_alloc_t<Allocator, cell_type>(core->get_allocator())};
      for (auto* op : staged) {
        if (auto cell = core->extract_from_queue_locked(op)) {
          retired.push_back(std::move(cell));
        }
      }
      core->write_in_flight_ = false;
      if (abandoned) {
        retired.clear();
      } else {
        for (auto& cell : retired) {
          cell->fail(std::make_error_code(std::errc::operation_canceled));
        }
        retired.clear();
      }
      core->maybe_finish_close();
      return;
    }
    // Materialize the allocator and the sender before the receiver
    // factory: capturing `core` by move initializes the lambda as an
    // argument of the same call, and with an unspecified argument
    // evaluation order the other arguments would dereference a
    // moved-from (null) shared_ptr. Both also re-enter the core mutex,
    // which the recursive lock permits.
    auto alloc = core->get_allocator();
    auto sender = core->stream_.async_write(
        core->scheduler(),
        bnio::const_buffer(core->write_staging_.data(),
                           core->write_staging_.size()),
        MSG_NOSIGNAL);
    auto* box = make_io_box(
        std::move(alloc), std::move(sender),
        [core = std::move(core),
         staged = std::move(staged)](io_box_base* self) mutable {
          return write_receiver(self, std::move(core), std::move(staged));
        });
    box->start();
  }

  /// Write completion: flush notifications, continuation-target
  /// bookkeeping, detached/staged-cell retirement, then chain, go idle,
  /// or join the teardown.
  static void on_write_done(
      core_type& ctx, std::error_code ec, std::size_t /*n*/,
      const std::vector<operation_type*,
                        rebind_alloc_t<Allocator, operation_type*>>&
          staged) noexcept {
    using cell_type = typename core_type::cell_type;
    // Cells this completion retires. The write is done, so the staged
    // list was the last pointer the pump held into them.
    std::vector<cell_type, rebind_alloc_t<Allocator, cell_type>> retired{
        rebind_alloc_t<Allocator, cell_type>(ctx.get_allocator())};

    if (ec) {
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.write_in_flight_ = false;
        // Lift the staged cells out first: connection_lost fails the
        // rest of the queue and must not destroy cells this write
        // referenced.
        for (auto* op : staged) {
          if (auto cell = ctx.extract_from_queue_locked(op)) {
            retired.push_back(std::move(cell));
          }
        }
      }
      if (ctx.abandoned_.load(std::memory_order_acquire)) {
        // Shell destroyed: no failure delivery, only bookkeeping.
        // (maybe_finish_close takes the mutex itself; call it unlocked.)
        {
          std::lock_guard lock(ctx.mutex_);
          retired.clear();
        }
        ctx.maybe_finish_close();
        return;
      }
      // Write failure is connection-fatal.
      ctx.connection_lost(ec);
      for (auto& cell : retired) {
        cell->fail(ec);
      }
      {
        std::lock_guard lock(ctx.mutex_);
        retired.clear();
      }
      ctx.maybe_finish_close();
      return;
    }

    if (ctx.abandoned_.load(std::memory_order_acquire)) {
      // Shell destroyed (usage.md §7.4): retire the staged cells
      // silently and finish the close protocol; no flush notifications.
      {
        std::lock_guard lock(ctx.mutex_);
        ctx.write_in_flight_ = false;
        for (auto* op : staged) {
          if (auto cell = ctx.extract_from_queue_locked(op)) {
            retired.push_back(std::move(cell));
          }
        }
        retired.clear();
      }
      ctx.maybe_finish_close();
      return;
    }

    bool open;
    {
      std::lock_guard lock(ctx.mutex_);
      ctx.write_in_flight_ = false;
      open = (ctx.phase_ == core_type::phase::open);
      if (open) {
        for (auto* op : staged) {
          // A tagged reply dispatched BEFORE this write completion (bnio
          // delivers read and write events in no guaranteed order, and the
          // server may already have answered) retires its cell first, so
          // the raw staged pointer must be re-validated against the queue.
          if (!ctx.queue_contains_locked(op)) {
            continue;
          }
          op->on_segment_flushed();
          // The {n} line just hit the wire: this operation is now the
          // single continuation route (at most one per drain, since the
          // wall stops staging behind it).
          if (op->awaits_continuation()) {
            ctx.continuation_target_ = op;
          }
          // Detached while mid-handshake or in flight: retire once the
          // operation is fully written (a parked operation renders its
          // remaining segments detached so the wire stays consistent).
          if (op->is_detached() && op->is_written()) {
            if (auto cell = ctx.extract_from_queue_locked(op)) {
              retired.push_back(std::move(cell));
            }
          }
        }
      } else {
        // Draining: fail the staged cells; the rest of the queue is
        // owned by the close protocol.
        for (auto* op : staged) {
          if (auto cell = ctx.extract_from_queue_locked(op)) {
            retired.push_back(std::move(cell));
          }
        }
      }
    }
    if (open) {
      for (auto& cell : retired) {
        ctx.post_complete_stopped(std::move(cell));
      }
      // Chain the next batched write immediately from the completion.
      kick(ctx);
      return;
    }
    for (auto& cell : retired) {
      cell->fail(std::make_error_code(std::errc::operation_canceled));
    }
    {
      std::lock_guard lock(ctx.mutex_);
      retired.clear();
    }
    // Draining: the close protocol finishes the teardown once the read
    // side is also quiet.
    ctx.maybe_finish_close();
  }
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_WRITE_PUMP_H_
