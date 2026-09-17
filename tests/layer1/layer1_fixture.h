/**
 * @file tests/layer1/layer1_fixture.h
 * @brief Shared scripted-context fixture for the Layer-1 unit tests.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_TESTS_LAYER1_LAYER1_FIXTURE_H_
#define BKMAIL_TESTS_LAYER1_LAYER1_FIXTURE_H_

#include <bkmail/bkmail.h>
#include <support/io_runner.h>
#include <support/scripted_stream.h>

#include <string_view>
#include <utility>

namespace bkmail::test {

namespace im = bkmail::imap;

using scripted_context = im::imap_context<scripted_stream>;

/// The greeting every Layer-1 script starts with; the context consumes it
/// through the unsolicited path (code_layout D5).
inline constexpr std::string_view kLayer1Greeting =
    "* OK [CAPABILITY IMAP4rev1 UIDPLUS MOVE IDLE LITERAL+ SASL-IR] fake "
    "ready\r\n";

/// Builds a context over the given script; the recorder must be captured
/// before the stream is moved. Usage:
///   context_holder h({...});
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

}  // namespace bkmail::test

#endif  // BKMAIL_TESTS_LAYER1_LAYER1_FIXTURE_H_
