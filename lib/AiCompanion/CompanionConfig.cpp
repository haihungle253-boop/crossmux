#include "CompanionConfig.h"

#include <cstdlib>
#include <cstring>

#include "Utf8.h"

namespace {

constexpr unsigned char BOM[] = {0xEF, 0xBB, 0xBF};

void trim(const char*& s, size_t& len) {
  while (len > 0 && (*s == ' ' || *s == '\t')) {
    ++s;
    --len;
  }
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
}

bool nameIs(const char* name, const size_t len, const char* literal) {
  return strlen(literal) == len && strncmp(name, literal, len) == 0;
}

// Copies with a cap on a UTF-8 boundary. Values here are normally ASCII, but a
// mistyped file should still never produce a broken sequence.
void copyCapped(char* dst, const size_t cap, const char* src, size_t len) {
  if (len > cap) {
    const int safe = utf8SafeTruncateBuffer(src, static_cast<int>(cap));
    len = safe > 0 ? static_cast<size_t>(safe) : 0;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

}  // namespace

void CompanionConfig::clear() {
  endpointBuf[0] = '\0';
  modelBuf[0] = '\0';
  keyBuf[0] = '\0';
  maxTokensValue = 0;
  insecure = false;
  unknown = 0;
}

void CompanionConfig::assign(const char* name, const size_t nameLen, const char* value, const size_t valueLen) {
  if (nameIs(name, nameLen, "endpoint")) {
    copyCapped(endpointBuf, ENDPOINT_BYTES, value, valueLen);
  } else if (nameIs(name, nameLen, "model")) {
    copyCapped(modelBuf, MODEL_BYTES, value, valueLen);
  } else if (nameIs(name, nameLen, "key")) {
    copyCapped(keyBuf, KEY_BYTES, value, valueLen);
  } else if (nameIs(name, nameLen, "max_tokens")) {
    char digits[12];
    const size_t n = valueLen < sizeof(digits) - 1 ? valueLen : sizeof(digits) - 1;
    memcpy(digits, value, n);
    digits[n] = '\0';
    const long parsed = strtol(digits, nullptr, 10);
    maxTokensValue = parsed > 0 && parsed < 100000 ? static_cast<int>(parsed) : 0;
  } else {
    ++unknown;
  }
}

bool CompanionConfig::load(const char* data, size_t len) {
  clear();
  if (!data || len == 0) return false;

  if (len >= sizeof(BOM) && memcmp(data, BOM, sizeof(BOM)) == 0) {
    data += sizeof(BOM);
    len -= sizeof(BOM);
  }

  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i != len && data[i] != '\n') continue;

    const char* line = data + start;
    size_t lineLen = i - start;
    if (lineLen > 0 && line[lineLen - 1] == '\r') --lineLen;
    start = i + 1;

    trim(line, lineLen);
    if (lineLen == 0 || line[0] == '#') continue;

    const void* sep = memchr(line, '=', lineLen);
    if (!sep) continue;  // not a key=value line; ignore rather than fail

    const size_t nameLen = static_cast<size_t>(static_cast<const char*>(sep) - line);
    const char* name = line;
    size_t trimmedNameLen = nameLen;
    trim(name, trimmedNameLen);

    const char* value = line + nameLen + 1;
    size_t valueLen = lineLen - nameLen - 1;
    trim(value, valueLen);

    if (trimmedNameLen == 0) continue;
    assign(name, trimmedNameLen, value, valueLen);
  }

  insecure = strncmp(endpointBuf, "http://", 7) == 0;
  const bool https = strncmp(endpointBuf, "https://", 8) == 0;
  return https || insecure;
}
