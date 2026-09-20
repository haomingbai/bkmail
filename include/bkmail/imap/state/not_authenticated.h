/**
 * @file not_authenticated.h
 * @brief IMAP Not-Authenticated session state.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_NOT_AUTHENTICATED_H_
#define BKMAIL_IMAP_STATE_NOT_AUTHENTICATED_H_

#include <bkmail/common/account_info.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/command/authenticate_command.h>
#include <bkmail/imap/command/capability_command.h>
#include <bkmail/imap/command/login_command.h>
#include <bkmail/imap/command/logout_command.h>
#include <bkmail/imap/command/noop_command.h>
#include <bkmail/imap/command/starttls_command.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/state/authenticated.h>
#include <bkmail/imap/state/detail/state_op_sender.h>
#include <bkmail/imap/state/logout.h>
#include <bnio/io_context.h>
#include <bnio/ssl.h>
#include <bnio/tcp.h>

#include <atomic>
#include <bexec/detail/operation.hpp>
#include <bexec/env.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bkmail::imap {

template <class Allocator>
class not_authenticated_state;

namespace detail {

/// Base64-encodes @p in into @p out (RFC 4648, standard alphabet, padded).
/// Drives authenticate_oauth2's XOAUTH2 initial response.
inline void base64_encode(std::string_view in, std::string& out) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.clear();
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 3 <= in.size()) {
    const std::uint32_t chunk =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1]))
         << 8) |
        static_cast<std::uint8_t>(in[i + 2]);
    out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    out.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    out.push_back(kAlphabet[chunk & 0x3f]);
    i += 3;
  }
  const std::size_t rest = in.size() - i;
  if (rest == 1) {
    const std::uint32_t chunk =
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16;
    out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const std::uint32_t chunk =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8);
    out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    out.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    out.push_back('=');
  }
}

/**
 * Operation state behind the STARTTLS upgrade (pinned, single-start).
 *
 * Three phases driven in sequence: the STARTTLS command on the plaintext
 * context, the client TLS handshake on the detached socket, and a fresh
 * CAPABILITY probe over the upgraded context (RFC 3501 invalidates the
 * pre-TLS capability view). Completes with the Not-Authenticated state over
 * TLS.
 *
 * The handshake phase has no Layer-1 tag and cannot be cancelled; a stop
 * request arriving during it (or racing the tagged reply into the latch)
 * is recorded in stop_latch_ and honoured at the next completion point
 * (handshakes are short). Command phases cancel through imap_context::cancel
 * as usual; the stopped completion itself is delivered from the context's
 * dispatch path (§6 contract), never inline on the requesting thread.
 */
template <class Allocator, class Receiver>
class start_tls_operation {
 public:
  start_tls_operation(
      imap_connection<Allocator>&& connection, bnio::ssl_context& ssl,
      Receiver&&
          receiver) noexcept(std::is_nothrow_move_constructible_v<Receiver>)
      : connection_(std::move(connection)),
        ssl_(&ssl),
        receiver_(std::move(receiver)),
        tag_(connection_.get_allocator()) {}

  start_tls_operation(const start_tls_operation&) = delete;
  start_tls_operation& operator=(const start_tls_operation&) = delete;
  start_tls_operation(start_tls_operation&&) = delete;
  start_tls_operation& operator=(start_tls_operation&&) = delete;

  void start() noexcept {
    const bool acquired = connection_.try_start_operation();
    assert(acquired &&
           "a Layer-2 operation is already in flight on this connection");
    (void)acquired;

    const auto token = bexec::get_stop_token(bexec::get_env(receiver_));
    if (token.stop_requested()) {
      connection_.finish_operation();
      bexec::set_stopped(std::move(receiver_));
      return;
    }

    phase_.store(phase::starttls, std::memory_order_release);
    connection_.with_context([this](auto& ctx) {
      tag_ =
          ctx.submit(starttls_command<Allocator>{},
                     [this](std::error_code ec) mutable { on_starttls(ec); });
      ctx.flush();
    });

    stop_callback_.emplace(token, stop_forwarder{this});
  }

 private:
  enum class phase { starttls, handshake, capability };

  struct stop_forwarder {
    start_tls_operation* op;

    void operator()() const noexcept {
      // Record the request FIRST: one that loses the completion claim
      // below is honoured at the next io-thread completion point
      // (on_starttls / on_handshake / on_capability) — never lost.
      op->stop_latch_.store(true, std::memory_order_release);
      if (op->phase_.load(std::memory_order_acquire) == phase::handshake) {
        // No cancellable Layer-1 command is in flight; on_handshake
        // delivers the stopped completion.
        return;
      }
      // Cancellable command phase (STARTTLS or CAPABILITY): drop the
      // reply on arrival, then complete.
      op->connection_.with_context(
          [tag = op->tag_](auto& ctx) mutable { ctx.cancel(tag); });
      if (op->completed_->exchange(true, std::memory_order_acq_rel)) {
        // on_starttls holds the claim (audit F5) or a rival completion
        // won; the latch above routes the request to the next
        // completion point.
        return;
      }
      op->connection_.finish_operation();
      op->stop_callback_.reset();
      // §6 contract: the receiver is completed from the context's
      // dispatch path, never inline on the requesting thread.
      post_stopped_delivery(op->connection_.get_allocator(),
                            op->connection_.io_context(), op->receiver_,
                            op->completed_);
    }
  };

  using tls_stream = bnio::ssl_stream<bnio::tcp::socket>;
  using handshake_sender = decltype(std::declval<tls_stream&>().async_handshake(
      std::declval<bnio::io_context&>().get_post_scheduler(),
      bnio::ssl_handshake_type::client));

  struct handshake_receiver {
    start_tls_operation* op;

    void set_value(std::error_code ec) noexcept { op->on_handshake(ec); }
    void set_stopped() noexcept {
      op->on_handshake(std::make_error_code(std::errc::operation_canceled));
    }
  };

  using handshake_op = decltype(bexec::connect(
      std::declval<handshake_sender>(), std::declval<handshake_receiver>()));

  /// STARTTLS tagged reply: on OK, detach the socket and start the
  /// client handshake; anything else keeps the plaintext state.
  void on_starttls(std::error_code ec) noexcept {
    // Audit F5: claim via exchange, not a plain load — winning closes the
    // race window against the stop forwarder for the whole reply
    // handling; losing means the forwarder (or a rival) owns the
    // operation and the reply is dropped.
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (stop_latch_.load(std::memory_order_acquire)) {
      // A stop request landed before this reply (the forwarder already
      // cancelled the command): deliver it. Runs on the io dispatch path.
      stop_callback_.reset();
      connection_.finish_operation();
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    if (ec) {
      complete(ec);
      return;
    }
    phase_.store(phase::handshake, std::memory_order_release);
    tls_stream_ = connection_.upgrade_to_tls_stream(*ssl_);
    handshake_op_.emplace(std::in_place, [this] {
      return bexec::connect(tls_stream_->async_handshake(
                                connection_.io_context().get_post_scheduler(),
                                bnio::ssl_handshake_type::client),
                            handshake_receiver{this});
    });
    bexec::start(*handshake_op_);
  }

  /// Handshake reply: on OK, build the TLS context and re-probe
  /// CAPABILITY; on failure the session is dead (TLS errors arrive in the
  /// OpenSSL error category) and the retained state carries the dead
  /// connection, so the failure surfaces again on the next operation.
  ///
  /// The completed_ claim has been held since on_starttls won its
  /// exchange (audit F5) — handshake_op_ is only armed there, so no rival
  /// completion can exist here and no re-claim is needed.
  void on_handshake(std::error_code ec) noexcept {
    if (stop_latch_.load(std::memory_order_acquire)) {
      // A stop request landed during the (uncancellable) handshake: the
      // operation completes stopped. Runs on the io dispatch path.
      stop_callback_.reset();
      connection_.finish_operation();
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    // Keep the connection object whole even on failure: the dead TLS
    // stream is emplaced so the session dies through the normal path.
    connection_.emplace_tls_context(std::move(*tls_stream_));
    tls_stream_.reset();
    if (ec) {
      complete(ec);
      return;
    }
    phase_.store(phase::capability, std::memory_order_release);
    connection_.with_context([this](auto& ctx) {
      tag_ = ctx.submit(capability_command<Allocator>{},
                        [this](std::error_code cap_ec,
                               capability_set<Allocator> caps) mutable {
                          on_capability(cap_ec, std::move(caps));
                        });
      ctx.flush();
    });
  }

  /// CAPABILITY reply over the upgraded context: refresh the cache and
  /// hand the Not-Authenticated state (over TLS) back. A stop request
  /// that latched during the probe (the forwarder cancelled the command
  /// in passing) is honoured here.
  void on_capability(std::error_code ec,
                     capability_set<Allocator> caps) noexcept {
    if (stop_latch_.load(std::memory_order_acquire)) {
      stop_callback_.reset();
      connection_.finish_operation();
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    if (!ec) {
      connection_.set_capabilities(std::move(caps));
    }
    complete(ec);
  }

  /// Value delivery; callers hold the completed_ claim (audit F5), so a
  /// once-only delivery needs no re-claim here. Runs on the io dispatch
  /// path.
  void complete(std::error_code ec) noexcept {
    stop_callback_.reset();
    connection_.finish_operation();
    bexec::set_value(
        std::move(receiver_), ec,
        not_authenticated_state<Allocator>{std::move(connection_)});
  }

  imap_connection<Allocator> connection_;
  bnio::ssl_context* ssl_;  // borrowed; must outlive the upgrade
  Receiver receiver_;
  bkmail::detail::string_of<Allocator> tag_;
  std::optional<tls_stream> tls_stream_;
  std::optional<bexec::detail::pass_through_operation<handshake_op>>
      handshake_op_;
  stop_callback_box<Receiver, stop_forwarder> stop_callback_;
  std::atomic<phase> phase_ = phase::starttls;
  // Stop requests recorded here are honoured at the next completion
  // point when the claiming stop path lost its race (see stop_forwarder).
  std::atomic<bool> stop_latch_ = false;
  // Shared so the posted stopped-delivery task can hold a flag copy.
  std::shared_ptr<std::atomic<bool>> completed_ =
      std::make_shared<std::atomic<bool>>(false);
};

/// Sender behind not_authenticated_state::start_tls().
template <class Allocator>
class start_tls_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, not_authenticated_state<Allocator>),
      bexec::set_stopped_t()>;

  start_tls_sender(imap_connection<Allocator> connection,
                   bnio::ssl_context& ssl)
      : connection_(std::move(connection)), ssl_(&ssl) {}

  template <class Receiver>
  auto connect(Receiver receiver) && noexcept(
      std::is_nothrow_constructible_v<start_tls_operation<Allocator, Receiver>,
                                      imap_connection<Allocator>&&,
                                      Receiver&&>) {
    return start_tls_operation<Allocator, Receiver>{std::move(connection_),
                                                    *ssl_, std::move(receiver)};
  }

 private:
  imap_connection<Allocator> connection_;
  bnio::ssl_context* ssl_;  // borrowed
};

}  // namespace detail

/**
 * IMAP Not-Authenticated session state.
 *
 * Owns the session (the imap_connection travels with the state chain).
 * Every operation consumes the state by rvalue and returns a lazy sender
 * completing with `set_value(std::error_code, [result,] next_state)` or
 * `set_stopped()`.
 */
template <class Allocator = std::allocator<std::byte>>
class not_authenticated_state {
 public:
  using allocator_type = Allocator;

  /// Wraps a live connection. Constructed by bkmail::async_connect* and by
  /// Layer-2 operation completions.
  explicit not_authenticated_state(imap_connection<Allocator> connection)
      : connection_(std::move(connection)) {}

  not_authenticated_state(const not_authenticated_state&) = delete;
  not_authenticated_state& operator=(const not_authenticated_state&) = delete;
  not_authenticated_state(not_authenticated_state&&) = default;
  not_authenticated_state& operator=(not_authenticated_state&&) = default;

  /// Returns the session allocator.
  [[nodiscard]] Allocator get_allocator() const noexcept {
    return connection_.get_allocator();
  }

  /**
   * LOGIN with a password. The sender completes with
   * `set_value(error_code, authenticated_state)` on OK and with
   * `set_value(error_code, not_authenticated_state)` — the retained
   * current state — on NO/BAD or a transport failure.
   */
  [[nodiscard]] auto login(const account_info<Allocator>& account) && {
    return detail::state_op_sender{std::move(connection_),
                                   login_command<Allocator>{account},
                                   detail::auth_branches<Allocator>()};
  }

  /**
   * AUTHENTICATE with a SASL mechanism. The command type models SASL-IR,
   * but the state layer currently always uses the two-step challenge
   * exchange (usage.md §8) — @p initial_response is carried as the SASL
   * payload, not as an initial response on the command line. Same
   * completion contract as login(): Authenticated on OK, the retained
   * Not-Authenticated state otherwise.
   */
  [[nodiscard]] auto authenticate(std::string_view mechanism,
                                  std::string_view initial_response) && {
    return detail::state_op_sender{
        std::move(connection_),
        authenticate_command<Allocator>{mechanism, initial_response},
        detail::auth_branches<Allocator>()};
  }

  /**
   * XOAUTH2 convenience: builds the `user=..auth=Bearer ..` initial
   * response and runs AUTHENTICATE XOAUTH2. Same completion contract as
   * login(): Authenticated on OK, the retained Not-Authenticated state
   * otherwise.
   */
  [[nodiscard]] auto authenticate_oauth2(std::string_view user,
                                         std::string_view token) && {
    std::string initial_response;
    initial_response.reserve(user.size() + token.size() + 24);
    initial_response += "user=";
    initial_response += user;
    initial_response += '\x01';
    initial_response += "auth=Bearer ";
    initial_response += token;
    initial_response += '\x01';
    initial_response += '\x01';
    std::string encoded;
    detail::base64_encode(initial_response, encoded);
    return detail::state_op_sender{
        std::move(connection_),
        authenticate_command<Allocator>{"XOAUTH2", encoded},
        detail::auth_branches<Allocator>()};
  }

  /**
   * STARTTLS upgrade (RFC 3501 §6.2.1): STARTTLS command, client handshake
   * over the detached socket, then a fresh CAPABILITY probe. Yields the
   * Not-Authenticated state over TLS.
   *
   * @pre The server advertised STARTTLS (capabilities()).
   * @param ssl borrowed; must outlive the upgraded connection.
   */
  [[nodiscard]] auto start_tls(bnio::ssl_context& ssl) && {
    return detail::start_tls_sender<Allocator>{std::move(connection_), ssl};
  }

  /// CAPABILITY probe; refreshes the connection's capability cache.
  [[nodiscard]] auto capability() && {
    return detail::state_op_sender{
        std::move(connection_), capability_command<Allocator>{},
        detail::capability_successor<Allocator>(
            detail::same_state_successor<not_authenticated_state,
                                         Allocator>())};
  }

  /// NOOP (protocol keep-alive).
  [[nodiscard]] auto noop() && {
    return same_state_op(noop_command<Allocator>{});
  }

  /// LOGOUT: the connection is torn down; yields the terminal state.
  [[nodiscard]] auto logout() && {
    return detail::state_op_sender{std::move(connection_),
                                   logout_command<Allocator>{},
                                   detail::logout_successor<Allocator>()};
  }

 private:
  /// Helper for void-result operations whose successor is this same state.
  /// Only called from &&-qualified operations, which own the connection.
  template <class Command>
  [[nodiscard]] auto same_state_op(Command command) {
    return detail::state_op_sender{
        std::move(connection_), std::move(command),
        detail::same_state_successor<not_authenticated_state, Allocator>()};
  }

  imap_connection<Allocator> connection_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_STATE_NOT_AUTHENTICATED_H_
