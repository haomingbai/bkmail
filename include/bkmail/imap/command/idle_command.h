/**
 * @file include/bkmail/imap/command/idle_command.h
 * @brief RFC 2177 IDLE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 2177 IDLE: `tag IDLE` -> `+ idling` -> server pushes untagged
 * events -> `DONE` -> tagged completion.
 *
 * Connection exclusivity (docs/architecture.md §9): while an IDLE is in
 * flight the connection is parked — no other command may be written to
 * the socket until the tagged completion arrives. The read side keeps
 * flowing: untagged EXISTS / EXPUNGE / FETCH(FLAGS) / ... are broadcast
 * through the imap_context unsolicited table as usual and are *not*
 * absorbed by this command. The state layer enforces exclusivity by
 * construction; on the raw command layer the caller is responsible for
 * not flushing other commands while idling().
 *
 * Termination: call done(), or cancel the operation through
 * imap_context::cancel — the engine forwards the cancel of a written
 * IDLE to done() (imap/operation_base.h contract), the command queues
 * `DONE`, and the tagged completion that answers it finishes the
 * operation (on the stopped channel for the cancel path). done() before
 * the `+ idling` continuation is legal: the DONE segment is queued as
 * soon as idling is acknowledged. RFC 5550 recommends re-issuing IDLE
 * at least every 29 minutes; re-issuing is the caller's loop.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a
 * tagged NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_IDLE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_IDLE_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/error.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// RFC 2177 IDLE command. `result_type` is `void`.
template <class Allocator = std::allocator<std::byte>>
class idle_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: IDLE carries no data.
  using result_type = void;
  /// Rebound character string used for the wire buffer.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates an IDLE command. No arguments.
  explicit idle_command(const Allocator& alloc = Allocator{})
      : alloc_(alloc), segment_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// True between the `+ idling` continuation and the DONE request —
  /// the window in which the server pushes events.
  [[nodiscard]] bool idling() const noexcept { return phase_ == phase::idling; }

  /// Requests termination of the IDLE session: queues the DONE segment
  /// (immediately if idling, otherwise as soon as the server
  /// acknowledges idling). Idempotent; a no-op once DONE is on the wire
  /// or the tagged completion arrived. May allocate for the DONE
  /// segment.
  void done() {
    done_requested_ = true;
    if (phase_ == phase::idling) queue_done();
  }

  /// Stamps the tag and renders `tag IDLE\r\n`.
  void render(std::string_view tag) {
    segment_.clear();
    segment_.append(tag);
    segment_.append(" IDLE\r\n");
    phase_ = phase::idle_sent;
    segment_pending_ = true;
  }

  /// Returns the pending wire segment (`tag IDLE\r\n` or `DONE\r\n`),
  /// or an empty buffer while idling / finished.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{segment_});
  }

  /// True while the engine must wait for the `+ idling` continuation
  /// before anything else may happen with this command.
  [[nodiscard]] bool awaits_continuation() const noexcept {
    return phase_ == phase::idle_sent;
  }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept {
    segment_pending_ = false;
    if (phase_ == phase::done_pending) phase_ = phase::done_sent;
  }

  /// Consumes the `+ idling` continuation and enters the idling phase;
  /// a done() requested before this point takes effect now.
  void on_continuation(std::string_view /*text*/) {
    if (phase_ != phase::idle_sent) return;
    phase_ = phase::idling;
    if (done_requested_) queue_done();
  }

  /// Untagged events during IDLE belong to the connection, not to this
  /// command: imap_context broadcasts them through the unsolicited table
  /// (docs/architecture.md §9). Nothing is absorbed here.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Maps the tagged completion that answers DONE: OK -> success,
  /// NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r) noexcept {
    phase_ = phase::finished;
    return detail::map_status_to_ec(r.status);
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;

  /// IDLE state machine: initial -> idle_sent (awaiting `+ idling`) ->
  /// idling -> done_pending (DONE queued) -> done_sent (awaiting tagged)
  /// -> finished.
  enum class phase {
    initial,
    idle_sent,
    idling,
    done_pending,
    done_sent,
    finished,
  };

  /// Queues the DONE segment for the write pump.
  void queue_done() {
    segment_.assign("DONE\r\n");
    segment_pending_ = true;
    phase_ = phase::done_pending;
  }

  [[no_unique_address]] Allocator alloc_;
  string_type segment_;
  phase phase_ = phase::initial;
  bool segment_pending_ = false;
  bool done_requested_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_IDLE_COMMAND_H_
