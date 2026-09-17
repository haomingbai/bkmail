/**
 * @file tests/integration/test_session_flow.cpp
 * @brief End-to-end session workflows mirroring the usage.md examples.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §1.3 (minimal example, blocking style), §1.4
 * (the same flow as a coroutine) and §2.5 (common workflows). The examples
 * in the guide use implicit TLS; these tests run the identical call sequence
 * over plaintext loopback against the scripted fake server, so the user-
 * facing flow is exercised line by line without certificate plumbing.
 *
 *   - MinimalBlockingFlow: connect → login → select → fetch_envelopes of
 *     the five most recent messages → logout (§1.3);
 *   - CoroutineFlow: the same shape inside a bexec::task, packed with the
 *     public bkmail::pack helper (§1.4 + code_layout D7);
 *   - ListMailboxes: list("", "*") entry parsing (§2.5);
 *   - SearchThenReadMessage: search UNSEEN + fetch_message BODY.PEEK[]
 *     (§2.5);
 *   - StoreModesAndExpunge: store_mode remove/replace wire forms (§2.5);
 *   - MoveWithoutCapabilityFailsLocally: UIDPLUS MOVE gating with
 *     errc::capability_required (§2.5/§8).
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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
using bkmail::test::server_send;
using bkmail::test::server_step;

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

/// One alternative of a sync_wait_with_variant / into_variant outcome:
/// (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

/// Extracts the error code from whichever alternative the outcome holds.
[[nodiscard]] std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
}

constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

/// Greeting + probe + LOGIN + SELECT of an INBOX with @p count messages.
[[nodiscard]] std::vector<server_step> session_prefix(
    std::uint32_t count, std::string_view capabilities) {
  return {
      server_send{"* OK [CAPABILITY " + std::string(capabilities) +
                  "] fake server ready\r\n"},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY " + std::string(capabilities) +
                  "\r\n{tag} OK CAPABILITY completed\r\n"},
      expect_client{"LOGIN"},
      server_send{"{tag} OK LOGIN completed\r\n"},
      expect_client{"SELECT"},
      server_send{"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
                  "* " +
                  std::to_string(count) +
                  " EXISTS\r\n"
                  "* 0 RECENT\r\n"
                  "* OK [UNSEEN 1] first unseen\r\n"
                  "* OK [UIDVALIDITY 3857529045] UIDs valid\r\n"
                  "* OK [UIDNEXT 100] next uid\r\n"
                  "{tag} OK [READ-WRITE] SELECT completed\r\n"},
  };
}

void append_logout(std::vector<server_step>& steps) {
  steps.push_back(expect_client{"LOGOUT"});
  steps.push_back(
      server_send{"* BYE fake server signing off\r\n"
                  "{tag} OK LOGOUT completed\r\n"});
}

/// One minimal ENVELOPE line carrying only a subject.
[[nodiscard]] std::string envelope_line(std::uint32_t seq,
                                        const std::string& subject) {
  return "* " + std::to_string(seq) + " FETCH (ENVELOPE (NIL \"" + subject +
         "\" NIL NIL NIL NIL NIL NIL NIL NIL))\r\n";
}

// ---- Branching-sender consumption helpers (usage.md §2.4) ---------------

/// Connects and returns the Not-Authenticated (OK greeting) branch.
[[nodiscard]] im::not_authenticated_state<> connect_ok(fake_imap_server& server,
                                                       bnio::io_context& ioc) {
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect("127.0.0.1", std::to_string(server.port()), ioc));
  EXPECT_TRUE(connected.has_value());
  auto* branch =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  EXPECT_NE(nullptr, branch) << "expected the OK-greeting branch";
  if (branch == nullptr) {
    throw std::runtime_error("connect did not yield not_authenticated_state");
  }
  auto& [ec, state] = *branch;
  if (ec) {
    throw std::runtime_error("connect failed: " + ec.message());
  }
  return std::move(state);
}

/// LOGINs and returns the Authenticated (success) branch.
[[nodiscard]] im::authenticated_state<> login_ok(
    im::not_authenticated_state<> state) {
  bkmail::account_info<> account{.user_name = "user", .password = "pw"};
  auto outcome = bth::sync_wait_with_variant(std::move(state).login(account));
  EXPECT_TRUE(outcome.has_value());
  auto* branch =
      std::get_if<state_outcome<im::authenticated_state<>>>(&*outcome);
  EXPECT_NE(nullptr, branch) << "expected the login success branch";
  if (branch == nullptr) {
    throw std::runtime_error("login took the failure branch");
  }
  auto& [ec, authed] = *branch;
  if (ec) {
    throw std::runtime_error("login failed: " + ec.message());
  }
  return std::move(authed);
}

/// SELECTs and returns the Selected (success) branch.
[[nodiscard]] im::selected_state<> select_ok(im::authenticated_state<> state,
                                             std::string_view mailbox) {
  auto outcome = bth::sync_wait_with_variant(std::move(state).select(mailbox));
  EXPECT_TRUE(outcome.has_value());
  auto* branch = std::get_if<state_outcome<im::selected_state<>>>(&*outcome);
  EXPECT_NE(nullptr, branch) << "expected the select success branch";
  if (branch == nullptr) {
    throw std::runtime_error("select took the failure branch");
  }
  auto& [ec, selected] = *branch;
  if (ec) {
    throw std::runtime_error("select failed: " + ec.message());
  }
  return std::move(selected);
}

// usage.md §1.3: the minimal blocking flow, step by step as documented.
TEST(SessionFlow, MinimalBlockingFlow) {
  fake_imap_server server;
  auto steps = session_prefix(5, kCapabilities);
  steps.push_back(expect_client{"FETCH"});
  std::string fetch_reply;
  for (std::uint32_t seq = 1; seq <= 5; ++seq) {
    fetch_reply += envelope_line(seq, "subject " + std::to_string(seq));
  }
  fetch_reply += "{tag} OK FETCH completed\r\n";
  steps.push_back(server_send{std::move(fetch_reply)});
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;

  // The next block follows usage.md §1.3 step by step (plaintext variant);
  // the branching senders (connect/login/select) publish one set_value
  // signature per outcome and are merged at the consumption point with
  // sync_wait_with_variant (usage.md §2.4).
  auto connected = bth::sync_wait_with_variant(bkmail::async_connect(
      "127.0.0.1", std::to_string(server.port()), runner.get()));
  ASSERT_TRUE(connected.has_value());
  auto* greeting =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  ASSERT_NE(nullptr, greeting);
  auto& [connect_ec, not_authed] = *greeting;
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  bkmail::account_info<> account{.user_name = "you@example.com",
                                 .password = "app-specific-password"};
  auto logged_in =
      bth::sync_wait_with_variant(std::move(not_authed).login(account));
  ASSERT_TRUE(logged_in.has_value());
  auto* login_branch =
      std::get_if<state_outcome<im::authenticated_state<>>>(&*logged_in);
  ASSERT_NE(nullptr, login_branch);
  auto& [login_ec, authed] = *login_branch;
  ASSERT_FALSE(login_ec) << login_ec.message();

  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  ASSERT_TRUE(selected_result.has_value());
  auto* select_branch =
      std::get_if<state_outcome<im::selected_state<>>>(&*selected_result);
  ASSERT_NE(nullptr, select_branch);
  auto& [select_ec, selected] = *select_branch;
  ASSERT_FALSE(select_ec) << select_ec.message();

  const std::uint32_t total = selected.mailbox().exists;
  ASSERT_EQ(5U, total);

  // "1:5": the five most recent messages (all of them here).
  auto fetched = bth::sync_wait(std::move(selected).fetch_envelopes("1:5"));
  ASSERT_TRUE(fetched.has_value());
  auto& [fetch_ec, envelopes, selected_back] = *fetched;
  ASSERT_FALSE(fetch_ec) << fetch_ec.message();

  std::vector<std::string> subjects;
  for (const auto& env : envelopes) {
    subjects.push_back(env.subject);
  }
  EXPECT_EQ((std::vector<std::string>{"subject 1", "subject 2", "subject 3",
                                      "subject 4", "subject 5"}),
            subjects);

  auto done = bth::sync_wait(std::move(selected_back).logout());
  ASSERT_TRUE(done.has_value());
  auto& [logout_ec, logged_out] = *done;
  EXPECT_FALSE(logout_ec) << logout_ec.message();
  (void)logged_out;

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §1.4 + code_layout D7: the coroutine flow. Branching senders
// (connect/login/select) publish several value completions, which a
// coroutine cannot await directly; bexec::into_variant merges them into a
// single variant value at the await point (usage.md §2.4), while the
// single-signature operations keep using bkmail::pack.
bexec::task<std::error_code> fetch_subjects_task(bnio::io_context& ioc,
                                                 std::uint16_t port,
                                                 std::vector<std::string>& out,
                                                 std::uint32_t& total_out) {
  auto greeting = co_await bexec::into_variant(
      bkmail::async_connect("127.0.0.1", std::to_string(port), ioc));
  const std::error_code connect_ec = outcome_ec(greeting);
  if (connect_ec) co_return connect_ec;
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&greeting);
  if (not_authed == nullptr) co_return connect_ec;  // PREAUTH: see §2.4

  bkmail::account_info<> account{.user_name = "you@example.com",
                                 .password = "app-specific-password"};
  auto logged_in = co_await bexec::into_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  if (const std::error_code ec = outcome_ec(logged_in)) co_return ec;
  // ec == 0: the Authenticated branch is the only possible outcome.
  auto& authed = std::get<1>(
      std::get<state_outcome<im::authenticated_state<>>>(logged_in));

  auto selected_outcome =
      co_await bexec::into_variant(std::move(authed).select("INBOX"));
  if (const std::error_code ec = outcome_ec(selected_outcome)) co_return ec;
  auto selected = std::move(std::get<1>(
      std::get<state_outcome<im::selected_state<>>>(selected_outcome)));

  total_out = selected.mailbox().exists;
  if (total_out != 0) {
    auto [fetch_ec, envelopes, selected_back] =
        co_await bkmail::pack(std::move(selected).fetch_envelopes("1:*"));
    if (fetch_ec) co_return fetch_ec;
    for (const auto& env : envelopes) {
      out.push_back(env.subject);
    }
    selected = std::move(selected_back);
  }

  auto [logout_ec, logged_out] =
      co_await bkmail::pack(std::move(selected).logout());
  co_return logout_ec;
}

TEST(SessionFlow, CoroutineFlow) {
  fake_imap_server server;
  auto steps = session_prefix(2, kCapabilities);
  steps.push_back(expect_client{"FETCH"});
  steps.push_back(server_send{envelope_line(1, "co one") +
                              envelope_line(2, "co two") +
                              "{tag} OK FETCH completed\r\n"});
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  std::vector<std::string> subjects;
  std::uint32_t total = 0;

  auto result = bth::sync_wait(
      fetch_subjects_task(runner.get(), server.port(), subjects, total));
  ASSERT_TRUE(result.has_value());
  auto& [ec] = *result;
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_EQ(2U, total);
  EXPECT_EQ((std::vector<std::string>{"co one", "co two"}), subjects);

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.5: LIST "" "*" entry parsing.
TEST(SessionFlow, ListMailboxes) {
  fake_imap_server server;
  server.start({
      server_send{"* OK [CAPABILITY " + std::string(kCapabilities) +
                  "] ready\r\n"},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY " + std::string(kCapabilities) +
                  "\r\n{tag} OK CAPABILITY completed\r\n"},
      expect_client{"LOGIN"},
      server_send{"{tag} OK LOGIN completed\r\n"},
      expect_client{"LIST"},
      server_send{"* LIST (\\HasNoChildren) \"/\" INBOX\r\n"
                  "* LIST (\\NoSelect \\HasChildren) \"/\" Archives\r\n"
                  "{tag} OK LIST completed\r\n"},
      expect_client{"LOGOUT"},
      server_send{"* BYE bye\r\n{tag} OK LOGOUT completed\r\n"},
  });

  io_runner runner;
  auto authed = login_ok(connect_ok(server, runner.get()));

  auto res = bth::sync_wait(std::move(authed).list("", "*"));
  ASSERT_TRUE(res.has_value());
  auto& [ec, entries, authed_back] = *res;
  ASSERT_FALSE(ec) << ec.message();

  ASSERT_EQ(2U, entries.size());
  EXPECT_EQ("INBOX", entries.at(0).name);
  EXPECT_EQ("/", entries.at(0).delimiter);
  EXPECT_FALSE(entries.at(0).no_select);
  EXPECT_TRUE(entries.at(0).has_no_children);
  EXPECT_EQ("Archives", entries.at(1).name);
  EXPECT_TRUE(entries.at(1).no_select);
  EXPECT_TRUE(entries.at(1).has_children);

  auto done = bth::sync_wait(std::move(authed_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.5: "Search unread and read one body" — BODY.PEEK[].
TEST(SessionFlow, SearchThenReadMessage) {
  const std::string rfc822 =
      "Subject: hello subject\r\n"
      "From: alice@example.com\r\n"
      "To: bob@example.net\r\n"
      "\r\n"
      "body line one\r\n"
      "body line two\r\n";

  fake_imap_server server;
  auto steps = session_prefix(1, kCapabilities);
  steps.insert(
      steps.end(),
      {
          expect_client{"SEARCH"},
          server_send{"* SEARCH 1\r\n{tag} OK SEARCH completed\r\n"},
          expect_client{"FETCH"},
          server_send{"* 1 FETCH (BODY[] {" + std::to_string(rfc822.size()) +
                      "}\r\n" + rfc822 + ")\r\n{tag} OK FETCH completed\r\n"},
      });
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto selected =
      select_ok(login_ok(connect_ok(server, runner.get())), "INBOX");

  auto found = bth::sync_wait(std::move(selected).search("UNSEEN"));
  ASSERT_TRUE(found.has_value());
  auto& [search_ec, unseen, sel1] = *found;
  ASSERT_FALSE(search_ec) << search_ec.message();
  ASSERT_EQ(1U, unseen.size());
  EXPECT_EQ(1U, unseen.front());
  selected = std::move(sel1);

  auto got = bth::sync_wait(
      std::move(selected).fetch_message(std::to_string(unseen.front())));
  ASSERT_TRUE(got.has_value());
  auto& [fetch_ec, messages, sel2] = *got;
  ASSERT_FALSE(fetch_ec) << fetch_ec.message();
  ASSERT_EQ(1U, messages.size());
  const bkmail::mail<>& m = messages.front();
  EXPECT_EQ("hello subject", m.header.subject);
  ASSERT_EQ(1U, m.header.from.size());
  EXPECT_EQ("alice@example.com", m.header.from.front().email());
  const std::string body(reinterpret_cast<const char*>(m.body.data.data()),
                         m.body.data.size());
  EXPECT_NE(std::string::npos, body.find("body line one"));
  EXPECT_NE(std::string::npos, body.find("body line two"));

  auto done = bth::sync_wait(std::move(sel2).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.5: store_mode remove/replace render as -FLAGS / FLAGS.
TEST(SessionFlow, StoreModesRenderOnTheWire) {
  fake_imap_server server;
  auto steps = session_prefix(5, kCapabilities);
  steps.insert(steps.end(), {
                                expect_client{"-FLAGS"},
                                server_send{"{tag} OK STORE completed\r\n"},
                                expect_client{" STORE "},
                                server_send{"{tag} OK STORE completed\r\n"},
                            });
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto selected =
      select_ok(login_ok(connect_ok(server, runner.get())), "INBOX");

  im::flag_set seen;
  seen.set(im::message_flag::seen);

  auto removed = bth::sync_wait(
      std::move(selected).store("1:5", seen, im::store_mode::remove));
  ASSERT_TRUE(removed.has_value());
  auto& [remove_ec, sel1] = *removed;
  ASSERT_FALSE(remove_ec) << remove_ec.message();

  auto replaced = bth::sync_wait(
      std::move(sel1).store("1:5", seen, im::store_mode::replace));
  ASSERT_TRUE(replaced.has_value());
  auto& [replace_ec, sel2] = *replaced;
  ASSERT_FALSE(replace_ec) << replace_ec.message();

  auto done = bth::sync_wait(std::move(sel2).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  ASSERT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
  const std::string received = server.received();
  EXPECT_NE(std::string::npos, received.find("-FLAGS"))
      << "store_mode::remove must render as -FLAGS";
  EXPECT_EQ(std::string::npos, received.find("+FLAGS"))
      << "no add-mode STORE was issued in this session";
}

// usage.md §2.5/§8: MOVE needs UIDPLUS; without it the operation fails
// locally with errc::capability_required and nothing hits the wire.
TEST(SessionFlow, MoveWithoutCapabilityFailsLocally) {
  fake_imap_server server;
  constexpr std::string_view kNoUidplus = "IMAP4rev1 IDLE LITERAL+ SASL-IR";
  auto steps = session_prefix(3, kNoUidplus);
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto selected =
      select_ok(login_ok(connect_ok(server, runner.get())), "INBOX");

  auto moved = bth::sync_wait(std::move(selected).move("1:2", "Archive"));
  ASSERT_TRUE(moved.has_value());
  auto& [move_ec, sel_back] = *moved;
  EXPECT_EQ(move_ec, bkmail::errc::capability_required);

  auto done = bth::sync_wait(std::move(sel_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  ASSERT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
  EXPECT_EQ(std::string::npos, server.received().find(" MOVE "))
      << "a MOVE command must never reach the wire without the capability";
}

}  // namespace
