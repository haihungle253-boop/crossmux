#include <gtest/gtest.h>

#include <string>

#include "lib/AiCompanion/CompanionConfig.h"

namespace {

bool load(CompanionConfig& cfg, const std::string& text) { return cfg.load(text.data(), text.size()); }

TEST(CompanionConfig, ParsesATypicalFile) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg,
                   "# 读书搭子连接配置\n"
                   "endpoint = https://ai.example.com/v1/chat/completions\n"
                   "model = gpt-4o-mini\n"
                   "key = sk-abc123\n"
                   "max_tokens = 400\n"));
  EXPECT_STREQ(cfg.endpoint(), "https://ai.example.com/v1/chat/completions");
  EXPECT_STREQ(cfg.model(), "gpt-4o-mini");
  EXPECT_STREQ(cfg.key(), "sk-abc123");
  EXPECT_EQ(cfg.maxTokens(), 400);
  EXPECT_TRUE(cfg.configured());
  EXPECT_FALSE(cfg.insecureEndpoint());
}

TEST(CompanionConfig, ToleratesBomCrlfSpacingAndComments) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, std::string("\xEF\xBB\xBF") + "\r\n"
                                                      "#   comment\r\n"
                                                      "   endpoint   =   https://example.com/v1   \r\n"
                                                      "\r\n"
                                                      "model=m\r\n"));
  EXPECT_STREQ(cfg.endpoint(), "https://example.com/v1");
  EXPECT_STREQ(cfg.model(), "m");
}

// A self-hosted proxy on a home network is a legitimate http endpoint; the
// parser reports it rather than refusing, so the UI can warn once.
TEST(CompanionConfig, AcceptsHttpButFlagsIt) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, "endpoint = http://192.168.1.50:8080/v1/chat/completions\n"));
  EXPECT_TRUE(cfg.configured());
  EXPECT_TRUE(cfg.insecureEndpoint());
}

TEST(CompanionConfig, RejectsAMissingOrNonsenseEndpoint) {
  CompanionConfig cfg;
  EXPECT_FALSE(load(cfg, "model = m\nkey = k\n"));
  EXPECT_FALSE(cfg.configured());

  EXPECT_FALSE(load(cfg, "endpoint = ftp://nope/\n"));
  EXPECT_FALSE(load(cfg, ""));
}

// A typo in a key name would otherwise be completely silent.
TEST(CompanionConfig, CountsUnrecognisedKeys) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nmodle = typo\ntemperature = 0.7\n"));
  EXPECT_EQ(cfg.unknownKeys(), 2u);
}

TEST(CompanionConfig, IgnoresLinesWithoutASeparator) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nthis line is prose\n"));
  EXPECT_STREQ(cfg.endpoint(), "https://a/b");
}

TEST(CompanionConfig, CapsOverlongValues) {
  CompanionConfig cfg;
  std::string longKey(CompanionConfig::KEY_BYTES + 50, 'k');
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nkey = " + longKey + "\n"));
  EXPECT_LE(strlen(cfg.key()), CompanionConfig::KEY_BYTES);
}

TEST(CompanionConfig, RejectsNonsenseMaxTokens) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nmax_tokens = banana\n"));
  EXPECT_EQ(cfg.maxTokens(), 0);
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nmax_tokens = -5\n"));
  EXPECT_EQ(cfg.maxTokens(), 0);
}

TEST(CompanionConfig, ClearResetsEverything) {
  CompanionConfig cfg;
  ASSERT_TRUE(load(cfg, "endpoint = https://a/b\nkey = k\n"));
  cfg.clear();
  EXPECT_FALSE(cfg.configured());
  EXPECT_STREQ(cfg.key(), "");
}

}  // namespace
