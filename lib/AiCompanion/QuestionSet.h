#pragma once

#include <cstddef>
#include <cstdint>

// The reader's preset questions, one per line, from /companion/questions.txt.
//
// On a four-button device these carry most of the conversation: picking a line
// is three keypresses where typing one is a minute of work. So the list is the
// reader's to edit, and parsing tolerates whatever a text editor produced —
// a BOM, CRLF, blank lines, and '#' comments.
//
// There is deliberately no built-in list here. The fallback questions are
// user-facing UI strings and must go through tr(), which belongs in the
// activity, not in a library that knows nothing about translation.
class QuestionSet {
 public:
  static constexpr size_t MAX_QUESTIONS = 12;
  static constexpr size_t MAX_QUESTION_BYTES = 160;

  // Parses the file. Over-long lines are truncated on a UTF-8 boundary rather
  // than dropped, so a reader who writes a long question still gets it, and
  // lines past MAX_QUESTIONS are ignored.
  void load(const char* data, size_t len);
  void clear();

  size_t count() const { return questionCount; }
  bool empty() const { return questionCount == 0; }
  // Returns an empty string for an out-of-range index rather than nullptr, so a
  // caller cannot accidentally hand a null to a string function.
  const char* at(size_t index) const;

  // Lines the caps dropped or shortened, for a one-line notice in the UI.
  size_t ignoredLines() const { return ignored; }
  size_t truncatedLines() const { return shortened; }

 private:
  void addLine(const char* line, size_t len);

  char questions[MAX_QUESTIONS][MAX_QUESTION_BYTES + 1]{};
  size_t questionCount = 0;
  size_t ignored = 0;
  size_t shortened = 0;
};
