/**
 * @file include/bkmail/imap/detail/submit_sender.h
 * @brief Lazy submission sender for the imap_context sender path.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details [self-built] sender half of
 * `imap_context::submit<Operation>(args...)` (code_layout D1, §4 split
 * control): nothing is sent until the sender is connected and started;
 * the tag is allocated at `start()`, in wire order.
 *
 * `submit_sender` decays and stores the constructor arguments;
 * `connect()` builds the `Operation` from them and returns a pinned,
 * single-start `submit_operation`. `start()` allocates the one
 * `operation_model` cell (through the context's rebound allocator) and
 * hands it to the context's registration path, which stamps the tag,
 * registers, queues, and kicks the write pump.
 *
 * Both types are templated on the context type (not on Stream/Allocator)
 * so this header never names `imap_context` and the include direction
 * stays one-way: imap_context.h includes this header.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_SUBMIT_SENDER_H_
#define BKMAIL_IMAP_DETAIL_SUBMIT_SENDER_H_

#include <bkmail/imap/operation_base.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/stop_token.hpp>
#include <memory>
#include <new>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bkmail::imap::detail {

template <class Context, class Operation, class Receiver>
class submit_operation;

/// Computes the completion signatures for an operation result type. The
/// void form needs a specialization: `std::conditional_t` would still
/// instantiate `set_value_t(std::error_code, void)` for the discarded
/// branch, which is ill-formed.
template <class Result>
struct submit_completion_signatures {
  using type =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, Result),
                                   bexec::set_stopped_t()>;
};

template <>
struct submit_completion_signatures<void> {
  using type = bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                            bexec::set_stopped_t()>;
};

/// Lazy sender produced by `imap_context::submit<Operation>(args...)`.
template <class Context, class Operation, class... Args>
class submit_sender {
 public:
  using operation_type = Operation;
  using result_type = typename Operation::result_type;

  /// Completion contract (architecture §3.8): errors travel on the value
  /// channel; there is no error channel.
  using completion_signatures =
      typename submit_completion_signatures<result_type>::type;

  submit_sender(Context* ctx, Args&&... args)
      : context_(ctx), arguments_(std::forward<Args>(args)...) {}

  submit_sender(const submit_sender&) = delete;
  submit_sender& operator=(const submit_sender&) = delete;
  submit_sender(submit_sender&&) = default;
  submit_sender& operator=(submit_sender&&) = default;

  template <class Receiver>
  [[nodiscard]] auto connect(Receiver receiver) && {
    return submit_operation<Context, Operation, std::remove_cvref_t<Receiver>>(
        context_, std::make_from_tuple<Operation>(std::move(arguments_)),
        std::move(receiver));
  }

 private:
  Context* context_;
  std::tuple<std::decay_t<Args>...> arguments_;
};

/// Pinned, single-start operation state: performs exactly one erased
/// registration on start (architecture §3.3).
template <class Context, class Operation, class Receiver>
class submit_operation {
 public:
  using result_type = typename Operation::result_type;

  submit_operation(Context* ctx, Operation operation, Receiver receiver)
      : context_(ctx),
        operation_(std::move(operation)),
        receiver_(std::move(receiver)) {}

  // Pinned: the start path stores receiver_ inside the heap cell.
  submit_operation(const submit_operation&) = delete;
  submit_operation& operator=(const submit_operation&) = delete;
  submit_operation(submit_operation&&) = delete;
  submit_operation& operator=(submit_operation&&) = delete;

  void start() noexcept {
    // Stop token observed at the start point (architecture §2.6).
    if (bexec::get_stop_token(bexec::get_env(receiver_)).stop_requested()) {
      bexec::set_stopped(std::move(receiver_));
      return;
    }
    using model_type = operation_model<Operation, Receiver>;
    using base_type = operation_base<typename Context::allocator_type>;
    std::unique_ptr<base_type, op_deleter<typename Context::allocator_type>>
        cell;
    try {
      cell = allocate_operation<model_type>(context_->get_allocator(),
                                            std::move(operation_),
                                            std::move(receiver_));
    } catch (...) {
      const auto ec = std::make_error_code(std::errc::not_enough_memory);
      if constexpr (std::is_void_v<result_type>) {
        bexec::set_value(std::move(receiver_), ec);
      } else {
        bexec::set_value(std::move(receiver_), ec, result_type{});
      }
      return;
    }
    // Stamps the tag, attaches the sink + stop token, registers, queues,
    // and kicks the write pump.
    context_->start_operation(std::move(cell));
  }

 private:
  Context* context_;
  Operation operation_;
  Receiver receiver_;
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_SUBMIT_SENDER_H_
