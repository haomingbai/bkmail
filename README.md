# bkmail

**bkmail** is a C++20 IMAP client library (RFC 3501) built on top of
[bnio](https://github.com/haomingbai/bnio) (async I/O) and
[bexec](https://github.com/haomingbai/bexec) (sender/receiver concurrency).

## Features

- **Two API layers.** A type-safe session state machine
  (`not_authenticated_state` → `authenticated_state` → `selected_state` →
  `logout_state`) for the common sequential conversation, and a command
  layer (`imap_context` plus one C++ type per IMAP command) for pipelining,
  batched writes, and extension commands.
- **Sender/receiver throughout.** Every operation returns a lazy bexec
  sender. Branching operations (connect / login / select) publish **one
  `set_value` signature per outcome state** — never a merged variant
  payload — and consumers pick their style:
  `bexec::this_thread::sync_wait`, `sync_wait_with_variant`,
  `co_await bexec::into_variant(...)` / `bkmail::pack(...)` inside a
  `bexec::task`, or a hand-written receiver.
- **Errors on the value channel.** No error channel, no exceptions: every
  failure arrives as a leading `std::error_code`; `set_stopped()` is
  reserved for cancellation.
- **Batching and pipelining.** `make_command` type erasure plus one
  `flush()` sends a whole batch in a single write; commands pipeline freely
  except across literal/SASL/STARTTLS/IDLE walls.
- **Unsolicited events.** One `on_unsolicited` handler receives server
  pushes (EXISTS / RECENT / EXPUNGE / flag updates / CAPABILITY / BYE /
  greeting) as a typed seven-alternative variant.
- **IDLE (RFC 2177).** `selected_state::idle()` drives one full IDLE/DONE
  cycle with the RFC 5550 29-minute heartbeat point built in; cancelling
  sends `DONE` first so the server stays in sync.
- **Cancellation.** Stop tokens honoured end to end, with queue-removal /
  drop-on-arrival / DONE-first granularity at the command layer.
- **TLS both ways.** Implicit TLS (`async_connect_tls`) and STARTTLS
  upgrade (`not_authenticated_state::start_tls`) with the RFC-required
  CAPABILITY re-probe after the upgrade.
- **Allocator-aware.** Every owning type takes a trailing
  `Allocator = std::allocator<std::byte>`; a whole session can run on a
  `std::pmr` arena.
- **Tested.** 258 ctest cases covering the data model, the protocol
  framer/parser, the command layer (against a scripted in-memory stream),
  the state machine, and loopback integration tests against a scripted
  fake IMAP server — none of them needs the network. Line coverage of the
  library sources stands at **82.5%** (configure with
  `-DBKMAIL_ENABLE_COVERAGE=ON`, run `ctest`, then collect the gcov data
  with `llvm-cov gcov`; known gaps: `authenticate`/`uid_search` command
  paths and the STARTTLS flow, which are exercised only against a live
  server). For a live read-only end-to-end session see
  `examples/real_server_session`.

## Documentation

- [`docs/usage.md`](docs/usage.md) — user guide and API reference; start
  here.
- [`docs/architecture.md`](docs/architecture.md) — the design as
  implemented, layer by layer.
- [`docs/code_layout.md`](docs/code_layout.md) — file tree, naming master
  table, CMake plan.
- [`docs/code_style.md`](docs/code_style.md) — naming and formatting rules.
- [`examples/`](examples/) — runnable programs: `list_subjects` (blocking
  state-machine flow), `idle_watch` (IDLE loop with SIGINT cancellation),
  `batch_commands` (command-layer batching), `hello_bkmail`.

---

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Dependencies

bkmail depends on `bexec` and `bnio`. `bnio` itself depends on `bexec`, and
bkmail resolves `bexec` *before* `bnio`; every resolver short-circuits when
the target already exists, so `bexec` is never introduced twice.

Each dependency is resolved through a dedicated provider:

| Variable | Values | Default behavior |
| --- | --- | --- |
| `BKMAIL_BEXEC_PROVIDER` | `AUTO`, `FIND_PACKAGE`, `SOURCE`, `FETCH` | `AUTO` |
| `BKMAIL_BNIO_PROVIDER` | `AUTO`, `FIND_PACKAGE`, `SOURCE`, `FETCH` | `AUTO` |

- `AUTO` — use `BKMAIL_<DEP>_SOURCE_DIR` if set; otherwise try
  `find_package`, and fall back to FetchContent from GitHub.
- `FIND_PACKAGE` — locate an installed package (`bexec`/`bnio`).
- `SOURCE` — add a local source tree from `BKMAIL_<DEP>_SOURCE_DIR`.
- `FETCH` — download from GitHub; `BKMAIL_BEXEC_GIT_TAG` defaults to
  `v0.1.0` and `BKMAIL_BNIO_GIT_TAG` to `v0.2.0`.

Example — resolve both dependencies from GitHub:

```sh
cmake -S . -B build \
  -DBKMAIL_BEXEC_PROVIDER=FETCH \
  -DBKMAIL_BNIO_PROVIDER=FETCH
```

Example — resolve from local checkouts:

```sh
cmake -S . -B build \
  -DBKMAIL_BEXEC_PROVIDER=SOURCE -DBKMAIL_BEXEC_SOURCE_DIR=../bexec \
  -DBKMAIL_BNIO_PROVIDER=SOURCE -DBKMAIL_BNIO_SOURCE_DIR=../bnio
```

## Consuming bkmail

- **`add_subdirectory`** — drop the checkout into your tree and link
  `bkmail::bkmail`.
- **`find_package(bkmail CONFIG REQUIRED)`** — after `cmake --install`;
  the package config pulls in `bnio` and `bexec` automatically (skipping
  any that are already provided as targets).
- **`FetchContent`** — fetch bkmail from GitHub; its dependency resolvers
  then bring in `bnio` and `bexec`.
- **pkg-config** — `pkg-config --cflags --libs bkmail`.
