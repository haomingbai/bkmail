/**
 * @file include/bkmail/imap/detail/unsolicited_table.h
 * @brief Unsolicited-event handler table and registration token.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details [self-built] handler registry behind
 * `imap_context::on_unsolicited` (code_layout §5.1): one handler receives
 * the whole `unsolicited_event` variant; registering returns a move-only
 * `registration` token whose destruction unregisters.
 *
 * The table owns its entries through a heap-allocated anchor whose address
 * is stable across table moves, so registrations survive the context's
 * idle move (STARTTLS relocation). Removal only marks an entry inactive;
 * memory is reclaimed on the next `add()` or at destruction, so a token
 * destroyed while a dispatch snapshot is in flight never frees a handler
 * mid-call.
 *
 * Threading: mirrors the context contract (architecture §6) — handlers
 * run inline on the read-dispatch path on an arbitrary worker thread, must
 * not block and must not call back into the context. Registering or
 * destroying a token concurrently with dispatch is unsupported; the
 * internal mutex keeps the entry list coherent, it is not a public
 * thread-safety promise.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_UNSOLICITED_TABLE_H_
#define BKMAIL_IMAP_DETAIL_UNSOLICITED_TABLE_H_

#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/unsolicited_event.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace bkmail::imap::detail {

template <class Allocator>
class unsolicited_table;

namespace unsolicited_detail {

/// Move-stable store behind `unsolicited_table`; registration tokens point
/// here so they survive a move of the table (context relocation).
template <class Allocator>
struct anchor {
  struct entry_base {
    entry_base() = default;
    entry_base(const entry_base&) = delete;
    entry_base& operator=(const entry_base&) = delete;
    virtual ~entry_base() = default;

    virtual void notify(const unsolicited_event<Allocator>& event) = 0;

    /// Destroys and deallocates the cell with `alloc` rebound to the
    /// concrete entry type.
    virtual void dispose(const Allocator& alloc) noexcept = 0;

    std::uint64_t id = 0;
    bool active = true;
  };

  template <class F>
  struct entry final : entry_base {
    explicit entry(F&& f) : handler(std::move(f)) {}

    void notify(const unsolicited_event<Allocator>& event) override {
      handler(event);
    }

    void dispose(const Allocator& alloc) noexcept override {
      using entry_alloc = rebind_alloc_t<Allocator, entry>;
      entry_alloc rebound(alloc);
      this->~entry();
      std::allocator_traits<entry_alloc>::deallocate(rebound, this, 1);
    }

    F handler;
  };

  explicit anchor(const Allocator& alloc)
      : entries(rebind_alloc_t<Allocator, entry_base*>(alloc)), alloc(alloc) {}

  anchor(const anchor&) = delete;
  anchor& operator=(const anchor&) = delete;

  ~anchor() {
    for (auto* cell : entries) {
      cell->dispose(alloc);
    }
  }

  /// Marks the entry inactive; memory is reclaimed lazily so an in-flight
  /// notify snapshot never dangles.
  void deactivate(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex);
    for (auto* cell : entries) {
      if (cell->id == id) {
        cell->active = false;
        return;
      }
    }
  }

  /// Frees every inactive entry. Caller must hold `mutex`.
  void compact_locked() noexcept {
    std::size_t out = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->active) {
        entries[out++] = entries[i];
      } else {
        entries[i]->dispose(alloc);
      }
    }
    entries.resize(out);
  }

  mutable std::mutex mutex;
  std::vector<entry_base*, rebind_alloc_t<Allocator, entry_base*>> entries;
  std::uint64_t next_id = 1;
  [[no_unique_address]] Allocator alloc;
};

}  // namespace unsolicited_detail

/**
 * Move-only registration token for one unsolicited-event handler.
 * Destroying the token unregisters the handler. The token shares
 * ownership of the table's anchor, so it may safely outlive the owning
 * context (e.g. a moved-from selected_state whose connection — and with
 * it the context core — was handed to the next operation before the
 * token's destruction runs).
 */
template <class Allocator = std::allocator<std::byte>>
class registration {
 public:
  registration() noexcept = default;
  registration(const registration&) = delete;
  registration& operator=(const registration&) = delete;

  registration(registration&&) noexcept = default;

  registration& operator=(registration&& other) noexcept {
    if (this != &other) {
      reset();
      anchor_ = std::move(other.anchor_);
      id_ = std::exchange(other.id_, 0);
    }
    return *this;
  }

  /// Unregisters the handler, if still registered.
  ~registration() { reset(); }

  /// True while this token refers to a live registration.
  [[nodiscard]] explicit operator bool() const noexcept {
    return anchor_ != nullptr;
  }

  /// Unregisters now instead of at destruction.
  void reset() noexcept {
    if (anchor_ != nullptr) {
      anchor_->deactivate(id_);
      anchor_.reset();
      id_ = 0;
    }
  }

 private:
  friend class unsolicited_table<Allocator>;

  registration(std::shared_ptr<unsolicited_detail::anchor<Allocator>> a,
               std::uint64_t id) noexcept
      : anchor_(std::move(a)), id_(id) {}

  std::shared_ptr<unsolicited_detail::anchor<Allocator>> anchor_;
  std::uint64_t id_ = 0;
};

/**
 * Owning table of unsolicited-event handlers. Add via `add()`, dispatch
 * via `notify()`; handlers are type-erased cells allocated with the
 * table's rebound allocator.
 */
template <class Allocator = std::allocator<std::byte>>
class unsolicited_table {
 public:
  using allocator_type = Allocator;
  using event_type = unsolicited_event<Allocator>;
  using token_type = registration<Allocator>;

  explicit unsolicited_table(const Allocator& alloc = Allocator{})
      : anchor_(
            std::make_shared<unsolicited_detail::anchor<Allocator>>(alloc)) {}

  unsolicited_table(const unsolicited_table&) = delete;
  unsolicited_table& operator=(const unsolicited_table&) = delete;

  /// Moves the table. The anchor address is stable, so existing
  /// registration tokens stay valid.
  unsolicited_table(unsolicited_table&&) noexcept = default;
  unsolicited_table& operator=(unsolicited_table&&) noexcept = default;

  ~unsolicited_table() = default;

  /**
   * Registers @p f for every unsolicited event. The handler is invoked as
   * `f(const unsolicited_event<Allocator>&)` inline on the read-dispatch
   * path.
   */
  template <class F>
    requires std::invocable<F&, const event_type&>
  [[nodiscard]] token_type add(F&& f) {
    auto& store = *anchor_;
    using entry_t = typename unsolicited_detail::anchor<
        Allocator>::template entry<std::remove_cvref_t<F>>;
    using entry_alloc = rebind_alloc_t<Allocator, entry_t>;
    entry_alloc alloc(store.alloc);

    std::lock_guard lock(store.mutex);
    store.compact_locked();
    entry_t* cell = std::allocator_traits<entry_alloc>::allocate(alloc, 1);
    std::allocator_traits<entry_alloc>::construct(alloc, cell,
                                                  std::forward<F>(f));
    cell->id = store.next_id++;
    store.entries.push_back(cell);
    return token_type{anchor_, cell->id};
  }

  /**
   * Invokes every active handler with @p event. Handler pointers are
   * snapshotted under the lock; the calls themselves run unlocked. Entry
   * storage is never freed by `reset()` during dispatch (mark-and-sweep),
   * so the snapshot stays valid even when a token is destroyed from inside
   * a handler on the dispatch thread.
   */
  void notify(const event_type& event) {
    auto& store = *anchor_;
    std::vector<typename unsolicited_detail::anchor<Allocator>::entry_base*>
        snapshot;
    {
      std::lock_guard lock(store.mutex);
      snapshot.reserve(store.entries.size());
      for (auto* cell : store.entries) {
        if (cell->active) {
          snapshot.push_back(cell);
        }
      }
    }
    for (auto* entry : snapshot) {
      entry->notify(event);
    }
  }

  /// Number of live registrations (diagnostics/tests).
  [[nodiscard]] std::size_t size() const {
    auto& store = *anchor_;
    std::lock_guard lock(store.mutex);
    std::size_t count = 0;
    for (auto* cell : store.entries) {
      if (cell->active) {
        ++count;
      }
    }
    return count;
  }

 private:
  // Shared with the registration tokens: a token may outlive this table
  // (and the context core owning it), so the anchor is co-owned rather
  // than solely table-owned.
  std::shared_ptr<unsolicited_detail::anchor<Allocator>> anchor_;
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_UNSOLICITED_TABLE_H_
