# Using bkmail

This guide is the user-facing contract for bkmail: it describes the API the
library exposes, from the caller's point of view, top-down. It mixes a
tutorial with reference tables. All examples are complete, compilable-style
fragments; names follow `docs/code_style.md`.

bkmail is an IMAP (RFC 3501) client library built on two layers:

| Layer | Header-facing types | Use it when |
| --- | --- | --- |
| State-machine layer | session states in `bkmail::imap`: `not_authenticated_state`, `authenticated_state`, `selected_state`, `logout_state` (+ the `idle_state` handle) | You want a sequential, type-safe conversation with one IMAP server. This is the default choice. |
| Command layer | `bkmail::imap::imap_context` plus one type per IMAP command | You need parallel commands, batched writes, custom/extension commands, or manual control over I/O scheduling. |

The protocol types of both layers live in `namespace bkmail::imap`; the
protocol-neutral data model (`mail`, `mail_header`, `envelope`, `address`,
`body_structure`, `account_info`), the error codes (`bkmail::errc`), the
connect entry points (`bkmail::async_connect`, `bkmail::async_connect_tls`)
and the `bkmail::pack` adaptor live directly in `namespace bkmail`.
Examples below alias the IMAP namespace as `namespace im = bkmail::imap;`
(as the bundled examples do).

Both layers are templates over an allocator. Every type that owns dynamic
storage takes a trailing `class Allocator = std::allocator<std::byte>`
parameter, so the common case needs no spelling. Examples use the default
unless the section is about allocators.

Throughout this guide a **sender** means a lazy bexec sender. Nothing
happens until the sender is connected to a receiver and started
(`bexec::connect` + `bexec::start`), awaited from a `bexec::task`, or pumped
through `bexec::this_thread::sync_wait`.

---

## 1. Getting started

### 1.1 Adding bkmail to a CMake project

The simplest route is `FetchContent`; bkmail's own dependency resolvers then
pull in `bnio` and `bexec` (resolving `bexec` first so it is never
introduced twice):

```cmake
include(FetchContent)

FetchContent_Declare(
  bkmail
  GIT_REPOSITORY https://github.com/haomingbai/bkmail
  GIT_TAG        main
)

# Optional: force how the transitive dependencies are resolved.
# Values are AUTO, FIND_PACKAGE, SOURCE, FETCH (default AUTO).
set(BKMAIL_BEXEC_PROVIDER FETCH)
set(BKMAIL_BNIO_PROVIDER  FETCH)
# Optional pins used by the FETCH provider:
# set(BKMAIL_BEXEC_GIT_TAG v0.1.0)
# set(BKMAIL_BNIO_GIT_TAG  v0.2.0)

FetchContent_MakeAvailable(bkmail)

target_link_libraries(your_app PRIVATE bkmail::bkmail)
target_compile_features(your_app PRIVATE cxx_std_20)
```

Other consumption modes (`add_subdirectory`, `find_package(bkmail CONFIG
REQUIRED)` after install, pkg-config) and the local-checkout `SOURCE`
provider are described in `README.md`.

### 1.2 The two ways to connect

IMAP servers listen on port 143 (plaintext, usually upgraded with STARTTLS)
and port 993 (implicit TLS). bkmail offers one entry function per transport;
both resolve the host, connect, (for TLS) perform the handshake, and consume
the server greeting:

```cpp
// Plaintext (typically port 143).
auto async_connect(std::string_view host, std::string_view service,
                   bnio::io_context& ioc,
                   const Allocator& alloc = Allocator{});

// Implicit TLS (typically port 993). `tls` is only borrowed.
auto async_connect_tls(std::string_view host, std::string_view service,
                       bnio::io_context& ioc, bnio::ssl_context& tls,
                       const Allocator& alloc = Allocator{});
```

Both return a lazy sender that publishes **one `set_value` signature per
greeting outcome** — bkmail never merges outcomes into a variant payload;
merging is the consumer's choice (§2.4):

| Outcome | Value signature |
| --- | --- |
| `OK` greeting | `set_value(std::error_code, im::not_authenticated_state<A>)` |
| `PREAUTH` greeting | `set_value(std::error_code, im::authenticated_state<A>)` |
| `BYE` greeting, or any pre-greeting failure (DNS / TCP connect / TLS handshake) | `set_value(std::error_code, im::logout_state<A>)` — `ec == errc::server_bye` for the BYE case |

plus `set_stopped()` when cancelled. A failed connect never fabricates a
session: the terminal `logout_state` always rides alongside the error
code. See §2.4 for how to match the alternatives.

### 1.3 Minimal example, blocking style

The example below connects over TLS, logs in, selects INBOX, prints the
subjects of the five most recent messages, and logs out. Every step blocks
the calling thread with `bexec::this_thread::sync_wait` (or its variant
form for the branching senders). The bundled `examples/list_subjects/`
program is this same flow, executable against a real server.

One rule matters before the code: a bkmail sender only makes progress while
some thread is running the `bnio::io_context` it was given. `sync_wait`
blocks the *current* thread, so the `io_context` needs its own worker
thread. The small `io_runner` helper below is reused in later examples.

```cpp
#include <bkmail/bkmail.h>

#include <cstdio>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace {

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

// Runs a bnio::io_context on a dedicated thread for the object's lifetime.
class io_runner {
 public:
  io_runner() : thread_([this] { (void)ioc_.run(); }) {}
  ~io_runner() {
    ioc_.stop();
    thread_.join();
  }

  bnio::io_context& get() noexcept { return ioc_; }

 private:
  bnio::io_context ioc_;
  std::thread thread_;
};

[[nodiscard]] bool failed(const char* what, std::error_code ec) {
  if (!ec) return false;
  std::fprintf(stderr, "%s: %s\n", what, ec.message().c_str());
  return true;
}

/// One alternative of a sync_wait_with_variant outcome: (ec, state).
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

/// Extracts the error code from whichever alternative the outcome holds.
[[nodiscard]] std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
}

}  // namespace

int main() {
  io_runner runner;
  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};

  // Connect (TLS, port 993) and consume the greeting. The connect sender
  // publishes one set_value signature per outcome; sync_wait_with_variant
  // merges them into a variant of (ec, state) tuples (§2.4).
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect_tls("imap.example.com", "993", runner.get(), tls));
  if (!connected) return 1;  // operation was stopped
  if (failed("connect", outcome_ec(*connected))) return 1;

  // OK means we must log in, PREAUTH means the server already considers us
  // authenticated. This example handles the common OK case; see §2.4 for
  // the full visit pattern.
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&*connected);
  if (not_authed == nullptr) {
    std::fprintf(stderr, "unexpected PREAUTH greeting\n");
    return 1;
  }

  // LOGIN with a password (use authenticate_oauth2 for XOAUTH2 servers).
  // login() is branching too: Authenticated on OK, the retained
  // Not-Authenticated state on rejection.
  bkmail::account_info<> account{.user_name = "you@example.com",
                                 .password = "app-specific-password"};
  auto logged_in = bth::sync_wait_with_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  if (!logged_in) return 1;
  if (failed("login", outcome_ec(*logged_in))) return 1;
  auto& authed = std::get<1>(
      std::get<state_outcome<im::authenticated_state<>>>(*logged_in));

  // SELECT INBOX; the Selected state carries a mailbox_info snapshot.
  auto selected_result =
      bth::sync_wait_with_variant(std::move(authed).select("INBOX"));
  if (!selected_result) return 1;
  if (failed("select", outcome_ec(*selected_result))) return 1;
  auto& selected = std::get<1>(
      std::get<state_outcome<im::selected_state<>>>(*selected_result));

  const std::uint32_t total = selected.mailbox().exists;
  std::printf("INBOX has %u messages\n", total);
  if (total == 0) {
    (void)bth::sync_wait(std::move(selected).logout());
    return 0;
  }

  // FETCH the ENVELOPE of the five most recent messages.
  char range[16];
  const std::uint32_t first = total > 5 ? total - 4 : 1;
  std::snprintf(range, sizeof range, "%u:%u", first, total);
  auto fetched = bth::sync_wait(std::move(selected).fetch_envelopes(range));
  if (!fetched) return 1;
  auto& [fetch_ec, envelopes, selected_back] = *fetched;
  if (failed("fetch", fetch_ec)) return 1;

  for (const auto& env : envelopes) {
    std::printf("- %s\n", env.subject.c_str());
  }

  // LOGOUT. The final state exists only to confirm the exchange completed.
  auto done = bth::sync_wait(std::move(selected_back).logout());
  if (!done) return 1;
  auto& [logout_ec, logged_out] = *done;
  return failed("logout", logout_ec) ? 1 : 0;
}
```

Note the shape that repeats at every step:

1. The current state is consumed by rvalue: `std::move(state).operation(...)`.
   Because the old state is moved from, you cannot issue two operations at
   once — the type system enforces the one-command-in-flight rule of this
   layer.
2. The returned sender completes on the value channel with the error code
   first, then the operation result (if any), then the next state. Most
   operations have exactly one value signature and pair with plain
   `sync_wait`; the *branching* ones (connect, `login`/`authenticate`,
   `select`/`examine`) publish one signature per outcome state and pair
   with `sync_wait_with_variant`, which merges the alternatives into a
   `std::variant` of tuples (§2.4).
3. `sync_wait`/`sync_wait_with_variant` return `std::optional<...>`;
   `std::nullopt` means the operation was stopped via a stop token (§2.7).

### 1.4 The same flow as a coroutine

`bexec::task` can `co_await` any sender, with one restriction: the awaited
sender must have at most one value in its value completion. bkmail offers
two adaptors at the await point (`include/bkmail/common/pack.h`, re-exported from
`bkmail/bkmail.h`):

- **single-signature operations** deliver `(error_code, [result,]
  next_state)` — two or three values — so pack them into one tuple with
  `bkmail::pack`: `co_await bkmail::pack(op)`;
- **branching senders** (connect, `login`/`authenticate`,
  `select`/`examine`) publish one value signature per outcome state, so
  merge the signatures first with `bexec::into_variant`:
  `co_await bexec::into_variant(op)` yields one value, a `std::variant` of
  per-outcome tuples (§2.4).

```cpp
#include <bkmail/bkmail.h>

#include <cstdio>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace im = bkmail::imap;

/// One alternative of an into_variant / sync_wait_with_variant outcome.
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

/// Extracts the error code from whichever alternative the outcome holds.
std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
}

bexec::task<std::error_code> print_recent_subjects(bnio::io_context& ioc,
                                                   bnio::ssl_context& tls) {
  auto connected = co_await bexec::into_variant(
      bkmail::async_connect_tls("imap.example.com", "993", ioc, tls));
  if (auto ec = outcome_ec(connected)) co_return ec;

  // Handle the common OK-greeting case; PREAUTH handling is shown in §2.4.
  auto* not_authed =
      std::get_if<state_outcome<im::not_authenticated_state<>>>(&connected);
  if (not_authed == nullptr) co_return std::errc::operation_not_permitted;

  bkmail::account_info<> account{.user_name = "you@example.com",
                                 .password = "app-specific-password"};
  auto logged_in = co_await bexec::into_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  if (auto ec = outcome_ec(logged_in)) co_return ec;
  auto& authed = std::get<1>(
      std::get<state_outcome<im::authenticated_state<>>>(logged_in));

  auto selected_result = co_await bexec::into_variant(
      std::move(authed).select("INBOX"));
  if (auto ec = outcome_ec(selected_result)) co_return ec;
  auto& selected = std::get<1>(
      std::get<state_outcome<im::selected_state<>>>(selected_result));

  if (selected.mailbox().exists != 0) {
    // Single-signature operations await through bkmail::pack instead.
    auto [fetch_ec, envelopes, selected_back] =
        co_await bkmail::pack(std::move(selected).fetch_envelopes("1:*"));
    if (fetch_ec) co_return fetch_ec;
    for (const auto& env : envelopes) {
      std::printf("- %s\n", env.subject.c_str());
    }
    selected = std::move(selected_back);
  }

  auto [logout_ec, logged_out] =
      co_await bkmail::pack(std::move(selected).logout());
  co_return logout_ec;
}

int main() {
  bnio::io_context ioc;
  std::thread io_thread([&ioc] { (void)ioc.run(); });
  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};

  auto result = bexec::this_thread::sync_wait(print_recent_subjects(ioc, tls));

  ioc.stop();
  io_thread.join();

  if (!result) return 1;  // task was stopped
  auto& [ec] = *result;
  if (ec) {
    std::fprintf(stderr, "failed: %s\n", ec.message().c_str());
    return 1;
  }
  return 0;
}
```

If an awaited sender completes stopped, the awaiting task is marked stopped
and `sync_wait(task)` returns `std::nullopt`; `task::result()` instead
throws `bexec::task_stopped`.

---

## 2. The state-machine layer

### 2.1 States at a glance

The layer mirrors the four IMAP states of RFC 3501. Each state is a
move-only handle (move-constructible and move-assignable) that owns the
session — the `imap_connection` travels with the state chain; each
operation is an `&&`-qualified member function returning a lazy sender.
All types below are in `bkmail::imap`.

| State type | Operations (the three live states also have `capability()`, `noop()`, `logout()`) | Successor state(s) |
| --- | --- | --- |
| `not_authenticated_state<A>` | `login(account)`, `authenticate(mechanism, initial_response)`, `authenticate_oauth2(user, token)`, `start_tls(ssl_context&)` | login/authenticate: `authenticated_state<A>` on OK, the **retained** `not_authenticated_state<A>` on NO/BAD/failure · `start_tls`: same state over TLS (single signature) |
| `authenticated_state<A>` | `select(mailbox)`, `examine(mailbox)`; `list(ref, pattern)`, `status(mailbox, items)`, `create(m)`, `delete_mailbox(m)`, `rename(old, new)`, `subscribe(m)`, `unsubscribe(m)`, `append(mailbox, mail, flags)` | select/examine: `selected_state<A>` on OK, the **retained** `authenticated_state<A>` on NO/failure · everything else: same state |
| `selected_state<A>` | `fetch_envelopes(seq)`, `fetch_headers(seq, fields)`, `fetch_message(seq)`, `fetch(seq, items)`, `search(criteria)`, `store(seq, flags, mode)`, `copy(seq, mailbox)`, `move(seq, mailbox)`, `expunge()`, `idle()`; plus UID variants `uid_fetch_envelopes`, `uid_fetch_headers`, `uid_fetch_message`, `uid_fetch`, `uid_search`, `uid_store`, `uid_copy`, `uid_move`; `close()` | same state / `authenticated_state<A>` for `close()` |
| `logout_state<A>` | — (terminal state, no operations) | — |

Branching operations publish **one `set_value` signature per successor
state**; the retained state on failure is the real session, not a
placeholder, so you can retry (or issue another command) on it. The
signature-level detail is §5.4 of `docs/architecture.md`.

Key accessors:

- `selected_state::mailbox() const noexcept -> const mailbox_info<A>&` —
  the server's last-known view of the open mailbox (EXISTS/RECENT/UNSEEN/
  UIDVALIDITY/UIDNEXT/flags, read-only flag). Unsolicited `EXISTS`,
  `RECENT` and `EXPUNGE` pushes update this snapshot between commands, so
  it is always safe to read after an operation completes.
- `logout_state` carries no data; it only proves the LOGOUT exchange
  finished.

`examine` is the read-only twin of `select`: with it, `store`, `expunge`,
`move` and `close` will fail at the server with a `NO`; bkmail reports that
as `errc::command_rejected` (see §6).

### 2.2 Consuming an operation sender

All four consumption styles work on every operation sender.

**`bexec::this_thread::sync_wait(sender)`** — blocks the current thread,
returns `std::optional<std::tuple<std::error_code, ...>>`. This is the
default for the **single-signature** operations (everything except
connect/login/authenticate/select/examine). Shown in §1.3.

**`bexec::this_thread::sync_wait_with_variant(sender)`** — the blocking
consumer for the **branching** senders (connect, `login`/`authenticate`,
`select`/`examine`): it merges the per-outcome value signatures into a
`std::variant` of tuples, one alternative per signature. Shown in §1.3 and
§2.4.

**`co_await` inside `bexec::task`** — `bkmail::pack` for single-signature
operations, `bexec::into_variant` for branching ones (§1.4).

**A custom receiver** — full control (this is also how cancellation is
wired, see §2.7). The receiver simply declares one `set_value` overload per
signature it wants to handle; here against a single-signature operation:

```cpp
class print_envelopes {
 public:
  using env_type = bexec::empty_env;

  env_type get_env() const noexcept { return {}; }

  void set_value(std::error_code ec, std::vector<bkmail::envelope<>> envs,
                 im::selected_state<> state) noexcept {
    if (!ec) {
      for (const auto& e : envs) std::printf("- %s\n", e.subject.c_str());
    }
    // `state` is the session again; store it somewhere or issue the next op.
  }

  void set_stopped() noexcept { std::puts("fetch cancelled"); }
};

auto op = bexec::connect(std::move(selected).fetch_envelopes("1:10"),
                         print_envelopes{});
bexec::start(op);
```

**Chaining with `bexec::let_value`** — the callable must accept every value
signature of the incoming sender. Merge a branching sender first with
`bexec::into_variant`: the chain then sees exactly one value (a
`std::variant` of per-outcome tuples) and plain `sync_wait` suffices at the
end of the chain. Single-signature operations chain directly.

### 2.3 Reading the error code and the next state

Every value tuple starts with `std::error_code`. A default-constructed
(falsy) code means success; the remaining tuple elements are the result (for
querying operations) and the next state. The next state is delivered by the
sender even when the operation failed, unless the failure killed the
connection — see §6 for which errors are recoverable and which mean
"rebuild the session".

```cpp
auto result = bexec::this_thread::sync_wait(
    std::move(selected).search("UNSEEN FROM \"bob@example.com\""));
if (!result) { /* stopped */ }
auto& [ec, matches, selected_back] = *result;
if (!ec) {
  std::printf("%zu unseen messages from bob\n", matches.size());
}
selected = std::move(selected_back);
```

### 2.4 Matching branching completions

Three operation groups have more than one possible successor state, and
their senders publish **one `set_value` signature per outcome** instead of
merging the states into a variant payload:

- `async_connect` / `async_connect_tls`: `not_authenticated_state` (`OK`
  greeting) / `authenticated_state` (`PREAUTH`) / `logout_state` (`BYE`
  greeting or a pre-greeting failure, with the error code alongside);
- `login` / `authenticate` / `authenticate_oauth2`: `authenticated_state`
  on OK / the **retained** `not_authenticated_state` on NO/BAD/failure;
- `select` / `examine`: `selected_state` on OK / the **retained**
  `authenticated_state` on NO/failure.

Merging the alternatives is a consumer-side choice:
`bexec::this_thread::sync_wait_with_variant` for blocking waits (§1.3),
`bexec::into_variant` ahead of `co_await` inside a task (§1.4). Both yield
a `std::variant` of `(std::error_code, State)` tuples with one alternative
per signature. Use `std::get_if` when exactly one alternative is
interesting, `std::visit` when you handle all of them. Because the two
greeting alternatives continue with *different* operations, an `if` is
usually clearer than a `visit` whose branches must return a common type:

```cpp
template <class State>
using state_outcome = std::tuple<std::error_code, State>;

std::error_code outcome_ec(const auto& outcome) {
  return std::visit([](const auto& t) { return std::get<0>(t); }, outcome);
}

bexec::task<std::error_code> run(bnio::io_context& ioc,
                                 bnio::ssl_context& tls) {
  auto connected = co_await bexec::into_variant(
      bkmail::async_connect_tls("imap.example.com", "993", ioc, tls));
  if (auto ec = outcome_ec(connected)) {
    co_return ec;  // covers the BYE-greeting / failed-connect logout branch
  }

  // States are move-only, so optional is the natural "state to be filled"
  // holder when the two greeting branches converge.
  std::optional<im::authenticated_state<>> authed;
  if (auto* na = std::get_if<state_outcome<im::not_authenticated_state<>>>(
          &connected)) {
    bkmail::account_info<> account{.user_name = "you@example.com",
                                   .password = "app-specific-password"};
    auto logged_in = co_await bexec::into_variant(
        std::move(std::get<1>(*na)).login(account));
    if (auto ec = outcome_ec(logged_in)) {
      co_return ec;  // rejected: NO/BAD keeps the Not-Authenticated state
    }
    authed.emplace(std::move(std::get<1>(
        std::get<state_outcome<im::authenticated_state<>>>(logged_in))));
  } else {
    authed.emplace(std::move(std::get<1>(
        std::get<state_outcome<im::authenticated_state<>>>(connected))));
  }

  auto selected_result =
      co_await bexec::into_variant(std::move(*authed).select("INBOX"));
  co_return outcome_ec(selected_result);  // continue with the Selected
                                          // state in a real program
}
```

`visit` shines when the follow-up does not depend on which alternative
arrived. The three live states all offer `logout()` with the same
completion shape — `(error_code, logout_state)` — so a visit that blocks
inside each branch has one return type even though the sender types
differ:

```cpp
// A state variant of your own making, e.g. produced by into_variant along
// a chain that may end in different live states.
using live_state = std::variant<im::not_authenticated_state<>,
                                im::authenticated_state<>,
                                im::selected_state<>>;
live_state state = /* ... */;

auto done = std::visit(
    [](auto& s) { return bexec::this_thread::sync_wait(std::move(s).logout()); },
    state);
// done: std::optional<std::tuple<std::error_code, im::logout_state<>>>
```

### 2.5 Common workflows

**List mailboxes** (LIST `"<reference>" "<pattern>"`; `""` + `"*"` lists
everything):

```cpp
auto res = bth::sync_wait(std::move(authed).list("", "*"));
auto& [ec, entries, authed_back] = *res;
for (const auto& entry : entries) {
  std::printf("%s (delimiter %s)\n", entry.name.c_str(),
              entry.delimiter.c_str());
}
authed = std::move(authed_back);
```

**Search unread and read one body** (BODY.PEEK[] never sets `\Seen`):

```cpp
auto found = bth::sync_wait(std::move(selected).search("UNSEEN"));
auto& [ec1, unseen, sel1] = *found;
selected = std::move(sel1);
if (!ec1 && !unseen.empty()) {
  // fetch_message issues BODY.PEEK[] for the given message sequence set.
  auto got = bth::sync_wait(
      std::move(selected).fetch_message(std::to_string(unseen.front())));
  auto& [ec2, messages, sel2] = *got;
  selected = std::move(sel2);
  if (!ec2 && !messages.empty()) {
    const bkmail::mail<>& m = messages.front();
    std::printf("subject: %s\n", m.header.subject.c_str());
    std::fwrite(m.body.data.data(), 1, m.body.data.size(), stdout);
  }
}
```

**Change flags** (`store_mode::add` = `+FLAGS`, `remove` = `-FLAGS`,
`replace` = `FLAGS`):

```cpp
im::flag_set seen;
seen.set(im::message_flag::seen);
auto stored = bth::sync_wait(
    std::move(selected).store("1:5", seen, im::store_mode::add));
auto& [ec3, sel3] = *stored;
selected = std::move(sel3);
```

**Copy or move between mailboxes** (`move` needs the `MOVE` capability from
UIDPLUS, see §8):

```cpp
auto copied = bth::sync_wait(std::move(selected).copy("1:5", "Archive"));
auto& [ec4, sel4] = *copied;
selected = std::move(sel4);

auto moved = bth::sync_wait(std::move(selected).move("1:5", "Archive"));
auto& [ec5, sel5] = *moved;  // ec5 == errc::capability_required if no MOVE
selected = std::move(sel5);
```

**Expunge** — permanently removes `\Deleted` messages. Remember that every
`EXPUNGE` (yours or the server's unsolicited one) renumbers the sequence
numbers after the removed message; `selected.mailbox()` tracks the new
EXISTS count automatically.

```cpp
auto expunged = bth::sync_wait(std::move(selected).expunge());
auto& [ec6, sel6] = *expunged;
```

**Close the mailbox** (CLOSE silently expunges and returns to the
authenticated state):

```cpp
auto closed = bth::sync_wait(std::move(selected).close());
auto& [ec7, authed_again] = *closed;
```

### 2.6 Waiting for new mail with IDLE

IDLE (RFC 2177) parks the connection: while it runs, no other command may be
sent on that session — which is exactly why it lives on the exclusive
state-machine layer. `selected_state::idle()` returns a sender that
completes when the server reports activity (an unsolicited `EXISTS` /
`EXPUNGE` arrives), when the heartbeat deadline approaches, or when the
operation is cancelled.

RFC 5550 recommends re-issuing IDLE at least every 29 minutes; bkmail ends
the operation at that point and hands the state back, so the idiom is a
loop:

```cpp
bexec::task<std::error_code> watch_inbox(im::selected_state<> selected) {
  std::uint32_t last_seen_exists = selected.mailbox().exists;
  for (;;) {
    auto [ec, back] = co_await bkmail::pack(std::move(selected).idle());
    selected = std::move(back);
    if (ec) co_return ec;

    // Woke up: either the 29-minute heartbeat point arrived (nothing new)
    // or the server pushed activity. mailbox() reflects the newest EXISTS.
    if (selected.mailbox().exists > last_seen_exists) {
      last_seen_exists = selected.mailbox().exists;
      // ... fetch the new envelopes here ...
      co_return {};
    }
    // Otherwise loop and re-issue IDLE.
  }
}
```

### 2.7 Cancellation with stop tokens

Operation senders take their stop token from the receiver's environment
(`bexec::get_stop_token`). `sync_wait`'s receiver carries a
`never_stop_token`, so blocking waits cannot be cancelled; use a receiver
whose env carries an `inplace_stop_token`. Requesting stop makes the
operation complete with `set_stopped()` (it never reports cancellation
through the error code). For `idle()` specifically, bkmail sends `DONE`
before completing, so the server stays in sync.

```cpp
class idle_printer {
 public:
  using env_type = bexec::env_with_stop_token<>;

  explicit idle_printer(bexec::inplace_stop_token token) : env_(token) {}

  env_type get_env() const noexcept { return env_; }

  void set_value(std::error_code ec,
                 im::selected_state<> state) noexcept {
    std::printf("idle ended, ec=%d, %u messages\n", ec.value(),
                state.mailbox().exists);
    // Keep `state` alive somewhere if the session should continue.
  }

  void set_stopped() noexcept { std::puts("idle cancelled (DONE sent)"); }

 private:
  env_type env_;
};

bexec::inplace_stop_source stop_src;
auto op = bexec::connect(std::move(selected).idle(),
                         idle_printer{stop_src.get_token()});
bexec::start(op);

// From any thread, e.g. on SIGINT:
stop_src.request_stop();
```

The same receiver pattern cancels any other operation; bkmail then stops
waiting for the tagged response and discards it when it arrives (IMAP cannot
un-send a command).

---

## 3. The command layer

### 3.1 When to use it

Reach for `imap_context` directly when you need to:

- run several commands **in parallel** (the state-machine layer forbids it);
- **batch** many commands and flush them in one write;
- send **custom or extension commands** the state layer does not model
  (via `raw_command`);
- control exactly **when** bytes hit the socket.

The price is manual protocol work: you order the commands yourself, consume
the greeting yourself, and interpret responses yourself.

### 3.2 Creating a context and borrowing an io_context

The context owns the stream; the `io_context` is only borrowed. Building the
stream is your job — here is the full TLS path with the primitives this
library is built on:

```cpp
#include <bkmail/bkmail.h>

#include <array>
#include <cstdio>
#include <thread>
#include <tuple>

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

int main() {
  bnio::io_context ioc;
  std::thread io_thread([&ioc] { (void)ioc.run(); });

  // Resolve the server name into caller-provided endpoint storage.
  std::array<bnio::ip::endpoint, 8> endpoints{};
  auto resolved = bth::sync_wait(ioc.get_post_scheduler().async_resolve(
      "imap.example.com", "993", bnio::dns_result_view{endpoints}));
  if (!resolved) return 1;
  auto& [resolve_ec, endpoint_count] = *resolved;
  if (resolve_ec || endpoint_count == 0) return 1;

  // Open and connect a TCP socket.
  bnio::tcp::socket sock;
  if (auto ec = sock.open(bnio::ip::tcp::v4())) return 1;
  auto connected = bth::sync_wait(
      sock.async_connect(ioc.get_post_scheduler(), endpoints[0]));
  if (!connected) return 1;
  auto& [connect_ec] = *connected;
  if (connect_ec) return 1;

  // Upgrade to TLS.
  bnio::ssl_context tls{bnio::ssl_context_method::tls_client};
  bnio::ssl_stream<bnio::tcp::socket> stream{std::move(sock), tls};
  auto shook = bth::sync_wait(stream.async_handshake(
      ioc.get_post_scheduler(), bnio::ssl_handshake_type::client));
  if (!shook) return 1;
  auto& [handshake_ec] = *shook;
  if (handshake_ec) return 1;

  // Hand the connected, handshaken stream to bkmail. The context starts its
  // permanent async_read loop immediately (on the borrowed ioc) and consumes
  // the greeting as an unsolicited event.
  im::imap_context<bnio::ssl_stream<bnio::tcp::socket>> ctx{std::move(stream),
                                                          ioc};

  // ... submit commands (see below) ...

  ioc.stop();
  io_thread.join();
}
```

For the plaintext path skip the handshake and pass the plain
`bnio::tcp::socket` (`im::imap_context<bnio::tcp::socket>`).

`set_io_context` re-points the context at a different `io_context` for the
*next* round of I/O — the one already in flight stays on the old context:

```cpp
ctx.set_io_context(other_ioc);  // call only with no operations in flight
```

See §7 for the exact rules.

### 3.3 Submitting commands: tags and handlers

Every IMAP command is a concrete type named `<noun>_command`, templated on
the same allocator as the context. `submit` stamps a tag on it
(`a0001`, `a0002`, ...), records the handler in the context's tag table,
appends the wire bytes to the outbox — and does **no I/O**. The returned tag
is your handle for correlation and cancellation:

```cpp
bkmail::account_info<> account{.user_name = "you@example.com",
                               .password = "app-specific-password"};

auto login_tag =
    ctx.submit(im::login_command<>{account},
               [](std::error_code ec) {
                 std::printf("login done, ec=%d\n", ec.value());
               });

// These two run concurrently once flushed; the server may answer them in
// any order. The context matches each tagged response to its handler.
auto caps_tag = ctx.submit(im::capability_command<>{},
                           [](std::error_code ec,
                              im::capability_set<> caps) {
                             if (!ec && caps.contains("IDLE")) { /* ... */ }
                           });

auto list_tag = ctx.submit(im::list_command<>{"", "*"},
                           [](std::error_code ec,
                              std::vector<im::mailbox_entry<>> boxes) {
                             for (const auto& b : boxes) {
                               std::printf("%s\n", b.name.c_str());
                             }
                           });

ctx.flush();  // one async_write for everything queued so far
```

A handler is invoked exactly once, on the thread running the borrowed
`io_context`, when the tagged completion (`OK` / `NO` / `BAD`) for its
command arrives. A `NO` is reported as `errc::command_rejected`, a `BAD` as
`errc::bad_command` — both arrive through the error code, never as
exceptions.

`ctx.cancel(tag)` withdraws a handler: the response is discarded when it
arrives, and — special case — cancelling a pending `idle_command` sends
`DONE` first. Cancellation never waits for the server.

Available command types and their handler result types:

| Command type | Wire form | `result_type` |
| --- | --- | --- |
| `login_command<A>` | `LOGIN` | `void` |
| `authenticate_command<A>` | `AUTHENTICATE` (e.g. `PLAIN`, `XOAUTH2`) | `void` |
| `capability_command<A>` | `CAPABILITY` | `capability_set<A>` |
| `noop_command<A>` | `NOOP` | `void` |
| `starttls_command<A>` | `STARTTLS` | `void` |
| `select_command<A>` / `examine_command<A>` | `SELECT` / `EXAMINE` | `mailbox_info<A>` |
| `create_command<A>` / `delete_command<A>` / `rename_command<A>` | `CREATE` / `DELETE` / `RENAME` | `void` |
| `subscribe_command<A>` / `unsubscribe_command<A>` | `SUBSCRIBE` / `UNSUBSCRIBE` | `void` |
| `list_command<A>` | `LIST` | `std::vector<mailbox_entry<A>>` |
| `status_command<A>` | `STATUS` | `mailbox_status<A>` |
| `append_command<A>` | `APPEND` (literal handshake; a second constructor takes a `const mail<A>&`) | `void` |
| `close_command<A>` | `CLOSE` | `void` |
| `fetch_command<A>` | `FETCH` over an explicit `fetch_items` selection | `std::vector<message_attributes<A>>` |
| `fetch_envelopes_command<A>` | `FETCH ... ENVELOPE` | `std::vector<envelope<A>>` |
| `fetch_headers_command<A>` | `FETCH ... BODY.PEEK[HEADER.FIELDS (...)]` | `std::vector<mail_header<A>>` |
| `fetch_message_command<A>` | `FETCH ... BODY.PEEK[]` | `std::vector<mail<A>>` |
| `search_command<A>` | `SEARCH` | `std::vector<std::uint32_t>` |
| `store_command<A>` | `STORE` | `void` |
| `copy_command<A>` / `move_command<A>` | `COPY` / `MOVE` (UIDPLUS) | `void` |
| `expunge_command<A>` | `EXPUNGE` | `void` |
| `idle_command<A>` | `IDLE` ... `DONE` | `void` |
| `logout_command<A>` | `LOGOUT` | `void` |
| `raw_command<A>` | any line you supply | `raw_response<A>` |

The message-addressing commands also exist in `uid_`-prefixed variants that
use the `UID` command form and treat the set argument as UIDs:
`uid_search_command`, `uid_fetch_command`, `uid_fetch_envelopes_command`,
`uid_fetch_headers_command`, `uid_fetch_message_command`, `uid_store_command`,
`uid_copy_command`, `uid_move_command`.

### 3.4 Type-erased batch submission

When the command set is not known at compile time (e.g. built from a work
queue), erase each command together with its handler and submit the batch at
once. `make_command` performs the erasure:

```cpp
std::vector<std::unique_ptr<im::imap_command<>>> batch;

batch.push_back(im::make_command(
    im::fetch_envelopes_command<>{"1:100"},
    [](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
      std::printf("first page: %zu envelopes\n", envs.size());
    }));

batch.push_back(im::make_command(
    im::fetch_envelopes_command<>{"101:200"},
    [](std::error_code ec, std::vector<bkmail::envelope<>> envs) {
      std::printf("second page: %zu envelopes\n", envs.size());
    }));

batch.push_back(im::make_command(
    im::noop_command<>{},
    [](std::error_code ec) { std::puts("noop ok"); }));

ctx.submit(std::move(batch));  // tags assigned in vector order
ctx.flush();                   // all three commands in one write
```

Each handler still fires independently when its own tagged response arrives;
batching only shares the tag space and the write syscall.

There is also a sender-flavoured overload for sender-composing callers:
`ctx.submit<Operation>(args...)` builds the command from `args` and returns
a lazy sender completing with `set_value(std::error_code[,
Operation::result_type])` / `set_stopped()`. Nothing is sent until the
sender is connected and started; the tag is allocated at `start()`, in wire
order, and the started sender kicks the write pump itself (co-arriving
starts still batch into one write), so no `flush()` call is needed on this
path.

### 3.5 Unsolicited events

Servers push messages that belong to no command: new-mail notifications,
expunges, flag changes, capability changes, and `BYE`. Register one handler
for all of them:

```cpp
ctx.on_unsolicited([](const im::unsolicited_event<>& event) {
  std::visit(
      [](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, im::exists_event>) {
          std::printf("now %u messages\n", e.count);
        } else if constexpr (std::is_same_v<T, im::expunge_event>) {
          std::printf("message %u expunged; later messages shift down\n",
                      e.sequence_number);
        } else if constexpr (std::is_same_v<T, im::flags_update_event>) {
          std::printf("message %u flags changed\n", e.sequence_number);
        } else if constexpr (std::is_same_v<T, im::bye_event<>>) {
          std::printf("server says bye: %s\n", e.text.c_str());
        }
      },
      event);
});
```

`im::unsolicited_event<A>` is a `std::variant` of seven alternatives:
`exists_event`, `recent_event`, `expunge_event`, `flags_update_event`,
`capability_event<A>`, `bye_event<A>` and `greeting_event<A>`. The greeting
surfaces through this handler too: an `OK`/`PREAUTH` greeting arrives as
`greeting_event` (`status`/`text`/`code` members), a `BYE` — including a
`BYE` greeting — as `bye_event`. Later untagged `OK`s (resp-code carriers)
also arrive as `greeting_event` and are ignorable by login flows.

### 3.6 When I/O actually happens

The context runs **one permanent `async_read`** from construction until
destruction or `BYE`. Incoming lines are parsed incrementally; when a tagged
completion line arrives, the matching handler is invoked from the read loop.

Writes are the mirror image: `submit` only appends bytes to an internal
vector. `flush()` concatenates the pending bytes and registers **one**
`async_write`; further submits accumulate until the next `flush()`. This is
what makes batching real — N commands cost one write syscall — and it means
*nothing you submit is sent until you flush*. A command handler that submits
follow-up commands should flush again when done.

---

## 4. Data types reference

All string members are `std::basic_string<char, std::char_traits<char>,
ReboundAlloc>` where `ReboundAlloc` is the type's allocator rebound to
`char`; byte members use the allocator directly. `<A>` below denotes the
trailing allocator parameter, defaulted everywhere. The protocol-neutral
types (`account_info`, `address`, `envelope`, `mail_header`, `mail_body`,
`mail`, `body_structure`) live in `namespace bkmail`; the IMAP-scoped types
(`mailbox_info`, `mailbox_entry`, `mailbox_status`, `message_attributes`,
`message_flag` / `flag_set` / `store_mode`, `capability_set`,
`raw_response`, `sequence_set`, `search_criteria`, `fetch_items`) live in
`namespace bkmail::imap` (`im::` below).

### `account_info<A>`

Credentials for `login` (`bkmail::`).

| Member | Type | Meaning |
| --- | --- | --- |
| `user_name` | string | IMAP user name |
| `password` | string | IMAP password |
| `authzid` | `std::optional<string>` | SASL authorization identity, when acting on behalf of another user |

### `address<A>`

One RFC 3501 `address` tuple (`bkmail::`). A wire `NIL` member parses to an
empty string.

| Member | Type | Meaning |
| --- | --- | --- |
| `display_name` | string | addr-name (may be empty) |
| `adl` | string | addr-adl (at-domain list / source route; obsolete, usually NIL) |
| `mailbox_name` | string | addr-mailbox (local part) |
| `host_name` | string | addr-host |
| `email()` | string | `"mailbox_name@host_name"` convenience accessor |

### `envelope<A>`

The ten ENVELOPE fields, in wire order.

| Member | Type | Member | Type |
| --- | --- | --- | --- |
| `date` | string | `to` | `std::vector<address<A>>` |
| `subject` | string | `cc` | `std::vector<address<A>>` |
| `from` | `std::vector<address<A>>` | `bcc` | `std::vector<address<A>>` |
| `sender` | `std::vector<address<A>>` | `in_reply_to` | string |
| `reply_to` | `std::vector<address<A>>` | `message_id` | string |

### `mail_header<A>`

Parsed `BODY.PEEK[HEADER.FIELDS (...)]` result.

| Member | Type | Meaning |
| --- | --- | --- |
| `subject` | string | Subject field |
| `from` / `to` / `cc` / `bcc` | `std::vector<address<A>>` | address lists |
| `date` | string | Date field, unparsed |
| `message_id` | string | Message-ID |
| `in_reply_to` | string | In-Reply-To |

### `mail_body<A>` / `mail<A>`

| Member | Type | Meaning |
| --- | --- | --- |
| `mail_body::content_type` | string | e.g. `"text/plain; charset=utf-8"` |
| `mail_body::data` | `std::vector<std::byte, A>` | decoded-or-raw body bytes |
| `mail::header` | `mail_header<A>` | parsed header |
| `mail::body` | `mail_body<A>` | body part |

### `body_structure<A>`

Recursive MIME BODYSTRUCTURE node (used when you issue a raw BODYSTRUCTURE
fetch through the command layer).

| Member | Type | Meaning |
| --- | --- | --- |
| `media_type` / `subtype` | string | e.g. `"text"` / `"html"` |
| `parameters` | `std::vector<std::pair<string, string>>` | attribute/value pairs |
| `encoding` | string | e.g. `"quoted-printable"`, `"base64"` |
| `octets` | `std::uint64_t` | body size in octets |
| `parts` | `std::vector<body_structure<A>>` | children for multipart types |

### `mailbox_info<A>`

Snapshot of a selected/examined mailbox; `selected_state::mailbox()` returns
a `const&` to one of these (`im::`).

| Member | Type | Meaning |
| --- | --- | --- |
| `name` | string | mailbox name |
| `exists` | `std::uint32_t` | message count (`* n EXISTS`) |
| `recent` | `std::uint32_t` | `\Recent` count |
| `unseen` | `std::optional<std::uint32_t>` | first unseen sequence number |
| `uid_validity` | `std::uint32_t` | UIDVALIDITY (32-bit) |
| `uid_next` | `std::uint32_t` | next predicted UID |
| `flags` | `flag_set` | flags defined in the mailbox |
| `permanent_flags` | `flag_set` | flags storable permanently |
| `read_only` | `bool` | true after `examine` or `READ-ONLY` select |

### `mailbox_status<A>`

Result of a STATUS command (`im::`). A STATUS response carries only the
requested items, so every field is optional: a disengaged field means the
item was not requested (or not returned), not that it is zero.

| Member | Type | Meaning |
| --- | --- | --- |
| `messages` | `std::optional<std::uint32_t>` | `MESSAGES` count |
| `recent` | `std::optional<std::uint32_t>` | `RECENT` count |
| `uid_next` | `std::optional<std::uint32_t>` | next predicted UID |
| `uid_validity` | `std::optional<std::uint32_t>` | UID validity value |
| `unseen` | `std::optional<std::uint32_t>` | count of messages without `\Seen` |

### `mailbox_entry<A>`

One LIST response line.

| Member | Type | Meaning |
| --- | --- | --- |
| `name` | string | mailbox name |
| `delimiter` | string | hierarchy delimiter |
| `no_select` / `has_children` / `has_no_children` | `bool` | common attributes |

### Flags

```cpp
enum class message_flag { seen, answered, flagged, deleted, draft, recent };

class flag_set {
 public:
  constexpr flag_set() noexcept;
  constexpr flag_set& set(message_flag f, bool value = true) noexcept;
  constexpr flag_set& reset(message_flag f) noexcept;
  [[nodiscard]] constexpr bool test(message_flag f) const noexcept;
  [[nodiscard]] constexpr bool any() const noexcept;
};

enum class store_mode { add, remove, replace };  // +FLAGS / -FLAGS / FLAGS
```

### `capability_set<A>` / `raw_response<A>`

```cpp
class capability_set {
 public:
  // Case-insensitive membership test, e.g. caps.contains("UIDPLUS").
  [[nodiscard]] bool contains(std::string_view name) const noexcept;
};

struct raw_response {
  string tag;     // the command's tag
  bool ok;        // tagged OK (false for NO/BAD)
  string text;    // response text after the status atom
};
```

Sequence sets and search criteria are passed as plain strings in RFC 3501
syntax: `"1:5"`, `"1,3,7:*"` for sequence sets; `"UNSEEN FROM \"bob\""` for
SEARCH.

---

## 5. Customizing allocators

Every owning type — states, `imap_context`, command results, the data types
in §4 — takes a trailing allocator template parameter. The allocator is
stored by value, propagated per `std::allocator_traits`, and flows into all
strings, vectors and the response storage the operation allocates.

Example: run a session on a `std::pmr::monotonic_buffer_resource` so all
per-command allocations come from one arena.

```cpp
#include <bkmail/bkmail.h>

#include <memory_resource>
#include <system_error>
#include <tuple>

namespace im = bkmail::imap;
namespace bth = bexec::this_thread;

void with_pmr(bnio::io_context& ioc, bnio::ssl_context& tls) {
  using alloc_t = std::pmr::polymorphic_allocator<std::byte>;

  std::pmr::monotonic_buffer_resource arena;

  // Template argument order: the allocator only. Branching senders are
  // consumed with sync_wait_with_variant regardless of the allocator (§2.4).
  auto connected = bth::sync_wait_with_variant(
      bkmail::async_connect_tls<alloc_t>("imap.example.com", "993", ioc, tls,
                                         alloc_t{&arena}));
  if (!connected) return;

  // State types carry the same allocator parameter:
  auto* not_authed = std::get_if<
      std::tuple<std::error_code, im::not_authenticated_state<alloc_t>>>(
      &*connected);
  if (not_authed == nullptr || std::get<0>(*not_authed)) return;

  bkmail::account_info<alloc_t> account{
      .user_name = "you@example.com", .password = "pw"};  // strings use arena
  auto logged_in = bth::sync_wait_with_variant(
      std::move(std::get<1>(*not_authed)).login(account));
  // ... every envelope/header/vector produced by these operations allocates
  // from `arena` ...
}
```

The same parameterization exists on the command layer
(`imap_context<Stream, alloc_t>`, `fetch_envelopes_command<alloc_t>`, ...).

Note that state types with different allocators are different types; do not
mix allocator instances across a session. The `io_context` itself is not
allocator-aware — it is borrowed, not owned.

---

## 6. Error handling

bkmail follows the bnio/bexec completion contract **with one deliberate
narrowing: there is no error channel**. Every failure is delivered as a
`std::error_code` through the *value* channel, always as the first value.
The stopped channel (`set_stopped`) is reserved for cancellation.

Consequences for the consumption styles:

- `sync_wait` never throws for an IMAP/network failure (it only throws what
  an error channel carries, and bkmail senders have none). Check the first
  tuple element.
- `sync_wait` returns `std::nullopt` on stopped completion.
- `co_await` inside a `bexec::task` behaves the same: errors come back as
  values; stopped completion marks the task stopped, surfacing as
  `std::nullopt` from `sync_wait(task)` or a `bexec::task_stopped` throw
  from `task::result()`.

Error code sources:

| Category | Produced by | Examples |
| --- | --- | --- |
| `std::generic_category()` / system category | TCP and DNS failures from bnio | `ECONNREFUSED`, `ETIMEDOUT`, `EPIPE` |
| `bnio::openssl_error_category()` | TLS handshake and record-layer failures | certificate verify failures, unexpected EOF |
| `bkmail::error_category()` | the IMAP protocol itself | see `bkmail::errc` below |

```cpp
enum class errc {
  command_rejected = 1,  // tagged NO  (bad credentials, no such mailbox, ...)
  bad_command,           // tagged BAD (protocol violation, usually a bug)
  server_bye,            // untagged BYE outside a logout exchange
  unexpected_response,   // unparseable or out-of-order server output
  capability_required,   // operation needs an extension the server lacks
};
```

`NO` and `BAD` are **recoverable**: the connection and the state object stay
valid, and you may issue the next command. The rest are **fatal to the
session**: the state/context must not be reused.

Reconnect policy:

- After `server_bye` or any transport error, discard the session, build a
  new stream, and reconnect from `async_connect` / `async_connect_tls`. IMAP
  has no resume facility; you must re-authenticate and re-SELECT.
- Use the `uid_validity` of the reopened mailbox to detect a server-side
  reset before trusting cached UIDs.
- An unsolicited `BYE` arrives as `errc::server_bye` on the next operation
  (state layer) or as a `bye_event` (command layer) followed by failing
  writes. Treat both as a signal to reconnect, not to retry the same
  command.

---

## 7. Threads and lifetime

The rules below are part of the API contract.

1. **The `io_context` is borrowed, never owned.** Both layers store a
   reference. The caller keeps the `io_context` — and a thread running its
   `run()` — alive for as long as any operation may be in flight. A sender
   that is started while nobody runs the context simply never completes.
2. **In-flight operations pin their `io_context`.** `set_io_context` (and
   the state layer's equivalent) only chooses the context for the *next*
   round of I/O. Calling it while a read or write is registered is
   prohibited; drain or cancel outstanding work first.
3. **Callback threads may change.** Handlers and receiver completions run on
   whatever thread runs the borrowed context, and after `set_io_context`
   that can be a different thread (or pool) than before. Do not rely on
   thread affinity; protect shared state with your own synchronization.
4. **Objects must outlive their operations.** The state handle (or
   `imap_context`), the `io_context`, the `ssl_context`, and any receiver /
   handler closure must all stay alive until the operation completes.
   Destroying an `imap_context` is itself the clean way to end a session:
   pending handlers are dropped, not invoked.
5. **The command queue is not thread-safe by contract.** `submit`, `flush`,
   `cancel` and `set_io_context` are designed for single-threaded use (the
   internal tag table takes a lock, but that is an implementation detail you
   must not rely on). If several threads produce commands, serialize them
   yourself — for example by posting the submit call onto the owning
   `io_context`.
6. **One state-machine session = one logical thread of commands.** The
   rvalue-consuming design makes it impossible to start a second operation
   before the previous one delivered the state back. If you need
   parallelism, use the command layer.

---

## 8. Compatibility notes

**Baseline.** bkmail speaks RFC 3501 (IMAP4rev1): the four states, tagged +
untagged responses, the greeting forms (`OK`, `PREAUTH`, `BYE`), SELECT
semantics including EXISTS/FLAGS/UIDVALIDITY reporting, and the FETCH items
used by the state layer (`ENVELOPE`, `BODYSTRUCTURE`,
`BODY.PEEK[HEADER.FIELDS (...)]`, `FLAGS`, `UID`). UIDs and UIDVALIDITY are
32-bit values; `uid_`-prefixed operations issue the `UID` command forms.

**Extensions.** After connecting, bkmail issues `CAPABILITY` and adjusts its
behavior; everything it relies on beyond the baseline is listed here:

| Capability | Used for | Behavior when absent |
| --- | --- | --- |
| `SASL-IR` | one-round-trip `AUTHENTICATE` | the two-step challenge form is always used today — SASL-IR is a **planned enhancement** (the command type models it; the state layer does not enable it yet) |
| `LITERAL+` | non-synchronizing literals in LOGIN/custom commands | falls back to synchronizing literals |
| `IDLE` | `selected_state::idle()` / `idle_command` | `idle()` fails immediately with `errc::capability_required` |
| `UIDPLUS` (`MOVE`) | `move()` / `move_command` | `move` fails with `errc::capability_required`; use `copy` + `store(\Deleted)` + `expunge` |
| `ENABLE` | extension negotiation | unused; reserved |
| `NAMESPACE` | mailbox prefix discovery | not required; `list("", "*")` works without it |

You can query the negotiated set yourself with `capability()` (state layer)
or `capability_command` (command layer) and gate your own `raw_command`
extensions on it.

**Deliberately unimplemented.** `CHECK`, `LSUB`, `ENABLE` and `NAMESPACE`
have no command types (use `raw_command` if you ever need them);
`move()`/`uid_move()` are gated on the `MOVE` capability and fail locally
with `errc::capability_required` when the server does not advertise it.

**Authentication.** Gmail, Outlook and most large providers authenticate
with XOAUTH2: obtain an OAuth2 access token out of band, then

```cpp
auto res = bth::sync_wait(
    std::move(not_authed).authenticate_oauth2("you@gmail.com", access_token));
```

`AUTHENTICATE PLAIN` over TLS is the interoperable fallback — drive it with
`authenticate("PLAIN", initial_response)`; `login()` issues the plain
`LOGIN` command, which is fine over TLS but best avoided on untrusted
plaintext networks.
