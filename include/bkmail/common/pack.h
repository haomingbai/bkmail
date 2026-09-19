/**
 * @file include/bkmail/common/pack.h
 * @brief Packs multi-value sender completions into one tuple for co_await.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_COMMON_PACK_H_
#define BKMAIL_COMMON_PACK_H_

#include <bexec/then.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bkmail {

namespace detail {

/// Builds one tuple from the values of a `set_value` completion.
struct pack_completion_fn {
  template <class... Values>
  [[nodiscard]] auto operator()(Values&&... values) const {
    return std::tuple<std::decay_t<Values>...>(std::forward<Values>(values)...);
  }
};

}  // namespace detail

/**
 * @brief Sender adaptor object packing a multi-value `set_value` completion
 * into a single `std::tuple`.
 *
 * bkmail operation senders complete with `set_value(std::error_code,
 * [result,] next_state)` — two or three values — but `bexec::task` can only
 * `co_await` senders with at most one value. Wrapping with `pack` makes
 * `co_await pack(op)` work out of the box (usage.md §1.4, code_layout D7).
 * Branching senders (connect/login/select — one set_value signature per
 * outcome state) additionally need bexec::into_variant before the await
 * point: `co_await bexec::into_variant(op)` yields a variant of per-outcome
 * tuples (usage.md §2.4).
 */
struct pack_t {
  template <bexec::sender Sender>
  [[nodiscard]] auto operator()(Sender&& sender) const {
    return bexec::then(std::forward<Sender>(sender),
                       detail::pack_completion_fn{});
  }
};

/// The `pack` adaptor instance.
inline constexpr pack_t pack{};

}  // namespace bkmail

#endif  // BKMAIL_COMMON_PACK_H_
