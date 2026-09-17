/**
 * @file tests/support/fake_imap_server.h
 * @brief Scripted fake IMAP server on a loopback socket for end-to-end tests.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * `bkmail::test::fake_imap_server` is the server half of the Layer-2 and
 * integration tests (docs/architecture.md appendix B): a single-connection
 * loopback TCP server replaying a fixed script against the real client stack
 * (`bkmail::async_connect` → state machine → tagged commands).
 *
 * The server side deliberately uses plain blocking POSIX sockets on its own
 * thread instead of bnio: the client under test supplies all the asynchrony,
 * and a blocking script engine keeps the expectation logic trivial and free
 * of races with the test thread.
 *
 * The script is a list of steps executed in order:
 *
 *   - `expect_client{"..."}` — block until the byte stream received from the
 *     client contains the given substring (searched after the previous match,
 *     so pipelined commands are consumed in order). When the match is on a
 *     command line, the command's tag is captured for `{tag}` substitution.
 *     A timeout (kStepTimeout) aborts the script with a recorded error.
 *   - `server_send{"..."}`   — write bytes to the client. The placeholder
 *     `{tag}` is replaced with the most recently captured command tag, so
 *     scripts never hardcode the client's tag counter.
 *
 * The engine never fails a gtest test from the server thread; it records
 * human-readable entries into an error log instead. Tests assert with
 * `server.wait_done()` + `server.ok()` / `server.errors()`.
 *
 * Usage sketch:
 *
 * ```cpp
 * fake_imap_server server;
 * server.start({
 *     server_send{"* OK [CAPABILITY IMAP4rev1] ready\r\n"},
 *     expect_client{"CAPABILITY"},
 *     server_send{"* CAPABILITY IMAP4rev1\r\n{tag} OK done\r\n"},
 * });
 * // ... drive bkmail against 127.0.0.1:server.port() ...
 * EXPECT_TRUE(server.wait_done(kDefaultTimeout));
 * EXPECT_TRUE(server.ok()) << server.errors();
 * ```
 *
 * Declare the server *before* the client session objects so it is destroyed
 * last (locals unwind in reverse order); the destructor closes the
 * connection and joins the thread.
 */

#pragma once
#ifndef BKMAIL_TESTS_SUPPORT_FAKE_IMAP_SERVER_H_
#define BKMAIL_TESTS_SUPPORT_FAKE_IMAP_SERVER_H_

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bkmail::test {

/**
 * Script step: wait until the client byte stream contains `substring`.
 * When `forbid` is non-empty, matching additionally asserts that the client
 * has NOT written `forbid` yet — this encodes stop-and-wait wire rules such
 * as "the APPEND literal must not be sent before the continuation request".
 * A violation is recorded in the error log.
 */
struct expect_client {
  std::string substring;
  // NSDMI keeps -Wmissing-field-initializers quiet for the many
  // `expect_client{"..."}` call sites that declare no gate.
  std::string forbid{};
};

/**
 * Script step: send `bytes` to the client; `{tag}` expands to the tag of the
 * most recently matched command line.
 */
struct server_send {
  std::string bytes;
};

using server_step = std::variant<expect_client, server_send>;

/**
 * Single-connection scripted IMAP server bound to 127.0.0.1:0.
 * See the file-level documentation for the script vocabulary.
 */
class fake_imap_server {
 public:
  /// Per-step bound for waiting on client bytes.
  static constexpr std::chrono::milliseconds kStepTimeout{10000};

  /// Binds and listens on a free loopback port; start() accepts and runs.
  fake_imap_server() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    const int one = 1;
    (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(listen_fd_, 4) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) ==
        0) {
      port_ = ntohs(addr.sin_port);
    }
  }

  fake_imap_server(const fake_imap_server&) = delete;
  fake_imap_server& operator=(const fake_imap_server&) = delete;

  ~fake_imap_server() { stop_and_join(); }

  /// The loopback port the server listens on (0 when construction failed).
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  /// Accepts one connection (on the server thread) and replays @p steps.
  /// When @p close_at_end is set the connection is closed after the last
  /// step, emulating a server that hangs up after its final word.
  void start(std::vector<server_step> steps, bool close_at_end = false) {
    thread_ = std::thread([this, script = std::move(steps), close_at_end] {
      run(script, close_at_end);
    });
  }

  /// Waits until the script finished (or was aborted); false on timeout.
  template <class Rep, class Period>
  [[nodiscard]] bool wait_done(
      std::chrono::duration<Rep, Period> timeout) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return done_; });
  }

  /// True when the script ran to completion without recorded errors.
  [[nodiscard]] bool ok() const {
    std::lock_guard lock(mutex_);
    return done_ && errors_.empty();
  }

  /// Newline-separated expectation failures, empty when none occurred.
  [[nodiscard]] std::string errors() const {
    std::lock_guard lock(mutex_);
    return errors_;
  }

  /// Everything the client has written so far.
  [[nodiscard]] std::string received() const {
    std::lock_guard lock(mutex_);
    return received_;
  }

  /// Bounded wait until the client byte stream contains `needle`.
  template <class Rep, class Period>
  [[nodiscard]] bool wait_received(
      std::string_view needle,
      std::chrono::duration<Rep, Period> timeout) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] {
      return received_.find(needle) != std::string::npos;
    });
  }

  /// Server-initiated connection close (BYE / transport-failure tests).
  void close_connection() {
    std::lock_guard lock(mutex_);
    close_conn_locked();
  }

 private:
  void run(const std::vector<server_step>& steps, bool close_at_end) {
    if (!accept_one()) {
      finish();
      return;
    }
    for (const auto& step : steps) {
      if (const auto* expect = std::get_if<expect_client>(&step)) {
        if (!await_bytes(expect->substring)) {
          record_error("timeout/disconnect waiting for client bytes: \"" +
                       expect->substring + "\"");
          break;
        }
        if (!expect->forbid.empty()) {
          std::lock_guard lock(mutex_);
          if (received_.find(expect->forbid) != std::string::npos) {
            record_error("client wrote forbidden bytes before the gate: \"" +
                         expect->forbid + "\"");
          }
        }
      } else if (const auto* send = std::get_if<server_send>(&step)) {
        if (!send_all(expand_tag(send->bytes))) {
          record_error("failed to send scripted bytes to the client");
          break;
        }
      }
    }
    if (close_at_end) {
      std::lock_guard lock(mutex_);
      close_conn_locked();
    }
    finish();
  }

  bool accept_one() {
    if (listen_fd_ < 0) {
      record_error("listener was not constructed successfully");
      return false;
    }
    pollfd pfd{};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, static_cast<int>(kStepTimeout.count()));
    if (ready <= 0) {
      record_error("timeout waiting for the client connection");
      return false;
    }
    const int conn = ::accept(listen_fd_, nullptr, nullptr);
    if (conn < 0) {
      record_error("accept failed");
      return false;
    }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    (void)::setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    {
      std::lock_guard lock(mutex_);
      conn_fd_ = conn;
    }
    return true;
  }

  bool await_bytes(const std::string& needle) {
    const auto deadline = std::chrono::steady_clock::now() + kStepTimeout;
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        const std::size_t pos = received_.find(needle, search_cursor_);
        if (pos != std::string::npos) {
          capture_tag_locked(pos);
          search_cursor_ = pos + needle.size();
          return true;
        }
        if (conn_fd_ < 0) {
          return false;
        }
      }
      pollfd pfd{};
      {
        std::lock_guard lock(mutex_);
        pfd.fd = conn_fd_;
      }
      pfd.events = POLLIN;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds{0}) {
        return false;
      }
      const int ready = ::poll(
          &pfd, 1,
          static_cast<int>((std::min)(remaining.count(), std::int64_t{100})));
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (ready == 0) {
        continue;
      }
      char buf[4096];
      const ssize_t n = ::recv(pfd.fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        return false;  // Orderly close (0) or error: the client went away.
      }
      std::lock_guard lock(mutex_);
      received_.append(buf, static_cast<std::size_t>(n));
      cv_.notify_all();
    }
  }

  /// Captures the tag of the command line containing the match at @p pos.
  /// Lines whose first token is not tag-shaped (e.g. "DONE" or literal data)
  /// leave the previous tag untouched.
  void capture_tag_locked(std::size_t pos) {
    std::size_t line_start = received_.rfind("\r\n", pos);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 2;
    const std::size_t line_end = received_.find(' ', line_start);
    const std::string token = received_.substr(
        line_start,
        line_end == std::string::npos ? line_end : line_end - line_start);
    bool tag_shaped = !token.empty() &&
                      std::isalpha(static_cast<unsigned char>(token.front()));
    bool has_digit = false;
    for (const char c : token) {
      if (!std::isalnum(static_cast<unsigned char>(c))) {
        tag_shaped = false;
        break;
      }
      has_digit = has_digit || std::isdigit(static_cast<unsigned char>(c));
    }
    if (tag_shaped && has_digit) {
      last_tag_ = token;
    }
  }

  std::string expand_tag(std::string bytes) const {
    std::lock_guard lock(mutex_);
    const std::string placeholder = "{tag}";
    for (std::size_t pos = bytes.find(placeholder); pos != std::string::npos;
         pos = bytes.find(placeholder, pos + last_tag_.size())) {
      bytes.replace(pos, placeholder.size(), last_tag_);
    }
    return bytes;
  }

  bool send_all(const std::string& bytes) {
    // MSG_NOSIGNAL is Linux-only; on BSD/macOS SO_NOSIGPIPE (set at accept
    // time) provides the same protection.
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      int fd;
      {
        std::lock_guard lock(mutex_);
        fd = conn_fd_;
      }
      if (fd < 0) {
        return false;
      }
      const ssize_t n =
          ::send(fd, bytes.data() + sent, bytes.size() - sent, kSendFlags);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  void record_error(std::string message) {
    std::lock_guard lock(mutex_);
    if (!errors_.empty()) {
      errors_ += '\n';
    }
    errors_ += message;
  }

  void close_conn_locked() {
    if (conn_fd_ >= 0) {
      (void)::shutdown(conn_fd_, SHUT_RDWR);
      (void)::close(conn_fd_);
      conn_fd_ = -1;
      cv_.notify_all();
    }
  }

  void finish() {
    {
      std::lock_guard lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
  }

  void stop_and_join() {
    {
      std::lock_guard lock(mutex_);
      close_conn_locked();
      if (listen_fd_ >= 0) {
        (void)::close(listen_fd_);
        listen_fd_ = -1;
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::uint16_t port_ = 0;
  int listen_fd_ = -1;
  int conn_fd_ = -1;
  std::thread thread_;

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::string received_;
  std::size_t search_cursor_ = 0;
  std::string last_tag_;
  std::string errors_;
  bool done_ = false;
};

}  // namespace bkmail::test

#endif  // BKMAIL_TESTS_SUPPORT_FAKE_IMAP_SERVER_H_
