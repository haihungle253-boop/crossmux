#pragma once

#include <cstddef>
#include <cstdint>

// Reassembles a Server-Sent Events byte stream into complete event payloads.
//
// Fed arbitrary chunks as they arrive from the network, it rejoins lines across
// chunk boundaries and reports one payload per event: a run of `data:` lines
// terminated by a blank line, joined with '\n' as the SSE specification
// requires. The terminal `data: [DONE]` sentinel is reported through onDone
// instead of onEvent, so a caller never feeds it to a JSON parser.
//
// Non-`data:` fields (`event:`, `id:`, `retry:`) and comment lines (starting
// with ':') are ignored — they carry nothing this client needs.
//
// Fixed buffers, no allocation. An event larger than EVENT_BUF_SIZE is dropped
// whole and counted by droppedEvents(); it is never truncated, because half a
// JSON object is worse than no JSON object. The decoder resynchronises on the
// next blank line, so one oversized event does not poison the rest of the
// stream.
//
// Callbacks follow the JsonCallbacks house style (raw function pointer plus a
// context pointer) to keep std::function and its heap allocation off the device.
class SseDecoder {
 public:
  // Payload is NUL-terminated; len excludes the terminator.
  using EventCallback = void (*)(void* ctx, const char* data, size_t len);
  using DoneCallback = void (*)(void* ctx);

  static constexpr size_t LINE_BUF_SIZE = 1024;
  static constexpr size_t EVENT_BUF_SIZE = 2048;

  SseDecoder(void* ctx, EventCallback onEvent, DoneCallback onDone);

  void reset();
  void feed(const char* data, size_t len);

  // Flush a trailing event that the stream ended without a blank line after.
  // Servers usually terminate cleanly; this covers the ones that do not.
  void finish();

  bool sawDone() const { return doneSeen; }
  uint32_t droppedEvents() const { return dropped; }

 private:
  void consumeLine();
  void appendToLine(char c);
  void emitEvent();

  void* ctx;
  EventCallback onEvent;
  DoneCallback onDone;

  char lineBuf[LINE_BUF_SIZE];
  size_t lineLen = 0;
  bool lineOverflow = false;

  char eventBuf[EVENT_BUF_SIZE];
  size_t eventLen = 0;
  bool eventOverflow = false;
  bool eventHasData = false;

  bool doneSeen = false;
  uint32_t dropped = 0;
};
