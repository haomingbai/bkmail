/**
 * @file src/error.cpp
 * @brief bkmail error category implementation.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/error.h>

#include <string>
#include <system_error>

namespace {

class bkmail_error_category final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "bkmail"; }

  [[nodiscard]] std::string message(int condition) const override {
    switch (static_cast<bkmail::errc>(condition)) {
      case bkmail::errc::command_rejected:
        return "command rejected by the server (tagged NO)";
      case bkmail::errc::bad_command:
        return "command unknown or malformed (tagged BAD)";
      case bkmail::errc::server_bye:
        return "server closed the connection (untagged BYE)";
      case bkmail::errc::unexpected_response:
        return "unexpected or malformed server response";
      case bkmail::errc::capability_required:
        return "operation requires a capability the server does not advertise";
    }
    return "unknown bkmail error";
  }
};

}  // namespace

namespace bkmail {

const std::error_category& error_category() noexcept {
  static const bkmail_error_category category;
  return category;
}

std::error_code make_error_code(errc e) noexcept {
  return std::error_code(static_cast<int>(e), error_category());
}

}  // namespace bkmail
