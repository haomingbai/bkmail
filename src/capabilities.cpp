/**
 * @file src/capabilities.cpp
 * @brief Case-insensitive capability name helpers (non-inline).
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/capability_set.h>

#include <cstddef>
#include <string_view>

namespace bkmail::imap::detail {

namespace {

/// ASCII-only uppercase fold: folds a-z, leaves every other octet alone.
[[nodiscard]] char ascii_upper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

}  // namespace

bool capability_name_equals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
  }
  return true;
}

void capability_name_to_upper(char* data, std::size_t size) noexcept {
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = ascii_upper(data[i]);
  }
}

}  // namespace bkmail::imap::detail
