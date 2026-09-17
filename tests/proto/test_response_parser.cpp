/**
 * @file tests/proto/test_response_parser.cpp
 * @brief Unit tests for the IMAP server response classifier/shallow parser.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/detail/response_parser.h>
#include <bkmail/imap/response.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace {

namespace im = bkmail::imap;
namespace det = im::detail;

using parser = det::response_parser<>;
using response_type = im::server_response<>;

std::optional<response_type> parse(std::string_view wire) {
  const parser p;
  return p.parse(wire);
}

const im::tagged_response<>& as_tagged(const response_type& r) {
  return std::get<im::tagged_response<>>(r);
}

const im::untagged_response<>& as_untagged(const response_type& r) {
  return std::get<im::untagged_response<>>(r);
}

const im::continuation_request<>& as_continuation(const response_type& r) {
  return std::get<im::continuation_request<>>(r);
}

// ----- tagged replies -------------------------------------------------

TEST(ResponseParser, TaggedOkNoBad) {
  const auto ok = parse("a0001 OK Select completed");
  ASSERT_TRUE(ok.has_value());
  const auto& t = as_tagged(*ok);
  EXPECT_EQ("a0001", t.tag);
  EXPECT_EQ(im::response_status::ok, t.status);
  EXPECT_FALSE(t.code.has_value());
  EXPECT_EQ("Select completed", t.text);

  const auto no = parse("a2 NO no such mailbox");
  ASSERT_TRUE(no.has_value());
  EXPECT_EQ(im::response_status::no, as_tagged(*no).status);

  const auto bad = parse("a3 BAD broken command");
  ASSERT_TRUE(bad.has_value());
  EXPECT_EQ(im::response_status::bad, as_tagged(*bad).status);
}

TEST(ResponseParser, TaggedWithRespCode) {
  const auto parsed = parse("a0001 OK [UIDVALIDITY 3857529] Select completed");
  ASSERT_TRUE(parsed.has_value());
  const auto& t = as_tagged(*parsed);
  ASSERT_TRUE(t.code.has_value());
  const auto* code = std::get_if<im::uidvalidity_code>(&*t.code);
  ASSERT_NE(nullptr, code);
  EXPECT_EQ(3857529U, code->value);
  EXPECT_EQ("Select completed", t.text);
}

TEST(ResponseParser, TaggedStatusKeywordIsCaseInsensitive) {
  const auto parsed = parse("a1 ok lower case");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(im::response_status::ok, as_tagged(*parsed).status);
}

TEST(ResponseParser, TaggedMalformedReturnsNullopt) {
  EXPECT_FALSE(parse("").has_value());
  EXPECT_FALSE(parse("a1").has_value());             // No status keyword.
  EXPECT_FALSE(parse("a1 XYZZY text").has_value());  // Unknown status.
  // PREAUTH/BYE are not legal tagged statuses (RFC 3501 §7.1).
  EXPECT_FALSE(parse("a1 PREAUTH x").has_value());
  EXPECT_FALSE(parse("a1 BYE x").has_value());
  // Unterminated resp-text-code bracket.
  EXPECT_FALSE(parse("a1 OK [UIDNEXT 12 text").has_value());
}

// ----- resp-text-code forms --------------------------------------------

TEST(ResponseParser, RespCodeSimpleForms) {
  const auto alert = parse("a1 OK [ALERT] warn");
  ASSERT_TRUE(alert.has_value());
  EXPECT_NE(nullptr, std::get_if<im::alert_code>(&*as_tagged(*alert).code));
  EXPECT_EQ("warn", as_tagged(*alert).text);

  const auto parse_code = parse("a1 BAD [PARSE] cannot");
  ASSERT_TRUE(parse_code.has_value());
  EXPECT_NE(nullptr,
            std::get_if<im::parse_code>(&*as_tagged(*parse_code).code));

  const auto ro = parse("a1 OK [READ-ONLY] selected");
  ASSERT_TRUE(ro.has_value());
  EXPECT_NE(nullptr, std::get_if<im::read_only_code>(&*as_tagged(*ro).code));

  const auto rw = parse("a1 OK [READ-WRITE] selected");
  ASSERT_TRUE(rw.has_value());
  EXPECT_NE(nullptr, std::get_if<im::read_write_code>(&*as_tagged(*rw).code));

  const auto tc = parse("a1 NO [TRYCREATE] create first");
  ASSERT_TRUE(tc.has_value());
  EXPECT_NE(nullptr, std::get_if<im::trycreate_code>(&*as_tagged(*tc).code));
}

TEST(ResponseParser, RespCodeNumericForms) {
  const auto uidnext = parse("a1 OK [UIDNEXT 4392] x");
  ASSERT_TRUE(uidnext.has_value());
  const auto* un = std::get_if<im::uidnext_code>(&*as_tagged(*uidnext).code);
  ASSERT_NE(nullptr, un);
  EXPECT_EQ(4392U, un->value);

  const auto unseen = parse("a1 OK [UNSEEN 12] x");
  ASSERT_TRUE(unseen.has_value());
  const auto* us = std::get_if<im::unseen_code>(&*as_tagged(*unseen).code);
  ASSERT_NE(nullptr, us);
  EXPECT_EQ(12U, us->value);

  // A known numeric code with malformed arguments degrades to unknown.
  const auto broken = parse("a1 OK [UIDNEXT abc] x");
  ASSERT_TRUE(broken.has_value());
  const auto* unk = std::get_if<im::unknown_code<>>(&*as_tagged(*broken).code);
  ASSERT_NE(nullptr, unk);
  EXPECT_EQ("UIDNEXT", unk->atom);
  EXPECT_EQ("abc", unk->arguments);
}

TEST(ResponseParser, RespCodeCapabilityForm) {
  const auto parsed = parse("a1 OK [CAPABILITY IMAP4rev1 IDLE LITERAL+] x");
  ASSERT_TRUE(parsed.has_value());
  const auto* cap =
      std::get_if<im::capability_code<>>(&*as_tagged(*parsed).code);
  ASSERT_NE(nullptr, cap);
  ASSERT_EQ(3U, cap->capabilities.size());
  EXPECT_EQ("IMAP4rev1", cap->capabilities.at(0));
  EXPECT_EQ("IDLE", cap->capabilities.at(1));
  EXPECT_EQ("LITERAL+", cap->capabilities.at(2));
}

TEST(ResponseParser, RespCodeBadcharsetForm) {
  const auto parsed = parse("a1 NO [BADCHARSET (UTF-8 US-ASCII)] nope");
  ASSERT_TRUE(parsed.has_value());
  const auto* bc =
      std::get_if<im::badcharset_code<>>(&*as_tagged(*parsed).code);
  ASSERT_NE(nullptr, bc);
  ASSERT_EQ(2U, bc->charsets.size());
  EXPECT_EQ("UTF-8", bc->charsets.at(0));
  EXPECT_EQ("US-ASCII", bc->charsets.at(1));

  // BADCHARSET without a charset list is legal.
  const auto bare = parse("a1 NO [BADCHARSET] nope");
  ASSERT_TRUE(bare.has_value());
  const auto* bb = std::get_if<im::badcharset_code<>>(&*as_tagged(*bare).code);
  ASSERT_NE(nullptr, bb);
  EXPECT_TRUE(bb->charsets.empty());
}

TEST(ResponseParser, RespCodePermanentflagsForm) {
  const auto parsed = parse("a1 OK [PERMANENTFLAGS (\\Seen \\Deleted \\*)] x");
  ASSERT_TRUE(parsed.has_value());
  const auto* pf =
      std::get_if<im::permanentflags_code>(&*as_tagged(*parsed).code);
  ASSERT_NE(nullptr, pf);
  EXPECT_TRUE(pf->flags.test(im::message_flag::seen));
  EXPECT_TRUE(pf->flags.test(im::message_flag::deleted));
  EXPECT_FALSE(pf->flags.test(im::message_flag::answered));
}

TEST(ResponseParser, UnknownRespCodeDegradesNeverFails) {
  const auto parsed = parse("a1 OK [XYZZY plug away] done");
  ASSERT_TRUE(parsed.has_value());
  const auto* unk = std::get_if<im::unknown_code<>>(&*as_tagged(*parsed).code);
  ASSERT_NE(nullptr, unk);
  EXPECT_EQ("XYZZY", unk->atom);
  EXPECT_EQ("plug away", unk->arguments);
  EXPECT_EQ("done", as_tagged(*parsed).text);
}

// ----- untagged responses ----------------------------------------------

TEST(ResponseParser, UntaggedNumberedKinds) {
  const auto exists = parse("* 23 EXISTS");
  ASSERT_TRUE(exists.has_value());
  const auto& e = as_untagged(*exists);
  EXPECT_EQ(im::untagged_kind::exists, e.kind);
  EXPECT_EQ(23U, e.number);

  const auto recent = parse("* 3 RECENT");
  ASSERT_TRUE(recent.has_value());
  EXPECT_EQ(im::untagged_kind::recent, as_untagged(*recent).kind);
  EXPECT_EQ(3U, as_untagged(*recent).number);

  const auto expunge = parse("* 12 EXPUNGE");
  ASSERT_TRUE(expunge.has_value());
  EXPECT_EQ(im::untagged_kind::expunge, as_untagged(*expunge).kind);
  EXPECT_EQ(12U, as_untagged(*expunge).number);

  const auto fetch = parse("* 7 FETCH (FLAGS (\\Seen))");
  ASSERT_TRUE(fetch.has_value());
  const auto& ft = as_untagged(*fetch);
  EXPECT_EQ(im::untagged_kind::fetch, ft.kind);
  EXPECT_EQ(7U, ft.number);
  EXPECT_EQ("(FLAGS (\\Seen))", ft.payload);
}

TEST(ResponseParser, UntaggedKeywordKinds) {
  const auto capability = parse("* CAPABILITY IMAP4rev1 IDLE");
  ASSERT_TRUE(capability.has_value());
  EXPECT_EQ(im::untagged_kind::capability, as_untagged(*capability).kind);
  EXPECT_EQ("IMAP4rev1 IDLE", as_untagged(*capability).payload);

  const auto flags = parse("* FLAGS (\\Seen \\Deleted)");
  ASSERT_TRUE(flags.has_value());
  EXPECT_EQ(im::untagged_kind::flags, as_untagged(*flags).kind);
  EXPECT_EQ("(\\Seen \\Deleted)", as_untagged(*flags).payload);

  const auto list = parse("* LIST (\\NoSelect) \"\\\" INBOX");
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(im::untagged_kind::list, as_untagged(*list).kind);
  EXPECT_EQ("(\\NoSelect) \"\\\" INBOX", as_untagged(*list).payload);

  const auto lsub = parse("* LSUB () . #news");
  ASSERT_TRUE(lsub.has_value());
  EXPECT_EQ(im::untagged_kind::lsub, as_untagged(*lsub).kind);

  const auto status = parse("* STATUS INBOX (MESSAGES 3)");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(im::untagged_kind::status, as_untagged(*status).kind);
  EXPECT_EQ("INBOX (MESSAGES 3)", as_untagged(*status).payload);

  const auto search = parse("* SEARCH 1 2 3");
  ASSERT_TRUE(search.has_value());
  EXPECT_EQ(im::untagged_kind::search, as_untagged(*search).kind);
  EXPECT_EQ("1 2 3", as_untagged(*search).payload);
}

TEST(ResponseParser, UntaggedStatusKinds) {
  const auto ok = parse("* OK [READ-ONLY] mailbox selected");
  ASSERT_TRUE(ok.has_value());
  const auto& o = as_untagged(*ok);
  EXPECT_EQ(im::untagged_kind::ok, o.kind);
  EXPECT_EQ(im::response_status::ok, o.status);
  ASSERT_TRUE(o.code.has_value());
  EXPECT_NE(nullptr, std::get_if<im::read_only_code>(&*o.code));
  EXPECT_EQ("mailbox selected", o.text);

  const auto no = parse("* NO not now");
  ASSERT_TRUE(no.has_value());
  EXPECT_EQ(im::untagged_kind::no, as_untagged(*no).kind);

  const auto bad = parse("* BAD bad news");
  ASSERT_TRUE(bad.has_value());
  EXPECT_EQ(im::untagged_kind::bad, as_untagged(*bad).kind);

  const auto bye = parse("* BYE [ALERT] going away");
  ASSERT_TRUE(bye.has_value());
  const auto& b = as_untagged(*bye);
  EXPECT_EQ(im::untagged_kind::bye, b.kind);
  EXPECT_EQ(im::response_status::bye, b.status);
  ASSERT_TRUE(b.code.has_value());
  EXPECT_NE(nullptr, std::get_if<im::alert_code>(&*b.code));
  EXPECT_EQ("going away", b.text);

  const auto preauth = parse("* PREAUTH ready");
  ASSERT_TRUE(preauth.has_value());
  const auto& p = as_untagged(*preauth);
  EXPECT_EQ(im::untagged_kind::preauth, p.kind);
  EXPECT_EQ(im::response_status::preauth, p.status);
}

TEST(ResponseParser, UntaggedUnknownKeywordsDegrade) {
  // Unknown extension keyword: raw remainder kept, never a failure.
  const auto ext = parse("* XYZZY plug");
  ASSERT_TRUE(ext.has_value());
  const auto& x = as_untagged(*ext);
  EXPECT_EQ(im::untagged_kind::unknown, x.kind);
  EXPECT_EQ("XYZZY plug", x.payload);

  // Unknown NUMBERED extension data (e.g. VANISHED): number + remainder.
  const auto vanished = parse("* 5 VANISHED");
  ASSERT_TRUE(vanished.has_value());
  const auto& v = as_untagged(*vanished);
  EXPECT_EQ(im::untagged_kind::unknown, v.kind);
  EXPECT_EQ("5 VANISHED", v.payload);
}

TEST(ResponseParser, UntaggedMalformedReturnsNullopt) {
  EXPECT_FALSE(parse("*").has_value());            // Missing SP + content.
  EXPECT_FALSE(parse("* ").has_value());           // Nothing after the SP.
  EXPECT_FALSE(parse("* OK [ALERT").has_value());  // Unterminated bracket.
}

// ----- continuation requests --------------------------------------------

TEST(ResponseParser, ContinuationRequests) {
  const auto idling = parse("+ idling");
  ASSERT_TRUE(idling.has_value());
  EXPECT_EQ("idling", as_continuation(*idling).text);

  // A bare "+" yields empty text.
  const auto bare = parse("+");
  ASSERT_TRUE(bare.has_value());
  EXPECT_EQ("", as_continuation(*bare).text);

  // Only one leading SP is stripped.
  const auto glued = parse("+go ahead");
  ASSERT_TRUE(glued.has_value());
  EXPECT_EQ("go ahead", as_continuation(*glued).text);
}

}  // namespace
