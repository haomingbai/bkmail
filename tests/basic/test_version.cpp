#include <bkmail/version.h>
#include <gtest/gtest.h>

#include <string>

namespace {

TEST(Version, ReportsNonEmptyVersion) {
  const char* v = bkmail::version();
  ASSERT_NE(nullptr, v);
  EXPECT_FALSE(std::string(v).empty());
  EXPECT_NE(std::string("unknown"), std::string(v));
}

}  // namespace
