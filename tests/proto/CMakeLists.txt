# @file tests/proto/CMakeLists.txt
# @brief Protocol-layer unit tests (astring helpers, parse cursor, response
#        lexer, response parser).
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT

add_executable(
  bkmail_proto_tests test_astring.cpp test_parse_cursor.cpp
  test_response_lexer.cpp test_response_parser.cpp)
bkmail_add_gtest(bkmail_proto_tests proto)
