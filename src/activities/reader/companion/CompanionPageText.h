#pragma once

#include <Epub/Page.h>
#include <Utf8.h>

#include <string>

// Flattens a laid-out page back into plain text for the companion's context.
//
// The reader already holds the current page as positioned TextBlocks, so the
// words are right there; nothing needs re-parsing from the EPUB. Words are
// joined with a space and lines with a newline, which is enough shape for a
// model and costs nothing to produce.
//
// The result is capped and cut on a UTF-8 boundary, because this text goes
// straight into a request where a half character would be malformed.
namespace CompanionPageText {

inline std::string extract(const Page& page, const size_t maxBytes) {
  std::string out;
  out.reserve(maxBytes < 2048 ? maxBytes : 2048);

  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    const size_t lineStart = out.size();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!text || *text == '\0') continue;
      if (out.size() > lineStart) out.push_back(' ');
      out.append(text);
      if (out.size() >= maxBytes) {
        const int safe = utf8SafeTruncateBuffer(out.data(), static_cast<int>(maxBytes));
        out.resize(safe > 0 ? static_cast<size_t>(safe) : 0);
        return out;
      }
    }
    if (out.size() > lineStart) out.push_back('\n');
  }

  if (out.size() > maxBytes) {
    const int safe = utf8SafeTruncateBuffer(out.data(), static_cast<int>(maxBytes));
    out.resize(safe > 0 ? static_cast<size_t>(safe) : 0);
  }
  return out;
}

}  // namespace CompanionPageText
