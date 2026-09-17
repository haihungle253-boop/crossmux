#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lib/AiCompanion/ConversationStore.h"
#include "lib/AiCompanion/PersonaStore.h"
#include "lib/AiCompanion/QuestionSet.h"
#include "lib/JsonParser/StreamingJsonParser.h"

namespace {

void loadPersona(PersonaStore& store, const std::string& text) { store.load(text.data(), text.size()); }
void loadQuestions(QuestionSet& set, const std::string& text) { set.load(text.data(), text.size()); }

const std::string kRealPersona =
    "你是我的读书搭子，我们正在一起读同一本书，进度完全同步。\n"
    "\n"
    "性格：聪明、友善、博学但不端着，说话像老友不像老师。\n"
    "规矩：这章要是平淡就直说，不用硬找亮点。\n"
    "      回答控制在 200 字以内。\n";

// ---------------------------------------------------------------- PersonaStore

TEST(PersonaStore, LoadsAHandWrittenPersona) {
  PersonaStore store;
  loadPersona(store, kRealPersona);
  EXPECT_FALSE(store.usingFallback());
  EXPECT_FALSE(store.wasTruncated());
  EXPECT_NE(std::string(store.text()).find("读书搭子"), std::string::npos);
  EXPECT_NE(std::string(store.text()).find("不用硬找亮点"), std::string::npos);
}

TEST(PersonaStore, FallsBackWhenAbsentOrBlank) {
  PersonaStore store;
  EXPECT_TRUE(store.usingFallback());  // before any load

  EXPECT_FALSE(store.load(nullptr, 0));
  EXPECT_TRUE(store.usingFallback());

  loadPersona(store, "   \n\n\t\n");
  EXPECT_TRUE(store.usingFallback());
  EXPECT_GT(store.length(), 0u) << "the fallback must still be usable text";
}

// Notepad writes a UTF-8 BOM. Passed through, it would put three invisible
// bytes at the head of every system prompt.
TEST(PersonaStore, StripsUtf8Bom) {
  PersonaStore store;
  loadPersona(store, std::string("\xEF\xBB\xBF") + "你是我的读书搭子。");
  EXPECT_FALSE(store.usingFallback());
  EXPECT_STREQ(store.text(), "你是我的读书搭子。");
}

TEST(PersonaStore, NormalisesWindowsLineEndings) {
  PersonaStore store;
  loadPersona(store, "第一行\r\n第二行\r\n");
  EXPECT_STREQ(store.text(), "第一行\n第二行");
  EXPECT_EQ(std::string(store.text()).find('\r'), std::string::npos);
}

TEST(PersonaStore, DropsStrayControlCharacters) {
  PersonaStore store;
  loadPersona(store, std::string("好的") + '\x01' + "搭子" + '\x1f');
  EXPECT_STREQ(store.text(), "好的搭子");
  EXPECT_EQ(store.droppedControlChars(), 2u);
}

// An over-long persona is cut, not rejected: a reader who pastes an essay should
// get a working companion, and the cut must not halve a CJK character.
TEST(PersonaStore, TruncatesOnUtf8BoundaryInsteadOfRejecting) {
  PersonaStore store;
  std::string huge;
  while (huge.size() < PersonaStore::MAX_BYTES + 90) huge += "中文人设";
  loadPersona(store, huge);

  EXPECT_FALSE(store.usingFallback());
  EXPECT_TRUE(store.wasTruncated());
  EXPECT_LE(store.length(), PersonaStore::MAX_BYTES);
  EXPECT_EQ(store.length() % 3, 0u) << "every retained character must be whole";
}

TEST(PersonaStore, ClearReturnsToTheBuiltIn) {
  PersonaStore store;
  loadPersona(store, kRealPersona);
  ASSERT_FALSE(store.usingFallback());
  store.clear();
  EXPECT_TRUE(store.usingFallback());
  EXPECT_STREQ(store.text(), PersonaStore::builtInPersona());
}

// ------------------------------------------------------------------ QuestionSet

TEST(QuestionSet, ParsesLinesAndSkipsCommentsAndBlanks) {
  QuestionSet set;
  loadQuestions(set,
                "# 一行一个问题\n"
                "读完这一章，你有什么感受？\n"
                "\n"
                "   \n"
                "这一段用了什么写作手法？\n"
                "# 结尾注释\n");
  ASSERT_EQ(set.count(), 2u);
  EXPECT_STREQ(set.at(0), "读完这一章，你有什么感受？");
  EXPECT_STREQ(set.at(1), "这一段用了什么写作手法？");
}

TEST(QuestionSet, HandlesCrlfBomAndTrailingSpaces) {
  QuestionSet set;
  loadQuestions(set, std::string("\xEF\xBB\xBF") + "第一个问题？  \r\n\t第二个问题？\r\n");
  ASSERT_EQ(set.count(), 2u);
  EXPECT_STREQ(set.at(0), "第一个问题？");
  EXPECT_STREQ(set.at(1), "第二个问题？");
}

TEST(QuestionSet, LastLineWithoutNewlineIsKept) {
  QuestionSet set;
  loadQuestions(set, "只有一行，而且没有换行符");
  ASSERT_EQ(set.count(), 1u);
  EXPECT_STREQ(set.at(0), "只有一行，而且没有换行符");
}

TEST(QuestionSet, CapsCountAndReportsIgnoredLines) {
  QuestionSet set;
  std::string many;
  for (size_t i = 0; i < QuestionSet::MAX_QUESTIONS + 4; ++i) many += "问题" + std::to_string(i) + "\n";
  loadQuestions(set, many);

  EXPECT_EQ(set.count(), QuestionSet::MAX_QUESTIONS);
  EXPECT_EQ(set.ignoredLines(), 4u);
}

TEST(QuestionSet, OutOfRangeIndexIsSafe) {
  QuestionSet set;
  EXPECT_TRUE(set.empty());
  EXPECT_STREQ(set.at(0), "");
  EXPECT_STREQ(set.at(999), "");
}

// ------------------------------------------------------------ ConversationStore

TEST(ConversationStore, KeepsExchangesInOrder) {
  ConversationStore store;
  EXPECT_TRUE(store.append("第一个问题", "第一个回答"));
  EXPECT_TRUE(store.append("第二个问题", "第二个回答"));

  ASSERT_EQ(store.count(), 2u);
  EXPECT_STREQ(store.question(0), "第一个问题");
  EXPECT_STREQ(store.reply(1), "第二个回答");
}

TEST(ConversationStore, RejectsAnEmptyExchange) {
  ConversationStore store;
  EXPECT_FALSE(store.append("", ""));
  EXPECT_FALSE(store.append(nullptr, nullptr));
  EXPECT_TRUE(store.empty());
}

// The newest turns are the ones a follow-up depends on, so pressure must fall on
// the oldest.
TEST(ConversationStore, EvictsOldestWhenTheCountCapBinds) {
  ConversationStore store;
  for (size_t i = 0; i < ConversationStore::MAX_EXCHANGES + 3; ++i) {
    ASSERT_TRUE(store.append(("问题" + std::to_string(i)).c_str(), ("回答" + std::to_string(i)).c_str()));
  }
  EXPECT_EQ(store.count(), ConversationStore::MAX_EXCHANGES);
  EXPECT_EQ(store.evicted(), 3u);
  EXPECT_STREQ(store.question(0), "问题3") << "the three oldest should be gone";
  EXPECT_STREQ(store.question(store.count() - 1), "问题12");
}

TEST(ConversationStore, EvictsOldestWhenTheArenaCapBinds) {
  ConversationStore store;
  const std::string bigReply(ConversationStore::MAX_REPLY_BYTES, 'x');
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(store.append("q", bigReply.c_str()));

  EXPECT_LE(store.usedBytes(), ConversationStore::ARENA_BYTES);
  EXPECT_GT(store.evicted(), 0u);
  EXPECT_GT(store.count(), 0u);
}

TEST(ConversationStore, CapsOverlongFieldsOnUtf8Boundaries) {
  ConversationStore store;
  std::string longReply;
  while (longReply.size() < ConversationStore::MAX_REPLY_BYTES + 60) longReply += "回答";
  ASSERT_TRUE(store.append("问", longReply.c_str()));

  const size_t kept = strlen(store.reply(0));
  EXPECT_LE(kept, ConversationStore::MAX_REPLY_BYTES);
  EXPECT_EQ(kept % 3, 0u) << "every retained character must be whole";
}

TEST(ConversationStore, ConvertsToPromptBuilderExchanges) {
  ConversationStore store;
  store.append("问一", "答一");
  store.append("问二", "答二");

  std::array<PromptBuilder::Exchange, 4> out{};
  const size_t n = store.toExchanges(out.data(), out.size());
  ASSERT_EQ(n, 2u);
  EXPECT_STREQ(out[0].question, "问一");
  EXPECT_STREQ(out[1].reply, "答二");
}

TEST(ConversationStore, SurvivesASaveAndLoadRoundTrip) {
  ConversationStore store;
  store.append("这章你怎么看？", "有点闷，但那两句天气在干活。");
  store.append("怎么讲？", "用环境替角色说心事，省掉一整段心理描写。");

  std::vector<uint8_t> blob(store.serializedSize());
  ASSERT_EQ(store.serialize(blob.data(), blob.size()), blob.size());

  ConversationStore restored;
  ASSERT_TRUE(restored.deserialize(blob.data(), blob.size()));
  ASSERT_EQ(restored.count(), 2u);
  EXPECT_STREQ(restored.question(0), "这章你怎么看？");
  EXPECT_STREQ(restored.reply(1), "用环境替角色说心事，省掉一整段心理描写。");
}

TEST(ConversationStore, SerializeRefusesATooSmallBuffer) {
  ConversationStore store;
  store.append("问", "答");
  std::array<uint8_t, 4> tiny{};
  EXPECT_EQ(store.serialize(tiny.data(), tiny.size()), 0u);
}

// Replaying a corrupted history into a prompt would be worse than losing it, so
// every rejection path must leave the store empty rather than half-filled.
TEST(ConversationStore, RejectsCorruptedDataAndStaysEmpty) {
  ConversationStore store;
  store.append("问", "答");
  std::vector<uint8_t> good(store.serializedSize());
  store.serialize(good.data(), good.size());

  {  // wrong magic
    auto bad = good;
    bad[0] ^= 0xFF;
    ConversationStore target;
    EXPECT_FALSE(target.deserialize(bad.data(), bad.size()));
    EXPECT_TRUE(target.empty());
  }
  {  // unknown version
    auto bad = good;
    bad[4] = 0x99;
    ConversationStore target;
    EXPECT_FALSE(target.deserialize(bad.data(), bad.size()));
    EXPECT_TRUE(target.empty());
  }
  {  // truncated body
    std::vector<uint8_t> bad(good.begin(), good.end() - 3);
    ConversationStore target;
    EXPECT_FALSE(target.deserialize(bad.data(), bad.size()));
    EXPECT_TRUE(target.empty());
  }
  {  // header only, claiming records that are not there
    std::vector<uint8_t> bad(good.begin(), good.begin() + ConversationStore::HEADER_BYTES);
    ConversationStore target;
    EXPECT_FALSE(target.deserialize(bad.data(), bad.size()));
    EXPECT_TRUE(target.empty());
  }
}

TEST(ConversationStore, EmptyStoreRoundTrips) {
  ConversationStore store;
  std::vector<uint8_t> blob(store.serializedSize());
  ASSERT_EQ(store.serialize(blob.data(), blob.size()), ConversationStore::HEADER_BYTES);

  ConversationStore restored;
  restored.append("会被清掉的", "会被清掉的");
  EXPECT_TRUE(restored.deserialize(blob.data(), blob.size()));
  EXPECT_TRUE(restored.empty());
}

}  // namespace

// ------------------------------------------------- the files we actually ship
//
// assets/companion/ is copied to a reader's SD card as their starting point. If
// an edit there ever breaks parsing -- a stray encoding, a persona grown past
// the cap -- it should fail here rather than on someone's device.

namespace {

std::string readShippedFile(const char* name) {
  const std::string path = std::string(COMPANION_ASSET_DIR) + "/" + name;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

}  // namespace

TEST(ShippedAssets, PersonaLoadsAndIsNotTheFallback) {
  const std::string text = readShippedFile("persona.txt");
  ASSERT_FALSE(text.empty()) << "assets/companion/persona.txt is missing or unreadable";

  PersonaStore store;
  EXPECT_TRUE(store.load(text.data(), text.size()));
  EXPECT_FALSE(store.usingFallback());
  EXPECT_FALSE(store.wasTruncated()) << "the shipped persona must fit inside the cap";
  EXPECT_EQ(store.droppedControlChars(), 0u) << "the shipped persona must be clean UTF-8";
  EXPECT_LE(store.length(), PersonaStore::MAX_BYTES);
}

TEST(ShippedAssets, QuestionsParseWithNothingIgnored) {
  const std::string text = readShippedFile("questions.txt");
  ASSERT_FALSE(text.empty()) << "assets/companion/questions.txt is missing or unreadable";

  QuestionSet set;
  set.load(text.data(), text.size());
  EXPECT_GT(set.count(), 0u);
  EXPECT_EQ(set.ignoredLines(), 0u) << "the shipped list must fit inside the caps";
  EXPECT_EQ(set.truncatedLines(), 0u);
  for (size_t i = 0; i < set.count(); ++i) {
    EXPECT_NE(set.at(i)[0], '#') << "comment leaked through at index " << i;
  }
}

// The persona and the questions are assembled into one request; the pairing has
// to fit the budget, not just each file on its own.
TEST(ShippedAssets, PersonaAndAQuestionBuildAValidRequest) {
  const std::string personaText = readShippedFile("persona.txt");
  const std::string questionText = readShippedFile("questions.txt");
  ASSERT_FALSE(personaText.empty());
  ASSERT_FALSE(questionText.empty());

  PersonaStore persona;
  persona.load(personaText.data(), personaText.size());
  QuestionSet questions;
  questions.load(questionText.data(), questionText.size());
  ASSERT_GT(questions.count(), 0u);

  ConversationStore history;
  history.append("上一轮问的", "上一轮答的");
  std::array<PromptBuilder::Exchange, ConversationStore::MAX_EXCHANGES> exchanges{};
  const size_t n = history.toExchanges(exchanges.data(), exchanges.size());

  std::array<char, 8192> buf{};
  PromptBuilder builder(buf.data(), buf.size());
  PromptBuilder::Position position;
  position.bookTitle = "百年孤独";
  position.chapterTitle = "第十二章";
  position.percent = 43;
  builder.setPersona(persona.text());
  builder.setPosition(position);
  builder.setHistory(exchanges.data(), n);
  builder.setExcerpt("他说：“这不可能。”\n她没有回答，只是把窗户关上了。");
  builder.setQuestion(questions.at(0));

  ASSERT_TRUE(builder.build());

  JsonCallbacks cb{};
  StreamingJsonParser verifier(cb);
  verifier.feed(builder.body(), builder.length());
  EXPECT_FALSE(verifier.hasError()) << "the shipped persona must survive JSON assembly";
}
