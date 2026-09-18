/**
 * @file main.cpp
 * @brief Read-only connectivity session against a real IMAP server.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-17
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * End-to-end proof for the full stack against a live server, kept out of
 * the test suite so `ctest` never needs the network. On success it
 * prints, in order: the mailbox (folder) list, the newest message
 * subjects, and the leading lines of the two newest messages.
 *
 * The session is strictly READ-ONLY: connect (implicit TLS) -> login
 * (AUTHENTICATE PLAIN when advertised, LOGIN otherwise) -> LIST -> EXAMINE
 * INBOX (never SELECT — no write side effects) -> FETCH ENVELOPE -> FETCH
 * BODY.PEEK[HEADER.FIELDS (...)] (bounded preview, never sets \Seen) ->
 * LOGOUT. STORE/EXPUNGE/APPEND/COPY/MOVE/DELETE/CLOSE are never issued.
 *
 * Usage:
 *   real_server_session <host> [port]        (port defaults to 993)
 * Credentials come from the environment, never from the source tree:
 *   BKMAIL_USER       IMAP user name
 *   BKMAIL_PASSWORD   IMAP password (use an app-specific password)
 *
 * SECURITY: credentials are never logged or printed — only the server
 * host, the step results, and mailbox data appear in the output.
 */

#include <bkmail/bkmail.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// Runs a bnio::io_context on a dedicated thread for the object's
/// lifetime: sync_wait blocks the calling thread, so the context needs
/// its own worker.
class io_runner {
 public:
  io_runner() : thread_([this] { (void)ioc_.run(); }) {}
  ~io_runner() {
    ioc_.stop();
    thread_.join();
  }

  [[nodiscard]] bnio::io_context& get() noexcept { return ioc_; }

  // Runs one empty task through the context and waits for it: when this
  // returns, deferred teardown work scheduled on the io thread (e.g. the
  // connection detained after LOGOUT) has finished, so exiting right
  // after cannot cut a pending close short.
  void quiesce() {
    class done_receiver {
     public:
      explicit done_receiver(std::atomic<bool>& done) noexcept : done_(done) {}
      void set_value(std::error_code) noexcept { done_.store(true); }
      void set_stopped() noexcept { done_.store(true); }

     private:
      std::atomic<bool>& done_;
    };
    std::atomic<bool> done{false};
    auto operation = bexec::connect(ioc_.get_post_scheduler().schedule(),
                                    done_receiver{done});
    bexec::start(operation);
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

 private:
  bnio::io_context ioc_;
  std::thread thread_;
};

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

/// Extracts the error code from whichever alternative the outcome holds.
[[nodiscard]] std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
}

/// Prints "what: message" on stderr and returns true when ec is set.
[[nodiscard]] bool failed(const char* what, std::error_code ec) {
  if (!ec) return false;
  std::fprintf(stderr, "%s: %s\n", what, ec.message().c_str());
  return true;
}

/// Reads one required environment variable; exits with a hint when unset.
[[nodiscard]] const char* required_env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    std::fprintf(stderr, "error: set the %s environment variable\n", name);
    std::exit(2);
  }
  return value;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr,
                 "usage: %s <host> [port]   (port defaults to 993)\n"
                 "credentials: BKMAIL_USER / BKMAIL_PASSWORD environment "
                 "variables\n",
                 argv[0]);
    return 2;
  }
  const char* host = argv[1];
  const char* port = argc == 3 ? argv[2] : "993";
  const char* user = required_env("BKMAIL_USER");
  const char* password = required_env("BKMAIL_PASSWORD");

  io_runner runner;
  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};

  // Connect (implicit TLS) and consume the greeting; the connect sender
  // publishes one set_value signature per outcome state.
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect_tls(host, port, runner.get(), tls));
  if (!connected) return 1;  // operation was stopped
  if (failed("connect", outcome_ec(*connected))) return 1;
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  if (not_authed == nullptr) {
    std::fprintf(stderr, "unexpected PREAUTH/BYE greeting from %s\n", host);
    return 1;
  }
  std::printf("== connected to %s:%s\n", host, port);

  // CAPABILITY probe (also refreshes the connection's capability cache).
  auto caps_result =
      bth::sync_wait(std::move(std::get<1>(*not_authed)).capability());
  if (!caps_result) return 1;
  auto& [caps_ec, caps, not_authed2] = *caps_result;
  if (failed("capability", caps_ec)) return 1;

  // Login: AUTHENTICATE PLAIN when advertised, LOGIN otherwise. The
  // credential material never leaves this scope and is never printed.
  std::optional<im::authenticated_state<>> authed;
  if (caps.contains("AUTH=PLAIN")) {
    // SASL PLAIN initial response: authzid NUL authcid NUL password.
    std::string initial_response;
    initial_response.push_back('\0');
    initial_response.append(user);
    initial_response.push_back('\0');
    initial_response.append(password);
    auto res = bth::sync_wait_with_variant(
        std::move(not_authed2).authenticate("PLAIN", initial_response));
    if (!res) return 1;
    if (failed("authenticate PLAIN", outcome_ec(*res))) return 1;
    authed.emplace(std::move(
        std::get<1>(std::get<state_outcome<im::authenticated_state<>>>(*res))));
  } else {
    bkmail::account_info<> account{.user_name = user, .password = password};
    auto logged_in =
        bth::sync_wait_with_variant(std::move(not_authed2).login(account));
    if (!logged_in) return 1;
    if (failed("login", outcome_ec(*logged_in))) return 1;
    authed.emplace(std::move(std::get<1>(
        std::get<state_outcome<im::authenticated_state<>>>(*logged_in))));
  }
  std::printf("== logged in\n");

  // Mailbox (folder) list.
  auto listed = bth::sync_wait(std::move(*authed).list("", "*"));
  if (!listed) return 1;
  auto& [list_ec, mailboxes, authed_back] = *listed;
  if (failed("list", list_ec)) return 1;
  std::printf("== mailboxes (%zu):\n", mailboxes.size());
  for (const auto& entry : mailboxes) {
    std::printf("  %s%s\n", entry.name.c_str(),
                entry.no_select ? "  (not selectable)" : "");
  }

  // EXAMINE INBOX — deliberately not SELECT: EXAMINE is read-only.
  auto examined =
      bth::sync_wait_with_variant(std::move(authed_back).examine("INBOX"));
  if (!examined) return 1;
  if (failed("examine INBOX", outcome_ec(*examined))) return 1;
  auto& selected =
      std::get<1>(std::get<state_outcome<im::selected_state<>>>(*examined));
  const std::uint32_t total = selected.mailbox().exists;
  std::printf("== INBOX: %u message(s)%s\n", total,
              selected.mailbox().read_only ? " (read-only)" : "");

  im::selected_state<> back = std::move(selected);
  if (total > 0) {
    // Header preview of the newest messages via
    // BODY.PEEK[HEADER.FIELDS (...)] — bounded on the wire and in memory,
    // never sets \Seen. (A full BODY.PEEK[] would buffer arbitrarily
    // large messages — up to the whole mailbox — before printing.)
    constexpr std::uint32_t kListCount = 5;
    const std::uint32_t first =
        total > kListCount ? total - (kListCount - 1) : 1;
    // "first:total" over two uint32_t needs at most 5 + 1 + 10 + 1 bytes;
    // 24 leaves headroom and the truncation check catches the impossible.
    char range[24];
    const int written =
        std::snprintf(range, sizeof range, "%u:%u", first, total);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof range) {
      std::fprintf(stderr, "sequence-set does not fit the buffer\n");
      return 1;
    }
    auto fetched = bth::sync_wait(
        std::move(back).fetch_headers(range, {"Date", "From", "Subject"}));
    if (!fetched) return 1;
    auto& [fetch_ec, envelopes, sel1] = *fetched;
    if (failed("fetch headers", fetch_ec)) return 1;
    std::printf("== newest %zu message(s):\n", envelopes.size());
    std::uint32_t seq = first;
    for (const auto& m : envelopes) {
      std::string from;
      if (!m.from.empty()) from = m.from.front().email();
      std::printf("  #%u [%s] %s — %s\n", seq, m.date.c_str(), from.c_str(),
                  m.subject.c_str());
      ++seq;
    }
    back = std::move(sel1);
  }

  // LOGOUT.
  auto done = bth::sync_wait(std::move(back).logout());
  if (!done) return 1;
  const int rc = failed("logout", std::get<0>(*done)) ? 1 : 0;
  runner.quiesce();
  std::printf("== logged out\n");
  return rc;
}
