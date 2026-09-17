/**
 * @file main.cpp
 * @brief Lists the subjects of the most recent INBOX messages (blocking
 *        style).
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Executable mirror of docs/usage.md §1.3: connect over implicit TLS, log
 * in, SELECT INBOX, FETCH the ENVELOPEs of the five most recent messages,
 * print their subjects, and log out. Every step blocks the calling thread
 * with bexec::this_thread::sync_wait, so the borrowed bnio::io_context runs
 * on its own worker thread (the io_runner helper of usage.md §1.3).
 *
 * Usage:
 *   list_subjects <host> [port]        (port defaults to 993)
 * Credentials come from the environment, never from the source tree:
 *   BKMAIL_USER       IMAP user name
 *   BKMAIL_PASSWORD   IMAP password (use an app-specific password)
 *
 * Safety: the session is read-only in effect — SELECT opens the mailbox
 * and FETCH ENVELOPE never sets \Seen; nothing is stored, moved, or
 * expunged. LOGOUT leaves the server state untouched.
 */

#include <bexec/bexec.hpp>
#include <bkmail/bkmail.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// Runs a bnio::io_context on a dedicated thread for the object's
/// lifetime (usage.md §1.3): sync_wait blocks the current thread, so the
/// context needs its own worker.
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
      explicit done_receiver(std::atomic<bool>& done) noexcept
          : done_(done) {}
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

/// Prints "what: message" on stderr and returns true when ec is set.
[[nodiscard]] bool failed(const char* what, std::error_code ec) {
  if (!ec) return false;
  std::fprintf(stderr, "%s: %s\n", what, ec.message().c_str());
  return true;
}

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

/// Extracts the error code from whichever alternative the outcome holds.
[[nodiscard]] std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
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

  // Connect (implicit TLS) and consume the greeting. The connect sender
  // publishes one set_value signature per outcome; sync_wait_with_variant
  // merges them into a variant of (ec, state) tuples (usage.md §2.4).
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect_tls(host, port, runner.get(), tls));
  if (!connected) return 1;  // operation was stopped
  if (failed("connect", outcome_ec(*connected))) return 1;

  // OK means we must log in, PREAUTH means the server already considers us
  // authenticated. Handle the common OK case; see usage.md §2.4 for the
  // full visit pattern.
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  if (not_authed == nullptr) {
    std::fprintf(stderr, "unexpected PREAUTH greeting\n");
    return 1;
  }

  // LOGIN with a password (use authenticate_oauth2 for XOAUTH2 servers).
  // login() is branching too: Authenticated on OK, the retained
  // Not-Authenticated state on rejection.
  bkmail::account_info<> account{.user_name = user, .password = password};
  auto logged_in = bth::sync_wait_with_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  if (!logged_in) return 1;
  if (failed("login", outcome_ec(*logged_in))) return 1;
  auto& authed = std::get<1>(
      std::get<state_outcome<im::authenticated_state<>>>(*logged_in));

  // SELECT INBOX; the Selected state carries a mailbox_info snapshot.
  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  if (!selected_result) return 1;
  if (failed("select", outcome_ec(*selected_result))) return 1;
  auto& selected = std::get<1>(
      std::get<state_outcome<im::selected_state<>>>(*selected_result));

  const std::uint32_t total = selected.mailbox().exists;
  std::printf("INBOX has %u messages\n", total);
  if (total == 0) {
    auto logout = bth::sync_wait(std::move(selected).logout());
    if (!logout) return 1;
    const int rc = failed("logout", std::get<0>(*logout)) ? 1 : 0;
    runner.quiesce();
    return rc;
  }

  // FETCH the ENVELOPE of the five most recent messages.
  constexpr std::uint32_t kRecentCount = 5;
  // "first:total" over two uint32_t needs at most 5 + 1 + 10 + 1 bytes;
  // 24 leaves headroom and the truncation check catches the impossible.
  char range[24];
  const std::uint32_t first =
      total > kRecentCount ? total - (kRecentCount - 1) : 1;
  const int written = std::snprintf(range, sizeof range, "%u:%u", first, total);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof range) {
    std::fprintf(stderr, "sequence-set does not fit the buffer\n");
    return 1;
  }
  auto fetched = bth::sync_wait(std::move(selected).fetch_envelopes(range));
  if (!fetched) return 1;
  auto& [fetch_ec, envelopes, selected_back] = *fetched;
  if (failed("fetch", fetch_ec)) return 1;

  for (const auto& env : envelopes) {
    std::printf("- %s\n", env.subject.c_str());
  }

  // LOGOUT. The final state exists only to confirm the exchange completed.
  auto done = bth::sync_wait(std::move(selected_back).logout());
  if (!done) return 1;
  auto& [logout_ec, logged_out] = *done;
  const int rc = failed("logout", logout_ec) ? 1 : 0;
  runner.quiesce();
  return rc;
}
