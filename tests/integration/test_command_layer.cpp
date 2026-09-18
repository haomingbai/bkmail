/**
 * @file tests/integration/test_command_layer.cpp
 * @brief Layer-1 imap_context contract tests over a scripted stream.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Behavior baseline: usage.md §3.3 (submit/tags/handlers), §3.4 (type-erased
 * batch submission), §3.5 (unsolicited events) and §3.6 (when I/O actually
 * happens). Every test drives a real `imap_context` over
 * `bkmail::test::scripted_stream`, so the exact bytes on the wire are
 * assertable and no networking is involved.
 *
 *   - submit performs no I/O until flush; one flush batches N commands into
 *     a single write;
 *   - tags are allocated in submission order (a0001, a0002, ...);
 *   - tagged replies are matched by tag and may complete out of order;
 *   - make_command erasure + vector submission shares the tag space and the
 *     write syscall;
 *   - on_unsolicited receives EXISTS/EXPUNGE/FETCH(FLAGS)/CAPABILITY pushes;
 *   - cancel of a queued command sends nothing; cancel of a written command
 *     drops its response; cancelling a pending IDLE sends DONE first;
 *   - tagged NO/BAD arrive through the handler error code as
 *     errc::command_rejected / errc::bad_command;
 *   - a BYE greeting surfaces as bye_event and stops the read pump.
 */

#include <bkmail/bkmail.h>
#include <gtest/gtest.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bkmail::test::expect_write;
using bkmail::test::io_runner;
using bkmail::test::kDefaultTimeout;
using bkmail::test::poll_until;
using bkmail::test::script;
using bkmail::test::script_recorder;
using bkmail::test::scripted_stream;
using bkmail::test::server_bytes;
using bkmail::test::server_eof;
using bkmail::test::signal_event;

namespace im = bkmail::imap;

using scripted_context = im::imap_context<scripted_stream>;

/// The greeting every Layer-1 script starts with; the context consumes it
/// through the unsolicited path (code_layout D5).
constexpr std::string_view kGreeting =
    "* OK [CAPABILITY IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR] fake "
    "ready\r\n";

/// Builds a context over the given script; the recorder must be captured
/// before the stream is moved. Usage:
///   auto h = make_context(script{...});
///   ... use h.ctx, assert via h.recorder ...
struct context_holder {
  io_runner runner;
  scripted_stream stream;
  script_recorder recorder;
  scripted_context ctx;

  explicit context_holder(script steps)
      : stream(std::move(steps)),
        recorder(stream.recorder()),
        ctx{std::move(stream), runner.get()} {}
};

// usage.md §3.3/§3.6: submit queues bytes without I/O; flush writes once.
TEST(CommandLayer, SubmitDoesNoIoUntilFlushThenBatches) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0002 NOOP"},  // server answers only after both commands
      server_bytes{"a0001 OK LOGIN completed\r\na0002 OK NOOP completed\r\n"},
  });

  signal_event login_done;
  signal_event noop_done;
  std::error_code login_ec;
  std::error_code noop_ec;

  bkmail::account_info<> account{.user_name = "user", .password = "pw"};
  const auto login_tag =
      h.ctx.submit(im::login_command<>{account}, [&](std::error_code ec) {
        login_ec = ec;
        login_done.arrive();
      });
  const auto noop_tag =
      h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
        noop_ec = ec;
        noop_done.arrive();
      });

  // usage.md §3.3: tags are stamped at submit time, in order.
  EXPECT_EQ("a0001", login_tag);
  EXPECT_EQ("a0002", noop_tag);

  // usage.md §3.6: "nothing you submit is sent until you flush".
  EXPECT_TRUE(h.recorder.writes().empty());
  EXPECT_TRUE(h.recorder.written().empty());

  h.ctx.flush();

  ASSERT_TRUE(login_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  EXPECT_FALSE(login_ec) << login_ec.message();
  EXPECT_FALSE(noop_ec) << noop_ec.message();

  // The two commands went out as ONE batched write (usage.md §3.6).
  const auto writes = h.recorder.writes();
  ASSERT_EQ(1U, writes.size());
  EXPECT_NE(std::string::npos, writes.front().find("a0001 LOGIN"));
  EXPECT_NE(std::string::npos, writes.front().find("a0002 NOOP"));
}

// usage.md §3.5: tagged replies may complete out of order when pipelined;
// dispatch is purely tag-keyed.
TEST(CommandLayer, OutOfOrderTaggedRepliesAreMatchedByTag) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0002"},
      // The server answers a0002 before a0001.
      server_bytes{"a0002 OK NOOP completed\r\n"
                   "* SEARCH 4 9\r\n"
                   "a0001 OK SEARCH completed\r\n"},
  });

  signal_event search_done;
  signal_event noop_done;
  std::error_code search_ec;
  std::vector<std::uint32_t> matches;

  const auto search_tag =
      h.ctx.submit(im::search_command<>{"UNSEEN"},
                   [&](std::error_code ec, std::vector<std::uint32_t> found) {
                     search_ec = ec;
                     matches = std::move(found);
                     search_done.arrive();
                   });
  const auto noop_tag = h.ctx.submit(
      im::noop_command<>{}, [&](std::error_code) { noop_done.arrive(); });
  EXPECT_EQ("a0001", search_tag);
  EXPECT_EQ("a0002", noop_tag);
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(search_done.wait_for(kDefaultTimeout));
  EXPECT_FALSE(search_ec) << search_ec.message();
  EXPECT_EQ((std::vector<std::uint32_t>{4U, 9U}), matches);
}

// usage.md §3.3: result-bearing handlers (capability_set) get their value;
// a second flush forms a second write.
TEST(CommandLayer, ResultBearingCommandAndSecondFlush) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
      expect_write{"a0002"},
      server_bytes{"* CAPABILITY IMAP4rev1 IDLE LITERAL+\r\n"
                   "a0002 OK CAPABILITY completed\r\n"},
  });

  signal_event noop_done;
  signal_event caps_done;
  std::optional<im::capability_set<>> caps;

  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));

  h.ctx.submit(im::capability_command<>{},
               [&](std::error_code ec, im::capability_set<> result) {
                 if (!ec) {
                   caps = std::move(result);
                 }
                 caps_done.arrive();
               });
  h.ctx.flush();
  ASSERT_TRUE(caps_done.wait_for(kDefaultTimeout));

  ASSERT_TRUE(caps.has_value());
  EXPECT_TRUE(caps->contains("IDLE"));  // case-insensitive (usage §4)
  EXPECT_TRUE(caps->contains("literal+"));
  EXPECT_FALSE(caps->contains("UIDPLUS"));

  // Two flushes → two writes.
  EXPECT_EQ(2U, h.recorder.writes().size());
}

// usage.md §3.4: type-erased batch submission through make_command.
TEST(CommandLayer, TypeErasedBatchSubmission) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0003 NOOP"},
      server_bytes{"* 1 FETCH (ENVELOPE (NIL \"page one\" NIL NIL NIL NIL "
                   "NIL NIL NIL NIL))\r\n"
                   "a0001 OK FETCH completed\r\n"
                   "* 1 FETCH (ENVELOPE (NIL \"page two\" NIL NIL NIL NIL "
                   "NIL NIL NIL NIL))\r\n"
                   "a0002 OK FETCH completed\r\n"
                   "a0003 OK NOOP completed\r\n"},
  });

  signal_event first_page;
  signal_event second_page;
  signal_event noop_done;
  std::vector<bkmail::envelope<>> page_one;
  std::vector<bkmail::envelope<>> page_two;

  std::vector<std::unique_ptr<im::imap_command<>>> batch;
  batch.push_back(im::make_command(
      im::fetch_envelopes_command<>{"1:100"},
      [&](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
        if (!ec) {
          page_one = std::move(envs);
        }
        first_page.arrive();
      }));
  batch.push_back(im::make_command(
      im::fetch_envelopes_command<>{"101:200"},
      [&](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
        if (!ec) {
          page_two = std::move(envs);
        }
        second_page.arrive();
      }));
  batch.push_back(im::make_command(
      im::noop_command<>{}, [&](std::error_code) { noop_done.arrive(); }));

  // usage.md §3.4: "tags assigned in vector order".
  const auto tags = h.ctx.submit(std::move(batch));
  ASSERT_EQ(3U, tags.size());
  EXPECT_EQ("a0001", tags.at(0));
  EXPECT_EQ("a0002", tags.at(1));
  EXPECT_EQ("a0003", tags.at(2));
  h.ctx.flush();

  ASSERT_TRUE(first_page.wait_for(kDefaultTimeout));
  ASSERT_TRUE(second_page.wait_for(kDefaultTimeout));
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));

  ASSERT_EQ(1U, page_one.size());
  EXPECT_EQ("page one", page_one.front().subject);
  ASSERT_EQ(1U, page_two.size());
  EXPECT_EQ("page two", page_two.front().subject);

  // The whole batch shared one write syscall.
  EXPECT_EQ(1U, h.recorder.writes().size());
}

// usage.md §3.5: unsolicited pushes reach the registered handler as the
// documented variant alternatives.
TEST(CommandLayer, UnsolicitedEventsAreDispatched) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      // Gated: the pushes are withheld until the client wrote its first
      // command, so the on_unsolicited registration below can never lose
      // the race against the read pump's dispatch.
      expect_write{"a0001 NOOP"},
      server_bytes{"* 7 EXISTS\r\n"
                   "* 2 EXPUNGE\r\n"
                   "* 4 FETCH (FLAGS (\\Seen \\Deleted))\r\n"
                   "* CAPABILITY IMAP4rev1 UIDPLUS\r\n"
                   "a0001 OK NOOP completed\r\n"},
  });

  struct seen_event {
    std::string kind;
    std::uint32_t number = 0;
  };
  std::mutex mutex;
  std::vector<seen_event> events;
  signal_event arrived;

  auto registration =
      h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        std::visit(
            [&](const auto& e) {
              using T = std::decay_t<decltype(e)>;
              std::lock_guard lock(mutex);
              if constexpr (std::is_same_v<T, im::exists_event>) {
                events.push_back({"exists", e.count});
              } else if constexpr (std::is_same_v<T, im::expunge_event>) {
                events.push_back({"expunge", e.sequence_number});
              } else if constexpr (std::is_same_v<T, im::flags_update_event>) {
                events.push_back({"flags", e.sequence_number});
              } else if constexpr (std::is_same_v<T, im::capability_event<>>) {
                events.push_back({"capability", 0});
              }
              // recent/bye and the greeting report are ignored here.
              arrived.arrive();
            },
            event);
      });

  // The gate-opening NOOP's tagged reply is dispatched after the four
  // pushes (in-chunk order), so its completion is a deterministic
  // handshake proving the handler observed every push — no polling.
  signal_event noop_done;
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    noop_done.arrive();
  });
  h.ctx.flush();
  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));

  std::lock_guard lock(mutex);
  ASSERT_EQ(4U, events.size());
  EXPECT_EQ("exists", events.at(0).kind);
  EXPECT_EQ(7U, events.at(0).number);
  EXPECT_EQ("expunge", events.at(1).kind);
  EXPECT_EQ(2U, events.at(1).number);
  EXPECT_EQ("flags", events.at(2).kind);
  EXPECT_EQ(4U, events.at(2).number);
  EXPECT_EQ("capability", events.at(3).kind);
}

// usage.md §3.3 + code_layout D3: cancelling a command that was never
// flushed removes it from the queue — no bytes hit the wire, the handler
// is never invoked.
TEST(CommandLayer, CancelQueuedCommandSendsNothing) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  signal_event noop_done;
  std::atomic<int> capability_calls{0};

  const auto noop_tag = h.ctx.submit(
      im::noop_command<>{}, [&](std::error_code) { noop_done.arrive(); });
  const auto caps_tag = h.ctx.submit(
      im::capability_command<>{},
      [&](std::error_code, im::capability_set<>) { ++capability_calls; });
  h.ctx.cancel(caps_tag);  // still queued: withdrawn before any byte is sent
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  EXPECT_EQ(0, capability_calls.load());
  EXPECT_TRUE(h.recorder.contains_written("a0001 NOOP"));
  EXPECT_FALSE(h.recorder.contains_written("CAPABILITY"))
      << "a cancelled queued command must never reach the wire";

  // The tag space is not reused after cancellation.
  EXPECT_EQ("a0001", noop_tag);
}

// usage.md §3.3 + code_layout D3: cancelling a written command detaches the
// handler; the late tagged reply is dropped and the connection stays usable.
TEST(CommandLayer, CancelWrittenCommandDropsLateResponse) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001 NOOP"},
      // The first reply is held back until a second command is written, so
      // the cancel below deterministically lands before the response.
      expect_write{"a0002 NOOP"},
      server_bytes{"a0001 OK NOOP one\r\n"
                   "a0002 OK NOOP two\r\n"},
  });

  std::atomic<int> first_calls{0};
  signal_event second_done;

  const auto first_tag = h.ctx.submit(im::noop_command<>{},
                                      [&](std::error_code) { ++first_calls; });
  h.ctx.flush();
  ASSERT_TRUE(h.recorder.wait_written("a0001 NOOP", kDefaultTimeout));
  h.ctx.cancel(first_tag);  // already written: response will be discarded

  // The second command proves the connection is still usable.
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    second_done.arrive();
  });
  h.ctx.flush();

  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout));
  // No sleep needed: both replies live in one scripted chunk and are
  // dispatched in chunk order, so second_done proves the late a0001
  // reply was already processed (and dropped).
  EXPECT_EQ(0, first_calls.load());
}

// usage.md §3.3 + code_layout D3: cancelling a pending IDLE sends DONE
// first so the server stays in sync.
TEST(CommandLayer, CancelPendingIdleSendsDone) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001 IDLE"},
      server_bytes{"+ idling\r\n"},
      expect_write{"DONE\r\n"},
      server_bytes{"a0001 OK IDLE terminated\r\n"
                   "a0002 OK NOOP completed\r\n"},
  });

  std::atomic<int> idle_calls{0};
  const auto idle_tag = h.ctx.submit(im::idle_command<>{},
                                     [&](std::error_code) { ++idle_calls; });
  h.ctx.flush();
  ASSERT_TRUE(h.recorder.wait_written("IDLE", kDefaultTimeout));

  // Registered before cancel(): its tagged reply is dispatched after the
  // a0001 line (in-chunk order), so its completion is the deterministic
  // handshake proving the withdrawn IDLE handler was never invoked.
  signal_event after_idle;
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    after_idle.arrive();
  });

  h.ctx.cancel(idle_tag);

  ASSERT_TRUE(h.recorder.wait_written("DONE\r\n", kDefaultTimeout))
      << "cancelling a pending IDLE must send DONE";
  ASSERT_TRUE(after_idle.wait_for(kDefaultTimeout));
  EXPECT_EQ(0, idle_calls.load())
      << "a cancelled handler is withdrawn, never invoked";
}

// usage.md §3.3: tagged NO/BAD arrive through the handler's error code.
// The raw_command escape hatch is the documented exception (code_layout
// reconciliation): its NO/BAD are NOT mapped, the status rides in
// raw_response::ok instead so extension outcomes stay interpretable.
TEST(CommandLayer, NoAndBadMapToErrc) {
  context_holder h({
      server_bytes{std::string(kGreeting)},
      expect_write{"a0001"},
      server_bytes{"a0001 NO rejected\r\n"},
      expect_write{"a0002"},
      server_bytes{"a0002 BAD broken\r\n"},
      expect_write{"a0003"},
      server_bytes{"a0003 NO extension refused\r\n"},
  });

  signal_event no_done;
  signal_event bad_done;
  signal_event raw_done;
  std::error_code no_ec;
  std::error_code bad_ec;
  std::error_code raw_ec;
  bool raw_ok = true;

  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    no_ec = ec;
    no_done.arrive();
  });
  h.ctx.flush();
  ASSERT_TRUE(no_done.wait_for(kDefaultTimeout));
  EXPECT_EQ(no_ec, bkmail::errc::command_rejected);

  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    bad_ec = ec;
    bad_done.arrive();
  });
  h.ctx.flush();
  ASSERT_TRUE(bad_done.wait_for(kDefaultTimeout));
  EXPECT_EQ(bad_ec, bkmail::errc::bad_command);

  // raw_command: NO/BAD stay off the error channel (reconciled contract).
  h.ctx.submit(im::raw_command<>{"XYZZY PLUG"},
               [&](std::error_code ec, im::raw_response<> r) {
                 raw_ec = ec;
                 raw_ok = r.ok;
                 raw_done.arrive();
               });
  h.ctx.flush();
  ASSERT_TRUE(raw_done.wait_for(kDefaultTimeout));
  EXPECT_FALSE(raw_ec) << raw_ec.message();
  EXPECT_FALSE(raw_ok) << "a tagged NO must surface as raw_response::ok";
}

// usage.md §3.5: a BYE greeting surfaces as bye_event; the read pump stops
// (is_alive() flips) once the server hangs up.
TEST(CommandLayer, ByeGreetingSurfacesAsByeEvent) {
  context_holder h({
      // Gated: the BYE greeting is withheld until the client wrote its
      // first command, so the registration below can never lose the race
      // against the read pump's dispatch.
      expect_write{"a0001 NOOP"},
      server_bytes{"* BYE server is going away\r\n"},
      server_eof{},
  });

  signal_event bye_seen;
  std::string bye_text;
  auto registration =
      h.ctx.on_unsolicited([&](const im::unsolicited_event<>& event) {
        std::visit(
            [&](const auto& e) {
              using T = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<T, im::bye_event<>>) {
                bye_text = e.text;
                bye_seen.arrive();
              }
            },
            event);
      });

  // Opening the gate: the write happens strictly after the registration.
  h.ctx.submit(im::noop_command<>{}, [](std::error_code) {});
  h.ctx.flush();

  ASSERT_TRUE(bye_seen.wait_for(kDefaultTimeout));
  EXPECT_NE(std::string::npos, bye_text.find("going away"));
  EXPECT_TRUE(poll_until([&] { return !h.ctx.is_alive(); }, kDefaultTimeout))
      << "the read pump must stop after BYE + EOF";
}

}  // namespace
