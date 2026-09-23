# bkmail Architecture

This document describes bkmail as implemented, a C++20 IMAP client
library built on [bnio](https://github.com/haomingbai/bnio) (async I/O) and
[bexec](https://github.com/haomingbai/bexec) (sender/receiver vocabulary).
Every public type is specified down to its name, template parameters, and
method signatures. Code organization rules (one type per file, ~400-line
soft limit, banner, naming) live in [`docs/code_style.md`](code_style.md)
and are not repeated here; header paths named here are the real ones —
[`docs/code_layout.md`](code_layout.md) keeps the authoritative file tree.

The protocol baseline is RFC 3501 (IMAP4rev1) with extension discovery via
`CAPABILITY`; the client never issues `ENABLE`, so an IMAP4rev2 server is
spoken to in rev1 syntax.

## 1. Overview

bkmail has two abstraction layers over bnio/bexec, plus a protocol-neutral
mail data model:

```
+------------------------------------------------------------------+
|                     application / user code                      |
+------------------------------------------------------------------+
| Layer 2 — session state machine            (namespace bkmail::imap) |
|   not_authenticated_state   authenticated_state   selected_state |
|   logout_state              imap_connection                       |
|   StateA::operationA(args...) &&  ->  Sender                     |
|   completion: one set_value signature per successor state —      |
|   set_value(std::error_code, [result,] next_state), never a      |
|   merged variant payload (§5.4)                                  |
|   strictly serial: one operation in flight per connection        |
+------------------------------------------------------------------+
| Layer 1 — command / connection layer       (namespace bkmail::imap) |
|   imap_context<Stream, Allocator>                                |
|     tag allocator + unordered_map<tag, operation_base*> registry |
|     write queue (vector<unique_ptr<operation_base>>) + write pump|
|     permanent read pump + two-mode response lexer [self-built]   |
|     unsolicited-response handler table                           |
|   one C++ operation type per IMAP command, type-erased for the   |
|   queue; pipelining (multiple commands in flight) is supported   |
+------------------------------------------------------------------+
| Mail data model                            (namespace bkmail)    |
|   account_info  mail  mail_header  mail_body  envelope  address  |
|   body_structure                                               |
+------------------------------------------------------------------+
| bnio v0.2.0: io_context, tcp::socket, ssl_stream, steady_timer,  |
|              dynamic_*_buffer;  Linux = io_uring, macOS = kqueue |
| bexec v0.1.0: sender/receiver vocabulary, adaptors, run_loop,    |
|               task<T>, stop tokens (header-only)                 |
+------------------------------------------------------------------+
```

Dependency direction: **bkmail → bnio → bexec**. bnio's asynchronous
operations return bexec senders (see
`include/bnio/detail/tcp/async_operations.h`: `tcp_read_sender` declares
`bexec::completion_signatures` and connects via `bexec::connect`), so every
I/O primitive in bkmail composes with bexec adaptors (`then`, `let_value`,
`when_all`, `starts_on`, `repeat_until`, …) and with `bexec::task<T>`
coroutines without adaptation. bkmail adds no third async model.

**Layer 1** wraps the wire: it owns the socket, frames and parses server
bytes, assigns command tags, multiplexes in-flight commands, batches
outgoing writes, and dispatches unsolicited responses. It is intentionally
"close to the protocol": callers may submit several commands concurrently
(pipelining) and receive raw-ish structured responses.

**Layer 2** wraps the session: the four RFC 3501 states are C++ types, each
exposing only the commands legal in that state, each command returning a
sender that completes with an error code and the *next* state. Layer 2 is
strictly serial by construction (§5).

**Mail data model** types (`mail`, `mail_header`, `envelope`,
`body_structure`, …) are plain value types shared by both layers and,
later, by other protocols (SMTP submission). MIME semantics (RFC 2047
encoded-words, Content-Transfer-Encoding, charset decoding) live here, never
in the protocol parser.

## 2. Dependency contracts that shape the design

These are facts about bnio v0.2.0 / bexec v0.1.0 that the design treats as
hard constraints. Each is cited to its evidence.

1. **Completion contract: no `set_error` channel.** Every bnio operation
   completes with `set_value(std::error_code[, std::size_t])` or
   `set_stopped()` (`tcp_read_sender::completion_signatures`,
   `include/bnio/detail/tcp/async_operations.h:115`; SSL senders,
   `include/bnio/detail/ssl/async_operations/senders.h:28,52,78,105`;
   scheduler senders, `io_context::schedule_sender`,
   `include/bnio/detail/posix/io_context/class.h:148`). Errors therefore
   travel on the **value** channel as `std::error_code`. bkmail adopts the
   same contract at every layer (§3.8, §5.4). This overrides the generic
   `set_error` remark in `code_style.md` for this library: "the bnio/bexec
   sender model" that the style guide defers to is, in bnio, the
   value-channel error model.
2. **Scheduler per call.** bnio sockets take a scheduler argument on every
   `async_*` call (`socket::async_read_some(Scheduler, Buffer&&, int)`,
   `include/bnio/tcp/socket.h:132-157`); sockets are not bound to a context.
   "Which `io_context` runs the next I/O" is a per-call decision — this is
   what makes runtime `io_context` borrowing possible (§3.6).
3. **`async_read` is read-all; `async_read_some` is the bounded single
   read.** The two differ only by a `bool` template parameter selecting the
   scheduler call (`make_child_sender`,
   `include/bnio/detail/tcp/async_operations.h:34-42`). The permanent read
   loop must use `async_read_some`; `n == 0` signals EOF.
4. **No `async_read_until`, no delimiter framing.** bnio offers only bounded
   reads plus dynamic buffers (`dynamic_string_buffer` /
   `dynamic_byte_vector_buffer<Allocator>` with `prepare`/`commit`/`consume`,
   `include/bnio/buffer/dynamic_string.h`, `dynamic_byte_vector.h`). IMAP
   line/literal framing is **self-built** (§3.5).
5. **Buffers are non-owning pointer+size views captured at sender
   creation.** A bnio sender holds the buffer address, not the storage
   (holders, `include/bnio/buffer/holders.h`). Bytes referenced by an
   in-flight write must not move or be destroyed; the write staging buffer
   may not be reallocated mid-write (§3.4).
6. **Cancellation:** receiver stop tokens are observed only at start/post
   points (e.g. `run_loop_schedule_sender::operation::execute`,
   `include/bexec/run_loop.hpp:315-323`); an already-armed read cannot be
   retracted by a token. Directed read cancellation is
   `socket::shutdown(SHUT_RD)`; connection-level teardown is
   `socket::close()` / `io_context::stop()`; watchdogs use
   `bnio::steady_timer` (§3.7).
7. **`steady_timer` binds its context at construction**
   (`steady_timer(io_context&)`,
   `include/bnio/detail/posix/io_context/steady_timer.h:45`) and cannot be
   re-pointed at another context. Timers must be recreated after an
   `io_context` switch (§3.6, §9).
8. **`io_context` is immovable** and multi-thread `run()` is first-class
   (`include/bnio/detail/posix/io_context/class.h:105,566-599`). Completion
   handlers run on whichever worker thread of the *initiating* context
   drains them; two completions of one connection may run on different
   threads (§6).
9. **No `strand`.** bnio provides no serialization adapter. bkmail
   compensates internally (§6) without promising public thread safety.
10. **No `any_sender` / `move_only_function` in bexec.** Type erasure is
    self-built, modeled on bexec's intrusive `run_loop_operation_base`
    (`virtual void execute() noexcept`, `include/bexec/run_loop.hpp:29-33`);
    small move-only function wrappers are likewise self-built (§3.3).
11. **Stop-token infrastructure:** `bexec::inplace_stop_source/token/
    callback` are intrusive and allocation-free
    (`include/bexec/stop_token.hpp`); custom senders obtain the token via
    `bexec::query(bexec::get_env(receiver), bexec::get_stop_token)`
    (`include/bexec/query.hpp`).
12. **Allocator query convention:** `bexec::get_allocator(env)` exists and
    falls back to `std::allocator<std::byte>`
    (`include/bexec/query.hpp:44-53`). bnio async operations do not accept
    allocators; allocator-awareness above the buffer layer is bkmail's own
    (§7).
13. **TLS:** `bnio::ssl_stream<NextLayer = tcp_socket>` owns its next layer
    by value (`include/bnio/ssl/stream_class.h:28-37`), which makes
    STARTTLS a *move the socket into a new stream* operation. The
    `bnio::ssl_context` must outlive the `ssl_stream`. Handshake:
    `async_handshake(sched, ssl_handshake_type::client)`. TLS errors use
    `bnio::openssl_error_category()` (`include/bnio/ssl/context.h:55`).
14. **SIGPIPE:** bnio never sets `MSG_NOSIGNAL` internally; its own
    examples pass it at every write call site. All bkmail writes pass
    `MSG_NOSIGNAL` (§3.4).

IMAP protocol facts the design relies on (RFC 3501 unless noted):

- Four states: Not Authenticated, Authenticated, Selected, Logout. The
  greeting (`OK` / `PREAUTH` / `BYE`) selects the initial state. Untagged
  `BYE` may arrive at any time and is connection-fatal.
- One command yields 0..n continuation/untagged responses and exactly one
  tagged reply (`OK`/`NO`/`BAD` with optional `[resp-code]`). Unknown
  response codes must be skipped without failing the parse.
- The receiver is always in one of two read modes: read-a-line (CRLF) or
  read-exactly-*n*-bytes-then-a-line; a literal `{n}` at end of line
  switches modes. After sending `{n}` the client must stop and wait for a
  continuation request; with `LITERAL+`, `{n+}` may proceed without
  waiting.
- Quoted strings escape only `\"` and `\\`. Tag characters are ASTRING-CHAR
  minus `+`; bkmail uses `a` + a zero-padded monotonically increasing
  decimal counter (`a0001`, ...).
- §5.5 restricts pipelining around ambiguous/sequencing-sensitive commands;
  bkmail's policy: never pipeline across literals, SASL, STARTTLS, or IDLE
  (§3.4, §10). For IDLE the Layer-1 wall is the `+ idling` continuation
  wait only; once the server is idling, keeping the connection exclusive
  is the command-layer caller's responsibility (documented on
  `idle_command`), while the state layer enforces it by construction.
- Unsolicited `EXISTS`, `RECENT`, `EXPUNGE`, `FETCH (FLAGS)`, `BYE` may
  arrive while no command is in flight (`EXPUNGE` excepted); `EXPUNGE`
  renumbers all later sequence numbers.
- IDLE (RFC 2177) monopolizes the connection, ends with `DONE`, and must be
  re-issued at least every 29 minutes.
- `ENVELOPE` has exactly 10 fields; an address is the 4-tuple
  `(name, adl, mailbox, host)`; `UID`/`UIDVALIDITY` are 32-bit; sizes and
  literal counts are 64-bit.

## 3. Layer 1 — command / connection layer

Everything in this section lives in `namespace bkmail::imap` unless noted;
implementation details additionally nest under `detail`.

### 3.1 Stream concept and socket unification

bnio async methods are templates and therefore cannot be virtualized; the
plain-TCP vs TLS choice is made at compile time. Both candidates expose an
identical async surface, captured by a concept:

```cpp
namespace bkmail::imap::detail {

/// [self-built] Stream requirements for imap_context.
/// Modeled by bnio::tcp::socket and bnio::ssl_stream<bnio::tcp::socket>.
template <class S>
concept imap_stream = requires(S& s, bnio::io_context::post_scheduler sched,
                               bnio::mutable_buffer mb, bnio::const_buffer cb) {
  { s.async_read_some(sched, mb, 0) };   // sender: set_value(ec, size_t) / set_stopped
  { s.async_write(sched, cb, 0) };       // write-all sender
  { s.lowest_layer() } -> std::same_as<bnio::tcp::socket&>;
  // plus move-constructible, RAII close
};

}  // namespace bkmail::imap::detail
```

`lowest_layer()` (present on both `bnio::tcp::socket` and
`bnio::ssl_stream`, see `include/bnio/tcp/socket.h:121` and
`include/bnio/ssl/stream_class.h:92`) gives access to `shutdown()` /
`close()` for cancellation (§3.7) regardless of the encryption layer.

STARTTLS crosses the type boundary at runtime by *moving* the socket into a
new context object; see §8.

### 3.2 `imap_context`

`imap_context` is the central Layer-1 object: it owns the stream (the "I/O
credential"), allocates tags, registers operations, runs the read/write
pumps, and dispatches responses.

```cpp
namespace bkmail::imap {

template <detail::imap_stream Stream,
          class Allocator = std::allocator<std::byte>>
class imap_context {
 public:
  using stream_type      = Stream;
  using allocator_type   = Allocator;
  using scheduler_type   = bnio::io_context::post_scheduler;
  using string_type      = /* basic_string<char> over rebound Allocator */;

  /// The context borrows @p ioc (never owns it) and owns @p stream, and
  /// arms the permanent read pump immediately (code_layout D5): a context
  /// whose read pump is not running is a half-object. The greeting is
  /// consumed through the unsolicited path.
  /// @pre stream is already connected; @pre ioc outlives the context and
  /// has a thread inside run().
  /// If arming the first read fails (allocation failure), the context is
  /// constructed closed: is_alive() reports false.
  imap_context(Stream stream, bnio::io_context& ioc,
               const Allocator& alloc = Allocator{});

  /// Non-copyable. Move-CONSTRUCTIBLE AT ANY TIME: the shell only carries
  /// a shared_ptr to the heap-held state (context_core, below), so a move
  /// relocates no I/O state — legal even with operations in flight.
  /// Move-assign is deleted; a moved-from shell owns no core and its
  /// destruction is a no-op.
  imap_context(const imap_context&) = delete;
  imap_context& operator=(const imap_context&) = delete;
  imap_context(imap_context&&) noexcept;
  imap_context& operator=(imap_context&&) = delete;

  /// Ends the session AT ANY TIME (usage.md §7): pending handlers are
  /// dropped, not invoked. The shell's destruction only *abandons* the
  /// core; the core self-destructs once both pumps have gone quiet and
  /// the last in-flight pump operation has retired. Never blocks.
  ~imap_context();

  // ---- submission --------------------------------------------------

  /// Callback path: stamps a tag, queues the command bytes WITHOUT
  /// doing I/O, and returns the tag. The handler is invoked from the
  /// read loop as handler(ec) / handler(ec, result). A tagged NO reports
  /// errc::command_rejected, BAD reports errc::bad_command (D2).
  /// Call flush() to write the batch.
  template <class Command, class F>
  string_type submit(Command command, F&& handler);

  /// Sender path: builds an Operation from args and returns a lazy
  /// sender completing with set_value(std::error_code[,
  /// Operation::result_type]) / set_stopped(). Nothing is sent until the
  /// sender is connected and started; the tag is allocated at start(),
  /// in wire order.
  template <class Operation, class... Args>
  [[nodiscard]] auto submit(Args&&... args);

  /// Erased batch path: submits pre-built command cells (see
  /// make_command), returning the stamped tags in vector order.
  /// No I/O until flush().
  std::vector<string_type, /*rebound*/>
  submit(std::vector<std::unique_ptr<imap_command<Allocator>>> commands);

  /// Performs one batched async_write of the pending queue (the write is
  /// staged and armed through the scheduler, so submissions already
  /// queued when the worker runs share the syscall).
  void flush();

  /// Cancels one operation (code_layout D3, §3.7). Cancel a tag at
  /// most once:
  ///  - queued, never on the wire: removed from queue and registry; the
  ///    completion carries stop semantics — no bytes ever hit the wire;
  ///  - once ANY byte was staged to the wire the operation can no
  ///    longer be extracted (the write queue keeps it): a written
  ///    idle_command re-enters done() (idempotent) so DONE is queued,
  ///    every other operation is rendered detached and its tagged reply
  ///    is dropped on arrival; the connection stays usable. DONE
  ///    delivery to the wire is asynchronous and requires a live
  ///    io_context.
  /// The completion runs on the context's dispatch path, never inline
  /// (one documented fallback: when allocating the scheduler box fails,
  /// context_core::post_complete_stopped completes inline as a last
  /// resort, §6).
  /// On the CALLBACK path a cancelled operation never invokes its
  /// handler at all ("cancel withdraws a handler", usage.md §3.3).
  void cancel(std::string_view tag);

  // ---- unsolicited responses ----------------------------------------

  /// Registers @p f for unsolicited events: one handler receives the
  /// whole unsolicited_event variant (§3.5). Handlers run inline on the
  /// read-dispatch path on an arbitrary worker thread (§6); they must
  /// not block and must not call back into the context. Destroying the
  /// returned token unregisters; tokens must not outlive the context.
  template <class F>
  [[nodiscard]] detail::registration<Allocator> on_unsolicited(F&& f);

  // ---- io_context borrowing ------------------------------------------

  /// Points the context's *next* I/O at @p ioc. In-flight operations
  /// stay with the old context. Rules in §3.6.
  void set_io_context(bnio::io_context& ioc) noexcept;
  [[nodiscard]] bnio::io_context& io_context() const noexcept;

  // ---- lifecycle ------------------------------------------------------

  [[nodiscard]] bool is_alive() const noexcept;   // read pump running
  void close() noexcept;                          // §3.7 close protocol

  /// Allocates the next command tag: "a" + zero-padded decimal counter
  /// ("a0001", ...). Exposed for tests; submit() calls it internally.
  [[nodiscard]] string_type allocate_tag();

  /// The allocator every internal container is configured with.
  [[nodiscard]] allocator_type get_allocator() const noexcept;

 private:
  // The shell owns no session state directly: everything lives in a
  // heap-allocated detail::context_core<Stream, Allocator>, shared with
  // the in-flight pump operations through shared_ptr (§3.2a).
  std::shared_ptr<detail::context_core<Stream, Allocator>> core_;
};

}  // namespace bkmail::imap
```

### 3.2a Shell/core split — why destruction is always safe

The whole object state (stream, tag counter, registry, write queue and
staging, read buffer, lexer, parser, unsolicited table, phase flags) lives
in `detail::context_core`, a heap object held as
`std::shared_ptr<context_core>` by the shell and by every self-owning
pump operation box (`detail::io_box`, §3.3). Consequences:

- **Destruction at any time.** `~imap_context()` runs the core's
  *abandonment* protocol: submissions stop, the read side is shut down,
  and every queued handler is DROPPED — destroyed without being invoked.
  The core itself stays alive until both pumps are quiet and every pump
  box has retired; the boxes hold the shared_ptrs that keep it (and with
  it the stream and the buffers the armed I/O references) alive.
- **Move at any time.** Only the shared ownership travels; the underlying
  state keeps its stable address, so a move is legal even with operations
  in flight. A moved-from shell owns no core; destroying it is a no-op.
- The abandoned core switches the pump completion paths into their quiet
  teardown forms (`abandoned_` flag): no dispatch, no handler invocation,
  only pump bookkeeping and the fd close.

Internal state of `context_core` (member names indicative; all
allocator-rebound):

```cpp
  // Coherence lock (mutable std::recursive_mutex: the arm paths
  // re-enter). Presence is documented here so implementers know
  // the invariant set; bkmail makes NO public thread-safety promise
  // about concurrent API calls on one context (§6).
  mutable std::recursive_mutex mutex_;

  // borrowed io_context, held through the post-scheduler handle; the
  // generic template channel io_context_of(sched) is the way back to
  // the io_context (§3.6)
  bnio::io_context::post_scheduler scheduler_;
  Stream stream_;                // owned I/O credential
  Allocator alloc_;

  std::uint64_t tag_counter_ = 1;

  // tag -> in-flight operation (non-owning; ownership lives in the
  // write queue)
  std::unordered_map<string_type, detail::operation_base<Allocator>*,
                     detail::string_hash, std::equal_to<>,
                     rebind_alloc<pair<const string_type, detail::operation_base<Allocator>*>>>
      registry_;

  // Outgoing operations, stable pointees (unique_ptr) per §3.4.
  std::vector<std::unique_ptr<detail::operation_base<Allocator>,
                              detail::op_deleter<Allocator>>,
              rebind_alloc<std::unique_ptr<...>>>
      write_queue_;

  // Contiguous staging buffer for batched writes; never reallocated
  // while a write is in flight (§3.4).
  std::vector<char, rebind_alloc<char>> write_staging_;
  bool write_in_flight_ = false;
  bool kick_posted_ = false;

  // The single operation paused on a continuation request (literal /
  // SASL), if any. Recorded when a segment awaiting a continuation is
  // STAGED (under this mutex), not when the write completes — a server
  // continuation can never arrive un-routed regardless of I/O
  // completion ordering.
  detail::operation_base<Allocator>* continuation_target_ = nullptr;

  // Read side.
  std::vector<std::byte, Allocator> read_storage_;
  bnio::dynamic_byte_vector_buffer<Allocator> read_buffer_;  // over read_storage_
  detail::response_lexer<Allocator>  lexer_;   // [self-built] two-mode framer, §3.5
  detail::response_parser<Allocator> parser_;  // [self-built] shallow parser, §3.5
  bool read_running_ = false;
  bool suspend_read_ = false;   // STARTTLS relocation hook (§8)
  bool bye_received_ = false;   // maps the trailing EOF to errc::server_bye

  // Set once the shell was destroyed: pump completions switch to their
  // quiet teardown forms (no dispatch, no handler invocation).
  std::atomic<bool> abandoned_ = false;

  // Unsolicited event handler table.
  detail::unsolicited_table<Allocator> unsolicited_;

  enum class phase { open, draining, closed } phase_ = phase::open;
```

`std::unique_ptr` elements guarantee pointee stability when the vector
reallocates — this is a hard bnio constraint (§2.5) and coincides with the
type-erasure storage (§3.3). The queue is **never**
`std::vector<std::string>`.

### 3.3 Operation types and type erasure

Each IMAP command is a C++ type. An operation knows how to (a) render its
command bytes, possibly in segments separated by literal continuation
waits, (b) absorb relevant untagged responses, (c) map its tagged reply to
a `result_type`. Operations are stream-agnostic (they produce bytes and
consume parsed responses); only `imap_context` is `Stream`-templated.

The implemented command set (one type per file under
`imap/command/`, all named `*_command`): `capability_command`,
`noop_command`, `logout_command`, `login_command`,
`authenticate_command` (SASL, SASL-IR aware), `starttls_command`,
`select_command`, `examine_command`, `create_command`, `delete_command`,
`rename_command`, `subscribe_command`, `unsubscribe_command`,
`list_command`, `status_command`, `append_command` (literal handshake;
an additional constructor takes a `const mail<Allocator>&` and renders it
to RFC 5322 octets internally), `close_command`, `expunge_command`,
`search_command`, `fetch_command` (generic `fetch_items` form),
`fetch_envelopes_command`, `fetch_headers_command`,
`fetch_message_command`, `store_command`, `copy_command`, `move_command`
(UIDPLUS), `idle_command` (RFC 2177), `raw_command` (extension escape
hatch), and the UID-prefixed variants `uid_search_command`,
`uid_fetch_command`, `uid_fetch_envelopes_command`,
`uid_fetch_headers_command`, `uid_fetch_message_command`,
`uid_store_command`, `uid_copy_command`, `uid_move_command`. Each declares
`result_type`, e.g.:

```cpp
template <class Allocator = std::allocator<std::byte>>
class capability_command {
 public:
  using result_type = capability_set<Allocator>;
  // ... rendering + response accumulation (interface below)
};
```

Type erasure is **self-built** (bexec has no `any_sender`; §2.10), modeled
on `bexec::detail::run_loop_operation_base`: a pinned base with virtual
entry points, allocated once per submission through the context's rebound
allocator, never moved afterwards.

```cpp
namespace bkmail::imap::detail {

template <class Allocator = std::allocator<std::byte>>
class operation_base {
 public:
  operation_base() = default;
  operation_base(const operation_base&) = delete;
  virtual ~operation_base() = default;

  // --- write side ------------------------------------------------
  /// Next chunk of command bytes to stage, or empty if none right now.
  /// A chunk never crosses a {n} boundary: an operation that must wait
  /// for a continuation returns the bytes up to and including the
  /// "{n}\r\n" line, then reports awaits_continuation(). Pure query:
  /// repeated calls before on_segment_flushed() return the same range.
  virtual bnio::const_buffer next_segment() noexcept = 0;
  virtual bool awaits_continuation() const noexcept = 0;
  /// Advisory pipelining wall (AUTHENTICATE / LOGOUT / STARTTLS /
  /// IDLE-style commands): nothing behind this operation is staged in
  /// the same batch.
  virtual bool blocks_pipeline() const noexcept = 0;
  /// The write pump staged next_segment() into the batch; until
  /// on_segment_flushed() arrives, next_segment() returns empty.
  virtual void on_segment_staged() noexcept = 0;
  /// The staged segment has been written to the wire.
  virtual void on_segment_flushed() noexcept = 0;
  /// True once every byte is written and the operation only awaits
  /// server replies ("in flight").
  virtual bool is_written() const noexcept = 0;
  /// True while a segment is staged but not yet flushed. Tagged
  /// dispatch extracts the cell under the context mutex even while
  /// staged (§3.5); the flush bookkeeping touches only cells still
  /// resident in the queue.
  virtual bool is_staged() const noexcept = 0;

  // --- read side ---------------------------------------------------
  virtual void on_continuation(std::string_view text) noexcept = 0;
  /// Hook for untagged responses. Runs under the context lock:
  /// no user code, no re-entry.
  virtual void on_untagged(const untagged_response<Allocator>& r) noexcept = 0;
  /// Ownership predicate for command-scoped untagged data (FETCH /
  /// LIST / LSUB / STATUS / SEARCH / CAPABILITY data lines and FLAGS):
  /// the read pump delivers each such line to the EARLIEST-queued
  /// operation whose predicate accepts it (FIFO attribution, §3.5).
  /// Connection-scoped events and status responses are broadcast to
  /// every operation instead and do not consult this predicate.
  virtual bool wants_untagged(untagged_kind kind) const noexcept = 0;
  /// Terminal tagged reply. Ownership of the cell is moved out of the
  /// queue before this call, so the operation outlives its own
  /// completion.
  virtual void on_tagged(tagged_response<Allocator> r) noexcept = 0;

  // --- control -----------------------------------------------------
  /// Connection-level failure: complete with ec on the value channel.
  virtual void fail(std::error_code ec) noexcept = 0;
  /// Stop-token driven cancel (§3.7). Records the request and forwards
  /// to the sink; never completes the receiver inline.
  virtual void request_stop() noexcept = 0;
  /// Completes the stored receiver/handler with set_stopped()
  /// semantics, from the context's dispatch path. One documented
  /// fallback: when allocating the scheduler box fails,
  /// context_core::post_complete_stopped completes inline as a last
  /// resort.
  virtual void complete_stopped() noexcept = 0;
  /// Registration race guard: true (once) when a stop request arrived
  /// before the operation became visible to the context.
  virtual bool take_stop_request() noexcept = 0;
  /// Cancel request against an operation already staged to the wire.
  /// Returns true when the operation handled it (IDLE re-enters
  /// done() and awaits its tagged reply); false selects detach /
  /// drop-on-arrival.
  virtual bool cancel_written() noexcept = 0;
  /// Marks the operation detached: its tagged reply is dropped on
  /// arrival and the write pump retires the cell after the in-flight
  /// staged write completes.
  virtual void mark_detached() noexcept = 0;
  virtual bool is_detached() const noexcept = 0;

  /// Binds the context sink and stamps the command tag. Called exactly
  /// once, at registration, before the cell enters the queue.
  virtual void attach(operation_sink<Allocator>* sink, std::string_view tag) = 0;
  /// The stamped tag (registry key). Valid after attach().
  virtual std::string_view tag() const noexcept = 0;
  /// Destroys and deallocates the cell with the allocator it was
  /// allocated with. Invoked through op_deleter.
  virtual void dispose() noexcept = 0;
};

template <class Command, class HandlerOrReceiver>
class operation_model final : public operation_base<
    typename Command::allocator_type> {
  // Holds: Command command_; HandlerOrReceiver target_; plus the stop
  // callback wired at attach(). HandlerOrReceiver is either a plain
  // callback void(std::error_code[, result_type]) (callback path) or a
  // bexec receiver (sender path); the model adapts with if constexpr.
  // The callback path maps stop-cancellation to "handler never invoked"
  // (usage.md §3.3); the sender path completes with set_stopped().
};

}  // namespace bkmail::imap::detail
```

The command type contract (`operation_base.h` header banner, implemented
by every command header class):

- `using result_type = ...;` and `using allocator_type = ...;`
- `void render(std::string_view tag)` — tag stamping, called exactly once
  at registration; renders at least the first segment.
- `next_segment() noexcept -> bnio::const_buffer` — the currently
  stageable byte chunk (empty when none); pure query; a segment never
  crosses a `{n}` boundary.
- `bool awaits_continuation() const noexcept` — the literal/SASL wall.
- `void on_segment_flushed() noexcept` — advances past the staged segment
  once it has hit the wire.
- `void on_continuation(std::string_view text)` — releases the next
  segment after `+ ...`.
- `void on_untagged(const untagged_response<allocator_type>&)` — absorbs
  command-relevant untagged data; runs under the context's internal lock
  on the read-dispatch path: fast, no user-code completion, no calls back
  into the context.
- `on_tagged`: terminal mapping, returning the error code to deliver
  (`errc::command_rejected` on NO, `errc::bad_command` on BAD — both
  submission paths, code_layout D2). Void-result commands:
  `std::error_code on_tagged(const tagged_response<A>&)`; result
  commands: `std::error_code on_tagged(const tagged_response<A>&,
  result_type& out)`, filling `out` only on success.
- Optional `bool blocks_pipeline() const noexcept`: advisory wall for the
  write pump (AUTHENTICATE/LOGOUT/STARTTLS return true); nothing behind
  such a command is staged in the same batch.
- Optional `void done()` (IDLE only): cancel of a written IDLE is
  forwarded here; the command queues its `DONE` segment and stays
  registered until the tagged reply arrives, which then completes with
  stop semantics instead of a value.

This is the single spelling of the contract — there is no dual
compatibility form.

The sender half of `submit<Operation>(args...)` lives in
`imap/detail/submit_sender.h`: `submit_sender` decays and stores the
constructor arguments; `connect()` builds the `Operation` from them and
returns a pinned, single-start `submit_operation`. Both are templated on
the context type (not on Stream/Allocator) so the header never names
`imap_context` and the include direction stays one-way.

```cpp
template <class Context, class Operation, class... Args>
class submit_sender {                  // imap/detail/submit_sender.h
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, typename Operation::result_type),
      bexec::set_stopped_t()>;         // void-result form: set_value_t(ec)

  template <class Receiver>
  auto connect(Receiver receiver) &&;  // -> submit_operation (pinned, single-start)

 private:
  Context* context_;
  std::tuple<std::decay_t<Args>...> arguments_;
};
```

`submit_operation::start() noexcept` performs, in order:

1. Observe the receiver's stop token at the start point (a token already
   fired completes stopped without touching the context).
2. Allocate `operation_model<Operation, Receiver>` via
   `rebind_alloc<operation_model>` (§7); on allocation failure complete
   with `std::errc::not_enough_memory` on the value channel.
3. Hand the cell to the context's registration path
  (`start_operation`), which stamps the tag (`allocate_tag()`),
  `attach()`es the sink (rendering the tag-prefixed first segment and
  registering the receiver's stop token, §3.7), pushes the owning
  `unique_ptr` into `write_queue_`, emplaces the registry entry, and
  kicks the write pump **through the scheduler** (so a batch of starts
  that arrive before the worker runs is staged into a single write,
  §3.4).

### 3.4 Write path

Goals: batch small commands into few syscalls; keep buffers stable;
respect literal continuation stops.

**Algorithm (write pump, self-built receiver chain):**

```
kick (on scheduler):
  drain lock {
    staging.clear();
    for op in write_queue_ (front to back):
      if op->is_written(): CONTINUE              // already fully on the wire
      seg = op->next_segment()
      if seg empty:
        if op->awaits_continuation(): STOP       // literal/SASL wall
        else: CONTINUE                           // nothing to send this round
      staging.append(seg); op->on_segment_staged()
      if op->awaits_continuation(): continuation_target_ = op
                 // routed at staging time, under the lock (§3.5):
                 // the server's "+ ..." can never arrive un-routed
      if op->awaits_continuation() or op->blocks_pipeline(): STOP
                 // staged the {n} line, or a pipeline-blocking command
                 // (AUTHENTICATE / LOGOUT / STARTTLS)
  }
  if staging empty: write_in_flight_ = false; return
  write_in_flight_ = true
  start stream_.async_write(scheduler, const_buffer(staging), MSG_NOSIGNAL)

on write completion (ec, n):
  if ec: fail_all(ec); enter phase::draining/closed; return
  notify staged ops on_segment_flushed();                // moves fully-written
                                                         // ops to "in flight"
  if more stageable bytes: chain next async_write immediately
  else: write_in_flight_ = false
```

Properties:

- **Batching.** N submissions that reach the queue before the pump's
  scheduled drain produce one `async_write`. Appending to a non-empty queue
  while a write is in flight chains the next write from the completion
  handler — both mechanisms required by the design ("write registered when
  added to an empty queue; chained while the queue has residue").
- **Contiguity.** bnio has no scatter/gather write; `async_write` takes one
  `const_buffer` (write-all, `include/bnio/tcp/socket.h:149`). Batching is
  therefore one copy into `write_staging_` followed by one syscall.
- **Stability.** `write_staging_` is only mutated when no write is in
  flight, so the pointer captured by the in-flight sender stays valid
  (§2.5). Queue elements are `unique_ptr`-owned, so vector reallocation
  never moves an operation.
- **Literal wall.** An operation paused on a continuation blocks staging of
  everything behind it: wire order forbids interleaving literal bytes with
  later commands. This is where the RFC 3501 §5.5 pipelining ambiguity is
  enforced conservatively (§10).
- **Lock.** The queue's mutex is documented here (it keeps internal state
  coherent when completion handlers hop worker threads), **but bkmail makes
  no thread-safety promise for the queue or for concurrent context API
  calls**: callers must serialize submissions (Layer 2 does; §6). The
  implementation may happen to be safe; that is not contractual.

### 3.5 Read path

A single permanent read is always armed while the connection is alive —
the "always one `async_read_some` outstanding" model.

```
arm:  stream_.async_read_some(scheduler, read_buffer_.prepare(kReadChunk), 0)
on set_value(ec, n):
  if ec or n == 0: connection_lost(ec_or_eof)   // fail registry + queue;
                                                // n == 0 is EOF: orderly
                                                // close after BYE maps to
                                                // errc::server_bye, an
                                                // unannounced EOF to
                                                // errc::unexpected_response
  read_buffer_.commit(n)
  while lexer_.next_complete_response(read_buffer_) yields r: dispatch(r)
  re-arm (unless draining/closed/abandoned/suspended for relocation)
```

`detail::response_lexer` (**self-built**; bnio has no delimiter framing,
§2.4) is a two-mode cursor over the committed bytes:

- **Line mode:** scan for `CRLF`. If the line ends with a literal trailer
  (`{n}`, `{n+}`, or the obsolete `~{n}` spelling), the trailer is part of
  the response and the lexer switches to literal mode with `n` remaining.
- **Literal mode:** wait until `n` more bytes are committed, append them
  verbatim (they may contain arbitrary octets, including `CRLF`), return
  to line mode. Server-to-client literals have no continuation handshake.
- A response is complete when a line-mode line terminates outside a
  literal. `consume()` only after a full response is extracted (bounded
  compaction; see §10 for the erase-from-front caveat).

The complete response text then goes to `detail::response_parser`
(**self-built**), which classifies and shallow-parses:

```cpp
namespace bkmail::imap {

enum class response_status { ok, no, bad, preauth, bye };

/// Classification of an untagged (*) response.
enum class untagged_kind {
  ok, no, bad, preauth, bye,                 // status kinds
  capability, flags, list, lsub, status, search,  // data kinds: payload set
  exists, recent, expunge, fetch,            // numbered kinds: number set
                                             // (fetch also sets payload)
  unknown                                    // extension keyword; payload set
};

struct tagged_response {
  string_type tag;
  response_status status;              // OK / NO / BAD
  std::optional<response_code<Allocator>> code;   // [ALERT], [UIDVALIDITY n], ...
  string_type text;
};

struct untagged_response {
  untagged_kind kind;
  std::uint64_t number = 0;            // seq number for EXISTS/RECENT/EXPUNGE/FETCH
  response_status status;              // for OK/NO/BAD/BYE/PREAUTH kinds
  std::optional<response_code<Allocator>> code;
  string_type text;
  // Remainder of the response for the data kinds (e.g. the
  // parenthesized FETCH attribute list); a view into the read buffer,
  // valid during dispatch only.
  std::string_view payload;
};

struct continuation_request { string_type text; };

using server_response =
    std::variant<tagged_response<Allocator>, untagged_response<Allocator>,
                 continuation_request<Allocator>>;

}  // namespace bkmail::imap
```

Deep structured parsing (ENVELOPE lists, BODYSTRUCTURE trees) happens
lazily in the consuming operation / data layer (`imap/detail/parse_cursor.h`
+ `imap/detail/fetch_parse.h`), not in the lexer.

**Dispatch rules:**

| Response | Routed to |
| --- | --- |
| tagged | the cell is extracted from the queue AND the registry under the context mutex, then `registry_[tag]->on_tagged(...)` runs on the owned cell — always, including a cell whose staged write has not flushed yet (there is no staged-cell-delivered-unlocked special case; the write completion no longer retires dispatched cells). The cell is destroyed after the handler returns, so the operation outlives its own completion. An unknown tag is a detached (cancelled) operation whose reply is dropped on arrival. |
| untagged, connection scope | First the unsolicited table (one whole-variant event per registered handler), then **broadcast** to every registered operation's `on_untagged(r)`. This covers the status kinds (`OK`/`NO`/`BAD`/`PREAUTH`/`BYE`), the connection events (`EXISTS`/`RECENT`/`EXPUNGE`), and unknown/extension kinds. |
| untagged, command scope | First the unsolicited table (when the kind has an event counterpart), then **FIFO attribution**: the line is delivered to the earliest-queued operation whose `wants_untagged(kind)` accepts it, and to no one else. This covers `FETCH`, `LIST`, `LSUB`, `STATUS`, `SEARCH`, `CAPABILITY` data lines and `FLAGS`. (IMAP puts no tag on untagged data; the server completes pipelined commands in submission order in practice, so the earliest waiter is the only honest owner.) |
| continuation | `continuation_target_` (the single operation paused on it); if none, treat as protocol violation per RFC and fail the connection. After the target renders its literal bytes the write pump is kicked. The target is recorded when its segment is staged — under the context mutex, not on write completion — so a continuation can never arrive un-routed regardless of I/O completion ordering. |

Tagged replies may complete **out of order** when commands are pipelined
(RFC 3501 §5.5 permits a server to finish independent commands in any
order); replies are matched purely by tag, which the tag-keyed
`unordered_map` registry handles naturally. The greeting is simply the
first untagged `OK`/`PREAUTH`/`BYE` and flows through the same dispatch.

**Unsolicited events.** `make_unsolicited_event` maps an untagged wire
response to the user-facing variant handed to `on_unsolicited` handlers
(`imap/unsolicited_event.h`):

```cpp
namespace bkmail::imap {

struct exists_event  { std::uint32_t count = 0; };  // * <n> EXISTS
struct recent_event  { std::uint32_t count = 0; };  // * <n> RECENT
struct expunge_event { std::uint32_t sequence_number = 0; };
// * <n> FETCH (FLAGS ...): dispatch fills sequence_number only; `flags`
// stays empty (interested watchers deep-parse the broadcast wire
// response's payload).
struct flags_update_event { std::uint32_t sequence_number = 0; flag_set flags; };
template <class Allocator> struct capability_event {
  capability_set<Allocator> capabilities;
};
template <class Allocator> struct bye_event {     // * BYE (incl. BYE greeting)
  string_type text; std::optional<response_code<Allocator>> code;
};
template <class Allocator> struct greeting_event {  // untagged OK / PREAUTH
  response_status status;                           //   (the greeting itself,
  string_type text;                                 //   and later resp-code
  std::optional<response_code<Allocator>> code;     //   carrying untagged OKs)
};

template <class Allocator = std::allocator<std::byte>>
using unsolicited_event =
    std::variant<exists_event, recent_event, expunge_event,
                 flags_update_event, capability_event<Allocator>,
                 bye_event<Allocator>, greeting_event<Allocator>>;

}  // namespace bkmail::imap
```

Seven alternatives. The `OK`/`PREAUTH` greeting surfaces as
`greeting_event`; a `BYE` — including a `BYE` greeting — surfaces as
`bye_event`, never as `greeting_event`. Kinds with no event counterpart
(command-scoped `FLAGS`/`LIST`/`LSUB`/`STATUS`/`SEARCH` data) yield no
event: their consumers are operations, not unsolicited handlers.

### 3.6 `io_context` borrowing rules

`set_io_context(bnio::io_context&)` stores the context's post-scheduler
handle — the handle carries the context pointer, and its `context()` is
the channel back to the `io_context` — used for the *next* I/O
initiation. Because bnio sockets are
unbound and take a scheduler per call (§2.2), borrowing is exact:
"run the next I/O on that context." The following boundaries are
contractual:

1. **In-flight operations never migrate.** The old context must outlive
   them (its workers run their completions).
2. **Completions run on the initiating context's worker threads.** After a
   switch, different callbacks of the same connection may execute on
   different threads (§6).
3. **The target context must have a thread inside `run()`**, otherwise work
   is queued but never executes.
4. **A single `ssl_stream` operation uses one scheduler internally**;
   switching is only meaningful between independent operations, never
   mid-handshake or mid-shutdown.
5. **`steady_timer` is context-bound at construction** (§2.7): watchdog and
   IDLE heartbeat timers are recreated lazily from the *current* context
   after every switch, and `set_io_context` during IDLE is a precondition
   violation (§9) — documented but not runtime-enforced; no check exists
   today.

### 3.7 Cancellation and teardown

Stop tokens cannot retract an armed read (§2.6); bkmail therefore offers
three granularities:

- **Queued, never on the wire:** removed from `write_queue_` and
  `registry_`; the receiver completes with `set_stopped()` (token-driven)
  — no bytes ever hit the wire.
- **Any byte staged to the wire:** the operation can no longer be
  extracted; the write queue keeps it. For every command except IDLE the
  operation is marked detached: its receiver completes with
  `set_stopped()` promptly and, when the tagged reply arrives, it is
  silently dropped. The connection stays usable. (This is the only honest
  semantic for per-command cancel in IMAP.)
- **Staged IDLE:** `cancel` re-enters the command's `done()`
  (idempotent); the `DONE` segment is queued and flushed, and the tagged
  reply that answers it completes the operation with stop semantics — the
  server stays in sync. DONE delivery to the wire is asynchronous and
  requires a live `io_context`.

Cancel a tag at most once.

On the **callback path** a cancelled operation's handler is never invoked
(there is no stopped channel to report through; "cancel withdraws a
handler", usage.md §3.3). On the **sender path** cancellation completes
with `set_stopped()`.

**Connection level:** `close()` runs the close protocol: (1) stop
accepting submissions; (2) `stream_.lowest_layer().shutdown(SHUT_RD)` to
make the armed read complete promptly; (3) fail queued, never-written
operations on the calling thread with `std::errc::operation_canceled`
(written ones are failed when the read side dies); (4) when the read pump
and any in-flight write have completed, close the descriptor. Closing the
fd *while* a kernel operation references it is forbidden: a recycled fd
number could alias a stale completion (§10). Destroying the context shell
runs the *abandonment* variant instead (§3.2a): queued handlers are
dropped without being invoked, and the core self-destructs once the pumps
go quiet.

**Timeouts:** the self-built `detail::with_timeout(sender, ioc, duration)`
adaptor (`imap/detail/with_timeout.h`) races the wrapped sender against a
`bnio::steady_timer` constructed at connect time from the *current*
`io_context` (§2.7). It wraps any sender whose value completions all carry
`std::error_code` first (the bkmail/bnio completion shape), **mirroring
every value signature of the wrapped sender** plus the stopped channel. A
value completion is forwarded verbatim, with the error code rewritten to
`std::errc::timed_out` when the watchdog fired first. On timeout the
wrapped operation is asked to stop through its receiver-environment stop
token; when that stop wins the race, a payload-less sender completes
`set_value(std::errc::timed_out)`, while a payload-carrying sender (e.g. a
Layer-2 state operation, whose retained state died with the stopped inner
operation) completes `set_stopped()` — the timeout verdict cannot
fabricate a state. No operation uses the adaptor today: zero call sites
exist in the tree, and `detail::with_timeout` is a reserved adaptor, not
an active watchdog.

### 3.8 Error model

Following the bnio completion contract (§2.1), Layer-1 senders declare
exactly:

```cpp
using completion_signatures = bexec::completion_signatures<
    bexec::set_value_t(std::error_code, result_type),
    bexec::set_stopped_t()>;
```

(with `set_value_t(std::error_code)` for void results.)

- `ec == 0`: normal tagged completion (`OK`).
- `ec == errc::command_rejected`: tagged `NO`; `ec ==
  errc::bad_command`: tagged `BAD` (code_layout D2 — both submission
  paths). Both are **recoverable**: the session stays usable. The
  structured resp-code, when present, is read directly from the parsed
  `tagged_response::code` variant member — never re-parsed from text.
- **Exception:** `raw_command` is the escape hatch and maps NO/BAD to no
  error code; the tagged reply is delivered as
  `raw_response{tag, ok = false, text}` so callers can interpret
  extension-specific outcomes themselves.
- `ec != 0` otherwise: transport failure (bnio errno →
  `generic_category`, TLS → `bnio::openssl_error_category()`), EOF,
  protocol violation (malformed response, continuation with no waiter),
  timeout (`errc::timed_out` from the watchdog), or cancellation-as-error.
- `set_stopped()`: stop-token cancellation only (§3.7).

No bkmail sender declares `set_error_t`. `bexec::sync_wait` is safe to use
on bkmail senders: with no error channel there is nothing to throw.

## 4. Mail data model

Plain value types, allocator-aware per §7. The data-model types (`mail`,
`mail_header`, `mail_body`, `envelope`, `address`, `body_structure`,
`account_info`) live in `namespace bkmail`; the IMAP-scoped model types
(`capability_set`, `mailbox_info`, ...) live in `namespace bkmail::imap`.

| Type | Responsibility / composition |
| --- | --- |
| `account_info<Allocator>` | User identity: `user_name`, `password`, optional SASL `authzid`. Credential carrier only; no server settings. |
| `address<Allocator>` | RFC 3501 address 4-tuple: `display_name`, `adl` (source route, usually NIL), `mailbox_name`, `host_name`; `email()` accessor. **Wire NIL is an empty string**, not `std::optional` (code_layout D8). |
| `envelope<Allocator>` | The 10 ENVELOPE fields: `date`, `subject`, `from`, `sender`, `reply_to`, `to`, `cc`, `bcc`, `in_reply_to`, `message_id`; address fields are `vector<address>`; a NIL list parses to an empty vector, a NIL string to an empty string. |
| `body_structure<Allocator>` | Recursive tree: `media_type`/`subtype`, `parameters`, `id`, `description`, `encoding`, `octets` (64-bit); multipart nodes own `vector<body_structure>` children. |
| `mail_header<Allocator>` | Structured RFC 5322 header view: `subject`, `from`/`to`/`cc`/`bcc` address lists, `date` (unparsed), `message_id`, `in_reply_to`. Values are stored as received; **MIME semantics live here**: `detail::decode_encoded_words` (RFC 2047) is declared in this header, never in the protocol parser. The decoder passes text outside encoded-words through unchanged and drops linear whitespace only between two *adjacent* encoded-words (RFC 2047 §6.2); no charset transcoding is performed. |
| `mail_body<Allocator>` | Body octets (`data`: owning `vector<std::byte, Allocator>`) plus `content_type` metadata. |
| `mail<Allocator>` | Composition: `header` + `body`; optionally an `envelope` when produced from IMAP FETCH data. |
| `imap::capability_set<Allocator>` | Parsed CAPABILITY/greeting capabilities (interned, uppercase-normalized; `contains` is case-insensitive); used to probe LITERAL+, SASL-IR, IDLE, UIDPLUS, STARTTLS availability. |
| `imap::message_flag` / `imap::flag_set` | `\Seen \Answered \Flagged \Deleted \Draft \Recent` enum + constexpr bitmask (`set`/`reset`/`test`/`any`). |
| `imap::sequence_set` | Message/UID sequence set (`2:4,7:*`), validated and renderable; values 32-bit. |
| `imap::mailbox_info<Allocator>` | SELECT/EXAMINE snapshot: `name`, `exists`, `recent`, `unseen` (`optional<uint32_t>` — NIL is semantically distinct here), `uid_validity`, `uid_next`, `flags`, `permanent_flags`, `read_only`. |
| `imap::mailbox_status<Allocator>` | STATUS result: `messages`, `recent`, `uid_next`, `uid_validity`, `unseen` — all `optional<uint32_t>`: a disengaged field means the item was not requested (or not returned), not that it is zero. |
| `imap::mailbox_entry<Allocator>` | One LIST row: `name`, `delimiter` (empty when NIL), `no_select`, `has_children`, `has_no_children`. |
| `imap::message_attributes<Allocator>` | One FETCH/STORE datum: `flags`, `internal_date` (raw string), `rfc822_size` (64-bit), optional `envelope`, optional `body_structure`, `sections` (BODY[...] data), `uid` (32-bit; 0 = absent). |
| `imap::raw_response<Allocator>` | Tagged reply payload of `raw_command`: `tag`, `ok`, `text`. |

## 5. Layer 2 — session state machine

### 5.1 Connection owner

Layer 2 needs to survive STARTTLS, which changes the context's type
(§3.1). A dedicated owner hides the type change behind a variant:

```cpp
namespace bkmail::imap {

template <class Allocator = std::allocator<std::byte>>
class imap_connection {
 public:
  /// Layer-1 context over a plaintext TCP stream.
  using tcp_context = imap_context<bnio::tcp::socket, Allocator>;
  /// Layer-1 context over an implicit-TLS or STARTTLS-upgraded stream.
  using tls_context =
      imap_context<bnio::ssl_stream<bnio::tcp::socket>, Allocator>;

  /// Only live connections exist: bkmail::async_connect* are the only
  /// public creators, and a failed connect delivers the terminal
  /// logout_state instead of a placeholder connection (§5.4).
  imap_connection(bnio::tcp::socket stream, bnio::io_context& ioc,
                  const Allocator& alloc = Allocator{});
  imap_connection(bnio::ssl_stream<bnio::tcp::socket> stream,
                  bnio::io_context& ioc, const Allocator& alloc = Allocator{});

  imap_connection(const imap_connection&) = delete;
  /// Move-only, and only while idle (no operation in flight): in-flight
  /// Layer-2 state pins the connection (asserted).
  imap_connection(imap_connection&&) noexcept;
  imap_connection& operator=(imap_connection&&) noexcept;

  /// The capability set negotiated during connect (or after the last
  /// capability() operation / STARTTLS re-probe).
  const capability_set<Allocator>& capabilities() const noexcept;
  void set_capabilities(capability_set<Allocator> caps);

  /// Runs f on the active Layer-1 context, whichever transport it
  /// wraps — the Layer-2 operation driver's single entry point into
  /// Layer 1 (submit/flush/cancel/on_unsolicited).
  template <class F> decltype(auto) with_context(F&& f);

  bnio::io_context& io_context() const noexcept;   // borrowed, never owned
  void set_io_context(bnio::io_context& ioc) noexcept;
  bool is_alive() const noexcept;
  void close() noexcept;

  // STARTTLS relocation (§8), two steps:
  /// Step 1: suspends the plaintext context's read pump
  /// (suspend_read_for_relocation) and hands the socket out
  /// (release_stream), layered under a fresh ssl_stream ready for
  /// async_handshake.
  bnio::ssl_stream<bnio::tcp::socket> upgrade_to_tls_stream(
      bnio::ssl_context& ssl);
  /// Step 2: replaces the inert plaintext shell with a TLS context over
  /// the handshaken stream; clears the capability cache (the caller
  /// re-probes CAPABILITY, RFC 3501).
  tls_context& emplace_tls_context(
      bnio::ssl_stream<bnio::tcp::socket> stream);

  // ---- Layer-2 seriality guard (internal) ---------------------------
  bool try_start_operation();  // one-operation-in-flight slot
  void finish_operation();

 private:
  // Heap-held so the context's address is stable across connection
  // moves: the permanent read pump pins the context object (§3.2), so a
  // context may NEVER be relocated — moving the connection only moves
  // the owning pointer.
  std::unique_ptr<std::variant<tcp_context, tls_context>> context_;
  capability_set<Allocator> capabilities_;
  bnio::io_context* ioc_;      // borrowed
  Allocator alloc_;
  std::atomic<bool> busy_;
};

}  // namespace bkmail::imap
```

The LOGOUT path's terminal teardown is `detail::detain_connection`: the
LOGOUT completion is delivered from inside the read dispatch, and
destroying a context there is unsafe (the pump still references it until
the dispatch unwinds), so the connection's destruction is deferred to a
scheduled task that runs one event-loop turn later.

### 5.2 States and the multi-signature completion model

Each RFC 3501 state is a type; each type exposes **only** the commands
legal in that state. **States own the connection by value** and pass it
from state to state (the state chain is the session's ownership flow);
every operation consumes its state by rvalue and delivers the successor
state for the observed server outcome through its sender — **one
`set_value` signature per successor state, never a merged variant
payload** (code_layout D9). `imap/session_state.h` carries only the four
forward declarations:

```cpp
namespace bkmail::imap {

template <class Allocator> class not_authenticated_state;
template <class Allocator> class authenticated_state;
template <class Allocator> class selected_state;
template <class Allocator> class logout_state;       // terminal

}  // namespace bkmail::imap
```

The earlier `session_state<A>` / `greeting_state<A>` variant aliases are
gone from the public API: where several successor states are possible the
sender publishes one `set_value` signature per outcome, and merging the
alternatives into a variant is a **consumer-side choice**
(`bexec::this_thread::sync_wait_with_variant` for blocking waits,
`bexec::into_variant` ahead of `co_await` inside a `bexec::task`).

The three live states support move-assignment (they are the natural
"session slot" of a caller loop). Operation surface (all `&&`-qualified,
each returning a lazy sender):

```cpp
namespace bkmail::imap {

template <class Allocator = std::allocator<std::byte>>
class not_authenticated_state {
 public:
  [[nodiscard]] auto login(const account_info<Allocator>&) &&;
  [[nodiscard]] auto authenticate(std::string_view mechanism,
                                  std::string_view initial_response) &&;
  [[nodiscard]] auto authenticate_oauth2(std::string_view user,
                                         std::string_view token) &&;
  [[nodiscard]] auto start_tls(bnio::ssl_context& ssl) &&;  // §8
  [[nodiscard]] auto capability() &&;   // refreshes the cache; result value
  [[nodiscard]] auto noop() &&;
  [[nodiscard]] auto logout() &&;
  // NO select/fetch/... here: not legal in this state.
};

template <class Allocator = std::allocator<std::byte>>
class authenticated_state {
 public:
  [[nodiscard]] auto select(std::string_view mailbox) &&;
  [[nodiscard]] auto examine(std::string_view mailbox) &&;
  [[nodiscard]] auto list(std::string_view reference,
                          std::string_view pattern) &&;
  [[nodiscard]] auto status(std::string_view mailbox, status_items) &&;
  [[nodiscard]] auto create(std::string_view mailbox) &&;
  [[nodiscard]] auto delete_mailbox(std::string_view mailbox) &&;
  [[nodiscard]] auto rename(std::string_view old_name,
                            std::string_view new_name) &&;
  [[nodiscard]] auto subscribe(std::string_view mailbox) &&;
  [[nodiscard]] auto unsubscribe(std::string_view mailbox) &&;
  [[nodiscard]] auto append(std::string_view mailbox,
                            const mail<Allocator>&, flag_set = {}) &&;
  [[nodiscard]] auto capability() &&;
  [[nodiscard]] auto noop() &&;
  [[nodiscard]] auto logout() &&;
};

template <class Allocator = std::allocator<std::byte>>
class selected_state {
 public:
  /// The server's last-known view of the open mailbox, maintained from
  /// EXISTS/RECENT/EXPUNGE pushes between commands.
  const mailbox_info<Allocator>& mailbox() const noexcept;

  [[nodiscard]] auto fetch_envelopes(std::string_view seq) &&;
  [[nodiscard]] auto fetch_headers(std::string_view seq,
                                   std::vector<std::string_view> fields) &&;
  [[nodiscard]] auto fetch_message(std::string_view seq) &&;
  [[nodiscard]] auto fetch(std::string_view seq, const fetch_items&) &&;
  [[nodiscard]] auto search(std::string_view criteria) &&;
  [[nodiscard]] auto store(std::string_view seq, flag_set, store_mode) &&;
  [[nodiscard]] auto copy(std::string_view seq, std::string_view mailbox) &&;
  [[nodiscard]] auto move(std::string_view seq, std::string_view mailbox) &&;
  [[nodiscard]] auto expunge() &&;
  [[nodiscard]] auto close() &&;        // -> authenticated_state
  [[nodiscard]] auto idle() &&;         // one IDLE/DONE cycle, §9
  [[nodiscard]] auto uid_fetch_envelopes(std::string_view seq) &&;
  [[nodiscard]] auto uid_fetch_headers(std::string_view seq,
                                       std::vector<std::string_view>) &&;
  [[nodiscard]] auto uid_fetch_message(std::string_view seq) &&;
  [[nodiscard]] auto uid_fetch(std::string_view seq, const fetch_items&) &&;
  [[nodiscard]] auto uid_search(std::string_view criteria) &&;
  [[nodiscard]] auto uid_store(std::string_view seq, flag_set,
                               store_mode) &&;
  [[nodiscard]] auto uid_copy(std::string_view seq,
                              std::string_view mailbox) &&;
  [[nodiscard]] auto uid_move(std::string_view seq,
                              std::string_view mailbox) &&;
  [[nodiscard]] auto capability() &&;
  [[nodiscard]] auto noop() &&;
  [[nodiscard]] auto logout() &&;
};

}  // namespace bkmail::imap
```

### 5.3 How a state operation works

`StateA::operationA(args) &&` returns a hand-written sender
(**self-built**, P2300-style: nested `completion_signatures`, member
`connect(receiver) &&` returning a pinned, single-start operation state
whose `start() noexcept` drives Layer 1). The shared glue is
`detail::state_op_sender` (`imap/state/detail/state_op_sender.h`):

1. `start()` acquires the connection's one-operation-in-flight slot
   (`try_start_operation`, asserted) and observes the receiver's stop
   token at the start point.
2. **Extension gate:** operations that need an advertised capability
   (e.g. `move`/`uid_move` need `MOVE`, `idle()` needs `IDLE`) fail fast
   with `errc::capability_required` without touching the wire when the
   server lacks it.
3. The operation submits the matching Layer-1 command through
   `connection.with_context(...)` (the callback path: `ctx.submit` +
   `ctx.flush`).
4. The Layer-1 command accumulates its untagged data (FIFO attribution,
   §3.5) and completes on the tagged reply; the internal receiver maps
   `(ec, Command::result_type)` through the **successor factory**:
   - the factory runs for every non-stopped completion, so operations
     whose successor equals the current state naturally "keep the current
     state" on NO/BAD (`ec == errc::command_rejected` /
     `errc::bad_command`);
   - operations whose tagged outcome maps to DIFFERENT successor states
     wrap their two per-outcome factories with
     `detail::branch_on_error(...)` (`imap/state/detail/state_op_sender.h`):
     LOGIN/AUTHENTICATE yield the Authenticated state on OK and the
     *retained* Not-Authenticated state otherwise, SELECT/EXAMINE yield
     the Selected state on OK and the *retained* Authenticated state
     otherwise. The completion picks the branch on the error code and each
     branch's successor rides **its own `set_value` signature** — the
     sender publishes both (§5.4). The retained state always carries the
     real connection; no placeholder successor exists anywhere in the
     layer;
   - a resultful factory either returns `std::pair{result, successor}` —
     the result rides the value channel alongside the state — or returns
     the successor state directly, having **absorbed the result**:
     `select`/`examine` fold the `mailbox_info` into the
     `selected_state`, so their completion is
     `set_value(std::error_code, selected_state)` with the snapshot
     readable through `selected_state::mailbox()`;
   - `capability()`-style operations fold bookkeeping (cache updates)
     into the factory;
   - `logout()` hands the connection to `detail::detain_connection` and
     yields the terminal `logout_state`.
5. `set_value(std::move(receiver), ec, [result,] next_state)`.

**Serial guarantee.** Operation methods are `&&`-qualified: calling one
moves the only state token into the sender. There is no API to obtain two
live states over one connection, so two Layer-2 operations can never be in
flight simultaneously. The connection additionally asserts the
one-in-flight slot at start time (contract check, not a synchronization
mechanism).

### 5.4 Completion shape: one `set_value` signature per successor

Layer-2 senders declare **one value signature per possible successor
state** plus the stopped channel — the alternatives travel as distinct
completions, never merged into a variant payload (code_layout D9). The
full table:

| Operation(s) | `set_value` signatures (all plus `set_stopped()`) |
| --- | --- |
| `async_connect` / `async_connect_tls` | `(ec, not_authenticated_state)` — `OK` greeting · `(ec, authenticated_state)` — `PREAUTH` greeting · `(ec, logout_state)` — `BYE` greeting (`ec == errc::server_bye`) or any pre-greeting failure (DNS / TCP connect / TLS handshake) |
| `login` / `authenticate` / `authenticate_oauth2` | `(ec, authenticated_state)` — OK · `(ec, not_authenticated_state)` — NO/BAD or transport failure: the *retained* current state |
| `select` / `examine` | `(ec, selected_state)` — OK (the `mailbox_info` snapshot is absorbed into the state) · `(ec, authenticated_state)` — NO/failure (RFC 3501: a failed SELECT selects no mailbox) |
| `start_tls` | `(ec, not_authenticated_state)` — single signature; over TLS on success |
| `close` | `(ec, authenticated_state)` |
| `logout` | `(ec, logout_state)` |
| `idle` | `(ec, selected_state)` |
| same-state void operations (`noop`, `store`, `copy`, `move`, `expunge`, `create`, …) | `(ec, same_state)` |
| same-state query operations (`list`, `status`, `search`, `fetch*`, `capability`, …) | `(ec, result_type, same_state)` |

`NO`/`BAD` are reported as `errc::command_rejected`/`errc::bad_command`
on the value channel and, for the branching operations, ride the
retained-state signature: the session stays usable (a failure that killed
the connection surfaces again as an error on the next operation issued on
the retained state). Consumers merge the alternatives themselves where
convenient: `bexec::this_thread::sync_wait_with_variant` yields
`std::optional<std::variant<std::tuple<std::error_code, State>...>>` for
blocking waits; `bexec::into_variant` performs the same merge ahead of
`co_await` inside a `bexec::task`. `bkmail::pack` remains the adaptor for
the **single-signature** operations, whose multi-*value* completions need
one tuple for `co_await` (code_layout D7).
`detail::branch_on_error` / `detail::error_branch`
(`imap/state/detail/state_op_sender.h`) is the implementation mechanism
behind the two-outcome operations, and
`detail::with_timeout` (§3.7) mirrors every value signature of the sender
it wraps.

### 5.5 Cancellation passthrough

Layer 2 adds no machinery of its own. The operation state's receiver env
forwards the caller's stop token; the token callback drives
`imap_context::cancel(tag)` (§3.7) and completes the receiver with
`set_stopped()`. Cancelling a Layer-2 operation leaves the connection
usable (the in-flight command's tagged reply is dropped on arrival);
callers who need the connection torn down call
`imap_connection::close()`.

## 6. Threading and locking policy

- `bnio::io_context::run()` may be entered by many threads; any completion
  of any bkmail operation may run on **any worker thread of the initiating
  context** (§2.8). After `set_io_context`, consecutive callbacks of one
  connection may run on different threads (§3.6.2).
- `imap_context` keeps an internal `mutex_` so that its invariants
  (registry ↔ queue ↔ staging) survive completion handlers hopping
  threads. **This is documented for implementers; it is not a public
  guarantee.** Public contract:
  - Concurrent calls on one `imap_context`/`imap_connection`/state from
    user threads: **not supported**; serialize externally or use Layer 2.
  - The write queue specifically: **no thread-safety is promised** even
    though a lock exists internally (§3.4).
- Unsolicited handlers (§3.2) and IDLE event callbacks (§9) run on an
  arbitrary worker thread of the connection's current `io_context`, inline
  in the read dispatch. They must not block and must not call back into the
  context; to re-affinitize, repost through the user's own scheduler
  (`bexec::starts_on`).
- `bexec::inplace_stop_callback` may invoke a cancellation callback on the
  requesting thread; no receiver is ever completed on that thread. The
  discipline is uniform across the library: Layer-1 `request_stop`
  implementations only set flags/erase queue cells under `mutex_`, and
  Layer-2 (state layer/connect) stop completions are likewise posted
  through the `io_context` scheduler — receivers are completed only from
  the `io_context`'s dispatch paths, never inline on the thread that
  requested the stop (deadlock avoidance; receiver completion can run
  arbitrary user code). One documented fallback: when allocating the
  scheduler box fails, `context_core::post_complete_stopped` completes
  inline as a last resort.

## 7. Allocator policy

- Owning public types take a trailing `template <class Allocator =
  std::allocator<std::byte>>` and expose `allocator_type`; strings are
  `basic_string<char, char_traits<char>, rebind_alloc<char>>`; containers
  are configured with `rebind_alloc<T>` (mirrors `code_style.md` and
  bnio's buffer conventions).
- `imap_context`, `imap_connection`, states, operations, and the mail data
  model propagate one allocator from construction; factory helpers deduce
  it from arguments where natural.
- The type-erasure boundary is the one forced heap allocation:
  `operation_model` cells are allocated with
  `std::allocator_traits<Allocator>::rebind_alloc<operation_model>` from
  the *context's* allocator. The self-owning pump I/O boxes
  (`detail::io_box`) are allocated the same way. Everything an operation
  accumulates (FETCH payloads, LIST rows, the response text it hands to
  handlers) uses the context/allocator family; the receiver environment's
  allocator is NOT queried — one allocator flows from construction through
  every internal container, which keeps the guarantee auditable.
- The read path uses `std::vector<std::byte, Allocator>` with
  `bnio::dynamic_byte_vector_buffer<Allocator>` (the allocator-transparent
  bnio buffer; `dynamic_string_buffer` hardcodes `std::string` and is not
  used here).
- bnio async operations themselves take no allocator (§2.12); nothing in
  this section attempts to change that.

## 8. TLS and STARTTLS

Two paths, both ending at the same Layer-1/2 machinery:

1. **Implicit TLS** (`bkmail::async_connect_tls`): DNS resolve → TCP
   `async_connect` → construct `bnio::ssl_stream<tcp::socket>` over the
   socket → `async_handshake(sched, ssl_handshake_type::client)` →
   session bring-up (greeting + initial CAPABILITY probe).
2. **STARTTLS upgrade** (`not_authenticated_state::start_tls(ssl)`):
   - precondition: capability `STARTTLS` advertised, no operation in
     flight (Layer-2 seriality guarantees this);
   - submit `starttls_command` on the plaintext context; on tagged `OK`:
   - `connection.upgrade_to_tls_stream(ssl)`: the plaintext context's
     read pump is **suspended for relocation**
     (`suspend_read_for_relocation()` — stops re-arming after the current
     dispatch unwinds, WITHOUT touching the descriptor, unlike `shutdown`
     which would persist across the move and break the handshake), then
     the `tcp::socket` is moved out (`release_stream()`) and layered
     under a fresh `ssl_stream`;
   - `async_handshake` on the detached stream;
   - `connection.emplace_tls_context(std::move(stream))`: a new TLS
     context is emplaced in the connection's variant (destroying the
     inert plaintext shell — legal because the upgrade quiesced it) and
     its read pump arms immediately; **the capability cache is
     invalidated** (RFC 3501 requires re-issuing CAPABILITY after
     STARTTLS since the plaintext and TLS views may differ);
   - a fresh CAPABILITY probe runs on the upgraded context and re-fills
     the cache; the Not-Authenticated state over TLS is handed back.
- `bnio::ssl_context` is caller-owned and must outlive the connection
  (§2.13).
- The handshake phase has no Layer-1 tag and cannot be cancelled; a stop
  request arriving during it is latched and honoured as soon as the
  handshake completes (handshakes are short).
- TLS failures arrive as `std::error_code` in
  `bnio::openssl_error_category()` on the value channel (§3.8).

## 9. IDLE policy (RFC 2177)

- `selected_state::idle() &&` returns a sender driving **one full
  IDLE/DONE cycle** (`detail::idle_op_sender`): it submits an
  `idle_command`, wakes when the server pushes activity (unsolicited
  `EXISTS`/`EXPUNGE`), when the 29-minute heartbeat point arrives, or —
  via `set_stopped()` — when the receiver's stop token fires. Every exit
  sends `DONE` first (a `cancel` of the pending `idle_command`, §3.7), so
  the server stays in sync; the completion is
  `set_value(std::error_code, selected_state)` with `mailbox()`
  reflecting the newest pushed view. Re-issuing per RFC 5550 is the
  caller's loop (usage.md §2.6). `idle()` fails immediately with
  `errc::capability_required` when the server did not advertise `IDLE`.
- While an IDLE is in flight the connection is **exclusive**: no other
  command may be written to the socket until the tagged completion
  arrives. The read side keeps flowing: pushes are dispatched through the
  unsolicited table as usual and are *not* absorbed by the command. The
  state layer enforces exclusivity by construction.
- **Heartbeat:** the operation arms a `bnio::steady_timer` created from
  the connection's **current** `io_context` at IDLE entry (timers are
  context-bound at construction, §2.7). `set_io_context` during IDLE is a
  precondition violation (§3.6.5) — documented but not runtime-enforced;
  no check exists today.
- Command-layer audiences drive `idle_command` directly: submit it, let
  the server's `+ idling` continuation route it, and end the cycle with
  `cancel(tag)`, which queues DONE (§3.7); the tagged reply that answers
  the DONE completes the operation with stop semantics.
- Mutual exclusion with the command queue is structural on the state
  layer: IDLE is a Layer-2 facility and Layer 2 is serial; Layer-1 users
  driving `idle_command` must observe the same exclusivity themselves —
  once the server is idling, nothing else may be written to the socket
  (documented on the command).

## 10. Risks, limitations, and compensations

| Gap / risk | Compensation |
| --- | --- |
| bnio has no `async_read_until` / delimiter read | self-built `response_lexer` two-mode framer over `dynamic_byte_vector_buffer` (§3.5). Note: `consume()` erases from the front of the storage (O(n) move); the implementation compacts lazily and consumes only after complete responses to bound the cost. |
| No per-operation cancellation of armed I/O | three-granularity model: queue-cell removal, drop-on-arrival detach, `shutdown(SHUT_RD)` / close protocol, watchdog timers (§3.7). |
| No `strand` | internal mutex for coherence across worker hops; explicit *no public thread-safety* contract (§6). |
| No `any_sender` / `move_only_function` | self-built intrusive `operation_base` erasure (§3.3) and a self-built move-only function wrapper for handler tables. |
| RFC 3501 §5.5 pipelining ambiguities | Layer 1 permits pipelining except across literal/SASL/STARTTLS boundaries and the `+ idling` continuation wait, which form hard serialization walls in the write pump (§3.4); once the server is idling, exclusivity is the command-layer caller's responsibility (documented on `idle_command`), and Layer 2 is strictly serial (§5.3). |
| fd reuse race | closing a descriptor while a kernel completion still references it can alias a recycled fd. The close protocol drains pumps before `close()` (§3.7). Destroying the `imap_context` shell is always safe: the shell/core split (§3.2a) keeps the stream alive until both pumps have gone quiet and drops pending handlers without invoking them. |
| Buffer stability | write staging never reallocates mid-write; queue cells are `unique_ptr`; read storage is only `prepare`d when no read is armed (§3.4, §3.5, §2.5). |
| SIGPIPE | every write passes `MSG_NOSIGNAL`; bnio does not set it (§2.14). |
| `ssl_context` lifetime | documented caller obligation; `imap_connection` asserts validity at upgrade/handshake entry (§8). |
| EXPUNGE renumbering | unsolicited `EXPUNGE` shifts later sequence numbers; `selected_state`'s mailbox view (maintained from `EXISTS`/`EXPUNGE` pushes) is the only supported source of truth for sequence numbers while selected. |
| Completion-thread drift | callbacks (unsolicited, IDLE, Layer-2 completions) may run on different worker threads; documented in §6; users needing affinity repost. |
| `bexec::sync_wait` throws error channels by value | bkmail senders have no error channel (§3.8), so `sync_wait` never throws for bkmail operations; noted for example code. |

## Appendix A — Type inventory

The implemented file inventory (one type per file, coupled groups may
share; ~400-line soft target). Header paths are relative to
`include/bkmail/`; the authoritative tree with per-file briefs is
[`code_layout.md`](code_layout.md) §3.

Layer 1 (`namespace bkmail::imap`):
`imap/imap_context.h` (imap_context + `detail::context_core`),
`imap/operation_base.h` (detail::operation_base, operation_sink,
operation_model, op_deleter, io_box — coupled erasure group),
`imap/imap_command.h` (imap_command erased handle + make_command),
one header per command under `imap/command/` (`capability_command.h`,
`noop_command.h`, `logout_command.h`, `login_command.h`,
`authenticate_command.h`, `starttls_command.h`, `select_command.h`,
`examine_command.h`, `create_command.h`, `delete_command.h`,
`rename_command.h`, `subscribe_command.h`, `unsubscribe_command.h`,
`list_command.h`, `status_command.h`, `append_command.h`,
`close_command.h`, `expunge_command.h`, `search_command.h`,
`fetch_command.h`, `fetch_envelopes_command.h`,
`fetch_headers_command.h`, `fetch_message_command.h`, `store_command.h`,
`copy_command.h`, `move_command.h`, `idle_command.h`, `raw_command.h`,
and the `uid_*` variants), `imap/command.h` (aggregate),
`imap/response.h` (response_status, untagged_kind, response_code,
tagged_response, untagged_response, continuation_request,
server_response), `imap/unsolicited_event.h` (the seven event structs +
variant), `imap/raw_response.h`,
`imap/detail/response_lexer.h`, `imap/detail/response_parser.h`,
`imap/detail/parse_cursor.h` (non-owning deep-parse cursor),
`imap/detail/fetch_parse.h` (shared ENVELOPE/BODYSTRUCTURE/FETCH deep
parsers), `imap/detail/astring.h` (rendering helpers),
`imap/detail/unsolicited_table.h` (+ `detail::registration` token),
`imap/detail/write_pump.h`, `imap/detail/read_pump.h`,
`imap/detail/submit_sender.h` (the sender path),
`imap/detail/with_timeout.h` (watchdog adaptor),
`imap/capability_set.h`, `imap/flags.h` (message_flag, flag_set),
`imap/sequence_set.h`, `imap/mailbox_info.h` (+ status_items),
`imap/mailbox_entry.h`, `imap/mailbox_status.h`,
`imap/message_attributes.h`, `imap/search_criteria.h`,
`imap/fetch_items.h` (+ store_mode).

Layer 2 (`namespace bkmail::imap`, entry points in `bkmail`):
`imap/imap_connection.h` (+ `detail::detain_connection`),
`imap/state/not_authenticated.h`, `imap/state/authenticated.h`,
`imap/state/selected.h`, `imap/state/logout.h`, `imap/state/idle.h`,
`imap/state/detail/state_op_sender.h` (state_op_sender + idle_op_sender,
one coupled group), `imap/session_state.h` (forward declarations of the
four states), `imap/connect.h` (async_connect / async_connect_tls).

Data model (`namespace bkmail`): `account_info.h`, `address.h`,
`envelope.h`, `body_structure.h`, `mail_header.h` (+
`detail::decode_encoded_words`), `mail_body.h`, `mail.h`, `pack.h`
(`bkmail::pack`), `error.h` (errc, error_category), `version.h`,
`export.h`.

Cross-module detail (`namespace bkmail::detail`): `detail/allocator_ext.h`
(rebind_alloc_t / string_of / vector_of), `detail/unique_function.h`.

Compiled residue (`src/`): `version.cpp`, `error.cpp`,
`capabilities.cpp` (capability name interning helpers).

## Appendix B — Testing strategy (summary)

As implemented under `tests/` (one test binary per module area; shared
doubles in `tests/support/`):

- **Unit — model (`tests/model/`):** data-model round trips: envelope /
  address / body_structure / mail_header + the RFC 2047 decoder, flags,
  sequence_set, search_criteria, fetch_items, capability_set, raw_response,
  error category round-trips.
- **Unit — framer/parser (`tests/proto/`):** corpus-driven tests for the
  lexer, the parser, and the parse cursor: literals spanning reads,
  quoted-string escapes, `{n+}` literals, unknown response codes
  (skipped), unsolicited interleaving, out-of-order tagged replies,
  untagged BYE mid-command, astring rendering.
- **Unit — queue/pumps/dispatch (`tests/layer1/`):** because
  `imap_context` is templated on `Stream`, tests inject a scripted
  in-memory stream (`tests/support/scripted_stream.h`, a test double
  modeling `detail::imap_stream` over `bexec::run_loop`) — no sockets, no
  threads, deterministic completion order. Verifies batching (N submits →
  1 write), literal walls, registry cleanup, the three cancellation
  granularities (queued / written drop-on-arrival / IDLE DONE-first),
  type-erased batching, shell destruction with pending handlers, and the
  unsolicited table (whole-variant dispatch, FIFO attribution of
  command-scoped data, token-based unregistration).
- **Unit — state machine (`tests/layer2/`):** scripted Layer-1 context
  drives Layer-2 transitions and the connect senders: greeting variants
  (OK/PREAUTH/BYE → not_authenticated / authenticated / logout_state with
  `errc::server_bye`), compile-time completion-signature tables (one
  `set_value` signature per successor state), NO keeps state, fatal ec
  invalidates the session, EXPUNGE snapshot updates, seriality assertion,
  STARTTLS upgrade on a scripted pair.
- **Integration (`tests/integration/`):** a fake IMAP server on loopback
  (`tests/support/fake_imap_server.h`, bnio acceptor) replaying scripted
  sessions: login/select/fetch with literals, APPEND with continuation,
  pipelined commands, IDLE push + DONE, unsolicited event flow, error
  paths, protocol robustness. The live-server connectivity check is not a
  test — it is the read-only `examples/real_server_session` program, so
  `ctest` never needs the network or credentials.
