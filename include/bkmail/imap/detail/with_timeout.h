/**
 * @file with_timeout.h
 * @brief Watchdog adaptor over Layer-1-style senders.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * detail::with_timeout(sender, ioc, duration) races the wrapped sender
 * against a bnio::steady_timer. The timer is context-bound at construction
 * (bnio steady_timer cannot borrow another context), so it is created from
 * the io_context current at connect() time — re-armed per operation, never
 * cached on the sender.
 *
 * The adaptor wraps senders whose value completions all carry
 * std::error_code as the first argument (the bkmail/bnio completion
 * shape: `set_value(std::error_code, [payload...])` / `set_stopped()`),
 * mirroring every value signature of the wrapped sender. A value
 * completion is forwarded verbatim, with the error code rewritten to
 * std::errc::timed_out when the watchdog fired first. On timeout the
 * wrapped operation is asked to stop through its receiver-environment
 * stop token; when that stop wins the race, a payload-less sender
 * completes `set_value(std::errc::timed_out)`, while a payload-carrying
 * sender (e.g. a Layer-2 state operation, whose retained state died with
 * the stopped inner operation) completes set_stopped() — the timeout
 * verdict cannot fabricate a state. Cancellation of the adaptor itself
 * forwards the stop request and stays a stopped completion.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_WITH_TIMEOUT_H_
#define BKMAIL_IMAP_DETAIL_WITH_TIMEOUT_H_

#include <bkmail/imap/operation_base.h>
#include <bnio/io_context.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation.hpp>
#include <bexec/env.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bkmail::imap::detail {

// ---- Value-signature analysis of the wrapped sender ---------------------

template <class Tuple>
struct ec_first : std::false_type {};

template <class First, class... Rest>
struct ec_first<std::tuple<First, Rest...>>
    : std::is_same<First, std::error_code> {};

template <class Tuples>
struct value_tuples_check;

template <class... Tuples>
struct value_tuples_check<bexec::type_list<Tuples...>> {
  /// Every value completion carries std::error_code as its first argument.
  static constexpr bool all_ec_first = (ec_first<Tuples>::value && ...);
  /// At least one value completion carries a payload beyond the code.
  static constexpr bool any_payload = ((std::tuple_size_v<Tuples> > 1U) || ...);
};

template <class Tuple>
struct value_signature_of;

template <class... Args>
struct value_signature_of<std::tuple<Args...>> {
  using type = bexec::set_value_t(Args...);
};

/// Rebuilds completion signatures mirroring the wrapped sender's value
/// completions plus the stopped channel.
template <class Tuples>
struct mirror_signatures;

template <class... Tuples>
struct mirror_signatures<bexec::type_list<Tuples...>> {
  using type =
      bexec::completion_signatures<typename value_signature_of<Tuples>::type...,
                                   bexec::set_stopped_t()>;
};

template <class Sender>
using wrapped_value_tuples_t =
    bexec::value_types_of_t<Sender, bexec::empty_env, std::tuple,
                            bexec::type_list>;

/// Operation state behind with_timeout (pinned, single-start).
template <class Sender, class Receiver>
class with_timeout_operation {
 public:
  with_timeout_operation(Sender&& sender, bnio::io_context& ioc,
                         bnio::steady_timer::duration timeout,
                         Receiver&& receiver)
      : sender_(std::move(sender)),
        receiver_(std::move(receiver)),
        timer_(ioc, timeout),
        completed_(std::make_shared<std::atomic<bool>>(false)) {}

  with_timeout_operation(const with_timeout_operation&) = delete;
  with_timeout_operation& operator=(const with_timeout_operation&) = delete;
  with_timeout_operation(with_timeout_operation&&) = delete;
  with_timeout_operation& operator=(with_timeout_operation&&) = delete;

  void start() noexcept {
    // pass_through_operation emplaces each pinned child op through an
    // in-place factory (pinned op states are not movable).
    inner_op_.emplace(std::in_place, [this] {
      return bexec::connect(std::move(sender_), inner_receiver{this});
    });
    // The watchdog wait is self-owning (io_box): its completion may be
    // queued by timer_.cancel() AFTER this operation state died, so it
    // must not live inside this object — the receiver guards every touch
    // of `this` with the shared completed flag and disposes the box.
    auto* timer_box =
        make_io_box(std::allocator<std::byte>{}, timer_.async_wait(),
                    [this](io_box_base* self) {
                      return timer_wait_receiver{self, this, completed_};
                    });
    bexec::start(*inner_op_);
    timer_box->start();

    const auto token = bexec::get_stop_token(bexec::get_env(receiver_));
    stop_callback_.emplace(token, stop_forwarder{this});
  }

 private:
  struct stop_forwarder {
    with_timeout_operation* op;

    void operator()() const noexcept {
      // Forward the request to the wrapped operation; its completion path
      // finishes the adaptor (and stays a stopped completion).
      op->stop_source_.request_stop();
    }
  };

  // Injects the adaptor's stop source into the wrapped sender's
  // environment, so timeout/outer-stop reaches Layer 1.
  using inner_env = bexec::env_with_stop_token<decltype(bexec::get_env(
      std::declval<const Receiver&>()))>;

  struct inner_receiver {
    with_timeout_operation* op;

    [[nodiscard]] inner_env get_env() const noexcept {
      return inner_env{op->stop_source_.get_token(),
                       bexec::get_env(op->receiver_)};
    }

    template <class... Args>
    void set_value(std::error_code ec, Args&&... args) noexcept {
      op->on_inner_value(ec, std::forward<Args>(args)...);
    }
    void set_stopped() noexcept { op->on_inner_stopped(); }
  };

  // Self-owning watchdog wait receiver (io_box). The wait's completion is
  // queued by timer_.cancel() from the inner completion paths, possibly
  // AFTER this operation state died: `op` may only be touched while the
  // shared flag says the adaptor never completed.
  struct timer_wait_receiver {
    io_box_base* box;
    with_timeout_operation* op;
    std::shared_ptr<std::atomic<bool>> completed;

    void set_value(std::error_code ec) noexcept {
      io_box_base* self = box;
      if (!ec && !completed->load(std::memory_order_acquire)) {
        op->on_timeout();
      }
      self->dispose();
    }

    void set_stopped() noexcept { box->dispose(); }
  };

  using inner_op = decltype(bexec::connect(std::declval<Sender>(),
                                           std::declval<inner_receiver>()));

  using env_type = decltype(bexec::get_env(std::declval<const Receiver&>()));
  using stop_token_type =
      decltype(bexec::get_stop_token(std::declval<const env_type&>()));
  using stop_callback =
      typename stop_token_type::template callback_type<stop_forwarder>;

  void on_timeout() noexcept {
    timed_out_.store(true, std::memory_order_release);
    stop_source_.request_stop();
  }

  template <class... Args>
  void on_inner_value(std::error_code ec, Args&&... args) noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stop_callback_.reset();
    timer_.cancel();
    // A value landing after the timeout fired is still reported as the
    // timeout: the watchdog, not the race winner, owns the verdict.
    if (timed_out_.load(std::memory_order_acquire)) {
      ec = std::make_error_code(std::errc::timed_out);
    }
    bexec::set_value(std::move(receiver_), ec, std::forward<Args>(args)...);
  }

  void on_inner_stopped() noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stop_callback_.reset();
    timer_.cancel();
    if (timed_out_.load(std::memory_order_acquire)) {
      // Timeout-driven stop becomes the watchdog verdict. A payload-less
      // sender reports it on the value channel; a payload-carrying sender
      // cannot fabricate the payload (the retained state died with the
      // stopped inner operation), so the completion stays stopped.
      if constexpr (!value_tuples_check<
                        wrapped_value_tuples_t<Sender>>::any_payload) {
        bexec::set_value(std::move(receiver_),
                         std::make_error_code(std::errc::timed_out));
      } else {
        bexec::set_stopped(std::move(receiver_));
      }
    } else {
      bexec::set_stopped(std::move(receiver_));
    }
  }

  Sender sender_;
  Receiver receiver_;
  bnio::steady_timer timer_;
  std::optional<bexec::detail::pass_through_operation<inner_op>> inner_op_;
  bexec::inplace_stop_source stop_source_;
  std::optional<stop_callback> stop_callback_;
  std::shared_ptr<std::atomic<bool>> completed_;
  std::atomic<bool> timed_out_ = false;
};

/// Sender type produced by with_timeout().
template <class Sender>
class with_timeout_sender {
  using value_tuples = wrapped_value_tuples_t<Sender>;

 public:
  static_assert(value_tuples_check<value_tuples>::all_ec_first &&
                    bexec::sends_stopped<Sender>,
                "with_timeout wraps senders whose value completions all carry "
                "std::error_code first (the bkmail completion shape), with a "
                "stopped channel");

  using completion_signatures = typename mirror_signatures<value_tuples>::type;

  with_timeout_sender(Sender sender, bnio::io_context& ioc,
                      bnio::steady_timer::duration timeout)
      : sender_(std::move(sender)), ioc_(&ioc), timeout_(timeout) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return with_timeout_operation<Sender, Receiver>{
        std::move(sender_), *ioc_, timeout_, std::move(receiver)};
  }

 private:
  Sender sender_;
  bnio::io_context* ioc_;  // borrowed; the watchdog binds to it at connect
  bnio::steady_timer::duration timeout_;
};

/**
 * Arms a watchdog next to @p sender: reports `std::errc::timed_out` when
 * @p timeout expires first (see the file banner for the payload-carrying
 * case), forwarding the wrapped sender's value/stopped completions
 * otherwise.
 *
 * @param sender  a sender whose value completions all carry
 *                std::error_code first, with a stopped channel
 * @param ioc     borrowed; the steady_timer watchdog binds to it at
 *                connect() time (re-armed per operation)
 * @param timeout watchdog duration
 */
template <class Sender, class Rep, class Period>
[[nodiscard]] auto with_timeout(Sender&& sender, bnio::io_context& ioc,
                                std::chrono::duration<Rep, Period> timeout) {
  return with_timeout_sender<std::decay_t<Sender>>{
      std::forward<Sender>(sender), ioc,
      std::chrono::duration_cast<bnio::steady_timer::duration>(timeout)};
}

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_WITH_TIMEOUT_H_
