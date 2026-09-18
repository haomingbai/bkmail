/**
 * @file include/bkmail/imap/imap_context.h
 * @brief IMAP command/connection context owning the stream.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details Central Layer-1 object (architecture §3.2): owns the stream
 * (the "I/O credential"), allocates command tags, registers operations,
 * runs the read/write pumps, and dispatches responses.
 *
 * Submission model (code_layout D1):
 *
 *  - callback path: `submit(command, handler)` stamps a tag and queues
 *    the command bytes WITHOUT doing I/O; `flush()` performs the batched
 *    write; the handler is invoked from the read loop.
 *  - batch path: `submit(vector<unique_ptr<imap_command<A>>>)` is the
 *    erased form of the callback path.
 *  - sender path: `submit<Operation>(args...)` returns a lazy sender;
 *    its `start()` performs exactly one erased registration (tag
 *    allocated at start, in wire order) and kicks the write pump
 *    through the scheduler, so co-arriving starts batch into one write.
 *
 * Threading (architecture §6): the internal mutex keeps the
 * registry/queue/staging invariants coherent while completion handlers
 * hop worker threads. This is documented for implementers; bkmail makes
 * NO public thread-safety promise — concurrent calls on one context from
 * user threads are unsupported (the write queue in particular, §3.4).
 *
 * Lifetime (usage.md §7.4): destroying an imap_context is the clean way
 * to end a session — pending handlers are dropped, not invoked. To make
 * that safe the whole object state lives in a heap-allocated
 * `detail::context_core` shared with the in-flight pump operations: the
 * shell's destruction only *abandons* the core (close protocol, handlers
 * dropped), and the core destroys itself once both pumps have gone quiet
 * and the last pump operation has retired.
 */

#pragma once
#ifndef BKMAIL_IMAP_IMAP_CONTEXT_H_
#define BKMAIL_IMAP_IMAP_CONTEXT_H_

#include <bkmail/error.h>
#include <bkmail/imap/detail/read_pump.h>
#include <bkmail/imap/detail/response_lexer.h>
#include <bkmail/imap/detail/response_parser.h>
#include <bkmail/imap/detail/submit_sender.h>
#include <bkmail/imap/detail/unsolicited_table.h>
#include <bkmail/imap/detail/write_pump.h>
#include <bkmail/imap/imap_command.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bkmail/imap/unsolicited_event.h>
#include <bnio/buffer/basic.h>
#include <bnio/buffer/dynamic_byte_vector.h>
#include <bnio/io_context.h>
#include <bnio/tcp/socket.h>
#include <sys/socket.h>

#include <atomic>
#include <bexec/scheduler.hpp>
#include <cassert>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bkmail::imap {

namespace detail {

/**
 * [self-built] Stream requirements for imap_context (architecture §3.1).
 * Modeled by `bnio::tcp::socket` and
 * `bnio::ssl_stream<bnio::tcp::socket>`. `lowest_layer()` gives access to
 * `shutdown()` / `close()` for cancellation regardless of the encryption
 * layer.
 */
template <class S>
concept imap_stream = std::move_constructible<S> &&
                      requires(S& s, bnio::io_context::post_scheduler sched,
                               bnio::mutable_buffer mb, bnio::const_buffer cb) {
                        // read: set_value(std::error_code, std::size_t) /
                        // set_stopped
                        { s.async_read_some(sched, mb, 0) };
                        // write-all sender
                        { s.async_write(sched, cb, 0) };
                        {
                          s.lowest_layer()
                        } -> std::same_as<bnio::tcp::socket&>;
                      };

/**
 * Heap-held state of an imap_context (see the imap_context documentation
 * for the lifetime contract). All members are technically public so the
 * friend-scoped pumps can drive them; the type itself is detail-only and
 * reachable solely through imap_context (which carries the imap_stream
 * constraint; this detail type is intentionally unconstrained so the
 * pumps' forward declarations stay consistent).
 *
 * The core outlives its shell whenever a pump operation is still in
 * flight: every self-owning I/O box holds a shared_ptr to the core.
 * `abandoned_` (set by the shell's destruction) switches the read/write
 * completion paths into their quiet teardown forms: no dispatch, no
 * handler invocation, only pump bookkeeping and the fd close.
 */
template <class Stream, class Allocator>
class context_core
    : public std::enable_shared_from_this<context_core<Stream, Allocator>>,
      private operation_sink<Allocator> {
  // The pumps drive the private sink/teardown entry points below.
  template <class, class>
  friend class read_pump;
  template <class, class>
  friend class write_pump;

 public:
  using stream_type = Stream;
  using allocator_type = Allocator;
  using scheduler_type = bnio::io_context::post_scheduler;
  using string_type = string_of<Allocator>;
  using operation_type = operation_base<Allocator>;
  using cell_type = std::unique_ptr<operation_type, op_deleter<Allocator>>;
  using registry_type = std::unordered_map<
      string_type, operation_type*, string_hash, std::equal_to<>,
      rebind_alloc_t<Allocator, std::pair<const string_type, operation_type*>>>;
  using queue_type =
      std::vector<cell_type, rebind_alloc_t<Allocator, cell_type>>;
  using staging_type = std::vector<char, rebind_alloc_t<Allocator, char>>;

  enum class phase { open, draining, closed };

  /**
   * Takes ownership of @p stream and borrows @p ioc. The read pump is
   * NOT armed here (a shared_ptr to the core does not exist yet); the
   * shell arms it right after construction with arm_read().
   */
  context_core(Stream stream, bnio::io_context& ioc, const Allocator& alloc)
      : ioc_(&ioc),
        stream_(std::move(stream)),
        alloc_(alloc),
        registry_(0, string_hash{}, std::equal_to<>{},
                  rebind_alloc_t<Allocator,
                                 std::pair<const string_type, operation_type*>>(
                      alloc)),
        write_queue_(rebind_alloc_t<Allocator, cell_type>(alloc)),
        write_staging_(rebind_alloc_t<Allocator, char>(alloc)),
        read_storage_(alloc),
        read_buffer_(read_storage_),
        parser_(alloc),
        unsolicited_(alloc) {}

  context_core(const context_core&) = delete;
  context_core& operator=(const context_core&) = delete;

  /// Arms the permanent read pump. If arming fails (allocation failure),
  /// the core is left closed: is_alive() reports false.
  void arm_read() noexcept {
    read_running_ = true;
    try {
      read_pump<Stream, Allocator>::arm(this->shared_from_this());
    } catch (...) {
      read_running_ = false;
      phase_ = phase::closed;
    }
  }

  // ---- submission ----------------------------------------------------

  /// Callback path (code_layout D1): see imap_context::submit.
  template <class Command, class F>
    requires command_like<Command> && handler_for<F, Command> &&
             std::same_as<typename Command::allocator_type, Allocator>
  string_type submit(Command command, F&& handler) {
    using model_type = operation_model<Command, std::remove_cvref_t<F>>;
    string_type tag = allocate_tag();
    cell_type cell = allocate_operation<model_type>(alloc_, std::move(command),
                                                    std::forward<F>(handler));
    cell->attach(sink(), tag);
    enqueue_operation(std::move(cell), tag, false);
    return tag;
  }

  /// Erased batch path: see imap_context::submit(vector).
  std::vector<string_type, rebind_alloc_t<Allocator, string_type>> submit(
      std::vector<std::unique_ptr<imap_command<Allocator>>> commands) {
    std::vector<string_type, rebind_alloc_t<Allocator, string_type>> tags{
        rebind_alloc_t<Allocator, string_type>(alloc_)};
    tags.reserve(commands.size());
    for (auto& command : commands) {
      string_type tag = allocate_tag();
      cell_type cell = std::move(*command).release();
      cell->attach(sink(), tag);
      enqueue_operation(std::move(cell), tag, false);
      tags.push_back(std::move(tag));
    }
    return tags;
  }

  /// Performs one batched `async_write` of the pending queue.
  void flush() {
    write_pump<Stream, Allocator>::post_kick(this->shared_from_this());
  }

  /// Three-granularity cancellation (code_layout D3): see
  /// imap_context::cancel.
  void cancel(std::string_view tag) {
    cell_type cell;
    bool kick = false;
    {
      std::lock_guard lock(mutex_);
      if (phase_ != phase::open) {
        return;
      }
      auto it = registry_.find(tag);
      if (it == registry_.end()) {
        return;
      }
      cell = detach_locked(it, kick);
    }
    if (kick) {
      kick_write_pump();
    }
    if (cell != nullptr) {
      post_complete_stopped(std::move(cell));
    }
  }

  // ---- unsolicited responses ------------------------------------------

  /// Registers @p f for unsolicited events: see imap_context::on_unsolicited.
  template <class F>
    requires std::invocable<F&, const unsolicited_event<Allocator>&>
  [[nodiscard]] registration<Allocator> on_unsolicited(F&& f) {
    return unsolicited_.add(std::forward<F>(f));
  }

  // ---- io_context borrowing -------------------------------------------

  /// Points the context's *next* I/O at @p ioc (architecture §3.6).
  void set_io_context(bnio::io_context& ioc) noexcept {
    std::lock_guard lock(mutex_);
    ioc_ = &ioc;
  }

  /// The context the next I/O initiation borrows.
  [[nodiscard]] bnio::io_context& io_context() const noexcept {
    std::lock_guard lock(mutex_);
    return *ioc_;
  }

  // ---- lifecycle -------------------------------------------------------

  /// True while the permanent read pump is running.
  [[nodiscard]] bool is_alive() const noexcept {
    std::lock_guard lock(mutex_);
    return read_running_;
  }

  /**
   * Close protocol (architecture §3.7): (1) stop accepting submissions;
   * (2) `shutdown(SHUT_RD)` so the armed read completes promptly;
   * (3) fail queued operations (written ones are failed when the read
   * side dies); (4) once the read pump and any in-flight write have
   * completed, close the descriptor. The fd is never closed while a
   * kernel operation could still reference it.
   */
  void close() noexcept {
    queue_type pending{rebind_alloc_t<Allocator, cell_type>(alloc_)};
    {
      std::lock_guard lock(mutex_);
      if (phase_ != phase::open) {
        return;
      }
      phase_ = phase::draining;
      (void)stream_.lowest_layer().shutdown(SHUT_RD);
      for (auto it = write_queue_.begin(); it != write_queue_.end();) {
        if (!(*it)->is_written() && !(*it)->is_staged()) {
          // Never-written cells fail now; staged cells are retired by
          // the in-flight write's completion, written cells when the
          // read side dies (promptly, due to SHUT_RD).
          if (const auto reg = registry_.find((*it)->tag());
              reg != registry_.end()) {
            registry_.erase(reg);
          }
          pending.push_back(std::move(*it));
          it = write_queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& cell : pending) {
      cell->fail(std::make_error_code(std::errc::operation_canceled));
    }
    maybe_finish_close();
  }

  /**
   * Abandonment protocol (usage.md §7.4 "destroying an imap_context is
   * itself the clean way to end a session"): the shell is gone, so
   * submissions stop, the read side is shut down, and every queued
   * handler is DROPPED — destroyed without being invoked. The core
   * itself stays alive until both pumps are quiet and every self-owning
   * pump box has retired (the boxes hold shared_ptrs to the core).
   */
  void abandon() noexcept {
    queue_type dropped{rebind_alloc_t<Allocator, cell_type>(alloc_)};
    {
      std::lock_guard lock(mutex_);
      if (phase_ == phase::closed) {
        return;
      }
      if (phase_ == phase::open) {
        phase_ = phase::draining;
      }
      abandoned_.store(true, std::memory_order_release);
      (void)stream_.lowest_layer().shutdown(SHUT_RD);
      // Never-written and written cells are dropped silently; staged
      // cells are still referenced by the in-flight write and are
      // retired (silently) by its completion.
      registry_.clear();
      for (auto it = write_queue_.begin(); it != write_queue_.end();) {
        if ((*it)->is_staged()) {
          ++it;
          continue;
        }
        dropped.push_back(std::move(*it));
        it = write_queue_.erase(it);
      }
    }
    // Cell destruction runs user receiver destructors; outside the lock,
    // and never through a completion (dropped, not invoked).
    dropped.clear();
    // A real transport completes the armed read (and any in-flight write)
    // once SHUT_RD happened, which retires the pump boxes so the core can
    // destroy itself. Streams that cannot observe the shutdown opt in to
    // the same effect explicitly (tests/scripted_stream); the bnio
    // sockets do not implement the extension and complete via the kernel.
    //
    // Ordering against a concurrent pump re-arm comes from the recursive
    // mutex itself: the pumps decide the phase and register the armed
    // operation inside one critical section (read_pump/write_pump
    // arm()), so this delivery is serialized against every arm — either
    // the armed operation is already registered and gets completed here,
    // or this abandon() won the mutex and the losing arm unwinds instead
    // of arming. Under every interleaving exactly one side delivers the
    // teardown completion.
    if constexpr (requires { stream_.complete_pending_io_for_teardown(); }) {
      stream_.complete_pending_io_for_teardown();
    }
    maybe_finish_close();
  }

  /**
   * Allocates the next command tag: `"a"` + zero-padded decimal counter
   * (`a0001`, ...). Exposed for tests; submit() calls it internally.
   */
  [[nodiscard]] string_type allocate_tag() {
    std::lock_guard lock(mutex_);
    const std::uint64_t value = tag_counter_++;
    char digits[20];
    const auto converted =
        std::to_chars(digits, digits + sizeof(digits), value);
    const auto length = static_cast<std::size_t>(converted.ptr - digits);
    string_type tag(alloc_);
    tag.push_back('a');
    if (length < 4) {
      tag.append(4 - length, '0');
    }
    tag.append(digits, length);
    return tag;
  }

  /// The allocator every internal container is configured with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  // ---- registration paths (pump/sender-facing) -------------------------

  [[nodiscard]] operation_sink<Allocator>* sink() noexcept { return this; }

  /// Sender-path entry: stamps the tag and enqueues with a pump kick.
  void start_operation(cell_type cell) {
    string_type tag = allocate_tag();
    cell->attach(sink(), tag);
    enqueue_operation(std::move(cell), std::move(tag), true);
  }

  /// Registers and queues the cell. When @p kick is set (sender path),
  /// the write pump is posted through the scheduler; the callback/batch
  /// paths wait for an explicit `flush()`.
  void enqueue_operation(cell_type cell, string_type tag, bool kick) {
    cell_type stopped;
    cell_type rejected;
    bool post = false;
    bool rejected_after_bye = false;
    {
      std::lock_guard lock(mutex_);
      if (phase_ == phase::open) {
        // Push first: if the registry insertion throws, the queue still
        // owns the cell and no dangling pointer is left behind.
        write_queue_.push_back(std::move(cell));
        try {
          registry_.emplace(tag, write_queue_.back().get());
        } catch (...) {
          write_queue_.pop_back();
          throw;
        }
        if (write_queue_.back()->take_stop_request()) {
          // Stop fired between attach() and registration.
          stopped = extract_from_queue_locked(write_queue_.back().get());
          registry_.erase(tag);
        } else {
          post = kick;
        }
      } else {
        // The failure reason is decided under the lock: a recorded
        // unsolicited BYE wins over the generic cancelled code, so the
        // NEXT operation surfaces server_bye no matter how long ago the
        // connection actually died (usage.md §6 reconnect policy).
        rejected = std::move(cell);
        rejected_after_bye = bye_received_;
      }
    }
    if (stopped != nullptr) {
      post_complete_stopped(std::move(stopped));
      return;
    }
    if (rejected != nullptr) {
      // Submissions on a closing/closed context fail immediately.
      rejected->fail(rejected_after_bye
                         ? make_error_code(errc::server_bye)
                         : std::make_error_code(std::errc::operation_canceled));
      return;
    }
    if (post) {
      write_pump<Stream, Allocator>::post_kick(this->shared_from_this());
    }
  }

  /// Detach-or-DONE for one registry entry (caller holds `mutex_`):
  ///  - queued, never on the wire: extracted for a posted stop-
  ///    completion — no bytes ever hit the wire;
  ///  - a written or mid-handshake operation that handles cancellation
  ///    itself (IDLE) stays registered and sets @p kick so its DONE
  ///    segment is flushed;
  ///  - fully written: extracted; its tagged reply is dropped on
  ///    arrival;
  ///  - mid-handshake (staged or parked on a continuation): marked
  ///    detached and left in the queue so the wire stays consistent —
  ///    it renders its remaining segments detached and the write pump
  ///    retires it (stop completion) at the flush that writes it out.
  cell_type detach_locked(typename registry_type::iterator it, bool& kick) {
    operation_type* op = it->second;
    if (!op->is_written() && !op->is_staged() && !op->awaits_continuation()) {
      cell_type cell = extract_from_queue_locked(op);
      registry_.erase(it);
      return cell;
    }
    if (op->cancel_written()) {
      kick = true;
      return nullptr;
    }
    if (op->is_written()) {
      cell_type cell = extract_from_queue_locked(op);
      registry_.erase(it);
      return cell;
    }
    op->mark_detached();
    registry_.erase(it);
    return nullptr;
  }

  /// Splices one cell out of the write queue (caller holds `mutex_`).
  cell_type extract_from_queue_locked(operation_type* op) {
    for (auto it = write_queue_.begin(); it != write_queue_.end(); ++it) {
      if (it->get() == op) {
        cell_type cell = std::move(*it);
        write_queue_.erase(it);
        return cell;
      }
    }
    return nullptr;
  }

  /// True while @p op is still owned by the write queue (caller holds
  /// `mutex_`). Used by the write pump to re-validate staged pointers
  /// against tagged replies that raced ahead of the write completion.
  [[nodiscard]] bool queue_contains_locked(operation_type* op) const {
    for (const auto& cell : write_queue_) {
      if (cell.get() == op) {
        return true;
      }
    }
    return false;
  }

  /// Posts a stop-completion for an owned cell through the scheduler:
  /// receivers are only ever completed from the context's dispatch path
  /// (architecture §6 deadlock-avoidance rule).
  void post_complete_stopped(cell_type cell) noexcept {
    try {
      auto core = this->shared_from_this();
      auto* box = make_io_box(
          alloc_, bexec::schedule(scheduler()),
          [&cell, core = std::move(core)](io_box_base* self) mutable {
            return stopped_receiver(self, std::move(cell), std::move(core));
          });
      box->start();
    } catch (...) {
      // No scheduler slot (allocation failure): complete inline.
      cell->complete_stopped();
    }
  }

  // ---- teardown (pump-facing) -------------------------------------------

  /**
   * Connection-level teardown: stops submissions, fails every queued and
   * registered operation with @p ec on the value channel, and shuts the
   * read side down. Idempotent; the descriptor is closed by
   * `maybe_finish_close()` once both pumps are quiet.
   *
   * Operations whose segment is staged in the in-flight write are NOT
   * touched here: the write still references their cells, so the write
   * completion retires (fails and destroys) them.
   */
  void connection_lost(std::error_code ec) noexcept {
    queue_type taken{rebind_alloc_t<Allocator, cell_type>(alloc_)};
    {
      std::lock_guard lock(mutex_);
      if (phase_ == phase::closed) {
        return;
      }
      if (phase_ == phase::open) {
        phase_ = phase::draining;
      }
      for (auto it = write_queue_.begin(); it != write_queue_.end();) {
        if ((*it)->is_staged()) {
          ++it;  // Owned by the in-flight write until it completes.
          continue;
        }
        taken.push_back(std::move(*it));
        it = write_queue_.erase(it);
      }
      registry_.clear();
      continuation_target_ = nullptr;
      if (read_running_) {
        (void)stream_.lowest_layer().shutdown(SHUT_RD);
      }
    }
    // Receivers (user code) complete outside the lock.
    for (auto& cell : taken) {
      cell->fail(ec);
    }
    {
      // Cell destruction is serialized with an in-flight untagged
      // broadcast (which holds the lock while calling on_untagged).
      // Receiver destructors therefore run under the lock: they must
      // not call back into the context (architecture §6 contract).
      std::lock_guard lock(mutex_);
      taken.clear();
    }
    maybe_finish_close();
  }

  /// Closes the descriptor once draining and both pumps are quiet. The
  /// fd is never closed while a kernel operation could reference it
  /// (fd-reuse race, architecture §10).
  void maybe_finish_close() noexcept {
    std::lock_guard lock(mutex_);
    if (phase_ == phase::draining && !read_running_ && !write_in_flight_) {
      (void)stream_.lowest_layer().close();
      phase_ = phase::closed;
    }
  }

  // ---- relocation hooks (Layer 2 / STARTTLS, architecture §8) -----------

  /// Stops the read pump from re-arming after the current dispatch
  /// unwinds, WITHOUT touching the descriptor (unlike `shutdown`, which
  /// would persist across the move and break a subsequent TLS
  /// handshake). The context must not be destroyed until
  /// `is_read_suspended()` reports true.
  void suspend_read_for_relocation() noexcept {
    std::lock_guard lock(mutex_);
    suspend_read_ = true;
  }

  /// True once the suspended read pump has fully unwound.
  [[nodiscard]] bool is_read_suspended() const noexcept {
    std::lock_guard lock(mutex_);
    return suspend_read_ && !read_running_;
  }

  /// Hands the stream to a new context (e.g. into an ssl_stream).
  /// @pre `is_read_suspended()` and no operation in flight.
  [[nodiscard]] Stream&& release_stream() noexcept {
    return std::move(stream_);
  }

  // ---- scheduler access --------------------------------------------------

  /// Scheduler for the next I/O initiation. Never call while holding
  /// `mutex_`.
  [[nodiscard]] scheduler_type scheduler() noexcept {
    std::lock_guard lock(mutex_);
    return ioc_->get_post_scheduler();
  }

  // ---- members ------------------------------------------------------------

  // Coherence lock. Recursive: the pump arm() paths hold it across the
  // registration of the armed operation, and an eagerly-completed arm
  // re-enters the completion path on the same thread (the scripted test
  // stream delivers through a scheduler fast path; bnio sockets complete
  // from their worker instead). Presence is documented so implementers
  // know the invariant set; bkmail makes NO public thread-safety promise
  // about concurrent API calls on one context (architecture §6).
  mutable std::recursive_mutex mutex_;

  bnio::io_context* ioc_;  // borrowed; see set_io_context contract
  Stream stream_;          // owned I/O credential
  [[no_unique_address]] Allocator alloc_;

  std::uint64_t tag_counter_ = 1;

  // tag -> in-flight operation (non-owning; ownership lives in the queue)
  registry_type registry_;

  // Outgoing operations; unique_ptr elements keep pointees stable across
  // reallocation (buffer stability, architecture §2.5/§3.4).
  queue_type write_queue_;

  // Contiguous staging buffer for batched writes; never mutated while a
  // write is in flight.
  staging_type write_staging_;
  bool write_in_flight_ = false;
  bool kick_posted_ = false;

  // The single operation paused on a continuation request, if any.
  operation_type* continuation_target_ = nullptr;

  // Read side: storage first, the buffer adapter aliases it.
  std::vector<std::byte, Allocator> read_storage_;
  bnio::dynamic_byte_vector_buffer<Allocator> read_buffer_;
  response_lexer<Allocator> lexer_;
  response_parser<Allocator> parser_;
  bool read_running_ = false;
  bool suspend_read_ = false;
  bool bye_received_ = false;

  // Set once the shell was destroyed: pump completions switch to their
  // quiet teardown forms (no dispatch, no handler invocation).
  std::atomic<bool> abandoned_ = false;

  // Unsolicited event handler table.
  unsolicited_table<Allocator> unsolicited_;

  phase phase_ = phase::open;

 private:
  // ---- sink implementation ---------------------------------------------

  void on_stop_requested(operation_type* op) noexcept override {
    cell_type cell;
    bool kick = false;
    {
      std::lock_guard lock(mutex_);
      if (phase_ != phase::open) {
        return;
      }
      auto it = registry_.find(op->tag());
      if (it == registry_.end() || it->second != op) {
        // Not registered yet (registration race) or already gone: the
        // registration path's take_stop_request() owns the former case.
        return;
      }
      cell = detach_locked(it, kick);
    }
    if (kick) {
      kick_write_pump();
    }
    if (cell != nullptr) {
      post_complete_stopped(std::move(cell));
    }
  }

  void kick_write_pump() noexcept override {
    write_pump<Stream, Allocator>::post_kick(this->shared_from_this());
  }

  /// Receiver completing a detached cell with stop semantics.
  class stopped_receiver {
   public:
    stopped_receiver(io_box_base* box, cell_type cell,
                     std::shared_ptr<context_core> core) noexcept
        : box_(box), cell_(std::move(cell)), core_(std::move(core)) {}

    void set_value(std::error_code) noexcept { finish(); }
    void set_stopped() noexcept { finish(); }

   private:
    void finish() noexcept {
      cell_->complete_stopped();
      cell_.reset();
      box_->dispose();
    }

    io_box_base* box_;
    cell_type cell_;
    std::shared_ptr<context_core> core_;  // keeps the core alive
  };
};

}  // namespace detail

template <detail::imap_stream Stream,
          class Allocator = std::allocator<std::byte>>
class imap_context {
 public:
  using stream_type = Stream;
  using allocator_type = Allocator;
  using scheduler_type = bnio::io_context::post_scheduler;
  using string_type = detail::string_of<Allocator>;

 private:
  using core_type = detail::context_core<Stream, Allocator>;

 public:
  /**
   * Takes ownership of @p stream and borrows @p ioc (never owns it), and
   * arms the permanent read pump immediately (code_layout D5): a context
   * whose read pump is not running is a half-object. The greeting is
   * consumed through the unsolicited path.
   *
   * @pre @p stream is already connected.
   * @pre @p ioc outlives this context and has a thread inside `run()`.
   *
   * If arming the first read fails (allocation failure), the context is
   * constructed closed: `is_alive()` returns false.
   */
  imap_context(Stream stream, bnio::io_context& ioc,
               const Allocator& alloc = Allocator{})
      : core_(std::make_shared<core_type>(std::move(stream), ioc, alloc)) {
    core_->arm_read();
  }

  imap_context(const imap_context&) = delete;
  imap_context& operator=(const imap_context&) = delete;

  /**
   * Move-constructs the shell: only the shared ownership travels; the
   * underlying state (stream, pumps, registry) keeps its stable address,
   * so a move is legal at any time — even with operations in flight.
   */
  imap_context(imap_context&&) noexcept = default;
  imap_context& operator=(imap_context&&) = delete;

  /**
   * Ends the session (usage.md §7.4): pending handlers are dropped, not
   * invoked. The underlying state self-destructs once the read/write
   * pumps have gone quiet; destroying the shell never blocks and never
   * invalidates an in-flight pump operation. A moved-from shell owns no
   * core and its destruction is a no-op.
   */
  ~imap_context() {
    if (core_ != nullptr) {
      core_->abandon();
    }
  }

  // ---- submission ----------------------------------------------------

  /**
   * Callback path (code_layout D1): stamps a tag, queues the command
   * bytes WITHOUT doing I/O, and returns the tag. The handler is invoked
   * from the read loop as `handler(ec)` (void results) or
   * `handler(ec, result)`. A tagged NO reports
   * `errc::command_rejected`, BAD reports `errc::bad_command`
   * (code_layout D2). Call `flush()` to write the batch.
   */
  template <class Command, class F>
    requires detail::command_like<Command> && detail::handler_for<F, Command> &&
             std::same_as<typename Command::allocator_type, Allocator>
  string_type submit(Command command, F&& handler) {
    return core_->submit(std::move(command), std::forward<F>(handler));
  }

  /**
   * Sender path (code_layout D1): builds an Operation from @p args and
   * returns a lazy sender completing with
   * `set_value(std::error_code[, Operation::result_type])` /
   * `set_stopped()`. Nothing is sent until the sender is connected and
   * started; the tag is allocated at `start()`, in wire order.
   */
  template <class Operation, class... Args>
    requires detail::command_like<Operation> &&
             std::same_as<typename Operation::allocator_type, Allocator>
  [[nodiscard]] auto submit(Args&&... args) {
    return detail::submit_sender<imap_context, Operation, Args...>(
        this, std::forward<Args>(args)...);
  }

  /**
   * Erased batch path: submits pre-built command cells (see
   * `make_command`), returning the stamped tags in vector order. No I/O
   * until `flush()`.
   */
  std::vector<string_type, detail::rebind_alloc_t<Allocator, string_type>>
  submit(std::vector<std::unique_ptr<imap_command<Allocator>>> commands) {
    return core_->submit(std::move(commands));
  }

  /// Performs one batched `async_write` of the pending queue (the write
  /// is staged and armed through the scheduler, so submissions already
  /// queued when the worker runs share the syscall).
  void flush() { core_->flush(); }

  /**
   * Three-granularity cancellation (code_layout D3):
   *  - queued, not yet written: removed from queue and registry; the
   *    completion carries stop semantics — no bytes ever hit the wire;
   *  - written, awaiting its tagged reply: completed with stop semantics
   *    and detached; the reply is dropped on arrival, the connection
   *    stays usable;
   *  - a pending `idle_command` sends DONE first (its tagged reply still
   *    completes the operation, with stop semantics).
   * The completion runs on the context's dispatch path, never inline.
   */
  void cancel(std::string_view tag) { core_->cancel(tag); }

  // ---- unsolicited responses ------------------------------------------

  /**
   * Registers @p f for unsolicited events: one handler receives the whole
   * `unsolicited_event` variant (code_layout §5.1). Handlers run inline
   * on the read-dispatch path on an arbitrary worker thread (§6); they
   * must not block and must not call back into the context. Destroying
   * the returned token unregisters; tokens must not outlive the context.
   */
  template <class F>
    requires std::invocable<F&, const unsolicited_event<Allocator>&>
  [[nodiscard]] detail::registration<Allocator> on_unsolicited(F&& f) {
    return core_->on_unsolicited(std::forward<F>(f));
  }

  // ---- io_context borrowing -------------------------------------------

  /**
   * Points the context's *next* I/O at @p ioc (architecture §3.6).
   * Contractual boundaries:
   *  1. In-flight operations never migrate; the old context must outlive
   *     them (its workers run their completions).
   *  2. Completions run on the initiating context's worker threads: after
   *     a switch, different callbacks of this connection may run on
   *     different threads.
   *  3. The target context must have a thread inside `run()`, otherwise
   *     work is queued but never executes.
   *  4. A single ssl_stream operation uses one scheduler internally;
   *     switching is only meaningful between independent operations,
   *     never mid-handshake or mid-shutdown.
   *  5. `bnio::steady_timer` is context-bound at construction: watchdog
   *     and IDLE heartbeat timers (Layer 2) are recreated from the
   *     current context after every switch, and switching during IDLE is
   *     a precondition violation.
   */
  void set_io_context(bnio::io_context& ioc) noexcept {
    core_->set_io_context(ioc);
  }

  /// The context the next I/O initiation borrows.
  [[nodiscard]] bnio::io_context& io_context() const noexcept {
    return core_->io_context();
  }

  // ---- lifecycle -------------------------------------------------------

  /// True while the permanent read pump is running.
  [[nodiscard]] bool is_alive() const noexcept { return core_->is_alive(); }

  /**
   * Close protocol (architecture §3.7): (1) stop accepting submissions;
   * (2) `shutdown(SHUT_RD)` so the armed read completes promptly;
   * (3) fail queued operations (written ones are failed when the read
   * side dies); (4) once the read pump and any in-flight write have
   * completed, close the descriptor. The fd is never closed while a
   * kernel operation could still reference it.
   */
  void close() noexcept { core_->close(); }

  /**
   * Allocates the next command tag: `"a"` + zero-padded decimal counter
   * (`a0001`, ...). Exposed for tests; `submit()` calls it internally.
   */
  [[nodiscard]] string_type allocate_tag() { return core_->allocate_tag(); }

  /// The allocator every internal container is configured with.
  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return core_->get_allocator();
  }

 private:
  template <class, class, class>
  friend class detail::submit_operation;
  // Layer 2 drives STARTTLS relocation through the private hooks below.
  template <class>
  friend class imap_connection;

  /// Sender-path entry: stamps the tag and enqueues with a pump kick.
  void start_operation(std::unique_ptr<detail::operation_base<Allocator>,
                                       detail::op_deleter<Allocator>>
                           cell) {
    core_->start_operation(std::move(cell));
  }

  // ---- relocation hooks (Layer 2 / STARTTLS, architecture §8) -----------

  /// Stops the read pump from re-arming after the current dispatch
  /// unwinds, WITHOUT touching the descriptor (unlike `shutdown`, which
  /// would persist across the move and break a subsequent TLS
  /// handshake). The context must not be destroyed until
  /// `is_read_suspended()` reports true.
  void suspend_read_for_relocation() noexcept {
    core_->suspend_read_for_relocation();
  }

  /// True once the suspended read pump has fully unwound.
  [[nodiscard]] bool is_read_suspended() const noexcept {
    return core_->is_read_suspended();
  }

  /// Hands the stream to a new context (e.g. into an ssl_stream).
  /// @pre `is_read_suspended()` and no operation in flight.
  [[nodiscard]] Stream&& release_stream() noexcept {
    return core_->release_stream();
  }

  // The shared session state; outlives the shell while any pump
  // operation is still in flight (see the file documentation).
  std::shared_ptr<core_type> core_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_IMAP_CONTEXT_H_
