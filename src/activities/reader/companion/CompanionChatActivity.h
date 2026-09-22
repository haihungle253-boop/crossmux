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
  enum class State : uint8_t { PickQuestion, FollowUp, WifiSelection, Thinking, ShowingAnswer, TypingQuestion, Error };

  // Request and reply buffers, allocated only for the duration of an exchange.
  //
  // The request buffer has to hold PromptBuilder's three section caps at once
  // (1024 persona + 4096 history + 3072 excerpt = 8192 of source text) plus JSON
  // escaping, the spoiler guard, the reading position and the question itself,
  // which measures at just under 9.6 KB when every section is full. 8192 was
  // therefore not a headroom figure but a guaranteed failure once a book's
  // history filled up. PromptBuilder drops old exchanges rather than failing
  // now, so this is a comfort setting: enough room to keep the whole memory
  // window instead of quietly shortening it.
  static constexpr size_t REQUEST_BYTES = 12288;
  static constexpr size_t REPLY_BYTES = 8192;

  // How long away counts as having lost the thread. Short enough that the
  // offer is still useful, long enough that a reader who picks the book up
  // every evening never sees it.
  static constexpr int RESUME_AFTER_DAYS = 3;

  void buildQuestionList();
  void buildFollowUpList();
  // The list the selection is currently moving over: openers before the first
  // reply, continuations after it.
  const std::vector<std::string>& activeList() const;
  void startAsk();
  void activateSelection();
  void launchKeyboard();
  void launchWifiSelection();
  void runExchange(const std::string& question);
  void showAnswer(const std::string& question, const std::string& reply);
  void failWith(StrId message);
  // What the provider's HTTP status means in words the reader can act on.
  static StrId statusMessage(int status);

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
  std::vector<std::string> followUps;
  int selected = 0;

  // Days since the last recorded exchange about this book, or -1 when there is
  // none or the device clock cannot be trusted.
  int daysSinceLastTalk = -1;
  // Whether the openers begin with "where did we leave off", and whether that
  // is the one being asked right now -- a resume question is about the
  // conversation, so it travels without the current page attached.
  bool resumeOffered = false;
  bool askingForResume = false;

  State state = State::PickQuestion;
  StrId errorMessage = StrId::STR_COMPANION_UNREACHABLE;
  std::string errorDetail;
  bool personaIsFallback = false;

  ButtonNavigator buttonNavigator;
};
