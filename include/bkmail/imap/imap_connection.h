/**
 * @file imap_connection.h
 * @brief Owns an IMAP connection across plaintext and TLS.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_IMAP_CONNECTION_H_
#define BKMAIL_IMAP_IMAP_CONNECTION_H_

#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/imap_context.h>
#include <bnio/io_context.h>
#include <bnio/ssl.h>
#include <bnio/tcp.h>

#include <atomic>
#include <bexec/scheduler.hpp>
#include <cassert>
#include <memory>
#include <new>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

namespace bkmail::imap {

template <class Allocator>
class imap_connection;

namespace detail {

/// Terminal teardown for the LOGOUT path (defined below); befriended so
/// it can inspect the connection's emptiness without a public query.
template <class Allocator>
void detain_connection(imap_connection<Allocator> conn) noexcept;

}  // namespace detail

/**
 * Owns the Layer-1 context across the plaintext/TLS type boundary.
 *
 * STARTTLS changes the context's type at runtime (the socket is moved into
 * an ssl_stream and a new context is built over it), so the connection
 * holds both context types in a variant and hides the switch behind
 * with_context(). The variant itself is heap-held: a live context has its
 * permanent read pump armed, and the armed I/O receivers pin the context
 * object's address (architecture §3.2/§2.5), so a context may NEVER be
 * relocated. Moving the connection (as the Layer-2 state chain does on
 * every operation) therefore only moves the owning pointer — the context
 * keeps its stable address for its whole lifetime.
 *
 * The capability set negotiated during connect is cached here and
 * invalidated by the STARTTLS upgrade (RFC 3501 requires re-issuing
 * CAPABILITY after STARTTLS).
 *
 * The connection is move-only, and may only be moved while idle (no
 * operation in flight): in-flight Layer-2 state pins the connection.
 * Layer-2 states carry the connection by value and pass it from state to
 * state; bkmail::async_connect / async_connect_tls are the only public
 * creators of a live connection.
 *
 * The io_context is borrowed, never owned; the caller keeps it (and a
 * thread running it) alive for as long as any operation may be in flight.
 */
template <class Allocator = std::allocator<std::byte>>
class imap_connection {
 public:
  using allocator_type = Allocator;

  /// Layer-1 context over a plaintext TCP stream.
  using tcp_context = imap_context<bnio::tcp::socket, Allocator>;

  /// Layer-1 context over an implicit-TLS or STARTTLS-upgraded stream.
  using tls_context =
      imap_context<bnio::ssl_stream<bnio::tcp::socket>, Allocator>;

 private:
  /// The heap-held context variant (see the class documentation: the
  /// indirection keeps the context's address stable across connection
  /// moves, which the armed read pump requires).
  using context_variant = std::variant<tcp_context, tls_context>;

 public:
  /// Builds the plaintext context over an already-connected socket and
  /// arms its read pump (see imap_context).
  imap_connection(bnio::tcp::socket stream, bnio::io_context& ioc,
                  const Allocator& alloc = Allocator{})
      : context_(std::make_unique<context_variant>(
            std::in_place_type<tcp_context>, std::move(stream), ioc, alloc)),
        capabilities_(alloc),
        ioc_(&ioc),
        alloc_(alloc) {}

  /// Builds the TLS context over an already-connected, already-handshaken
  /// stream and arms its read pump.
  imap_connection(bnio::ssl_stream<bnio::tcp::socket> stream,
                  bnio::io_context& ioc, const Allocator& alloc = Allocator{})
      : context_(std::make_unique<context_variant>(
            std::in_place_type<tls_context>, std::move(stream), ioc, alloc)),
        capabilities_(alloc),
        ioc_(&ioc),
        alloc_(alloc) {}

  imap_connection(const imap_connection&) = delete;
  imap_connection& operator=(const imap_connection&) = delete;

  /// Moves the connection (the context object itself is NOT relocated;
  /// only the owning pointer travels). @pre other is idle (no operation
  /// in flight).
  imap_connection(imap_connection&& other) noexcept
      : context_(std::move(other.context_)),
        capabilities_(std::move(other.capabilities_)),
        ioc_(other.ioc_),
        alloc_(other.alloc_) {
    assert(!other.busy_.load(std::memory_order_acquire) &&
           "imap_connection moved while an operation is in flight");
  }

  /// Move-assigns the connection. @pre Both objects are idle; the previous
  /// content (a moved-from, empty, or properly closed connection) is
  /// destroyed in place.
  imap_connection& operator=(imap_connection&& other) noexcept {
    assert(!busy_.load(std::memory_order_acquire) &&
           !other.busy_.load(std::memory_order_acquire) &&
           "imap_connection move-assigned while an operation is in flight");
    context_ = std::move(other.context_);
    capabilities_ = std::move(other.capabilities_);
    ioc_ = other.ioc_;
    alloc_ = other.alloc_;
    return *this;
  }

  /// Returns the allocator the connection and its contexts were built with.
  [[nodiscard]] Allocator get_allocator() const noexcept { return alloc_; }

  /// Returns the cached capability set negotiated during connect (or after
  /// the last capability() operation / STARTTLS re-probe).
  [[nodiscard]] const capability_set<Allocator>& capabilities() const noexcept {
    return capabilities_;
  }

  /// Replaces the cached capability set (Layer-2 capability() bookkeeping).
  void set_capabilities(capability_set<Allocator> caps) {
    capabilities_ = std::move(caps);
  }

  /// Returns the io_context the connection's next round of I/O borrows.
  /// @pre The connection is not empty.
  [[nodiscard]] bnio::io_context& io_context() const noexcept {
    assert(ioc_ != nullptr);
    return *ioc_;
  }

  /// Re-points the active context at @p ioc for the next round of I/O.
  /// @pre No operation in flight (docs/architecture.md §3.6).
  void set_io_context(bnio::io_context& ioc) noexcept {
    with_context([&ioc](auto& ctx) { ctx.set_io_context(ioc); });
    ioc_ = &ioc;
  }

  /// Returns whether the active context's read pump is still running.
  [[nodiscard]] bool is_alive() const noexcept {
    return context_ != nullptr &&
           with_context([](auto& ctx) { return ctx.is_alive(); });
  }

  /// Runs the close protocol on the active context (§3.7). Safe on an
  /// empty connection.
  void close() noexcept {
    if (context_ != nullptr) {
      with_context([](auto& ctx) { ctx.close(); });
    }
  }

  /**
   * Applies @p f to the active Layer-1 context, whichever transport it
   * wraps. This is the Layer-2 operation driver's single entry point into
   * Layer 1 (submit/flush/cancel/on_unsolicited).
   *
   * @pre The connection is not empty.
   */
  template <class F>
  decltype(auto) with_context(F&& f) {
    assert(context_ != nullptr && "with_context on an empty imap_connection");
    return std::visit(std::forward<F>(f), *context_);
  }

  /// @copydoc with_context
  template <class F>
  decltype(auto) with_context(F&& f) const {
    assert(context_ != nullptr && "with_context on an empty imap_connection");
    return std::visit(std::forward<F>(f), *context_);
  }

  /**
   * Detaches the plaintext socket and layers an ssl_stream over it
   * (STARTTLS, architecture §8.2, step 1 of 2).
   *
   * Moves the tcp::socket out of the plaintext context (the context stays
   * behind as an inert shell until emplace_tls_context() replaces it) and
   * returns the TLS stream ready for async_handshake. Splitting the upgrade
   * keeps the new context's read pump from being armed before the
   * handshake has run.
   *
   * @pre The connection holds the plaintext context and is idle.
   * @pre @p ssl outlives the upgraded connection.
   */
  [[nodiscard]] bnio::ssl_stream<bnio::tcp::socket> upgrade_to_tls_stream(
      bnio::ssl_context& ssl) {
    assert(context_ != nullptr &&
           std::holds_alternative<tcp_context>(*context_) &&
           "upgrade_to_tls_stream requires the plaintext context");
    auto& plain = std::get<tcp_context>(*context_);
    // Suspend BEFORE releasing the stream: the read pump must stop
    // re-arming (it unwinds after the in-progress dispatch returns),
    // otherwise the old context would keep a read armed on a moved-from
    // socket and trip its teardown assertions when emplace_tls_context()
    // destroys it.
    plain.suspend_read_for_relocation();
    // Move the socket out of the plaintext context (the context is idle
    // between the STARTTLS reply and the handshake, so its pumps pin
    // nothing) and layer the TLS stream over it.
    return bnio::ssl_stream<bnio::tcp::socket>{plain.release_stream(), ssl};
  }

  /**
   * Replaces the inert plaintext shell with a TLS context over the
   * handshaken stream (STARTTLS, architecture §8.2, step 2 of 2). The new
   * context's read pump arms immediately; the old context is destroyed in
   * place (it is quiesced: the upgrade suspended its read side and no
   * operation was in flight). The capability cache is cleared; the caller
   * re-probes CAPABILITY afterwards (RFC 3501).
   */
  [[nodiscard]] tls_context& emplace_tls_context(
      bnio::ssl_stream<bnio::tcp::socket> stream) {
    context_->template emplace<tls_context>(std::move(stream), *ioc_, alloc_);
    capabilities_ = capability_set<Allocator>{alloc_};
    return std::get<tls_context>(*context_);
  }

  // ---- Layer-2 seriality guard (internal; not public API) -------------

  /// Acquires the one-operation-in-flight slot. Returns false when the
  /// connection is already busy (a Layer-2 contract violation).
  [[nodiscard]] bool try_start_operation() noexcept {
    return !busy_.exchange(true, std::memory_order_acq_rel);
  }

  /// Releases the one-operation-in-flight slot.
  void finish_operation() noexcept {
    busy_.store(false, std::memory_order_release);
  }

 private:
  template <class A>
  friend void detail::detain_connection(imap_connection<A>) noexcept;

  // Heap-held so the context's address is stable across connection moves:
  // the permanent read pump pins the context object (architecture §3.2).
  // nullptr only in a moved-from connection.
  std::unique_ptr<context_variant> context_;
  capability_set<Allocator> capabilities_;
  bnio::io_context* ioc_ = nullptr;  // borrowed
  [[no_unique_address]] Allocator alloc_{};
  std::atomic<bool> busy_ = false;
};

namespace detail {

/**
 * Terminal teardown for the LOGOUT path: initiates the close protocol and
 * hands the connection to a scheduled task that destroys it one event-loop
 * turn later. The LOGOUT completion is delivered from inside the read
 * dispatch, and destroying a context there is unsafe (the pump still
 * references it until the dispatch unwinds and the pumps go quiet).
 * Should the io_context never run again, the box (and the connection)
 * leaks rather than dangling — the session is over either way.
 */
template <class Allocator>
void detain_connection(imap_connection<Allocator> conn) noexcept {
  if (conn.context_ == nullptr || conn.ioc_ == nullptr) {
    return;  // Empty connection: nothing to close, nothing to detain.
  }
  conn.close();
  try {
    auto* box =
        make_io_box(conn.get_allocator(),
                    bexec::schedule(conn.io_context().get_post_scheduler()),
                    [&conn](io_box_base* self) {
                      struct teardown_receiver {
                        imap_connection<Allocator> conn;
                        io_box_base* box;

                        // The dispatch that delivered the LOGOUT reply has
                        // unwound by the time this runs; the close protocol has
                        // quieted the pumps, so the connection (destroyed with
                        // the box) may finally go away.
                        void set_value(std::error_code) noexcept {
                          box->dispose();
                        }
                        void set_stopped() noexcept { box->dispose(); }
                      };
                      return teardown_receiver{std::move(conn), self};
                    });
    box->start();
  } catch (...) {
    // Allocation or scheduling failure: the connection destructs here.
    // The close protocol already ran; on a drained io_context this is as
    // good as teardown gets.
  }
}

}  // namespace detail

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_IMAP_CONNECTION_H_
