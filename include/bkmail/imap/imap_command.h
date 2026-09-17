/**
 * @file include/bkmail/imap/imap_command.h
 * @brief Type-erased IMAP command for batch submission.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * `imap_command<A>` is the erased handle imap_context accepts in its
 * batch-submission overload
 * (`submit(std::vector<std::unique_ptr<imap_command<A>>>)`, decision D1 of
 * docs/code_layout.md): a move-only unique_ptr wrapper over
 * `detail::operation_base<A>`. `make_command(command, handler)` performs
 * the erasure by stuffing the concrete command together with its handler
 * into a `detail::operation_model`, allocated through the command's
 * allocator rebound to the model (detail::allocate_operation).
 *
 * Each handler still fires independently when its own tagged response
 * arrives; batching only shares the tag space and the write syscall
 * (docs/usage.md §3.4).
 */

#pragma once
#ifndef BKMAIL_IMAP_IMAP_COMMAND_H_
#define BKMAIL_IMAP_IMAP_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/imap/operation_base.h>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace bkmail::imap {

/// Type-erased IMAP command handle: owns one queued operation through
/// the detail::operation_base interface. Move-only.
template <class Allocator = std::allocator<std::byte>>
class imap_command {
 public:
  /// Allocator the erased operation was allocated with (rebound).
  using allocator_type = Allocator;
  /// The erased operation pointer this handle wraps.
  using operation_pointer = std::unique_ptr<detail::operation_base<Allocator>,
                                            detail::op_deleter<Allocator>>;

  /// Creates an empty handle.
  imap_command() noexcept = default;

  /// Takes ownership of an erased operation.
  explicit imap_command(operation_pointer op) noexcept : op_(std::move(op)) {}

  imap_command(imap_command&&) noexcept = default;
  imap_command& operator=(imap_command&&) noexcept = default;
  imap_command(const imap_command&) = delete;
  imap_command& operator=(const imap_command&) = delete;
  ~imap_command() = default;

  /// Borrows the erased operation; nullptr on an empty handle.
  [[nodiscard]] detail::operation_base<Allocator>* get() const noexcept {
    return op_.get();
  }

  /// True when the handle owns an operation.
  [[nodiscard]] explicit operator bool() const noexcept {
    return op_ != nullptr;
  }

  /// Releases the erased operation to the caller (used by imap_context
  /// when it takes the batch into its write queue).
  [[nodiscard]] operation_pointer release() noexcept { return std::move(op_); }

 private:
  operation_pointer op_;
};

/// Erases a concrete command together with its handler into an
/// imap_command owned by a std::unique_ptr, ready for the imap_context
/// batch-submission overload.
///
/// The handler signature is `void(std::error_code, Command::result_type)`
/// or `void(std::error_code)` when result_type is void (docs/usage.md
/// §3.3). The operation cell is allocated with the command's allocator
/// (via get_allocator() when the command exposes one) through
/// detail::allocate_operation, so the cell frees itself with the same
/// allocator family (detail::op_deleter / dispose()).
template <class Command, class F>
[[nodiscard]] std::unique_ptr<imap_command<typename Command::allocator_type>>
make_command(Command command, F&& handler) {
  using allocator_type = typename Command::allocator_type;
  using model_type = detail::operation_model<Command, std::decay_t<F>>;

  const allocator_type alloc = [&] {
    if constexpr (requires(const Command& c) { c.get_allocator(); }) {
      return allocator_type{command.get_allocator()};
    } else {
      return allocator_type{};
    }
  }();

  return std::make_unique<imap_command<allocator_type>>(
      detail::allocate_operation<model_type>(alloc, std::move(command),
                                             std::forward<F>(handler)));
}

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_IMAP_COMMAND_H_
