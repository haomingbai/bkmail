/**
 * @file authenticated.h
 * @brief IMAP Authenticated session state.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_AUTHENTICATED_H_
#define BKMAIL_IMAP_STATE_AUTHENTICATED_H_

#include <bkmail/common/mail.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/command/append_command.h>
#include <bkmail/imap/command/capability_command.h>
#include <bkmail/imap/command/create_command.h>
#include <bkmail/imap/command/delete_command.h>
#include <bkmail/imap/command/examine_command.h>
#include <bkmail/imap/command/list_command.h>
#include <bkmail/imap/command/logout_command.h>
#include <bkmail/imap/command/noop_command.h>
#include <bkmail/imap/command/rename_command.h>
#include <bkmail/imap/command/select_command.h>
#include <bkmail/imap/command/status_command.h>
#include <bkmail/imap/command/subscribe_command.h>
#include <bkmail/imap/command/unsubscribe_command.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/mailbox_entry.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/mailbox_status.h>
#include <bkmail/imap/state/detail/state_op_sender.h>
#include <bkmail/imap/state/logout.h>
#include <bkmail/imap/state/selected.h>

#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bkmail::imap {

/**
 * IMAP Authenticated session state.
 *
 * Owns the session (the imap_connection travels with the state chain).
 * Every operation consumes the state by rvalue and returns a lazy sender
 * completing with `set_value(std::error_code, [result,] next_state)` or
 * `set_stopped()`.
 */
template <class Allocator = std::allocator<std::byte>>
class authenticated_state {
 public:
  using allocator_type = Allocator;

  /// Wraps a live connection. Constructed by Layer-2 operation completions
  /// (login/authenticate/PREAUTH greeting/close).
  explicit authenticated_state(imap_connection<Allocator> connection)
      : connection_(std::move(connection)) {}

  authenticated_state(const authenticated_state&) = delete;
  authenticated_state& operator=(const authenticated_state&) = delete;
  authenticated_state(authenticated_state&&) = default;
  authenticated_state& operator=(authenticated_state&&) = default;

  /// Returns the session allocator.
  [[nodiscard]] Allocator get_allocator() const noexcept {
    return connection_.get_allocator();
  }

  /**
   * SELECT a mailbox. On OK the sender completes with
   * `set_value(error_code, selected_state)`: the mailbox_info snapshot is
   * absorbed into the state (mailbox()). On NO/failure the completion is
   * `set_value(error_code, authenticated_state)` — RFC 3501: a failed
   * SELECT selects no mailbox, so the session stays Authenticated.
   */
  [[nodiscard]] auto select(std::string_view mailbox) && {
    return detail::state_op_sender{
        std::move(connection_), select_command<Allocator>{mailbox},
        detail::branch_on_error(
            [](std::error_code /*ec*/, imap_connection<Allocator> conn,
               mailbox_info<Allocator> info) {
              return selected_state<Allocator>{std::move(conn),
                                               std::move(info)};
            },
            [](std::error_code /*ec*/, imap_connection<Allocator> conn,
               mailbox_info<Allocator> /*info*/) {
              return authenticated_state{std::move(conn)};
            })};
  }

  /// EXAMINE (read-only twin of select); same completion contract, with
  /// read_only set in the Selected state's snapshot.
  [[nodiscard]] auto examine(std::string_view mailbox) && {
    return detail::state_op_sender{
        std::move(connection_), examine_command<Allocator>{mailbox},
        detail::branch_on_error(
            [](std::error_code /*ec*/, imap_connection<Allocator> conn,
               mailbox_info<Allocator> info) {
              return selected_state<Allocator>{std::move(conn),
                                               std::move(info)};
            },
            [](std::error_code /*ec*/, imap_connection<Allocator> conn,
               mailbox_info<Allocator> /*info*/) {
              return authenticated_state{std::move(conn)};
            })};
  }

  /// LIST mailboxes under @p reference matching @p pattern ("" + "*"
  /// lists everything).
  [[nodiscard]] auto list(std::string_view reference,
                          std::string_view pattern) && {
    using command_type = list_command<Allocator>;
    return detail::state_op_sender{
        std::move(connection_), command_type{reference, pattern},
        [](std::error_code /*ec*/, imap_connection<Allocator> conn,
           typename command_type::result_type entries) {
          return std::pair{std::move(entries),
                           authenticated_state{std::move(conn)}};
        }};
  }

  /// STATUS on a mailbox without selecting it.
  [[nodiscard]] auto status(std::string_view mailbox, status_items items) && {
    return detail::state_op_sender{
        std::move(connection_), status_command<Allocator>{mailbox, items},
        [](std::error_code /*ec*/, imap_connection<Allocator> conn,
           mailbox_status<Allocator> status) {
          return std::pair{std::move(status),
                           authenticated_state{std::move(conn)}};
        }};
  }

  /// CREATE a mailbox.
  [[nodiscard]] auto create(std::string_view mailbox) && {
    return same_state_op(create_command<Allocator>{mailbox});
  }

  /// DELETE a mailbox.
  [[nodiscard]] auto delete_mailbox(std::string_view mailbox) && {
    return same_state_op(delete_command<Allocator>{mailbox});
  }

  /// RENAME a mailbox.
  [[nodiscard]] auto rename(std::string_view old_name,
                            std::string_view new_name) && {
    return same_state_op(rename_command<Allocator>{old_name, new_name});
  }

  /// SUBSCRIBE a mailbox.
  [[nodiscard]] auto subscribe(std::string_view mailbox) && {
    return same_state_op(subscribe_command<Allocator>{mailbox});
  }

  /// UNSUBSCRIBE a mailbox.
  [[nodiscard]] auto unsubscribe(std::string_view mailbox) && {
    return same_state_op(unsubscribe_command<Allocator>{mailbox});
  }

  /// APPEND a message to a mailbox (literal handshake handled by Layer 1).
  [[nodiscard]] auto append(std::string_view mailbox,
                            const mail<Allocator>& message,
                            flag_set flags = {}) && {
    return same_state_op(append_command<Allocator>{mailbox, message, flags});
  }

  /// CAPABILITY probe; refreshes the connection's capability cache.
  [[nodiscard]] auto capability() && {
    return detail::state_op_sender{
        std::move(connection_), capability_command<Allocator>{},
        [](std::error_code ec, imap_connection<Allocator> conn,
           capability_set<Allocator> caps) {
          if (!ec) {
            conn.set_capabilities(caps);
          }
          return std::pair{std::move(caps),
                           authenticated_state{std::move(conn)}};
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
  /// Helper for void-result operations whose successor is this same state.
  /// Only called from &&-qualified operations, which own the connection.
  template <class Command>
  [[nodiscard]] auto same_state_op(Command command) {
    return detail::state_op_sender{
        std::move(connection_), std::move(command),
        [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
          return authenticated_state{std::move(conn)};
        }};
  }

  imap_connection<Allocator> connection_;
};

namespace detail {

// Out-of-line definition of the close() successor factory declared in
// selected.h; selected_state is complete here, so the pair that carries
// the close() result can finally be materialized.
template <class Allocator>
authenticated_state<Allocator> close_successor<Allocator>::operator()(
    std::error_code /*ec*/, imap_connection<Allocator> conn) const {
  return authenticated_state{std::move(conn)};
}

}  // namespace detail

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_STATE_AUTHENTICATED_H_
