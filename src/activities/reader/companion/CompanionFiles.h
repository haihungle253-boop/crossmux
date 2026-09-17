#pragma once

#include <string>

#include "ConversationStore.h"
#include "PersonaStore.h"
#include "QuestionSet.h"

// SD-card access for the reading companion.
//
// lib/AiCompanion deliberately takes buffers rather than paths, so all of the
// device's storage knowledge lives here. That keeps the parsing rules — where
// the edge cases are — testable on the host, and leaves this layer with nothing
// but reads, writes and path construction.
//
// The reader's own files sit at a visible root, because a hidden dot-directory
// is not somewhere someone can be asked to find a file they are meant to edit.
// Machine-written state stays under /.crosspoint/ with the rest of it.
namespace CompanionFiles {

inline constexpr const char* COMPANION_DIR = "/companion";
inline constexpr const char* PERSONA_PATH = "/companion/persona.txt";
inline constexpr const char* QUESTIONS_PATH = "/companion/questions.txt";
inline constexpr const char* STATE_DIR = "/.crosspoint/companion";

// Reads /companion/persona.txt. A missing or unreadable file is not an error:
// the store keeps its built-in persona and reports usingFallback(), so the
// companion still works for a reader who has not written one yet.
// Returns true when a persona file was actually loaded.
bool loadPersona(PersonaStore& store);

// Reads /companion/questions.txt. A missing file leaves the set empty; the
// caller supplies translated defaults, which cannot live in a library that has
// no notion of tr().
bool loadQuestions(QuestionSet& set);

// Per-book history file, derived from the book's path the same way bookmarks
// are (BookmarkUtil), so the two stay consistent for the same book.
std::string historyPath(const std::string& bookPath);

// Loads this book's conversation. A missing, truncated or corrupted file leaves
// the store empty and returns false — starting a fresh conversation is better
// than replaying a damaged one into a prompt.
bool loadHistory(const std::string& bookPath, ConversationStore& store);

// Writes through a temporary file and renames, so an interruption cannot leave
// a half-written history that would fail to load on the next open.
bool saveHistory(const std::string& bookPath, const ConversationStore& store);

// Removes this book's history. Conversation is personal in a way a page number
// is not, so deleting it has to be possible without deleting the book.
bool clearHistory(const std::string& bookPath);

}  // namespace CompanionFiles
