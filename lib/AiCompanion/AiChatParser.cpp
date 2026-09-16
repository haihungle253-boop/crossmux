#include "AiChatParser.h"

#include <cstring>

#include "Utf8.h"

namespace {

constexpr char KEY_CHOICES[] = "choices";
constexpr char KEY_DELTA[] = "delta";
constexpr char KEY_CONTENT[] = "content";
constexpr char KEY_FINISH_REASON[] = "finish_reason";
constexpr char KEY_ERROR[] = "error";
constexpr char KEY_MESSAGE[] = "message";

bool keyIs(const char* key, const char* literal) { return strcmp(key, literal) == 0; }

}  // namespace

AiChatParser::AiChatParser(char* contentBuf, const size_t contentCap)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd, sOnStringChunk}),
      contentBuf(contentBuf),
      contentCap(contentCap) {
  reset();
}

void AiChatParser::reset() {
  parser.reset();
  contentLen = 0;
  contentTruncated = false;
  if (contentBuf && contentCap > 0) contentBuf[0] = '\0';
  errorMsg[0] = '\0';
  errorLen = 0;
  errorSeen = false;
  finishReason[0] = '\0';
  lastKey[0] = '\0';
  depth = 0;
  choicesArrayDepth = -1;
  choiceObjectDepth = -1;
  deltaObjectDepth = -1;
  errorObjectDepth = -1;
  malformed = 0;
}

void AiChatParser::feedEvent(const char* json, const size_t len) {
  // Each event is a self-contained object: restart the JSON state machine and
  // the path tracking, but keep the accumulated content.
  parser.reset();
  lastKey[0] = '\0';
  depth = 0;
  choicesArrayDepth = -1;
  choiceObjectDepth = -1;
  deltaObjectDepth = -1;
  errorObjectDepth = -1;

  parser.feed(json, len);

  // A streaming parser reports only genuinely invalid syntax; an event cut short
  // mid-object is, to it, simply not finished yet. Since each event is supposed
  // to be one complete object, every container it opened must be closed again by
  // the end of it — an unbalanced depth is the truncation signal.
  if (parser.hasError() || depth != 0) ++malformed;
}

void AiChatParser::appendContent(const char* data, const size_t len) {
  if (!contentBuf || contentCap == 0) return;
  if (contentTruncated) return;

  const size_t room = contentCap - 1 - contentLen;  // reserve the NUL
  if (len > room) {
    if (room > 0) {
      memcpy(contentBuf + contentLen, data, room);
      // Never leave a partial UTF-8 sequence at the cut: a half CJK character
      // would render as a replacement glyph and corrupt anything saved to disk.
      const int safe = utf8SafeTruncateBuffer(contentBuf, static_cast<int>(contentLen + room));
      contentLen = safe > 0 ? static_cast<size_t>(safe) : contentLen;
    }
    contentTruncated = true;
    contentBuf[contentLen] = '\0';
    return;
  }

  memcpy(contentBuf + contentLen, data, len);
  contentLen += len;
  contentBuf[contentLen] = '\0';
}

void AiChatParser::appendError(const char* data, const size_t len) {
  const size_t room = ERROR_BUF_SIZE - 1 - errorLen;
  const size_t n = len < room ? len : room;
  memcpy(errorMsg + errorLen, data, n);
  errorLen += n;
  errorMsg[errorLen] = '\0';
  errorSeen = true;
}

AiChatParser::Slot AiChatParser::currentSlot() const {
  if (deltaObjectDepth >= 0 && depth == deltaObjectDepth && keyIs(lastKey, KEY_CONTENT)) return Slot::Content;
  if (choiceObjectDepth >= 0 && depth == choiceObjectDepth && keyIs(lastKey, KEY_FINISH_REASON)) {
    return Slot::FinishReason;
  }
  if (errorObjectDepth >= 0 && depth == errorObjectDepth && keyIs(lastKey, KEY_MESSAGE)) return Slot::ErrorMessage;
  return Slot::None;
}

void AiChatParser::onValueComplete() { lastKey[0] = '\0'; }

void AiChatParser::handleKey(const char* key, const size_t len) {
  const size_t n = len < KEY_BUF_SIZE - 1 ? len : KEY_BUF_SIZE - 1;
  memcpy(lastKey, key, n);
  lastKey[n] = '\0';
}

void AiChatParser::handleString(const char* value, const size_t len, const bool final) {
  switch (currentSlot()) {
    case Slot::Content:
      appendContent(value, len);
      break;
    case Slot::FinishReason: {
      const size_t n = len < FINISH_REASON_BUF_SIZE - 1 ? len : FINISH_REASON_BUF_SIZE - 1;
      memcpy(finishReason, value, n);
      finishReason[n] = '\0';
      break;
    }
    case Slot::ErrorMessage:
      appendError(value, len);
      break;
    case Slot::None:
      break;
  }
  // A chunked string keeps its key live until the final chunk arrives.
  if (final) onValueComplete();
}

void AiChatParser::handleObjectStart() {
  if (depth == 1 && keyIs(lastKey, KEY_ERROR)) {
    errorObjectDepth = depth + 1;
  } else if (choicesArrayDepth >= 0 && depth == choicesArrayDepth) {
    choiceObjectDepth = depth + 1;
  } else if (choiceObjectDepth >= 0 && depth == choiceObjectDepth && keyIs(lastKey, KEY_DELTA)) {
    deltaObjectDepth = depth + 1;
  }
  ++depth;
  onValueComplete();
}

void AiChatParser::handleObjectEnd() {
  --depth;
  if (deltaObjectDepth > depth) deltaObjectDepth = -1;
  if (choiceObjectDepth > depth) choiceObjectDepth = -1;
  if (errorObjectDepth > depth) errorObjectDepth = -1;
  onValueComplete();
}

void AiChatParser::handleArrayStart() {
  if (depth == 1 && keyIs(lastKey, KEY_CHOICES)) choicesArrayDepth = depth + 1;
  ++depth;
  onValueComplete();
}

void AiChatParser::handleArrayEnd() {
  --depth;
  if (choicesArrayDepth > depth) choicesArrayDepth = -1;
  if (choiceObjectDepth > depth) choiceObjectDepth = -1;
  onValueComplete();
}

void AiChatParser::sOnKey(void* ctx, const char* key, const size_t len) {
  static_cast<AiChatParser*>(ctx)->handleKey(key, len);
}
void AiChatParser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<AiChatParser*>(ctx)->handleString(value, len, true);
}
void AiChatParser::sOnStringChunk(void* ctx, const char* value, const size_t len, const bool final) {
  static_cast<AiChatParser*>(ctx)->handleString(value, len, final);
}
void AiChatParser::sOnNumber(void* ctx, const char*, size_t) { static_cast<AiChatParser*>(ctx)->onValueComplete(); }
void AiChatParser::sOnBool(void* ctx, bool) { static_cast<AiChatParser*>(ctx)->onValueComplete(); }
void AiChatParser::sOnNull(void* ctx) { static_cast<AiChatParser*>(ctx)->onValueComplete(); }
void AiChatParser::sOnObjectStart(void* ctx) { static_cast<AiChatParser*>(ctx)->handleObjectStart(); }
void AiChatParser::sOnObjectEnd(void* ctx) { static_cast<AiChatParser*>(ctx)->handleObjectEnd(); }
void AiChatParser::sOnArrayStart(void* ctx) { static_cast<AiChatParser*>(ctx)->handleArrayStart(); }
void AiChatParser::sOnArrayEnd(void* ctx) { static_cast<AiChatParser*>(ctx)->handleArrayEnd(); }
