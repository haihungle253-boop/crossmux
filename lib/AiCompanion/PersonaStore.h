#pragma once

#include <cstddef>

// Holds the reader's persona, loaded from /companion/persona.txt.
//
// Takes the file's bytes rather than reading the file itself, so the validation
// rules — the part with the edge cases — are testable on the host while the
// device keeps its Storage access at the edge.
//
// A persona is hand-edited by someone who is not thinking about encodings, so
// load() is forgiving by design: a UTF-8 BOM from Notepad is stripped, CRLF is
// normalised, stray control characters are dropped rather than passed through to
// the provider, and an over-long file is cut on a UTF-8 boundary instead of
// rejected. A reader who mistypes something should get a working companion with
// a slightly odd personality, never a broken feature.
//
// text() is always usable: an absent, empty or all-whitespace file falls back to
// a built-in neutral persona, with usingFallback() set so the UI can say so once.
class PersonaStore {
 public:
  static constexpr size_t MAX_BYTES = 1024;

  PersonaStore();

  // Returns false only when the input was unusable and the fallback is in
  // effect; the store is always left in a valid state.
  bool load(const char* data, size_t len);

  // Drops any loaded persona and returns to the built-in one.
  void clear();

  const char* text() const { return buffer; }
  size_t length() const { return len; }

  bool usingFallback() const { return fallback; }
  // True when the file was longer than MAX_BYTES and the tail was dropped.
  bool wasTruncated() const { return truncated; }
  // Count of control characters removed; a non-zero value usually means the
  // file was saved in the wrong encoding.
  size_t droppedControlChars() const { return droppedControls; }

  static const char* builtInPersona();

 private:
  void useFallback();

  char buffer[MAX_BYTES + 1];
  size_t len = 0;
  bool fallback = true;
  bool truncated = false;
  size_t droppedControls = 0;
};
