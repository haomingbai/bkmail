# bkmail Code Style

This document defines the code style for the bkmail repository. It is based
on the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
and adds project-specific rules. Where this document is silent, the Google
guide applies. bkmail follows the same conventions as its sibling libraries
[bnio](https://github.com/haomingbai/bnio) and
[bexec](https://github.com/haomingbai/bexec) so the whole family reads alike.

Formatting is enforced mechanically: the repository root `.clang-format`
contains `BasedOnStyle: Google`. Run `clang-format` over everything you
touch; CI rejects unformatted code.

## File header banner

Every source file (`.h`, `.hpp`, `.cpp`, `CMakeLists.txt`, `*.cmake`) starts
with a banner comment. The full form is:

```cpp
/**
 * @file message.h
 * @brief MIME message representation.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */
```

- `@file` is the file name (family libraries sometimes use the
  repository-relative path, e.g. `include/bexec/task.hpp`; either is
  accepted, but plain file names match the existing bkmail headers).
- `@brief` is a single line describing what the file provides.
- `@date` uses ISO format `YYYY-MM-DD`, aligned with two spaces after
  `@date` so the values line up with `@author`.
- The copyright line uses the `©` symbol and is followed by
  `SPDX-License-Identifier: MIT`.
- For `CMakeLists.txt` / `*.cmake` files the same banner is written with
  `#` comment markers (see the top-level `CMakeLists.txt`).
- An optional `@details` block may follow for longer descriptions.

Short internal helpers may use the reduced form (`@file` + `@brief` only),
as seen in `include/bkmail/export.h`. New public headers should use the full
form.

## Header guards

Headers use both `#pragma once` and a traditional include guard:

```cpp
#pragma once
#ifndef BKMAIL_MESSAGE_H_
#define BKMAIL_MESSAGE_H_

// ...

#endif  // BKMAIL_MESSAGE_H_
```

The guard name is `BKMAIL_` followed by the path of the header relative to
`include/bkmail/`, uppercased, with `/` and `.` replaced by `_` (e.g.
`include/bkmail/mime/message.h` → `BKMAIL_MIME_MESSAGE_H_`). The `#endif`
carries a trailing comment with the guard name.

## Naming

| Entity | Convention | Example |
| --- | --- | --- |
| Types (class/struct/enum/alias) | `snake_case` | `message`, `dynamic_byte_vector_buffer` |
| Functions and variables | `snake_case` | `version()`, `content_type` |
| Member variables | trailing underscore | `storage_`, `socket_` |
| Template parameters | `CamelCase` | `class Allocator`, `class Receiver` |
| Macros and guard names | `UPPER_SNAKE` | `BKMAIL_EXPORT`, `BKMAIL_VERSION_H_` |
| Enum constants | `snake_case` | `enum class version { unspecified, v4, v6 }` |
| Compile-time constants | `kCamelCase` | `kMaxHeaderSize` |

This matches the bnio/bexec family (`bnio::async_io::ip::address`,
`bexec::task`).

## Include order

Includes are grouped and sorted by `clang-format` (Google style). Within a
group, order is alphabetical; groups are separated by one blank line:

1. Related header (in `.cpp` files): `#include <bkmail/message.h>`.
2. Project and family headers: `<bkmail/...>`, `<bexec/...>`, `<bnio/...>`.
3. Third-party headers: `<gtest/gtest.h>`, Boost, etc.
4. C++ standard library headers: `<string>`, `<vector>`, `<memory>`.

All includes use angle brackets and repository-rooted paths
(`<bkmail/version.h>`), never relative includes (`"../export.h"`).

## Namespaces

- All public code lives in `namespace bkmail`. Sub-features may add one
  nested level (`namespace bkmail::mime`), mirroring bnio's
  `bnio::async_io::ip` layout.
- C++17 nested namespace syntax is welcome (`namespace bkmail::mime {`).
- Every namespace closing brace carries a comment: `}  // namespace bkmail`.
- Implementation details go in `namespace bkmail::detail` or an anonymous
  namespace inside the `.cpp` file; tests use an anonymous namespace.
- Never write `using namespace` in headers.

## File organization

- **One type per file** wherever practical: a public class `foo` lives in
  `include/bkmail/foo.h` with its implementation in `src/foo.cpp`.
- **Soft size limit of ~400 lines per file.** This is a readability target,
  not a hard gate — split a file when it grows well past the limit, but do
  not fragment naturally cohesive code just to stay under it.
- **Tightly coupled types may share a file.** A small group of types that
  only make sense together (e.g. a buffer view and its owning storage
  adapter, as in bnio's `buffer/basic.h`) may live in one header; name the
  file after the group.
- Aggregate headers (`bkmail.h`, and per-module headers like bnio's `ip.h`)
  only include other headers and forward-declare/alias; they contain no
  logic.

## Error handling

- Public API functions are `noexcept` unless they allocate or can genuinely
  fail; mark them `[[nodiscard]]` when ignoring the result is a bug.
- Report programmer errors (contract violations) with `assert`.
- Report runtime failures either with exceptions deriving from
  `std::exception` (family style, e.g. `bexec::task_stopped`) or with
  `std::error_code` / `std::expected` out-parameters for async paths —
  pick one style per interface and be consistent. Do not mix exceptions and
  error codes for the same operation.
- Asynchronous operations follow the bnio/bexec sender model. bnio's
  completion contract has **no `set_error` channel**: outcomes are delivered
  as `set_value(std::error_code, ...)` and cancellation as `set_stopped()`.
  bkmail follows the same convention end to end — async failures travel in
  the value channel as `std::error_code`, never thrown across coroutine or
  callback boundaries.

## Allocator support

The library is generic over allocators, following bnio's buffer design:

- Classes that own dynamic storage take a trailing template parameter
  `template <class Allocator = std::allocator<T>>` so the common case needs
  no spelling.
- The allocator is stored by value (typically via the container it
  configures) and propagated on copy/move/swap according to
  `std::allocator_traits`.
- Factory helpers deduce the allocator from the argument
  (`template <class Allocator> buffer(std::vector<std::byte, Allocator>&)`),
  so callers never name the template parameter explicitly.
- Public functions that produce strings/containers either return
  allocator-aware types or take the container by reference.

## Public API documentation

- Every public header carries the full banner (see above).
- Every public type, function, and enumerator has a Doxygen comment:
  `/** ... */` blocks for types and multi-line descriptions, `///` for
  one-liners. Start with `@brief` or an imperative one-sentence summary.
- Public symbols exported from a shared library are annotated
  `BKMAIL_EXPORT` (see `include/bkmail/export.h`).
- Document ownership, lifetime, thread-safety, and allocator propagation
  whenever they are not obvious from the signature.

## Example

```cpp
/**
 * @file address.h
 * @brief Generic IPv4/IPv6 address type.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_ADDRESS_H_
#define BKMAIL_ADDRESS_H_

#include <bkmail/export.h>

#include <array>
#include <cstdint>

namespace bkmail {

/// Value type representing an IPv4 or IPv6 address.
class BKMAIL_EXPORT address {
 public:
  /// IPv4 address bytes in network byte order.
  using v4_bytes = std::array<std::uint8_t, 4>;

  /// Creates an unspecified address.
  address() noexcept;

  /// Returns true when the address holds a valid family.
  [[nodiscard]] bool is_valid() const noexcept { return valid_; }

 private:
  bool valid_ = false;
};

}  // namespace bkmail

#endif  // BKMAIL_ADDRESS_H_
```

And an allocator-aware owning type:

```cpp
template <class Allocator = std::allocator<std::byte>>
class BKMAIL_EXPORT message_storage {
 public:
  using container = std::vector<std::byte, Allocator>;

  explicit message_storage(const Allocator& alloc = Allocator{})
      : bytes_(alloc) {}

  /// Returns the underlying byte container.
  [[nodiscard]] container& bytes() noexcept { return bytes_; }

 private:
  container bytes_;
};
```
