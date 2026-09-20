/**
 * @file connect.h
 * @brief IMAP session connect entry points.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * bkmail::async_connect / bkmail::async_connect_tls are self-contained
 * connect entry points (docs/code_layout.md D6): DNS resolve, TCP connect,
 * the TLS handshake for the implicit-TLS variant, greeting consumption,
 * and the initial CAPABILITY probe all live inside the returned sender.
 * Callers receive a ready session, not homework.
 */

#pragma once
#ifndef BKMAIL_CONNECT_H_
#define BKMAIL_CONNECT_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/command/capability_command.h>
#include <bkmail/imap/detail/unsolicited_table.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/state/authenticated.h>
#include <bkmail/imap/state/logout.h>
#include <bkmail/imap/state/not_authenticated.h>
#include <bkmail/imap/unsolicited_event.h>
#include <bnio/io_context.h>
#include <bnio/ip.h>
#include <bnio/ssl.h>
#include <bnio/tcp.h>

#include <array>
#include <atomic>
#include <bexec/detail/operation.hpp>
#include <bexec/env.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace bkmail {

namespace detail {

/// Endpoints resolved per connect attempt (usage.md §3.2 shape).
///
/// Deliberately first-endpoint-only: the connection attempt opens the
/// resolver's FIRST endpoint and does not fall back to the rest of the
/// list (multi-endpoint failover is a future feature, not an oversight).
/// The cap bounds the fixed dns_result_view storage; hosts resolving to
/// more records than the cap simply expose fewer candidates to that
/// never-consumed tail.
inline constexpr std::size_t kMaxResolveEndpoints = 8;

/// Greeting forms that select the initial session state.
enum class greeting_kind { ok, preauth, bye };

/// Maps the greeting's unsolicited report to its kind: a BYE greeting, an
/// OK/PREAUTH greeting report, or — for any other event (which cannot
/// arrive before the greeting in a well-formed session) — the OK default.
template <class Allocator>
[[nodiscard]] greeting_kind greeting_kind_of(
    const imap::unsolicited_event<Allocator>& event) noexcept {
  if (std::holds_alternative<imap::bye_event<Allocator>>(event)) {
    return greeting_kind::bye;
  }
  if (const auto* greeting =
          std::get_if<imap::greeting_event<Allocator>>(&event)) {
    return greeting->status == imap::response_status::preauth
               ? greeting_kind::preauth
               : greeting_kind::ok;
  }
  return greeting_kind::ok;
}

/**
 * Operation state behind the connect senders (pinned, single-start).
 *
 * Drives the phases resolve -> TCP connect -> [TLS handshake] -> session
 * bring-up in sequence. Session bring-up arms the Layer-1 context, listens
 * for the greeting through the unsolicited path, and runs the initial
 * CAPABILITY probe; the probe's tagged completion doubles as the
 * "greeting consumed" signal (the server answers commands in order, so the
 * greeting line has been dispatched by then).
 *
 * Stop requests cancel the in-flight phase when it has a Layer-1 tag
 * (the CAPABILITY probe); the bnio phases (resolve/connect/handshake)
 * take no stop token, so a request landing there is latched and honoured
 * as soon as the phase completes.
 */
template <class Allocator, class Receiver, bool UseTls>
class connect_operation {
 public:
  using tls_stream = bnio::ssl_stream<bnio::tcp::socket>;

  connect_operation(std::string&& host, std::string&& service,
                    bnio::io_context& ioc, bnio::ssl_context* ssl,
                    const Allocator& alloc, Receiver&& receiver)
      : receiver_(std::move(receiver)),
        ioc_(&ioc),
        ssl_(ssl),
        alloc_(alloc),
        host_(std::move(host)),
        service_(std::move(service)),
        tag_(alloc) {}

  connect_operation(const connect_operation&) = delete;
  connect_operation& operator=(const connect_operation&) = delete;
  connect_operation(connect_operation&&) = delete;
  connect_operation& operator=(connect_operation&&) = delete;

  void start() noexcept {
    const auto token = bexec::get_stop_token(bexec::get_env(receiver_));
    if (token.stop_requested()) {
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    phase_.store(phase::resolve, std::memory_order_release);
    // pass_through_operation emplaces the pinned child op through an
    // in-place factory (pinned op states are not movable).
    resolve_op_.emplace(std::in_place, [this] {
      return bexec::connect(
          ioc_->get_post_scheduler().async_resolve(
              host_, service_, bnio::dns_result_view{endpoints_}),
          resolve_receiver{this});
    });
    bexec::start(*resolve_op_);
    stop_callback_.emplace(token, stop_forwarder{this});
  }

 private:
  enum class phase { resolve, tcp_connect, handshake, session };

  struct stop_forwarder {
    connect_operation* op;

    void operator()() const noexcept {
      if (op->phase_.load(std::memory_order_acquire) != phase::session) {
        // bnio phases take no stop token; latch and let the phase
        // completion deliver the stopped signal.
        op->stop_requested_.store(true, std::memory_order_release);
        return;
      }
      if (op->completed_->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      op->greeting_registration_.reset();
      op->connection_->with_context(
          [tag = op->tag_](auto& ctx) mutable { ctx.cancel(tag); });
      op->stop_callback_.reset();
      // §6 contract: the receiver is completed from the context's
      // dispatch path, never inline on the requesting thread.
      imap::detail::post_stopped_delivery(op->alloc_, *op->ioc_, op->receiver_,
                                          op->completed_);
    }
  };

  struct resolve_receiver {
    connect_operation* op;

    void set_value(std::error_code ec, std::size_t count) noexcept {
      op->on_resolve(ec, count);
    }
    void set_stopped() noexcept { op->on_phase_stopped(); }
  };

  struct tcp_connect_receiver {
    connect_operation* op;

    void set_value(std::error_code ec) noexcept { op->on_tcp_connect(ec); }
    void set_stopped() noexcept { op->on_phase_stopped(); }
  };

  struct handshake_receiver {
    connect_operation* op;

    void set_value(std::error_code ec) noexcept { op->on_handshake(ec); }
    void set_stopped() noexcept { op->on_phase_stopped(); }
  };

  using resolve_sender =
      decltype(std::declval<bnio::io_context&>()
                   .get_post_scheduler()
                   .async_resolve(std::declval<std::string_view>(),
                                  std::declval<std::string_view>(),
                                  std::declval<bnio::dns_result_view>()));
  using tcp_connect_sender =
      decltype(std::declval<bnio::tcp::socket&>().async_connect(
          std::declval<bnio::io_context&>().get_post_scheduler(),
          std::declval<const bnio::ip::endpoint&>()));
  using handshake_sender = decltype(std::declval<tls_stream&>().async_handshake(
      std::declval<bnio::io_context&>().get_post_scheduler(),
      bnio::ssl_handshake_type::client));

  using resolve_op = decltype(bexec::connect(std::declval<resolve_sender>(),
                                             std::declval<resolve_receiver>()));
  using tcp_connect_op =
      decltype(bexec::connect(std::declval<tcp_connect_sender>(),
                              std::declval<tcp_connect_receiver>()));
  using handshake_op = decltype(bexec::connect(
      std::declval<handshake_sender>(), std::declval<handshake_receiver>()));

  /// Delivers the latched stop request, if any. Returns true when the
  /// operation is finished and the phase callback must bail out.
  bool deliver_latched_stop() noexcept {
    if (!stop_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    if (!completed_->exchange(true, std::memory_order_acq_rel)) {
      stop_callback_.reset();
      bexec::set_stopped(std::move(receiver_));
    }
    return true;
  }

  void on_resolve(std::error_code ec, std::size_t count) noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    resolve_op_.reset();
    if (!ec && count == 0) {
      ec = std::make_error_code(std::errc::host_unreachable);
    }
    if (ec) {
      complete(ec);
      return;
    }
    phase_.store(phase::tcp_connect, std::memory_order_release);
    const bnio::ip::endpoint& endpoint = endpoints_[0];
    if (const std::error_code open_ec =
            socket_.open(endpoint.address().is_v4() ? bnio::ip::tcp::v4()
                                                    : bnio::ip::tcp::v6())) {
      complete(open_ec);
      return;
    }
    tcp_connect_op_.emplace(std::in_place, [this, &endpoint] {
      return bexec::connect(
          socket_.async_connect(ioc_->get_post_scheduler(), endpoint),
          tcp_connect_receiver{this});
    });
    bexec::start(*tcp_connect_op_);
  }

  void on_tcp_connect(std::error_code ec) noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    tcp_connect_op_.reset();
    if (ec) {
      complete(ec);
      return;
    }
    if constexpr (UseTls) {
      phase_.store(phase::handshake, std::memory_order_release);
      tls_stream_.emplace(std::move(socket_), *ssl_);
      handshake_op_.emplace(std::in_place, [this] {
        return bexec::connect(
            tls_stream_->async_handshake(ioc_->get_post_scheduler(),
                                         bnio::ssl_handshake_type::client),
            handshake_receiver{this});
      });
      bexec::start(*handshake_op_);
    } else {
      begin_session();
    }
  }

  void on_handshake(std::error_code ec) noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    handshake_op_.reset();
    if (ec) {
      // TLS failures arrive in the OpenSSL error category.
      complete(ec);
      return;
    }
    begin_session();
  }

  /// bnio phase completed stopped without a latched request (e.g. the
  /// io_context stopped): surface it as a generic cancellation on the
  /// value channel.
  void on_phase_stopped() noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    complete(std::make_error_code(std::errc::operation_canceled));
  }

  /// Arms the Layer-1 context, subscribes for the greeting, and starts
  /// the initial CAPABILITY probe. The session phase is published only
  /// after every moving part is in place, so the stop path can rely on
  /// greeting_registration_/tag_ whenever it observes it.
  void begin_session() noexcept {
    if constexpr (UseTls) {
      connection_.emplace(std::move(*tls_stream_), *ioc_, alloc_);
      tls_stream_.reset();
    } else {
      connection_.emplace(std::move(socket_), *ioc_, alloc_);
    }
    connection_->with_context([this](auto& ctx) {
      greeting_registration_.emplace(ctx.on_unsolicited(
          [this](const imap::unsolicited_event<Allocator>& event) {
            on_greeting_event(event);
          }));
      tag_ = ctx.submit(imap::capability_command<Allocator>{},
                        [this](std::error_code ec,
                               imap::capability_set<Allocator> caps) mutable {
                          on_capability(ec, std::move(caps));
                        });
      ctx.flush();
    });
    phase_.store(phase::session, std::memory_order_release);
  }

  /// Greeting report through the unsolicited path: BYE is connection-fatal
  /// before the session even starts; only the FIRST greeting event decides
  /// the initial state (later untagged OKs are resp-code carriers and must
  /// not overwrite a PREAUTH verdict).
  void on_greeting_event(
      const imap::unsolicited_event<Allocator>& event) noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    const greeting_kind kind = greeting_kind_of(event);
    if (kind == greeting_kind::bye) {
      complete(make_error_code(errc::server_bye));
      return;
    }
    if (!greeting_seen_ &&
        std::holds_alternative<imap::greeting_event<Allocator>>(event)) {
      greeting_seen_ = true;
      greeting_ = kind;
    }
  }

  /// CAPABILITY probe reply: on success the set is cached on the
  /// connection and the greeting is considered consumed.
  void on_capability(std::error_code ec,
                     imap::capability_set<Allocator> caps) noexcept {
    if (deliver_latched_stop()) {
      return;
    }
    if (!ec) {
      connection_->set_capabilities(std::move(caps));
    }
    complete(ec);
  }

  /// Terminal delivery: each outcome rides its own set_value signature.
  /// A consumed greeting picks the session state by its kind; every
  /// failure (DNS/connect/TLS before the greeting, or a BYE greeting)
  /// completes with the terminal logout_state alongside the error code.
  void complete(std::error_code ec) noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stop_callback_.reset();
    // greeting_registration_ is deliberately NOT reset here: complete()
    // may run inside the unsolicited greeting handler itself (the BYE
    // path), and destroying a registration from within its own dispatch is
    // the table's business. The member's destruction retires the hook.
    if (!ec) {
      // The CAPABILITY probe completed: the greeting was consumed (the
      // server answers commands in order), so connection_ is live.
      if (greeting_ == greeting_kind::preauth) {
        bexec::set_value(
            std::move(receiver_), ec,
            imap::authenticated_state<Allocator>{std::move(*connection_)});
      } else {
        bexec::set_value(
            std::move(receiver_), ec,
            imap::not_authenticated_state<Allocator>{std::move(*connection_)});
      }
    } else {
      // No session outlives a failed connect: the connection (when the
      // session phase was reached) dies with this operation state.
      bexec::set_value(std::move(receiver_), ec,
                       imap::logout_state<Allocator>{});
    }
  }

  Receiver receiver_;
  bnio::io_context* ioc_;   // borrowed
  bnio::ssl_context* ssl_;  // borrowed; only used when UseTls
  [[no_unique_address]] Allocator alloc_{};
  std::string host_;
  std::string service_;
  std::array<bnio::ip::endpoint, kMaxResolveEndpoints> endpoints_{};
  bnio::tcp::socket socket_;
  std::optional<tls_stream> tls_stream_;
  std::optional<imap::imap_connection<Allocator>> connection_;
  std::optional<imap::detail::registration<Allocator>> greeting_registration_;
  bkmail::detail::string_of<Allocator> tag_;
  std::optional<bexec::detail::pass_through_operation<resolve_op>> resolve_op_;
  std::optional<bexec::detail::pass_through_operation<tcp_connect_op>>
      tcp_connect_op_;
  std::optional<bexec::detail::pass_through_operation<handshake_op>>
      handshake_op_;
  imap::detail::stop_callback_box<Receiver, stop_forwarder> stop_callback_;
  std::atomic<phase> phase_ = phase::resolve;
  std::atomic<bool> stop_requested_ = false;
  // Shared so the posted stopped-delivery task can hold a flag copy.
  std::shared_ptr<std::atomic<bool>> completed_ =
      std::make_shared<std::atomic<bool>>(false);
  greeting_kind greeting_ = greeting_kind::ok;  // ioc-thread domain
  bool greeting_seen_ = false;                  // ioc-thread domain
};

/// Sender type behind bkmail::async_connect / async_connect_tls.
///
/// One set_value signature per server outcome: OK greeting ->
/// not_authenticated_state, PREAUTH greeting -> authenticated_state, and
/// any failure (DNS/connect/TLS before the greeting, BYE greeting) ->
/// logout_state with the error code.
template <class Allocator, bool UseTls>
class connect_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code,
                         imap::not_authenticated_state<Allocator>),
      bexec::set_value_t(std::error_code, imap::authenticated_state<Allocator>),
      bexec::set_value_t(std::error_code, imap::logout_state<Allocator>),
      bexec::set_stopped_t()>;

  connect_sender(std::string host, std::string service, bnio::io_context& ioc,
                 bnio::ssl_context* ssl, const Allocator& alloc)
      : host_(std::move(host)),
        service_(std::move(service)),
        ioc_(&ioc),
        ssl_(ssl),
        alloc_(alloc) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return connect_operation<Allocator, Receiver, UseTls>{
        std::move(host_), std::move(service_), *ioc_, ssl_,
        alloc_,           std::move(receiver)};
  }

 private:
  std::string host_;
  std::string service_;
  bnio::io_context* ioc_;   // borrowed
  bnio::ssl_context* ssl_;  // borrowed; null for the plaintext variant
  [[no_unique_address]] Allocator alloc_{};
};

}  // namespace detail

/**
 * Connects to an IMAP server over plaintext (typically port 143, usually
 * upgraded later with not_authenticated_state::start_tls()).
 *
 * Resolves @p host/@p service, connects, consumes the server greeting, and
 * runs the initial CAPABILITY probe. The returned lazy sender publishes
 * one set_value signature per outcome: an `OK` greeting completes with
 * `set_value(std::error_code, not_authenticated_state<Allocator>)`, a
 * `PREAUTH` greeting with `set_value(std::error_code,
 * authenticated_state<Allocator>)`, and a `BYE` greeting or any
 * pre-greeting failure with `set_value(std::error_code,
 * logout_state<Allocator>)` (ec == errc::server_bye for the BYE case);
 * cancellation completes with `set_stopped()`. Consumers merge the
 * alternatives themselves where convenient (bexec::into_variant /
 * bexec::this_thread::sync_wait_with_variant).
 *
 * @param ioc borrowed; must outlive the operation (and a thread must run
 *            it while the sender is in flight).
 */
template <class Allocator = std::allocator<std::byte>>
[[nodiscard]] auto async_connect(std::string_view host,
                                 std::string_view service,
                                 bnio::io_context& ioc,
                                 const Allocator& alloc = Allocator{}) {
  return detail::connect_sender<Allocator, false>{
      std::string{host}, std::string{service}, ioc, nullptr, alloc};
}

/**
 * Connects to an IMAP server over implicit TLS (typically port 993).
 *
 * Same as async_connect, with the client TLS handshake performed between
 * TCP connect and greeting consumption.
 *
 * @param tls borrowed; must outlive the connection built by this sender.
 */
template <class Allocator = std::allocator<std::byte>>
[[nodiscard]] auto async_connect_tls(std::string_view host,
                                     std::string_view service,
                                     bnio::io_context& ioc,
                                     bnio::ssl_context& tls,
                                     const Allocator& alloc = Allocator{}) {
  return detail::connect_sender<Allocator, true>{
      std::string{host}, std::string{service}, ioc, &tls, alloc};
}

}  // namespace bkmail

#endif  // BKMAIL_CONNECT_H_
