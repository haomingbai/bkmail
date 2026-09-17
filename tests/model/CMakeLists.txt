# @file tests/model/CMakeLists.txt
# @brief Build rules for the core/data-model unit tests.
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT

add_executable(
  bkmail_model_tests
  test_capability_set.cpp
  test_data_model.cpp
  test_detail.cpp
  test_error.cpp
  test_fetch_items.cpp
  test_flags.cpp
  test_mailbox.cpp
  test_message_attributes.cpp
  test_raw_response.cpp
  test_search_criteria.cpp
  test_sequence_set.cpp)
bkmail_add_gtest(bkmail_model_tests model)
