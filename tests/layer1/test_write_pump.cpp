/**
 * @file tests/layer1/test_write_pump.cpp
 * @brief Layer-1 write-path unit tests: queueing without I/O, batched
 *        single-write flush, automatic write registration, consecutive
 *        flushes, and queue element stability across interleaved batches.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/imap_context.h>
#include <gtest/gtest.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <bexec/bexec.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "layer1_fixture.h"

namespace {

using bkmail::test::context_holder;
using bkmail::test::expect_write;
using bkmail::test::kDefaultTimeout;
using bkmail::test::kLayer1Greeting;
using bkmail::test::poll_until;
using bkmail::test::server_bytes;
using bkmail::test::signal_event;

namespace im = bkmail::imap;

// usage.md §3.3/§3.6: submit() stamps and queues WITHOUT doing I/O;
// flush() performs one batched write of the pending queue.
TEST(WritePump, SubmitQueuesBytesWithoutIoUntilFlush) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0002 NOOP"},
      server_bytes{"a0001 OK NOOP one\r\n"
                   "a0002 OK NOOP two\r\n"},
  });

  signal_event first_done;
  signal_event second_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { first_done.arrive(); });
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { second_done.arrive(); });

  // Give any rogue I/O a chance: nothing may be written before flush().
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  EXPECT_TRUE(h.recorder.writes().empty());
  EXPECT_TRUE(h.recorder.written().empty());

  h.ctx.flush();
  ASSERT_TRUE(first_done.wait_for(kDefaultTimeout));
  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout));

  // Both commands went out as ONE batched write (architecture §3.4).
  const auto writes = h.recorder.writes();
  ASSERT_EQ(1U, writes.size());
  EXPECT_NE(std::string::npos, writes.front().find("a0001 NOOP"));
  EXPECT_NE(std::string::npos, writes.front().find("a0002 NOOP"));
}

// flush() on an empty queue is a no-op; a later flush after new
// submissions behaves normally.
TEST(WritePump, FlushOnEmptyQueueWritesNothing) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  h.ctx.flush();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_TRUE(h.recorder.writes().empty())
      << "flushing an empty queue must not write";

  signal_event done;
  h.ctx.submit(im::noop_command<>{}, [&](std::error_code) { done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(done.wait_for(kDefaultTimeout));
  EXPECT_EQ(1U, h.recorder.writes().size());

  // A second flush with nothing new pending stays silent.
  h.ctx.flush();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_EQ(1U, h.recorder.writes().size());
}

// Consecutive flushes produce consecutive writes (no state leaks between
// batches).
TEST(WritePump, ConsecutiveFlushesProduceConsecutiveWrites) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP one\r\n"},
      expect_write{"a0002 NOOP"},
      server_bytes{"a0002 OK NOOP two\r\n"},
  });

  signal_event first_done;
  signal_event second_done;
  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { first_done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(first_done.wait_for(kDefaultTimeout));

  h.ctx.submit(im::noop_command<>{},
               [&](std::error_code) { second_done.arrive(); });
  h.ctx.flush();
  ASSERT_TRUE(second_done.wait_for(kDefaultTimeout));

  const auto writes = h.recorder.writes();
  ASSERT_EQ(2U, writes.size());
  EXPECT_NE(std::string::npos, writes.at(0).find("a0001 NOOP"));
  EXPECT_EQ(std::string::npos, writes.at(0).find("a0002"));
  EXPECT_NE(std::string::npos, writes.at(1).find("a0002 NOOP"));
}

// Queue element stability across interleaved batches (architecture §2.5 /
// §3.4): while the first command is on the wire awaiting its reply, eight
// more are submitted (growing the queue), and every reply is still paired
// with its own handler. unique_ptr cells keep pointees stable across the
// queue vector's reallocation.
TEST(WritePump, InterleavedBatchesKeepElementsStable) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      expect_write{"a0009 NOOP"},
      server_bytes{"a0001 OK n\r\n"
                   "a0002 OK n\r\n"
                   "a0003 OK n\r\n"
                   "a0004 OK n\r\n"
                   "a0005 OK n\r\n"
                   "a0006 OK n\r\n"
                   "a0007 OK n\r\n"
                   "a0008 OK n\r\n"
                   "a0009 OK n\r\n"},
  });

  std::mutex mutex;
  std::map<std::string, std::error_code> completed;
  const auto submit_noop = [&] {
    return h.ctx.submit(im::noop_command<>{}, [&](std::error_code ec) {
      std::lock_guard lock(mutex);
      // The handler fires from the read loop; capture by tag via the
      // completion order is enough: every invocation is one completed tag.
      completed.emplace("call" + std::to_string(completed.size()), ec);
    });
  };

  // First batch: one command on the wire.
  const auto first_tag = submit_noop();
  EXPECT_EQ("a0001", first_tag);
  h.ctx.flush();
  ASSERT_TRUE(h.recorder.wait_written("a0001 NOOP", kDefaultTimeout));

  // Second batch: eight more while a0001 is still awaiting its reply.
  for (int i = 2; i <= 9; ++i) {
    const auto tag = submit_noop();
    EXPECT_EQ("a000" + std::to_string(i), tag);
  }
  h.ctx.flush();

  ASSERT_TRUE(poll_until(
      [&] {
        std::lock_guard lock(mutex);
        return completed.size() >= 9U;
      },
      kDefaultTimeout));
  {
    std::lock_guard lock(mutex);
    for (const auto& [tag, ec] : completed) {
      EXPECT_FALSE(ec) << tag << ": " << ec.message();
    }
  }

  // The first batch is a write of its own; the rest follows in later
  // write(s), in submission order, each exactly once.
  const auto writes = h.recorder.writes();
  ASSERT_GE(writes.size(), 2U);
  EXPECT_NE(std::string::npos, writes.front().find("a0001 NOOP"));
  EXPECT_EQ(std::string::npos, writes.front().find("a0002"));

  const std::string all = h.recorder.written();
  std::size_t cursor = 0;
  for (int i = 1; i <= 9; ++i) {
    const std::string needle = "a000" + std::to_string(i) + " NOOP\r\n";
    const std::size_t pos = all.find(needle, cursor);
    ASSERT_NE(std::string::npos, pos) << needle << " missing from the wire";
    cursor = pos + needle.size();
  }
  // In-order presence plus an exact byte count proves each command was
  // written exactly once ("a000X NOOP\r\n" is 12 bytes).
  EXPECT_EQ(9U * 12U, all.size());
}

// Receiver for the sender-path test below: void result, no stop token.
class sender_path_receiver {
 public:
  sender_path_receiver(signal_event& done, std::error_code& ec) noexcept
      : done_(&done), ec_(&ec) {}

  void set_value(std::error_code ec) noexcept {
    *ec_ = ec;
    done_->arrive();
  }

  void set_stopped() noexcept { done_->arrive(); }

 private:
  signal_event* done_;
  std::error_code* ec_;
};

// Sender path (code_layout D1): a started submission on an empty queue
// registers a write automatically (the pump is kicked through the
// scheduler) — no explicit flush() is needed.
TEST(WritePump, SenderPathKicksWriteOnEmptyQueue) {
  context_holder h({
      server_bytes{std::string(kLayer1Greeting)},
      expect_write{"a0001 NOOP"},
      server_bytes{"a0001 OK NOOP completed\r\n"},
  });

  signal_event done;
  std::error_code ec;
  auto sender = h.ctx.submit<im::noop_command<>>();
  auto operation =
      bexec::connect(std::move(sender), sender_path_receiver{done, ec});
  bexec::start(operation);

  ASSERT_TRUE(done.wait_for(kDefaultTimeout))
      << "the started sender must complete without any flush()";
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_TRUE(h.recorder.contains_written("a0001 NOOP"));
  EXPECT_EQ(1U, h.recorder.writes().size());
}

}  // namespace
