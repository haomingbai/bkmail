/**
 * @file include/bkmail/detail/unique_function.h
 * @brief Minimal move-only function wrapper for handler storage.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_DETAIL_UNIQUE_FUNCTION_H_
#define BKMAIL_DETAIL_UNIQUE_FUNCTION_H_

#include <cassert>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace bkmail::detail {

template <class Signature>
class unique_function;

/**
 * @brief Move-only type-erased callable, in the spirit of
 * `std::move_only_function`.
 *
 * Self-built because bexec ships no `move_only_function` (architecture
 * §2.10). Used to store unsolicited-response handlers and similar
 * type-erased callbacks. The target is heap-allocated with `std::allocator`
 * storage semantics (plain `new`/`delete`).
 */
template <class R, class... Args>
class unique_function<R(Args...)> {
 public:
  /// Creates an empty wrapper.
  unique_function() noexcept = default;

  /// Creates an empty wrapper.
  unique_function(std::nullptr_t) noexcept {}

  /**
   * @brief Stores a copy of `f`.
   * @tparam F Callable with `R(Args...)`; must not be another
   * `unique_function`.
   */
  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, unique_function> &&
             std::is_invocable_r_v<R, F&, Args...>)
  unique_function(F&& f) {
    using fn_type = std::remove_cvref_t<F>;
    auto* cell = new fn_type(std::forward<F>(f));
    target_ = cell;
    invoke_ = [](void* target, Args... args) -> R {
      return std::invoke(*static_cast<fn_type*>(target),
                         std::forward<Args>(args)...);
    };
    destroy_ = [](void* target) noexcept {
      delete static_cast<fn_type*>(target);
    };
  }

  unique_function(unique_function&& other) noexcept
      : target_(std::exchange(other.target_, nullptr)),
        invoke_(std::exchange(other.invoke_, nullptr)),
        destroy_(std::exchange(other.destroy_, nullptr)) {}

  unique_function& operator=(unique_function&& other) noexcept {
    if (this != &other) {
      reset();
      target_ = std::exchange(other.target_, nullptr);
      invoke_ = std::exchange(other.invoke_, nullptr);
      destroy_ = std::exchange(other.destroy_, nullptr);
    }
    return *this;
  }

  unique_function(const unique_function&) = delete;
  unique_function& operator=(const unique_function&) = delete;

  /// Empties the wrapper.
  unique_function& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  ~unique_function() { reset(); }

  /// Calls the stored target. @pre `*this` is not empty.
  R operator()(Args... args) {
    assert(target_ != nullptr);
    return invoke_(target_, std::forward<Args>(args)...);
  }

  /// Returns true when a target is stored.
  [[nodiscard]] explicit operator bool() const noexcept {
    return target_ != nullptr;
  }

  /// Destroys the stored target, leaving the wrapper empty.
  void reset() noexcept {
    if (target_ != nullptr) {
      destroy_(target_);
      target_ = nullptr;
      invoke_ = nullptr;
      destroy_ = nullptr;
    }
  }

  /// Exchanges contents with `other`.
  void swap(unique_function& other) noexcept {
    std::swap(target_, other.target_);
    std::swap(invoke_, other.invoke_);
    std::swap(destroy_, other.destroy_);
  }

 private:
  void* target_ = nullptr;
  R (*invoke_)(void*, Args...) = nullptr;
  void (*destroy_)(void*) noexcept = nullptr;
};

template <class R, class... Args>
void swap(unique_function<R(Args...)>& a,
          unique_function<R(Args...)>& b) noexcept {
  a.swap(b);
}

}  // namespace bkmail::detail

#endif  // BKMAIL_DETAIL_UNIQUE_FUNCTION_H_
