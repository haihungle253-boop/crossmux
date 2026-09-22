#include "AiChatClient.h"

AiChatClient::AiChatClient(char* replyBuf, const size_t replyCap)
    : parser(replyBuf, replyCap), decoder(this, onEventThunk, onDoneThunk) {}

void AiChatClient::begin() {
  parser.reset();
  decoder.reset();
  done = false;
  cancelled = false;
}

bool AiChatClient::onData(const uint8_t* data, const size_t len) {
  if (cancelled) return false;
  decoder.feed(reinterpret_cast<const char*>(data), len);
  // Stop as soon as the stream is complete rather than waiting for the server to
  // close the connection, and stop immediately if the user cancelled while this
  // chunk was being processed.
  return !done && !cancelled;
}

void AiChatClient::end() { decoder.finish(); }

void AiChatClient::onEventThunk(void* ctx, const char* data, const size_t len) {
  static_cast<AiChatClient*>(ctx)->parser.feedEvent(data, len);
}

void AiChatClient::onDoneThunk(void* ctx) { static_cast<AiChatClient*>(ctx)->done = true; }
