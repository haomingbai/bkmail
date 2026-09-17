# @file tests/layer2/CMakeLists.txt
# @brief Test binary for the session state-machine layer.
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT

# Test-support headers (scripted_stream.h, fake_imap_server.h, io_runner.h)
# are shared across suites; expose the tests/ root per docs/code_layout.md §6.
include_directories("${PROJECT_SOURCE_DIR}/tests")

add_executable(bkmail_layer2_tests test_connect.cpp test_state_machine.cpp)
bkmail_add_gtest(bkmail_layer2_tests layer2)
