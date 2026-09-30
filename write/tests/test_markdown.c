/* Markdown export: build formatted documents and compare the text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/document.h"
#include "io/markdown.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static WpPos add(WpDocument *d, WpPos at, const char *text, uint32_t flags)
{
  WpTextAttrs a = { flags, 0, NULL, 0 };
  return wp_document_insert_text(d, at, text, strlen(text), &a);
}

static void style(WpDocument *d, size_t para, const char *name)
{
  wp_document_set_style_name(d, (WpPos){ para, 0 }, (WpPos){ para, 0 }, name);
}

static void expect(WpDocument *d, const char *want)
{
  size_t len = 0;
  char *got = wp_markdown_format(d, &len);
  if (strcmp(got, want) || len != strlen(want)) {
    printf("FAIL: markdown differs\n--- want ---\n%s\n--- got ---\n%s\n---\n", want, got);
    failures++;
  }
  free(got);
}

int main(int argc, char **argv)
{
  WpDocument *d = wp_document_new();
  expect(d, "");

  /* styles: title, headings, body, quote, note; blank paragraphs vanish */
  WpPos p = add(d, (WpPos){ 0, 0 }, "My Title\nIntro\n\nFirst\nSecond\nSaid\nAlso said\nHeads up\nBody", 0);
  style(d, 0, "Title");
  style(d, 1, "Heading 1");
  style(d, 3, "Heading 2");
  style(d, 4, "Heading 3");
  style(d, 5, "Quote");
  style(d, 6, "Quote");
  style(d, 7, "Note");
  expect(d,
    "# My Title\n\n# Intro\n\n## First\n\n### Second\n\n"
    "> Said\n>\n> Also said\n\n"
    "> [!NOTE]\n> Heads up\n\n"
    "Body\n");

  /* a blank body paragraph between quotes splits them */
  add(d, p, "\nA\n\nB", 0);
  style(d, 9, "Quote");
  style(d, 11, "Quote");
  size_t len = 0;
  char *md = wp_markdown_format(d, &len);
  CHECK(strstr(md, "> A\n\n> B\n") != NULL);
  free(md);

  /* inline formatting: markers hug the words, runs that differ only in
   * size merge, style base attributes are not repeated */
  wp_document_clear(d);
  p = add(d, (WpPos){ 0, 0 }, "plain ", 0);
  p = add(d, p, "bold ", WP_ATTR_BOLD);
  p = add(d, p, "both", WP_ATTR_BOLD | WP_ATTR_ITALIC);
  p = add(d, p, " under", WP_ATTR_UNDERLINE);
  p = add(d, p, "line", WP_ATTR_UNDERLINE);
  WpTextAttrs big = { WP_ATTR_UNDERLINE, 20, NULL, 0 };
  p = wp_document_insert_text(d, p, "d", 1, &big);
  p = add(d, p, ".", 0);
  expect(d, "plain **bold** ***both*** <u>underlined</u>.\n");
  style(d, 0, "Heading 1");
  expect(d, "# plain **bold** ***both*** <u>underlined</u>.\n");

  /* escaping: inline markup characters, and block syntax at the start */
  wp_document_clear(d);
  add(d, (WpPos){ 0, 0 },
      "a*b _c_ `d` [e] \\f <g> 1 < 2 & &amp; <!-- h\n"
      "# not a heading\n"
      "#hashtag\n"
      "- not a list\n"
      "-5 degrees\n"
      "--flag\n"
      "1. not a list\n"
      "1.5 litres\n"
      "> not a quote\n"
      "+ plus\n"
      "~~~\n"
      "  - indented", 0);
  expect(d,
    "a\\*b \\_c\\_ \\`d\\` \\[e\\] \\\\f \\<g> 1 < 2 & \\&amp; \\<!-- h\n\n"
    "\\# not a heading\n\n"
    "#hashtag\n\n"
    "\\- not a list\n\n"
    "-5 degrees\n\n"
    "\\--flag\n\n"
    "1\\. not a list\n\n"
    "1.5 litres\n\n"
    "\\> not a quote\n\n"
    "\\+ plus\n\n"
    "\\~~~\n\n"
    "  \\- indented\n");

  /* a bold list marker: the marker goes out plain, the rest keeps its formatting */
  wp_document_clear(d);
  p = add(d, (WpPos){ 0, 0 }, "1.", WP_ATTR_BOLD);
  p = add(d, p, " item", WP_ATTR_BOLD);
  expect(d, "1\\. **item**\n");

  /* lists: tight items, nested four spaces a level, numbers as counted, a
   * body paragraph or a bullet ends the numbering */
  wp_document_clear(d);
  p = add(d, (WpPos){ 0, 0 }, "Intro\none\ntwo\nsub\nthree\n\nfour\nbullet\nsub bullet\nfive\nAfter", 0);
  for (size_t i = 1; i <= 4; i++) style(d, i, WP_STYLE_LIST_NUMBER);
  d->paras[3].style.list_level = 1;
  style(d, 6, WP_STYLE_LIST_NUMBER);
  style(d, 7, WP_STYLE_LIST_BULLET);
  style(d, 8, WP_STYLE_LIST_BULLET);
  d->paras[8].style.list_level = 1;
  style(d, 9, WP_STYLE_LIST_NUMBER);
  expect(d,
    "Intro\n\n"
    "1. one\n2. two\n    1. sub\n3. three\n\n"
    "1. four\n- bullet\n    - sub bullet\n1. five\n\n"
    "After\n");
  /* an item whose text looks like a marker is still escaped */
  wp_document_clear(d);
  add(d, (WpPos){ 0, 0 }, "- dash", 0);
  style(d, 0, WP_STYLE_LIST_BULLET);
  expect(d, "- \\- dash\n");

  /* comments are dropped, formatting around them is kept */
  wp_document_clear(d);
  p = add(d, (WpPos){ 0, 0 }, "Alpha beta gamma", WP_ATTR_ITALIC);
  wp_document_add_comment(d, (WpPos){ 0, 6 }, (WpPos){ 0, 10 }, "A", "2026-01-01T00:00:00", "hm");
  expect(d, "*Alpha beta gamma*\n");

  /* writing a file */
  if (argc > 1) {
    char *err = NULL;
    CHECK(wp_markdown_write(d, argv[1], &err));
    CHECK(!wp_markdown_write(d, "/nonexistent-dir/x.md", &err));
    CHECK(err != NULL);
    free(err);
  }

  wp_document_free(d);
  printf(failures ? "markdown: %d failure(s)\n" : "markdown ok\n", failures);
  return failures ? 1 : 0;
}
