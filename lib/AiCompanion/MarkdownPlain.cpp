#include "MarkdownPlain.h"

#include <cstddef>

namespace {

// A delimiter run only opens emphasis when something can close it on the same
// line, and the span between them is not empty. Anything else -- a lone
// asterisk, a multiplication sign, an underscore inside an identifier -- is
// left alone, because removing it would damage the text rather than tidy it.
size_t findCloser(const std::string& line, const size_t from, const char delim, const size_t width) {
  for (size_t i = from; i + width <= line.size(); ++i) {
    bool match = true;
    for (size_t w = 0; w < width; ++w) {
      if (line[i + w] != delim) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    // A run longer than we are looking for is not this delimiter.
    if (i + width < line.size() && line[i + width] == delim) continue;
    return i;
  }
  return std::string::npos;
}

// ASCII letters, digits and the underscore itself. Bytes above 0x7f -- every
// byte of a CJK character -- are deliberately not word bytes, so `_强调_` still
// works while `snake_case_name` is left alone.
bool isWordByte(const unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isSpace(const char c) { return c == ' ' || c == '\t'; }

// The two rules that stop ordinary prose being eaten: a delimiter run may not
// sit inside a word (underscores only, as Markdown has it), and the span it
// opens may not begin or end with whitespace.
bool spanIsEmphasis(const std::string& line, const size_t open, const size_t close,
                    const char delim, const size_t width) {
  const size_t innerStart = open + width;
  if (close <= innerStart) return false;
  if (isSpace(line[innerStart]) || isSpace(line[close - 1])) return false;
  if (delim == '_') {
    if (open > 0 && isWordByte(static_cast<unsigned char>(line[open - 1]))) return false;
    const size_t after = close + width;
    if (after < line.size() && isWordByte(static_cast<unsigned char>(line[after]))) return false;
  }
  return true;
}

bool isRuleLine(const std::string& line) {
  char seen = '\0';
  size_t count = 0;
  for (const char c : line) {
    if (c == ' ' || c == '\t') continue;
    if (c != '-' && c != '*' && c != '_') return false;
    if (seen != '\0' && c != seen) return false;
    seen = c;
    ++count;
  }
  return count >= 3;
}

// `[text](url)` keeps the text. A bare `[text]` with no parenthesised target is
// left alone: brackets carry meaning of their own in ordinary prose.
bool takeLink(const std::string& line, size_t& i, std::string& out) {
  const size_t close = line.find(']', i + 1);
  if (close == std::string::npos) return false;
  if (close + 1 >= line.size() || line[close + 1] != '(') return false;
  const size_t target = line.find(')', close + 2);
  if (target == std::string::npos) return false;
  out.append(line, i + 1, close - i - 1);
  i = target + 1;
  return true;
}

std::string flattenInline(const std::string& line) {
  std::string out;
  out.reserve(line.size());

  for (size_t i = 0; i < line.size();) {
    const char c = line[i];

    // Escaped delimiter: emit the character itself, drop the backslash.
    if (c == '\\' && i + 1 < line.size()) {
      const char next = line[i + 1];
      if (next == '*' || next == '_' || next == '`' || next == '[' || next == '#') {
        out.push_back(next);
        i += 2;
        continue;
      }
    }

    if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
      const size_t close = findCloser(line, i + 2, c, 2);
      if (close != std::string::npos && spanIsEmphasis(line, i, close, c, 2)) {
        out.append(flattenInline(line.substr(i + 2, close - i - 2)));
        i = close + 2;
        continue;
      }
    }

    if (c == '*' || c == '_') {
      const size_t close = findCloser(line, i + 1, c, 1);
      if (close != std::string::npos && spanIsEmphasis(line, i, close, c, 1)) {
        out.append(flattenInline(line.substr(i + 1, close - i - 1)));
        i = close + 1;
        continue;
      }
    }

    if (c == '`') {
      const size_t close = line.find('`', i + 1);
      if (close != std::string::npos && close > i + 1) {
        out.append(line, i + 1, close - i - 1);
        i = close + 1;
        continue;
      }
    }

    if (c == '[' && takeLink(line, i, out)) continue;

    out.push_back(c);
    ++i;
  }
  return out;
}

std::string flattenBlock(const std::string& raw) {
  std::string line = raw;
  size_t lead = 0;
  while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;

  // Heading: one to six hashes followed by a space.
  size_t hashes = lead;
  while (hashes < line.size() && line[hashes] == '#') ++hashes;
  if (hashes > lead && hashes - lead <= 6 && hashes < line.size() && line[hashes] == ' ') {
    return flattenInline(line.substr(hashes + 1));
  }

  // Blockquote.
  if (lead < line.size() && line[lead] == '>') {
    size_t rest = lead + 1;
    if (rest < line.size() && line[rest] == ' ') ++rest;
    return flattenInline(line.substr(rest));
  }

  // Unordered list: keep the indent, swap the marker for a real bullet so the
  // structure survives without an asterisk pretending to be one.
  if (lead + 1 < line.size() && (line[lead] == '-' || line[lead] == '*' || line[lead] == '+') &&
      line[lead + 1] == ' ') {
    return line.substr(0, lead) + "· " + flattenInline(line.substr(lead + 2));
  }

  return flattenInline(line);
}

}  // namespace

namespace MarkdownPlain {

std::string flatten(const std::string& markdown) {
  std::string out;
  out.reserve(markdown.size());

  size_t start = 0;
  bool first = true;
  while (start <= markdown.size()) {
    size_t end = markdown.find('\n', start);
    const bool last = end == std::string::npos;
    if (last) end = markdown.size();

    std::string line = markdown.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Code fences and horizontal rules are structure with nothing to say once
    // the styling is gone, so the line goes rather than becoming a row of
    // leftover punctuation.
    const bool fence = line.compare(0, 3, "```") == 0 || line.compare(0, 3, "~~~") == 0;
    if (!fence && !isRuleLine(line)) {
      if (!first) out.push_back('\n');
      out.append(flattenBlock(line));
      first = false;
    }

    if (last) break;
    start = end + 1;
  }
  return out;
}

}  // namespace MarkdownPlain
