/* markdown.c — see markdown.h. */
#include "io/markdown.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

/* ---- inline text --------------------------------------------------------- */

#define MD_FLAGS (WP_ATTR_BOLD | WP_ATTR_ITALIC | WP_ATTR_UNDERLINE)

static bool is_space(char c) { return c == ' ' || c == '\t'; }

/* Append text with the characters that could start inline markup escaped.
 * '<' and '&' matter only where they could begin a tag or an entity. */
static void escape_inline(GString *out, const char *s, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    char next = i + 1 < len ? s[i + 1] : 0;
    switch (c) {
    case '\\': case '*': case '_': case '`': case '[': case ']':
      g_string_append_c(out, '\\');
      break;
    case '<':
      if (g_ascii_isalpha(next) || next == '/' || next == '!' || next == '?') g_string_append_c(out, '\\');
      break;
    case '&':
      if (g_ascii_isalpha(next) || next == '#') g_string_append_c(out, '\\');
      break;
    default:
      break;
    }
    g_string_append_c(out, c);
  }
}

/* Bytes at the start of the paragraph that would begin block syntax (a
 * heading, quote, list item, rule or fence) if left alone: the count runs
 * up to and including the character that needs a backslash, 0 if none.
 * Characters escaped inline ('*', '_', '`') are covered already. */
static size_t block_start_len(const char *s, size_t len)
{
  size_t i = 0;
  while (i < len && is_space(s[i])) i++;
  if (i == len) return 0;
  char c = s[i];
  bool end_or_space = i + 1 == len || is_space(s[i + 1]);
  switch (c) {
  case '>': return i + 1;
  case '+': return end_or_space ? i + 1 : 0;
  case '-': return end_or_space || s[i + 1] == '-' ? i + 1 : 0;
  case '#': {
    size_t j = i;
    while (j < len && s[j] == '#' && j - i < 6) j++;
    return j == len || is_space(s[j]) ? i + 1 : 0;
  }
  case '~': return i + 2 < len && s[i + 1] == '~' && s[i + 2] == '~' ? i + 1 : 0;
  default: break;
  }
  if (g_ascii_isdigit(c)) {
    size_t j = i;
    while (j < len && g_ascii_isdigit(s[j])) j++;
    if (j < len && (s[j] == '.' || s[j] == ')') && (j + 1 == len || is_space(s[j + 1]))) return j + 1;
  }
  return 0;
}

/* Emphasis delimiters must hug non-space text, so whitespace at either end
 * of a formatted span stays outside the markers. */
static void append_span(GString *out, const char *s, size_t len, uint32_t flags)
{
  size_t lead = 0, trail = len;
  while (lead < len && is_space(s[lead])) lead++;
  while (trail > lead && is_space(s[trail - 1])) trail--;
  g_string_append_len(out, s, (gssize)lead);
  if (trail > lead) {
    if (flags & WP_ATTR_UNDERLINE) g_string_append(out, "<u>");
    if (flags & WP_ATTR_BOLD)      g_string_append(out, "**");
    if (flags & WP_ATTR_ITALIC)    g_string_append_c(out, '*');
    escape_inline(out, s + lead, trail - lead);
    if (flags & WP_ATTR_ITALIC)    g_string_append_c(out, '*');
    if (flags & WP_ATTR_BOLD)      g_string_append(out, "**");
    if (flags & WP_ATTR_UNDERLINE) g_string_append(out, "</u>");
  }
  g_string_append_len(out, s + trail, (gssize)(len - trail));
}

/* The paragraph's text with inline formatting. Runs that differ only in
 * ways Markdown cannot show (size, font, comment) are merged so the markers
 * do not close and reopen mid-word. Style base attributes are not applied:
 * a heading is bold by being a heading. A block-syntax prefix (see
 * block_start_len) goes out plain with its last character escaped, so any
 * formatting on those few bytes is dropped rather than wrapped around a
 * list marker. */
static void append_inline(GString *out, const WpParagraph *p)
{
  size_t off = block_start_len(p->text, p->len);
  if (off) {
    g_string_append_len(out, p->text, (gssize)(off - 1));
    g_string_append_c(out, '\\');
    g_string_append_c(out, p->text[off - 1]);
  }
  size_t run_start = 0, r = 0;
  while (r < p->nruns && run_start + p->runs[r].len <= off) run_start += p->runs[r++].len;
  while (r < p->nruns) {
    uint32_t flags = p->runs[r].attrs.flags & MD_FLAGS;
    size_t end = run_start;
    while (r < p->nruns && (p->runs[r].attrs.flags & MD_FLAGS) == flags) end += p->runs[r++].len;
    append_span(out, p->text + off, end - off, flags);
    off = run_start = end;
  }
}

/* ---- blocks -------------------------------------------------------------- */

static bool para_blank(const WpParagraph *p)
{
  for (size_t i = 0; i < p->len; i++)
    if (!is_space(p->text[i])) return false;
  return true;
}

/* The heading level a style maps to, 0 for none. Title and Heading 1 share
 * the top level: a document has one title, so no reader is confused. */
static int heading_level(const WpDocument *doc, size_t i)
{
  const WpNamedStyle *s = wp_document_style_of(doc, i);
  if (s->outline_level > 0) return s->outline_level > 6 ? 6 : s->outline_level;
  return strcmp(s->name, "Title") == 0 ? 1 : 0;
}

typedef enum { BLOCK_PLAIN, BLOCK_QUOTE, BLOCK_NOTE, BLOCK_LIST } BlockKind;

static BlockKind block_kind(const WpDocument *doc, size_t i)
{
  if (wp_document_para(doc, i)->style.list_kind != WP_LIST_NONE) return BLOCK_LIST;
  const char *name = wp_document_style_of(doc, i)->name;
  if (!strcmp(name, "Quote")) return BLOCK_QUOTE;
  if (!strcmp(name, "Note"))  return BLOCK_NOTE;
  return BLOCK_PLAIN;
}

char *wp_markdown_format(const WpDocument *doc, size_t *out_len)
{
  GString *out = g_string_new(NULL);
  size_t n = wp_document_para_count(doc);
  BlockKind open = BLOCK_PLAIN;   /* a quote, note or list block being continued */
  int *numbers = malloc((n ? n : 1) * sizeof *numbers);
  wp_document_list_numbers(doc, numbers);
  for (size_t i = 0; i < n; i++) {
    const WpParagraph *p = wp_document_para(doc, i);
    BlockKind kind = block_kind(doc, i);
    if (para_blank(p) && kind != BLOCK_LIST) {
      /* blank lines separate blocks; inside a quote or note they join
       * consecutive paragraphs, which the ">" continuation below does */
      if (kind != open) open = BLOCK_PLAIN;
      continue;
    }
    if (kind == BLOCK_LIST) {
      /* Items are tight (no blank line between them), nested by four
       * spaces a level, which is past any marker's content column. */
      if (open == BLOCK_LIST) g_string_truncate(out, out->len - 1);
      open = BLOCK_LIST;
      int level = p->style.list_level < 0 ? 0 : p->style.list_level;
      for (int l = 0; l < level; l++) g_string_append(out, "    ");
      if (p->style.list_kind == WP_LIST_NUMBER) g_string_append_printf(out, "%d. ", numbers[i]);
      else g_string_append(out, "- ");
      append_inline(out, p);
      g_string_append(out, "\n\n");
      continue;
    }
    if (kind == BLOCK_PLAIN) {
      open = BLOCK_PLAIN;
      int level = heading_level(doc, i);
      for (int h = 0; h < level; h++) g_string_append_c(out, '#');
      if (level) g_string_append_c(out, ' ');
      append_inline(out, p);
      g_string_append(out, "\n\n");
      continue;
    }
    if (kind == open) {
      /* continue the block: replace the closing blank line with a ">" one */
      g_string_truncate(out, out->len - 1);
      g_string_append(out, ">\n");
    } else if (kind == BLOCK_NOTE) {
      g_string_append(out, "> [!NOTE]\n");
    }
    open = kind;
    g_string_append(out, "> ");
    append_inline(out, p);
    g_string_append(out, "\n\n");
  }
  free(numbers);
  if (out->len) g_string_truncate(out, out->len - 1);   /* one trailing newline */
  if (out_len) *out_len = out->len;
  return g_string_free(out, FALSE);
}

bool wp_markdown_write(const WpDocument *doc, const char *path, char **err)
{
  size_t len = 0;
  char *text = wp_markdown_format(doc, &len);
  g_autoptr(GError) gerr = NULL;
  bool ok = g_file_set_contents(path, text, (gssize)len, &gerr);
  free(text);
  if (!ok && err) *err = strdup(gerr->message);
  return ok;
}
