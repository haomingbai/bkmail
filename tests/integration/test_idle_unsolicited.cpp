/**
 * @file tests/integration/test_idle_unsolicited.cpp
 * @brief IDLE wait/push/DONE flows and unsolicited snapshot updates.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §2.6 (waiting for new mail with IDLE), §2.7
 * (cancellation with stop tokens; IDLE sends DONE before completing) and
 * §2.1 (unsolicited EXISTS/EXPUNGE/FETCH(FLAGS) update the mailbox snapshot
 * between commands). Covered here:
 *
 *   - idle() parks the connection, a pushed EXISTS completes it, the state
 *     returns with the new EXISTS count, and the DONE handshake is visible
 *     on the wire;
 *   - cancelling idle() through a receiver stop token sends DONE and
 *     completes with set_stopped (never through the error code);
 *   - unsolicited EXPUNGE/FETCH(FLAGS) pushed between commands land in the
 *     selected_state::mailbox() snapshot, observed after the next completed
 *     operation.
 *
 * All tests run the state layer against the loopback fake server.
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bkmail::test::expect_client;
using bkmail::test::fake_imap_server;
using bkmail::test::io_runner;
using bkmail::test::kDefaultTimeout;
using bkmail::test::quiesce_guard;
using bkmail::test::server_send;
using bkmail::test::server_step;
using bkmail::test::signal_event;

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

/// Greeting + probe + LOGIN + SELECT with @p count messages.
[[nodiscard]] std::vector<server_step> selected_prefix(std::uint32_t count) {
  return {
      server_send{"* OK [CAPABILITY " + std::string(kCapabilities) +
                  "] fake server ready\r\n"},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY " + std::string(kCapabilities) +
                  "\r\n{tag} OK CAPABILITY completed\r\n"},
      expect_client{"LOGIN"},
      server_send{"{tag} OK LOGIN completed\r\n"},
      expect_client{"SELECT"},
      server_send{"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
                  "* " +
                  std::to_string(count) +
                  " EXISTS\r\n"
                  "* 0 RECENT\r\n"
                  "* OK [UIDVALIDITY 4242] UIDs valid\r\n"
                  "* OK [UIDNEXT 100] next uid\r\n"
                  "{tag} OK [READ-WRITE] SELECT completed\r\n"},
  };
}

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

// usage.md §2.6: idle() completes when the server reports activity; the
// returned state's mailbox() reflects the pushed EXISTS count.
TEST(IdleUnsolicited, IdlePushExistsCompletesWithUpdatedSnapshot) {
  fake_imap_server server;
  auto steps = selected_prefix(3);
  steps.insert(steps.end(),
               {
                   expect_client{"IDLE"},
                   server_send{"+ idling\r\n"},
                   // New mail arrives while IDLE is parked.
                   server_send{"* 4 EXISTS\r\n"},
                   // bkmail ends the IDLE cycle with DONE (RFC 2177).
                   expect_client{"DONE"},
                   server_send{"{tag} OK IDLE terminated\r\n"},
                   expect_client{"LOGOUT"},
                   server_send{"* BYE bye\r\n{tag} OK LOGOUT completed\r\n"},
               });
  server.start(std::move(steps));

  io_runner runner;
  auto selected = connect_login_select(server, runner.get());
  ASSERT_EQ(3U, selected.mailbox().exists);

  auto idled = bth::sync_wait(std::move(selected).idle());
  ASSERT_TRUE(idled.has_value());
  auto& [idle_ec, selected_back] = *idled;
  ASSERT_FALSE(idle_ec) << idle_ec.message();
  EXPECT_EQ(4U, selected_back.mailbox().exists)
      << "mailbox() reflects the newest EXISTS after idle() (usage §2.6)";

  auto done = bth::sync_wait(std::move(selected_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.7: cancelling idle() through the receiver's stop token sends
// DONE and completes with set_stopped — never through the error code.
TEST(IdleUnsolicited, IdleCancelSendsDoneAndCompletesStopped) {
  fake_imap_server server;
  auto steps = selected_prefix(2);
  steps.insert(steps.end(), {
                                expect_client{"IDLE"},
                                server_send{"+ idling\r\n"},
                                expect_client{"DONE"},
                                server_send{"{tag} OK IDLE terminated\r\n"},
                            });
  server.start(std::move(steps));

  io_runner runner;
  auto selected = connect_login_select(server, runner.get());

  // The receiver shape of usage.md §2.7, with an env carrying the token.
  class idle_receiver {
   public:
    using env_type = bexec::env_with_stop_token<>;

    idle_receiver(bexec::inplace_stop_token token, signal_event& stopped,
                  std::atomic<int>& value_calls)
        : env_(token), stopped_(&stopped), value_calls_(&value_calls) {}

    env_type get_env() const noexcept { return env_; }

    void set_value(std::error_code, im::selected_state<>) noexcept {
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

  auto op =
      bexec::connect(std::move(selected).idle(),
                     idle_receiver{stop_src.get_token(), stopped, value_calls});
  bexec::start(op);
  // Covers every early return below: the io worker must have left the
  // receiver call chain before the stack-allocated operation state and
  // receiver go out of scope.
  quiesce_guard teardown{runner};

  // Cancel only once the IDLE command is actually on the wire.
  ASSERT_TRUE(server.wait_received("IDLE", kDefaultTimeout));
  stop_src.request_stop();

  ASSERT_TRUE(stopped.wait_for(kDefaultTimeout))
      << "a cancelled idle() completes with set_stopped (usage §2.7)";
  EXPECT_EQ(0, value_calls.load());
  EXPECT_TRUE(server.wait_received("DONE", kDefaultTimeout))
      << "bkmail sends DONE before completing a cancelled idle()";

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.1: unsolicited EXPUNGE/FETCH(FLAGS) pushed between commands
// update the mailbox snapshot; reading it after the next completed
// operation is the contractual observation point.
TEST(IdleUnsolicited, UnsolicitedPushesUpdateMailboxSnapshot) {
  fake_imap_server server;
  auto steps = selected_prefix(3);
  steps.insert(steps.end(),
               {
                   expect_client{"NOOP"},
                   // Pushed between commands, ahead of the NOOP completion
                   // in the same server burst: on the wire the EXPUNGE
                   // deterministically precedes the tagged reply, so the
                   // snapshot observation after NOOP (usage.md §2.1) is
                   // race-free. One expunge (EXISTS 3 → 2) and one flag
                   // change.
                   server_send{"* 2 EXPUNGE\r\n"
                               "* 1 FETCH (FLAGS (\\Seen \\Flagged))\r\n"
                               "{tag} OK NOOP completed\r\n"},
                   expect_client{"LOGOUT"},
                   server_send{"* BYE bye\r\n{tag} OK LOGOUT completed\r\n"},
               });
  server.start(std::move(steps));

  io_runner runner;
  auto selected = connect_login_select(server, runner.get());
  ASSERT_EQ(3U, selected.mailbox().exists);

  // NOOP is the observation round trip: after it completes, mailbox() must
  // reflect the pushed EXPUNGE.
  auto nooped = bth::sync_wait(std::move(selected).noop());
  ASSERT_TRUE(nooped.has_value());
  auto& [noop_ec, selected_back] = *nooped;
  ASSERT_FALSE(noop_ec) << noop_ec.message();
  EXPECT_EQ(2U, selected_back.mailbox().exists);

  auto done = bth::sync_wait(std::move(selected_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

}  // namespace
