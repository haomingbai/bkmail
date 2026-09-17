/**
 * @file tests/model/test_data_model.cpp
 * @brief Unit tests for the bkmail mail data model aggregates and the
 * RFC 2047 encoded-word decoder.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/account_info.h>
#include <bkmail/address.h>
#include <bkmail/body_structure.h>
#include <bkmail/envelope.h>
#include <bkmail/mail.h>
#include <bkmail/mail_body.h>
#include <bkmail/mail_header.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <type_traits>

namespace {

// -- address -----------------------------------------------------------------

TEST(Address, DefaultConstructedFieldsAreEmpty) {
  const bkmail::address<> addr;
  EXPECT_TRUE(addr.display_name.empty());
  EXPECT_TRUE(addr.adl.empty());
  EXPECT_TRUE(addr.mailbox_name.empty());
  EXPECT_TRUE(addr.host_name.empty());
}

TEST(Address, AggregateInitializationWithDesignatedInitializers) {
  const bkmail::address<> addr{.display_name = "Alice Example",
                               .mailbox_name = "alice",
                               .host_name = "example.com"};
  EXPECT_EQ("Alice Example", addr.display_name);
  EXPECT_TRUE(addr.adl.empty());
  EXPECT_EQ("alice", addr.mailbox_name);
  EXPECT_EQ("example.com", addr.host_name);
}

TEST(Address, EmailJoinsMailboxAndHost) {
  const bkmail::address<> addr{.mailbox_name = "bob",
                               .host_name = "mail.example.org"};
  EXPECT_EQ("bob@mail.example.org", addr.email());
}

TEST(Address, EmailOmitsAtSignWhenHostIsNil) {
  const bkmail::address<> addr{.mailbox_name = "carol"};
  EXPECT_EQ("carol", addr.email());
}

TEST(Address, EmailOfEmptyAddressIsEmpty) {
  const bkmail::address<> addr;
  EXPECT_EQ("", addr.email());
}

// -- envelope ----------------------------------------------------------------

TEST(Envelope, HasTenWireFieldsInOrder) {
  // Aggregate-initialize all ten fields positionally to pin the wire
  // order: date, subject, from, sender, reply_to, to, cc, bcc,
  // in_reply_to, message_id.
  bkmail::envelope<> env{"Mon, 7 Sep 2026 00:00:00 +0000",
                         "subject",
                         {{.mailbox_name = "from", .host_name = "a.dev"}},
                         {{.mailbox_name = "sender", .host_name = "b.dev"}},
                         {{.mailbox_name = "reply", .host_name = "c.dev"}},
                         {{.mailbox_name = "to", .host_name = "d.dev"}},
                         {{.mailbox_name = "cc", .host_name = "e.dev"}},
                         {{.mailbox_name = "bcc", .host_name = "f.dev"}},
                         "<parent@a.dev>",
                         "<child@a.dev>"};
  EXPECT_EQ("Mon, 7 Sep 2026 00:00:00 +0000", env.date);
  EXPECT_EQ("subject", env.subject);
  ASSERT_EQ(1U, env.from.size());
  EXPECT_EQ("from@a.dev", env.from.front().email());
  ASSERT_EQ(1U, env.sender.size());
  EXPECT_EQ("sender@b.dev", env.sender.front().email());
  ASSERT_EQ(1U, env.reply_to.size());
  EXPECT_EQ("reply@c.dev", env.reply_to.front().email());
  ASSERT_EQ(1U, env.to.size());
  EXPECT_EQ("to@d.dev", env.to.front().email());
  ASSERT_EQ(1U, env.cc.size());
  EXPECT_EQ("cc@e.dev", env.cc.front().email());
  ASSERT_EQ(1U, env.bcc.size());
  EXPECT_EQ("bcc@f.dev", env.bcc.front().email());
  EXPECT_EQ("<parent@a.dev>", env.in_reply_to);
  EXPECT_EQ("<child@a.dev>", env.message_id);
}

TEST(Envelope, DefaultConstructedIsAllEmpty) {
  const bkmail::envelope<> env;
  EXPECT_TRUE(env.date.empty());
  EXPECT_TRUE(env.subject.empty());
  EXPECT_TRUE(env.from.empty());
  EXPECT_TRUE(env.sender.empty());
  EXPECT_TRUE(env.reply_to.empty());
  EXPECT_TRUE(env.to.empty());
  EXPECT_TRUE(env.cc.empty());
  EXPECT_TRUE(env.bcc.empty());
  EXPECT_TRUE(env.in_reply_to.empty());
  EXPECT_TRUE(env.message_id.empty());
}

TEST(Envelope, AddressListsHoldMultipleEntries) {
  bkmail::envelope<>::address_list list;
  list.push_back({.mailbox_name = "a", .host_name = "x.dev"});
  list.push_back({.mailbox_name = "b", .host_name = "y.dev"});
  const bkmail::envelope<> env{.to = std::move(list)};
  ASSERT_EQ(2U, env.to.size());
  EXPECT_EQ("a@x.dev", env.to[0].email());
  EXPECT_EQ("b@y.dev", env.to[1].email());
}

// -- body_structure ----------------------------------------------------------

TEST(BodyStructure, LeafNodeHoldsScalarFields) {
  const bkmail::body_structure<> leaf{
      .media_type = "text",
      .subtype = "plain",
      .parameters = {{"charset", "utf-8"}},
      .id = "<part1@x.dev>",
      .description = "greeting",
      .encoding = "7bit",
      .octets = 42,
  };
  EXPECT_EQ("text", leaf.media_type);
  EXPECT_EQ("plain", leaf.subtype);
  ASSERT_EQ(1U, leaf.parameters.size());
  EXPECT_EQ("charset", leaf.parameters[0].first);
  EXPECT_EQ("utf-8", leaf.parameters[0].second);
  EXPECT_EQ("<part1@x.dev>", leaf.id);
  EXPECT_EQ("greeting", leaf.description);
  EXPECT_EQ("7bit", leaf.encoding);
  EXPECT_EQ(42U, leaf.octets);
  EXPECT_TRUE(leaf.parts.empty());
}

TEST(BodyStructure, MultipartNodeNestsChildren) {
  bkmail::body_structure<> root{.media_type = "multipart", .subtype = "mixed"};
  root.parts.push_back(
      {.media_type = "text", .subtype = "plain", .octets = 10});
  bkmail::body_structure<> nested{.media_type = "multipart",
                                  .subtype = "alternative"};
  nested.parts.push_back(
      {.media_type = "text", .subtype = "plain", .octets = 20});
  nested.parts.push_back(
      {.media_type = "text", .subtype = "html", .octets = 30});
  root.parts.push_back(std::move(nested));

  ASSERT_EQ(2U, root.parts.size());
  EXPECT_EQ("text", root.parts[0].media_type);
  EXPECT_TRUE(root.parts[0].parts.empty());
  ASSERT_EQ(2U, root.parts[1].parts.size());
  EXPECT_EQ("html", root.parts[1].parts[1].subtype);
  EXPECT_EQ(30U, root.parts[1].parts[1].octets);
}

TEST(BodyStructure, OctetsIs64Bit) {
  static_assert(std::is_same_v<std::uint64_t,
                               decltype(bkmail::body_structure<>::octets)>);
  bkmail::body_structure<> node;
  node.octets = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), node.octets);
}

TEST(BodyStructure, DefaultOctetsIsZero) {
  const bkmail::body_structure<> node;
  EXPECT_EQ(0U, node.octets);
}

// -- mail_header + RFC 2047 decoding -----------------------------------------

TEST(MailHeader, FieldsRoundTrip) {
  bkmail::mail_header<> header;
  header.subject = "hello";
  header.date = "Tue, 8 Sep 2026 01:02:03 +0000";
  header.message_id = "<m1@x.dev>";
  header.in_reply_to = "<m0@x.dev>";
  header.from.push_back({.mailbox_name = "alice", .host_name = "x.dev"});
  header.to.push_back({.mailbox_name = "bob", .host_name = "y.dev"});

  EXPECT_EQ("hello", header.subject);
  EXPECT_EQ("Tue, 8 Sep 2026 01:02:03 +0000", header.date);
  EXPECT_EQ("<m1@x.dev>", header.message_id);
  EXPECT_EQ("<m0@x.dev>", header.in_reply_to);
  ASSERT_EQ(1U, header.from.size());
  EXPECT_EQ("alice@x.dev", header.from[0].email());
  ASSERT_EQ(1U, header.to.size());
  EXPECT_EQ("bob@y.dev", header.to[0].email());
  EXPECT_TRUE(header.cc.empty());
  EXPECT_TRUE(header.bcc.empty());
}

TEST(EncodedWords, PlainTextPassesThroughUnchanged) {
  EXPECT_EQ("plain subject",
            bkmail::detail::decode_encoded_words("plain subject"));
}

TEST(EncodedWords, DecodesBase64Word) {
  // "hello" in Base64 is "aGVsbG8=".
  EXPECT_EQ("hello",
            bkmail::detail::decode_encoded_words("=?UTF-8?B?aGVsbG8=?="));
}

TEST(EncodedWords, DecodesQWord) {
  // Q-encoding: "_" is a space, "=XX" hex escapes.
  EXPECT_EQ("hello world",
            bkmail::detail::decode_encoded_words("=?UTF-8?Q?hello_world?="));
  EXPECT_EQ("a=b", bkmail::detail::decode_encoded_words("=?UTF-8?Q?a=3Db?="));
}

TEST(EncodedWords, DecodesMultibyteUtf8Sequences) {
  // "中" is E4 B8 AD in UTF-8.
  EXPECT_EQ("\xE4\xB8\xAD",
            bkmail::detail::decode_encoded_words("=?UTF-8?B?5Lit?="));
  EXPECT_EQ("\xE4\xB8\xAD",
            bkmail::detail::decode_encoded_words("=?UTF-8?Q?=E4=B8=AD?="));
}

TEST(EncodedWords, DropsLinearWhitespaceBetweenAdjacentWords) {
  // RFC 2047 §6.2: LWSP between two adjacent encoded-words is ignored.
  EXPECT_EQ("helloworld", bkmail::detail::decode_encoded_words(
                              "=?UTF-8?Q?hello?=\r\n\t =?UTF-8?Q?world?="));
  EXPECT_EQ("ab", bkmail::detail::decode_encoded_words(
                      "=?UTF-8?B?YQ==?=\t=?UTF-8?B?Yg==?="));
}

TEST(EncodedWords, KeepsWhitespaceAroundPlainText) {
  // LWSP is dropped only between two *adjacent* encoded-words; whitespace
  // bordering plain text must survive.
  EXPECT_EQ("say hello !",
            bkmail::detail::decode_encoded_words("say =?UTF-8?Q?hello?= !"));
  EXPECT_EQ("hi there",
            bkmail::detail::decode_encoded_words("=?UTF-8?Q?hi?= there"));
}

TEST(EncodedWords, AcceptsLowercaseEncodingLetters) {
  EXPECT_EQ("hello",
            bkmail::detail::decode_encoded_words("=?UTF-8?b?aGVsbG8=?="));
  EXPECT_EQ("hello", bkmail::detail::decode_encoded_words("=?UTF-8?q?hello?="));
}

TEST(EncodedWords, MalformedWordsPassThroughVerbatim) {
  const std::string_view cases[] = {
      "=?",                // truncated
      "=??B?YQ==?=",       // empty charset
      "=?UTF-8?X?YQ==?=",  // unknown encoding
      "=?UTF-8?B?YQ==",    // missing terminator
      "=?UTF-8?B?YQ= ?=",  // invalid Base64 alphabet member
      "=?UTF-8?Q?=ZZ?=",   // bad hex escape
      "=?UTF-8?Q?=3D",     // truncated escape at word end
  };
  for (const auto text : cases) {
    EXPECT_EQ(text, bkmail::detail::decode_encoded_words(text)) << text;
  }
}

TEST(EncodedWords, InvalidBase64AlphabetWordPassesThrough) {
  // '!' is outside the Base64 alphabet: the word is not decodable.
  EXPECT_EQ("=?UTF-8?B?YQ!=?=",
            bkmail::detail::decode_encoded_words("=?UTF-8?B?YQ!=?="));
}

TEST(EncodedWords, CharsetWithSpaceIsNotAWord) {
  // Encoded-word charsets may not contain LWSP.
  const std::string_view text = "=?UTF 8?Q?hi?=";
  EXPECT_EQ(text, bkmail::detail::decode_encoded_words(text));
}

TEST(EncodedWords, DecodeHonoursAllocator) {
  char buffer[1024];
  std::pmr::monotonic_buffer_resource resource{buffer, sizeof(buffer)};
  const auto decoded = bkmail::detail::decode_encoded_words<
      std::pmr::polymorphic_allocator<std::byte>>("=?UTF-8?Q?hi?=",
                                                  {&resource});
  EXPECT_EQ("hi", decoded);
}

// -- mail_body / mail / account_info -----------------------------------------

TEST(MailBody, HoldsContentTypeAndOctets) {
  bkmail::mail_body<> body;
  body.content_type = "text/plain; charset=utf-8";
  body.data = {std::byte{'h'}, std::byte{'i'}};
  EXPECT_EQ("text/plain; charset=utf-8", body.content_type);
  ASSERT_EQ(2U, body.data.size());
  EXPECT_EQ(std::byte{'h'}, body.data[0]);
}

TEST(Mail, ComposesHeaderBodyAndOptionalEnvelope) {
  bkmail::mail<> msg;
  msg.header.subject = "s";
  msg.body.content_type = "text/plain";
  EXPECT_FALSE(msg.envelope.has_value());

  msg.envelope.emplace();
  msg.envelope->subject = "wire-subject";
  ASSERT_TRUE(msg.envelope.has_value());
  EXPECT_EQ("wire-subject", msg.envelope->subject);
  EXPECT_EQ("s", msg.header.subject);
  EXPECT_EQ("text/plain", msg.body.content_type);
}

TEST(AccountInfo, AuthzidIsOptional) {
  const bkmail::account_info<> plain{.user_name = "u", .password = "p"};
  EXPECT_EQ("u", plain.user_name);
  EXPECT_EQ("p", plain.password);
  EXPECT_FALSE(plain.authzid.has_value());

  const bkmail::account_info<> admin{
      .user_name = "u", .password = "p", .authzid = "root"};
  ASSERT_TRUE(admin.authzid.has_value());
  EXPECT_EQ("root", *admin.authzid);
}

}  // namespace
