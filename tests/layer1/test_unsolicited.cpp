/**
 * @file tests/layer1/test_unsolicited.cpp
 * @brief Layer-1 unsolicited-event unit tests: handler registration and
 *        token-based unregistration, FIFO attribution of command-scoped
 *        untagged data, broadcast of connection-scoped events, and the
 *        greeting/BYE reports.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/common/error.h>
#include <bkmail/imap/imap_context.h>
#include <bkmail/imap/unsolicited_event.h>
#include <gtest/gtest.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "layer1_fixture.h"

namespace {

using bkmail::test::context_holder;
using bkmail::test::expect_write;
using bkmail::test::kDefaultTimeout;
using bkmail::test::kLayer1Greeting;
using bkmail::test::poll_until;
using bkmail::test::script;
using bkmail::test::server_bytes;
using bkmail::test::server_eof;
using bkmail::test::signal_event;

namespace im = bkmail::imap;

/// Records every unsolicited event as a small descriptive row. Greeting
/// reports are skipped: the greeting races with handler registration by
/// design (it is the first bytes on the wire), and it has its own
/// dedicated tests below.
struct event_log {
  struct row {
    std::string kind;
    std::uint32_t number = 0;
  };

  std::mutex mutex;
  std::vector<row> rows;

  void record(const im::unsolicited_event<>& event) {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, im::greeting_event<>>) {
            return;  // See the struct comment.
          }
          std::lock_guard lock(mutex);
          if constexpr (std::is_same_v<T, im::exists_event>) {
            rows.push_back({"exists", e.count});
          } else if constexpr (std::is_same_v<T, im::recent_event>) {
            rows.push_back({"recent", e.count});
          } else if constexpr (std::is_same_v<T, im::expunge_event>) {
            rows.push_back({"expunge", e.sequence_number});
          } else if constexpr (std::is_same_v<T, im::flags_update_event>) {
            rows.push_back({"flags", e.sequence_number});
          } else if constexpr (std::is_same_v<T, im::capability_event<>>) {
            rows.push_back({"capability", 0});
          } else if constexpr (std::is_same_v<T, im::bye_event<>>) {
            rows.push_back({"bye", 0});
          }
        },
        event);
  }

  std::size_t size() {
    std::lock_guard lock(mutex);
    return rows.size();
  }
};

// Connection-scoped pushes (EXISTS / RECENT / EXPUNGE) and a CAPABILITY
// push are broadcast to the registered handler as the documented variant
// alternatives (usage.md §3.5).
TEST(Unsolicited, ConnectionScopedEventsAreBroadcast) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      // The pushes are gated behind a client write so the handler is
      // guaranteed to be registered before they are dispatched.
      expect_write{"a0001 NOOP"},
      server_bytes{"* 7 EXISTS\r\n"
                   "* 1 RECENT\r\n"
                   "* 2 EXPUNGE\r\n"
                   "* CAPABILITY IMAP4rev1 UIDPLUS\r\n"
                   "a0001 OK NOOP completed\r\n"},
  });

  event_log log;
  auto registration = h.ctx.on_unsolicited(
      [&](const im::unsolicited_event<>& event) { log.record(event); });

  signal_event noop_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(poll_until([&] { return log.size() >= 4U; }, kDefaultTimeout));

  std::lock_guard lock(log.mutex);
  ASSERT_EQ(4U, log.rows.size());
  EXPECT_EQ("exists", log.rows.at(0).kind);
  EXPECT_EQ(7U, log.rows.at(0).number);
  EXPECT_EQ("recent", log.rows.at(1).kind);
  EXPECT_EQ(1U, log.rows.at(1).number);
  EXPECT_EQ("expunge", log.rows.at(2).kind);
  EXPECT_EQ(2U, log.rows.at(2).number);
  EXPECT_EQ("capability", log.rows.at(3).kind);
}

// registration is a move-only token: destroying it unregisters the
// handler; other registrations keep receiving events.
TEST(Unsolicited, TokenDestructionUnregisters) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"* 5 EXISTS\r\n"
                   "a0001 OK NOOP completed\r\n"},
  });

  std::atomic<int> dropped_calls{0};
  event_log live_log;

  {
    // Greeting reports may legitimately arrive while this token is alive
    // (the greeting races registration by design); only push events count.
    auto dropped =
        h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
          if (!std::holds_alternative<im::greeting_event<>>(event)) {
            ++dropped_calls;
          }
        });
    // `dropped` dies here: the handler is unregistered.
  }
  auto live = h.ctx.on_unsolicited(
      [&](const im::unsolicited_event<>& event) { live_log.record(event); });

  signal_event noop_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(
      poll_until([&] { return live_log.size() >= 1U; }, kDefaultTimeout));
  EXPECT_EQ(0, dropped_calls.load())
      << "a destroyed registration token must unregister its handler";
  std::lock_guard lock(live_log.mutex);
  ASSERT_EQ(1U, live_log.rows.size());
  EXPECT_EQ("exists", live_log.rows.front().kind);
  EXPECT_EQ(5U, live_log.rows.front().number);
}

// FIFO attribution (operation_base wants_untagged contract): command-
// scoped untagged data goes to the earliest-queued waiter, so two
// concurrent FETCH commands never cross their data lines.
TEST(Unsolicited, DataUntaggedFifoAttribution) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0002 FETCH"},
      server_bytes{
          "* 1 FETCH (ENVELOPE (NIL \"page one\" NIL NIL NIL NIL NIL NIL "
          "NIL NIL))\r\n"
          "a0001 OK FETCH completed\r\n"
          "* 2 FETCH (ENVELOPE (NIL \"page two\" NIL NIL NIL NIL NIL NIL "
          "NIL NIL))\r\n"
          "a0002 OK FETCH completed\r\n"},
  });

  signal_event first_done;
  signal_event second_done;
  std::vector<bkmail::envelope<>> page_one;
  std::vector<bkmail::envelope<>> page_two;

  h.ctx.submit(im::fetch_envelopes_command<>{"1:100"},
               [&](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
                 if (!ec) {
                   page_one = std::move(envs);
                 }
                 first_done.arrive();
               });
  h.ctx.submit(im::fetch_envelopes_command<>{"101:200"},
               [&](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
                 if (!ec) {
                   page_two = std::move(envs);
                 }
                 second_done.arrive();
               });
  h.ctx.flush();

  ASSERT_TRUE(first_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout));
  ASSERT_EQ(1U, page_one.size());
  EXPECT_EQ("page one", page_one.front().subject);
  ASSERT_EQ(1U, page_two.size());
  EXPECT_EQ("page two", page_two.front().subject)
      << "the second FETCH must not see the first one's data";
}

// The greeting surfaces through the unsolicited path as a greeting_event
// (code_layout D5); the expect_write gate guarantees the handler is
// registered before the greeting bytes are dispatched.
TEST(Unsolicited, OkGreetingProducesGreetingEvent) {
  context_holder h({
      expect_write{"a0001 NOOP"},
      server_bytes{"* OK [CAPABILITY IMAP4rev1] fake ready\r\n"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  std::mutex mutex;
  std::optional<im::greeting_event<>> greeting;
  auto registration =
      h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        if (const auto* g = std::get_if<im::greeting_event<>>(&event)) {
          std::lock_guard lock(mutex);
          greeting = *g;
        }
      });

  signal_event noop_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(poll_until(
      [&] {
        std::lock_guard lock(mutex);
        return greeting.has_value();
      },
      kDefaultTimeout));

  std::lock_guard lock(mutex);
  EXPECT_EQ(im::response_status::ok, greeting->status);
  EXPECT_NE(std::string::npos, greeting->text.find("fake ready"));
  ASSERT_TRUE(greeting->code.has_value());
  EXPECT_NE(nullptr, std::get_if<im::capability_code<>>(&*greeting->code));
}

// A PREAUTH greeting reports the Authenticated entry state through the
// same greeting_event alternative.
TEST(Unsolicited, PreauthGreetingProducesGreetingEvent) {
  context_holder h({
      expect_write{"a0001 NOOP"},
      server_bytes{"* PREAUTH authenticated ready\r\n"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  std::mutex mutex;
  std::optional<im::greeting_event<>> greeting;
  auto registration =
      h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        if (const auto* g = std::get_if<im::greeting_event<>>(&event)) {
          std::lock_guard lock(mutex);
          greeting = *g;
        }
      });

  signal_event noop_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(poll_until(
      [&] {
        std::lock_guard lock(mutex);
        return greeting.has_value();
      },
      kDefaultTimeout));

  std::lock_guard lock(mutex);
  EXPECT_EQ(im::response_status::preauth, greeting->status);
}

// A BYE greeting surfaces as bye_event (connection-fatal); the following
// EOF stops the read pump and fails in-flight operations with
// errc::server_bye (read pump BYE bookkeeping).
TEST(Unsolicited, ByeGreetingProducesByeEventAndStopsPump) {
  context_holder h({
      expect_write{"a0001 NOOP"},
      server_bytes{"* BYE [ALERT] server is going away\r\n"},
      server_eof{},
  });

  std::mutex mutex;
  std::optional<im::bye_event<>> bye;
  auto registration =
      h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        if (const auto* b = std::get_if<im::bye_event<>>(&event)) {
          std::lock_guard lock(mutex);
          bye = *b;
        }
      });

  signal_event noop_done;
  std::error_code noop_ec;
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    noop_ec = ec;
    noop_done.arrive();
  });
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  EXPECT_EQ(bkmail::make_error_code(bkmail::errc::server_bye), noop_ec)
      << "BYE + EOF must fail the in-flight operation with server_bye";

  ASSERT_TRUE(poll_until(
      [&] {
        std::lock_guard lock(mutex);
        return bye.has_value();
      },
      kDefaultTimeout));
  {
    std::lock_guard lock(mutex);
    EXPECT_NE(std::string::npos, bye->text.find("going away"));
    ASSERT_TRUE(bye->code.has_value());
    EXPECT_NE(nullptr, std::get_if<im::alert_code>(&*bye->code));
  }
  EXPECT_TRUE(poll_until([&] { return !h.ctx.is_alive(); }, kDefaultTimeout))
      << "the read pump must stop after BYE + EOF";
}

}  // namespace
