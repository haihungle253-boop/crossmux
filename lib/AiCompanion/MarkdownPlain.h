#pragma once

#include <string>

// Flattens the Markdown a chat model reaches for into plain text.
//
// The reply is rendered by the dictionary's paged reader, which takes a plain
// string and has no notion of inline style. So `**like this**` arrives on the
// panel with its asterisks intact, which is what a reader actually sees and
// complains about. Since there is no bold to convert to, the markers come off.
//
// This is deliberately not a Markdown parser. It handles the constructs a model
// produces in ordinary prose and leaves everything else exactly as it was --
// a stray asterisk in the text is far less bad than mangled prose, so every
// rule here only fires on a delimiter it can match.
//
// UTF-8 safe: only ASCII delimiters are ever removed, and multi-byte sequences
// are copied through untouched.
namespace MarkdownPlain {

// Emphasis, inline code, links, headings, quotes, bullets and rules.
// Line structure is preserved: one input line stays one output line, except
// horizontal rules and code fences, which are dropped entirely.
std::string flatten(const std::string& markdown);

}  // namespace MarkdownPlain
