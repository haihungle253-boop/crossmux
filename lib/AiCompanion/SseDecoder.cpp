#include "SseDecoder.h"

#include <cstring>

namespace {

constexpr char DATA_FIELD[] = "data:";
constexpr size_t DATA_FIELD_LEN = sizeof(DATA_FIELD) - 1;
constexpr char DONE_SENTINEL[] = "[DONE]";

}  // namespace

SseDecoder::SseDecoder(void* ctx, const EventCallback onEvent, const DoneCallback onDone)
    : ctx(ctx), onEvent(onEvent), onDone(onDone) {
  reset();
}

void SseDecoder::reset() {
  lineLen = 0;
  lineOverflow = false;
  eventLen = 0;
  eventOverflow = false;
  eventHasData = false;
  doneSeen = false;
  dropped = 0;
}

void SseDecoder::appendToLine(const char c) {
  if (lineLen + 1 >= LINE_BUF_SIZE) {
    // Mark and keep scanning: the line still has to be consumed to find its
    // terminator, we just refuse to store any more of it.
    lineOverflow = true;
    return;
  }
  lineBuf[lineLen++] = c;
}

void SseDecoder::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\n') {
      consumeLine();
    } else if (c != '\r') {
      // A lone '\r' is a line terminator per the spec, but every provider in
      // practice sends CRLF or LF. Dropping '\r' handles CRLF without needing
      // lookahead across a chunk boundary.
      appendToLine(c);
    }
  }
}

void SseDecoder::consumeLine() {
  const size_t len = lineLen;
  const bool overflowed = lineOverflow;
  lineLen = 0;
  lineOverflow = false;

  if (len == 0 && !overflowed) {
    emitEvent();  // blank line: end of event
    return;
  }

  // Terminate before inspecting the field name. On overflow only the leading
  // LINE_BUF_SIZE-1 bytes were kept, which is still more than enough to tell
  // which field this line is — the name is at the front.
  lineBuf[len] = '\0';

  if (len > 0 && lineBuf[0] == ':') return;  // comment, however long

  if (len < DATA_FIELD_LEN || memcmp(lineBuf, DATA_FIELD, DATA_FIELD_LEN) != 0) {
    return;  // event:, id:, retry:, or an unknown field — none of them concern us
  }

  // Mark the event as carrying data before testing overflow, so an event whose
  // only data line was oversized is still recognised as a dropped event rather
  // than silently vanishing from the drop count.
  eventHasData = true;

  if (overflowed) {
    // The line was longer than we can hold, so its content is unusable. Poison
    // the event rather than emitting a payload with a hole in the middle.
    eventOverflow = true;
    return;
  }

  const char* value = lineBuf + DATA_FIELD_LEN;
  size_t valueLen = len - DATA_FIELD_LEN;
  if (valueLen > 0 && value[0] == ' ') {  // the single optional space after the colon
    ++value;
    --valueLen;
  }

  if (eventLen + valueLen + 1 >= EVENT_BUF_SIZE) {
    eventOverflow = true;
    return;
  }
  if (eventLen > 0) eventBuf[eventLen++] = '\n';  // multi-line data joins with '\n'
  memcpy(eventBuf + eventLen, value, valueLen);
  eventLen += valueLen;
}

void SseDecoder::emitEvent() {
  const bool hadData = eventHasData;
  const bool overflowed = eventOverflow;
  const size_t len = eventLen;

  eventLen = 0;
  eventOverflow = false;
  eventHasData = false;

  if (!hadData) return;  // stray blank line between events

  if (overflowed) {
    ++dropped;
    return;
  }

  eventBuf[len] = '\0';

  if (len == sizeof(DONE_SENTINEL) - 1 && memcmp(eventBuf, DONE_SENTINEL, len) == 0) {
    doneSeen = true;
    if (onDone) onDone(ctx);
    return;
  }

  if (onEvent) onEvent(ctx, eventBuf, len);
}

void SseDecoder::finish() {
  if (lineLen > 0 || lineOverflow) consumeLine();
  emitEvent();
}
