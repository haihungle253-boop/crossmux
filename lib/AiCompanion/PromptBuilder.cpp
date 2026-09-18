#include "PromptBuilder.h"

#include <cstdio>
#include <cstring>

#include "Utf8.h"

namespace {

constexpr char DEFAULT_MODEL[] = "gpt-4o-mini";

// Composed into the system message after the reader's own persona. Kept short
// and in English because instruction-following is most reliable that way; the
// last line hands the reply language back to the reader rather than assuming it.
constexpr char GUARD_HEADER[] =
    "\\n\\n---\\nYou and the reader are reading the same book together and are at the same point in it.";
constexpr char GUARD_RULES[] =
    "\\nYou have been given only text the reader has already read. Never reveal, hint at, or speculate about "
    "anything beyond this point in the book, even if asked directly.\\n"
    "Reply in the same language the reader writes in.";

constexpr char DEFAULT_PERSONA[] =
    "You are the reader's reading companion. Talk with them about the book like a friend would.";

size_t sourceCap(const char* s, const size_t maxBytes) {
  if (!s) return 0;
  const size_t actual = strlen(s);
  if (actual <= maxBytes) return actual;
  const int safe = utf8SafeTruncateBuffer(s, static_cast<int>(maxBytes));
  return safe > 0 ? static_cast<size_t>(safe) : 0;
}

size_t exchangeBytes(const PromptBuilder::Exchange& e) {
  return (e.question ? strlen(e.question) : 0) + (e.reply ? strlen(e.reply) : 0);
}

}  // namespace

PromptBuilder::PromptBuilder(char* out, const size_t capacity, const Limits& limits)
    : out(out), capacity(capacity), limits(limits) {}

bool PromptBuilder::put(const char c) {
  if (overflow) return false;
  if (len + 1 >= capacity) {
    overflow = true;
    return false;
  }
  out[len++] = c;
  return true;
}

bool PromptBuilder::putRaw(const char* s) {
  for (const char* p = s; *p; ++p) {
    if (!put(*p)) return false;
  }
  return true;
}

bool PromptBuilder::putInt(const int value) {
  char buf[16];
  const int n = snprintf(buf, sizeof(buf), "%d", value);
  if (n < 0) return false;
  return putRaw(buf);
}

// Appends a JSON-escaped string, reading at most maxSourceBytes of source (cut
// on a UTF-8 boundary). UTF-8 continuation bytes are >= 0x80 and pass through
// untouched; only ASCII control characters and the two structural characters
// need escaping.
bool PromptBuilder::putEscaped(const char* s, const size_t maxSourceBytes) {
  if (!s) return true;
  const size_t n = sourceCap(s, maxSourceBytes);
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    switch (c) {
      case '"':
        if (!putRaw("\\\"")) return false;
        break;
      case '\\':
        if (!putRaw("\\\\")) return false;
        break;
      case '\n':
        if (!putRaw("\\n")) return false;
        break;
      case '\r':
        if (!putRaw("\\r")) return false;
        break;
      case '\t':
        if (!putRaw("\\t")) return false;
        break;
      case '\b':
        if (!putRaw("\\b")) return false;
        break;
      case '\f':
        if (!putRaw("\\f")) return false;
        break;
      default: {
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          if (!putRaw(esc)) return false;
        } else if (!put(c)) {
          return false;
        }
        break;
      }
    }
  }
  return true;
}

bool PromptBuilder::putMessage(const char* role, bool& first) {
  if (!first && !putRaw(",")) return false;
  first = false;
  if (!putRaw("{\"role\":\"")) return false;
  if (!putRaw(role)) return false;
  return putRaw("\",\"content\":\"");
}

bool PromptBuilder::putSystemMessage() {
  bool first = true;
  if (!putMessage("system", first)) return false;
  if (!putEscaped(persona && persona[0] ? persona : DEFAULT_PERSONA, limits.personaBytes)) return false;

  if (!putRaw(GUARD_HEADER)) return false;

  if (position.bookTitle && position.bookTitle[0]) {
    if (!putRaw("\\nBook: ")) return false;
    if (!putEscaped(position.bookTitle, 256)) return false;
  }
  if (position.author && position.author[0]) {
    if (!putRaw("\\nAuthor: ")) return false;
    if (!putEscaped(position.author, 128)) return false;
  }
  if (position.chapterTitle && position.chapterTitle[0]) {
    if (!putRaw("\\nCurrent chapter: ")) return false;
    if (!putEscaped(position.chapterTitle, 256)) return false;
  }
  if (position.percent >= 0) {
    if (!putRaw("\\nProgress: ")) return false;
    if (!putInt(position.percent)) return false;
    if (!putRaw("% of the book")) return false;
  }

  if (!putRaw(GUARD_RULES)) return false;
  return putRaw("\"}");
}

// Walks back from the newest exchange while the cap allows, so the kept window
// is always the most recent one.
size_t PromptBuilder::firstKeptExchange() const {
  if (!history || historyCount == 0) return 0;
  size_t used = 0;
  size_t i = historyCount;
  while (i > 0) {
    const size_t cost = exchangeBytes(history[i - 1]);
    if (used + cost > limits.historyBytes) break;
    used += cost;
    --i;
  }
  // Always keep the newest exchange, even if it alone exceeds the cap; its
  // fields are then truncated individually below.
  if (i == historyCount && historyCount > 0) i = historyCount - 1;
  return i < forcedDrop ? forcedDrop : i;
}

bool PromptBuilder::putUserMessage() {
  bool first = false;  // always preceded by the system message
  if (!putMessage("user", first)) return false;

  if (position.chapterTitle && position.chapterTitle[0]) {
    if (!putRaw("[Chapter: ")) return false;
    if (!putEscaped(position.chapterTitle, 256)) return false;
    if (!putRaw("]\\n")) return false;
  }
  if (excerpt && excerpt[0]) {
    if (!putRaw("Passage the reader has just read:\\n\\\"\\\"\\\"\\n")) return false;
    if (!putEscaped(excerpt, limits.excerptBytes)) return false;
    if (!putRaw("\\n\\\"\\\"\\\"\\n\\n")) return false;
  }
  if (!putEscaped(question, 1024)) return false;
  return putRaw("\"}");
}

bool PromptBuilder::build() {
  if (!out || capacity == 0) return false;

  // Each pass gives up one more of the oldest exchanges. The last pass carries
  // no history at all, which is the request the companion must always be able
  // to make: a reader who has just opened a book is in exactly that state.
  for (forcedDrop = 0; forcedDrop <= historyCount; ++forcedDrop) {
    len = 0;
    dropped = 0;
    overflow = false;
    out[0] = '\0';

    if (buildInternal()) {
      out[len] = '\0';
      return true;
    }
  }

  // Fail closed. Every step above bails out the moment the buffer is exhausted,
  // so what is in the buffer at that point is a truncated request. Returning it
  // would send a body the provider rejects, and the failure would look like the
  // provider's fault rather than ours.
  forcedDrop = 0;
  len = 0;
  dropped = 0;
  out[0] = '\0';
  return false;
}

bool PromptBuilder::buildInternal() {
  if (!putRaw("{\"model\":\"")) return false;
  if (!putEscaped(model && model[0] ? model : DEFAULT_MODEL, 128)) return false;
  if (!putRaw("\",\"stream\":true")) return false;
  if (maxTokens > 0) {
    if (!putRaw(",\"max_tokens\":")) return false;
    if (!putInt(maxTokens)) return false;
  }
  if (!putRaw(",\"messages\":[")) return false;

  if (!putSystemMessage()) return false;

  const size_t firstKept = firstKeptExchange();
  dropped = firstKept;
  for (size_t i = firstKept; i < historyCount; ++i) {
    const Exchange& e = history[i];
    bool first = false;
    if (e.question && e.question[0]) {
      if (!putMessage("user", first)) return false;
      if (!putEscaped(e.question, limits.historyBytes)) return false;
      if (!putRaw("\"}")) return false;
    }
    if (e.reply && e.reply[0]) {
      first = false;
      if (!putMessage("assistant", first)) return false;
      if (!putEscaped(e.reply, limits.historyBytes)) return false;
      if (!putRaw("\"}")) return false;
    }
  }

  if (!putUserMessage()) return false;
  if (!putRaw("]}")) return false;

  return !overflow;
}
