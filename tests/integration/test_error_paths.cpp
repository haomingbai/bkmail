/**
 * @file tests/integration/test_error_paths.cpp
 * @brief Error-path contract: transport errors, EOF, BYE, stop tokens.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §6 (error handling: error codes on the value
 * channel, fatal vs recoverable errors, reconnect policy) and §2.7
 * (cancellation completes with set_stopped, never through the error code).
 * Covered here:
 *
 *   - an injected transport error fails every pending Layer-1 handler with
 *     that error code and stops the read pump;
 *   - an orderly server EOF mid-command fails the pending handler and stops
 *     the pump;
 *   - the state layer surfaces an unsolicited BYE as errc::server_bye on the
 *     next operation (usage.md §6 reconnect policy);
 *   - cancelling an in-flight state operation through a receiver stop token
 *     completes with set_stopped; the tagged reply is discarded when it
 *     arrives (usage.md §2.7).
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bkmail::test::expect_client;
using bkmail::test::expect_write;
using bkmail::test::fake_imap_server;
using bkmail::test::io_runner;
using bkmail::test::kDefaultTimeout;
using bkmail::test::poll_until;
using bkmail::test::script_recorder;
using bkmail::test::scripted_stream;
using bkmail::test::server_bytes;
using bkmail::test::server_eof;
using bkmail::test::server_error;
using bkmail::test::server_send;
using bkmail::test::signal_event;

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

using scripted_context = im::imap_context<scripted_stream>;

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

constexpr std::string_view kGreeting = "* OK fake ready\r\n";
constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

/// Extracts the success branch State from a branching sender's outcome;
/// throws on set_stopped, the wrong branch, or a truthy error code.
template <class State, class Outcome>
[[nodiscard]] State success_branch(Outcome& outcome, const char* what) {
  if (!outcome.has_value()) {
    throw std::runtime_error(std::string(what) + " completed with set_stopped");
  }
  auto* branch = std::get_if<state_outcome<State>>(&*outcome);
  if (branch == nullptr) {
    throw std::runtime_error(std::string(what) + " took an unexpected branch");
  }
  auto& [ec, state] = *branch;
  if (ec) {
    throw std::runtime_error(std::string(what) + " failed: " + ec.message());
  }
  return std::move(state);
}

/// Drives the session into the selected state; throws on setup failure.
[[nodiscard]] im::selected_state<> connect_login_select(
    fake_imap_server& server, bnio::io_context& ioc) {
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect("127.0.0.1", std::to_string(server.port()), ioc));
  auto not_authed =
      success_branch<im::not_authenticated_state<>>(connected, "connect");

  bkmail::account_info<> account{.user_name = "user", .password = "pw"};
  auto logged_in =
      bth::sync_wait_with_variant(std::move(not_authed).login(account));
  auto authed = success_branch<im::authenticated_state<>>(logged_in, "login");

  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  return success_branch<im::selected_state<>>(selected_result, "select");
}

// usage.md §6: transport failures travel on the value channel and are fatal
// to the session; the injected error code reaches the pending handler.
TEST(ErrorPaths, TransportErrorFailsPendingHandler) {
  const auto injected = std::make_error_code(std::errc::connection_reset);

  io_runner runner;
  scripted_stream stream({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001 SEARCH"},
      server_error{injected},
  });
  script_recorder recorder = stream.recorder();
  scripted_context ctx{std::move(stream), runner.get()};

  signal_event search_done;
  std::error_code search_ec;
  ctx.submit(im::search_command<>{"ALL"},
             [&](std::error_code ec, std::vector<std::uint32_t>) {
               search_ec = ec;
               search_done.arrive();
             });
  ctx.flush();

  ASSERT_TRUE(search_done.wait_for(kDefaultTimeout));
  // architecture §3.5: connection_lost(ec) fails registry and queue with ec.
  EXPECT_EQ(injected, search_ec);
  EXPECT_TRUE(poll_until([&] { return !ctx.is_alive(); }, kDefaultTimeout));
}

// usage.md §6: EOF (the server hanging up mid-command) is fatal; the
// pending handler completes with a truthy error code and the pump stops.
TEST(ErrorPaths, ServerHangupFailsPendingHandler) {
  io_runner runner;
  scripted_stream stream({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001 NOOP"},
      server_eof{},
  });
  script_recorder recorder = stream.recorder();
  scripted_context ctx{std::move(stream), runner.get()};

  signal_event done;
  std::optional<std::error_code> result_ec;
  ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    result_ec = ec;
    done.arrive();
  });
  ctx.flush();

  ASSERT_TRUE(done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(result_ec.has_value());
  EXPECT_TRUE(*result_ec) << "EOF mid-command must not report success";
  EXPECT_TRUE(poll_until([&] { return !ctx.is_alive(); }, kDefaultTimeout));
}

// usage.md §6 reconnect policy: an unsolicited BYE between commands surfaces
// as errc::server_bye on the NEXT state-layer operation.
TEST(ErrorPaths, ServerByeSurfacesOnNextStateOperation) {
  fake_imap_server server;
  server.start(
      {
          server_send{"* OK [CAPABILITY " + std::string(kCapabilities) +
                      "] ready\r\n"},
          expect_client{"CAPABILITY"},
          server_send{"* CAPABILITY " + std::string(kCapabilities) +
                      "\r\n{tag} OK CAPABILITY completed\r\n"},
          expect_client{"LOGIN"},
          server_send{"{tag} OK LOGIN completed\r\n"},
          expect_client{"SELECT"},
          server_send{"* FLAGS (\\Seen \\Deleted)\r\n"
                      "* 1 EXISTS\r\n"
                      "* 0 RECENT\r\n"
                      "* OK [UIDVALIDITY 1] valid\r\n"
                      "{tag} OK [READ-WRITE] SELECT completed\r\n"},
          // BYE pushed while no command is in flight; then the server dies.
          server_send{"* BYE scheduled maintenance\r\n"},
      },
      /*close_at_end=*/true);

  io_runner runner;
  auto selected = connect_login_select(server, runner.get());

  auto found = bth::sync_wait(std::move(selected).search("ALL"));
  ASSERT_TRUE(found.has_value());
  auto& [search_ec, matches, state_back] = *found;
  EXPECT_EQ(search_ec, bkmail::errc::server_bye);
  (void)state_back;  // Session dead: discard and reconnect (usage.md §6).

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
}

// usage.md §2.7: requesting stop on an in-flight state operation completes
// it with set_stopped; IMAP cannot un-send the command, so the (never
// arriving here) tagged reply would simply be discarded.
TEST(ErrorPaths, StopTokenCancelsInFlightOperation) {
  fake_imap_server server;
  server.start(
      {
          server_send{"* OK [CAPABILITY " + std::string(kCapabilities) +
                      "] ready\r\n"},
          expect_client{"CAPABILITY"},
          server_send{"* CAPABILITY " + std::string(kCapabilities) +
                      "\r\n{tag} OK CAPABILITY completed\r\n"},
          expect_client{"LOGIN"},
          server_send{"{tag} OK LOGIN completed\r\n"},
          expect_client{"SELECT"},
          server_send{"* FLAGS (\\Seen)\r\n"
                      "* 1 EXISTS\r\n"
                      "* 0 RECENT\r\n"
                      "* OK [UIDVALIDITY 1] valid\r\n"
                      "{tag} OK [READ-WRITE] SELECT completed\r\n"},
          expect_client{"SEARCH"},
          // Deliberately no tagged reply, and the connection stays open:
          // an EOF here would race the cancellation and could complete
          // the operation on the error channel before the stop is even
          // requested. With the server silent and connected, the stop
          // token is the only way this operation can complete.
      },
      /*close_at_end=*/false);

  io_runner runner;
  auto selected = connect_login_select(server, runner.get());

  // The custom-receiver consumption style of usage.md §2.2/§2.7.
  class search_receiver {
   public:
    using env_type = bexec::env_with_stop_token<>;

    search_receiver(bexec::inplace_stop_token token, signal_event& stopped,
                    std::atomic<int>& value_calls)
        : env_(token), stopped_(&stopped), value_calls_(&value_calls) {}

    env_type get_env() const noexcept { return env_; }

    void set_value(std::error_code, std::vector<std::uint32_t>,
                   im::selected_state<>) noexcept {
      ++(*value_calls_);
    }

    void set_stopped() noexcept { stopped_->arrive(); }

   private:
    env_type env_;
    signal_event* stopped_;
    std::atomic<int>* value_calls_;
  };

  signal_event stopped;
  std::atomic<int> value_calls{0};
  bexec::inplace_stop_source stop_src;

  auto op = bexec::connect(
      std::move(selected).search("ALL"),
      search_receiver{stop_src.get_token(), stopped, value_calls});
  bexec::start(op);

  // Cancel once the command is on the wire (written, awaiting the reply).
  ASSERT_TRUE(server.wait_received("SEARCH", kDefaultTimeout));
  stop_src.request_stop();

  ASSERT_TRUE(stopped.wait_for(kDefaultTimeout))
      << "cancellation completes with set_stopped, never an error code";
  EXPECT_EQ(0, value_calls.load());

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
}

}  // namespace
