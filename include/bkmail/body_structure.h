/**
 * @file include/bkmail/body_structure.h
 * @brief MIME BODYSTRUCTURE tree node.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_BODY_STRUCTURE_H_
#define BKMAIL_BODY_STRUCTURE_H_

#include <bkmail/detail/allocator_ext.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace bkmail {

/**
 * @brief Recursive MIME BODYSTRUCTURE tree node.
 *
 * Leaf nodes describe one body part (`media_type`/`subtype`, content
 * `parameters`, `encoding`, `octets`); multipart nodes own their children
 * in `parts`. Wire `NIL` strings parse to empty strings (code_layout D8).
 */
template <class Allocator = std::allocator<std::byte>>
struct body_structure {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// (attribute, value) content-parameter pair container.
  using parameter_list =
      detail::vector_of<std::pair<string_type, string_type>, Allocator>;
  /// Child node container (multipart nodes only).
  using parts_container = detail::vector_of<body_structure, Allocator>;

  /// MIME top-level media type, e.g. `"text"`, `"multipart"`.
  string_type media_type;
  /// MIME media subtype, e.g. `"html"`, `"mixed"`.
  string_type subtype;
  /// Content-type parameter attribute/value pairs.
  parameter_list parameters;
  /// Content-ID; empty when NIL.
  string_type id;
  /// Content-Description; empty when NIL.
  string_type description;
  /// Content-Transfer-Encoding, e.g. `"quoted-printable"`, `"base64"`.
  string_type encoding;
  /// Body size in octets (64-bit per architecture §2 protocol facts).
  std::uint64_t octets = 0;
  /// Children for multipart types; empty for leaf nodes.
  parts_container parts;
};

}  // namespace bkmail

#endif  // BKMAIL_BODY_STRUCTURE_H_
