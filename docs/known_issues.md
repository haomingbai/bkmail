# bkmail Known Issues

This document is the register of accepted limitations: gaps and
contract-restricted corners the project knows about and has chosen not to
fix yet. Each entry states the issue, why it is accepted, and what should
trigger a revisit. Nothing here is hidden behavior — every entry is a
recorded decision to defer. Usage questions belong in
[`docs/usage.md`](usage.md); the design context is
[`docs/architecture.md`](architecture.md).

---

## 1. `connect.h` is a real-content top-level header

**Statement.** `include/bkmail/connect.h` carries ~450 lines of
implementation, including a third-party `detail` include, while the
umbrella-header rule (`docs/code_style.md`, "File organization") says a
top-level header is umbrella-only.

**Why accepted.** Moving the implementation to `imap/connect.h` is
mechanical, but it churns the include graph of every consumer that spells
`<bkmail/connect.h>` directly.

**Revisit.** When the connect surface next changes.

## 2. `detail::registration` is a public-API type in a detail header

**Statement.** `imap::detail::registration<Allocator>` — the token
returned by `imap_context::on_unsolicited` — is public API, but it lives
in `imap/detail/unsolicited_table.h`, forcing public headers to include
`detail` paths.

**Why accepted.** Renaming or moving the type is an ABI-visible include
restructure.

**Revisit.** Together with the `connect.h` move (entry 1), so the include
churn is paid once.

## 3. Executor and transport coupling

**Statement.** The abstraction is coupled to one executor and one
transport family: `context_core` hardcodes
`bnio::io_context::post_scheduler`; the `imap_stream` concept fixes the
bnio buffer/scheduler types and `lowest_layer()` to
`bnio::tcp::socket&`; the pumps pass `MSG_NOSIGNAL` and use `SHUT_RD`.
A fully executor-agnostic design (an executor concept, a timer factory,
and `shutdown_read`/`close` in the stream concept) has been scoped but is
deferred. A visible cost today: the test double
`tests/support/scripted_stream.h` carries a real socketpair fd solely to
satisfy `lowest_layer()`.

**Why accepted.** There is exactly one executor and one transport in the
tree; generalizing now would pay abstraction rent for zero users.

**Revisit.** When a second executor or transport actually appears.

## 4. Layer-2 completion lifetime window

**Statement.** A Layer-2 completion "loser" that is preempted before its
guard exchange can, in a narrow window, touch an operation state already
destroyed by the winner's downstream. Closing it completely requires
heap-boxed Layer-2 completion records. The posted stop-delivery change
(architecture.md §6: receivers are completed only from the `io_context`'s
dispatch paths) narrows the window but does not eliminate it.

**Why accepted.** The window requires a preempted thread between the
completion hand-off and the guard exchange; the cost of boxing every
Layer-2 completion record is disproportionate to that probability.

**Revisit.** Any ASan/TSan report near `state_op_operation` / `on_result`.

## 5. `unsolicited_table` concurrency is contract-restricted

**Statement.** Concurrent registration or token destruction against
dispatch remains contract-restricted rather than fully synchronized; the
restriction is documented in the header
(`imap/detail/unsolicited_table.h`).

**Why accepted.** All internal users comply: registration happens from
the owning thread, dispatch from the read path.

**Revisit.** If the table's users ever grow beyond the internal
read-dispatch callers, or concurrent registration becomes a supported
pattern.

## 6. Documented preconditions without runtime enforcement

**Statement.** Some preconditions are documented but not checked at
runtime — most prominently "no `set_io_context` during IDLE"
(architecture.md §3.6, §9), along with related lifetime and exclusivity
preconditions.

**Why accepted.** The checks would add cost and code paths on the hot
completion paths for violations the API shape already makes unlikely.

**Revisit.** When a debug-mode contract-check pass (asserts behind a
build flag) is added to the library.

## 7. The 29-minute IDLE heartbeat wake path is untested

**Statement.** The wake-on-heartbeat path of `selected_state::idle()`
(RFC 5550's 29-minute re-issue point, usage.md §2.6) is not covered by
the test suite — the timescale cannot be exercised in CI wall-clock time.
Cancellation of IDLE, including the DONE-first path, has deterministic
coverage in `tests/layer1`.

**Why accepted.** The heartbeat shares its timer machinery with paths
that are tested; only the deadline value is untested.

**Revisit.** When the tests grow an adjustable/mock clock, or when the
heartbeat wiring next changes.
