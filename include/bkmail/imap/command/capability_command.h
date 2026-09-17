/**
 * @file include/bkmail/imap/command/capability_command.h
 * @brief CAPABILITY command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_CAPABILITY_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_CAPABILITY_COMMAND_H_

#include <bkmail/error.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// CAPABILITY command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// Capability atoms are accumulated from every `* CAPABILITY ...` untagged
/// response seen while the command is in flight, plus a `[CAPABILITY ...]`
/// resp-code carried by an untagged or tagged `OK` (RFC 3501 permits the
/// server to advertise capabilities that way).
template <class Allocator = std::allocator<std::byte>>
class capability_command {
 public:
  using allocator_type = Allocator;
  using result_type = capability_set<Allocator>;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  explicit capability_command(const Allocator& alloc = Allocator{})
      : capabilities_(alloc), wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" CAPABILITY\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// CAPABILITY never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// CAPABILITY expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// Absorbs `* CAPABILITY atom ...` lines and `* OK [CAPABILITY ...]`.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind == untagged_kind::capability) {
      // Data kinds carry their remainder in `payload` (a dispatch-lifetime
      // view); `text` is only set for the status kinds.
      add_atoms(r.payload);
    } else if (r.kind == untagged_kind::ok && r.code.has_value()) {
      add_code_atoms(*r.code);
    }
  }

  /// Advisory for the write pump: CAPABILITY pipelines freely.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  /// FIFO ownership (operation_base.h): the `* CAPABILITY` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::capability;
  }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply; on OK the accumulated set is moved into @p out.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    auto ec = status_ec(r.status);
    if (ec) return ec;
    if (r.code.has_value()) add_code_atoms(*r.code);
    out = std::move(capabilities_);
    return {};
  }

 private:
  /// Maps a tagged status onto the callback-path error code contract.
  static std::error_code status_ec(response_status status) {
    switch (status) {
      case response_status::ok:
        return {};
      case response_status::no:
        return make_error_code(errc::command_rejected);
      case response_status::bad:
        return make_error_code(errc::bad_command);
      default:
        return make_error_code(errc::unexpected_response);
    }
  }

  /// Merges the atoms of a `[CAPABILITY ...]` resp-code, when present.
  void add_code_atoms(const response_code<Allocator>& code) {
    if (const auto* caps = std::get_if<capability_code<Allocator>>(&code)) {
      for (const auto& cap : caps->capabilities) {
        capabilities_.insert(cap);
      }
    }
  }

  /// Splits a space-separated capability list into the result set.
  void add_atoms(std::string_view atoms) {
    while (!atoms.empty()) {
      const auto sp = atoms.find(' ');
      const auto atom = atoms.substr(0, sp);
      if (!atom.empty()) capabilities_.insert(atom);
      if (sp == std::string_view::npos) break;
      atoms.remove_prefix(sp + 1);
    }
  }

  capability_set<Allocator> capabilities_;  // Accumulated atoms.
  string_type wire_;                        // Whole command line.
  bool emitted_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_CAPABILITY_COMMAND_H_
