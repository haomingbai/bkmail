# @file CMakeLists.txt
# @brief Build definition for the bkmail examples.
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-16
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT
#
# @details
# One executable per example directory, each with a single main.cpp
# (docs/code_layout.md §3.9). list_subjects mirrors the blocking flow of
# docs/usage.md §1.3, idle_watch the §2.6/§2.7 IDLE loop with SIGINT
# cancellation, batch_commands the §3.2–§3.6 command-layer batching, and
# real_server_session is the read-only live-server connectivity check
# (deliberately outside the test suite).

function(bkmail_add_example name)
  add_executable(${name} ${name}/main.cpp)
  target_link_libraries(${name} PRIVATE bkmail::bkmail)
  bkmail_set_project_warnings(${name})
endfunction()

bkmail_add_example(hello_bkmail)
bkmail_add_example(list_subjects)
bkmail_add_example(idle_watch)
bkmail_add_example(batch_commands)
bkmail_add_example(real_server_session)
