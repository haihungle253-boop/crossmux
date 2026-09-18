#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CompanionFiles.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// One conversation with the reading companion, opened from the reader.
//
// The flow is deliberately short: pick a question, wait, read the reply. A
// round trip costs a Wi-Fi association plus generation -- several seconds --
// which is fine at a reading pause and unacceptable between pages, so the
// companion is invited rather than ambient.
//
// The reply is shown by DictionaryDefinitionActivity, which is already a
// paginated text viewer with CJK wrapping and batched SD-font loading, proven
// on device. Reusing it costs a slightly odd class name and saves writing a
// second text layout engine; a rename to a neutral PagedTextActivity would be
// the tidy follow-up.
class CompanionChatActivity final : public Activity {
 public:
  // Everything the companion is told about where the reader is. The excerpt is
  // already clipped to what has been read -- this activity never reaches past
  // the reader's position, which is what makes the shared progress real.
  struct Context {
    std::string bookPath;
    std::string bookTitle;
    std::string author;
    std::string chapterTitle;
    int percent = -1;
    std::string excerpt;
  };

  // How much already-read text the activity wants in Context::excerpt. The
  // caller does the extraction, so it needs to know the budget.
  static constexpr size_t EXCERPT_BYTES = 3072;

  CompanionChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Context context)
      : Activity("CompanionChat", renderer, mappedInput), context(std::move(context)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // A blocking exchange must not be interrupted by an idle sleep part-way
  // through, and the reader is looking at a "thinking" screen meanwhile.
  bool preventAutoSleep() override { return state == State::Thinking; }

 private:
  enum class State : uint8_t { PickQuestion, WifiSelection, Thinking, ShowingAnswer, Error };

  // Request and reply buffers, allocated only for the duration of an exchange.
  static constexpr size_t REQUEST_BYTES = 8192;
  static constexpr size_t REPLY_BYTES = 8192;

  void buildQuestionList();
  void startAsk();
  void launchWifiSelection();
  void runExchange(const std::string& question);
  void showAnswer(const std::string& question, const std::string& reply);
  void failWith(StrId message);

  void drawQuestionList(int contentX, int contentWidth, int contentY);
  void drawCentered(StrId message, int contentX, int contentWidth, int contentY);

  Context context;

  // Heap-held so the activity object stays small and everything is released on
  // exit rather than living as long as the reader underneath it.
  std::unique_ptr<PersonaStore> persona;
  std::unique_ptr<QuestionSet> questionSet;
  std::unique_ptr<ConversationStore> history;
  CompanionConfig config;

  std::vector<std::string> questions;
  int selected = 0;

  State state = State::PickQuestion;
  StrId errorMessage = StrId::STR_COMPANION_UNREACHABLE;
  std::string errorDetail;
  bool personaIsFallback = false;

  ButtonNavigator buttonNavigator;
};
