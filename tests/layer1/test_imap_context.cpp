/**
 * @file tests/layer1/test_imap_context.cpp
 * @brief Layer-1 imap_context unit tests: tag allocation, the tag registry,
 *        three-granularity cancellation, type-erased batching, and shell
 *        lifecycle.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/common/error.h>
#include <bkmail/imap/imap_context.h>
#include <gtest/gtest.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "layer1_fixture.h"

namespace {

using bkmail::test::context_holder;
using bkmail::test::expect_write;
using bkmail::test::kDefaultTimeout;
using bkmail::test::kLayer1Greeting;
using bkmail::test::poll_until;
using bkmail::test::quiesce_guard;
using bkmail::test::script;
using bkmail::test::scripted_context;
using bkmail::test::server_bytes;
using bkmail::test::server_eof;
using bkmail::test::signal_event;

namespace im = bkmail::imap;

// Tags: "a" + zero-padded decimal counter, monotonically increasing,
// unique across submissions (usage.md §3.3; code_layout §5.1).
TEST(ImapContext, TagAllocationFormatMonotonicUnique) {
  context_holder h({server_bytes{std::string(kLayer1Greeting)}});

  std::set<std::string> tags;
  for (int i = 0; i < 12; ++i) {
    tags.insert(h.ctx.allocate_tag());
  }
  EXPECT_EQ(12U, tags.size()) << "tags must be unique";
  EXPECT_EQ("a0001", *tags.begin());
  EXPECT_TRUE(tags.contains("a0009"));
  EXPECT_TRUE(tags.contains("a0010"))
      << "the counter keeps zero-padding past 9";
  EXPECT_TRUE(tags.contains("a0012"));

  // submit() stamps from the same counter: the next tag is a0013.
  const auto tag =
      h.ctx.submit(im::noop_command<>{}, [](std::error_code) { /* dropped */ });
  EXPECT_EQ("a0013", tag);
}

// Registry: pipelined tagged replies may complete out of order; dispatch
// is purely tag-keyed (architecture §3.5).
TEST(ImapContext, OutOfOrderTaggedRepliesMatchedByTag) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0002"},
      // The server answers a0002 before a0001.
      server_bytes{"a0002 OK second\r\n"
                   "a0001 OK first\r\n"},
  });

  std::mutex mutex;
  std::vector<int> completion_order;
  signal_event first_done;
  signal_event second_done;

  const auto first_tag =
      h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
        EXPECT_FALSE(ec) << ec.message();
        {
          std::lock_guard lock(mutex);
          completion_order.push_back(1);
        }
        first_done.arrive();
      });
  const auto second_tag =
      h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
        EXPECT_FALSE(ec) << ec.message();
        {
          std::lock_guard lock(mutex);
          completion_order.push_back(2);
        }
        second_done.arrive();
      });
  EXPECT_EQ("a0001", first_tag);
  EXPECT_EQ("a0002", second_tag);
  h.ctx.flush();

  ASSERT_TRUE(first_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout));
  std::lock_guard lock(mutex);
  ASSERT_EQ(2U, completion_order.size());
  EXPECT_EQ(2, completion_order.at(0)) << "a0002 replied first";
  EXPECT_EQ(1, completion_order.at(1));
}

// Registry: a tagged reply whose tag was never submitted is dropped; the
// connection stays usable (detached-tag drop-on-arrival path).
TEST(ImapContext, UnknownTaggedReplyIsDropped) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a9999 OK stray\r\n"
                   "a0001 OK NOOP one\r\n"},
      expect_write{"a0002 NOOP"},
      server_bytes{"a0002 OK NOOP two\r\n"},
  });

  signal_event first_done;
  signal_event second_done;

  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    first_done.arrive();
  });
  h.ctx.flush();
  ASSERT_TRUE(first_done.wait_for(kDefaultTimeout))
      << "the unknown a9999 reply must not disturb dispatch";

  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    second_done.arrive();
  });
  h.ctx.flush();
  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout))
      << "the connection stays usable after a stray reply";
}

// Cancellation granularity 1 (code_layout D3): cancelling a command that
// was never flushed removes it from the queue — no bytes hit the wire and
// the handler is never invoked.
TEST(ImapContext, CancelQueuedCommandSendsNothing) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  signal_event noop_done;
  std::atomic<int> capability_calls{0};

  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { noop_done.arrive(); });
  const auto caps_tag = h.ctx.submit(
      im::capability_command<>{},
      [&](std::error_code, im::capability_set<>) { ++capability_calls; });
  h.ctx.cancel(caps_tag);  // Still queued: withdrawn before any byte is sent.
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  EXPECT_EQ(0, capability_calls.load())
      << "a cancelled queued handler is withdrawn, never invoked";
  EXPECT_TRUE(h.recorder.contains_written("a0001 NOOP"));
  EXPECT_FALSE(h.recorder.contains_written("CAPABILITY"))
      << "a cancelled queued command must never reach the wire";
}

// Cancellation granularity 2 (code_layout D3): cancelling a written
// command detaches the handler; the late tagged reply is dropped on
// arrival and the connection stays usable.
TEST(ImapContext, CancelWrittenCommandDropsLateResponse) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
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
  h.ctx.cancel(first_tag);  // Already written: the reply will be discarded.

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
  EXPECT_EQ(0, first_calls.load())
      << "the detached operation's reply must be dropped on arrival";
}

// Cancellation granularity 3 (code_layout D3): cancelling a pending IDLE
// sends DONE first so the server stays in sync. The cancel here lands at
// an intentionally uncontrolled phase (before or after the "+ idling"
// dispatch); both interleavings of a second cancel pass are pinned
// deterministically by the SecondCancelPassKeepsQueuedDoneOnWire test.
TEST(ImapContext, CancelPendingIdleSendsDoneFirst) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
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
  ASSERT_TRUE(h.recorder.wait_written("a0001 IDLE", kDefaultTimeout));

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
      << "cancelling a pending IDLE must send DONE first";
  ASSERT_TRUE(after_idle.wait_for(kDefaultTimeout));
  EXPECT_EQ(0, idle_calls.load())
      << "a cancelled handler is withdrawn, never invoked";
}

// Regression pin for the CI failure
// IdleUnsolicited.IdleCancelSendsDoneAndCompletesStopped: a second cancel
// pass arriving while the queued DONE was not yet staged (phase
// done_pending) used to extract the operation cell in detach_locked, so
// the DONE never reached the wire and the IDLE handshake stalled. The
// deciding interleaving is client-internal — no wire evidence can force
// it, which is why the integration test cannot reproduce it — so it is
// pinned here at layer 1 over the scripted stream. Both cancel passes
// fire from one request_stop() executed ON the io worker (run_on_worker):
// the first pass queues DONE and posts the pump kick BEHIND the running
// task, so the second pass observes done_pending-not-staged with no pump
// interleaving in between. The detach_locked contract: an operation is
// never extracted once any of its bytes reached the wire, so the second
// pass falls through to the idempotent cancel_written() and the DONE is
// still written.
TEST(ImapContext, SecondCancelPassKeepsQueuedDoneOnWire) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 IDLE"},
      server_bytes{"+ idling\r\n"},
      expect_write{"DONE\r\n"},
      server_bytes{"a0001 OK IDLE terminated\r\n"},
  });

  // Receiver shape of the state layer's idle operation (usage.md §2.7):
  // the stop token rides in the environment; cancellation completes with
  // set_stopped and never through the error code.
  class idle_receiver {
   public:
    using env_type = bexec::env_with_stop_token<>;

    idle_receiver(bexec::inplace_stop_token token, signal_event& stopped,
                  std::atomic<int>& value_calls)
        : env_(token), stopped_(&stopped), value_calls_(&value_calls) {}

    env_type get_env() const noexcept { return env_; }

    void set_value(std::error_code) noexcept { ++(*value_calls_); }

    void set_stopped() noexcept { stopped_->arrive(); }

   private:
    env_type env_;
    signal_event* stopped_;
    std::atomic<int>* value_calls_;
  };

  signal_event stopped;
  std::atomic<int> value_calls{0};
  bexec::inplace_stop_source stop_src;
  auto operation =
      bexec::connect(h.ctx.submit<im::idle_command<>>(),
                     idle_receiver{stop_src.get_token(), stopped, value_calls});
  bexec::start(operation);
  // Covers every early return below: the io worker must have left all
  // receiver call chains before the stack operation state is destroyed.
  quiesce_guard teardown{h.runner};

  // start() ran attach(), which registered the production stop callback
  // (callback #1 of the production pair).
  ASSERT_TRUE(h.recorder.wait_written("a0001 IDLE", kDefaultTimeout));
  // Drive barrier: the read dispatch of "+ idling" has run, so the
  // command is deterministically in the idling phase.
  h.runner.quiesce();

  // Callback #2 of the production pair: a second cancel pass on the same
  // operation (the tag is deterministic: this is the fixture's first
  // submission).
  bexec::inplace_stop_callback second_pass{stop_src.get_token(),
                                           [&] { h.ctx.cancel("a0001"); }};

  // request_stop() ON the io worker pins the interleaving by queue order:
  // pass 1 queues DONE and posts the pump kick behind the running task,
  // so pass 2 sees the cell in done_pending-not-staged.
  h.runner.run_on_worker([&] { stop_src.request_stop(); });

  ASSERT_TRUE(h.recorder.wait_written("DONE\r\n", kDefaultTimeout))
      << "the second cancel pass must not extract the cell while the "
         "queued DONE is unstaged (detach_locked contract)";
  ASSERT_TRUE(stopped.wait_for(kDefaultTimeout));
  EXPECT_EQ(0, value_calls.load());
}

// Type erasure (usage.md §3.4): make_command erases heterogeneous commands
// into one batch; submit(vector) returns the stamped tags in vector order.
TEST(ImapContext, TypeErasedBatchReturnsTagsInVectorOrder) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0003 FETCH"},
      server_bytes{"a0001 OK NOOP completed\r\n"
                   "* CAPABILITY IMAP4rev1 IDLE\r\n"
                   "a0002 OK CAPABILITY completed\r\n"
                   "* 1 FETCH (ENVELOPE (NIL \"page one\" NIL NIL NIL NIL "
                   "NIL NIL NIL NIL))\r\n"
                   "a0003 OK FETCH completed\r\n"},
  });

  signal_event noop_done;
  signal_event caps_done;
  signal_event fetch_done;
  std::optional<im::capability_set<>> caps;
  std::vector<bkmail::envelope<>> envelopes;

  // Heterogeneous command mix in one erased batch.
  std::vector<std::unique_ptr<im::imap_command<>>> batch;
  batch.push_back(im::make_command(
      im::noop_command<>{}, [&](std::error_code) { noop_done.arrive(); }));
  batch.push_back(
      im::make_command(im::capability_command<>{},
                       [&](std::error_code ec, im::capability_set<> result) {
                         if (!ec) {
                           caps = std::move(result);
                         }
                         caps_done.arrive();
                       }));
  batch.push_back(im::make_command(
      im::fetch_envelopes_command<>{"1:100"},
      [&](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
        if (!ec) {
          envelopes = std::move(envs);
        }
        fetch_done.arrive();
      }));

  const auto tags = h.ctx.submit(std::move(batch));
  ASSERT_EQ(3U, tags.size());
  EXPECT_EQ("a0001", tags.at(0));
  EXPECT_EQ("a0002", tags.at(1));
  EXPECT_EQ("a0003", tags.at(2));
  h.ctx.flush();

  ASSERT_TRUE(noop_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(caps_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(fetch_done.wait_for(kDefaultTimeout));

  ASSERT_TRUE(caps.has_value());
  EXPECT_TRUE(caps->contains("IDLE"));
  ASSERT_EQ(1U, envelopes.size());
  EXPECT_EQ("page one", envelopes.front().subject);

  // The erased batch shared one write syscall.
  EXPECT_EQ(1U, h.recorder.writes().size());
}

// Shell move semantics: only the shared ownership travels; the moved-to
// shell drives the very same session.
TEST(ImapContext, ShellMoveTransfersSessionOwnership) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  scripted_context moved{std::move(h.ctx)};
  EXPECT_TRUE(moved.is_alive());

  signal_event done;
  const auto tag = moved.submit(im::noop_command<>{}, [&](std::error_code ec) {
    EXPECT_FALSE(ec) << ec.message();
    done.arrive();
  });
  EXPECT_EQ("a0001", tag);
  moved.flush();
  ASSERT_TRUE(done.wait_for(kDefaultTimeout));
  EXPECT_TRUE(moved.is_alive());
}

// A moved-from shell is safely destructible: its destructor must not touch
// the core that now belongs to the moved-to shell.
TEST(ImapContext, MovedFromShellDestroysSafely) {
  auto h = std::make_unique<context_holder>(
      script{server_bytes{std::string(kLayer1Greeting)}});
  scripted_context moved{std::move(h->ctx)};
  // Destroys the moved-from shell while the session lives on in `moved`.
  h.reset();
  EXPECT_TRUE(moved.is_alive());
}

// is_alive() reports the read pump state; close() runs the close protocol:
// queued operations fail with operation_canceled, later submissions are
// rejected, and close() itself is idempotent (architecture §3.7).
TEST(ImapContext, CloseFailsQueuedAndRejectsSubmissions) {
  context_holder h({server_bytes{std::string(kLayer1Greeting)}});
  EXPECT_TRUE(h.ctx.is_alive());

  std::error_code queued_ec;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code ec) { queued_ec = ec; });

  h.ctx.close();
  EXPECT_EQ(std::make_error_code(std::errc::operation_canceled), queued_ec)
      << "a queued, never-written operation fails on close()";

  std::error_code late_ec;
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) { late_ec = ec; });
  EXPECT_EQ(std::make_error_code(std::errc::operation_canceled), late_ec)
      << "submissions on a closing context are rejected";

  h.ctx.close();  // Idempotent: no crash, no further effects.
}

// An unannounced server EOF stops the read pump (architecture §3.5).
TEST(ImapContext, ServerEofStopsTheReadPump) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      server_eof{},
  });
  EXPECT_TRUE(poll_until([&] { return !h.ctx.is_alive(); }, kDefaultTimeout))
      << "the read pump must stop after EOF";
}

}  // namespace
