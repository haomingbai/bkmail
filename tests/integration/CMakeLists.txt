# @file tests/integration/CMakeLists.txt
# @brief Integration test binary (command layer, session flows, IDLE,
#        protocol robustness, error paths).
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT

# Test-support headers (scripted_stream.h, fake_imap_server.h, io_runner.h)
# are shared across suites; expose the tests/ root per docs/code_layout.md §6.
include_directories("${PROJECT_SOURCE_DIR}/tests")

add_executable(
  bkmail_integration_tests
  test_command_layer.cpp
  test_error_paths.cpp
  test_idle_unsolicited.cpp
  test_protocol_robustness.cpp
  test_session_flow.cpp)
bkmail_add_gtest(bkmail_integration_tests integration)
