/**
 * @file selected.h
 * @brief IMAP Selected session state.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_SELECTED_H_
#define BKMAIL_IMAP_STATE_SELECTED_H_

#include <bkmail/envelope.h>
#include <bkmail/error.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/command/capability_command.h>
#include <bkmail/imap/command/close_command.h>
#include <bkmail/imap/command/copy_command.h>
#include <bkmail/imap/command/expunge_command.h>
#include <bkmail/imap/command/fetch_command.h>
#include <bkmail/imap/command/fetch_envelopes_command.h>
#include <bkmail/imap/command/fetch_headers_command.h>
#include <bkmail/imap/command/fetch_message_command.h>
#include <bkmail/imap/command/logout_command.h>
#include <bkmail/imap/command/move_command.h>
#include <bkmail/imap/command/noop_command.h>
#include <bkmail/imap/command/search_command.h>
#include <bkmail/imap/command/store_command.h>
#include <bkmail/imap/command/uid_copy_command.h>
#include <bkmail/imap/command/uid_fetch_command.h>
#include <bkmail/imap/command/uid_fetch_envelopes_command.h>
#include <bkmail/imap/command/uid_fetch_headers_command.h>
#include <bkmail/imap/command/uid_fetch_message_command.h>
#include <bkmail/imap/command/uid_move_command.h>
#include <bkmail/imap/command/uid_search_command.h>
#include <bkmail/imap/command/uid_store_command.h>
#include <bkmail/imap/fetch_items.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/message_attributes.h>
#include <bkmail/imap/search_criteria.h>
#include <bkmail/imap/session_state.h>
#include <bkmail/imap/state/detail/state_op_sender.h>
#include <bkmail/imap/state/logout.h>
#include <bkmail/imap/unsolicited_event.h>
#include <bkmail/mail.h>
#include <bkmail/mail_header.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bkmail::imap {

namespace detail {

/**
 * Successor factory for selected_state::close().
 *
 * Declared here and defined in <bkmail/imap/state/authenticated.h>: the
 * two state types reference each other (select/close), and the include
 * edge points from authenticated.h to selected.h so this header stays
 * includable on its own for every other operation.
 */
template <class Allocator>
struct close_successor {
  authenticated_state<Allocator> operator()(
      std::error_code ec, imap_connection<Allocator> conn) const;
};

}  // namespace detail

/**
 * IMAP Selected session state.
 *
 * Owns the session (the imap_connection travels with the state chain) and
 * carries a mailbox_info snapshot of the open mailbox. Unsolicited EXISTS,
 * RECENT and EXPUNGE pushes update the snapshot between commands, so
 * mailbox() always reflects the server's last-known view. Every operation
 * consumes the state by rvalue and returns a lazy sender completing with
 * `set_value(std::error_code, [result,] next_state)` or `set_stopped()`.
 */
template <class Allocator = std::allocator<std::byte>>
class selected_state {
 public:
  using allocator_type = Allocator;

  /// Wraps a live connection and the SELECT/EXAMINE snapshot. Constructed
  /// by Layer-2 operation completions.
  selected_state(imap_connection<Allocator> connection,
                 mailbox_info<Allocator> info)
      : connection_(std::move(connection)),
        mailbox_(std::make_unique<mailbox_info<Allocator>>(std::move(info))) {
    auto* snapshot = mailbox_.get();
    connection_.with_context([this, snapshot](auto& ctx) {
      snapshot_registration_.emplace(ctx.on_unsolicited(
          [snapshot](const unsolicited_event<Allocator>& event) {
            std::visit(
                [snapshot](const auto& e) {
                  using event_type = std::decay_t<decltype(e)>;
                  if constexpr (std::is_same_v<event_type, exists_event>) {
                    snapshot->exists = e.count;
                  } else if constexpr (std::is_same_v<event_type,
                                                      recent_event>) {
                    snapshot->recent = e.count;
                  } else if constexpr (std::is_same_v<event_type,
                                                      expunge_event>) {
                    // EXPUNGE renumbers later messages; EXISTS follows
                    // with the authoritative count.
                    if (snapshot->exists > 0) {
                      --snapshot->exists;
                    }
                  }
                  // flags_update carries per-message flags mailbox_info
                  // does not track; bye/capability are connection-level.
                },
                event);
          }));
    });
  }

  selected_state(const selected_state&) = delete;
  selected_state& operator=(const selected_state&) = delete;
  selected_state(selected_state&&) = default;
  selected_state& operator=(selected_state&&) = default;

  /// Returns the session allocator.
  [[nodiscard]] Allocator get_allocator() const noexcept {
    return connection_.get_allocator();
  }

  /// The server's last-known view of the open mailbox (EXISTS/RECENT/
  /// UNSEEN/UIDVALIDITY/UIDNEXT/flags, read-only flag). Updated by
  /// unsolicited pushes between commands.
  [[nodiscard]] const mailbox_info<Allocator>& mailbox() const noexcept {
    return *mailbox_;
  }

  /// FETCH the ENVELOPEs of @p seq (RFC 3501 sequence-set syntax, e.g.
  /// "1:5", "1,3,7:*").
  [[nodiscard]] auto fetch_envelopes(std::string_view seq) && {
    return query_op(fetch_envelopes_command<Allocator>{seq});
  }

  /// FETCH BODY.PEEK[HEADER.FIELDS (...)] for the given header fields.
  [[nodiscard]] auto fetch_headers(std::string_view seq,
                                   std::vector<std::string_view> fields) && {
    return query_op(fetch_headers_command<Allocator>{seq, std::move(fields)});
  }

  /// FETCH BODY.PEEK[] (full message; never sets \Seen).
  [[nodiscard]] auto fetch_message(std::string_view seq) && {
    return query_op(fetch_message_command<Allocator>{seq});
  }

  /// Generic FETCH over an explicit item selection.
  [[nodiscard]] auto fetch(std::string_view seq, const fetch_items& items) && {
    return query_op(fetch_command<Allocator>{seq, items});
  }

  /// SEARCH with RFC 3501 criteria syntax (e.g. `UNSEEN FROM "bob@x"`).
  [[nodiscard]] auto search(std::string_view criteria) && {
    return query_op(search_command<Allocator>{criteria});
  }

  /// STORE flags: store_mode::add = +FLAGS, remove = -FLAGS,
  /// replace = FLAGS.
  [[nodiscard]] auto store(std::string_view seq, flag_set flags,
                           store_mode mode) && {
    return same_state_op(store_command<Allocator>{seq, flags, mode});
  }

  /// COPY messages to another mailbox.
  [[nodiscard]] auto copy(std::string_view seq, std::string_view mailbox) && {
    return same_state_op(copy_command<Allocator>{seq, mailbox});
  }

  /// MOVE messages (UIDPLUS). Fails immediately with
  /// errc::capability_required when the server lacks MOVE.
  [[nodiscard]] auto move(std::string_view seq, std::string_view mailbox) && {
    return detail::state_op_sender{std::move(connection_),
                                   move_command<Allocator>{seq, mailbox},
                                   take_snapshot(), "MOVE"};
  }

  /// EXPUNGE: permanently removes \Deleted messages.
  [[nodiscard]] auto expunge() && {
    return same_state_op(expunge_command<Allocator>{});
  }

  /// CLOSE the mailbox (silently expunges) and return to the
  /// Authenticated state.
  [[nodiscard]] auto close() && {
    return detail::state_op_sender<Allocator, close_command<Allocator>,
                                   detail::close_successor<Allocator>>{
        std::move(connection_), close_command<Allocator>{}, {}};
  }

  /**
   * One IDLE/DONE cycle (RFC 2177). The sender completes when the server
   * pushes activity (unsolicited EXISTS/EXPUNGE), when the 29-minute
   * heartbeat point arrives (re-issue per RFC 5550), or — via
   * set_stopped() — on cancellation; DONE is sent on every exit. The
   * returned state's mailbox() reflects the newest pushed view.
   *
   * Fails immediately with errc::capability_required when the server did
   * not advertise IDLE.
   */
  [[nodiscard]] auto idle() && {
    // The idle operation drives its own unsolicited listener; retire this
    // state's snapshot hook and hand the snapshot value to the operation.
    snapshot_registration_.reset();
    return detail::idle_op_sender{
        std::move(connection_), std::move(*mailbox_),
        [](std::error_code /*ec*/, imap_connection<Allocator> conn,
           mailbox_info<Allocator> info) {
          return selected_state{std::move(conn), std::move(info)};
        }};
  }

  /// UID FETCH ENVELOPEs (@p seq names UIDs).
  [[nodiscard]] auto uid_fetch_envelopes(std::string_view seq) && {
    return query_op(uid_fetch_envelopes_command<Allocator>{seq});
  }

  /// UID FETCH BODY.PEEK[HEADER.FIELDS (...)] (@p seq names UIDs).
  [[nodiscard]] auto uid_fetch_headers(
      std::string_view seq, std::vector<std::string_view> fields) && {
    return query_op(
        uid_fetch_headers_command<Allocator>{seq, std::move(fields)});
  }

  /// UID FETCH BODY.PEEK[] (@p seq names UIDs).
  [[nodiscard]] auto uid_fetch_message(std::string_view seq) && {
    return query_op(uid_fetch_message_command<Allocator>{seq});
  }

  /// Generic UID FETCH (@p seq names UIDs).
  [[nodiscard]] auto uid_fetch(std::string_view seq,
                               const fetch_items& items) && {
    return query_op(uid_fetch_command<Allocator>{seq, items});
  }

  /// UID SEARCH (@p seq-free criteria; result UIDs).
  [[nodiscard]] auto uid_search(std::string_view criteria) && {
    return query_op(uid_search_command<Allocator>{criteria});
  }

  /// UID STORE (@p seq names UIDs).
  [[nodiscard]] auto uid_store(std::string_view seq, flag_set flags,
                               store_mode mode) && {
    return same_state_op(uid_store_command<Allocator>{seq, flags, mode});
  }

  /// UID COPY (@p seq names UIDs).
  [[nodiscard]] auto uid_copy(std::string_view seq,
                              std::string_view mailbox) && {
    return same_state_op(uid_copy_command<Allocator>{seq, mailbox});
  }

  /// UID MOVE (@p seq names UIDs; UIDPLUS). Fails immediately with
  /// errc::capability_required when the server lacks MOVE.
  [[nodiscard]] auto uid_move(std::string_view seq,
                              std::string_view mailbox) && {
    return detail::state_op_sender{std::move(connection_),
                                   uid_move_command<Allocator>{seq, mailbox},
                                   take_snapshot(), "MOVE"};
  }

  /// CAPABILITY probe; refreshes the connection's capability cache.
  [[nodiscard]] auto capability() && {
    return detail::state_op_sender{
        std::move(connection_), capability_command<Allocator>{},
        [snapshot = take_snapshot()](std::error_code ec,
                                     imap_connection<Allocator> conn,
                                     capability_set<Allocator> caps) mutable {
          if (!ec) {
            conn.set_capabilities(caps);
          }
          return std::pair{std::move(caps), snapshot(ec, std::move(conn))};
        }};
  }

  /// NOOP (protocol keep-alive).
  [[nodiscard]] auto noop() && {
    return same_state_op(noop_command<Allocator>{});
  }

  /// LOGOUT: the connection is torn down; yields the terminal state.
  [[nodiscard]] auto logout() && {
    return detail::state_op_sender{
        std::move(connection_), logout_command<Allocator>{},
        [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
          // The completion runs inside the read dispatch; the connection's
          // destruction is detained to after the dispatch unwinds.
          detail::detain_connection(std::move(conn));
          return logout_state<Allocator>{};
        }};
  }

 private:
  /**
   * Moves the mailbox snapshot (and its live unsolicited hook) into a
   * rebuild closure usable as a state_op_sender successor factory.
   *
   * The closure rebuilds this state over the returned connection with the
   * newest pushed view: while the operation is in flight the moved hook
   * keeps feeding the moved pointee, so EXISTS/EXPUNGE that race the
   * command are not lost.
   */
  [[nodiscard]] auto take_snapshot() {
    return
        [mailbox_ptr = std::move(mailbox_),
         reg = std::move(snapshot_registration_)](
            std::error_code /*ec*/, imap_connection<Allocator> conn) mutable {
          // `reg` is captured for liveness only: it keeps the unsolicited hook
          // feeding mailbox_ptr until the rebuilt state registers its own.
          (void)reg;
          return selected_state{std::move(conn), std::move(*mailbox_ptr)};
        };
  }

  /// Helper for void-result operations whose successor is this same state.
  /// Only called from &&-qualified operations, which own the connection.
  template <class Command>
  [[nodiscard]] auto same_state_op(Command command) {
    return detail::state_op_sender{std::move(connection_), std::move(command),
                                   take_snapshot()};
  }

  /// Helper for resultful operations whose successor is this same state.
  template <class Command>
  [[nodiscard]] auto query_op(Command command) {
    return detail::state_op_sender{
        std::move(connection_), std::move(command),
        [snapshot = take_snapshot()](
            std::error_code ec, imap_connection<Allocator> conn,
            typename Command::result_type result) mutable {
          return std::pair{std::move(result), snapshot(ec, std::move(conn))};
        }};
  }

  imap_connection<Allocator> connection_;
  // Indirect so the unsolicited snapshot handler survives state moves:
  // the handler holds the pointee, the unique_ptr travels with the state.
  std::unique_ptr<mailbox_info<Allocator>> mailbox_;
  // Destroys (unregisters) before mailbox_ is released.
  std::optional<detail::registration<Allocator>> snapshot_registration_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_STATE_SELECTED_H_
