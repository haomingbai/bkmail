/**
 * @file include/bkmail/imap/command/store_command.h
 * @brief STORE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 STORE: `tag STORE <seq-set> <+FLAGS|-FLAGS|FLAGS> (<flags>)`.
 * The store_mode selects the operator: add -> `+FLAGS`, remove ->
 * `-FLAGS`, replace -> `FLAGS`. The `.SILENT` suffix is intentionally not
 * used: the server then confirms each change with a
 * `* n FETCH (FLAGS ...)` response, which is tolerated here (it is also
 * broadcast as flags_update_event on the unsolicited path) and carries
 * no result for this command.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a
 * tagged NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_STORE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_STORE_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/error.h>
#include <bkmail/imap/fetch_items.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// RFC 3501 STORE command over message sequence numbers.
template <class Allocator = std::allocator<std::byte>>
class store_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: STORE carries no data.
  using result_type = void;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a STORE command for a sequence set, a flag set, and the
  /// store mode (add / remove / replace).
  store_command(std::string_view seq_set, flag_set flags, store_mode mode,
                const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        flags_(flags),
        mode_(mode),
        line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders
  /// `tag STORE <seq-set> <op>FLAGS (<flags>)\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" STORE ");
    line_.append(seq_set_);
    line_.push_back(' ');
    switch (mode_) {
      case store_mode::add:
        line_.append("+FLAGS");
        break;
      case store_mode::remove:
        line_.append("-FLAGS");
        break;
      case store_mode::replace:
        line_.append("FLAGS");
        break;
    }
    line_.append(" (");
    append_flag_names(line_);
    line_.append(")\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// STORE never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// STORE issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// Non-SILENT STORE answers `* n FETCH (FLAGS ...)`; tolerated and
  /// dropped (the unsolicited path broadcasts the flag update).
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Maps the tagged completion: OK -> success, NO -> command_rejected,
  /// BAD -> bad_command.
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const noexcept {
    return detail::map_status_to_ec(r.status);
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;

  /// Appends the wire names of the set system flags, space-separated.
  void append_flag_names(string_type& out) const {
    bool first = true;
    auto append = [&](bool set, std::string_view name) {
      if (!set) return;
      if (!first) out.push_back(' ');
      first = false;
      out.append(name);
    };
    append(flags_.test(message_flag::seen), "\\Seen");
    append(flags_.test(message_flag::answered), "\\Answered");
    append(flags_.test(message_flag::flagged), "\\Flagged");
    append(flags_.test(message_flag::deleted), "\\Deleted");
    append(flags_.test(message_flag::draft), "\\Draft");
    append(flags_.test(message_flag::recent), "\\Recent");
  }

  [[no_unique_address]] Allocator alloc_;
  string_type seq_set_;
  flag_set flags_;
  store_mode mode_;
  string_type line_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_STORE_COMMAND_H_
