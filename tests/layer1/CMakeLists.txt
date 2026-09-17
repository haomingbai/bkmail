# @file tests/layer1/CMakeLists.txt
# @brief Layer-1 unit tests (imap_context, write pump, unsolicited events)
#        over the scripted in-memory stream.
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT

# Test-support headers (scripted_stream.h, io_runner.h) are shared across
# suites; expose the tests/ root per docs/code_layout.md §6.
include_directories("${PROJECT_SOURCE_DIR}/tests")

add_executable(bkmail_layer1_tests test_imap_context.cpp test_write_pump.cpp
                                   test_unsolicited.cpp)
bkmail_add_gtest(bkmail_layer1_tests layer1)
