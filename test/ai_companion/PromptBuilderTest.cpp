#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "lib/AiCompanion/PromptBuilder.h"
#include "lib/JsonParser/StreamingJsonParser.h"

namespace {

// Round-trips the built body through the same parser the firmware uses. Escaping
// bugs are the likeliest defect in this class and the hardest to spot by eye, so
// validity is asserted rather than inspected.
bool parsesAsJson(const char* body, const size_t len) {
  JsonCallbacks cb{};
  cb.ctx = nullptr;
  StreamingJsonParser parser(cb);
  parser.feed(body, len);
  return !parser.hasError();
}

bool contains(const char* haystack, const std::string& needle) {
  return std::string(haystack).find(needle) != std::string::npos;
}

PromptBuilder::Position samplePosition() {
  PromptBuilder::Position p;
  p.bookTitle = "百年孤独";
  p.author = "加西亚·马尔克斯";
  p.chapterTitle = "第十二章";
  p.percent = 43;
  return p;
}

TEST(PromptBuilder, ProducesValidJsonWithRequiredFields) {
  std::array<char, 4096> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setModel("test-model");
  builder.setPersona("You are a companion.");
  builder.setQuestion("What did you make of that chapter?");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "\"model\":\"test-model\""));
  EXPECT_TRUE(contains(builder.body(), "\"stream\":true"));
  EXPECT_TRUE(contains(builder.body(), "\"role\":\"system\""));
  EXPECT_TRUE(contains(builder.body(), "\"role\":\"user\""));
}

// Book text routinely contains all of these. Any one of them unescaped makes
// the request unparseable at the provider.
TEST(PromptBuilder, EscapesQuotesBackslashesAndNewlines) {
  std::array<char, 4096> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setExcerpt("She said \"stop\".\nHe wrote C:\\path\tand left.");
  builder.setQuestion("Thoughts?");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "\\\"stop\\\""));
  EXPECT_TRUE(contains(builder.body(), "\\\\path"));
  EXPECT_TRUE(contains(builder.body(), "\\n"));
  EXPECT_TRUE(contains(builder.body(), "\\t"));
}

TEST(PromptBuilder, EscapesControlCharacters) {
  std::array<char, 2048> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  const char excerpt[] = {'a', 0x01, 'b', 0x1f, 'c', '\0'};
  builder.setExcerpt(excerpt);
  builder.setQuestion("q");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "\\u0001"));
  EXPECT_TRUE(contains(builder.body(), "\\u001f"));
}

// The guard is what makes shared progress real; it must not depend on the
// reader having written anything particular in their persona file.
TEST(PromptBuilder, AlwaysEmitsProgressGuardWithPosition) {
  std::array<char, 4096> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setPersona("Be blunt.");
  builder.setPosition(samplePosition());
  builder.setQuestion("q");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "Be blunt."));
  EXPECT_TRUE(contains(builder.body(), "Never reveal, hint at, or speculate about anything beyond this point"));
  EXPECT_TRUE(contains(builder.body(), "百年孤独"));
  EXPECT_TRUE(contains(builder.body(), "第十二章"));
  EXPECT_TRUE(contains(builder.body(), "Progress: 43%"));
}

TEST(PromptBuilder, FallsBackToADefaultPersona) {
  std::array<char, 2048> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setPersona("");
  builder.setQuestion("q");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(contains(builder.body(), "reading companion"));
}

// A cap landing inside a multi-byte character would put invalid UTF-8 on the
// wire. Each of these characters is three bytes, so a 10-byte cap must yield 9.
TEST(PromptBuilder, ExcerptCapTruncatesOnUtf8Boundary) {
  std::array<char, 4096> buf{};
  PromptBuilder::Limits limits;
  limits.excerptBytes = 10;
  PromptBuilder builder(buf.data(), buf.size(), limits);
  builder.setExcerpt("一二三四五");  // 15 bytes
  builder.setQuestion("q");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "一二三"));
  EXPECT_FALSE(contains(builder.body(), "四"));
}

TEST(PromptBuilder, HistoryIsReplayedAsAlternatingTurns) {
  std::array<char, 4096> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  const std::vector<PromptBuilder::Exchange> history = {
      {"Who is he?", "A soldier."},
      {"And her?", "His sister."},
  };
  builder.setHistory(history.data(), history.size());
  builder.setQuestion("What now?");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "\"role\":\"assistant\""));
  EXPECT_TRUE(contains(builder.body(), "A soldier."));
  EXPECT_TRUE(contains(builder.body(), "His sister."));
  EXPECT_EQ(builder.droppedExchanges(), 0u);
}

// When the window binds, the recent exchanges are the ones a follow-up depends
// on, so the oldest must be the ones dropped.
TEST(PromptBuilder, HistoryCapDropsOldestAndKeepsNewest) {
  std::array<char, 8192> buf{};
  PromptBuilder::Limits limits;
  limits.historyBytes = 64;
  PromptBuilder builder(buf.data(), buf.size(), limits);

  const std::vector<PromptBuilder::Exchange> history = {
      {"oldest question here", "oldest reply here"},
      {"middle question here", "middle reply here"},
      {"newest question here", "newest reply here"},
  };
  builder.setHistory(history.data(), history.size());
  builder.setQuestion("next");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "newest reply here"));
  EXPECT_FALSE(contains(builder.body(), "oldest reply here"));
  EXPECT_GE(builder.droppedExchanges(), 1u);
}

// Failing closed matters: a half-written body would be sent and rejected, and
// the error would point at the provider rather than at us.
TEST(PromptBuilder, FailsClosedWhenOutputBufferIsTooSmall) {
  std::array<char, 64> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setPersona("a persona long enough to overflow this tiny buffer");
  builder.setExcerpt("and an excerpt too");
  builder.setQuestion("and a question");

  EXPECT_FALSE(builder.build());
  EXPECT_EQ(builder.length(), 0u);
  EXPECT_STREQ(builder.body(), "");
}

TEST(PromptBuilder, MaxTokensOmittedWhenUnset) {
  std::array<char, 2048> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setQuestion("q");
  ASSERT_TRUE(builder.build());
  EXPECT_FALSE(contains(builder.body(), "max_tokens"));

  builder.setMaxTokens(300);
  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(contains(builder.body(), "\"max_tokens\":300"));
}

TEST(PromptBuilder, DropsOldestExchangesUntilTheRequestFits) {
  // The section caps do not add up to a promise: a full persona, a full history
  // and a full excerpt are each inside their own limit and together exceed any
  // buffer sized from the caps alone. Before this was handled, a reader whose
  // history had filled up got a failed build on every question for the rest of
  // that book -- history only grows, so the failure never cleared.
  const std::string cjk = [] {
    std::string s;
    while (s.size() < 3072) s += "中";
    return s;
  }();

  std::vector<std::string> questions, replies;
  std::vector<PromptBuilder::Exchange> history;
  for (int i = 0; i < 10; ++i) {
    questions.push_back(cjk.substr(0, 102));
    replies.push_back(cjk.substr(0, 306));
  }
  for (int i = 0; i < 10; ++i) history.push_back({questions[i].c_str(), replies[i].c_str()});

  std::array<char, 8192> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setModel("deepseek-chat");
  builder.setPersona(cjk.substr(0, 1020).c_str());
  builder.setPosition(samplePosition());
  builder.setExcerpt(cjk.c_str());
  builder.setHistory(history.data(), history.size());
  builder.setQuestion("这一章你怎么看？");
  builder.setMaxTokens(800);

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_GT(builder.droppedExchanges(), 0u);
  // What it gives up is the far end of the conversation, never the question in
  // front of it or the newest exchange the follow-up depends on.
  EXPECT_TRUE(contains(builder.body(), "这一章你怎么看？"));
  EXPECT_LT(builder.droppedExchanges(), history.size());
}

TEST(PromptBuilder, SucceedsWithNoHistoryWhenEvenOneExchangeWillNotFit) {
  // The last pass carries no history at all. A reader who has just opened a
  // book is in exactly that state, so this request must always be possible.
  std::string huge(6000, 'x');
  PromptBuilder::Exchange one{huge.c_str(), huge.c_str()};

  std::array<char, 4096> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  builder.setHistory(&one, 1);
  builder.setQuestion("q");

  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_EQ(builder.droppedExchanges(), 1u);
}

TEST(PromptBuilder, TellsTheCompanionHowLongTheReaderHasBeenAway) {
  std::array<char, 2048> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  PromptBuilder::Position position = samplePosition();
  builder.setPosition(position);
  builder.setQuestion("上次我们聊到哪儿了？");

  // Unknown by default: an unset device clock must produce silence, not a
  // confident number.
  ASSERT_TRUE(builder.build());
  EXPECT_FALSE(contains(builder.body(), "Days since you last talked"));

  position.daysSinceLastTalk = 12;
  builder.setPosition(position);
  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(parsesAsJson(builder.body(), builder.length()));
  EXPECT_TRUE(contains(builder.body(), "Days since you last talked about it: 12"));

  // Same day still counts as known, and says so.
  position.daysSinceLastTalk = 0;
  builder.setPosition(position);
  ASSERT_TRUE(builder.build());
  EXPECT_TRUE(contains(builder.body(), "Days since you last talked about it: 0"));
}

}  // namespace
