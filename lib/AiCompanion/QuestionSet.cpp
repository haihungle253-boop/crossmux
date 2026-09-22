#include "QuestionSet.h"

#include <cstring>

#include "Utf8.h"

namespace {

constexpr unsigned char BOM[] = {0xEF, 0xBB, 0xBF};

bool isBlank(const char* s, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (s[i] != ' ' && s[i] != '\t') return false;
  }
  return true;
}

}  // namespace

void QuestionSet::clear() {
  questionCount = 0;
  ignored = 0;
  shortened = 0;
}

const char* QuestionSet::at(const size_t index) const { return index < questionCount ? questions[index] : ""; }

void QuestionSet::addLine(const char* line, size_t len) {
  // Trim both ends; trailing spaces are invisible in an editor but would be
  // sent verbatim.
  while (len > 0 && (*line == ' ' || *line == '\t')) {
    ++line;
    --len;
  }
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) --len;

  if (len == 0 || isBlank(line, len)) return;
  if (line[0] == '#') return;  // comment

  if (questionCount >= MAX_QUESTIONS) {
    ++ignored;
    return;
  }

  size_t n = len;
  if (n > MAX_QUESTION_BYTES) {
    const int safe = utf8SafeTruncateBuffer(line, static_cast<int>(MAX_QUESTION_BYTES));
    n = safe > 0 ? static_cast<size_t>(safe) : 0;
    if (n == 0) {
      ++ignored;
      return;
    }
    ++shortened;
  }

  memcpy(questions[questionCount], line, n);
  questions[questionCount][n] = '\0';
  ++questionCount;
}

void QuestionSet::load(const char* data, size_t len) {
  clear();
  if (!data || len == 0) return;

  if (len >= sizeof(BOM) && memcmp(data, BOM, sizeof(BOM)) == 0) {
    data += sizeof(BOM);
    len -= sizeof(BOM);
  }

  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    const bool atEnd = i == len;
    if (!atEnd && data[i] != '\n') continue;

    size_t lineLen = i - start;
    if (lineLen > 0 && data[start + lineLen - 1] == '\r') --lineLen;  // CRLF
    addLine(data + start, lineLen);
    start = i + 1;
  }
}
