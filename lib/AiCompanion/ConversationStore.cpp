#include "ConversationStore.h"

#include <cstring>

#include "Utf8.h"

namespace {

// Caps a string on a UTF-8 boundary, returning the usable length.
size_t cappedLength(const char* s, const size_t cap) {
  if (!s) return 0;
  const size_t actual = strlen(s);
  if (actual <= cap) return actual;
  const int safe = utf8SafeTruncateBuffer(s, static_cast<int>(cap));
  return safe > 0 ? static_cast<size_t>(safe) : 0;
}

}  // namespace

void ConversationStore::clear() {
  exchangeCount = 0;
  arenaUsed = 0;
  evictedCount = 0;
}

const char* ConversationStore::question(const size_t index) const {
  return index < exchangeCount ? arena + entries[index].questionOffset : "";
}

const char* ConversationStore::reply(const size_t index) const {
  return index < exchangeCount ? arena + entries[index].replyOffset : "";
}

bool ConversationStore::fits(const size_t questionLength, const size_t replyLength) const {
  // Two NUL terminators, so every stored string can be handed out as a C string.
  return arenaUsed + questionLength + replyLength + 2 <= ARENA_BYTES && exchangeCount < MAX_EXCHANGES;
}

void ConversationStore::dropOldest() {
  if (exchangeCount == 0) return;

  const Entry& oldest = entries[0];
  // Entries are appended in order, so the oldest occupies the front of the arena
  // and its two strings are contiguous.
  const size_t freed = static_cast<size_t>(oldest.questionLength) + oldest.replyLength + 2;

  memmove(arena, arena + freed, arenaUsed - freed);
  arenaUsed -= freed;

  for (size_t i = 1; i < exchangeCount; ++i) {
    entries[i - 1] = entries[i];
    entries[i - 1].questionOffset = static_cast<uint16_t>(entries[i - 1].questionOffset - freed);
    entries[i - 1].replyOffset = static_cast<uint16_t>(entries[i - 1].replyOffset - freed);
  }
  --exchangeCount;
  ++evictedCount;
}

bool ConversationStore::append(const char* question_, const char* reply_) {
  const size_t questionLength = cappedLength(question_, MAX_QUESTION_BYTES);
  const size_t replyLength = cappedLength(reply_, MAX_REPLY_BYTES);
  if (questionLength == 0 && replyLength == 0) return false;

  // An exchange at both caps still has to fit an empty arena, or evicting
  // everything would spin without ever making room.
  if (questionLength + replyLength + 2 > ARENA_BYTES) return false;

  while (!fits(questionLength, replyLength)) dropOldest();

  Entry entry{};
  entry.questionOffset = static_cast<uint16_t>(arenaUsed);
  entry.questionLength = static_cast<uint16_t>(questionLength);
  if (questionLength > 0) memcpy(arena + arenaUsed, question_, questionLength);
  arenaUsed += questionLength;
  arena[arenaUsed++] = '\0';

  entry.replyOffset = static_cast<uint16_t>(arenaUsed);
  entry.replyLength = static_cast<uint16_t>(replyLength);
  if (replyLength > 0) memcpy(arena + arenaUsed, reply_, replyLength);
  arenaUsed += replyLength;
  arena[arenaUsed++] = '\0';

  entries[exchangeCount++] = entry;
  return true;
}

size_t ConversationStore::toExchanges(PromptBuilder::Exchange* out, const size_t capacity) const {
  if (!out) return 0;
  const size_t n = exchangeCount < capacity ? exchangeCount : capacity;
  for (size_t i = 0; i < n; ++i) {
    out[i].question = question(i);
    out[i].reply = reply(i);
  }
  return n;
}

size_t ConversationStore::serializedSize() const {
  size_t total = HEADER_BYTES;
  for (size_t i = 0; i < exchangeCount; ++i) {
    total += 4;  // two uint16 lengths
    total += entries[i].questionLength;
    total += entries[i].replyLength;
  }
  return total;
}

size_t ConversationStore::serialize(uint8_t* out, const size_t cap) const {
  const size_t needed = serializedSize();
  if (!out || cap < needed) return 0;

  size_t offset = 0;
  const uint32_t magic = MAGIC;
  const uint16_t version = VERSION;
  const auto storedCount = static_cast<uint16_t>(exchangeCount);
  // memcpy rather than a cast: RISC-V faults on an unaligned wide store.
  memcpy(out + offset, &magic, sizeof(magic));
  offset += sizeof(magic);
  memcpy(out + offset, &version, sizeof(version));
  offset += sizeof(version);
  memcpy(out + offset, &storedCount, sizeof(storedCount));
  offset += sizeof(storedCount);

  for (size_t i = 0; i < exchangeCount; ++i) {
    const uint16_t questionLength = entries[i].questionLength;
    const uint16_t replyLength = entries[i].replyLength;
    memcpy(out + offset, &questionLength, sizeof(questionLength));
    offset += sizeof(questionLength);
    memcpy(out + offset, &replyLength, sizeof(replyLength));
    offset += sizeof(replyLength);
    memcpy(out + offset, arena + entries[i].questionOffset, questionLength);
    offset += questionLength;
    memcpy(out + offset, arena + entries[i].replyOffset, replyLength);
    offset += replyLength;
  }
  return offset;
}

bool ConversationStore::deserialize(const uint8_t* data, const size_t len) {
  clear();
  if (!data || len < HEADER_BYTES) return false;

  size_t offset = 0;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t storedCount = 0;
  memcpy(&magic, data + offset, sizeof(magic));
  offset += sizeof(magic);
  memcpy(&version, data + offset, sizeof(version));
  offset += sizeof(version);
  memcpy(&storedCount, data + offset, sizeof(storedCount));
  offset += sizeof(storedCount);

  if (magic != MAGIC || version != VERSION || storedCount > MAX_EXCHANGES) return false;

  for (uint16_t i = 0; i < storedCount; ++i) {
    if (offset + 4 > len) {
      clear();
      return false;
    }
    uint16_t questionLength = 0;
    uint16_t replyLength = 0;
    memcpy(&questionLength, data + offset, sizeof(questionLength));
    offset += sizeof(questionLength);
    memcpy(&replyLength, data + offset, sizeof(replyLength));
    offset += sizeof(replyLength);

    if (questionLength > MAX_QUESTION_BYTES || replyLength > MAX_REPLY_BYTES ||
        offset + questionLength + replyLength > len || !fits(questionLength, replyLength)) {
      clear();
      return false;
    }

    Entry entry{};
    entry.questionOffset = static_cast<uint16_t>(arenaUsed);
    entry.questionLength = questionLength;
    memcpy(arena + arenaUsed, data + offset, questionLength);
    arenaUsed += questionLength;
    offset += questionLength;
    arena[arenaUsed++] = '\0';

    entry.replyOffset = static_cast<uint16_t>(arenaUsed);
    entry.replyLength = replyLength;
    memcpy(arena + arenaUsed, data + offset, replyLength);
    arenaUsed += replyLength;
    offset += replyLength;
    arena[arenaUsed++] = '\0';

    entries[exchangeCount++] = entry;
  }
  return true;
}
