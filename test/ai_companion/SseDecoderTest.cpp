#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/AiCompanion/SseDecoder.h"

namespace {

struct Capture {
  std::vector<std::string> events;
  int doneCount = 0;
};

void onEvent(void* ctx, const char* data, const size_t len) {
  static_cast<Capture*>(ctx)->events.emplace_back(data, len);
}
void onDone(void* ctx) { static_cast<Capture*>(ctx)->doneCount++; }

// Feeds a whole stream in slices of `chunk` bytes, to exercise reassembly.
Capture decode(const std::string& stream, const size_t chunk) {
  Capture cap;
  SseDecoder decoder(&cap, onEvent, onDone);
  for (size_t i = 0; i < stream.size(); i += chunk) {
    const size_t n = std::min(chunk, stream.size() - i);
    decoder.feed(stream.data() + i, n);
  }
  return cap;
}

TEST(SseDecoder, SingleEvent) {
  const auto cap = decode("data: {\"a\":1}\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "{\"a\":1}");
}

TEST(SseDecoder, MultipleEventsInOneChunk) {
  const auto cap = decode("data: one\n\ndata: two\n\ndata: three\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 3u);
  EXPECT_EQ(cap.events[0], "one");
  EXPECT_EQ(cap.events[1], "two");
  EXPECT_EQ(cap.events[2], "three");
}

// The property that matters most on a real socket: a frame boundary can land
// anywhere, including in the middle of a JSON object.
TEST(SseDecoder, SplitAcrossChunkBoundariesAtEverySize) {
  const std::string stream = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\ndata: [DONE]\n\n";
  for (size_t chunk = 1; chunk <= stream.size(); ++chunk) {
    const auto cap = decode(stream, chunk);
    ASSERT_EQ(cap.events.size(), 1u) << "chunk size " << chunk;
    EXPECT_EQ(cap.events[0], "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}") << "chunk size " << chunk;
    EXPECT_EQ(cap.doneCount, 1) << "chunk size " << chunk;
  }
}

TEST(SseDecoder, CrLfLineEndings) {
  const auto cap = decode("data: {\"a\":1}\r\n\r\n", 1024);
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "{\"a\":1}");
}

TEST(SseDecoder, DoneSentinelIsNotDeliveredAsAnEvent) {
  const auto cap = decode("data: [DONE]\n\n", 1024);
  EXPECT_TRUE(cap.events.empty());
  EXPECT_EQ(cap.doneCount, 1);
}

TEST(SseDecoder, IgnoresCommentsAndOtherFields) {
  const auto cap = decode(": keep-alive\nevent: message\nid: 42\nretry: 1000\ndata: payload\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "payload");
}

TEST(SseDecoder, OptionalSpaceAfterColonIsStrippedOnlyOnce) {
  const auto cap = decode("data:tight\n\ndata:  двойной\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 2u);
  EXPECT_EQ(cap.events[0], "tight");
  EXPECT_EQ(cap.events[1], " двойной");
}

TEST(SseDecoder, MultipleDataLinesJoinWithNewline) {
  const auto cap = decode("data: line one\ndata: line two\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "line one\nline two");
}

TEST(SseDecoder, BlankLinesBetweenEventsAreHarmless) {
  const auto cap = decode("\n\ndata: a\n\n\n\ndata: b\n\n", 1024);
  ASSERT_EQ(cap.events.size(), 2u);
  EXPECT_EQ(cap.events[0], "a");
  EXPECT_EQ(cap.events[1], "b");
}

// An oversized event must be dropped whole rather than delivered with a hole in
// it: half a JSON object would parse into plausible-looking wrong content.
TEST(SseDecoder, OversizedEventIsDroppedAndStreamRecovers) {
  const std::string huge(SseDecoder::EVENT_BUF_SIZE + 64, 'x');
  Capture cap;
  SseDecoder decoder(&cap, onEvent, onDone);
  const std::string stream = "data: " + huge + "\n\ndata: after\n\n";
  decoder.feed(stream.data(), stream.size());

  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "after");
  EXPECT_EQ(decoder.droppedEvents(), 1u);
}

TEST(SseDecoder, FinishFlushesEventNotTerminatedByBlankLine) {
  Capture cap;
  SseDecoder decoder(&cap, onEvent, onDone);
  const std::string stream = "data: trailing";
  decoder.feed(stream.data(), stream.size());
  EXPECT_TRUE(cap.events.empty());

  decoder.finish();
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "trailing");
}

TEST(SseDecoder, ResetClearsState) {
  Capture cap;
  SseDecoder decoder(&cap, onEvent, onDone);
  const std::string partial = "data: half";
  decoder.feed(partial.data(), partial.size());
  decoder.reset();

  const std::string rest = "data: whole\n\n";
  decoder.feed(rest.data(), rest.size());
  ASSERT_EQ(cap.events.size(), 1u);
  EXPECT_EQ(cap.events[0], "whole");
}

}  // namespace
