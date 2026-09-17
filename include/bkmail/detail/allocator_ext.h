/**
 * @file include/bkmail/detail/allocator_ext.h
 * @brief Allocator rebinding aliases shared across modules.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_DETAIL_ALLOCATOR_EXT_H_
#define BKMAIL_DETAIL_ALLOCATOR_EXT_H_

#include <memory>
#include <string>
#include <vector>

namespace bkmail::detail {

/// Rebinds `Allocator` to value type `T`.
template <class Allocator, class T>
using rebind_alloc_t =
    typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

/// The `char` string type configured with `Allocator` rebound to `char`.
template <class Allocator>
using string_of = std::basic_string<char, std::char_traits<char>,
                                    rebind_alloc_t<Allocator, char>>;

/// The `std::vector` of `T` configured with `Allocator` rebound to `T`.
template <class T, class Allocator>
using vector_of = std::vector<T, rebind_alloc_t<Allocator, T>>;

}  // namespace bkmail::detail

#endif  // BKMAIL_DETAIL_ALLOCATOR_EXT_H_
