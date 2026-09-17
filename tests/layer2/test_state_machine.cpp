/**
 * @file tests/layer2/test_state_machine.cpp
 * @brief State-machine layer transitions, constraints, and failure semantics.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §2.1 (states at a glance), §2.3 (reading the
 * error code and the next state), §2.5 (common workflows) and §6 (error
 * handling; NO/BAD recoverable, BYE fatal). Covered here:
 *
 *   - compile-time state constraints: which operations each state offers,
 *     the &&-qualified consumption rule, the terminal logout_state
 *     (the "one command in flight" seriality of §2.1/§7 is enforced by the
 *     type system, so it is verified with concept checks);
 *   - the full chain login → select → fetch_envelopes → search → store →
 *     copy → move → expunge → logout, including mailbox_info aggregation
 *     (EXISTS/RECENT/UNSEEN/UIDVALIDITY/UIDNEXT/FLAGS) and live snapshot
 *     updates from unsolicited EXISTS/EXPUNGE between commands;
 *   - examine → read-only mailbox, and store failing with
 *     errc::command_rejected there (usage.md §2.1);
 *   - tagged NO/BAD recoverability (usage.md §6): the session stays usable;
 *   - close() returning to the authenticated state;
 *   - unsolicited BYE failing the next operation with errc::server_bye.
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>

#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
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

constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

// ---------------------------------------------------------------------------
// Compile-time contract: the type system enforces the state machine
// (usage.md §2.1, §7 rule 6). These checks stand in for negative-compilation
// tests: a requires-expression that must NOT be satisfied proves the call is
// ill-formed.
// ---------------------------------------------------------------------------

using bkmail::account_info;
using im::authenticated_state;
using im::logout_state;
using im::not_authenticated_state;
using im::selected_state;

// The negative checks below are phrased as concepts rather than plain
// `static_assert(!requires(...))`: the Apple Clang in the build toolchain
// (incorrectly) hard-errors on invalid expressions inside requires-clauses
// evaluated outside a template context, while substitution into a concept
// definition follows the SFINAE rules these checks rely on.
template <class S>
concept can_login = requires(S s, account_info<> a) { std::move(s).login(a); };
template <class S>
concept can_oauth2 = requires(S s) {
  std::move(s).authenticate_oauth2("user@example.com", "token");
};
template <class S>
concept can_select = requires(S s) { std::move(s).select("INBOX"); };
template <class S>
concept can_examine = requires(S s) { std::move(s).examine("INBOX"); };
template <class S>
concept can_list = requires(S s) { std::move(s).list("", "*"); };
template <class S>
concept can_search = requires(S s) { std::move(s).search("ALL"); };
template <class S>
concept can_fetch_envelopes =
    requires(S s) { std::move(s).fetch_envelopes("1:*"); };
template <class S>
concept can_fetch_headers =
    requires(S s) { std::move(s).fetch_headers("1:*", {"Subject"}); };
template <class S>
concept can_fetch_message = requires(S s) { std::move(s).fetch_message("1"); };
template <class S>
concept can_store = requires(S s, im::flag_set f) {
  std::move(s).store("1", f, im::store_mode::add);
};
template <class S>
concept can_copy = requires(S s) { std::move(s).copy("1", "A"); };
template <class S>
concept can_move = requires(S s) { std::move(s).move("1", "A"); };
template <class S>
concept can_expunge = requires(S s) { std::move(s).expunge(); };
template <class S>
concept can_idle = requires(S s) { std::move(s).idle(); };
template <class S>
concept can_close = requires(S s) { std::move(s).close(); };
template <class S>
concept can_uid_search = requires(S s) { std::move(s).uid_search("ALL"); };
template <class S>
concept can_capability = requires(S s) { std::move(s).capability(); };
template <class S>
concept can_noop = requires(S s) { std::move(s).noop(); };
template <class S>
concept can_logout = requires(S s) { std::move(s).logout(); };
// Lvalue spellings: must NOT compile (operations consume the state).
template <class S>
concept can_search_lvalue = requires(S& s) { s.search("ALL"); };
template <class S>
concept can_select_lvalue = requires(S& s) { s.select("INBOX"); };
template <class S>
concept can_login_lvalue = requires(S& s, account_info<> a) { s.login(a); };

// login/authenticate_oauth2 exist only in the not-authenticated state.
static_assert(can_login<not_authenticated_state<>>);
static_assert(can_oauth2<not_authenticated_state<>>);
static_assert(!can_login<authenticated_state<>>);
static_assert(!can_login<selected_state<>>);

// select/examine/list exist only in the authenticated state.
static_assert(can_select<authenticated_state<>>);
static_assert(can_examine<authenticated_state<>>);
static_assert(can_list<authenticated_state<>>);
static_assert(!can_select<not_authenticated_state<>>);

// Mailbox operations exist only in the selected state.
static_assert(can_search<selected_state<>>);
static_assert(can_fetch_envelopes<selected_state<>>);
static_assert(can_fetch_headers<selected_state<>>);
static_assert(can_fetch_message<selected_state<>>);
static_assert(can_store<selected_state<>>);
static_assert(can_copy<selected_state<>>);
static_assert(can_move<selected_state<>>);
static_assert(can_expunge<selected_state<>>);
static_assert(can_idle<selected_state<>>);
static_assert(can_close<selected_state<>>);
static_assert(can_uid_search<selected_state<>>);
static_assert(!can_search<authenticated_state<>>);
static_assert(!can_expunge<authenticated_state<>>);
static_assert(!can_idle<authenticated_state<>>);

// capability/noop/logout exist on all three live states.
static_assert(can_capability<not_authenticated_state<>>);
static_assert(can_noop<authenticated_state<>>);
static_assert(can_logout<selected_state<>>);

// Operations consume the state: lvalues must not compile (&&-qualified).
static_assert(!can_search_lvalue<selected_state<>>);
static_assert(!can_select_lvalue<authenticated_state<>>);
static_assert(!can_login_lvalue<not_authenticated_state<>>);

// logout_state is terminal.
static_assert(!can_logout<logout_state<>>);
static_assert(!can_noop<logout_state<>>);

// mailbox() is a const accessor returning the mailbox snapshot.
static_assert(requires(const selected_state<>& s) {
  { s.mailbox() } -> std::same_as<const im::mailbox_info<>&>;
});

// ---------------------------------------------------------------------------
// Completion-signature shapes (design ruling: one set_value signature per
// successor state; merged variants never appear in the library interface).
// ---------------------------------------------------------------------------

// login: Authenticated on OK, the retained Not-Authenticated state on
// NO/BAD/transport failure.
static_assert(
    std::same_as<
        decltype(std::declval<not_authenticated_state<>>().login(
            std::declval<const account_info<>&>()))::completion_signatures,
        bexec::completion_signatures<
            bexec::set_value_t(std::error_code, authenticated_state<>),
            bexec::set_value_t(std::error_code, not_authenticated_state<>),
            bexec::set_stopped_t()>>);

// select: Selected on OK, the retained Authenticated state on NO/failure
// (RFC 3501: a failed SELECT selects no mailbox).
static_assert(
    std::same_as<decltype(std::declval<authenticated_state<>>().select(
                     "INBOX"))::completion_signatures,
                 bexec::completion_signatures<
                     bexec::set_value_t(std::error_code, selected_state<>),
                     bexec::set_value_t(std::error_code, authenticated_state<>),
                     bexec::set_stopped_t()>>);

// Same-state operations keep the single-signature shape (resultful and
// void-result variants shown once each).
static_assert(
    std::same_as<decltype(std::declval<authenticated_state<>>().list(
                     "", "*"))::completion_signatures,
                 bexec::completion_signatures<
                     bexec::set_value_t(std::error_code,
                                        std::vector<im::mailbox_entry<>>,
                                        authenticated_state<>),
                     bexec::set_stopped_t()>>);
static_assert(
    std::same_as<decltype(std::declval<selected_state<>>()
                              .expunge())::completion_signatures,
                 bexec::completion_signatures<
                     bexec::set_value_t(std::error_code, selected_state<>),
                     bexec::set_stopped_t()>>);

// The merged-variant aliases (session_state / greeting_state) are gone
// from the public API. Their absence cannot be name-checked here — naming
// a nonexistent template is a parse-time hard error on this toolchain, not
// a substitution failure — so the signature-shape assertions above (and
// the connect-sender assertion in test_connect.cpp) stand in: reintroducing
// a merged variant payload would change those signatures and fail them.

// ---------------------------------------------------------------------------
// Runtime contract: scripted sessions over loopback TCP.
// ---------------------------------------------------------------------------

/// Greeting + CAPABILITY probe + LOGIN, the common prefix of most sessions.
[[nodiscard]] std::vector<server_step> login_prefix() {
  return {
      server_send{"* OK [CAPABILITY " + std::string(kCapabilities) +
                  "] fake server ready\r\n"},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY " + std::string(kCapabilities) +
                  "\r\n{tag} OK CAPABILITY completed\r\n"},
      expect_client{"LOGIN"},
      server_send{"{tag} OK LOGIN completed\r\n"},
  };
}

/// The SELECT INBOX exchange used by the chain test: 3 messages, full
/// FLAGS/UNSEEN/UIDVALIDITY/UIDNEXT reporting (usage.md §2.1, §4).
[[nodiscard]] std::vector<server_step> select_steps() {
  return {
      expect_client{"SELECT"},
      server_send{"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
                  "* 3 EXISTS\r\n"
                  "* 1 RECENT\r\n"
                  "* OK [UNSEEN 2] first unseen\r\n"
                  "* OK [UIDVALIDITY 3857529045] UIDs valid\r\n"
                  "* OK [UIDNEXT 4392] predicted next uid\r\n"
                  "* OK [PERMANENTFLAGS (\\Answered \\Flagged \\Deleted "
                  "\\Seen \\Draft \\*)] flags permitted\r\n"
                  "{tag} OK [READ-WRITE] SELECT completed\r\n"},
  };
}

void append_logout(std::vector<server_step>& steps) {
  steps.push_back(expect_client{"LOGOUT"});
  steps.push_back(
      server_send{"* BYE fake server signing off\r\n"
                  "{tag} OK LOGOUT completed\r\n"});
}

/// Drives the session up to the authenticated state; throws on any failure
/// (test setup must never produce a half-valid state).
[[nodiscard]] im::authenticated_state<> connect_and_login(
    fake_imap_server& server, bnio::io_context& ioc) {
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect("127.0.0.1", std::to_string(server.port()), ioc));
  if (!connected.has_value()) {
    throw std::runtime_error("connect completed with set_stopped");
  }
  auto* greeting =
      std::get_if<state_outcome<not_authenticated_state<>>>(&*connected);
  if (greeting == nullptr) {
    throw std::runtime_error("expected an OK greeting branch");
  }
  auto& [connect_ec, not_authed] = *greeting;
  if (connect_ec) {
    throw std::runtime_error("connect failed: " + connect_ec.message());
  }

  account_info<> account{.user_name = "you@example.com", .password = "pw"};
  auto logged_in =
      bth::sync_wait_with_variant(std::move(not_authed).login(account));
  if (!logged_in.has_value()) {
    throw std::runtime_error("login completed with set_stopped");
  }
  auto* login_ok =
      std::get_if<state_outcome<authenticated_state<>>>(&*logged_in);
  if (login_ok == nullptr) {
    throw std::runtime_error("expected the login success branch");
  }
  auto& [login_ec, authed] = *login_ok;
  if (login_ec) {
    throw std::runtime_error("login failed: " + login_ec.message());
  }
  return std::move(authed);
}

/// Runs sync_wait_with_variant on select/examine and returns the Selected
/// success branch; throws (after EXPECT failures) on the failure branch.
template <class SelectSender>
[[nodiscard]] selected_state<> select_ok(SelectSender&& sender) {
  auto outcome =
      bth::sync_wait_with_variant(std::forward<SelectSender>(sender));
  EXPECT_TRUE(outcome.has_value());
  auto* branch = std::get_if<state_outcome<selected_state<>>>(&*outcome);
  EXPECT_NE(nullptr, branch)
      << "select/examine completed on the failure (Authenticated) branch";
  if (branch == nullptr) {
    throw std::runtime_error("select/examine took the failure branch");
  }
  auto& [ec, selected] = *branch;
  if (ec) {
    throw std::runtime_error("select failed: " + ec.message());
  }
  return std::move(selected);
}

// usage.md §1.3 + §2.5: the canonical sequential session, end to end.
TEST(StateMachine, FullChain) {
  fake_imap_server server;
  auto steps = login_prefix();
  auto sel = select_steps();
  steps.insert(steps.end(), sel.begin(), sel.end());
  steps.insert(
      steps.end(),
      {
          // fetch_envelopes("1:3"): one fully populated ENVELOPE
          // (all 10 fields) plus two all-NIL ones.
          expect_client{"FETCH"},
          server_send{"* 1 FETCH (ENVELOPE "
                      "(\"Wed, 17 Jul 1996 02:23:25 -0700\" \"subject one\" "
                      "((\"Alice\" NIL \"alice\" \"example.com\")) "
                      "((\"Alice\" NIL \"alice\" \"example.com\")) "
                      "((\"Alice\" NIL \"alice\" \"example.com\")) "
                      "((\"Bob\" NIL \"bob\" \"example.net\") "
                      "(\"Carol\" NIL \"carol\" \"example.org\")) "
                      "NIL NIL \"<thread-1@example.com>\" "
                      "\"<id-1@example.com>\"))\r\n"
                      "* 2 FETCH (ENVELOPE (NIL NIL NIL NIL NIL NIL NIL NIL "
                      "NIL NIL))\r\n"
                      "* 3 FETCH (ENVELOPE (NIL NIL NIL NIL NIL NIL NIL NIL "
                      "NIL NIL))\r\n"
                      "{tag} OK FETCH completed\r\n"},
          // search: matches plus an unsolicited EXISTS that must
          // land in the mailbox snapshot (usage.md §2.1).
          expect_client{"SEARCH"},
          server_send{"* SEARCH 2 3\r\n"
                      "* 4 EXISTS\r\n"
                      "{tag} OK SEARCH completed\r\n"},
          expect_client{"STORE"},
          server_send{"* 2 FETCH (FLAGS (\\Seen))\r\n"
                      "{tag} OK STORE completed\r\n"},
          expect_client{"COPY"},
          server_send{"{tag} OK COPY completed\r\n"},
          expect_client{"MOVE"},
          server_send{"{tag} OK MOVE completed\r\n"},
          // expunge: one EXPUNGE push; EXISTS drops from 4 to 3.
          expect_client{"EXPUNGE"},
          server_send{"* 1 EXPUNGE\r\n"
                      "{tag} OK EXPUNGE completed\r\n"},
      });
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  // SELECT INBOX and validate the aggregated mailbox_info snapshot.
  auto selected = select_ok(std::move(authed).select("INBOX"));
  static_assert(std::same_as<decltype(selected), selected_state<>>);
  {
    const im::mailbox_info<>& box = selected.mailbox();
    EXPECT_EQ(3U, box.exists);
    EXPECT_EQ(1U, box.recent);
    ASSERT_TRUE(box.unseen.has_value());
    EXPECT_EQ(2U, *box.unseen);
    EXPECT_EQ(3857529045U, box.uid_validity);
    EXPECT_EQ(4392U, box.uid_next);
    EXPECT_FALSE(box.read_only);
    EXPECT_TRUE(box.flags.test(im::message_flag::answered));
    EXPECT_TRUE(box.flags.test(im::message_flag::flagged));
    EXPECT_TRUE(box.flags.test(im::message_flag::deleted));
    EXPECT_TRUE(box.flags.test(im::message_flag::seen));
    EXPECT_TRUE(box.flags.test(im::message_flag::draft));
    EXPECT_TRUE(box.permanent_flags.test(im::message_flag::seen));
  }

  // FETCH ENVELOPE of the whole mailbox; verify the 10-field parse.
  auto fetched = bth::sync_wait(std::move(selected).fetch_envelopes("1:3"));
  ASSERT_TRUE(fetched.has_value());
  auto& [fetch_ec, envelopes, sel_after_fetch] = *fetched;
  ASSERT_FALSE(fetch_ec) << fetch_ec.message();
  ASSERT_EQ(3U, envelopes.size());
  {
    const bkmail::envelope<>& env = envelopes.front();
    EXPECT_EQ("Wed, 17 Jul 1996 02:23:25 -0700", env.date);
    EXPECT_EQ("subject one", env.subject);
    ASSERT_EQ(1U, env.from.size());
    EXPECT_EQ("Alice", env.from.front().display_name);
    EXPECT_EQ("alice", env.from.front().mailbox_name);
    EXPECT_EQ("example.com", env.from.front().host_name);
    EXPECT_EQ("alice@example.com", env.from.front().email());
    ASSERT_EQ(1U, env.sender.size());
    ASSERT_EQ(1U, env.reply_to.size());
    ASSERT_EQ(2U, env.to.size());
    EXPECT_EQ("bob@example.net", env.to.at(0).email());
    EXPECT_EQ("carol@example.org", env.to.at(1).email());
    EXPECT_TRUE(env.cc.empty());
    EXPECT_TRUE(env.bcc.empty());
    EXPECT_EQ("<thread-1@example.com>", env.in_reply_to);
    EXPECT_EQ("<id-1@example.com>", env.message_id);

    // NIL fields arrive as empty strings/vectors (code_layout D8).
    const bkmail::envelope<>& nil_env = envelopes.at(1);
    EXPECT_TRUE(nil_env.subject.empty());
    EXPECT_TRUE(nil_env.from.empty());
    EXPECT_TRUE(nil_env.message_id.empty());
  }
  selected = std::move(sel_after_fetch);

  // SEARCH; the unsolicited * 4 EXISTS updates the snapshot.
  auto found = bth::sync_wait(std::move(selected).search("UNSEEN"));
  ASSERT_TRUE(found.has_value());
  auto& [search_ec, matches, sel_after_search] = *found;
  ASSERT_FALSE(search_ec) << search_ec.message();
  EXPECT_EQ((std::vector<std::uint32_t>{2U, 3U}), matches);
  selected = std::move(sel_after_search);
  EXPECT_EQ(4U, selected.mailbox().exists);

  // STORE +FLAGS (\Seen).
  im::flag_set seen;
  seen.set(im::message_flag::seen);
  auto stored =
      bth::sync_wait(std::move(selected).store("2", seen, im::store_mode::add));
  ASSERT_TRUE(stored.has_value());
  auto& [store_ec, sel_after_store] = *stored;
  ASSERT_FALSE(store_ec) << store_ec.message();
  selected = std::move(sel_after_store);
  EXPECT_TRUE(server.wait_received("+FLAGS", kDefaultTimeout))
      << "store_mode::add must render as +FLAGS (usage.md §2.5)";

  // COPY and MOVE (MOVE is advertised via UIDPLUS).
  auto copied = bth::sync_wait(std::move(selected).copy("1:2", "Archive"));
  ASSERT_TRUE(copied.has_value());
  auto& [copy_ec, sel_after_copy] = *copied;
  ASSERT_FALSE(copy_ec) << copy_ec.message();
  auto moved = bth::sync_wait(std::move(sel_after_copy).move("1:2", "Archive"));
  ASSERT_TRUE(moved.has_value());
  auto& [move_ec, sel_after_move] = *moved;
  ASSERT_FALSE(move_ec) << move_ec.message();

  // EXPUNGE: the push renumbers the mailbox (usage.md §2.5).
  auto expunged = bth::sync_wait(std::move(sel_after_move).expunge());
  ASSERT_TRUE(expunged.has_value());
  auto& [expunge_ec, sel_after_expunge] = *expunged;
  ASSERT_FALSE(expunge_ec) << expunge_ec.message();
  EXPECT_EQ(3U, sel_after_expunge.mailbox().exists);

  // LOGOUT.
  auto done = bth::sync_wait(std::move(sel_after_expunge).logout());
  ASSERT_TRUE(done.has_value());
  auto& [logout_ec, logged_out] = *done;
  EXPECT_FALSE(logout_ec) << logout_ec.message();
  (void)logged_out;

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §6: a tagged NO reports errc::command_rejected, keeps the
// session alive, and hands back a usable state object. SELECT is the
// state-changing failure path: RFC 3501 leaves the session in the
// Authenticated state, so the NO completes on the Authenticated branch —
// the retained current state, not a placeholder Selected state.
TEST(StateMachine, RejectedSelectIsRecoverable) {
  fake_imap_server server;
  auto steps = login_prefix();
  steps.push_back(expect_client{"SELECT"});
  steps.push_back(server_send{"{tag} NO no such mailbox\r\n"});
  // The session must still work after the NO: prove it with a clean
  // SELECT of a real mailbox followed by logout.
  auto sel = select_steps();
  steps.insert(steps.end(), sel.begin(), sel.end());
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  auto rejected =
      bth::sync_wait_with_variant(std::move(authed).select("NoSuchBox"));
  ASSERT_TRUE(rejected.has_value());
  auto* failed_branch =
      std::get_if<state_outcome<authenticated_state<>>>(&*rejected);
  ASSERT_NE(nullptr, failed_branch)
      << "a failed SELECT must keep the Authenticated state (RFC 3501)";
  auto& [ec, state_back] = *failed_branch;
  EXPECT_EQ(ec, bkmail::errc::command_rejected);

  // usage.md §6: NO is recoverable — the retained Authenticated state is
  // fully usable: SELECT succeeds right away.
  auto selected = select_ok(std::move(state_back).select("INBOX"));

  auto done = bth::sync_wait(std::move(selected).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §6: a tagged BAD reports errc::bad_command and is recoverable.
// list() stays in the authenticated state, so the successor type is fixed.
TEST(StateMachine, BadCommandIsRecoverableAndKeepsState) {
  fake_imap_server server;
  auto steps = login_prefix();
  steps.push_back(expect_client{"LIST"});
  steps.push_back(server_send{"{tag} BAD syntax error\r\n"});
  auto sel = select_steps();
  steps.insert(steps.end(), sel.begin(), sel.end());  // session still usable
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  auto listed = bth::sync_wait(std::move(authed).list("", "*"));
  ASSERT_TRUE(listed.has_value());
  auto& [list_ec, entries, authed_back] = *listed;
  EXPECT_EQ(list_ec, bkmail::errc::bad_command);
  EXPECT_TRUE(entries.empty());
  static_assert(std::same_as<std::remove_cvref_t<decltype(authed_back)>,
                             authenticated_state<>>);

  // The connection is untouched: SELECT works right away.
  auto selected = select_ok(std::move(authed_back).select("INBOX"));

  auto done = bth::sync_wait(std::move(selected).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.1: examine is the read-only twin of select; a store there
// fails with NO which bkmail reports as errc::command_rejected.
TEST(StateMachine, ExamineSelectsReadOnlyMailbox) {
  fake_imap_server server;
  auto steps = login_prefix();
  steps.insert(steps.end(),
               {
                   expect_client{"EXAMINE"},
                   server_send{"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen "
                               "\\Draft)\r\n"
                               "* 2 EXISTS\r\n"
                               "* 0 RECENT\r\n"
                               "* OK [UIDVALIDITY 77] UIDs valid\r\n"
                               "* OK [UIDNEXT 3] next uid\r\n"
                               "{tag} OK [READ-ONLY] EXAMINE completed\r\n"},
                   expect_client{"STORE"},
                   server_send{"{tag} NO mailbox is read-only\r\n"},
               });
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  auto selected = select_ok(std::move(authed).examine("INBOX"));
  EXPECT_TRUE(selected.mailbox().read_only);
  EXPECT_EQ(2U, selected.mailbox().exists);

  im::flag_set seen;
  seen.set(im::message_flag::seen);
  auto stored =
      bth::sync_wait(std::move(selected).store("1", seen, im::store_mode::add));
  ASSERT_TRUE(stored.has_value());
  auto& [store_ec, sel_back] = *stored;
  EXPECT_EQ(store_ec, bkmail::errc::command_rejected);
  static_assert(
      std::same_as<std::remove_cvref_t<decltype(sel_back)>, selected_state<>>);

  auto done = bth::sync_wait(std::move(sel_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §2.5: CLOSE silently expunges and returns to the authenticated
// state — from there another mailbox can be selected immediately.
TEST(StateMachine, CloseReturnsToAuthenticatedState) {
  fake_imap_server server;
  auto steps = login_prefix();
  auto sel = select_steps();
  steps.insert(steps.end(), sel.begin(), sel.end());
  steps.push_back(expect_client{"CLOSE"});
  steps.push_back(server_send{"{tag} OK CLOSE completed\r\n"});
  auto sel2 = select_steps();
  steps.insert(steps.end(), sel2.begin(), sel2.end());
  append_logout(steps);
  server.start(std::move(steps));

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  auto selected = select_ok(std::move(authed).select("INBOX"));

  auto closed = bth::sync_wait(std::move(selected).close());
  ASSERT_TRUE(closed.has_value());
  auto& [close_ec, authed_again] = *closed;
  ASSERT_FALSE(close_ec) << close_ec.message();
  static_assert(std::same_as<std::remove_cvref_t<decltype(authed_again)>,
                             authenticated_state<>>);

  // Back in the authenticated state, select is legal again.
  auto selected_again = select_ok(std::move(authed_again).select("INBOX"));

  auto done = bth::sync_wait(std::move(selected_again).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
}

// usage.md §6: an unsolicited BYE is connection-fatal; the next operation
// reports errc::server_bye.
TEST(StateMachine, UnsolicitedByeFailsNextOperation) {
  fake_imap_server server;
  auto steps = login_prefix();
  steps.insert(steps.end(),
               {
                   expect_client{"SELECT"},
                   // The BYE rides along with the SELECT completion; it is
                   // processed right after it.
                   server_send{"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen "
                               "\\Draft)\r\n"
                               "* 1 EXISTS\r\n"
                               "* 0 RECENT\r\n"
                               "* OK [UIDVALIDITY 1] UIDs valid\r\n"
                               "{tag} OK [READ-WRITE] SELECT completed\r\n"
                               "* BYE server is going down\r\n"},
               });
  // No further steps: the server hangs up right after the BYE.
  server.start(std::move(steps), /*close_at_end=*/true);

  io_runner runner;
  auto authed = connect_and_login(server, runner.get());

  auto selected = select_ok(std::move(authed).select("INBOX"));

  auto found = bth::sync_wait(std::move(selected).search("ALL"));
  ASSERT_TRUE(found.has_value());
  auto& [search_ec, matches, state_back] = *found;
  EXPECT_EQ(search_ec, bkmail::errc::server_bye);
  (void)state_back;  // The session is dead; the state must not be reused.

  EXPECT_TRUE(server.wait_done(kDefaultTimeout));
}

}  // namespace
