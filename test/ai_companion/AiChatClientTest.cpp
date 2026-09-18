#include <gtest/gtest.h>

#include <array>
#include <string>

#include "lib/AiCompanion/AiChatClient.h"

namespace {

std::string chunk(const std::string& content) {
  return "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + content + "\"},\"finish_reason\":null}]}\n\n";
}

// Drives the client the way a transport does: repeated onData() until it says
// stop. Returns how many bytes were consumed before it asked to stop.
size_t drive(AiChatClient& client, const std::string& stream, const size_t chunkSize) {
  size_t consumed = 0;
  for (size_t i = 0; i < stream.size(); i += chunkSize) {
    const size_t n = std::min(chunkSize, stream.size() - i);
    const bool keepGoing = client.onData(reinterpret_cast<const uint8_t*>(stream.data() + i), n);
    consumed += n;
    if (!keepGoing) return consumed;
  }
  client.end();
  return consumed;
}

TEST(AiChatClient, AssemblesAReply) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();
  drive(client, chunk("你好") + chunk("，世界"), 8);
  client.end();

  EXPECT_STREQ(client.reply(), "你好，世界");
  EXPECT_FALSE(client.hasError());
}

// Asking the transport to stop at [DONE] rather than waiting for the server to
// close saves holding the radio open at the end of every exchange.
TEST(AiChatClient, StopsAsSoonAsTheStreamCompletes) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();

  const std::string stream = chunk("done") + "data: [DONE]\n\n" + std::string(4096, 'z');
  const size_t consumed = drive(client, stream, 16);

  EXPECT_TRUE(client.complete());
  EXPECT_STREQ(client.reply(), "done");
  EXPECT_LT(consumed, stream.size()) << "should not have read the trailing filler";
}

// Back during generation: the next chunk aborts the transfer, and whatever
// arrived before that stays readable rather than being thrown away.
TEST(AiChatClient, CancelStopsTheTransferAndKeepsPartialText) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();

  const std::string first = chunk("已经收到的部分");
  EXPECT_TRUE(client.onData(reinterpret_cast<const uint8_t*>(first.data()), first.size()));

  client.cancel();

  const std::string second = chunk("不该出现");
  EXPECT_FALSE(client.onData(reinterpret_cast<const uint8_t*>(second.data()), second.size()));

  EXPECT_TRUE(client.wasCancelled());
  EXPECT_FALSE(client.complete());
  EXPECT_STREQ(client.reply(), "已经收到的部分");
}

TEST(AiChatClient, SurfacesProviderErrors) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();
  const std::string stream = "data: {\"error\":{\"message\":\"Invalid API key\"}}\n\n";
  drive(client, stream, 64);
  client.end();

  EXPECT_TRUE(client.hasError());
  EXPECT_STREQ(client.errorMessage(), "Invalid API key");
}

TEST(AiChatClient, BeginResetsBetweenExchanges) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());

  client.begin();
  drive(client, chunk("第一次") + "data: [DONE]\n\n", 64);
  EXPECT_STREQ(client.reply(), "第一次");
  EXPECT_TRUE(client.complete());

  client.begin();
  EXPECT_FALSE(client.complete());
  EXPECT_EQ(client.replyLength(), 0u);
  drive(client, chunk("第二次") + "data: [DONE]\n\n", 64);
  EXPECT_STREQ(client.reply(), "第二次");
}

// The three signals the activity consults before deciding a reply is whole.
// They were all built; until a reply actually came back cut short on the
// device, nothing asked them anything.

TEST(AiChatClient, ReportsTruncationWhenTheReplyOutgrowsItsBuffer) {
  std::array<char, 32> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();
  drive(client, chunk(std::string(200, 'x')) + "data: [DONE]\n\n", 64);
  client.end();

  EXPECT_TRUE(client.replyTruncated());
  EXPECT_LT(client.replyLength(), 200u);
}

TEST(AiChatClient, ReportsNoTruncationWhenTheReplyFits) {
  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();
  drive(client, chunk("刚好装得下") + "data: [DONE]\n\n", 64);
  client.end();

  EXPECT_FALSE(client.replyTruncated());
}

TEST(AiChatClient, ReportsTheProvidersFinishReason) {
  const std::string capped =
      R"(data: {"choices":[{"index":0,"delta":{},"finish_reason":"length"}]})"
      "\n\ndata: [DONE]\n\n";

  std::array<char, 512> buf{};
  AiChatClient client(buf.data(), buf.size());
  client.begin();
  drive(client, chunk("说到一半") + capped, 64);
  client.end();

  EXPECT_STREQ(client.finishReason(), "length");
}

TEST(AiChatClient, IsIndependentOfTransportChunkSize) {
  const std::string stream = chunk("稳") + chunk("定") + chunk("的") + "data: [DONE]\n\n";
  for (size_t size = 1; size <= 40; ++size) {
    std::array<char, 512> buf{};
    AiChatClient client(buf.data(), buf.size());
    client.begin();
    drive(client, stream, size);
    client.end();
    EXPECT_STREQ(client.reply(), "稳定的") << "chunk size " << size;
    EXPECT_TRUE(client.complete()) << "chunk size " << size;
  }
}

}  // namespace
