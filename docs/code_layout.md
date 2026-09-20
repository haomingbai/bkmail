# bkmail Code Layout

This document is the file-level organization contract for bkmail. It turns
the type inventory of [`architecture.md`](architecture.md) and the API
surface of [`usage.md`](usage.md) into a concrete file tree, a naming
master table, CMake changes, and an implementation batching plan. Where the
two input documents disagree, §1 records the binding decision. Naming and
formatting rules from [`code_style.md`](code_style.md) apply to every file
listed here and are not repeated.

Reading guide: §1 is binding on every implementation agent; §2 gives the
module graph; §3 is the complete file tree with per-file contents and the
`@brief` line of every public header; §4 covers the ~400-line soft limit;
§5 is the naming master table (the implementation contract); §6 is the
CMake plan; §7 is the parallel-implementation batching plan.

**Migration note (2026-09-19):** the general-purpose headers have moved
from the `include/bkmail/` root into `include/bkmail/common/` —
`address.h`, `envelope.h`, `body_structure.h`, `mail_header.h`,
`mail_body.h`, `mail.h`, `account_info.h`, `pack.h`, `error.h` — and the
former top-level `detail/` directory into
`include/bkmail/common/detail/` (`allocator_ext.h`, `unique_function.h`).
Namespaces are unchanged; only file locations moved. The affected
sections (§2, §3.1/§3.1a, §3.6, §6) below have been updated accordingly.
§7 is the historical implementation plan and is kept as written, so its
file lists still use the pre-move paths.

## 1. Reconciliation decisions

`architecture.md` and `usage.md` were written in parallel and diverge on
eight points (D1–D8). The library author's original design takes
precedence, then consistency with bnio/bexec facilities, then simplicity.
Each decision below is final; D9 records a later author ruling from the
post-implementation Layer-2 completion-model refactor and supersedes the
completion-model half of D2.

**D1. Layer-1 submission model — author's original model wins; sender
submission survives as a convenience overload.**
The author's design is "one C++ type per command + type erasure + batch
registration + callbacks": `submit(command, handler)` stamps a tag and
queues bytes **without doing I/O**, `flush()` performs the batched write,
and handlers are invoked from the read loop. This is the Layer-1 contract
(usage.md §3.3–3.6). The architecture's lazy `submit<Operation>(args)`
sender does not conflict with that model — its `start()` performs exactly
one erased registration — so it is kept as an additional convenience
overload for sender-composing callers. Both paths share the same
`operation_base` erasure and the same write queue; the sender path's tag is
allocated at `start()`, the callback path's tag is allocated at `submit()`
call time. Only the callback path exists on the batch form
(`submit(vector<unique_ptr<imap_command>>)`); `flush()` is explicit on the
callback path, while a started sender kicks the pump itself (kicking
through the scheduler still batches co-arriving starts, architecture §3.4).

**D2. Layer-2 merged-state scope — hybrid.** *(Superseded by D9 for the
completion model; kept here as the record of the original ruling.)*
The architecture's
`session_state<A>` four-state variant is kept as the documented union of
the session (and as the target type of user-side `let_value` chains), but
no single operation completes with the four-state variant. Free functions
complete with `greeting_state<A>` (two alternatives: `not_authenticated`,
`authenticated`); a `BYE` greeting completes with `ec ==
errc::server_bye` instead of a `logout_state` alternative, because a dead
connection has no usable successor state. Every state operation completes
with its **determinate successor state type** (`login` →
`authenticated_state`, `select` → `selected_state`, `close` →
`authenticated_state`, `logout` → `logout_state`, …). `NO`/`BAD` keep the
current state and are reported via the error channel as
`errc::command_rejected` / `errc::bad_command`; transport/protocol failure
is reported via the error code and invalidates the session (no
`logout_state` is fabricated). Rationale: determinate successor types are
strictly simpler for callers (no variant matching per step) while the
`session_state` union stays available for chains; mapping `NO`/`BAD` into
the value channel would contradict the usage-document error contract, and
shipping a `logout_state` on `ec != 0` would contradict the "state is
unusable after fatal error" rule.

**D3. Cancellation — three-granularity internal model, one public entry
point.** The architecture's queue-removal / drop-on-arrival /
shutdown-drain model is the implementation truth (it is what stop tokens
can physically do in bnio). The public Layer-1 surface is the usage
document's single `imap_context::cancel(tag)`: it maps onto queue removal
for not-yet-written commands and onto detach-drop-on-arrival for written
ones, and queues `DONE` first when cancelling a pending `idle_command`.
Sender-path operations additionally honour receiver stop tokens end to end
(`set_stopped()`). Layer 2 has no extra machinery. The timeout adaptor
(`detail::with_timeout`) stays internal.

**D4. Command and state naming — command layer follows usage.md, states
follow architecture.md.** Layer-1 types are `*_command` (`login_command`,
`capability_command`, …) with `uid_`-prefixed UID variants and an
extension escape hatch `raw_command`; Layer-1 result types are plain values
per command (§5.2), with `NO`/`BAD` mapped to `errc` codes on the callback
path. State types are the architecture's four
(`not_authenticated_state`, `authenticated_state`, `selected_state`,
`logout_state`). State method names follow usage.md §2.1
(`fetch_envelopes`, `fetch_headers`, `fetch_message`, `move`, …) because
they carry the operation's fetch-item intent that architecture's generic
`fetch(items)` leaves to a parameter type; the full table is §5.3.

**D5. `imap_context` shape — constructor starts the permanent read.**
`imap_context{std::move(stream), ioc, alloc}` (stream first, then the
borrowed `io_context`, matching usage.md) arms the permanent
`async_read_some` loop immediately and consumes the greeting through the
unsolicited path. There is no `start()` method: a context whose read pump
is not running is a half-object, and two-phase init buys nothing because
the stream must already be connected at hand-over. `is_alive()` reports
whether the read pump is still running.

**D6. Connect entry points — self-contained free functions.** DNS resolve,
TCP connect, TLS handshake (for the TLS variant), CAPABILITY probing, and
greeting consumption all live inside `bkmail::async_connect` /
`bkmail::async_connect_tls`. Callers receive a ready session, not homework.
bnio provides resolution on the post scheduler
(`ioc.get_post_scheduler().async_resolve(host, service,
bnio::dns_result_view{...})`, used in usage.md §3.2), so no resolver
facility is invented. The capability set negotiated during connect
is cached on `imap_connection` and invalidated by STARTTLS. The
architecture's `imap_connection::connect`/`connect_tls` statics are
demoted to implementation detail: the public surface is the free
functions, whose allocator parameter is a normal template parameter
(`async_connect_tls<Alloc>(...)`) rather than a class-scope one.

**D7. Multi-value completions stay; `pack` idiom documented, not
hidden.** bexec `task` awaits only single-value senders, but changing every
bkmail sender to a one-tuple signature would force `sync_wait` users into
an extra tuple unwrap and would make receiver signatures opaque. Instead,
bkmail ships a tiny public helper `bkmail::pack(sender)` (the `bexec::then`
tuple-packing adaptor from usage.md §1.4) so `co_await bkmail::pack(op)`
works out of the box. Sender signatures stay `(std::error_code, [result,]
next_state)`.

**D8. Data-model field naming — usage.md wins, with the architecture's
NIL policy.** `address` has `display_name`, `mailbox_name`, `host_name`,
`email()`; `mailbox_info` has `uid_validity`, `uid_next`; flags are
`message_flag` + `flag_set`; the LIST row type is `mailbox_entry`. The
architecture's 4-tuple adds `adl` (source route): kept as a member of
`address` for wire fidelity, defaulted and rarely read. NIL on the wire is
represented by empty `std::optional`-free strings except where NIL is
semantically distinct (`mailbox_info::unseen` is
`std::optional<std::uint32_t>`). `body_structure` keeps its architecture
responsibilities with usage.md field names (`media_type`, `subtype`,
`parameters`, `encoding`, `octets`, `parts`, plus `id`/`description`).

**D9. Layer-2 completion model — one `set_value` signature per successor
state, no variant payloads (post-implementation refactor, author
ruling).** D2's "determinate successor + documented variant unions"
compromise was replaced wholesale once the layer settled:

- The `session_state<A>` and `greeting_state<A>` variant aliases are
  **deleted** from the public API; `imap/session_state.h` carries only the
  four forward declarations. Merging alternatives into a variant is a
  **consumer-side choice** (`bexec::this_thread::sync_wait_with_variant`
  for blocking waits, `bexec::into_variant` ahead of `co_await`), never a
  library-spelled payload.
- Branching senders publish one independent value signature per outcome:
  connect → `(ec, not_authenticated_state)` / `(ec, authenticated_state)`
  / `(ec, logout_state)`; login/authenticate → Authenticated on OK, the
  **retained** Not-Authenticated state otherwise; select/examine →
  Selected on OK, the retained Authenticated state otherwise. The retained
  state carries the **real** connection — the placeholder-successor
  mechanism (including the empty `imap_connection` constructor) was
  removed with the refactor.
- A `BYE` greeting (or any pre-greeting connect failure) completes with
  `(ec, logout_state)`, `ec == errc::server_bye` for the BYE case —
  superseding D2's "no logout_state is fabricated" carve-out: the terminal
  state is not a session, so delivering it alongside a fatal error does
  not contradict the reusability rule.
- `detail::branch_on_error` / `detail::error_branch`
  (`imap/state/detail/state_op_sender.h`) is the implementation mechanism
  for the two-outcome operations; `detail::with_timeout` mirrors every
  value signature of the wrapped sender. `NO`/`BAD` keep the
  `errc::command_rejected` / `errc::bad_command` value-channel mapping of
  D2, now riding the retained-state signature.

One further divergence not on the list: usage.md's Layer-1 types live in
`namespace bkmail` while the architecture places them in
`bkmail::imap`. **Decision: `bkmail::imap` for both layers** (the
architecture's rule); data-model types (`mail`, `envelope`, `address`,
`mail_header`, `mail_body`, `body_structure`, `account_info`) and the free
functions live directly in `bkmail`. `imap::` types are introduced into
examples with `using namespace` or qualification at the user's choice.

## 2. Modules and dependency direction

Five logical modules, acyclically ordered. A module may include anything
below it and nothing above it; within a module, `detail` headers may be
included by the module's public headers but never by a lower module.

```
(4) layer2     session state machine, imap_connection, connect entry
      |  includes
(3) layer1     imap_context, commands, erasure, response types, unsolicited
      |  includes
(2) proto      [detail] response_lexer, response_parser, parse_cursor,
               fetch_parse, tag/astring utilities
      |  includes
(1) model      data model + error codes + flags/sequence_set/capability_set
      |  includes
(0) core       export.h, version.h  (no intra-project dependencies)
```

Concrete mapping:

| Module | Namespace | Directory |
| --- | --- | --- |
| core | `bkmail` | `include/bkmail/` root files (`export.h`, `version.h`) |
| model | `bkmail`, `bkmail::imap` | `include/bkmail/common/` + `include/bkmail/imap/` value headers |
| proto | `bkmail::imap::detail` | `include/bkmail/imap/detail/` |
| layer1 | `bkmail::imap` (+`detail`) | `include/bkmail/imap/`, `include/bkmail/imap/command/`, `include/bkmail/imap/detail/` |
| layer2 | `bkmail::imap`, `bkmail` (free functions) | `include/bkmail/imap/state/`, `include/bkmail/imap/imap_connection.h`, `include/bkmail/connect.h` |

Aggregate headers contain only `#include`s: `bkmail/bkmail.h` (everything),
`bkmail/imap.h` (layer1 + layer2 + the imap-scoped model types),
`bkmail/imap/command.h` (every command type + `make_command` + the erased
`imap_command`). No logic, no inline functions beyond aliases.

Library target shape: **the existing `STATIC`-by-default `bkmail` target
stays** (`add_library(bkmail)` + `BUILD_SHARED_LIBS`), it is not split and
not converted to INTERFACE. Because every public type is allocator-
templated, ~95% of the code is header-only; the compiled target carries the
non-template residue: `version.cpp`, `error.cpp` (`bkmail::error_category()`
+ `make_error_code(errc)`), and `capabilities.cpp` (case-insensitive
capability interning helpers that must not be inlined into every TU).
Template-heavy code being in headers is a consequence of the allocator
policy, not a choice to review.

`include/bkmail/common/` holds the general-purpose, IMAP-independent
headers shared across modules: the data-model types (`account_info.h`,
`address.h`, `envelope.h`, `body_structure.h`, `mail_header.h`,
`mail_body.h`, `mail.h`), the error codes (`error.h`), and the `pack`
adaptor (`pack.h`, `bkmail::pack` — public API, re-exported from
`bkmail.h`). `include/bkmail/common/detail/` holds cross-module template
helpers that are not public API: `unique_function.h` and
`allocator_ext.h` (rebound string/vector aliases). The module `core`
(`export.h`, `version.h`) and the IMAP facility stay under
`include/bkmail/`.

## 3. File tree

Legend: **[H]** header-only (template/inline, part of the interface, no
compiled code) · **[H+CPP]** header plus a translation unit compiled into
`bkmail` · **[AGG]** aggregate header (includes only) · **[detail]** not
public API (shipped, but excluded from the documented surface). Every file
starts with the full banner of code_style.md; the `brief:` line under each
public header is its exact `@brief` content — implementation agents copy it
verbatim (date: day of authorship, author: Haoming Bai
\<haomingbai@hotmail.com\>).

### 3.1 `include/bkmail/` — core, connect entry, and aggregate headers

| File | Kind | Contents |
| --- | --- | --- |
| `export.h` | [H] (exists) | `BKMAIL_EXPORT` macro family. Unchanged. brief: *Export macros for bkmail.* |
| `version.h` | [H+CPP] (exists) | `bkmail::version()`. Unchanged. brief: *Version information for bkmail.* |
| `connect.h` | [H] | `bkmail::async_connect`, `bkmail::async_connect_tls`. brief: *IMAP session connect entry points.* |
| `imap.h` | [AGG] | includes every header of §3.2 + §3.3 + §3.4. brief: *Aggregate header for the bkmail IMAP facility.* |
| `bkmail.h` | [AGG] (exists) | adds includes of the above and of §3.1a; keeps bnio/bexec umbrella includes. brief: *Aggregate header for the entire bkmail library.* |

### 3.1a `include/bkmail/common/` — general-purpose data model + error + pack

Moved here from the `include/bkmail/` root on 2026-09-19 (see the
migration note at the top); namespaces unchanged.

| File | Kind | Contents |
| --- | --- | --- |
| `common/error.h` | [H+CPP] | `bkmail::errc`, `bkmail::error_category()`, `make_error_code`, `is_error_code_enum` specialization. brief: *bkmail error codes and error category.* |
| `common/account_info.h` | [H] | `account_info<Allocator>` (`user_name`, `password`, `authzid` optional). brief: *IMAP account credentials carrier.* |
| `common/address.h` | [H] | `address<Allocator>` (`display_name`, `adl`, `mailbox_name`, `host_name`, `email()`). brief: *RFC 3501 address tuple.* |
| `common/envelope.h` | [H] | `envelope<Allocator>` (10 ENVELOPE fields). brief: *IMAP ENVELOPE structure.* |
| `common/body_structure.h` | [H] | `body_structure<Allocator>` (recursive MIME tree). brief: *MIME BODYSTRUCTURE tree node.* |
| `common/mail_header.h` | [H] | `mail_header<Allocator>` + MIME decoding entry points (RFC 2047 encoded-word decoding as `detail` inline helpers declared here). brief: *Structured RFC 5322 header view with MIME decoding.* |
| `common/mail_body.h` | [H] | `mail_body<Allocator>`. brief: *Owning message body with content metadata.* |
| `common/mail.h` | [H] | `mail<Allocator>` (`header`, `body`, optional `envelope`). brief: *Complete mail message composition.* |
| `common/pack.h` | [H] | `bkmail::pack` adaptor (tuple-packing `bexec::then` wrapper). brief: *Packs multi-value sender completions into one tuple for co_await.* |

### 3.2 `include/bkmail/imap/` — model types + Layer 1 + Layer 2

| File | Kind | Contents |
| --- | --- | --- |
| `imap/flags.h` | [H] | `imap::message_flag`, `imap::flag_set`. brief: *IMAP message system flags and flag set.* |
| `imap/sequence_set.h` | [H] | `imap::sequence_set` (validated, renderable `2:4,7:*`). brief: *Validated IMAP sequence-set value type.* |
| `imap/capability_set.h` | [H] | `imap::capability_set<Allocator>` (`contains`, iteration). brief: *Parsed IMAP capability set with case-insensitive lookup.* |
| `imap/mailbox_info.h` | [H] | `imap::mailbox_info<Allocator>` + `imap::status_items`. brief: *SELECT/EXAMINE mailbox snapshot.* |
| `imap/mailbox_entry.h` | [H] | `imap::mailbox_entry<Allocator>` (one LIST row). brief: *One LIST response entry.* |
| `imap/mailbox_status.h` | [H] | `imap::mailbox_status<Allocator>` (STATUS result). brief: *STATUS command result.* |
| `imap/message_attributes.h` | [H] | `imap::message_attributes<Allocator>` (one FETCH datum; may embed `envelope`/`body_structure`). brief: *One FETCH/STORE message datum.* |
| `imap/search_criteria.h` | [H] | `imap::search_criteria<Allocator>` (owning string wrapper with validation). brief: *IMAP SEARCH criteria carrier.* |
| `imap/fetch_items.h` | [H] | `imap::fetch_items` (bitmask over ENVELOPE/BODYSTRUCTURE/FLAGS/UID/INTERNALDATE/RFC822.SIZE/body sections) + `imap::store_mode`. brief: *FETCH item selection and STORE mode.* |
| `imap/raw_response.h` | [H] | `imap::raw_response<Allocator>` (`tag`, `ok`, `text`). brief: *Tagged reply payload of a raw command.* |
| `imap/response.h` | [H] | `imap::response_status`, `imap::response_code` (variant over the coded forms), `imap::tagged_response<Allocator>`, `imap::untagged_response<Allocator>`, `imap::continuation_request<Allocator>`, `imap::server_response<Allocator>`. Coupled wire types sharing one file per the coupled-types rule. brief: *Parsed IMAP server response types.* |
| `imap/unsolicited_event.h` | [H] | `imap::exists_event`, `recent_event`, `expunge_event`, `flags_update_event`, `capability_event<A>`, `bye_event<A>`, `greeting_event<A>` (7 alternatives), `imap::unsolicited_event<Allocator>` variant. Coupled event set, one file. brief: *Unsolicited server event types.* |
| `imap/operation_base.h` | [H] [detail] | `detail::operation_base`, `detail::operation_sink`, `detail::operation_model<Command, HandlerOrReceiver>`, `detail::op_deleter`, `detail::allocate_operation`, `detail::map_status_to_ec`, `detail::string_hash`, `detail::io_box_base`/`detail::io_box`/`detail::make_io_box` (self-owning pump I/O boxes), the `command_like`/`handler_for` concepts. One coupled erasure group; the file runs past the soft limit on purpose (§4). brief: *Type-erasure base for queued IMAP commands.* |
| `imap/imap_command.h` | [H] | `imap::imap_command<Allocator>` erased handle (unique_ptr wrapper over `operation_base`), `imap::make_command(command, handler)`. brief: *Type-erased IMAP command for batch submission.* |
| `imap/imap_context.h` | [H] | `imap::imap_context<Stream, Allocator>` shell + `detail::context_core<Stream, Allocator>` (the heap-held state shared with the in-flight pump boxes; architecture §3.2a — one coupled group in one file). brief: *IMAP command/connection context owning the stream.* |
| `imap/imap_connection.h` | [H] | `imap::imap_connection<Allocator>` (variant owner, capability cache, `close()`, STARTTLS relocation hooks, the one-operation-in-flight slot) + `detail::detain_connection` (LOGOUT teardown deferred past the read dispatch). brief: *Owns an IMAP connection across plaintext and TLS.* |
| `imap/session_state.h` | [H] | forward declarations of the four states only (D9: the `session_state`/`greeting_state` variant aliases are gone). brief: *Forward declarations of the four Layer-2 session states.* |
| `imap/command.h` | [AGG] | includes every `imap/command/*.h`, `imap/imap_command.h`. brief: *Aggregate header for all IMAP command types.* |

### 3.3 `include/bkmail/imap/state/` — Layer 2

| File | Kind | Contents |
| --- | --- | --- |
| `state/not_authenticated.h` | [H] | `not_authenticated_state<Allocator>`. brief: *IMAP Not-Authenticated session state.* |
| `state/authenticated.h` | [H] | `authenticated_state<Allocator>`. brief: *IMAP Authenticated session state.* |
| `state/selected.h` | [H] | `selected_state<Allocator>` (holds `mailbox_info` snapshot). brief: *IMAP Selected session state.* |
| `state/logout.h` | [H] | `logout_state<Allocator>` (terminal, dataless). brief: *IMAP Logout session state.* |

### 3.4 `include/bkmail/imap/command/` — one command per file

All **[H]**, all templates on `Allocator` (defaulted). Each file defines
exactly the command class named by the file; `uid_*` variants are separate
classes in separate files (rendering and result mapping differ enough that
sharing a file would read as one type with a flag — rejected).

| File | Command class (all `namespace bkmail::imap`) | brief |
| --- | --- | --- |
| `command/capability_command.h` | `capability_command<A>` | *CAPABILITY command.* |
| `command/noop_command.h` | `noop_command<A>` | *NOOP command.* |
| `command/logout_command.h` | `logout_command<A>` | *LOGOUT command.* |
| `command/login_command.h` | `login_command<A>` | *LOGIN command.* |
| `command/authenticate_command.h` | `authenticate_command<A>` (mechanism + optional initial response; SASL continuation aware) | *AUTHENTICATE command (SASL, SASL-IR aware).* |
| `command/starttls_command.h` | `starttls_command<A>` | *STARTTLS command.* |
| `command/select_command.h` | `select_command<A>` | *SELECT command.* |
| `command/examine_command.h` | `examine_command<A>` | *EXAMINE command.* |
| `command/create_command.h` | `create_command<A>` | *CREATE command.* |
| `command/delete_command.h` | `delete_command<A>` | *DELETE command.* |
| `command/rename_command.h` | `rename_command<A>` | *RENAME command.* |
| `command/subscribe_command.h` | `subscribe_command<A>` | *SUBSCRIBE command.* |
| `command/unsubscribe_command.h` | `unsubscribe_command<A>` | *UNSUBSCRIBE command.* |
| `command/list_command.h` | `list_command<A>` | *LIST command.* |
| `command/status_command.h` | `status_command<A>` | *STATUS command.* |
| `command/append_command.h` | `append_command<A>` (literal handshake) | *APPEND command with literal continuation.* |
| `command/close_command.h` | `close_command<A>` | *CLOSE command.* |
| `command/expunge_command.h` | `expunge_command<A>` | *EXPUNGE command.* |
| `command/search_command.h` | `search_command<A>` | *SEARCH command.* |
| `command/fetch_envelopes_command.h` | `fetch_envelopes_command<A>` | *FETCH ENVELOPE command.* |
| `command/fetch_headers_command.h` | `fetch_headers_command<A>` | *FETCH BODY.PEEK[HEADER.FIELDS] command.* |
| `command/fetch_message_command.h` | `fetch_message_command<A>` | *FETCH BODY.PEEK[] full-message command.* |
| `command/fetch_command.h` | `fetch_command<A>` (generic `fetch_items` form) | *Generic FETCH command over fetch_items.* |
| `command/store_command.h` | `store_command<A>` | *STORE command.* |
| `command/copy_command.h` | `copy_command<A>` | *COPY command.* |
| `command/move_command.h` | `move_command<A>` (UIDPLUS MOVE) | *MOVE command (UIDPLUS).* |
| `command/uid_search_command.h` | `uid_search_command<A>` | *UID SEARCH command.* |
| `command/uid_fetch_envelopes_command.h` | `uid_fetch_envelopes_command<A>` | *UID FETCH ENVELOPE command.* |
| `command/uid_fetch_headers_command.h` | `uid_fetch_headers_command<A>` | *UID FETCH BODY.PEEK[HEADER.FIELDS] command.* |
| `command/uid_fetch_message_command.h` | `uid_fetch_message_command<A>` | *UID FETCH BODY.PEEK[] command.* |
| `command/uid_fetch_command.h` | `uid_fetch_command<A>` | *Generic UID FETCH command.* |
| `command/uid_store_command.h` | `uid_store_command<A>` | *UID STORE command.* |
| `command/uid_copy_command.h` | `uid_copy_command<A>` | *UID COPY command.* |
| `command/uid_move_command.h` | `uid_move_command<A>` | *UID MOVE command (UIDPLUS).* |
| `command/idle_command.h` | `idle_command<A>` (DONE on cancel) | *RFC 2177 IDLE command.* |
| `command/raw_command.h` | `raw_command<A>` | *Caller-supplied raw IMAP command line.* |

### 3.5 `include/bkmail/imap/detail/` — protocol engine (all [H] [detail])

| File | Contents |
| --- | --- |
| `detail/response_lexer.h` | `detail::response_lexer` two-mode framer over `bnio::dynamic_byte_vector_buffer`. |
| `detail/response_parser.h` | `detail::response_parser`: classify tagged/untagged/continuation; shallow parse into the §3.2 response types. |
| `detail/parse_cursor.h` | `detail::parse_cursor` non-owning deep-parse cursor (standalone header — the shared base of the fetch parsers). |
| `detail/fetch_parse.h` | Shared ENVELOPE / BODYSTRUCTURE / FETCH deep parsers used by the fetch command family (coupled-group exception, §4). |
| `detail/astring.h` | `detail::render_astring`/`render_quoted`/`render_literal` escaping helpers used by every command renderer. |
| `detail/unsolicited_table.h` | `detail::unsolicited_table` + `detail::registration` move-only token. |
| `detail/with_timeout.h` | `detail::with_timeout(sender, ioc, duration)` watchdog adaptor (mirrors every value signature of the wrapped sender, D9). |
| `detail/write_pump.h` | `detail::write_pump<Stream, Allocator>`: staging buffer, batching drain, literal wall, `MSG_NOSIGNAL` write chain. |
| `detail/read_pump.h` | `detail::read_pump<Stream, Allocator>`: permanent `async_read_some` loop, lexer drive, dispatch into registry + unsolicited table. |
| `detail/submit_sender.h` | `detail::submit_sender` / `detail::submit_operation`: the sender half of `imap_context::submit<Operation>(args...)`, templated on the context type so the include direction stays one-way. |

### 3.6 `include/bkmail/common/detail/` — cross-module helpers (all [H] [detail])

| File | Contents |
| --- | --- |
| `common/detail/unique_function.h` | `detail::unique_function<R(Args...)>` minimal move-only function wrapper (handler storage). |
| `common/detail/allocator_ext.h` | `detail::rebind_alloc_t`, `detail::string_of<Allocator>`, `detail::vector_of<T, Allocator>` aliases used across modules. |

### 3.7 `src/`

| File | Contents |
| --- | --- |
| `version.cpp` (exists) | `bkmail::version()` implementation. Unchanged. |
| `error.cpp` | `bkmail::error_category()` singleton, message strings, `make_error_code(errc)`. |
| `capabilities.cpp` | Non-inline `capability_set` internals: ASCII case-insensitive compare/intern helpers shared by every TU that parses CAPABILITY lines. |

No other `.cpp` files: everything else is templated on `Allocator` (and
usually `Stream`) and therefore lives in headers. If implementation
discovers a non-template, non-inline-worthy body (candidate: the response
parser's atom tables), it goes in `src/` under the same module path
(`src/imap/detail/response_parser.cpp`) and is added to the explicit source
list (§6).

### 3.8 `tests/`

The existing `bkmail_add_gtest(target prefix)` function is reused; one test
binary per module area, one subdirectory per area, each with its own
`CMakeLists.txt` calling the function. As implemented (258 tests
registered with ctest — none of them needs the network):

```
tests/
  CMakeLists.txt            (exists; one add_subdirectory per area)
  support/                  shared test infrastructure (no test binary):
      fake_imap_server.h    — scripted IMAP server on loopback (bnio acceptor)
      io_runner.h           — io_context-on-a-thread helper + timeouts
      scripted_stream.h     — in-memory stream double over bexec::run_loop,
                              modelling detail::imap_stream for Layer-1 tests
  basic/                    (exists; test_version.cpp stays)
  model/                    test_capability_set.cpp, test_data_model.cpp,
      test_detail.cpp, test_error.cpp, test_fetch_items.cpp, test_flags.cpp,
      test_mailbox.cpp, test_message_attributes.cpp, test_raw_response.cpp,
      test_search_criteria.cpp, test_sequence_set.cpp
      — data-model round trips: envelope/address/body_structure/mail_header
        + the RFC 2047 decoder, flags, sequence_set, error category
  proto/                    test_astring.cpp, test_parse_cursor.cpp,
      test_response_lexer.cpp, test_response_parser.cpp
      — corpus-driven: literals spanning reads, quoted escapes, {n+},
        unknown resp-codes skipped, interleaving, out-of-order tagged,
        untagged BYE mid-command, astring rendering
  layer1/                   test_imap_context.cpp, test_unsolicited.cpp,
      test_write_pump.cpp (+ layer1_fixture.h)
      — scripted in-memory stream double: batching (N submits -> 1 write),
        literal wall, registry cleanup, the three cancellation
        granularities, type-erased batching, shell destruction with pending
        handlers, the unsolicited table (whole-variant dispatch, FIFO
        attribution, token unregistration)
  layer2/                   test_connect.cpp, test_state_machine.cpp
      — connect entry points and state transitions: greeting OK/PREAUTH/BYE
        branches, compile-time completion-signature tables (one set_value
        signature per successor state, D9), NO keeps state, fatal ec
        invalidates session, EXPUNGE snapshot updates, seriality assert,
        STARTTLS upgrade on a scripted pair
  integration/              test_command_layer.cpp, test_error_paths.cpp,
      test_idle_unsolicited.cpp, test_protocol_robustness.cpp,
      test_session_flow.cpp
      — loopback fake server: login/select/fetch with literals, APPEND
        continuation, pipelined commands, IDLE push + DONE, unsolicited
        flow, error paths, protocol robustness. The live-server
        connectivity check lives in examples/real_server_session instead
        (kept out of the test suite per project decision).
```

### 3.9 `examples/`

One directory per example, one `main.cpp` each; `examples/CMakeLists.txt`
has one executable stanza per directory (same shape as `hello_bkmail`).

```
examples/
  hello_bkmail/           prints the bkmail version string
  list_subjects/          — usage.md §1.3 flow, blocking style (TLS, login,
                            select, fetch_envelopes, logout)
  idle_watch/             — watch_inbox loop of usage.md §2.6 with a stop
                            token wired to SIGINT
  batch_commands/         — command-layer make_command batching + flush
                            (usage.md §3.2–§3.6)
  real_server_session/    — read-only live-server connectivity session
                            (kept out of ctest; prints the mailbox list,
                            the newest subjects, and the leading lines of
                            the two newest messages)
```

## 4. The ~400-line soft limit in practice

Expected largest files and their controls, as implemented:

| File | Estimate | Control |
| --- | --- | --- |
| `imap/imap_context.h` | ~930 | Largest header: the `imap_context` shell and its `detail::context_core` form one coupled lifetime group (architecture §3.2a) and stay in one file on purpose. The two pumps are extracted to `detail/write_pump.h` / `detail/read_pump.h` as friend-scoped helpers owning the algorithms, and the sender path lives in `detail/submit_sender.h` (a coupled-type split, not fragmentation). |
| `imap/operation_base.h` | ~700 | The whole erasure group (operation_base/operation_sink/operation_model/op_deleter/io_box/concepts) is one internal contract between `imap_context` and the command files; splitting it would scatter a single invariant set. Coupled-group exception. |
| `imap/detail/response_parser.h` | 350–400 | Shallow parse only by design (ENVELOPE/BODYSTRUCTURE deep parse lives in `detail/fetch_parse.h`). The resp-code atom table, if it proves non-templatable, moves to `src/imap/detail/response_parser.cpp`. |
| `command/fetch_command.h` family | 200–260 each | Deep BODYSTRUCTURE/ENVELOPE parsing is shared through `imap/detail/fetch_parse.h` (created on evidence, as planned); `detail/parse_cursor.h` is its standalone cursor base. |
| `state/selected.h` | 300–350 | Twelve operation methods, each a thin builder over one command type; the sender glue is a single `detail::state_op_sender` template in `imap/state/detail/state_op_sender.h` shared by all four states (one coupled group, together with `idle_op_sender`). |
| `imap/detail/response_lexer.h` | ~150 | Single-purpose cursor; no control needed. |

The coupled-types exception of code_style.md is exercised deliberately in
`response.h` (six wire types that only make sense together),
`unsolicited_event.h` (the seven-event variant set), `session_state.h`
(four forward declarations, D9), `operation_base.h` and
`imap/detail/fetch_parse.h`. No other sharing is planned.

## 5. Naming master table

This section is the implementation contract: names below are final. All
types are templates with trailing `class Allocator =
std::allocator<std::byte>` unless marked non-template. Namespace is
`bkmail::imap` unless marked `bkmail::`.

### 5.1 Core and data model (`bkmail::`)

| Name | Kind | Notes |
| --- | --- | --- |
| `version()` | fn (exists) | |
| `errc` | enum class (non-template) | `command_rejected=1`, `bad_command`, `server_bye`, `unexpected_response`, `capability_required` |
| `error_category()` | fn | `const std::error_category&` |
| `account_info<A>` | struct | `user_name`, `password`, `authzid` (`std::optional<string>`) |
| `address<A>` | struct | `display_name`, `adl`, `mailbox_name`, `host_name`; `email()` |
| `envelope<A>` | struct | `date`, `subject`, `from`, `sender`, `reply_to`, `to`, `cc`, `bcc`, `in_reply_to`, `message_id` |
| `body_structure<A>` | struct | `media_type`, `subtype`, `parameters`, `id`, `description`, `encoding`, `octets`, `parts` |
| `mail_header<A>` | struct | `subject`, `from`, `to`, `cc`, `bcc`, `date`, `message_id`, `in_reply_to` |
| `mail_body<A>` | struct | `content_type`, `data` (`vector<byte, A>`) |
| `mail<A>` | struct | `header`, `body`, `envelope` (`std::optional`) |
| `pack(sender)` | fn adaptor | tuple-packs multi-value completions |
| `async_connect(host, service, ioc, alloc)` | free fn | branching sender (D9): `set_value(ec, not_authenticated_state<A>)` / `set_value(ec, authenticated_state<A>)` / `set_value(ec, logout_state<A>)` per greeting outcome + `set_stopped()` |
| `async_connect_tls(host, service, ioc, ssl_ctx, alloc)` | free fn | same; `ssl_ctx` borrowed |

### 5.2 Layer 1 (`bkmail::imap`)

**`imap_context<Stream, Allocator>`** — public surface:

| Member | Signature sketch |
| --- | --- |
| ctor | `imap_context(Stream stream, bnio::io_context& ioc, const Allocator& = {})` — arms the read pump |
| (deleted copy; idle-only move ctor) | per architecture §3.2 |
| `submit(cmd, handler)` | `template <class Command, class F> string_type submit(Command, F&&)` — stamps tag, queues, returns tag |
| `submit<Operation>(args...)` | sender convenience overload (D1): lazy sender of `set_value(ec, Operation::result_type)` / `set_stopped()` |
| `submit(vector<unique_ptr<imap_command<A>>>)` | `vector<string_type>` — erased batch, tags in vector order |
| `flush()` | one batched `async_write` of the pending queue |
| `cancel(tag)` | three-granularity per D3; IDLE cancel queues DONE (asynchronous, needs a live io_context) |
| `on_unsolicited(f)` | `template <class F> detail::registration on_unsolicited(F&&)` — one handler for the whole `unsolicited_event` variant |
| `set_io_context(ioc)` / `io_context()` | borrowing rules per architecture §3.6 |
| `is_alive()` / `close()` | close protocol per architecture §3.7 |
| `allocate_tag()` | `"a"` + zero-padded decimal counter (`a0001`, …), exposed for tests |

**Command types**: the 36 classes of §3.4, each with
`using result_type = ...;` as follows:

| Command | `result_type` |
| --- | --- |
| `capability_command` | `capability_set<A>` |
| `noop`, `logout`, `login`, `authenticate`, `starttls`, `create`, `delete`, `rename`, `subscribe`, `unsubscribe`, `append`, `close`, `expunge`, `store`, `copy`, `move`, `uid_store`, `uid_copy`, `uid_move`, `idle` | `void` (handler: `void(std::error_code)`) |
| `select_command`, `examine_command` | `mailbox_info<A>` |
| `list_command` | `std::vector<mailbox_entry<A>, …>` |
| `status_command` | `mailbox_status<A>` |
| `search_command`, `uid_search_command` | `std::vector<std::uint32_t, …>` |
| `fetch_envelopes_command`, `uid_fetch_envelopes_command` | `std::vector<envelope<A>, …>` |
| `fetch_headers_command`, `uid_fetch_headers_command` | `std::vector<mail_header<A>, …>` |
| `fetch_message_command`, `uid_fetch_message_command` | `std::vector<mail<A>, …>` |
| `fetch_command`, `uid_fetch_command` | `std::vector<message_attributes<A>, …>` |
| `raw_command` | `raw_response<A>` |

On both submission paths a tagged `NO` maps to
`make_error_code(errc::command_rejected)` and a tagged `BAD` to
`errc::bad_command` (the mapping lives in each command's `on_tagged`, shared
by the callback and sender paths — D2). The single deliberate exception is
`raw_command`: its NO/BAD are **not** mapped to an error code; the tagged
reply is delivered as `raw_response{tag, ok = false, text}` so callers can
interpret extension-specific outcomes themselves.

**Supporting types**: `message_flag` (`seen, answered, flagged, deleted,
draft, recent`), `flag_set` (`set/reset/test/any`, constexpr),
`store_mode` (`add, remove, replace`), `sequence_set`,
`capability_set<A>` (`contains(string_view)`, `begin/end`),
`mailbox_info<A>` (`name, exists, recent, unseen, uid_validity, uid_next,
flags, permanent_flags, read_only`), `mailbox_entry<A>` (`name, delimiter,
no_select, has_children, has_no_children`), `mailbox_status<A>` (`messages,
recent, uid_next, uid_validity, unseen`), `message_attributes<A>`,
`search_criteria<A>`, `fetch_items`, `raw_response<A>`, `response_status`,
`response_code`, `tagged_response<A>`, `untagged_response<A>`,
`continuation_request<A>`, `server_response<A>`, `imap_command<A>` +
`make_command(cmd, handler)`, the seven unsolicited event structs
(`exists_event`, `recent_event`, `expunge_event`, `flags_update_event`,
`capability_event<A>`, `bye_event<A>`, `greeting_event<A>`) +
`unsolicited_event<A>`.

### 5.3 Layer 2 (`bkmail::imap` states)

All operations are `&&`-qualified, return lazy senders completing
`set_value(std::error_code, [result,] successor_state)` / `set_stopped()`,
with **one `set_value` signature per successor state** on the branching
operations (D9). `capability()`, `noop()`, and `logout()` exist on the
three live states with successor = same state / same state / `logout_state`.

| State | Operations → successor (result in parentheses) |
| --- | --- |
| `not_authenticated_state<A>` | `login(account_info)` → `authenticated_state` on OK / **retained** `not_authenticated_state` on NO/BAD/failure (two signatures, D9) · `authenticate(mechanism, initial_response)` → same branching · `authenticate_oauth2(user, token)` → same branching · `start_tls()` → `not_authenticated_state` over TLS (single signature) · `capability()` → same (`capability_set`) · `noop()` → same · `logout()` → `logout_state` |
| `authenticated_state<A>` | `select(mailbox)` / `examine(mailbox)` → `selected_state` on OK / **retained** `authenticated_state` on NO/failure (two signatures, D9) · `list(ref, pattern)` → same (`vector<mailbox_entry>`) · `status(mailbox, items)` → same (`mailbox_status`) · `create/delete_mailbox/rename/subscribe/unsubscribe` → same · `append(mailbox, mail, flags)` → same · `capability()` / `noop()` → same · `logout()` → `logout_state` |
| `selected_state<A>` | `mailbox() const -> const mailbox_info<A>&` (not an operation) · `fetch_envelopes(seq)` → same (`vector<envelope>`) · `fetch_headers(seq, fields)` → same (`vector<mail_header>`) · `fetch_message(seq)` → same (`vector<mail>`) · `fetch(seq, items)` → same (`vector<message_attributes>`) · `search(criteria)` → same (`vector<uint32_t>`) · `store(seq, flags, store_mode)` → same · `copy(seq, mailbox)` → same · `move(seq, mailbox)` → same · `expunge()` → same · `close()` → `authenticated_state` · `idle()` → `selected_state` (drives an IDLE/DONE cycle internally; heartbeat per usage.md §2.6) · UID variants `uid_fetch_envelopes`, `uid_fetch_headers`, `uid_fetch_message`, `uid_fetch`, `uid_search`, `uid_store`, `uid_copy`, `uid_move` → same (same result shapes) · `capability()` / `noop()` → same · `logout()` → `logout_state` |
| `logout_state<A>` | — (terminal) |

On the state layer `idle()` is one operation that returns with the
`selected_state` when activity or the heartbeat arrives (the usage.md
§2.6 loop idiom); command-layer audiences drive `idle_command` directly
(architecture.md §9).

`imap_connection<A>`: `variant`-owning context holder (TCP/TLS), capability
cache (`capabilities()`), `close()`, idle-only move. Created only by
`bkmail::async_connect*` (which only ever create **live** connections);
the states carry it by value along the chain. There is no empty/placeholder
form (D9).

## 6. CMake plan

- **`include/CMakeLists.txt`: unchanged.** The family convention is
  `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` over `bkmail/*.h` (identical in
  bnio); new subdirectories (`imap/`, `imap/command/`, …) are picked up
  automatically. The `common/` directory (and `common/detail/`) added by
  the 2026-09-19 header migration is covered by the same glob — no
  CMake edit was needed for it. Keep it.
- **`src/CMakeLists.txt`: explicit source list, family-style.** Replace
  `target_sources(bkmail PRIVATE version.cpp ${BKMAIL_PUBLIC_HEADERS})`
  with an explicit list variable:
  ```cmake
  set(BKMAIL_SOURCES version.cpp error.cpp capabilities.cpp)
  target_sources(bkmail PRIVATE ${BKMAIL_SOURCES} ${BKMAIL_PUBLIC_HEADERS})
  ```
  Rationale: sources are few and change rarely; explicit lists match how
  bnio lists its per-directory sources, keep `install(EXPORT)` auditable,
  and avoid globbing compiled code. `${BKMAIL_PUBLIC_HEADERS}` stays in
  `target_sources` for IDE visibility only.
- **`tests/CMakeLists.txt`**: add `add_subdirectory(model)`, `(proto)`,
  `(layer1)`, `(layer2)`, `(integration)` after `basic`. Each
  subdirectory's `CMakeLists.txt` calls `bkmail_add_gtest(<target>
  <prefix>)` per test binary, e.g. `bkmail_add_gtest(test_response_lexer
  proto.lexer)`. The scripted stream double and the other shared fixtures
  live in `tests/support/` and are picked up through
  `target_include_directories(<test> PRIVATE ${PROJECT_SOURCE_DIR}/tests)`
  inside the helper function (extend `bkmail_add_gtest` to accept
  `EXTRA_INCLUDES`-style `ARGN` passthrough — it already forwards `ARGN` to
  `gtest_discover_tests`, so instead set the include dir per-subdirectory
  with a one-line `include_directories`; do not redesign the helper).
- **`examples/CMakeLists.txt`**: one stanza per example directory,
  mirroring `hello_bkmail`; factor a three-line `bkmail_add_example(name)`
  function only when the third example lands.
- No changes to dependency resolution, install rules, or packaging:
  header-only growth does not affect `install(DIRECTORY include/bkmail)`.

## 7. Implementation batches

Batch dependency edges: A → B → C → D; E depends on its module. Within a
batch all files are independent and may be implemented by parallel agents.

- **Batch A — core + model** (no dependencies):
  `error.h/.cpp`, `account_info.h`, `address.h`, `envelope.h`,
  `body_structure.h`, `mail_header.h`, `mail_body.h`, `mail.h`,
  `imap/flags.h`, `imap/sequence_set.h`, `imap/capability_set.h` +
  `src/capabilities.cpp`, `imap/mailbox_info.h`, `imap/mailbox_entry.h`,
  `imap/mailbox_status.h`, `imap/message_attributes.h`,
  `imap/search_criteria.h`, `imap/fetch_items.h`, `imap/raw_response.h`,
  `detail/allocator_ext.h`, `detail/unique_function.h`, `pack.h`.
  Tests: `tests/model/`.
- **Batch B — protocol engine** (needs A's response model types; the
  response wire types `imap/response.h` + `imap/unsolicited_event.h` belong
  to this batch's first file):
  `imap/response.h`, `imap/unsolicited_event.h`, `imap/detail/astring.h`,
  `imap/detail/response_lexer.h`, `imap/detail/response_parser.h`.
  Tests: `tests/proto/` + `tests/support/scripted_stream.h`.
- **Batch C — Layer 1** (needs A + B):
  `imap/operation_base.h`, `imap/detail/unsolicited_table.h`,
  `imap/detail/write_pump.h`, `imap/detail/read_pump.h`,
  `imap/imap_context.h`, then all 36 `imap/command/*.h` files (each command
  is independent), `imap/imap_command.h` + `make_command`,
  `imap/command.h` aggregate. Tests: `tests/layer1/`.
- **Batch D — Layer 2** (needs C):
  `imap/imap_connection.h`, `imap/session_state.h`,
  `imap/state/detail/state_op_sender.h`, the five `imap/state/*.h`,
  `imap/detail/with_timeout.h`, `connect.h`, `imap.h` aggregate,
  `bkmail.h` update. Tests: `tests/layer2/`, then `tests/integration/`.
- **Batch E — examples** (needs D, parallel with D's tests):
  `list_subjects`, `idle_watch`, `batch_commands`.

Hand-off notes for the orchestrator: batches A and B define every data
contract C consumes, so an interface drift in A/B must be reported before C
starts (per the project pipeline rule). Each batch is complete only when
its tests build and pass under `bkmail_add_gtest`.
