/**
 * @file main.cpp
 * @brief Command-layer batching: make_command erasure and one flush().
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Executable mirror of docs/usage.md §3.2–§3.6: the stream is built by
 * hand with bnio primitives (resolve, TCP connect, TLS handshake), handed
 * to bkmail::imap::imap_context, and a LOGIN + CAPABILITY + LIST batch is
 * erased through make_command and submitted at once, so all three commands
 * share one write syscall after a single flush(). A follow-up LOGOUT is
 * submitted and flushed separately once the batch has completed.
 *
 * The example also registers an on_unsolicited handler (usage.md §3.5):
 * the greeting and any server pushes surface there because the state
 * layer, which would otherwise consume them, is not in play.
 *
 * Pipelining note: LOGIN, CAPABILITY and LIST are sent in one batch; the
 * server processes them in order, so the LIST after a failed LOGIN simply
 * answers NO (reported as errc::command_rejected).
 *
 * Usage:
 *   batch_commands <host> [port]       (port defaults to 993)
 * Credentials come from the environment, never from the source tree:
 *   BKMAIL_USER       IMAP user name
 *   BKMAIL_PASSWORD   IMAP password (use an app-specific password)
 *
 * Safety: read-only in effect — LOGIN, CAPABILITY, LIST and LOGOUT change
 * no mailbox or message state.
 */

#include <bkmail/bkmail.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

class io_runner {
 public:
  io_runner() : thread_([this] { (void)ioc_.run(); }) {}
  ~io_runner() {
    ioc_.stop();
    thread_.join();
  }

  [[nodiscard]] bnio::io_context& get() noexcept { return ioc_; }

 private:
  bnio::io_context ioc_;
  std::thread thread_;
};

[[nodiscard]] bool failed(const char* what, std::error_code ec) {
  if (!ec) return false;
  std::fprintf(stderr, "%s: %s\n", what, ec.message().c_str());
  return true;
}

[[nodiscard]] const char* required_env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    std::fprintf(stderr, "error: set the %s environment variable\n", name);
    std::exit(2);
  }
  return value;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr,
                 "usage: %s <host> [port]   (port defaults to 993)\n"
                 "credentials: BKMAIL_USER / BKMAIL_PASSWORD environment "
                 "variables\n",
                 argv[0]);
    return 2;
  }
  const char* host = argv[1];
  const char* port = argc == 3 ? argv[2] : "993";
  const char* user = required_env("BKMAIL_USER");
  const char* password = required_env("BKMAIL_PASSWORD");

  io_runner runner;

  // ---- Build the stream by hand (usage.md §3.2) ----

  std::array<bnio::ip::endpoint, 8> endpoints{};
  auto resolved =
      bth::sync_wait(runner.get().get_post_scheduler().async_resolve(
          host, port, bnio::dns_result_view{endpoints}));
  if (!resolved) return 1;
  auto& [resolve_ec, endpoint_count] = *resolved;
  if (failed("resolve", resolve_ec) || endpoint_count == 0) return 1;

  bnio::tcp::socket sock;
  {
    const auto& endpoint = endpoints[0];
    if (auto ec = sock.open(endpoint.address().is_v4() ? bnio::ip::tcp::v4()
                                                       : bnio::ip::tcp::v6())) {
      (void)failed("socket", ec);
      return 1;
    }
    auto connected = bth::sync_wait(
        sock.async_connect(runner.get().get_post_scheduler(), endpoint));
    if (!connected) return 1;
    auto& [connect_ec] = *connected;
    if (failed("connect", connect_ec)) return 1;
  }

  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};
  bnio::ssl_stream<bnio::tcp::socket> stream{std::move(sock), tls};
  {
    auto shook = bth::sync_wait(stream.async_handshake(
        runner.get().get_post_scheduler(), bnio::ssl_handshake_type::client));
    if (!shook) return 1;
    auto& [handshake_ec] = *shook;
    if (failed("handshake", handshake_ec)) return 1;
  }

  // Hand the connected, handshaken stream to bkmail. The context arms its
  // permanent read loop immediately and the greeting arrives through the
  // unsolicited path.
  im::imap_context<bnio::ssl_stream<bnio::tcp::socket>> ctx{std::move(stream),
                                                            runner.get()};

  auto registration = ctx.on_unsolicited([](const im::unsolicited_event<>& e) {
    std::visit(
        [](const auto& event) {
          using T = std::decay_t<decltype(event)>;
          if constexpr (std::is_same_v<T, im::greeting_event<>>) {
            std::printf("greeting: %s\n", event.text.c_str());
          } else if constexpr (std::is_same_v<T, im::bye_event<>>) {
            std::printf("server bye: %s\n", event.text.c_str());
          } else if constexpr (std::is_same_v<T, im::exists_event>) {
            std::printf("push: %u messages exist\n", event.count);
          }
          // recent/expunge/flags/capability pushes are ignored here.
        },
        e);
  });

  // ---- Type-erased batch submission (usage.md §3.4) ----

  // Handlers run on the io thread; the semaphore counts completions and the
  // mutex serializes their printf output.
  std::counting_semaphore<8> completions{0};
  std::mutex print_mutex;

  bkmail::account_info<> account{.user_name = user, .password = password};

  std::vector<std::unique_ptr<im::imap_command<>>> batch;
  batch.push_back(
      im::make_command(im::login_command<>{account}, [&](std::error_code ec) {
        std::lock_guard lock(print_mutex);
        (void)failed("login", ec);
        completions.release();
      }));
  batch.push_back(im::make_command(
      im::capability_command<>{},
      [&](std::error_code ec, im::capability_set<> caps) {
        std::lock_guard lock(print_mutex);
        if (!failed("capability", ec)) {
          std::printf("capabilities: IDLE=%d UIDPLUS=%d LITERAL+=%d\n",
                      static_cast<int>(caps.contains("IDLE")),
                      static_cast<int>(caps.contains("UIDPLUS")),
                      static_cast<int>(caps.contains("LITERAL+")));
        }
        completions.release();
      }));
  batch.push_back(im::make_command(
      im::list_command<>{"", "*"},
      [&](std::error_code ec, std::vector<im::mailbox_entry<>> entries) {
        std::lock_guard lock(print_mutex);
        if (!failed("list", ec)) {
          for (const auto& entry : entries) {
            std::printf("mailbox: %s (delimiter %s%s)\n", entry.name.c_str(),
                        entry.delimiter.c_str(),
                        entry.no_select ? ", no-select" : "");
          }
        }
        completions.release();
      }));

  const auto tags = ctx.submit(std::move(batch));
  ctx.flush();  // all three commands in one write
  {
    std::lock_guard lock(print_mutex);
    std::printf("submitted batch: login=%s capability=%s list=%s\n",
                tags.at(0).c_str(), tags.at(1).c_str(), tags.at(2).c_str());
  }

  // Wait for all three tagged completions.
  for (int i = 0; i < 3; ++i) {
    completions.acquire();
  }

  // ---- LOGOUT as a second, separate round trip ----
  ctx.submit(im::logout_command<>{}, [&](std::error_code ec) {
    std::lock_guard lock(print_mutex);
    (void)failed("logout", ec);
    completions.release();
  });
  ctx.flush();
  completions.acquire();

  return 0;
}
