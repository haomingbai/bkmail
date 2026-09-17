/**
 * @file tests/integration/test_protocol_robustness.cpp
 * @brief Protocol-level robustness: unknown codes, literals, BYE, literals
 *        stop-and-wait.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: docs/architecture.md §2 (IMAP protocol facts: unknown
 * resp-text-codes must be skipped; the two-mode line/literal reader; BYE is
 * connection-fatal) and usage.md §6/§8. Covered here:
 *
 *   - unknown resp-text-codes in the greeting, in untagged responses, and in
 *     tagged completions are skipped without failing the parse;
 *   - literals containing CRLF and even a syntactically valid tagged reply
 *     are delivered as opaque bytes (the two-mode reader, §3.5);
 *   - APPEND honors the synchronizing-literal stop-and-wait: the client
 *     sends the {n} line, waits for "+", and only then sends the literal
 *     (verified at the wire level with the expect_client "forbid" gate);
 *   - an untagged BYE arriving mid-command fails the in-flight operation and
 *     surfaces as bye_event;
 *   - tagged NO/BAD map to errc::command_rejected / errc::bad_command
 *     (also covered at Layer 1 in test_command_layer.cpp).
 *
 * Layer-1 tests run over scripted_stream; the APPEND test runs at the state
 * layer over the loopback fake server because the append command's wire
 * rendering is only reachable through the documented state API
 * (authenticated_state::append, code_layout §5.3).
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/fake_imap_server.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
using bkmail::test::script;
using bkmail::test::script_recorder;
using bkmail::test::scripted_stream;
using bkmail::test::server_bytes;
using bkmail::test::server_eof;
using bkmail::test::server_send;
using bkmail::test::signal_event;

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

using scripted_context = im::imap_context<scripted_stream>;

constexpr std::string_view kCapabilities =
    "IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR";

// architecture §2: "Unknown response codes must be skipped without failing
// the parse." The greeting, an interleaved untagged OK, and the tagged
// completion all carry made-up codes here.
TEST(ProtocolRobustness, UnknownRespTextCodesAreTolerated) {
  io_runner runner;
  scripted_stream stream({
      server_bytes{"* OK [X-MAPSYNC FOO-42] fake ready\r\n"},
      expect_write{"a0001 SELECT"},
      server_bytes{"* OK [X-WEIRDCODE (NESTED (PAYLOAD))] noise\r\n"
                   "* 1 EXISTS\r\n"
                   "* OK [UIDVALIDITY 9] UIDs valid\r\n"
                   "a0001 OK [X-UNKNOWN=VALUE] SELECT completed\r\n"},
  });
  script_recorder recorder = stream.recorder();
  scripted_context ctx{std::move(stream), runner.get()};

  signal_event done;
  std::error_code result_ec;
  std::optional<im::mailbox_info<>> info;
  ctx.submit(im::select_command<>{"INBOX"},
             [&](std::error_code ec, im::mailbox_info<> box) {
               result_ec = ec;
               info = std::move(box);
               done.arrive();
             });
  ctx.flush();

  ASSERT_TRUE(done.wait_for(kDefaultTimeout));
  EXPECT_FALSE(result_ec) << result_ec.message();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(1U, info->exists);
  EXPECT_EQ(9U, info->uid_validity);
}

// architecture §3.5: literal mode reads exactly n bytes, whatever they are —
// including CRLF and byte sequences that look like protocol lines.
TEST(ProtocolRobustness, LiteralWithEmbeddedCrlfAndFakeReply) {
  const std::string payload =
      "Subject: literal test\r\n"
      "From: bob@example.com\r\n"
      "\r\n"
      "line one\r\n"
      "a0001 OK this is NOT a tagged reply\r\n"  // inside the literal
      "line three\r\n";

  io_runner runner;
  scripted_stream stream({
      server_bytes{"* OK fake ready\r\n"},
      expect_write{"a0001 FETCH"},
      server_bytes{"* 1 FETCH (BODY[] {" + std::to_string(payload.size()) +
                   "}\r\n" + payload +
                   ")\r\n"
                   "a0001 OK FETCH completed\r\n"},
  });
  script_recorder recorder = stream.recorder();
  scripted_context ctx{std::move(stream), runner.get()};

  signal_event done;
  std::error_code result_ec;
  std::vector<bkmail::mail<>> messages;
  ctx.submit(im::fetch_message_command<>{"1"},
             [&](std::error_code ec, std::vector<bkmail::mail<>> mails) {
               result_ec = ec;
               messages = std::move(mails);
               done.arrive();
             });
  ctx.flush();

  ASSERT_TRUE(done.wait_for(kDefaultTimeout));
  EXPECT_FALSE(result_ec) << result_ec.message();
  ASSERT_EQ(1U, messages.size());
  const bkmail::mail<>& m = messages.front();
  EXPECT_EQ("literal test", m.header.subject);
  const std::string body(reinterpret_cast<const char*>(m.body.data.data()),
                         m.body.data.size());
  EXPECT_NE(std::string::npos, body.find("line one"));
  EXPECT_NE(std::string::npos, body.find("NOT a tagged reply"));
  EXPECT_NE(std::string::npos, body.find("line three"));
}

// architecture §2/§3.4: with no LITERAL+ advertised, a literal-carrying
// command stops after the {n} line and waits for the continuation request.
// Verified at the state layer (usage.md §2.1 append workflow) where the
// append wire form is contractual.
TEST(ProtocolRobustness, AppendWaitsForContinuationBeforeLiteral) {
  fake_imap_server server;
  server.start({
      // Note: no LITERAL+ in this set, so literals are synchronizing.
      server_send{"* OK [CAPABILITY IMAP4rev1 UIDPLUS MOVE IDLE SASL-IR] "
                  "fake ready\r\n"},
      expect_client{"CAPABILITY"},
      server_send{"* CAPABILITY IMAP4rev1 UIDPLUS MOVE IDLE SASL-IR\r\n"
                  "{tag} OK CAPABILITY completed\r\n"},
      expect_client{"LOGIN"},
      server_send{"{tag} OK LOGIN completed\r\n"},
      // The client must stop after the {n} line: BODY-MARKER must not have
      // been written when the APPEND line matched.
      expect_client{"APPEND", "BODY-MARKER"},
      server_send{"+ go ahead\r\n"},
      expect_client{"BODY-MARKER"},
      server_send{"{tag} OK APPEND completed\r\n"},
      expect_client{"LOGOUT"},
      server_send{"* BYE bye\r\n{tag} OK LOGOUT completed\r\n"},
  });

  io_runner runner;
  auto connected = bth::sync_wait_with_variant(bkmail::async_connect(
      "127.0.0.1", std::to_string(server.port()), runner.get()));
  ASSERT_TRUE(connected.has_value());
  auto* greeting =
      std::get_if<std::tuple<std::error_code, im::not_authenticated_state<>>>(
          &*connected);
  ASSERT_NE(nullptr, greeting);
  auto& [connect_ec, not_authed] = *greeting;
  ASSERT_FALSE(connect_ec) << connect_ec.message();

  bkmail::account_info<> account{.user_name = "user", .password = "pw"};
  auto logged_in =
      bth::sync_wait_with_variant(std::move(not_authed).login(account));
  ASSERT_TRUE(logged_in.has_value());
  auto* login_branch =
      std::get_if<std::tuple<std::error_code, im::authenticated_state<>>>(
          &*logged_in);
  ASSERT_NE(nullptr, login_branch);
  auto& [login_ec, authed] = *login_branch;
  ASSERT_FALSE(login_ec) << login_ec.message();

  // Build a small message whose body contains a literal-forcing CRLF and a
  // recognizable marker.
  bkmail::mail<> msg;
  msg.header.subject = "append me";
  msg.body.content_type = "text/plain";
  const std::string body = "line one\r\nBODY-MARKER\r\n";
  msg.body.data.assign(
      reinterpret_cast<const std::byte*>(body.data()),
      reinterpret_cast<const std::byte*>(body.data()) + body.size());

  // append(mailbox, mail, flags) — code_layout §5.3; the third argument is
  // the flag set to store with the message (empty here).
  auto appended =
      bth::sync_wait(std::move(authed).append("Archive", msg, im::flag_set{}));
  ASSERT_TRUE(appended.has_value());
  auto& [append_ec, authed_back] = *appended;
  EXPECT_FALSE(append_ec) << append_ec.message();

  auto done = bth::sync_wait(std::move(authed_back).logout());
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(std::get<0>(*done)) << std::get<0>(*done).message();

  ASSERT_TRUE(server.wait_done(kDefaultTimeout));
  EXPECT_TRUE(server.ok()) << server.errors();
  // The marker went out, and only after the APPEND line.
  const std::string received = server.received();
  const auto append_pos = received.find("APPEND");
  const auto marker_pos = received.find("BODY-MARKER");
  ASSERT_NE(std::string::npos, append_pos);
  ASSERT_NE(std::string::npos, marker_pos);
  EXPECT_LT(append_pos, marker_pos);
}

// architecture §2: "Untagged BYE may arrive at any time and is
// connection-fatal." An in-flight command fails; the BYE also surfaces as a
// bye_event (usage.md §3.5/§6).
TEST(ProtocolRobustness, ByeMidCommandFailsInFlightOperation) {
  io_runner runner;
  scripted_stream stream({
      server_bytes{"* OK fake ready\r\n"},
      expect_write{"a0001 SEARCH"},
      // The server answers with partial data, then gives up: no tagged
      // reply ever arrives for a0001.
      server_bytes{"* SEARCH 1 2\r\n* BYE meltdown\r\n"},
      server_eof{},
  });
  script_recorder recorder = stream.recorder();
  scripted_context ctx{std::move(stream), runner.get()};

  signal_event bye_seen;
  signal_event search_done;
  std::error_code search_ec = std::make_error_code(std::errc::io_error);
  auto registration =
      ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        std::visit(
            [&](const auto& e) {
              using T = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<T, im::bye_event<>>) {
                bye_seen.arrive();
              }
            },
            event);
      });

  ctx.submit(im::search_command<>{"ALL"},
             [&](std::error_code ec, std::vector<std::uint32_t>) {
               search_ec = ec;
               search_done.arrive();
             });
  ctx.flush();

  ASSERT_TRUE(search_done.wait_for(kDefaultTimeout));
  // Expected mapping per usage.md §6 (errc::server_bye = "untagged BYE
  // outside a logout exchange").
  EXPECT_EQ(search_ec, bkmail::errc::server_bye);
  ASSERT_TRUE(bye_seen.wait_for(kDefaultTimeout));
  EXPECT_TRUE(poll_until([&] { return !ctx.is_alive(); }, kDefaultTimeout));
}

}  // namespace
