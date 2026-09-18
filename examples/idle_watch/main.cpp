/**
 * @file main.cpp
 * @brief Watches INBOX for new/deleted mail with IDLE, stopped by SIGINT.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Executable mirror of the docs/usage.md §2.6 watch_inbox loop with the
 * §2.7 stop-token cancellation wired to SIGINT: connect over implicit TLS,
 * log in, SELECT INBOX, then re-issue selected_state::idle() in a loop.
 * Each wake-up means the server pushed activity (mailbox() then reflects
 * the newest EXISTS) or the RFC 5550 29-minute heartbeat point arrived.
 *
 * Cancellation: idle() takes its stop token from the receiver environment
 * (usage.md §2.7). The receiver below carries an inplace_stop_token; a
 * SIGINT flag polled by the main loop makes it call request_stop(), and
 * bkmail then sends DONE before completing with set_stopped(). A stopped
 * idle() does not hand the state back, so the connection is torn down by
 * destroying the session; a normal exit (after --max-events) logs out
 * cleanly instead.
 *
 * Usage:
 *   idle_watch <host> [port] [max-events]
 *     port        defaults to 993
 *     max-events  exit and log out after this many mailbox events
 *                 (0 = watch until SIGINT; default 0)
 * Credentials come from the environment, never from the source tree:
 *   BKMAIL_USER       IMAP user name
 *   BKMAIL_PASSWORD   IMAP password (use an app-specific password)
 *
 * Safety: read-only in effect — SELECT + IDLE + ENVELOPE-free watching
 * never stores flags, moves, or expunges anything.
 */

#include <bkmail/bkmail.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <semaphore>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// Set by the SIGINT handler; the main loop polls it and requests stop on
/// the operation's stop source (request_stop is deliberately kept off the
/// signal handler: only atomic stores are async-signal-safe here).
std::atomic<bool> g_sigint_seen = false;

extern "C" void on_sigint(int) { g_sigint_seen.store(true); }

/// Runs a bnio::io_context on a dedicated thread for the object's
/// lifetime (usage.md §1.3).
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

[[nodiscard]] const char* required_env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    std::fprintf(stderr, "error: set the %s environment variable\n", name);
    std::exit(2);
  }
  return value;
}

/// Shared slots between the idle receiver (io thread) and the main loop.
struct idle_outcome {
  std::binary_semaphore done{0};
  std::optional<im::selected_state<>> state;
  std::error_code ec;
  bool stopped = false;
};

/// Receiver for one idle() cycle, with the stop token in its environment
/// (usage.md §2.7). On set_value the selected state is handed back through
/// the outcome slot; on set_stopped (cancellation, DONE already sent) no
/// state comes back.
class idle_receiver {
 public:
  using env_type = bexec::env_with_stop_token<>;

  idle_receiver(bexec::inplace_stop_token token, idle_outcome& outcome)
      : env_(token), outcome_(&outcome) {}

  [[nodiscard]] env_type get_env() const noexcept { return env_; }

  void set_value(std::error_code ec, im::selected_state<> state) noexcept {
    outcome_->ec = ec;
    outcome_->state.emplace(std::move(state));
    outcome_->done.release();
  }

  void set_stopped() noexcept {
    outcome_->stopped = true;
    outcome_->done.release();
  }

 private:
  env_type env_;
  idle_outcome* outcome_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr,
                 "usage: %s <host> [port] [max-events]\n"
                 "credentials: BKMAIL_USER / BKMAIL_PASSWORD environment "
                 "variables\n",
                 argv[0]);
    return 2;
  }
  const char* host = argv[1];
  const char* port = argc >= 3 ? argv[2] : "993";
  const std::uint32_t max_events =
      argc == 4 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10))
                : 0;
  const char* user = required_env("BKMAIL_USER");
  const char* password = required_env("BKMAIL_PASSWORD");

  std::signal(SIGINT, on_sigint);

  io_runner runner;
  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};

  // Connect, log in, and select INBOX (plain sync_wait_with_variant steps;
  // the watch loop below is the part that needs cancellation). Branching
  // senders publish one set_value signature per outcome (usage.md §2.4).
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect_tls(host, port, runner.get(), tls));
  if (!connected) return 1;
  if (failed("connect", outcome_ec(*connected))) return 1;
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  if (not_authed == nullptr) {
    std::fprintf(stderr, "unexpected PREAUTH greeting\n");
    return 1;
  }
  bkmail::account_info<> account{.user_name = user, .password = password};
  auto logged_in = bth::sync_wait_with_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  if (!logged_in) return 1;
  if (failed("login", outcome_ec(*logged_in))) return 1;
  auto& authed = std::get<1>(
      std::get<state_outcome<im::authenticated_state<>>>(*logged_in));
  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  if (!selected_result) return 1;
  if (failed("select", outcome_ec(*selected_result))) return 1;
  auto selected = std::move(std::get<1>(
      std::get<state_outcome<im::selected_state<>>>(*selected_result)));

  std::uint32_t last_exists = selected.mailbox().exists;
  std::printf("watching INBOX (%u messages); Ctrl-C to stop\n", last_exists);

  bexec::inplace_stop_source stop_src;
  std::uint32_t events_seen = 0;
  bool stop_requested = false;
  bool cancelled = false;

  for (;;) {
    idle_outcome outcome;
    {
      auto op = bexec::connect(std::move(selected).idle(),
                               idle_receiver{stop_src.get_token(), outcome});
      bexec::start(op);

      // Wait for the cycle to end, polling the SIGINT flag; request stop
      // from this thread, never from the signal handler. Once a stop was
      // requested, allow a grace period for the DONE + tagged-reply
      // exchange: on a half-open connection the server's receipt never
      // arrives, and waiting forever would hang the example.
      std::chrono::steady_clock::time_point stop_deadline{};
      while (!outcome.done.try_acquire_for(std::chrono::milliseconds{100})) {
        if (!stop_requested && g_sigint_seen.load()) {
          stop_requested = true;
          stop_deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds{10};
          stop_src.request_stop();
        }
        if (stop_requested &&
            std::chrono::steady_clock::now() > stop_deadline) {
          // The started idle() operation state cannot be destroyed while
          // in flight, and waiting longer cannot help — exit the process
          // without unwinding; the OS reclaims the descriptors.
          std::fprintf(stderr,
                       "server did not confirm the cancellation; exiting\n");
          std::_Exit(1);
        }
      }
      // `op` must not outlive the completion it delivered.
    }

    if (outcome.stopped) {
      // set_stopped: DONE was sent, but the state is not handed back, so
      // the session ends here by destruction.
      std::puts("interrupted; connection closed");
      cancelled = true;
      break;
    }
    if (failed("idle", outcome.ec)) return 1;
    selected = std::move(*outcome.state);

    // Woke up: pushed activity or the heartbeat point. mailbox() reflects
    // the newest pushed view (usage.md §2.6).
    const std::uint32_t now_exists = selected.mailbox().exists;
    if (now_exists > last_exists) {
      std::printf("new mail: %u message(s) arrived (total %u)\n",
                  now_exists - last_exists, now_exists);
      ++events_seen;
    } else if (now_exists < last_exists) {
      std::printf("mail removed: %u message(s) expunged (total %u)\n",
                  last_exists - now_exists, now_exists);
      ++events_seen;
    }
    last_exists = now_exists;

    if (max_events != 0 && events_seen >= max_events) {
      break;
    }
  }

  if (cancelled) {
    // The idle cycle completed with set_stopped; let the io thread finish
    // any deferred teardown before the runner goes away.
    runner.quiesce();
    return 0;
  }

  // Normal exit path: the state is back, so LOGOUT cleanly.
  auto done = bth::sync_wait(std::move(selected).logout());
  if (!done) return 1;
  auto& [logout_ec, logged_out] = *done;
  const int rc = failed("logout", logout_ec) ? 1 : 0;
  runner.quiesce();
  return rc;
}
