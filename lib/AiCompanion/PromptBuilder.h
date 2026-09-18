#pragma once

#include <cstddef>

// Assembles the JSON request body for an OpenAI-compatible streaming chat
// completion, from the pieces the companion works with: a persona, the recent
// conversation, the reader's position, an excerpt, and the question.
//
// Two properties this type is responsible for, both of them the kind of defect
// that is miserable to diagnose on a device:
//
//   1. Every string is JSON-escaped. Book text routinely contains quotes,
//      backslashes and newlines, any of which produces an unparseable body.
//   2. Every per-section cap truncates on a UTF-8 character boundary. A cap
//      landing mid-character would cut a CJK glyph in half and corrupt the
//      request.
//
// The progress guard is composed automatically into the system message from the
// supplied position, rather than being left to the persona file. It is what
// makes "we are reading this together" true, so it must not be something a
// reader can accidentally delete while editing their persona.
//
// Writes into a caller-owned buffer and never allocates.
class PromptBuilder {
 public:
  struct Limits {
    size_t personaBytes = 1024;
    size_t historyBytes = 4096;
    size_t excerptBytes = 3072;
  };

  struct Position {
    const char* bookTitle = nullptr;
    const char* author = nullptr;
    const char* chapterTitle = nullptr;
    int percent = -1;  // negative omits it
  };

  // One completed round of conversation. Oldest first; the builder drops from
  // the oldest end when the history cap binds, so the most recent exchanges —
  // the ones a follow-up actually depends on — always survive.
  struct Exchange {
    const char* question = nullptr;
    const char* reply = nullptr;
  };

  PromptBuilder(char* out, size_t capacity, const Limits& limits);
  // A brace-initialised default argument for Limits is not portable here: the
  // enclosing class is still incomplete where the default would be parsed.
  PromptBuilder(char* out, const size_t capacity) : PromptBuilder(out, capacity, Limits{}) {}

  PromptBuilder(const PromptBuilder&) = delete;
  PromptBuilder& operator=(const PromptBuilder&) = delete;

  void setModel(const char* model) { this->model = model; }
  void setPersona(const char* persona) { this->persona = persona; }
  void setPosition(const Position& position) { this->position = position; }
  void setExcerpt(const char* excerpt) { this->excerpt = excerpt; }
  void setHistory(const Exchange* history, const size_t count) {
    this->history = history;
    this->historyCount = count;
  }
  void setQuestion(const char* question) { this->question = question; }
  // 0 omits the field and lets the persona govern length.
  void setMaxTokens(const int maxTokens) { this->maxTokens = maxTokens; }

  // Returns false if the assembled body does not fit the output buffer, in
  // which case the buffer holds an empty string rather than a partial request.
  //
  // The per-section caps do not add up to a guarantee: persona, history and
  // excerpt can each be inside its own cap and still overflow the buffer
  // together. So a build that does not fit drops the oldest exchange and tries
  // again, as many times as it takes. Losing the far end of the conversation is
  // a cost the reader may not even notice; failing the request outright means
  // the companion stops answering at all, and -- since history only grows --
  // stops answering permanently.
  bool build();

  const char* body() const { return out; }
  size_t length() const { return len; }

  // How many of the oldest exchanges the history cap dropped in the last build.
  size_t droppedExchanges() const { return dropped; }

 private:
  bool buildInternal();
  bool put(char c);
  bool putRaw(const char* s);
  bool putEscaped(const char* s, size_t maxSourceBytes);
  bool putInt(int value);
  bool putMessage(const char* role, bool& first);
  bool putSystemMessage();
  bool putUserMessage();
  size_t firstKeptExchange() const;

  char* const out;
  const size_t capacity;
  const Limits limits;

  const char* model = nullptr;
  const char* persona = nullptr;
  const char* excerpt = nullptr;
  const char* question = nullptr;
  Position position{};
  const Exchange* history = nullptr;
  size_t historyCount = 0;
  int maxTokens = 0;

  size_t len = 0;
  size_t dropped = 0;
  // Raised by build() on each retry: the minimum number of oldest exchanges to
  // leave out, over and above what the history cap already drops.
  size_t forcedDrop = 0;
  bool overflow = false;
};
