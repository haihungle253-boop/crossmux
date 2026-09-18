#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// Extracts assistant text from OpenAI-compatible streaming chat responses.
//
// Each SSE event carries one complete JSON object, so the caller hands whole
// events to feedEvent(). Content found in choices[].delta.content is appended to
// a caller-owned buffer that persists across events, letting the reply
// accumulate while the buffer itself lives wherever the caller wants it — PSRAM
// on S3 targets, internal heap elsewhere.
//
// Also recognised:
//   - choices[].finish_reason, so a caller can tell a complete reply from one
//     the provider cut short at its token limit;
//   - a top-level error object, because providers report an invalid key or an
//     exhausted quota as HTTP 200 followed by {"error":{"message":...}} on the
//     stream. Treating that as "no content" would surface as a silent empty
//     reply with no way to tell the user what went wrong.
//
// Content longer than the JSON parser's 512-byte token buffer arrives through
// onStringChunk and is handled; nothing assumes a delta fits in one callback.
class AiChatParser {
 public:
  static constexpr size_t KEY_BUF_SIZE = 32;
  static constexpr size_t ERROR_BUF_SIZE = 160;
  static constexpr size_t FINISH_REASON_BUF_SIZE = 24;

  // contentBuf is borrowed, not owned, and must outlive the parser.
  AiChatParser(char* contentBuf, size_t contentCap);

  AiChatParser(const AiChatParser&) = delete;
  AiChatParser& operator=(const AiChatParser&) = delete;

  // Clears accumulated content and all flags.
  void reset();

  // Feeds one complete SSE event payload (a single JSON object).
  void feedEvent(const char* json, size_t len);

  const char* content() const { return contentBuf; }
  size_t contentLength() const { return contentLen; }

  // True when the reply did not fit in the caller's buffer. The content held is
  // still valid and complete up to the cut, which falls on a UTF-8 boundary.
  bool truncated() const { return contentTruncated; }

  bool hasError() const { return errorSeen; }
  const char* errorMessage() const { return errorMsg; }

  bool hasFinishReason() const { return finishReason[0] != '\0'; }
  const char* getFinishReason() const { return finishReason; }

  // Count of events whose JSON did not parse. A stream that produces content is
  // not necessarily clean; the caller can log this without failing the reply.
  uint32_t malformedEvents() const { return malformed; }

 private:
  enum class Slot : uint8_t { None, Content, FinishReason, ErrorMessage };

  void appendContent(const char* data, size_t len);
  void appendError(const char* data, size_t len);
  Slot currentSlot() const;
  void onValueComplete();

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnStringChunk(void* ctx, const char* value, size_t len, bool final);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void handleKey(const char* key, size_t len);
  void handleString(const char* value, size_t len, bool final);
  void handleObjectStart();
  void handleObjectEnd();
  void handleArrayStart();
  void handleArrayEnd();

  StreamingJsonParser parser;

  char* const contentBuf;
  const size_t contentCap;
  size_t contentLen = 0;
  bool contentTruncated = false;

  char errorMsg[ERROR_BUF_SIZE];
  size_t errorLen = 0;
  bool errorSeen = false;

  char finishReason[FINISH_REASON_BUF_SIZE];

  char lastKey[KEY_BUF_SIZE];

  // Container depths at which each interesting scope sits; -1 when not inside
  // one. Depth counts containers this parser has seen opened, so the top-level
  // object sits at depth 1.
  int depth = 0;
  int choicesArrayDepth = -1;
  int choiceObjectDepth = -1;
  int deltaObjectDepth = -1;
  int errorObjectDepth = -1;

  uint32_t malformed = 0;
};
