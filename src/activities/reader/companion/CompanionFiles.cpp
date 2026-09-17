#include "CompanionFiles.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

namespace {

constexpr char LOG_TAG[] = "COMPANION";

// Bounded by the store's own cap, so an oversized file is read only up to what
// can be kept. One byte spare for the terminator readFileToBuffer writes.
constexpr size_t PERSONA_READ_BYTES = PersonaStore::MAX_BYTES + 1;
constexpr size_t QUESTIONS_READ_BYTES = QuestionSet::MAX_QUESTIONS * (QuestionSet::MAX_QUESTION_BYTES + 2) + 1;

// Reads a whole text file into a heap buffer. Returns an empty unique_ptr when
// the file is absent or unreadable; outLength receives the byte count.
std::unique_ptr<char[]> readTextFile(const char* path, const size_t capacity, size_t& outLength) {
  outLength = 0;
  if (!Storage.exists(path)) return nullptr;

  auto buffer = makeUniqueNoThrow<char[]>(capacity);
  if (!buffer) {
    LOG_ERR(LOG_TAG, "OOM: %u byte buffer for %s", static_cast<unsigned>(capacity), path);
    return nullptr;
  }

  outLength = Storage.readFileToBuffer(path, buffer.get(), capacity);
  if (outLength == 0) {
    LOG_ERR(LOG_TAG, "empty or unreadable: %s", path);
    return nullptr;
  }
  return buffer;
}

}  // namespace

namespace CompanionFiles {

bool loadPersona(PersonaStore& store) {
  size_t length = 0;
  const auto buffer = readTextFile(PERSONA_PATH, PERSONA_READ_BYTES, length);
  if (!buffer) {
    store.clear();  // keeps the built-in persona
    return false;
  }

  const bool loaded = store.load(buffer.get(), length);
  if (!loaded) {
    LOG_DBG(LOG_TAG, "persona.txt was blank; using the built-in persona");
  } else if (store.wasTruncated()) {
    LOG_DBG(LOG_TAG, "persona.txt exceeded %u bytes and was cut", static_cast<unsigned>(PersonaStore::MAX_BYTES));
  }
  return loaded;
}

bool loadQuestions(QuestionSet& set) {
  size_t length = 0;
  const auto buffer = readTextFile(QUESTIONS_PATH, QUESTIONS_READ_BYTES, length);
  if (!buffer) {
    set.clear();
    return false;
  }

  set.load(buffer.get(), length);
  if (set.ignoredLines() > 0) {
    LOG_DBG(LOG_TAG, "questions.txt: %u lines past the cap were ignored", static_cast<unsigned>(set.ignoredLines()));
  }
  return !set.empty();
}

std::string historyPath(const std::string& bookPath) {
  // Same flattening as BookmarkUtil: strip the leading slash, replace
  // separators, drop the extension. Kept in step with bookmarks so one book
  // maps to one predictable name in both places.
  std::string name = bookPath;
  if (!name.empty() && name.front() == '/') name.erase(0, 1);
  std::replace(name.begin(), name.end(), '/', '_');
  std::replace(name.begin(), name.end(), '\\', '_');
  const size_t lastDot = name.find_last_of('.');
  if (lastDot != std::string::npos) name.erase(lastDot);
  return std::string(STATE_DIR) + "/" + name + ".bin";
}

bool loadHistory(const std::string& bookPath, ConversationStore& store) {
  store.clear();
  const std::string path = historyPath(bookPath);
  if (!Storage.exists(path.c_str())) return false;

  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    LOG_ERR(LOG_TAG, "could not open history: %s", path.c_str());
    return false;
  }

  const size_t size = file.fileSize();
  if (size == 0 || size > ConversationStore::ARENA_BYTES * 2) {
    LOG_ERR(LOG_TAG, "implausible history size %u", static_cast<unsigned>(size));
    return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    LOG_ERR(LOG_TAG, "OOM: %u byte history buffer", static_cast<unsigned>(size));
    return false;
  }
  if (file.read(buffer.get(), size) != static_cast<int>(size)) {
    LOG_ERR(LOG_TAG, "short read on history");
    return false;
  }

  if (!store.deserialize(buffer.get(), size)) {
    LOG_ERR(LOG_TAG, "history rejected; starting a fresh conversation");
    return false;
  }
  return true;
}

bool saveHistory(const std::string& bookPath, const ConversationStore& store) {
  if (!Storage.ensureDirectoryExists(STATE_DIR)) {
    LOG_ERR(LOG_TAG, "could not create %s", STATE_DIR);
    return false;
  }

  const size_t size = store.serializedSize();
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    LOG_ERR(LOG_TAG, "OOM: %u byte history buffer", static_cast<unsigned>(size));
    return false;
  }
  if (store.serialize(buffer.get(), size) != size) {
    LOG_ERR(LOG_TAG, "history serialisation disagreed with its own size");
    return false;
  }

  const std::string path = historyPath(bookPath);
  const std::string tempPath = path + ".tmp";

  {
    HalFile file;
    if (!Storage.openFileForWrite(LOG_TAG, tempPath, file)) {
      LOG_ERR(LOG_TAG, "could not open %s", tempPath.c_str());
      return false;
    }
    if (file.write(buffer.get(), size) != size) {
      LOG_ERR(LOG_TAG, "short write on history");
      return false;
    }
    file.flush();
    // Closed at scope exit (DESTRUCTOR_CLOSES_FILE) before the rename below;
    // SdFat must not rename a path that still has an open file.
  }

  // SdFat's rename does not overwrite, so the old file goes first. The instant
  // where neither exists reads as "no history", never as a corrupt one.
  Storage.remove(path.c_str());
  if (!Storage.rename(tempPath.c_str(), path.c_str())) {
    LOG_ERR(LOG_TAG, "could not move history into place");
    return false;
  }
  return true;
}

bool clearHistory(const std::string& bookPath) {
  const std::string path = historyPath(bookPath);
  if (!Storage.exists(path.c_str())) return true;
  return Storage.remove(path.c_str());
}

}  // namespace CompanionFiles
