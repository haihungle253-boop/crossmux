#pragma once

#include <cstddef>
#include <cstdint>

#include "AiChatParser.h"
#include "SseDecoder.h"

// Owns one streaming exchange: raw response bytes in, assembled reply out.
//
// The transport is deliberately not a member. onData() has the same shape as
// HttpDownloader::DataCallback -- bool(const uint8_t*, size_t), false to abort --
// so on device it is handed straight to the existing HTTP client, while a host
// test drives it from a socket or a canned buffer. Nothing in this class knows
// whether the bytes came from TLS, a plain socket, or a string literal, which is
// what makes the whole pipeline testable without a device.
//
// Returning false once the provider's [DONE] sentinel arrives lets the transfer
// be torn down immediately instead of waiting for the server to close, and is
// the same mechanism that implements Back-to-cancel.
class AiChatClient {
 public:
  // replyBuf is borrowed and must outlive the client. On S3 targets it belongs
  // in PSRAM; the reply is the largest buffer in an exchange.
  AiChatClient(char* replyBuf, size_t replyCap);

  AiChatClient(const AiChatClient&) = delete;
  AiChatClient& operator=(const AiChatClient&) = delete;

  // Clears all state for a new exchange.
  void begin();

  // Feeds response bytes. Returns false when the transfer should stop: either
  // the stream is complete, or the caller asked to cancel.
  bool onData(const uint8_t* data, size_t len);

  // Call when the transport stops delivering, so a final event that arrived
  // without its terminating blank line is still processed.
  void end();

  // Asks the exchange to stop at the next chunk. Safe to set from the UI loop
  // between onData() calls.
  void cancel() { cancelled = true; }
  bool wasCancelled() const { return cancelled; }

  bool complete() const { return done; }
  const char* reply() const { return parser.content(); }
  size_t replyLength() const { return parser.contentLength(); }
  bool replyTruncated() const { return parser.truncated(); }

  bool hasError() const { return parser.hasError(); }
  const char* errorMessage() const { return parser.errorMessage(); }

  // "stop" is a complete reply; "length" means the provider hit its own token
  // limit and the reply is cut short through no fault of ours.
  bool hasFinishReason() const { return parser.hasFinishReason(); }
  const char* finishReason() const { return parser.getFinishReason(); }

  uint32_t malformedEvents() const { return parser.malformedEvents(); }
  uint32_t droppedEvents() const { return decoder.droppedEvents(); }

 private:
  static void onEventThunk(void* ctx, const char* data, size_t len);
  static void onDoneThunk(void* ctx);

  AiChatParser parser;
  SseDecoder decoder;
  bool done = false;
  bool cancelled = false;
};
