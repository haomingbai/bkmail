/**
 * @file tests/layer2/test_connect.cpp
 * @brief Connect entry points and greeting branches of the state layer.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §1.2 (the two ways to connect), §1.3 (minimal
 * blocking flow) and §2.4 (matching the per-outcome greeting branches).
 * Covered here:
 *
 *   - an OK greeting completes with `not_authenticated_state` (login
 *     required);
 *   - a PREAUTH greeting completes with `authenticated_state` (already
 *     logged in);
 *   - a BYE greeting completes with `logout_state` and
 *     `errc::server_bye`, fabricating no session state (code_layout D2);
 *   - the capability probing built into async_connect (code_layout D6) is
 *     observable on the wire of every connection.
 *
 * The connect sender publishes one set_value signature per outcome state;
 * the tests consume it with bexec::this_thread::sync_wait_with_variant
 * (the consumer-side merge point of usage.md §2.4).
 *
 * All tests run the real client stack against a scripted loopback server
 * (tests/support/fake_imap_server.h); the plaintext entry `async_connect` is
 * used throughout.
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>

#include <cstdint>
#include <optional>
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
using bkmail::test::server_send;
using bkmail::test::server_step;

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

// The connect sender publishes one set_value signature per outcome state
// (design ruling: no merged variant payload in the library interface).
static_assert(
    std::same_as<
        decltype(bkmail::async_connect(
            "h", "143",
            std::declval<bnio::io_context&>()))::completion_signatures,
        bexec::completion_signatures<
            bexec::set_value_t(std::error_code, im::not_authenticated_state<>),
            bexec::set_value_t(std::error_code, im::authenticated_state<>),
            bexec::set_value_t(std::error_code, im::logout_state<>),
            bexec::set_stopped_t()>>);

/// The capability set every script in this suite advertises.
constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

/// Greeting line plus the CAPABILITY probe wired into async_connect (D6).
[[nodiscard]] std::vector<server_step> greeting_script(std::string greeting) {
  return {
      server_send{std::move(greeting)},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY " + std::string(kCapabilities) +
                  "\r\n{tag} OK CAPABILITY completed\r\n"},
  };
}

/// Appends a clean LOGOUT exchange to a script.
void append_logout(std::vector<server_step>& steps) {
  steps.push_back(expect_client{"LOGOUT"});
  steps.push_back(
      server_send{"* BYE fake server signing off\r\n"
                  "{tag} OK LOGOUT completed\r\n"});
}

/// Connects to the fake server; fails the test on anything but a value
/// completion, returns the per-outcome (ec, state) variant.
[[nodiscard]] auto connect_to(fake_imap_server& server, bnio::io_context& ioc) {
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect("127.0.0.1", std::to_string(server.port()), ioc));
  EXPECT_TRUE(connected.has_value())
      << "connect sender completed with set_stopped";
  EXPECT_TRUE(server.wait_received("CAPABILITY", kDefaultTimeout))
      << "async_connect did not probe CAPABILITY on the wire";
  return connected;
}

// usage.md §1.2/§2.4: an OK greeting selects the not-authenticated branch.
TEST(Connect, OkGreetingYieldsNotAuthenticatedState) {
  fake_imap_server server;
  auto steps =
      greeting_script("* OK [CAPABILITY " + std::string(kCapabilities) +
                      "] fake server ready\r\n");
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto connected = connect_to(server, runner.get());
  ASSERT_TRUE(connected.has_value());

  auto* ok_branch =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  ASSERT_NE(nullptr, ok_branch)
      << "OK greeting must yield not_authenticated_state";
  EXPECT_EQ(nullptr,
            std::get_if<state_outcome<im::authenticated_state<>>>(&*connected));
  auto& [connect_ec, not_authed] = *ok_branch;
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  // logout() is legal in every live state (usage.md §2.1).
  auto done = bth::sync_wait(std::move(not_authed).logout());
  ASSERT_TRUE(done.has_value());
  auto& [logout_ec, logged_out] = *done;
  EXPECT_FALSE(logout_ec) << logout_ec.message();
  static_assert(std::same_as<std::remove_cvref_t<decltype(logged_out)>,
                             im::logout_state<>>);

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §1.2/§2.4: a PREAUTH greeting selects the authenticated branch,
// and the session is usable without a LOGIN round trip.
TEST(Connect, PreauthGreetingYieldsUsableAuthenticatedState) {
  fake_imap_server server;
  auto steps =
      greeting_script("* PREAUTH [CAPABILITY " + std::string(kCapabilities) +
                      "] already authenticated\r\n");
  steps.insert(steps.end(),
               {
                   expect_client{"SELECT"},
                   server_send{"* FLAGS (\\Answered \\Flagged \\Deleted "
                               "\\Seen \\Draft)\r\n"
                               "* 0 EXISTS\r\n"
                               "* 0 RECENT\r\n"
                               "* OK [UIDVALIDITY 4711] UIDs valid\r\n"
                               "* OK [UIDNEXT 1] next uid\r\n"
                               "{tag} OK [READ-WRITE] SELECT completed\r\n"},
               });
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto connected = connect_to(server, runner.get());
  ASSERT_TRUE(connected.has_value());

  auto* preauth_branch =
      std::get_if<state_outcome<im::authenticated_state<>>>(&*connected);
  ASSERT_NE(nullptr, preauth_branch)
      << "PREAUTH greeting must yield authenticated_state";
  auto& [connect_ec, authed] = *preauth_branch;
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  ASSERT_TRUE(selected_result.has_value());
  auto* select_ok =
      std::get_if<state_outcome<im::selected_state<>>>(&*selected_result);
  ASSERT_NE(nullptr, select_ok);
  auto& [select_ec, selected] = *select_ok;
  ASSERT_FALSE(select_ec) << select_ec.message();
  EXPECT_EQ(0U, selected.mailbox().exists);
  EXPECT_EQ(4711U, selected.mailbox().uid_validity);

  auto done = bth::sync_wait(std::move(selected).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// code_layout D2: a BYE greeting delivers the terminal logout_state; the
// connect sender reports errc::server_bye on the value channel.
TEST(Connect, ByeGreetingReportsServerBye) {
  fake_imap_server server;
  server.start({server_send{"* BYE server is overloaded\r\n"}},
               /*close_at_end=*/true);

  io_runner runner;
  auto connected = bth::sync_wait_with_variant(bkmail::async_connect(
      "127.0.0.1", std::to_string(server.port()), runner.get()));
  ASSERT_TRUE(connected.has_value());
  auto* bye_branch =
      std::get_if<state_outcome<im::logout_state<>>>(&*connected);
  ASSERT_NE(nullptr, bye_branch)
      << "a BYE greeting completes with logout_state";
  EXPECT_EQ(std::get<0>(*bye_branch), bkmail::errc::server_bye);

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
}

// usage.md §1.2/§7: the connect sender composes with plain sync_wait-style
// consumption; a failed DNS resolution (no listener anywhere) must surface
// as a value error code, never as an exception — sync_wait only throws
// error-channel values and bkmail senders have none (usage.md §6).
TEST(Connect, RefusedConnectionReportsErrorCodeNotException) {
  io_runner runner;
  // Port 9 (discard) on loopback has no listener in the test environment.
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect("127.0.0.1", "9", runner.get()));
  ASSERT_TRUE(connected.has_value());
  auto* failed_branch =
      std::get_if<state_outcome<im::logout_state<>>>(&*connected);
  ASSERT_NE(nullptr, failed_branch)
      << "a failed connect delivers logout_state alongside the error";
  EXPECT_TRUE(std::get<0>(*failed_branch))
      << "connecting to a dead port must fail";
}

}  // namespace
