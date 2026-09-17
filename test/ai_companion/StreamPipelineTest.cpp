#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/AiCompanion/AiChatParser.h"
#include "lib/AiCompanion/PromptBuilder.h"
#include "lib/AiCompanion/SseDecoder.h"

// End-to-end through the three pieces that make up the transport layer: raw
// bytes off a socket -> SSE events -> assistant text. Each piece is covered on
// its own elsewhere; what is asserted here is that they compose.
namespace {

struct Pipeline {
  explicit Pipeline(char* buf, const size_t cap) : parser(buf, cap), decoder(this, &Pipeline::event, &Pipeline::done) {}

  static void event(void* ctx, const char* data, const size_t len) {
    static_cast<Pipeline*>(ctx)->parser.feedEvent(data, len);
  }
  static void done(void* ctx) { static_cast<Pipeline*>(ctx)->finished = true; }

  AiChatParser parser;
  SseDecoder decoder;
  bool finished = false;
};

std::string chunk(const std::string& content) {
  return "data: {\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{"
         "\"content\":\"" +
         content + "\"},\"finish_reason\":null}]}\n\n";
}

// A stream shaped like a real one: a role-only opener, several content deltas, a
// finish_reason chunk, then the sentinel.
std::string realisticStream() {
  return "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n" +
         chunk("这一章") + chunk("读得我") + chunk("有点难受。") +
         "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
         "data: [DONE]\n\n";
}

TEST(StreamPipeline, AssemblesReplyFromAWholeStream) {
  std::array<char, 1024> buf{};
  Pipeline pipe(buf.data(), buf.size());
  const std::string stream = realisticStream();
  pipe.decoder.feed(stream.data(), stream.size());

  EXPECT_TRUE(pipe.finished);
  EXPECT_STREQ(pipe.parser.content(), "这一章读得我有点难受。");
  EXPECT_STREQ(pipe.parser.getFinishReason(), "stop");
  EXPECT_FALSE(pipe.parser.hasError());
  EXPECT_FALSE(pipe.parser.truncated());
  EXPECT_EQ(pipe.parser.malformedEvents(), 0u);
}

// TCP delivers whatever it delivers. The assembled reply must not depend on how
// the stream happens to be sliced -- including slices that split a CJK character
// across two reads.
TEST(StreamPipeline, ResultIsIndependentOfChunkSize) {
  const std::string stream = realisticStream();
  for (size_t size = 1; size <= 64; ++size) {
    std::array<char, 1024> buf{};
    Pipeline pipe(buf.data(), buf.size());
    for (size_t i = 0; i < stream.size(); i += size) {
      const size_t n = std::min(size, stream.size() - i);
      pipe.decoder.feed(stream.data() + i, n);
    }
    EXPECT_STREQ(pipe.parser.content(), "这一章读得我有点难受。") << "chunk size " << size;
    EXPECT_TRUE(pipe.finished) << "chunk size " << size;
  }
}

TEST(StreamPipeline, ProviderErrorMidStreamIsReported) {
  std::array<char, 1024> buf{};
  Pipeline pipe(buf.data(), buf.size());
  const std::string stream =
      "data: {\"error\":{\"message\":\"You exceeded your current quota\",\"type\":\"insufficient_quota\"}}\n\n";
  pipe.decoder.feed(stream.data(), stream.size());

  EXPECT_TRUE(pipe.parser.hasError());
  EXPECT_STREQ(pipe.parser.errorMessage(), "You exceeded your current quota");
  EXPECT_EQ(pipe.parser.contentLength(), 0u);
}

// A connection dropped mid-reply must leave the text received so far intact and
// usable, not discard it.
TEST(StreamPipeline, TruncatedStreamKeepsWhatArrived) {
  std::array<char, 1024> buf{};
  Pipeline pipe(buf.data(), buf.size());
  const std::string stream = chunk("开头还在") + "data: {\"choices\":[{\"delta\":{\"content\":\"没说完";
  pipe.decoder.feed(stream.data(), stream.size());

  EXPECT_FALSE(pipe.finished);
  EXPECT_STREQ(pipe.parser.content(), "开头还在");
}

// The request the builder produces must be parseable by the same machinery that
// reads responses -- the cheapest available check that we speak our own dialect.
TEST(StreamPipeline, BuiltRequestIsWellFormedForTheParser) {
  std::array<char, 4096> requestBuf{};
  PromptBuilder builder(requestBuf.data(), requestBuf.size());
  PromptBuilder::Position position;
  position.bookTitle = "我的书 \"带引号\"";
  position.chapterTitle = "第一章\n换行";
  position.percent = 7;
  builder.setPersona("你是我的读书搭子。\n说话要像朋友。");
  builder.setPosition(position);
  builder.setExcerpt("他说：“这不可能。”\\反斜杠也在这里。");
  builder.setQuestion("你怎么看？");

  ASSERT_TRUE(builder.build());

  JsonCallbacks cb{};
  StreamingJsonParser verifier(cb);
  verifier.feed(builder.body(), builder.length());
  EXPECT_FALSE(verifier.hasError());
}

}  // namespace
