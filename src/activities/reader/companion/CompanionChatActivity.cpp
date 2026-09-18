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
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

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
  buildQuestionList();

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
}

void CompanionChatActivity::buildQuestionList() {
  questions.clear();
  const size_t count = questionSet->count();
  if (count > 0) {
    questions.reserve(count);
    for (size_t i = 0; i < count; ++i) questions.emplace_back(questionSet->at(i));
    return;
  }
  // No questions.txt yet. The built-in list is translated, which is why it
  // lives here rather than in the library.
  questions.reserve(3);
  questions.emplace_back(tr(STR_COMPANION_Q_CHAPTER));
  questions.emplace_back(tr(STR_COMPANION_Q_CRAFT));
  questions.emplace_back(tr(STR_COMPANION_Q_CHARACTER));
}

void CompanionChatActivity::loop() {
  // A child activity owns the screen and the input in these states.
  if (state == State::WifiSelection || state == State::ShowingAnswer) return;

  if (state == State::Thinking) return;  // transient: the exchange runs inline

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) finish();
    return;
  }

  if (questions.empty()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startAsk();
    return;
  }

  buttonNavigator.onNext([this] {
    selected = ButtonNavigator::nextIndex(selected, static_cast<int>(questions.size()));
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selected = ButtonNavigator::previousIndex(selected, static_cast<int>(questions.size()));
    requestUpdate();
  });
}

void CompanionChatActivity::startAsk() {
  if (WiFi.status() != WL_CONNECTED) {
    launchWifiSelection();
    return;
  }
  runExchange(questions[static_cast<size_t>(selected)]);
}

void CompanionChatActivity::launchWifiSelection() {
  const std::string question = questions[static_cast<size_t>(selected)];
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

  // The builder writes straight into the string's storage, so the assembled
  // body is never copied on its way to the transport.
  std::string body;
  body.resize(REQUEST_BYTES);
  PromptBuilder builder(body.data(), body.size());

  PromptBuilder::Position position;
  position.bookTitle = context.bookTitle.empty() ? nullptr : context.bookTitle.c_str();
  position.author = context.author.empty() ? nullptr : context.author.c_str();
  position.chapterTitle = context.chapterTitle.empty() ? nullptr : context.chapterTitle.c_str();
  position.percent = context.percent;

  std::array<PromptBuilder::Exchange, ConversationStore::MAX_EXCHANGES> exchanges{};
  const size_t replayed = history->toExchanges(exchanges.data(), exchanges.size());

  builder.setModel(config.model());
  builder.setPersona(persona->text());
  builder.setPosition(position);
  builder.setExcerpt(context.excerpt.empty() ? nullptr : context.excerpt.c_str());
  builder.setHistory(exchanges.data(), replayed);
  builder.setQuestion(question.c_str());
  builder.setMaxTokens(config.maxTokens());

  if (!builder.build()) {
    LOG_ERR(LOG_TAG, "request did not fit %u bytes", static_cast<unsigned>(REQUEST_BYTES));
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }
  body.resize(builder.length());

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
      config.endpoint(), body, [&client](const uint8_t* data, const size_t len) { return client.onData(data, len); },
      config.key(), &status);
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
    failWith(StrId::STR_COMPANION_UNREACHABLE);
    return;
  }
  if (client.replyLength() == 0) {
    errorDetail.clear();
    failWith(StrId::STR_COMPANION_NO_REPLY);
    return;
  }

  const std::string reply(client.reply(), client.replyLength());
  // Recorded only once the reply is complete, so a failed exchange leaves no
  // half-turn for the next request to replay.
  history->append(question.c_str(), reply.c_str());
  CompanionFiles::saveHistory(context.bookPath, *history);
  showAnswer(question, reply);
}

void CompanionChatActivity::showAnswer(const std::string& question, const std::string& reply) {
  state = State::ShowingAnswer;
  if (!startActivityForResultWith<DictionaryDefinitionActivity>(
          [this](const ActivityResult&) {
            state = State::PickQuestion;
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

  for (size_t i = 0; i < questions.size(); ++i) {
    const bool isSelected = static_cast<int>(i) == selected;
    if (isSelected) renderer.drawText(UI_12_FONT_ID, x, y, ">", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + SELECTION_GUTTER, y, questions[i].c_str(), true,
                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += lineHeight + metrics.verticalSpacing;
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
  if (state == State::WifiSelection || state == State::ShowingAnswer) return;

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

  const auto labels = state == State::PickQuestion
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_COMPANION_ASK), "^", "v")
                          : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
