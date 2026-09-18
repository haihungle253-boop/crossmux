#include "CompanionChatActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <array>
#include <utility>

#include "AiChatClient.h"
#include "PromptBuilder.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/TimeUtils.h"

namespace {

constexpr char LOG_TAG[] = "COMPANION";

// Marker column for the selected row, in the same spirit as the reader's other
// button-driven lists.
constexpr int SELECTION_GUTTER = 24;

}  // namespace

void CompanionChatActivity::onEnter() {
  Activity::onEnter();

  persona = makeUniqueNoThrow<PersonaStore>();
  questionSet = makeUniqueNoThrow<QuestionSet>();
  history = makeUniqueNoThrow<ConversationStore>();
  if (!persona || !questionSet || !history) {
    LOG_ERR(LOG_TAG, "OOM allocating companion state");
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }

  CompanionFiles::loadPersona(*persona);
  personaIsFallback = persona->usingFallback();
  CompanionFiles::loadQuestions(*questionSet);
  CompanionFiles::loadHistory(context.bookPath, *history);

  // getCurrentValidTimestamp() returns 0 when the clock has not been set, and
  // exchanges stored while it was unset carry 0 too. Either way the gap stays
  // unknown and the offer is simply not made -- a companion that announces "it
  // has been 20440 days" because the clock came up at the epoch is worse than
  // one that says nothing.
  const uint32_t lastAt = history->lastTimestamp();
  const uint32_t nowSeconds = TimeUtils::getCurrentValidTimestamp();
  if (lastAt > 0 && nowSeconds > lastAt) {
    daysSinceLastTalk = static_cast<int>((nowSeconds - lastAt) / 86400u);
  }
  resumeOffered = !history->empty() && daysSinceLastTalk >= RESUME_AFTER_DAYS;

  buildQuestionList();
  buildFollowUpList();

  if (!CompanionFiles::loadConfig(config)) {
    errorMessage = StrId::STR_COMPANION_NOT_SET_UP;
    state = State::Error;
  }

  requestUpdate();
}

void CompanionChatActivity::onExit() {
  Activity::onExit();
  // Everything allocated in onEnter goes here; the reader underneath should not
  // pay for a conversation that has ended.
  persona.reset();
  questionSet.reset();
  history.reset();
  questions.clear();
  questions.shrink_to_fit();
  followUps.clear();
  followUps.shrink_to_fit();
}

void CompanionChatActivity::buildQuestionList() {
  questions.clear();
  // First, because it is the question a reader returning to a half-read book
  // actually has, and because it is the one that stops being useful the moment
  // they have read on.
  if (resumeOffered) questions.emplace_back(tr(STR_COMPANION_Q_RESUME));

  const size_t count = questionSet->count();
  if (count > 0) {
    questions.reserve(questions.size() + count);
    for (size_t i = 0; i < count; ++i) questions.emplace_back(questionSet->at(i));
    return;
  }
  // No questions.txt yet. The built-in list is translated, which is why it
  // lives here rather than in the library.
  questions.reserve(questions.size() + 3);
  questions.emplace_back(tr(STR_COMPANION_Q_CHAPTER));
  questions.emplace_back(tr(STR_COMPANION_Q_CRAFT));
  questions.emplace_back(tr(STR_COMPANION_Q_CHARACTER));
}

// Continuations are the same four moves a real conversation is mostly made of:
// go on, try another angle, I'm not sure I agree, and what happened next. Four
// buttons cannot carry typing, but they carry these, and that is enough to push
// an exchange somewhere worth going. The disagree option is the one that turns a
// generated answer into an actual conversation, so it is not optional.
void CompanionChatActivity::buildFollowUpList() {
  followUps.clear();
  followUps.reserve(5);
  followUps.emplace_back(tr(STR_COMPANION_MORE));
  followUps.emplace_back(tr(STR_COMPANION_ANGLE));
  followUps.emplace_back(tr(STR_COMPANION_DISAGREE));
  followUps.emplace_back(tr(STR_COMPANION_AND_THEN));
  // Always last, and always present: four fixed moves cover most of a
  // conversation, not all of it.
  followUps.emplace_back(tr(STR_COMPANION_TYPE_OWN));
}

const std::vector<std::string>& CompanionChatActivity::activeList() const {
  return state == State::FollowUp ? followUps : questions;
}

void CompanionChatActivity::activateSelection() {
  const auto& list = activeList();
  if (list.empty()) return;
  const size_t index = static_cast<size_t>(selected);
  // The typing option is the last continuation; everything else is sent as-is.
  if (state == State::FollowUp && index + 1 == list.size()) {
    launchKeyboard();
    return;
  }
  startAsk();
}

void CompanionChatActivity::launchKeyboard() {
  state = State::TypingQuestion;
  requestUpdate();

  if (!startActivityForResultWith<KeyboardEntryActivity>(
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              state = State::FollowUp;
              requestUpdate();
              return;
            }
            const std::string typed = std::get<KeyboardResult>(result.data).text;
            if (typed.empty()) {
              state = State::FollowUp;
              requestUpdate();
              return;
            }
            if (WiFi.status() != WL_CONNECTED) {
              failWith(StrId::STR_COMPANION_UNREACHABLE);
              return;
            }
            askingForResume = false;  // a typed question is about the page, not the gap
            runExchange(typed);
          },
          std::string(tr(STR_COMPANION_YOUR_QUESTION)), std::string(), size_t{240}, InputType::Text)) {
    failWith(StrId::STR_COMPANION_UNREACHABLE);
  }
}

void CompanionChatActivity::loop() {
  // A child activity owns the screen and the input in these states.
  if (state == State::WifiSelection || state == State::ShowingAnswer || state == State::TypingQuestion) {
    return;
  }

  if (state == State::Thinking) return;  // transient: the exchange runs inline

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back steps out one level: continuations return to the openers, and only
    // from there does it leave the conversation.
    if (state == State::FollowUp) {
      state = State::PickQuestion;
      selected = 0;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) finish();
    return;
  }

  const int itemCount = static_cast<int>(activeList().size());
  if (itemCount == 0) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
    return;
  }

  buttonNavigator.onNext([this, itemCount] {
    selected = ButtonNavigator::nextIndex(selected, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selected = ButtonNavigator::previousIndex(selected, itemCount);
    requestUpdate();
  });
}

void CompanionChatActivity::startAsk() {
  askingForResume = resumeOffered && state == State::PickQuestion && selected == 0;
  if (WiFi.status() != WL_CONNECTED) {
    launchWifiSelection();
    return;
  }
  runExchange(activeList()[static_cast<size_t>(selected)]);
}

void CompanionChatActivity::launchWifiSelection() {
  const std::string question = activeList()[static_cast<size_t>(selected)];
  state = State::WifiSelection;
  requestUpdate();

  if (!startActivityForResultWith<WifiSelectionActivity>([this, question](const ActivityResult& result) {
        if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
          failWith(StrId::STR_COMPANION_UNREACHABLE);
          return;
        }
        runExchange(question);
      })) {
    failWith(StrId::STR_COMPANION_UNREACHABLE);
  }
}

void CompanionChatActivity::runExchange(const std::string& question) {
  state = State::Thinking;
  // Paint before blocking: the exchange takes seconds, and a panel left showing
  // the previous screen would read as a freeze.
  requestUpdateAndWait();

  // A no-throw allocation, like the reply buffer below: with exceptions off a
  // std::string this size aborts the device on a failed allocation rather than
  // letting us show the reader an error.
  auto bodyBuf = makeUniqueNoThrow<char[]>(REQUEST_BYTES);
  if (!bodyBuf) {
    LOG_ERR(LOG_TAG, "OOM: %u byte request buffer", static_cast<unsigned>(REQUEST_BYTES));
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }
  PromptBuilder builder(bodyBuf.get(), REQUEST_BYTES);

  PromptBuilder::Position position;
  position.bookTitle = context.bookTitle.empty() ? nullptr : context.bookTitle.c_str();
  position.author = context.author.empty() ? nullptr : context.author.c_str();
  position.chapterTitle = context.chapterTitle.empty() ? nullptr : context.chapterTitle.c_str();
  position.percent = context.percent;
  position.daysSinceLastTalk = askingForResume ? daysSinceLastTalk : -1;

  std::array<PromptBuilder::Exchange, ConversationStore::MAX_EXCHANGES> exchanges{};
  const size_t replayed = history->toExchanges(exchanges.data(), exchanges.size());

  builder.setModel(config.model());
  builder.setPersona(persona->text());
  builder.setPosition(position);
  // "Where did we leave off" is answered from the conversation, not from
  // whatever page happens to be open -- which may be one the reader has not read
  // yet. Leaving the excerpt out keeps the question honest and buys 3 KB for the
  // history that does answer it.
  const bool sendExcerpt = !askingForResume && !context.excerpt.empty();
  builder.setExcerpt(sendExcerpt ? context.excerpt.c_str() : nullptr);
  builder.setHistory(exchanges.data(), replayed);
  builder.setQuestion(question.c_str());
  builder.setMaxTokens(config.maxTokens());

  if (!builder.build()) {
    LOG_ERR(LOG_TAG, "request did not fit %u bytes", static_cast<unsigned>(REQUEST_BYTES));
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }
  if (builder.droppedExchanges() > 0) {
    LOG_INF(LOG_TAG, "dropped %u oldest exchanges to fit the request",
            static_cast<unsigned>(builder.droppedExchanges()));
  }

  auto replyBuf = makeUniqueNoThrow<char[]>(REPLY_BYTES);
  if (!replyBuf) {
    LOG_ERR(LOG_TAG, "OOM: %u byte reply buffer", static_cast<unsigned>(REPLY_BYTES));
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }

  AiChatClient client(replyBuf.get(), REPLY_BYTES);
  client.begin();

  int status = 0;
  const bool delivered = HttpDownloader::postJson(
      config.endpoint(), builder.body(), builder.length(),
      [&client](const uint8_t* data, const size_t len) { return client.onData(data, len); }, config.key(), &status);
  client.end();

  if (client.hasError()) {
    // The provider explained itself; that is worth more than our own guess.
    errorDetail = client.errorMessage();
    LOG_ERR(LOG_TAG, "provider error: %s", errorDetail.c_str());
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }
  if (!delivered && client.replyLength() == 0) {
    LOG_ERR(LOG_TAG, "exchange failed, status=%d", status);
    errorDetail.clear();
    // "Could not reach the companion" sends the reader to look at Wi-Fi, which
    // is the wrong place when the provider answered and said no. A wrong key is
    // the likeliest first-run failure of all, so it gets its own words.
    failWith(statusMessage(status));
    return;
  }
  if (client.replyLength() == 0) {
    errorDetail.clear();
    failWith(StrId::STR_COMPANION_NO_REPLY);
    return;
  }

  const std::string reply(client.reply(), client.replyLength());
  if (!delivered && !client.complete()) {
    // Two independent ways of hearing that the reply finished: the provider's
    // [DONE] sentinel, and the transport reaching the end of the response. Only
    // when neither says so has the connection actually dropped part-way. Both
    // are needed -- not every OpenAI-compatible endpoint sends the sentinel, and
    // demanding it would mark perfectly good replies as broken.
    //
    // Show what came, because half an answer still reads, but do not record it:
    // history is replayed into every later request, and a truncated assistant
    // turn in it is a standing instruction to break off mid-sentence.
    LOG_ERR(LOG_TAG, "reply cut short at %u bytes; not recorded", static_cast<unsigned>(client.replyLength()));
    showAnswer(question, reply + "\n\n" + tr(STR_COMPANION_CUT_SHORT));
    return;
  }

  // Recorded only once the reply is complete, so a failed exchange leaves no
  // half-turn for the next request to replay.
  history->append(question.c_str(), reply.c_str(), TimeUtils::getCurrentValidTimestamp());
  CompanionFiles::saveHistory(context.bookPath, *history);

  // They have just caught up, so stop offering to catch them up.
  if (resumeOffered) {
    resumeOffered = false;
    askingForResume = false;
    buildQuestionList();
    selected = 0;
  }

  showAnswer(question, reply);
}

StrId CompanionChatActivity::statusMessage(const int status) {
  if (status == 401 || status == 403) return StrId::STR_COMPANION_BAD_KEY;
  if (status == 429 || status >= 500) return StrId::STR_COMPANION_BUSY;
  return StrId::STR_COMPANION_UNREACHABLE;
}

void CompanionChatActivity::showAnswer(const std::string& question, const std::string& reply) {
  state = State::ShowingAnswer;
  if (!startActivityForResultWith<DictionaryDefinitionActivity>(
          [this](const ActivityResult&) {
            // The conversation continues from here rather than resetting to the
            // openers: having just read a reply, the useful next move is a
            // follow-up.
            state = State::FollowUp;
            selected = 0;
            requestUpdate();
          },
          question, reply, false)) {
    failWith(StrId::STR_COMPANION_UNREACHABLE);
  }
}

void CompanionChatActivity::failWith(const StrId message) {
  state = State::Error;
  errorMessage = message;
  requestUpdate();
}

void CompanionChatActivity::drawQuestionList(const int contentX, const int contentWidth, const int contentY) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int x = contentX + metrics.contentSidePadding;
  int y = contentY + metrics.topPadding + metrics.headerHeight;

  const auto& list = activeList();

  // questions.txt is the reader's own file and has no length limit, so the list
  // can be longer than the panel. Without a window, the extra rows are drawn
  // under the button hints or off the bottom entirely -- while the selection
  // still walks onto them, leaving the reader pressing down against what looks
  // like a frozen screen.
  const int rowHeight = lineHeight + metrics.verticalSpacing;
  // Whichever edge the hints are on, this is the last y a row may start at.
  const int bottom = contentY + renderer.getScreenHeight() - metrics.buttonHintsHeight;
  int visibleRows = rowHeight > 0 ? (bottom - y) / rowHeight : 1;
  if (personaIsFallback) --visibleRows;  // the notice needs the last line
  if (visibleRows < 1) visibleRows = 1;

  size_t firstVisible = 0;
  if (selected >= visibleRows) firstVisible = static_cast<size_t>(selected - visibleRows + 1);
  const size_t lastVisible = firstVisible + static_cast<size_t>(visibleRows) < list.size()
                                 ? firstVisible + static_cast<size_t>(visibleRows)
                                 : list.size();

  for (size_t i = firstVisible; i < lastVisible; ++i) {
    const bool isSelected = static_cast<int>(i) == selected;
    if (isSelected) renderer.drawText(UI_12_FONT_ID, x, y, ">", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + SELECTION_GUTTER, y, list[i].c_str(), true,
                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += rowHeight;
  }

  if (personaIsFallback) {
    const char* notice = tr(STR_COMPANION_DEFAULT_PERSONA);
    const int noticeWidth = renderer.getTextWidth(UI_10_FONT_ID, notice);
    renderer.drawText(UI_10_FONT_ID, contentX + (contentWidth - noticeWidth) / 2, y + metrics.verticalSpacing, notice);
  }
}

void CompanionChatActivity::drawCentered(const StrId message, const int contentX, const int contentWidth,
                                         const int contentY) {
  const char* text = I18N.get(message);
  const int width = renderer.getTextWidth(UI_12_FONT_ID, text);
  const int y = contentY + renderer.getScreenHeight() / 2;
  renderer.drawText(UI_12_FONT_ID, contentX + (contentWidth - width) / 2, y, text);

  if (!errorDetail.empty()) {
    const int detailWidth = renderer.getTextWidth(UI_10_FONT_ID, errorDetail.c_str());
    renderer.drawText(UI_10_FONT_ID, contentX + (contentWidth - detailWidth) / 2,
                      y + renderer.getLineHeight(UI_12_FONT_ID), errorDetail.c_str());
  } else if (message == StrId::STR_COMPANION_NOT_SET_UP) {
    const char* hint = tr(STR_COMPANION_SETUP_HINT);
    const int hintWidth = renderer.getTextWidth(UI_10_FONT_ID, hint);
    renderer.drawText(UI_10_FONT_ID, contentX + (contentWidth - hintWidth) / 2,
                      y + renderer.getLineHeight(UI_12_FONT_ID), hint);
  }
}

void CompanionChatActivity::render(RenderLock&&) {
  // A child activity paints its own screen in these states.
  if (state == State::WifiSelection || state == State::ShowingAnswer || state == State::TypingQuestion) {
    return;
  }

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  GUI.drawHeader(renderer, Rect{contentX, contentY + metrics.topPadding, contentWidth, metrics.headerHeight},
                 tr(STR_COMPANION));

  switch (state) {
    case State::PickQuestion:
    case State::FollowUp:
      drawQuestionList(contentX, contentWidth, contentY);
      break;
    case State::Thinking:
      drawCentered(StrId::STR_COMPANION_THINKING, contentX, contentWidth, contentY);
      break;
    case State::Error:
      drawCentered(errorMessage, contentX, contentWidth, contentY);
      break;
    default:
      break;
  }

  const bool listVisible = state == State::PickQuestion || state == State::FollowUp;
  const auto labels = listVisible ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_COMPANION_ASK), "^", "v")
                                  : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
