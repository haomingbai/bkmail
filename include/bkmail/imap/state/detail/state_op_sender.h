/**
 * @file state_op_sender.h
 * @brief Shared sender glue driving Layer 1 for the Layer-2 states.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * One coupled group (docs/code_layout.md §4): the generic
 * detail::state_op_sender that turns "submit one command, map its tagged
 * result into the successor state" into a hand-written bexec sender, and
 * the dedicated detail::idle_op_sender behind selected_state::idle().
 *
 * Sender conventions (bexec hand-written rules):
 *  - nested completion_signatures; connect(receiver) is &&-qualified and
 *    returns a pinned (non-movable, non-copyable) operation state;
 *  - start() is noexcept; connect is conditionally noexcept;
 *  - every failure rides the value channel as std::error_code (no
 *    set_error); cancellation completes with set_stopped();
 *  - the receiver's stop token is obtained from its environment and
 *    forwarded to Layer 1 as imap_context::cancel(tag), and the stopped
 *    completion itself is delivered from the context's dispatch path
 *    (post_stopped_delivery) — never inline on the thread that requested
 *    the stop.
 *
 * The file also hosts the shared operation-state glue: stop_callback_box
 * (dedup C1), guarded_timer_receiver (dedup C3), the shared successor
 * factories (dedup C6) and the complete_with delivery tail (dedup C7).
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_DETAIL_STATE_OP_SENDER_H_
#define BKMAIL_IMAP_STATE_DETAIL_STATE_OP_SENDER_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/command/idle_command.h>
#include <bkmail/imap/detail/unsolicited_table.h>
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/imap_context.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/session_state.h>
#include <bkmail/imap/unsolicited_event.h>
#include <bnio/io_context.h>

#include <atomic>
#include <bexec/detail/operation.hpp>
#include <bexec/env.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace bkmail::imap::detail {

/// RFC 5550 heartbeat point: re-issue IDLE at least every 29 minutes.
inline constexpr std::chrono::minutes kIdleHeartbeat{29};

// Type derivation behind state_op_sender, split on whether the command
// yields a result (a void result_type must never reach invoke_result's
// argument list). A resultful successor factory either returns
// std::pair{result, successor} — the result rides the value channel
// alongside the state — or returns the successor state directly, having
// absorbed the result (SELECT/EXAMINE fold mailbox_info into
// selected_state, so their completion is (error_code, selected_state)).
template <class T>
struct is_result_bundle : std::false_type {};

template <class First, class Second>
struct is_result_bundle<std::pair<First, Second>> : std::true_type {};

template <bool CarriesResult, class Bundle>
struct successor_of {
  using type = Bundle;
};

template <class Bundle>
struct successor_of<true, Bundle> {
  using type = typename Bundle::second_type;
};

/**
 * Branching successor factory. Operations whose tagged outcome maps to
 * DIFFERENT successor states (LOGIN/SELECT: the next state depends on
 * OK vs NO) wrap their two per-outcome factories with branch_on_error().
 * At completion the operation picks the branch on the error code:
 * ec == 0 runs on_value, anything else runs on_error. Each branch returns
 * its own successor state type and the sender publishes one independent
 * set_value signature per branch — the alternatives travel as distinct
 * completions, never merged into a variant payload.
 */
template <class OnValue, class OnError>
struct error_branch {
  OnValue on_value;
  OnError on_error;
};

/// Wraps the (success, failure) successor factories of a branching
/// operation (see error_branch).
template <class OnValue, class OnError>
[[nodiscard]] auto branch_on_error(OnValue on_value, OnError on_error) {
  return error_branch<OnValue, OnError>{std::move(on_value),
                                        std::move(on_error)};
}

template <class T>
struct is_error_branch : std::false_type {};

template <class OnValue, class OnError>
struct is_error_branch<error_branch<OnValue, OnError>> : std::true_type {};

template <class T>
inline constexpr bool is_error_branch_v = is_error_branch<T>::value;

template <bool HasResult, class Allocator, class Command, class MakeSuccessor>
struct state_op_traits;

// Resultful command, single successor: the factory maps
// (ec, connection, result) to the successor or to pair{result, successor}.
template <class Allocator, class Command, class MakeSuccessor>
struct state_op_traits<true, Allocator, Command, MakeSuccessor> {
  using bundle = std::invoke_result_t<MakeSuccessor, std::error_code,
                                      imap_connection<Allocator>&&,
                                      typename Command::result_type&&>;
  static constexpr bool carries_result = is_result_bundle<bundle>::value;
  using successor_state = typename successor_of<carries_result, bundle>::type;
  using completion_signatures = bexec::completion_signatures<
      std::conditional_t<carries_result,
                         bexec::set_value_t(std::error_code,
                                            typename Command::result_type,
                                            successor_state),
                         bexec::set_value_t(std::error_code, successor_state)>,
      bexec::set_stopped_t()>;
};

// Resultful command, branching successor (SELECT/EXAMINE): both branches
// absorb the result — the success branch folds it into its state, the
// failure branch drops it — so the completion carries (ec, state) only.
template <class Allocator, class Command, class OnValue, class OnError>
struct state_op_traits<true, Allocator, Command,
                       error_branch<OnValue, OnError>> {
  static constexpr bool carries_result = false;
  using value_state = std::invoke_result_t<OnValue, std::error_code,
                                           imap_connection<Allocator>&&,
                                           typename Command::result_type&&>;
  using failure_state = std::invoke_result_t<OnError, std::error_code,
                                             imap_connection<Allocator>&&,
                                             typename Command::result_type&&>;
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, value_state),
      bexec::set_value_t(std::error_code, failure_state),
      bexec::set_stopped_t()>;
};

// Void-result command, single successor.
template <class Allocator, class Command, class MakeSuccessor>
struct state_op_traits<false, Allocator, Command, MakeSuccessor> {
  static constexpr bool carries_result = false;
  using successor_state = std::invoke_result_t<MakeSuccessor, std::error_code,
                                               imap_connection<Allocator>&&>;
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code,
                                                      successor_state),
                                   bexec::set_stopped_t()>;
};

// Void-result command, branching successor (LOGIN/AUTHENTICATE).
template <class Allocator, class Command, class OnValue, class OnError>
struct state_op_traits<false, Allocator, Command,
                       error_branch<OnValue, OnError>> {
  static constexpr bool carries_result = false;
  using value_state = std::invoke_result_t<OnValue, std::error_code,
                                           imap_connection<Allocator>&&>;
  using failure_state = std::invoke_result_t<OnError, std::error_code,
                                             imap_connection<Allocator>&&>;
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, value_state),
      bexec::set_value_t(std::error_code, failure_state),
      bexec::set_stopped_t()>;
};

// ---- Shared operation-state glue ----------------------------------------

/**
 * Terminal delivery tail shared by the Layer-2 operation states (dedup
 * C7): a branching factory (branch_on_error) picks its branch on the
 * error code; a plain factory maps every completion to its single
 * successor; a pair-valued bundle (resultful query operations) is
 * unwrapped so the result rides the value channel next to the error code.
 *
 * @param make_successor the operation's successor factory
 * @param receiver       the operation state's receiver, consumed here
 * @param ec             the completion's error code
 * @param args           the remaining factory arguments (connection,
 *                       result, snapshot — factory-shaped)
 */
template <class MakeSuccessor, class Receiver, class... Args>
void complete_with(MakeSuccessor& make_successor, Receiver& receiver,
                   std::error_code ec, Args&&... args) noexcept {
  if constexpr (is_error_branch_v<MakeSuccessor>) {
    if (!ec) {
      bexec::set_value(
          std::move(receiver), ec,
          make_successor.on_value(ec, std::forward<Args>(args)...));
    } else {
      bexec::set_value(
          std::move(receiver), ec,
          make_successor.on_error(ec, std::forward<Args>(args)...));
    }
  } else {
    auto bundle = make_successor(ec, std::forward<Args>(args)...);
    if constexpr (is_result_bundle<decltype(bundle)>::value) {
      bexec::set_value(std::move(receiver), ec, std::move(bundle.first),
                       std::move(bundle.second));
    } else {
      bexec::set_value(std::move(receiver), ec, std::move(bundle));
    }
  }
}

// Shared successor factories (dedup C6). The session states hand these to
// state_op_sender so the per-state lambdas stay single-sourced. They are
// templates: the state types they name are only forward-declared here
// (session_state.h) and complete at instantiation inside the state
// headers, after their definitions.

/// Successor factory for void-result operations that keep the session in
/// @p State (NOOP-style): every non-stopped completion rebuilds @p State
/// over the returned connection.
template <class State, class Allocator>
[[nodiscard]] auto same_state_successor() {
  return [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
    return State{std::move(conn)};
  };
}

/// Successor factory for LOGOUT: the connection's teardown is detained
/// (see detain_connection) and the terminal state is yielded.
template <class Allocator>
[[nodiscard]] auto logout_successor() {
  return [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
    // The completion runs inside the read dispatch; the connection's
    // destruction is detained to after the dispatch unwinds.
    detain_connection(std::move(conn));
    return logout_state<Allocator>{};
  };
}

/// Successor factory for CAPABILITY: refreshes the connection's
/// capability cache on success and hands the (capability_set, state) pair
/// back, with @p inner rebuilding the successor state.
template <class Allocator, class Inner>
[[nodiscard]] auto capability_successor(Inner inner) {
  return [inner = std::move(inner)](std::error_code ec,
                                    imap_connection<Allocator> conn,
                                    capability_set<Allocator> caps) mutable {
    if (!ec) {
      conn.set_capabilities(caps);
    }
    return std::pair{std::move(caps), inner(ec, std::move(conn))};
  };
}

/// Successor branch pair for LOGIN/AUTHENTICATE-class operations: OK
/// advances to Authenticated, anything else retains the
/// Not-Authenticated state.
template <class Allocator>
[[nodiscard]] auto auth_branches() {
  return branch_on_error(
      [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
        return authenticated_state<Allocator>{std::move(conn)};
      },
      [](std::error_code /*ec*/, imap_connection<Allocator> conn) {
        return not_authenticated_state<Allocator>{std::move(conn)};
      });
}

/// Successor branch pair for SELECT/EXAMINE: OK folds the mailbox_info
/// snapshot into the Selected state, failure keeps the session
/// Authenticated (a failed SELECT selects no mailbox, RFC 3501).
template <class Allocator>
[[nodiscard]] auto select_branches() {
  return branch_on_error(
      [](std::error_code /*ec*/, imap_connection<Allocator> conn,
         mailbox_info<Allocator> info) {
        return selected_state<Allocator>{std::move(conn), std::move(info)};
      },
      [](std::error_code /*ec*/, imap_connection<Allocator> conn,
         mailbox_info<Allocator> /*info*/) {
        return authenticated_state<Allocator>{std::move(conn)};
      });
}

/**
 * Stop-callback plumbing shared by every Layer-2 operation state (dedup
 * C1): derives the environment/stop-token/callback types for @p Forwarder
 * from @p Receiver and wraps the callback in an optional with the
 * emplace/reset choreography.
 */
template <class Receiver, class Forwarder>
class stop_callback_box {
 public:
  using env_type = decltype(bexec::get_env(std::declval<const Receiver&>()));
  using stop_token_type =
      decltype(bexec::get_stop_token(std::declval<const env_type&>()));
  using stop_callback_type =
      typename stop_token_type::template callback_type<Forwarder>;

  /// Registers @p forwarder for @p token; a token that is already
  /// cancelled fires the callback inline before emplace returns.
  void emplace(const stop_token_type& token, Forwarder forwarder) {
    callback_.emplace(token, std::move(forwarder));
  }

  /// Unregisters the callback; bexec-verified safe to call from within
  /// the callback itself.
  void reset() noexcept { callback_.reset(); }

 private:
  std::optional<stop_callback_type> callback_;
};

/**
 * Self-owning timer-wait receiver (io_box), shared by watchdog-style
 * timers (dedup C3): the wait's completion may be queued by cancel()
 * AFTER the owning operation state died, so @p on_fire may only run
 * while the shared completed flag says the owner never completed.
 */
template <class OnFire>
struct guarded_timer_receiver {
  io_box_base* box;
  std::shared_ptr<std::atomic<bool>> completed;
  OnFire on_fire;

  void set_value(std::error_code ec) noexcept {
    io_box_base* self = box;
    if (!ec && !completed->load(std::memory_order_acquire)) {
      on_fire();
    }
    self->dispose();
  }

  void set_stopped() noexcept { box->dispose(); }
};

/**
 * Receiver of a posted stopped-delivery task (see
 * post_stopped_delivery): the poster already won the operation's
 * completed_ exchange, so this task is the operation's sole remaining
 * completion and moves the receiver out of the operation state.
 */
template <class Receiver>
struct stopped_delivery_receiver {
  io_box_base* box;
  Receiver* receiver;  // into the operation state; see liveness below
  std::shared_ptr<std::atomic<bool>> completed;  // the poster's claim

  void set_value(std::error_code) noexcept {
    // Liveness: the operation state is destroyed only by its completion;
    // after the poster won the completed_ exchange, no other path can
    // complete (every rival completion point exchange-loses and returns
    // without touching the state), so the state — and with it *receiver —
    // is still alive here and the receiver may be moved out.
    assert(completed->load(std::memory_order_acquire));
    bexec::set_stopped(std::move(*receiver));
    box->dispose();
  }

  void set_stopped() noexcept {
    // The io_context drained without running the task: the operation can
    // never complete — the same accepted leak as detain_connection.
    box->dispose();
  }
};

/**
 * Delivers a Layer-2 operation's stopped completion from the io_context's
 * dispatch path (contract §6: receivers are only ever completed from the
 * context's dispatch path — never inline on the thread that requested the
 * stop). The caller must have won the operation's completed_ exchange and
 * finished its synchronous bookkeeping (tag cancel, finish_operation,
 * stop-callback reset, timer cancel); everything else rides the posted,
 * self-owning io_box task.
 *
 * Liveness: the operation state is destroyed only by its completion, and
 * after the completed_ exchange was won no other path can complete (every
 * rival completion point exchange-loses and returns without touching the
 * state) — the posted task is the operation's sole remaining completion,
 * so the state is still alive when the task runs and the task may safely
 * move @p receiver (the operation state's member) out.
 *
 * Should the io_context never run again, the task — and with it the
 * completion — never runs; the same accepted leak as detain_connection.
 * On allocation failure the delivery falls back to inline completion: a
 * hung consumer is worse than a bent contract under OOM.
 */
template <class Allocator, class Receiver>
void post_stopped_delivery(
    const Allocator& alloc, bnio::io_context& ioc, Receiver& receiver,
    std::shared_ptr<std::atomic<bool>> completed) noexcept {
  try {
    auto* box = make_io_box(
        alloc, bexec::schedule(ioc.get_post_scheduler()),
        [&receiver, completed = std::move(completed)](io_box_base* self) {
          return stopped_delivery_receiver<Receiver>{self, &receiver,
                                                     std::move(completed)};
        });
    box->start();
  } catch (...) {
    bexec::set_stopped(std::move(receiver));
  }
}

/**
 * Operation state behind state_op_sender (pinned, single-start).
 *
 * Owns the connection for the duration of the operation: the state chain's
 * ownership flows state -> sender -> operation -> successor state.
 *
 * The successor factory maps the completion to the next state:
 *  - resultful command: (std::error_code, imap_connection<A>&&,
 *    result_type&&) -> std::pair<result_type, successor_state>, or the
 *    bare successor state when the result is absorbed into it;
 *  - void command: (std::error_code, imap_connection<A>&&) ->
 *    successor_state.
 * A factory wrapped in error_branch (branch_on_error) instead holds two
 * per-outcome factories and the completion picks one on the error code —
 * each branch's successor state rides its own set_value signature.
 * Single-successor factories run for every non-stopped completion, so
 * operations whose successor equals the current state naturally "keep the
 * current state" on NO/BAD, and capability()-style operations can fold
 * bookkeeping (cache updates) into the factory.
 */
template <class Allocator, class Command, class MakeSuccessor, class Receiver>
class state_op_operation {
 public:
  using result_type = typename Command::result_type;
  static constexpr bool has_result = !std::is_void_v<result_type>;
  using traits = state_op_traits<has_result, Allocator, Command, MakeSuccessor>;

  state_op_operation(
      imap_connection<Allocator>&& connection, Command&& command,
      MakeSuccessor&& make_successor, std::string_view required_capability,
      Receiver&&
          receiver) noexcept(std::is_nothrow_move_constructible_v<Command> &&
                             std::is_nothrow_move_constructible_v<
                                 MakeSuccessor> &&
                             std::is_nothrow_move_constructible_v<Receiver>)
      : connection_(std::move(connection)),
        command_(std::move(command)),
        make_successor_(std::move(make_successor)),
        required_capability_(required_capability),
        receiver_(std::move(receiver)),
        tag_(connection_.get_allocator()) {}

  state_op_operation(const state_op_operation&) = delete;
  state_op_operation& operator=(const state_op_operation&) = delete;
  state_op_operation(state_op_operation&&) = delete;
  state_op_operation& operator=(state_op_operation&&) = delete;

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

    // Extension gate (e.g. UIDPLUS MOVE): fail fast without touching the
    // wire when the server lacks the required capability.
    if (!required_capability_.empty() &&
        !connection_.capabilities().contains(required_capability_)) {
      connection_.finish_operation();
      const auto ec = make_error_code(errc::capability_required);
      if constexpr (has_result) {
        complete_with(make_successor_, receiver_, ec, std::move(connection_),
                      result_type{});
      } else {
        complete_with(make_successor_, receiver_, ec, std::move(connection_));
      }
      return;
    }

    connection_.with_context([this](auto& ctx) {
      if constexpr (has_result) {
        tag_ =
            ctx.submit(std::move(command_),
                       [this](std::error_code ec, result_type result) mutable {
                         on_result(ec, std::move(result));
                       });
      } else {
        tag_ =
            ctx.submit(std::move(command_),
                       [this](std::error_code ec) mutable { on_result(ec); });
      }
      ctx.flush();
    });

    // Registering after submit/flush publishes tag_ to the stop callback;
    // a token already cancelled in between fires the callback inline here.
    stop_callback_.emplace(token, stop_forwarder{this});
  }

 private:
  struct stop_forwarder {
    state_op_operation* op;

    void operator()() const noexcept {
      // Cancel first (drop the reply on arrival), then complete; never
      // run the receiver inline twice.
      if (op->completed_->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      op->connection_.with_context(
          [tag = op->tag_](auto& ctx) mutable { ctx.cancel(tag); });
      op->connection_.finish_operation();
      op->stop_callback_.reset();
      // §6 contract: the receiver is completed from the context's
      // dispatch path, never inline on the requesting thread.
      post_stopped_delivery(op->connection_.get_allocator(),
                            op->connection_.io_context(), op->receiver_,
                            op->completed_);
    }
  };

  template <class R = result_type>
    requires(!std::is_void_v<R>)
  void on_result(std::error_code ec, R result) noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;  // stop won the race; drop the reply.
    }
    stop_callback_.reset();
    connection_.finish_operation();
    complete_with(make_successor_, receiver_, ec, std::move(connection_),
                  std::move(result));
  }

  template <class R = result_type>
    requires(std::is_void_v<R>)
  void on_result(std::error_code ec) noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stop_callback_.reset();
    connection_.finish_operation();
    complete_with(make_successor_, receiver_, ec, std::move(connection_));
  }

  imap_connection<Allocator> connection_;
  Command command_;
  [[no_unique_address]] MakeSuccessor make_successor_;
  std::string_view required_capability_;
  Receiver receiver_;
  bkmail::detail::string_of<Allocator> tag_;
  stop_callback_box<Receiver, stop_forwarder> stop_callback_;
  // Shared (like idle_op_operation's) so the posted stopped-delivery task
  // can hold a flag copy of its own.
  std::shared_ptr<std::atomic<bool>> completed_ =
      std::make_shared<std::atomic<bool>>(false);
};

/**
 * The generic Layer-2 operation sender.
 *
 * Submits Command through the connection's active Layer-1 context, kicks
 * the batched write with flush(), and maps the tagged completion to the
 * successor state through the factory described above.
 */
template <class Allocator, class Command, class MakeSuccessor>
class state_op_sender {
 public:
  using result_type = typename Command::result_type;
  static constexpr bool has_result = !std::is_void_v<result_type>;

  using traits = state_op_traits<has_result, Allocator, Command, MakeSuccessor>;

  using completion_signatures = typename traits::completion_signatures;

  state_op_sender(imap_connection<Allocator> connection, Command command,
                  MakeSuccessor make_successor,
                  std::string_view required_capability = {})
      : connection_(std::move(connection)),
        command_(std::move(command)),
        make_successor_(std::move(make_successor)),
        required_capability_(required_capability) {}

  template <class Receiver>
  auto connect(Receiver receiver) && noexcept(
      std::is_nothrow_constructible_v<
          state_op_operation<Allocator, Command, MakeSuccessor, Receiver>,
          imap_connection<Allocator>&&, Command&&, MakeSuccessor&&,
          std::string_view, Receiver&&>) {
    return state_op_operation<Allocator, Command, MakeSuccessor, Receiver>{
        std::move(connection_), std::move(command_), std::move(make_successor_),
        required_capability_, std::move(receiver)};
  }

 private:
  imap_connection<Allocator> connection_;
  Command command_;
  [[no_unique_address]] MakeSuccessor make_successor_;
  std::string_view required_capability_;
};

/**
 * Operation state behind idle_op_sender: one IDLE/DONE cycle.
 *
 * Drives idle_command through Layer 1 and completes with the rebuilt
 * selected state when the server pushes activity (unsolicited EXISTS /
 * EXPUNGE), when the 29-minute heartbeat point arrives, or — via
 * set_stopped() — when the receiver's stop token fires. Every exit sends
 * DONE first (imap_context::cancel on a pending idle_command) so the
 * server stays in sync.
 *
 * Carries the mailbox snapshot across the cycle: server pushes update it,
 * so the rebuilt state's mailbox() reflects the newest EXISTS/RECENT view.
 */
template <class Allocator, class MakeSuccessor, class Receiver>
class idle_op_operation {
 public:
  using successor_state = std::invoke_result_t<MakeSuccessor, std::error_code,
                                               imap_connection<Allocator>&&,
                                               mailbox_info<Allocator>&&>;

  idle_op_operation(
      imap_connection<Allocator>&& connection,
      mailbox_info<Allocator>&& snapshot, MakeSuccessor&& make_successor,
      Receiver&&
          receiver) noexcept(std::
                                 is_nothrow_move_constructible_v<
                                     MakeSuccessor> &&
                             std::is_nothrow_move_constructible_v<Receiver>)
      : connection_(std::move(connection)),
        snapshot_(std::move(snapshot)),
        make_successor_(std::move(make_successor)),
        receiver_(std::move(receiver)),
        tag_(connection_.get_allocator()),
        completed_(std::make_shared<std::atomic<bool>>(false)) {}

  idle_op_operation(const idle_op_operation&) = delete;
  idle_op_operation& operator=(const idle_op_operation&) = delete;
  idle_op_operation(idle_op_operation&&) = delete;
  idle_op_operation& operator=(idle_op_operation&&) = delete;

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

    // IDLE is only offered when the server advertised it.
    if (!connection_.capabilities().contains("IDLE")) {
      connection_.finish_operation();
      complete_with(make_successor_, receiver_,
                    make_error_code(errc::capability_required),
                    std::move(connection_), std::move(snapshot_));
      return;
    }

    connection_.with_context([this](auto& ctx) {
      // Wake on server pushes that change the mailbox view.
      unsolicited_reg_.emplace(
          ctx.on_unsolicited([this](const unsolicited_event<Allocator>& event) {
            on_unsolicited(event);
          }));
      tag_ = ctx.submit(idle_command<Allocator>{},
                        [this](std::error_code ec) mutable { on_tagged(ec); });
      ctx.flush();
    });

    // Heartbeat watchdog, bound to the connection's current io_context.
    // The wait is self-owning (io_box): its completion may be queued by
    // cancel() AFTER this operation state was destroyed, so it must not
    // live inside this object — guarded_timer_receiver guards the wake()
    // call with the shared completed flag and disposes the box.
    timer_.emplace(connection_.io_context(), kIdleHeartbeat);
    auto* timer_box = make_io_box(
        connection_.get_allocator(), timer_->async_wait(),
        [this](io_box_base* self) {
          return guarded_timer_receiver{self, completed_, [this] { wake(); }};
        });
    timer_box->start();

    stop_callback_.emplace(token, stop_forwarder{this});
  }

 private:
  struct stop_forwarder {
    idle_op_operation* op;

    void operator()() const noexcept {
      if (op->completed_->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      // DONE first, then complete: the server stays in sync (usage §2.7).
      op->connection_.with_context(
          [tag = op->tag_](auto& ctx) mutable { ctx.cancel(tag); });
      op->cleanup();
      // §6 contract: the receiver is completed from the context's
      // dispatch path, never inline on the requesting thread.
      post_stopped_delivery(op->connection_.get_allocator(),
                            op->connection_.io_context(), op->receiver_,
                            op->completed_);
    }
  };

  void on_unsolicited(const unsolicited_event<Allocator>& event) noexcept {
    // Post-completion pushes are dropped before the snapshot is touched:
    // the completion has already moved snapshot_ out (audit Finding 6).
    if (completed_->load(std::memory_order_acquire)) {
      return;
    }
    std::visit(
        [this](const auto& e) {
          using event_type = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<event_type, exists_event>) {
            snapshot_.exists = e.count;
            wake();
          } else if constexpr (std::is_same_v<event_type, expunge_event>) {
            if (snapshot_.exists > 0) {
              --snapshot_.exists;
            }
            wake();
          } else if constexpr (std::is_same_v<event_type, recent_event>) {
            snapshot_.recent = e.count;
          }
          // BYE and the rest are left to the tagged path: a dead connection
          // fails the pending idle_command, which completes with its ec.
        },
        event);
  }

  /// Wakes the cycle: sends DONE and completes successfully once.
  void wake() noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    connection_.with_context(
        [tag = tag_](auto& ctx) mutable { ctx.cancel(tag); });
    cleanup();
    complete_with(make_successor_, receiver_, std::error_code{},
                  std::move(connection_), std::move(snapshot_));
  }

  /// Tagged reply of the IDLE command (DONE acknowledged, NO/BAD, or a
  /// connection-level failure reported through Layer 1).
  void on_tagged(std::error_code ec) noexcept {
    if (completed_->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    cleanup();
    complete_with(make_successor_, receiver_, ec, std::move(connection_),
                  std::move(snapshot_));
  }

  void cleanup() noexcept {
    stop_callback_.reset();
    // unsolicited_reg_ is deliberately NOT reset here: wake() may run
    // inside the very unsolicited handler it would unregister, and
    // destroying a registration from within its own dispatch is the
    // table's business, not ours. The member's destruction (after the
    // operation state dies) retires the hook; stray pushes until then are
    // dropped by the completed_ guard.
    if (timer_.has_value()) {
      // Return value (number of cancelled waits) is deliberately unused:
      // an already-fired heartbeat completion is guarded by completed_.
      (void)timer_->cancel();
    }
    connection_.finish_operation();
  }

  imap_connection<Allocator> connection_;
  mailbox_info<Allocator> snapshot_;
  [[no_unique_address]] MakeSuccessor make_successor_;
  Receiver receiver_;
  bkmail::detail::string_of<Allocator> tag_;
  std::optional<bnio::steady_timer> timer_;
  std::optional<registration<Allocator>> unsolicited_reg_;
  stop_callback_box<Receiver, stop_forwarder> stop_callback_;
  // Shared so the posted stopped-delivery task and the self-owning timer
  // receiver can hold a flag copy of their own.
  std::shared_ptr<std::atomic<bool>> completed_;
};

/// Sender behind selected_state::idle(): one IDLE/DONE cycle per operation.
template <class Allocator, class MakeSuccessor>
class idle_op_sender {
 public:
  using successor_state = std::invoke_result_t<MakeSuccessor, std::error_code,
                                               imap_connection<Allocator>&&,
                                               mailbox_info<Allocator>&&>;

  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code,
                                                      successor_state),
                                   bexec::set_stopped_t()>;

  idle_op_sender(imap_connection<Allocator> connection,
                 mailbox_info<Allocator> snapshot, MakeSuccessor make_successor)
      : connection_(std::move(connection)),
        snapshot_(std::move(snapshot)),
        make_successor_(std::move(make_successor)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && noexcept(
      std::is_nothrow_constructible_v<
          idle_op_operation<Allocator, MakeSuccessor, Receiver>,
          imap_connection<Allocator>&&, mailbox_info<Allocator>&&,
          MakeSuccessor&&, Receiver&&>) {
    return idle_op_operation<Allocator, MakeSuccessor, Receiver>{
        std::move(connection_), std::move(snapshot_),
        std::move(make_successor_), std::move(receiver)};
  }

 private:
  imap_connection<Allocator> connection_;
  mailbox_info<Allocator> snapshot_;
  [[no_unique_address]] MakeSuccessor make_successor_;
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_STATE_DETAIL_STATE_OP_SENDER_H_
