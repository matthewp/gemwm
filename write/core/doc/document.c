#include "document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- interning --------------------------------------------------------- */

static char  **intern_tab;
static size_t  intern_n, intern_cap;

const char *wp_intern(const char *s)
{
  if (!s) return NULL;
  for (size_t i = 0; i < intern_n; i++)
    if (strcmp(intern_tab[i], s) == 0) return intern_tab[i];
  if (intern_n == intern_cap) {
    intern_cap = intern_cap ? intern_cap * 2 : 16;
    intern_tab = realloc(intern_tab, intern_cap * sizeof *intern_tab);
  }
  intern_tab[intern_n] = strdup(s);
  return intern_tab[intern_n++];
}

/* ---- utf-8 --------------------------------------------------------------- */

static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t wp_utf8_next(const char *text, size_t len, size_t off)
{
  if (off >= len) return len;
  off++;
  while (off < len && is_cont((unsigned char)text[off])) off++;
  return off;
}

size_t wp_utf8_prev(const char *text, size_t off)
{
  if (off == 0) return 0;
  off--;
  while (off > 0 && is_cont((unsigned char)text[off])) off--;
  return off;
}

/* ---- small helpers ------------------------------------------------------- */

bool wp_attrs_equal(const WpTextAttrs *a, const WpTextAttrs *b)
{
  return a->flags == b->flags && a->size_pt == b->size_pt && a->family == b->family &&
         a->comment == b->comment;
}

int wp_pos_cmp(WpPos a, WpPos b)
{
  if (a.para != b.para) return a.para < b.para ? -1 : 1;
  if (a.offset != b.offset) return a.offset < b.offset ? -1 : 1;
  return 0;
}

static size_t max_sz(size_t a, size_t b) { return a > b ? a : b; }
static size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

/* ---- paragraph internals ------------------------------------------------- */

static void para_reserve(WpParagraph *p, size_t extra)
{
  size_t need = p->len + extra + 1;
  if (need <= p->cap) return;
  size_t cap = p->cap ? p->cap : 32;
  while (cap < need) cap *= 2;
  p->text = realloc(p->text, cap);
  p->cap = cap;
}

static void para_insert_run(WpParagraph *p, size_t idx, WpRun run)
{
  if (p->nruns == p->runs_cap) {
    p->runs_cap = p->runs_cap ? p->runs_cap * 2 : 4;
    p->runs = realloc(p->runs, p->runs_cap * sizeof *p->runs);
  }
  memmove(p->runs + idx + 1, p->runs + idx, (p->nruns - idx) * sizeof *p->runs);
  p->runs[idx] = run;
  p->nruns++;
}

static void para_remove_run(WpParagraph *p, size_t idx)
{
  memmove(p->runs + idx, p->runs + idx + 1, (p->nruns - idx - 1) * sizeof *p->runs);
  p->nruns--;
}

static void para_init(WpParagraph *p, const char *style_name, const WpParaStyle *style, const WpTextAttrs *attrs)
{
  memset(p, 0, sizeof *p);
  p->style_name = style_name;
  p->style = *style;
  para_reserve(p, 0);
  p->text[0] = 0;
  para_insert_run(p, 0, (WpRun){ 0, *attrs });
}

static void para_free(WpParagraph *p)
{
  free(p->text);
  free(p->runs);
  memset(p, 0, sizeof *p);
}

/* Restore invariants: drop zero-length runs (unless sole), merge equal neighbours. */
static void para_normalize(WpParagraph *p)
{
  for (size_t i = 0; i < p->nruns && p->nruns > 1;) {
    if (p->runs[i].len == 0) para_remove_run(p, i);
    else i++;
  }
  for (size_t i = 1; i < p->nruns;) {
    if (wp_attrs_equal(&p->runs[i - 1].attrs, &p->runs[i].attrs)) {
      p->runs[i - 1].len += p->runs[i].len;
      para_remove_run(p, i);
    } else i++;
  }
}

/* Index of the run containing byte `offset`; offset == len yields the last run. */
static size_t para_run_at(const WpParagraph *p, size_t offset)
{
  size_t s = 0;
  for (size_t i = 0; i < p->nruns; i++) {
    if (offset < s + p->runs[i].len) return i;
    s += p->runs[i].len;
  }
  return p->nruns - 1;
}

/* Ensure a run boundary exists at `offset`; return index of the run that
 * starts there (== nruns when offset == len). */
static size_t para_split_run_at(WpParagraph *p, size_t offset)
{
  size_t s = 0;
  for (size_t i = 0; i < p->nruns; i++) {
    if (s == offset) return i;
    size_t e = s + p->runs[i].len;
    if (offset < e) {
      WpRun tail = { e - offset, p->runs[i].attrs };
      p->runs[i].len = offset - s;
      para_insert_run(p, i + 1, tail);
      return i + 1;
    }
    s = e;
  }
  return p->nruns;
}

/* Attributes that typing at `offset` should inherit. */
static WpTextAttrs para_inherit_attrs(const WpParagraph *p, size_t offset)
{
  return p->runs[para_run_at(p, offset > 0 ? offset - 1 : 0)].attrs;
}

/* Whether text typed at `offset` sits strictly inside the range of comment
 * `id`: the characters on both sides carry it. At either edge, or in an
 * empty paragraph, typing does not extend the comment. */
static bool para_comment_continues(const WpParagraph *p, size_t offset, uint32_t id)
{
  if (offset == 0 || offset >= p->len) return false;
  return p->runs[para_run_at(p, offset - 1)].attrs.comment == id &&
         p->runs[para_run_at(p, offset)].attrs.comment == id;
}

static void para_insert_bytes(WpParagraph *p, size_t offset, const char *s, size_t n,
                              const WpTextAttrs *attrs)
{
  if (n == 0) return;
  WpTextAttrs edge;
  if (!attrs) {   /* inherited attributes: typing at a comment's edge stays outside it */
    edge = para_inherit_attrs(p, offset);
    if (edge.comment && !para_comment_continues(p, offset, edge.comment)) {
      edge.comment = 0;
      attrs = &edge;
    }
  }
  para_reserve(p, n);
  memmove(p->text + offset + n, p->text + offset, p->len - offset);
  memcpy(p->text + offset, s, n);
  p->len += n;
  p->text[p->len] = 0;

  if (attrs) {
    size_t i = para_split_run_at(p, offset);
    para_insert_run(p, i, (WpRun){ n, *attrs });
  } else {
    p->runs[para_run_at(p, offset > 0 ? offset - 1 : 0)].len += n;
  }
  para_normalize(p);
}

static void para_delete_bytes(WpParagraph *p, size_t a, size_t b)
{
  if (a >= b) return;
  size_t s = 0;
  for (size_t i = 0; i < p->nruns; i++) {
    size_t e = s + p->runs[i].len;
    size_t lo = max_sz(s, a), hi = min_sz(e, b);
    if (hi > lo) p->runs[i].len -= hi - lo;
    s = e;
  }
  memmove(p->text + a, p->text + b, p->len - b);
  p->len -= b - a;
  p->text[p->len] = 0;
  para_normalize(p);
}

/* ---- document internals -------------------------------------------------- */

static void doc_insert_para(WpDocument *d, size_t idx, WpParagraph p)
{
  if (d->nparas == d->paras_cap) {
    d->paras_cap = d->paras_cap ? d->paras_cap * 2 : 8;
    d->paras = realloc(d->paras, d->paras_cap * sizeof *d->paras);
  }
  memmove(d->paras + idx + 1, d->paras + idx, (d->nparas - idx) * sizeof *d->paras);
  d->paras[idx] = p;
  d->nparas++;
}

static void doc_remove_para(WpDocument *d, size_t idx)
{
  para_free(&d->paras[idx]);
  memmove(d->paras + idx, d->paras + idx + 1, (d->nparas - idx - 1) * sizeof *d->paras);
  d->nparas--;
}

/* Split paragraph at pos; text after pos moves to a new following paragraph. */
static void doc_split_para(WpDocument *d, WpPos pos)
{
  WpParagraph *p = &d->paras[pos.para];
  WpTextAttrs inherit = para_inherit_attrs(p, pos.offset);

  WpParagraph np;
  memset(&np, 0, sizeof np);
  np.style_name = p->style_name;
  np.style = p->style;
  /* Enter at the end of a heading starts its "next" style (normally body text). */
  const WpNamedStyle *ns = wp_document_style_of(d, pos.para);
  if (pos.offset == p->len && ns->next) {
    const WpNamedStyle *nx = wp_document_find_style(d, ns->next);
    if (nx) { np.style_name = nx->name; np.style = nx->para; }
  }
  size_t tail = p->len - pos.offset;
  para_reserve(&np, tail);
  memcpy(np.text, p->text + pos.offset, tail);
  np.len = tail;
  np.text[tail] = 0;

  size_t i = para_split_run_at(p, pos.offset);
  for (size_t k = i; k < p->nruns; k++) para_insert_run(&np, np.nruns, p->runs[k]);
  p->nruns = i;
  if (np.nruns == 0) para_insert_run(&np, 0, (WpRun){ 0, inherit });
  if (p->nruns == 0) para_insert_run(p, 0, (WpRun){ 0, inherit });

  p->len = pos.offset;
  p->text[p->len] = 0;
  para_normalize(p);
  para_normalize(&np);
  doc_insert_para(d, pos.para + 1, np);
}

/* Append paragraph i+1 onto paragraph i and remove it. */
static void doc_merge_para(WpDocument *d, size_t i)
{
  WpParagraph *a = &d->paras[i], *b = &d->paras[i + 1];
  if (b->len > 0) {
    para_reserve(a, b->len);
    memcpy(a->text + a->len, b->text, b->len);
    a->len += b->len;
    a->text[a->len] = 0;
    for (size_t k = 0; k < b->nruns; k++) para_insert_run(a, a->nruns, b->runs[k]);
    para_normalize(a);
  }
  doc_remove_para(d, i + 1);
}

/* ---- named styles -------------------------------------------------------- */

const WpNamedStyle *wp_document_find_style(const WpDocument *d, const char *name)
{
  if (!name) return NULL;
  for (size_t i = 0; i < d->nstyles; i++)
    if (d->styles[i].name == name || strcmp(d->styles[i].name, name) == 0) return &d->styles[i];
  return NULL;
}

const WpNamedStyle *wp_document_style_of(const WpDocument *d, size_t para)
{
  const WpNamedStyle *s = para < d->nparas ? wp_document_find_style(d, d->paras[para].style_name) : NULL;
  if (!s) s = wp_document_find_style(d, WP_STYLE_STANDARD);
  return s ? s : &d->styles[0];
}

void wp_document_add_style(WpDocument *d, const WpNamedStyle *style)
{
  WpNamedStyle ns = *style;
  ns.name = wp_intern(ns.name);
  ns.next = wp_intern(ns.next);
  ns.text.family = wp_intern(ns.text.family);
  for (size_t i = 0; i < d->nstyles; i++) {
    if (d->styles[i].name != ns.name) continue;
    d->styles[i] = ns;
    for (size_t k = 0; k < d->nparas; k++)     /* paragraphs using it pick up the new properties */
      if (d->paras[k].style_name == ns.name) d->paras[k].style = ns.para;
    return;
  }
  if (d->nstyles == d->styles_cap) {
    d->styles_cap = d->styles_cap ? d->styles_cap * 2 : 8;
    d->styles = realloc(d->styles, d->styles_cap * sizeof *d->styles);
  }
  d->styles[d->nstyles++] = ns;
}

void wp_document_reset_styles(WpDocument *d)
{
  d->nstyles = 0;
  const WpParaStyle body = d->default_style;
  WpNamedStyle std = { WP_STYLE_STANDARD, 0, false, NULL, body, { 0, 0, NULL } };
  wp_document_add_style(d, &std);
  WpNamedStyle title = { "Title", 0, true, WP_STYLE_STANDARD,
                         { WP_ALIGN_CENTER, 0.0f, 12.0f, body.line_spacing }, { WP_ATTR_BOLD, 28.0f, NULL } };
  wp_document_add_style(d, &title);
  struct { const char *name; float size, before, after; } h[] = {
    { "Heading 1", 18.0f, 12.0f, 6.0f }, { "Heading 2", 16.0f, 10.0f, 6.0f }, { "Heading 3", 14.0f, 8.0f, 4.0f },
  };
  for (size_t i = 0; i < sizeof h / sizeof h[0]; i++) {
    WpNamedStyle hs = { h[i].name, (int)i + 1, true, WP_STYLE_STANDARD,
                        { body.align, h[i].before, h[i].after, body.line_spacing }, { WP_ATTR_BOLD, h[i].size, NULL } };
    wp_document_add_style(d, &hs);
  }
  WpNamedStyle quote = { "Quote", 0, false, WP_STYLE_STANDARD,
                         { body.align, 6.0f, 6.0f, body.line_spacing, 36.0f, 36.0f, 0, 0, 0, 0 }, { WP_ATTR_ITALIC, 0, NULL } };
  wp_document_add_style(d, &quote);
  WpNamedStyle note = { "Note", 0, false, WP_STYLE_STANDARD,
                        { body.align, 6.0f, 6.0f, body.line_spacing, 0, 0, 8.0f, 3.0f, WP_COLOR(0x35, 0x84, 0xe4), WP_COLOR(0xee, 0xf4, 0xfc) },
                        { 0, 0, NULL } };
  wp_document_add_style(d, &note);
  /* Lists have no "next" style: Enter at the end of an item adds another. */
  WpNamedStyle bullet = { WP_STYLE_LIST_BULLET, 0, false, NULL,
                          { body.align, 0, 0, body.line_spacing, wp_list_indent(0), 0, 0, 0, 0, 0, WP_LIST_BULLET, 0 }, { 0, 0, NULL } };
  wp_document_add_style(d, &bullet);
  WpNamedStyle number = { WP_STYLE_LIST_NUMBER, 0, false, NULL,
                          { body.align, 0, 0, body.line_spacing, wp_list_indent(0), 0, 0, 0, 0, 0, WP_LIST_NUMBER, 0 }, { 0, 0, NULL } };
  wp_document_add_style(d, &number);
  for (size_t k = 0; k < d->nparas; k++) {
    const WpNamedStyle *s = wp_document_find_style(d, d->paras[k].style_name);
    if (!s) { d->paras[k].style_name = d->styles[0].name; d->paras[k].style = d->styles[0].para; }
  }
}

void wp_document_set_style_name(WpDocument *d, WpPos a, WpPos b, const char *name)
{
  const WpNamedStyle *s = wp_document_find_style(d, name);
  if (!s) return;
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  for (size_t pi = a.para; pi <= b.para; pi++) {
    d->paras[pi].style_name = s->name;
    d->paras[pi].style = s->para;
  }
  wp_document_changed(d, a.para);
}

WpTextAttrs wp_document_para_base_attrs(const WpDocument *d, size_t para)
{
  const WpNamedStyle *s = wp_document_style_of(d, para);
  WpTextAttrs r = d->default_attrs;
  r.flags = s->text.flags;
  if (s->text.size_pt > 0) r.size_pt = s->text.size_pt;
  if (s->text.family) r.family = s->text.family;
  return r;
}

/* ---- tab stops ----------------------------------------------------------- */

static bool tab_near(float a, float b) { return a - b < 0.5f && b - a < 0.5f; }

void wp_para_style_add_tab(WpParaStyle *st, float pos_pt, WpTabKind kind)
{
  if (pos_pt < 0) pos_pt = 0;
  for (int i = 0; i < st->ntabs; i++)
    if (tab_near(st->tabs[i].pos_pt, pos_pt)) { st->tabs[i].kind = kind; return; }
  if (st->ntabs == WP_MAX_TABS) {
    if (pos_pt > st->tabs[WP_MAX_TABS - 1].pos_pt) return;
    st->ntabs--;
  }
  int i = st->ntabs;
  while (i > 0 && st->tabs[i - 1].pos_pt > pos_pt) { st->tabs[i] = st->tabs[i - 1]; i--; }
  st->tabs[i] = (WpTabStop){ pos_pt, kind };
  st->ntabs++;
}

bool wp_para_style_remove_tab(WpParaStyle *st, float pos_pt)
{
  for (int i = 0; i < st->ntabs; i++) {
    if (!tab_near(st->tabs[i].pos_pt, pos_pt)) continue;
    memmove(st->tabs + i, st->tabs + i + 1, (size_t)(st->ntabs - i - 1) * sizeof *st->tabs);
    st->ntabs--;
    memset(st->tabs + st->ntabs, 0, sizeof *st->tabs);
    return true;
  }
  return false;
}

/* ---- lists --------------------------------------------------------------- */

void wp_document_list_numbers(const WpDocument *d, int *out)
{
  int count[WP_LIST_MAX_LEVEL + 1] = { 0 };
  for (size_t i = 0; i < d->nparas; i++) {
    const WpParaStyle *st = &d->paras[i].style;
    out[i] = 0;
    if (st->list_kind == WP_LIST_NONE) { memset(count, 0, sizeof count); continue; }
    int level = st->list_level < 0 ? 0 : st->list_level > WP_LIST_MAX_LEVEL ? WP_LIST_MAX_LEVEL : st->list_level;
    for (int l = level + 1; l <= WP_LIST_MAX_LEVEL; l++) count[l] = 0;
    if (st->list_kind == WP_LIST_BULLET) count[level] = 0;
    else out[i] = ++count[level];
  }
}

/* Lower-case roman numerals; buf needs 16 bytes (up to 3999 fits). */
static void roman(int n, char *buf)
{
  static const struct { int v; const char *s; } t[] = {
    { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" }, { 90, "xc" },
    { 50, "l" }, { 40, "xl" }, { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" },
  };
  size_t used = 0;
  buf[0] = 0;
  if (n > 3999) n = 3999;
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    while (n >= t[i].v && used + strlen(t[i].s) < 15) {
      strcpy(buf + used, t[i].s);
      used += strlen(t[i].s);
      n -= t[i].v;
    }
  }
}

/* Letters: a. b. ... z. aa. ab. ...; buf needs 16 bytes. */
static void letters(int n, char *buf)
{
  char rev[8];
  int k = 0;
  while (n > 0 && k < 6) { n--; rev[k++] = (char)('a' + n % 26); n /= 26; }
  for (int i = 0; i < k; i++) buf[i] = rev[k - 1 - i];
  buf[k] = 0;
}

void wp_list_label(WpListKind kind, int level, int number, char *buf)
{
  static const char *bullets[] = { "\xe2\x80\xa2", "\xe2\x97\xa6", "\xe2\x96\xaa" };   /* U+2022, U+25E6, U+25AA */
  if (level < 0) level = 0;
  if (kind == WP_LIST_BULLET) { strcpy(buf, bullets[level % 3]); return; }
  if (kind != WP_LIST_NUMBER) { buf[0] = 0; return; }
  if (number < 1) number = 1;
  char body[16];
  switch (level % 3) {
  case 0:  snprintf(body, sizeof body, "%d", number > 999999 ? 999999 : number); break;
  case 1:  letters(number, body); break;
  default: roman(number, body); break;
  }
  body[14] = 0;
  strcpy(buf, body);
  strcat(buf, ".");
}

/* ---- public: lifecycle --------------------------------------------------- */

/* Runs store only what differs from the paragraph's base attributes; a fresh
 * paragraph therefore starts with "inherit everything". */
static const WpTextAttrs no_attrs = { 0, 0, NULL, 0 };

WpDocument *wp_document_new(void)
{
  WpDocument *d = calloc(1, sizeof *d);
  d->default_attrs = (WpTextAttrs){ 0, 12.0f, wp_intern("Serif") };
  d->default_style = (WpParaStyle){ WP_ALIGN_LEFT, 0.0f, 0.0f, 1.0f };
  d->page = wp_page_setup_letter();
  wp_document_reset_styles(d);
  wp_document_clear(d);
  return d;
}

void wp_document_free(WpDocument *d)
{
  if (!d) return;
  for (size_t i = 0; i < d->nparas; i++) para_free(&d->paras[i]);
  free(d->paras);
  free(d->styles);
  wp_comments_free(d->comments, d->ncomments);
  free(d);
}

void wp_document_clear(WpDocument *d)
{
  for (size_t i = 0; i < d->nparas; i++) para_free(&d->paras[i]);
  d->nparas = 0;
  wp_comments_free(d->comments, d->ncomments);
  d->comments = NULL;
  d->ncomments = d->comments_cap = 0;
  d->next_comment_id = 1;
  const WpNamedStyle *std = wp_document_find_style(d, WP_STYLE_STANDARD);
  WpParagraph p;
  para_init(&p, std->name, &std->para, &no_attrs);
  doc_insert_para(d, 0, p);
}

void wp_document_changed(WpDocument *d, size_t first_para)
{
  if (d->on_change) d->on_change(d, first_para, d->on_change_user);
}

/* ---- public: queries ----------------------------------------------------- */

size_t wp_document_para_count(const WpDocument *d) { return d->nparas; }
const WpParagraph *wp_document_para(const WpDocument *d, size_t i) { return &d->paras[i]; }

WpPos wp_document_end(const WpDocument *d)
{
  return (WpPos){ d->nparas - 1, d->paras[d->nparas - 1].len };
}

WpPos wp_document_clamp(const WpDocument *d, WpPos p)
{
  if (p.para >= d->nparas) return wp_document_end(d);
  if (p.offset > d->paras[p.para].len) p.offset = d->paras[p.para].len;
  return p;
}

bool wp_document_pos_forward(const WpDocument *d, WpPos *p)
{
  const WpParagraph *para = &d->paras[p->para];
  if (p->offset < para->len) { p->offset = wp_utf8_next(para->text, para->len, p->offset); return true; }
  if (p->para + 1 < d->nparas) { p->para++; p->offset = 0; return true; }
  return false;
}

bool wp_document_pos_backward(const WpDocument *d, WpPos *p)
{
  const WpParagraph *para = &d->paras[p->para];
  if (p->offset > 0) { p->offset = wp_utf8_prev(para->text, p->offset); return true; }
  if (p->para > 0) { p->para--; p->offset = d->paras[p->para].len; return true; }
  return false;
}

static bool is_word_byte(unsigned char c)
{
  return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || c == '_' || c == '\'';
}

void wp_document_word_bounds(const WpDocument *d, WpPos p, WpPos *start, WpPos *end)
{
  const WpParagraph *para = &d->paras[p.para];
  size_t s = p.offset, e = p.offset;
  if (para->len == 0) { *start = *end = p; return; }
  if (s == para->len) s = e = wp_utf8_prev(para->text, s);
  bool word = is_word_byte((unsigned char)para->text[s]);
  while (s > 0 && is_word_byte((unsigned char)para->text[s - 1]) == word) s--;
  while (e < para->len && is_word_byte((unsigned char)para->text[e]) == word) e++;
  *start = (WpPos){ p.para, s };
  *end   = (WpPos){ p.para, e };
}

/* ---- public: paragraph snapshots ----------------------------------------- */

static void para_copy(WpParagraph *dst, const WpParagraph *src)
{
  memset(dst, 0, sizeof *dst);
  dst->len = src->len;
  dst->cap = src->len + 1;
  dst->text = malloc(dst->cap);
  memcpy(dst->text, src->text, src->len + 1);
  dst->nruns = dst->runs_cap = src->nruns;
  dst->runs = malloc(src->nruns * sizeof *src->runs);
  memcpy(dst->runs, src->runs, src->nruns * sizeof *src->runs);
  dst->style_name = src->style_name;
  dst->style = src->style;
}

WpParagraph *wp_document_copy_paras(const WpDocument *d, size_t first, size_t count)
{
  WpParagraph *out = malloc(max_sz(count, 1) * sizeof *out);
  for (size_t i = 0; i < count; i++) para_copy(&out[i], &d->paras[first + i]);
  return out;
}

void wp_paragraphs_free(WpParagraph *paras, size_t count)
{
  if (!paras) return;
  for (size_t i = 0; i < count; i++) para_free(&paras[i]);
  free(paras);
}

void wp_document_replace_paras(WpDocument *d, size_t first, size_t count, const WpParagraph *src, size_t nsrc)
{
  for (size_t i = 0; i < count; i++) doc_remove_para(d, first);
  for (size_t i = 0; i < nsrc; i++) {
    WpParagraph p;
    para_copy(&p, &src[i]);
    doc_insert_para(d, first + i, p);
  }
  wp_document_changed(d, first);
}

/* ---- public: fragments --------------------------------------------------- */

WpParagraph *wp_document_copy_range(const WpDocument *d, WpPos a, WpPos b, size_t *count)
{
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  size_t n = b.para - a.para + 1;
  WpParagraph *out = wp_document_copy_paras(d, a.para, n);
  /* Trim the tail first so a.offset is still right when both ends share a paragraph. */
  para_delete_bytes(&out[n - 1], b.offset, out[n - 1].len);
  para_delete_bytes(&out[0], 0, a.offset);
  *count = n;
  return out;
}

WpParagraph *wp_paragraphs_from_text(const char *text, size_t len, size_t *count)
{
  static const WpParaStyle unstyled = { WP_ALIGN_LEFT, 0, 0, 1.0f };
  size_t n = 0, cap = 4;
  WpParagraph *out = malloc(cap * sizeof *out);
  size_t i = 0;
  for (;;) {
    size_t j = i;
    while (j < len && text[j] != '\n') j++;
    size_t e = j;
    if (e > i && text[e - 1] == '\r') e--;
    if (n == cap) out = realloc(out, (cap *= 2) * sizeof *out);
    para_init(&out[n], NULL, &unstyled, &no_attrs);
    para_insert_bytes(&out[n], 0, text + i, e - i, NULL);
    n++;
    if (j >= len) break;
    i = j + 1;
  }
  *count = n;
  return out;
}

static bool doc_comment_in_use(const WpDocument *d, uint32_t id)
{
  for (size_t pi = 0; pi < d->nparas; pi++)
    for (size_t r = 0; r < d->paras[pi].nruns; r++)
      if (d->paras[pi].runs[r].attrs.comment == id) return true;
  return false;
}

/* Comment ids a fragment may keep when pasted: known to the document and
 * carried by no text right now (see wp_document_insert_paras). */
typedef struct KeepIds { uint32_t *ids; size_t n; } KeepIds;

static KeepIds keep_ids_for(const WpDocument *d, const WpParagraph *src, size_t nsrc)
{
  KeepIds k = { NULL, 0 };
  size_t cap = 0;
  for (size_t i = 0; i < nsrc; i++) {
    for (size_t r = 0; r < src[i].nruns; r++) {
      uint32_t id = src[i].runs[r].attrs.comment;
      if (!id) continue;
      bool seen = false;
      for (size_t j = 0; j < k.n; j++) if (k.ids[j] == id) seen = true;
      if (seen || !wp_document_comment(d, id) || doc_comment_in_use(d, id)) continue;
      if (k.n == cap) k.ids = realloc(k.ids, (cap = cap ? cap * 2 : 4) * sizeof *k.ids);
      k.ids[k.n++] = id;
    }
  }
  return k;
}

static uint32_t keep_ids_filter(const KeepIds *k, uint32_t id)
{
  for (size_t j = 0; j < k->n; j++) if (k->ids[j] == id) return id;
  return 0;
}

/* Insert every run of src into dst at offset, keeping the run attributes
 * (comment ids filtered through keep); returns the offset just after them. */
static size_t para_insert_runs(WpParagraph *dst, size_t offset, const WpParagraph *src, const KeepIds *keep)
{
  size_t s = 0;
  for (size_t i = 0; i < src->nruns; i++) {
    WpTextAttrs a = src->runs[i].attrs;
    a.comment = keep_ids_filter(keep, a.comment);
    para_insert_bytes(dst, offset + s, src->text + s, src->runs[i].len, &a);
    s += src->runs[i].len;
  }
  return offset + s;
}

static void para_filter_comments(WpParagraph *p, const KeepIds *keep)
{
  for (size_t r = 0; r < p->nruns; r++) p->runs[r].attrs.comment = keep_ids_filter(keep, p->runs[r].attrs.comment);
  para_normalize(p);
}

static void para_take_style(WpParagraph *p, const WpParagraph *from, const WpParagraph *fallback)
{
  const WpParagraph *src = from->style_name ? from : fallback;
  p->style_name = src->style_name;
  p->style = src->style;
}

WpPos wp_document_insert_paras(WpDocument *d, WpPos pos, const WpParagraph *src, size_t nsrc)
{
  pos = wp_document_clamp(d, pos);
  if (nsrc == 0) return pos;
  KeepIds keep = keep_ids_for(d, src, nsrc);
  WpPos end;
  if (nsrc == 1) {
    end = (WpPos){ pos.para, para_insert_runs(&d->paras[pos.para], pos.offset, &src[0], &keep) };
  } else {
    WpParagraph at;   /* the paragraph at pos, for unstyled fragment paragraphs to copy */
    at.style_name = d->paras[pos.para].style_name;
    at.style = d->paras[pos.para].style;
    bool head_empty = pos.offset == 0, tail_empty = pos.offset == d->paras[pos.para].len;

    doc_split_para(d, pos);
    WpParagraph *head = &d->paras[pos.para];
    if (head_empty && src[0].style_name) para_take_style(head, &src[0], &at);
    para_insert_runs(head, pos.offset, &src[0], &keep);

    for (size_t i = 1; i + 1 < nsrc; i++) {
      WpParagraph p;
      para_copy(&p, &src[i]);
      para_take_style(&p, &src[i], &at);
      para_filter_comments(&p, &keep);
      doc_insert_para(d, pos.para + i, p);
    }

    WpParagraph *tail = &d->paras[pos.para + nsrc - 1];
    if (tail_empty) para_take_style(tail, &src[nsrc - 1], &at);
    end = (WpPos){ pos.para + nsrc - 1, para_insert_runs(tail, 0, &src[nsrc - 1], &keep) };
  }
  free(keep.ids);
  wp_document_changed(d, pos.para);
  return end;
}

/* ---- public: mutation ---------------------------------------------------- */

WpPos wp_document_insert_text(WpDocument *d, WpPos pos, const char *text, size_t len,
                              const WpTextAttrs *attrs)
{
  pos = wp_document_clamp(d, pos);
  WpPos p = pos;
  size_t i = 0;
  for (;;) {
    size_t j = i;
    while (j < len && text[j] != '\n') j++;
    para_insert_bytes(&d->paras[p.para], p.offset, text + i, j - i, attrs);
    p.offset += j - i;
    if (j >= len) break;
    doc_split_para(d, p);
    p.para++;
    p.offset = 0;
    i = j + 1;
  }
  wp_document_changed(d, pos.para);
  return p;
}

void wp_document_delete_range(WpDocument *d, WpPos a, WpPos b)
{
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  if (wp_pos_eq(a, b)) return;

  if (a.para == b.para) {
    para_delete_bytes(&d->paras[a.para], a.offset, b.offset);
  } else {
    para_delete_bytes(&d->paras[a.para], a.offset, d->paras[a.para].len);
    para_delete_bytes(&d->paras[b.para], 0, b.offset);
    for (size_t i = b.para - 1; i > a.para; i--) doc_remove_para(d, i);
    doc_merge_para(d, a.para);
  }
  wp_document_changed(d, a.para);
}

WpTextAttrs wp_document_attrs_at(const WpDocument *d, WpPos pos)
{
  pos = wp_document_clamp(d, pos);
  return para_inherit_attrs(&d->paras[pos.para], pos.offset);
}

WpTextAttrs wp_document_typing_attrs(const WpDocument *d, WpPos pos)
{
  pos = wp_document_clamp(d, pos);
  const WpParagraph *p = &d->paras[pos.para];
  WpTextAttrs a = para_inherit_attrs(p, pos.offset);
  if (a.comment && !para_comment_continues(p, pos.offset, a.comment)) a.comment = 0;
  return a;
}

WpTextAttrs wp_document_resolve_attrs(const WpDocument *d, const WpTextAttrs *a)
{
  WpTextAttrs r = *a;
  if (r.size_pt <= 0) r.size_pt = d->default_attrs.size_pt;
  if (!r.family) r.family = d->default_attrs.family;
  return r;
}

void wp_document_apply_attrs(WpDocument *d, WpPos a, WpPos b, WpAttrsFn fn, void *user)
{
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }

  for (size_t pi = a.para; pi <= b.para; pi++) {
    WpParagraph *p = &d->paras[pi];
    size_t s = pi == a.para ? a.offset : 0;
    size_t e = pi == b.para ? b.offset : p->len;
    if (s == e) {
      if (p->len == 0) fn(&p->runs[0].attrs, user);   /* empty paragraph inside range */
      continue;
    }
    size_t i0 = para_split_run_at(p, s);
    size_t i1 = para_split_run_at(p, e);
    for (size_t i = i0; i < i1; i++) fn(&p->runs[i].attrs, user);
    para_normalize(p);
  }
  wp_document_changed(d, a.para);
}

void wp_document_apply_para_style(WpDocument *d, WpPos a, WpPos b, WpParaStyleFn fn, void *user)
{
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  for (size_t pi = a.para; pi <= b.para; pi++) fn(&d->paras[pi].style, user);
  wp_document_changed(d, a.para);
}

struct flag_op { uint32_t mask; bool set; };
static void flag_fn(WpTextAttrs *a, void *user)
{
  struct flag_op *op = user;
  if (op->set) a->flags |= op->mask; else a->flags &= ~op->mask;
}

void wp_document_set_flags(WpDocument *d, WpPos a, WpPos b, uint32_t mask, bool set)
{
  struct flag_op op = { mask, set };
  wp_document_apply_attrs(d, a, b, flag_fn, &op);
}

/* ---- public: comments ---------------------------------------------------- */

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

static WpComment *find_comment(WpDocument *d, uint32_t id)
{
  for (size_t i = 0; i < d->ncomments; i++)
    if (d->comments[i].id == id) return &d->comments[i];
  return NULL;
}

uint32_t wp_document_new_comment_record(WpDocument *d, uint32_t parent, const char *author, const char *date, const char *text)
{
  if (d->ncomments == d->comments_cap) {
    d->comments_cap = d->comments_cap ? d->comments_cap * 2 : 8;
    d->comments = realloc(d->comments, d->comments_cap * sizeof *d->comments);
  }
  if (d->next_comment_id == 0) d->next_comment_id = 1;
  WpComment *c = &d->comments[d->ncomments++];
  c->id = d->next_comment_id++;
  c->parent = parent;
  c->author = dup_or_empty(author);
  c->date = dup_or_empty(date);
  c->text = dup_or_empty(text);
  return c->id;
}

static void set_comment_fn(WpTextAttrs *a, void *user) { a->comment = *(uint32_t *)user; }

uint32_t wp_document_add_comment(WpDocument *d, WpPos a, WpPos b, const char *author, const char *date, const char *text)
{
  uint32_t id = wp_document_new_comment_record(d, 0, author, date, text);
  wp_document_apply_attrs(d, a, b, set_comment_fn, &id);   /* notifies */
  return id;
}

uint32_t wp_document_add_reply(WpDocument *d, uint32_t parent, const char *author, const char *date, const char *text)
{
  uint32_t root = wp_document_comment_root(d, parent);
  if (!root) return 0;
  uint32_t id = wp_document_new_comment_record(d, root, author, date, text);
  WpPos a, b;
  wp_document_changed(d, wp_document_comment_range(d, root, &a, &b) ? a.para : 0);
  return id;
}

uint32_t wp_document_comment_root(const WpDocument *d, uint32_t id)
{
  const WpComment *c = wp_document_comment(d, id);
  if (!c) return 0;
  return c->parent && wp_document_comment(d, c->parent) ? c->parent : c->id;
}

const WpComment *wp_document_comment(const WpDocument *d, uint32_t id)
{
  return id ? find_comment((WpDocument *)d, id) : NULL;
}

void wp_document_set_comment_text(WpDocument *d, uint32_t id, const char *text)
{
  WpComment *c = find_comment(d, id);
  if (!c) return;
  free(c->text);
  c->text = dup_or_empty(text);
  WpPos a, b;
  wp_document_changed(d, wp_document_comment_range(d, id, &a, &b) ? a.para : 0);
}

static void drop_comment_record(WpDocument *d, WpComment *c)
{
  free(c->author);
  free(c->date);
  free(c->text);
  size_t i = (size_t)(c - d->comments);
  memmove(d->comments + i, d->comments + i + 1, (d->ncomments - i - 1) * sizeof *d->comments);
  d->ncomments--;
}

void wp_document_remove_comment(WpDocument *d, uint32_t id)
{
  WpComment *c = find_comment(d, id);
  if (!c) return;
  if (c->parent) {   /* a reply: only the record goes */
    WpPos a, b;
    size_t para = wp_document_comment_range(d, c->parent, &a, &b) ? a.para : 0;
    drop_comment_record(d, c);
    wp_document_changed(d, para);
    return;
  }
  for (size_t i = 0; i < d->ncomments;) {   /* its replies first */
    if (d->comments[i].parent == id) drop_comment_record(d, &d->comments[i]);
    else i++;
  }
  c = find_comment(d, id);
  size_t first = d->nparas;
  for (size_t pi = 0; pi < d->nparas; pi++) {
    WpParagraph *p = &d->paras[pi];
    bool hit = false;
    for (size_t r = 0; r < p->nruns; r++)
      if (p->runs[r].attrs.comment == id) { p->runs[r].attrs.comment = 0; hit = true; }
    if (hit) { para_normalize(p); if (first == d->nparas) first = pi; }
  }
  drop_comment_record(d, c);
  wp_document_changed(d, first == d->nparas ? 0 : first);
}

bool wp_document_comment_range(const WpDocument *d, uint32_t id, WpPos *a, WpPos *b)
{
  if (!id) return false;
  const WpComment *c = wp_document_comment(d, id);
  if (c && c->parent) id = c->parent;   /* a reply lives on its parent's text */
  bool found = false;
  for (size_t pi = 0; pi < d->nparas; pi++) {
    const WpParagraph *p = &d->paras[pi];
    size_t off = 0;
    for (size_t r = 0; r < p->nruns; r++) {
      if (p->runs[r].attrs.comment == id) {
        if (!found) { *a = (WpPos){ pi, off }; found = true; }
        *b = (WpPos){ pi, off + p->runs[r].len };
      }
      off += p->runs[r].len;
    }
  }
  return found;
}

uint32_t wp_document_comment_at(const WpDocument *d, WpPos pos)
{
  pos = wp_document_clamp(d, pos);
  const WpParagraph *p = &d->paras[pos.para];
  if (pos.offset < p->len) {
    uint32_t id = p->runs[para_run_at(p, pos.offset)].attrs.comment;
    if (id) return id;
  }
  return para_inherit_attrs(p, pos.offset).comment;
}

WpComment *wp_document_copy_comments(const WpDocument *d, size_t *count)
{
  WpComment *out = malloc(max_sz(d->ncomments, 1) * sizeof *out);
  for (size_t i = 0; i < d->ncomments; i++) {
    out[i].id = d->comments[i].id;
    out[i].parent = d->comments[i].parent;
    out[i].author = strdup(d->comments[i].author);
    out[i].date = strdup(d->comments[i].date);
    out[i].text = strdup(d->comments[i].text);
  }
  *count = d->ncomments;
  return out;
}

void wp_comments_free(WpComment *comments, size_t count)
{
  if (!comments) return;
  for (size_t i = 0; i < count; i++) { free(comments[i].author); free(comments[i].date); free(comments[i].text); }
  free(comments);
}

void wp_document_set_comments(WpDocument *d, const WpComment *src, size_t nsrc)
{
  wp_comments_free(d->comments, d->ncomments);
  d->comments = nsrc ? malloc(nsrc * sizeof *d->comments) : NULL;
  d->comments_cap = d->ncomments = nsrc;
  for (size_t i = 0; i < nsrc; i++) {
    d->comments[i].id = src[i].id;
    d->comments[i].parent = src[i].parent;
    d->comments[i].author = strdup(src[i].author);
    d->comments[i].date = strdup(src[i].date);
    d->comments[i].text = strdup(src[i].text);
    if (src[i].id >= d->next_comment_id) d->next_comment_id = src[i].id + 1;
  }
}

/* ---- public: text in/out ------------------------------------------------- */

char *wp_document_get_text(const WpDocument *d, WpPos a, WpPos b, size_t *out_len)
{
  a = wp_document_clamp(d, a);
  b = wp_document_clamp(d, b);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }

  size_t total = 0;
  for (size_t pi = a.para; pi <= b.para; pi++) {
    const WpParagraph *p = &d->paras[pi];
    size_t s = pi == a.para ? a.offset : 0;
    size_t e = pi == b.para ? b.offset : p->len;
    total += e - s + (pi < b.para ? 1 : 0);
  }
  char *out = malloc(total + 1), *w = out;
  for (size_t pi = a.para; pi <= b.para; pi++) {
    const WpParagraph *p = &d->paras[pi];
    size_t s = pi == a.para ? a.offset : 0;
    size_t e = pi == b.para ? b.offset : p->len;
    memcpy(w, p->text + s, e - s);
    w += e - s;
    if (pi < b.para) *w++ = '\n';
  }
  *w = 0;
  if (out_len) *out_len = total;
  return out;
}

char *wp_document_get_all_text(const WpDocument *d, size_t *out_len)
{
  return wp_document_get_text(d, (WpPos){ 0, 0 }, wp_document_end(d), out_len);
}

void wp_document_load_text(WpDocument *d, const char *text, size_t len)
{
  wp_document_clear(d);
  size_t i = 0, pi = 0;
  for (;;) {
    size_t j = i;
    while (j < len && text[j] != '\n') j++;
    size_t e = j;
    if (e > i && text[e - 1] == '\r') e--;
    if (pi > 0) {
      WpParagraph p;
      const WpNamedStyle *std = wp_document_find_style(d, WP_STYLE_STANDARD);
      para_init(&p, std->name, &std->para, &no_attrs);
      doc_insert_para(d, pi, p);
    }
    para_insert_bytes(&d->paras[pi], 0, text + i, e - i, NULL);
    if (j >= len) break;
    i = j + 1;
    pi++;
  }
  wp_document_changed(d, 0);
}
