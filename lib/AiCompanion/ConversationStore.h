#pragma once

#include <cstddef>
#include <cstdint>

#include "PromptBuilder.h"

// Per-book conversation memory: the last few exchanges, kept so a follow-up has
// something to follow.
//
// Storage is one fixed arena sized to match PromptBuilder's history budget, not
// a per-exchange array. Ten exchanges at their individual caps would be over
// 12 KB reserved against a 4 KB budget that can never use it; a shared arena
// bounds the real cost and makes "drop the oldest until it fits" fall out of the
// same rule that governs what is actually sent.
//
// Oldest is always what goes. A follow-up depends on the most recent turns, so
// those are the ones worth the bytes.
//
// About 4.3 KB in size, which belongs in an owning activity's allocation (PSRAM
// on S3 targets) rather than on the stack.
class ConversationStore {
 public:
  static constexpr size_t ARENA_BYTES = 4096;
  static constexpr size_t MAX_EXCHANGES = 10;
  static constexpr size_t MAX_QUESTION_BYTES = 256;
  static constexpr size_t MAX_REPLY_BYTES = 1024;

  static constexpr uint32_t MAGIC = 0x31434D43;  // "CMC1"
  // 2 added a per-exchange timestamp. Version 1 files are still read -- their
  // exchanges simply have no time -- because refusing them would throw away a
  // reader's conversation to gain a feature they had not asked for.
  static constexpr uint16_t VERSION = 2;
  static constexpr uint16_t VERSION_WITHOUT_TIMESTAMPS = 1;
  static constexpr size_t HEADER_BYTES = 8;

  void clear();

  // Appends a completed exchange, dropping the oldest as needed. Returns false
  // only if the exchange is unusable (both sides empty) or cannot fit even in an
  // empty arena, in which case nothing is stored and nothing is dropped.
  //
  // `at` is epoch seconds, or 0 when the device's clock is not trustworthy. It
  // is what lets the companion notice that a reader has been away, so 0 has to
  // mean "no idea" rather than "the epoch": a wrong clock must not produce a
  // confident claim about how long it has been.
  bool append(const char* question, const char* reply, uint32_t at = 0);

  size_t count() const { return exchangeCount; }
  bool empty() const { return exchangeCount == 0; }
  // index 0 is the oldest retained exchange.
  const char* question(size_t index) const;
  const char* reply(size_t index) const;
  // Epoch seconds, or 0 if unknown (clock unset when it was stored, or the file
  // predates version 2).
  uint32_t timestamp(size_t index) const;
  // When the newest retained exchange happened, or 0 if unknown.
  uint32_t lastTimestamp() const;

  size_t usedBytes() const { return arenaUsed; }
  // Exchanges dropped to make room, over this object's lifetime.
  size_t evicted() const { return evictedCount; }

  // Fills `out` with Exchange views for PromptBuilder, oldest first. Returns the
  // number written. The pointers reference this store and are invalidated by
  // append() or clear().
  size_t toExchanges(PromptBuilder::Exchange* out, size_t capacity) const;

  // Versioned, self-describing, and read back with memcpy so an unaligned
  // buffer cannot fault on RISC-V. Returns bytes written, or 0 if `cap` is too
  // small. serializedSize() gives the exact requirement.
  size_t serializedSize() const;
  size_t serialize(uint8_t* out, size_t cap) const;

  // Replaces all contents. A wrong magic, an unknown version or a truncated or
  // internally inconsistent body leaves the store empty and returns false —
  // losing a conversation is better than replaying a corrupted one into a prompt.
  bool deserialize(const uint8_t* data, size_t len);

 private:
  struct Entry {
    uint16_t questionOffset;
    uint16_t questionLength;
    uint16_t replyOffset;
    uint16_t replyLength;
    uint32_t at;
  };

  void dropOldest();
  bool fits(size_t questionLength, size_t replyLength) const;

  char arena[ARENA_BYTES]{};
  Entry entries[MAX_EXCHANGES]{};
  size_t exchangeCount = 0;
  size_t arenaUsed = 0;
  size_t evictedCount = 0;
};
