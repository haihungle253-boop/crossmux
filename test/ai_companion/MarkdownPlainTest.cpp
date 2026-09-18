#include <gtest/gtest.h>

#include <string>

#include "lib/AiCompanion/MarkdownPlain.h"

namespace {

std::string flat(const std::string& in) { return MarkdownPlain::flatten(in); }

}  // namespace

TEST(MarkdownPlain, StripsBoldMarkers) {
  EXPECT_EQ(flat("**写作手法：侧面干扰。**"), "写作手法：侧面干扰。");
  EXPECT_EQ(flat("前面 **中间** 后面"), "前面 中间 后面");
  EXPECT_EQ(flat("__也是粗体__"), "也是粗体");
}

TEST(MarkdownPlain, StripsItalicMarkers) {
  EXPECT_EQ(flat("*侧重*语气"), "侧重语气");
  EXPECT_EQ(flat("_强调_"), "强调");
}

TEST(MarkdownPlain, HandlesBoldInsideBold) {
  EXPECT_EQ(flat("**外层 *内层* 外层**"), "外层 内层 外层");
}

// The whole point of matching pairs: prose that merely contains a delimiter
// must come through untouched.
TEST(MarkdownPlain, LeavesUnpairedDelimitersAlone) {
  EXPECT_EQ(flat("3 * 4 = 12"), "3 * 4 = 12");
  EXPECT_EQ(flat("一个孤零零的 * 号"), "一个孤零零的 * 号");
  EXPECT_EQ(flat("snake_case_name"), "snake_case_name");
  EXPECT_EQ(flat("**"), "**");
  // A span may not open or close on whitespace, so spaced-out delimiters are
  // arithmetic or punctuation, not emphasis.
  EXPECT_EQ(flat("前面 * 中间 * 后面"), "前面 * 中间 * 后面");
}

// Three or more of the same delimiter alone on a line is a thematic break in
// Markdown, and a row of leftover punctuation on the panel otherwise.
TEST(MarkdownPlain, TreatsALoneDelimiterRunAsARule) {
  EXPECT_EQ(flat("****"), "");
  EXPECT_EQ(flat("上\n___\n下"), "上\n下");
}

TEST(MarkdownPlain, StripsInlineCode) {
  EXPECT_EQ(flat("用 `getTextWidth` 量宽度"), "用 getTextWidth 量宽度");
  EXPECT_EQ(flat("落单的 ` 反引号"), "落单的 ` 反引号");
}

TEST(MarkdownPlain, StripsHeadingMarkers) {
  EXPECT_EQ(flat("## 为什么有效"), "为什么有效");
  EXPECT_EQ(flat("###### 六级标题"), "六级标题");
  EXPECT_EQ(flat("####### 七个井号不是标题"), "####### 七个井号不是标题");
  EXPECT_EQ(flat("#没有空格也不是标题"), "#没有空格也不是标题");
}

TEST(MarkdownPlain, TurnsListMarkersIntoBullets) {
  EXPECT_EQ(flat("- 第一点"), "· 第一点");
  EXPECT_EQ(flat("* 第二点"), "· 第二点");
  EXPECT_EQ(flat("  - 缩进的一点"), "  · 缩进的一点");
  // A numbered list already reads as one; leave it exactly as written.
  EXPECT_EQ(flat("1. 第一条"), "1. 第一条");
}

TEST(MarkdownPlain, StripsBlockquoteMarkers) {
  EXPECT_EQ(flat("> 引用的一句"), "引用的一句");
  EXPECT_EQ(flat(">紧贴的引用"), "紧贴的引用");
}

TEST(MarkdownPlain, KeepsLinkTextAndDropsTarget) {
  EXPECT_EQ(flat("见 [这一段](https://example.com/x) 的写法"), "见 这一段 的写法");
  // Brackets without a target are ordinary punctuation.
  EXPECT_EQ(flat("[未完成的方括号"), "[未完成的方括号");
  EXPECT_EQ(flat("[没有链接]"), "[没有链接]");
}

TEST(MarkdownPlain, DropsRulesAndCodeFences) {
  EXPECT_EQ(flat("上\n---\n下"), "上\n下");
  EXPECT_EQ(flat("上\n***\n下"), "上\n下");
  EXPECT_EQ(flat("上\n```cpp\n代码\n```\n下"), "上\n代码\n下");
}

TEST(MarkdownPlain, PreservesParagraphStructure) {
  EXPECT_EQ(flat("第一段\n\n第二段"), "第一段\n\n第二段");
  EXPECT_EQ(flat("带回车的\r\n两行"), "带回车的\n两行");
}

TEST(MarkdownPlain, HonoursEscapes) {
  EXPECT_EQ(flat("\\*不是强调\\*"), "*不是强调*");
}

TEST(MarkdownPlain, PassesPlainTextThroughUnchanged) {
  const std::string plain =
      "作者没有平铺直叙地说山很高，而是写主角抬头时帽子掉了。\n"
      "这一笔比任何形容词都有效。";
  EXPECT_EQ(flat(plain), plain);
  EXPECT_EQ(flat(""), "");
}

// The actual reply that reached the panel during the first real exchange.
TEST(MarkdownPlain, CleansTheReplyThatExposedThis) {
  const std::string seen =
      "**写作手法：视角重心的“侧面干扰”。**\n"
      "**为什么有效：** 作者没平铺直叙。";
  EXPECT_EQ(flat(seen),
            "写作手法：视角重心的“侧面干扰”。\n"
            "为什么有效： 作者没平铺直叙。");
}
