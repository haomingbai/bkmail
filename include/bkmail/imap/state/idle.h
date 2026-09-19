/**
 * @file idle.h
 * @brief RFC 2177 IDLE session state.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_IDLE_H_
#define BKMAIL_IMAP_STATE_IDLE_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/imap/detail/unsolicited_table.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/state/selected.h>

#include <atomic>
#include <bexec/env.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace bkmail::imap {

namespace detail {

/// Operation state behind idle_state::done() (pinned, single-start):
/// sends DONE (cancel of the pending idle_command) and hands the Selected
/// state back. The server acknowledges DONE with the idle command's tagged
/// reply, which Layer 1 discards on arrival after the cancel.
template <class Allocator, class Receiver>
class idle_done_operation {
 public:
  idle_done_operation(imap_connection<Allocator>&& connection,
                      bkmail::detail::string_of<Allocator>&& tag,
                      mailbox_info<Allocator>&& snapshot, Receiver&& receiver)
      : connection_(std::move(connection)),
        tag_(std::move(tag)),
        snapshot_(std::move(snapshot)),
        receiver_(std::move(receiver)) {}

  idle_done_operation(const idle_done_operation&) = delete;
  idle_done_operation& operator=(const idle_done_operation&) = delete;
  idle_done_operation(idle_done_operation&&) = delete;
  idle_done_operation& operator=(idle_done_operation&&) = delete;

  void start() noexcept {
    const auto token = bexec::get_stop_token(bexec::get_env(receiver_));
    if (token.stop_requested()) {
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    connection_.with_context(
        [tag = tag_](auto& ctx) mutable { ctx.cancel(tag); });
    bexec::set_value(std::move(receiver_), std::error_code{},
                     selected_state<Allocator>{std::move(connection_),
                                               std::move(snapshot_)});
  }

 private:
  imap_connection<Allocator> connection_;
  bkmail::detail::string_of<Allocator> tag_;
  mailbox_info<Allocator> snapshot_;
  Receiver receiver_;
};

/// Sender behind idle_state::done().
template <class Allocator>
class idle_done_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, selected_state<Allocator>),
      bexec::set_stopped_t()>;

  idle_done_sender(imap_connection<Allocator> connection,
                   bkmail::detail::string_of<Allocator> tag,
                   mailbox_info<Allocator> snapshot)
      : connection_(std::move(connection)),
        tag_(std::move(tag)),
        snapshot_(std::move(snapshot)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return idle_done_operation<Allocator, Receiver>{
        std::move(connection_), std::move(tag_), std::move(snapshot_),
        std::move(receiver)};
  }

 private:
  imap_connection<Allocator> connection_;
  bkmail::detail::string_of<Allocator> tag_;
  mailbox_info<Allocator> snapshot_;
};

}  // namespace detail

/**
 * Exclusive IDLE handle (RFC 2177).
 *
 * While an idle_state exists the connection is monopolized: no other
 * command may be submitted. The state layer's selected_state::idle()
 * already drives a full IDLE/DONE cycle per operation; this handle is the
 * persistent form for command-layer audiences that drive idle_command
 * manually and want the typed DONE/stop semantics.
 *
 * The handle owns the session for its lifetime; done() returns it to the
 * Selected state.
 */
template <class Allocator = std::allocator<std::byte>>
class idle_state {
 public:
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /**
   * Wraps a session whose idle_command (@p tag) is already in flight.
   * @param connection the session owner
   * @param tag        the submitted idle_command's tag (cancel sends DONE)
   * @param snapshot   the open mailbox's snapshot, carried into the
   *                   Selected state done() returns
   */
  idle_state(imap_connection<Allocator> connection, string_type tag,
             mailbox_info<Allocator> snapshot)
      : connection_(std::move(connection)),
        tag_(std::move(tag)),
        snapshot_(std::move(snapshot)) {}

  idle_state(const idle_state&) = delete;
  idle_state& operator=(const idle_state&) = delete;
  idle_state(idle_state&&) = default;

  /// Destruction without done()/stop() ends IDLE politely (DONE) before
  /// the connection is torn down.
  ~idle_state() {
    if (!finished_.load(std::memory_order_acquire)) {
      connection_.with_context(
          [tag = tag_](auto& ctx) mutable { ctx.cancel(tag); });
    }
  }

  /**
   * Registers the push callback (EXISTS/EXPUNGE/FETCH/BYE events). Runs on
   * an arbitrary worker thread of the connection's current io_context and
   * must not block. Replaces any previously registered callback.
   */
  template <class F>
  void on_event(F&& f) {
    connection_.with_context([&](auto& ctx) {
      event_registration_.emplace(ctx.on_unsolicited(std::forward<F>(f)));
    });
  }

  /// Sends DONE and completes with the Selected state once the DONE write
  /// is queued; the idle command's tagged reply is discarded on arrival.
  [[nodiscard]] auto done() && {
    finished_.store(true, std::memory_order_release);
    event_registration_.reset();
    return detail::idle_done_sender<Allocator>{
        std::move(connection_), std::move(tag_), std::move(snapshot_)};
  }

  /// Sends DONE and completes immediately with stopped semantics: the
  /// handle is left spent (only destruction remains meaningful).
  void stop() noexcept {
    if (finished_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    event_registration_.reset();
    connection_.with_context(
        [tag = tag_](auto& ctx) mutable { ctx.cancel(tag); });
  }

 private:
  imap_connection<Allocator> connection_;
  string_type tag_;
  mailbox_info<Allocator> snapshot_;
  std::optional<detail::registration<Allocator>> event_registration_;
  std::atomic<bool> finished_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_STATE_IDLE_H_
