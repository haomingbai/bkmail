/**
 * @file include/bkmail/imap/command/uid_search_command.h
 * @brief UID SEARCH command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 UID SEARCH: identical to SEARCH except the set semantics are
 * UIDs and the returned numbers are UIDs. Wire form:
 * `tag UID SEARCH <criteria>`.
 *
 * Handler contract (docs/usage.md §3.3):
 * `void(std::error_code, std::vector<std::uint32_t, ...>)`; a tagged NO is
 * reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_UID_SEARCH_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_UID_SEARCH_COMMAND_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// RFC 3501 UID SEARCH command (UID semantics).
template <class Allocator = std::allocator<std::byte>>
class uid_search_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Matching UIDs in arrival order.
  using result_type = bkmail::detail::vector_of<std::uint32_t, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a UID SEARCH command from raw RFC 3501 criteria.
  explicit uid_search_command(std::string_view criteria,
                              const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        criteria_(criteria, char_allocator{alloc}),
        line_(char_allocator{alloc}),
        matches_(uint_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag UID SEARCH <criteria>\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" UID SEARCH ");
    line_.append(criteria_);
    line_.append("\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// UID SEARCH never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// UID SEARCH issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// Absorbs `* SEARCH [n ...]` responses (UIDs, possibly empty list).
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::search) return;
    const std::string_view text = r.payload;
    std::size_t pos = 0;
    while (pos < text.size()) {
      while (pos < text.size() &&
             (text[pos] == ' ' || text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
      }
      std::uint32_t number = 0;
      bool digits = false;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        number = number * 10 + static_cast<std::uint32_t>(text[pos] - '0');
        digits = true;
        ++pos;
      }
      if (digits) matches_.push_back(number);
    }
  }

  /// FIFO ownership (operation_base.h): the `* SEARCH ...` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::search;
  }

  /// Maps the tagged completion: OK fills `out` with the accumulated
  /// UIDs, NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    const auto ec = detail::map_status_to_ec(r.status);
    if (!ec) out = std::move(matches_);
    return ec;
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;
  using uint_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, std::uint32_t>;

  [[no_unique_address]] Allocator alloc_;
  string_type criteria_;
  string_type line_;
  result_type matches_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_UID_SEARCH_COMMAND_H_
