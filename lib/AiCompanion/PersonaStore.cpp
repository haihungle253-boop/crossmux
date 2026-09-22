#include "PersonaStore.h"

#include <cstring>

#include "Utf8.h"

namespace {

constexpr char BUILT_IN[] =
    "你是我的读书搭子，我们正在一起读同一本书，进度完全同步。\n"
    "说话像老友不像老师，有自己的判断，可以不同意我。\n"
    "不要复述剧情，说你注意到的细节和你的想法。\n"
    "这章要是平淡就直说，不用硬找亮点。\n"
    "回答控制在 200 字以内。";

constexpr unsigned char BOM[] = {0xEF, 0xBB, 0xBF};

bool isBlank(const char* s, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  }
  return true;
}

}  // namespace

const char* PersonaStore::builtInPersona() { return BUILT_IN; }

PersonaStore::PersonaStore() { useFallback(); }

void PersonaStore::useFallback() {
  const size_t n = strlen(BUILT_IN);
  // The built-in persona is authored here, so it fits by construction; the
  // clamp is kept so editing it can never overrun the buffer.
  len = n < MAX_BYTES ? n : MAX_BYTES;
  memcpy(buffer, BUILT_IN, len);
  buffer[len] = '\0';
  fallback = true;
}

void PersonaStore::clear() {
  truncated = false;
  droppedControls = 0;
  useFallback();
}

bool PersonaStore::load(const char* data, size_t len_) {
  truncated = false;
  droppedControls = 0;

  if (!data || len_ == 0) {
    useFallback();
    return false;
  }

  // Notepad and several Windows editors write a UTF-8 BOM. Passing it through
  // would put three invisible bytes at the head of every system prompt.
  if (len_ >= sizeof(BOM) && memcmp(data, BOM, sizeof(BOM)) == 0) {
    data += sizeof(BOM);
    len_ -= sizeof(BOM);
  }

  if (isBlank(data, len_)) {
    useFallback();
    return false;
  }

  size_t out = 0;
  for (size_t i = 0; i < len_ && out < MAX_BYTES; ++i) {
    const char c = data[i];
    if (c == '\r') continue;  // normalise CRLF; a lone CR becomes nothing
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 && c != '\n' && c != '\t') {
      ++droppedControls;
      continue;
    }
    buffer[out++] = c;
  }

  if (out >= MAX_BYTES) {
    // Cut on a character boundary so a CJK character is never halved.
    const int safe = utf8SafeTruncateBuffer(buffer, static_cast<int>(out));
    out = safe > 0 ? static_cast<size_t>(safe) : 0;
    truncated = true;
  }

  if (out == 0 || isBlank(buffer, out)) {
    useFallback();
    return false;
  }

  // Trailing blank lines add nothing to the prompt but do count against the cap.
  while (out > 0 && (buffer[out - 1] == '\n' || buffer[out - 1] == ' ' || buffer[out - 1] == '\t')) --out;

  len = out;
  buffer[len] = '\0';
  fallback = false;
  return true;
}
