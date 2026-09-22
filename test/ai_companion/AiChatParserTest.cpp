#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/AiCompanion/AiChatParser.h"

namespace {

std::string deltaEvent(const std::string& content) {
  return "{\"id\":\"x\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" +
         content + "\"},\"finish_reason\":null}]}";
}

void feed(AiChatParser& parser, const std::string& json) { parser.feedEvent(json.data(), json.size()); }

TEST(AiChatParser, ExtractsContentFromDelta) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("hello"));
  EXPECT_STREQ(parser.content(), "hello");
  EXPECT_FALSE(parser.hasError());
}

TEST(AiChatParser, AccumulatesAcrossEvents) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("Hello"));
  feed(parser, deltaEvent(", "));
  feed(parser, deltaEvent("world"));
  EXPECT_STREQ(parser.content(), "Hello, world");
  EXPECT_EQ(parser.contentLength(), 12u);
}

TEST(AiChatParser, HandlesCjkContent) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("这一章"));
  feed(parser, deltaEvent("很精彩"));
  EXPECT_STREQ(parser.content(), "这一章很精彩");
}

// The first chunk of an OpenAI stream announces the role and carries no
// content; it must not contribute anything.
TEST(AiChatParser, RoleOnlyDeltaContributesNothing) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, "{\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}");
  EXPECT_EQ(parser.contentLength(), 0u);
  EXPECT_FALSE(parser.hasError());
}

// A "content" key somewhere other than choices[].delta must not be mistaken for
// reply text.
TEST(AiChatParser, IgnoresContentOutsideDelta) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, "{\"content\":\"top level\",\"meta\":{\"content\":\"nested\"},\"choices\":[]}");
  EXPECT_EQ(parser.contentLength(), 0u);
}

TEST(AiChatParser, CapturesFinishReason) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}");
  EXPECT_TRUE(parser.hasFinishReason());
  EXPECT_STREQ(parser.getFinishReason(), "length");
}

// Providers report auth and quota failures as a 200 response whose stream
// carries an error object. Without this the user would see an empty reply and
// no reason for it.
TEST(AiChatParser, CapturesProviderError) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, "{\"error\":{\"message\":\"Incorrect API key provided\",\"type\":\"invalid_request_error\"}}");
  EXPECT_TRUE(parser.hasError());
  EXPECT_STREQ(parser.errorMessage(), "Incorrect API key provided");
}

TEST(AiChatParser, MalformedJsonIsCountedNotFatal) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, "{\"choices\":[{\"delta\":{\"content\":\"good\"}}]}");
  feed(parser, "{\"choices\":[{\"delta\":{\"content\":");  // cut short
  feed(parser, "{\"choices\":[{\"delta\":{\"content\":\" more\"}}]}");

  EXPECT_STREQ(parser.content(), "good more");
  EXPECT_GE(parser.malformedEvents(), 1u);
}

// Content longer than the JSON parser's 512-byte token buffer is delivered
// through onStringChunk instead of onString.
TEST(AiChatParser, HandlesContentLongerThanJsonTokenBuffer) {
  const std::string longContent(1500, 'a');
  std::array<char, 4096> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent(longContent));
  EXPECT_EQ(parser.contentLength(), longContent.size());
  EXPECT_EQ(std::string(parser.content()), longContent);
  EXPECT_FALSE(parser.truncated());
}

TEST(AiChatParser, TruncationFlagsAndStopsAppending) {
  std::array<char, 8> buf{};  // 7 usable bytes
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("abcdefghij"));
  EXPECT_TRUE(parser.truncated());
  EXPECT_STREQ(parser.content(), "abcdefg");

  feed(parser, deltaEvent("more"));
  EXPECT_STREQ(parser.content(), "abcdefg");  // unchanged after truncation
}

// A cap landing mid-character must not leave a broken UTF-8 sequence: half a
// CJK glyph renders as a replacement character and corrupts anything saved.
TEST(AiChatParser, TruncationCutsOnUtf8Boundary) {
  std::array<char, 8> buf{};  // 7 usable bytes; each CJK char is 3
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("你好世界"));

  EXPECT_TRUE(parser.truncated());
  EXPECT_STREQ(parser.content(), "你好");
  EXPECT_EQ(parser.contentLength(), 6u);
}

TEST(AiChatParser, ResetClearsContentAndFlags) {
  std::array<char, 256> buf{};
  AiChatParser parser(buf.data(), buf.size());
  feed(parser, deltaEvent("first"));
  parser.reset();
  EXPECT_EQ(parser.contentLength(), 0u);
  EXPECT_FALSE(parser.hasError());

  feed(parser, deltaEvent("second"));
  EXPECT_STREQ(parser.content(), "second");
}

}  // namespace
