#include "layout_pango.h"

#include <pango/pangocairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PS ((double)PANGO_SCALE)

/* One visual line placed on a page. */
typedef struct LineRec {
  size_t para;
  int    line;       /* index within the paragraph's PangoLayout */
  size_t page;
  double x, y;       /* top-left of the line box on the page */
  double origin;     /* left edge of the paragraph's text area (x minus the alignment offset) */
  double baseline;   /* offset from y */
  double height, width;
  size_t start, len; /* byte range within the paragraph */
} LineRec;

typedef struct ParaCache {
  PangoLayout *layout;   /* NULL when stale */
  PangoLayout *label;    /* the list label, for an item; NULL otherwise */
  int    number;         /* the item's number in its list, 0 for a bullet or no list */
  size_t first_line, nlines;
} ParaCache;

typedef struct PageRec { size_t first_line, nlines; } PageRec;

typedef struct WpPangoEngine {
  WpLayoutEngine base;
  WpDocument   *doc;
  WpPageSetup   ps;
  PangoContext *ctx;

  ParaCache *cache; size_t ncache, cache_cap;
  LineRec   *lines; size_t nlines, lines_cap;
  PageRec   *pages; size_t npages, pages_cap;
  bool dirty;
  bool no_comment_marks;   /* see set_comment_marks */
} WpPangoEngine;

static double text_width(WpPangoEngine *pe, const WpParaStyle *st);
static double text_left(WpPangoEngine *pe, const WpParaStyle *st);
static double box_pad(const WpParaStyle *st);
static double first_indent(const WpParaStyle *st);
static double layout_left(WpPangoEngine *pe, const WpParaStyle *st);

/* ---- cache management ---------------------------------------------------- */

static void drop_layouts(WpPangoEngine *pe, size_t from)
{
  for (size_t i = from; i < pe->ncache; i++) {
    g_clear_object(&pe->cache[i].layout);
    g_clear_object(&pe->cache[i].label);
  }
  pe->dirty = true;
}

static void sync_cache(WpPangoEngine *pe)
{
  size_t n = pe->doc ? pe->doc->nparas : 0;
  if (n > pe->cache_cap) {
    size_t cap = pe->cache_cap ? pe->cache_cap : 16;
    while (cap < n) cap *= 2;
    pe->cache = realloc(pe->cache, cap * sizeof *pe->cache);
    memset(pe->cache + pe->cache_cap, 0, (cap - pe->cache_cap) * sizeof *pe->cache);
    pe->cache_cap = cap;
  }
  if (n < pe->ncache)
    for (size_t i = n; i < pe->ncache; i++) { g_clear_object(&pe->cache[i].layout); g_clear_object(&pe->cache[i].label); }
  pe->ncache = n;
}

static void push_line(WpPangoEngine *pe, LineRec r)
{
  if (pe->nlines == pe->lines_cap) {
    pe->lines_cap = pe->lines_cap ? pe->lines_cap * 2 : 64;
    pe->lines = realloc(pe->lines, pe->lines_cap * sizeof *pe->lines);
  }
  pe->lines[pe->nlines++] = r;
}

static void push_page(WpPangoEngine *pe)
{
  if (pe->npages == pe->pages_cap) {
    pe->pages_cap = pe->pages_cap ? pe->pages_cap * 2 : 8;
    pe->pages = realloc(pe->pages, pe->pages_cap * sizeof *pe->pages);
  }
  pe->pages[pe->npages++] = (PageRec){ pe->nlines, 0 };
}

/* ---- paragraph layout ---------------------------------------------------- */

static void add_attr(PangoAttrList *al, PangoAttribute *a, size_t s, size_t e)
{
  a->start_index = (guint)s;
  a->end_index = (guint)e;
  pango_attr_list_insert(al, a);
}

static PangoFontDescription *base_font(const WpTextAttrs *base)
{
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, base->family);
  pango_font_description_set_size(fd, (int)(base->size_pt * PS));
  if (base->flags & WP_ATTR_BOLD) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
  if (base->flags & WP_ATTR_ITALIC) pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
  return fd;
}

/* The bullet or number of a list item, in the paragraph's base font. */
static PangoLayout *build_label(WpPangoEngine *pe, size_t pi, int number)
{
  const WpParagraph *p = &pe->doc->paras[pi];
  char text[16];
  wp_list_label(p->style.list_kind, p->style.list_level, number, text);
  PangoLayout *l = pango_layout_new(pe->ctx);
  pango_layout_set_text(l, text, -1);
  WpTextAttrs base = wp_document_para_base_attrs(pe->doc, pi);
  PangoFontDescription *fd = base_font(&base);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  return l;
}

static PangoLayout *build_layout(WpPangoEngine *pe, size_t pi)
{
  const WpParagraph *p = &pe->doc->paras[pi];
  PangoLayout *l = pango_layout_new(pe->ctx);
  pango_layout_set_text(l, p->text, (int)p->len);

  /* A hanging first line starts left of the other lines: the layout's
   * left edge is the first line's, and Pango's negative indent pushes the
   * rest in. A positive first-line indent is Pango's positive indent. */
  double fi = first_indent(&p->style);
  double cw = text_width(pe, &p->style) - (fi < 0 ? fi : 0);
  pango_layout_set_width(l, (int)(cw * PS));
  pango_layout_set_indent(l, (int)(fi * PS));
  pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);

  /* Tab stops: the paragraph's own, then the default interval on to the
   * right edge. Positions are from the left indent; the layout's edge is
   * further left by a hanging first line. */
  {
    int n = 0;
    double shift = fi < 0 ? -fi : 0, last = 0;
    PangoTabArray *tabs = pango_tab_array_new(0, FALSE);
    for (int i = 0; i < p->style.ntabs; i++) {
      double pos = p->style.tabs[i].pos_pt;
      if (pos <= last && n > 0) continue;
      PangoTabAlign al = p->style.tabs[i].kind == WP_TAB_CENTER ? PANGO_TAB_CENTER :
                         p->style.tabs[i].kind == WP_TAB_RIGHT ? PANGO_TAB_RIGHT :
                         p->style.tabs[i].kind == WP_TAB_DECIMAL ? PANGO_TAB_DECIMAL : PANGO_TAB_LEFT;
      pango_tab_array_resize(tabs, n + 1);
      pango_tab_array_set_tab(tabs, n, al, (int)((pos + shift) * PS));
      if (al == PANGO_TAB_DECIMAL) pango_tab_array_set_decimal_point(tabs, n, '.');
      n++;
      last = pos;
    }
    for (double pos = floor(last / WP_TAB_DEFAULT_PT + 1) * WP_TAB_DEFAULT_PT; pos < cw && n < 64; pos += WP_TAB_DEFAULT_PT) {
      pango_tab_array_resize(tabs, n + 1);
      pango_tab_array_set_tab(tabs, n, PANGO_TAB_LEFT, (int)((pos + shift) * PS));
      n++;
    }
    pango_layout_set_tabs(l, tabs);
    pango_tab_array_free(tabs);
  }

  switch (p->style.align) {
  case WP_ALIGN_LEFT:    pango_layout_set_alignment(l, PANGO_ALIGN_LEFT); break;
  case WP_ALIGN_CENTER:  pango_layout_set_alignment(l, PANGO_ALIGN_CENTER); break;
  case WP_ALIGN_RIGHT:   pango_layout_set_alignment(l, PANGO_ALIGN_RIGHT); break;
  case WP_ALIGN_JUSTIFY: pango_layout_set_alignment(l, PANGO_ALIGN_LEFT);
                         pango_layout_set_justify(l, TRUE); break;
  }
  /* Line spacing is applied in paginate(), not here: Pango's own factor
   * moves lines but still reports each line's natural height, and it leaves
   * the first line alone, whereas word processors grow every line. */

  /* The paragraph's named style supplies the base font; runs layer on top. */
  WpTextAttrs base = wp_document_para_base_attrs(pe->doc, pi);
  PangoFontDescription *fd = base_font(&base);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);

  PangoAttrList *al = pango_attr_list_new();
  if ((base.flags & WP_ATTR_UNDERLINE) && p->len > 0)
    add_attr(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), 0, p->len);
  size_t off = 0;
  for (size_t i = 0; i < p->nruns; i++) {
    const WpRun *r = &p->runs[i];
    size_t s = off, e = off + r->len;
    off = e;
    if (r->len == 0) continue;
    if (r->attrs.flags & WP_ATTR_BOLD)      add_attr(al, pango_attr_weight_new(PANGO_WEIGHT_BOLD), s, e);
    if (r->attrs.flags & WP_ATTR_ITALIC)    add_attr(al, pango_attr_style_new(PANGO_STYLE_ITALIC), s, e);
    if (r->attrs.flags & WP_ATTR_UNDERLINE) add_attr(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), s, e);
    if (r->attrs.size_pt > 0)               add_attr(al, pango_attr_size_new((int)(r->attrs.size_pt * PS)), s, e);
    if (r->attrs.family)                    add_attr(al, pango_attr_family_new(r->attrs.family), s, e);
    if (r->attrs.comment && !pe->no_comment_marks && wp_document_comment(pe->doc, r->attrs.comment)) {
      /* Commented text sits on a translucent yellow so the selection still shows through. */
      add_attr(al, pango_attr_background_new(0xffff, 0xe8e8, 0x8a8a), s, e);
      add_attr(al, pango_attr_background_alpha_new(0x7fff), s, e);
    }
  }
  pango_layout_set_attributes(l, al);
  pango_attr_list_unref(al);
  return l;
}

/* ---- paragraph geometry -------------------------------------------------- */

/* A paragraph's text sits inside its indents, and inside the box padding and
 * rule when it has a background or border. */
static double box_pad(const WpParaStyle *st) { return wp_para_style_boxed(st) ? st->padding_pt : 0; }

static double text_left(WpPangoEngine *pe, const WpParaStyle *st)
{
  return pe->ps.margin_left + st->indent_left_pt + st->border_left_pt + box_pad(st);
}

static double text_width(WpPangoEngine *pe, const WpParaStyle *st)
{
  double w = pe->ps.width - pe->ps.margin_left - pe->ps.margin_right -
             st->indent_left_pt - st->indent_right_pt - st->border_left_pt - 2 * box_pad(st);
  return w > 36 ? w : 36;
}

/* The first line's inset from the text's left edge; a list item's label
 * takes that place, so it has none. Clamped so some text is always left. */
static double first_indent(const WpParaStyle *st)
{
  if (st->list_kind != WP_LIST_NONE) return 0;
  double fi = st->indent_first_pt;
  if (fi < -(st->indent_left_pt)) fi = -(st->indent_left_pt);   /* no further left than the margin */
  return fi;
}

/* Where the PangoLayout's left edge sits: the text's, or a hanging first line's. */
static double layout_left(WpPangoEngine *pe, const WpParaStyle *st)
{
  double fi = first_indent(st);
  return text_left(pe, st) + (fi < 0 ? fi : 0);
}

/* ---- pagination ---------------------------------------------------------- */

/* Proportional line spacing: 1.0 single, 2.0 double. */
static double spacing_factor(const WpParagraph *p)
{
  return p->style.line_spacing > 0 ? p->style.line_spacing : 1.0;
}

static void paginate(WpPangoEngine *pe)
{
  pe->nlines = 0;
  pe->npages = 0;

  const double top = pe->ps.margin_top;
  const double bottom = pe->ps.height - pe->ps.margin_bottom;
  push_page(pe);
  size_t page = 0;
  double y = top;
  bool page_has_lines = false;

  for (size_t pi = 0; pi < pe->ncache; pi++) {
    const WpParagraph *para = &pe->doc->paras[pi];
    ParaCache *c = &pe->cache[pi];
    c->first_line = pe->nlines;

    const double factor = spacing_factor(para);
    const double pad = box_pad(&para->style), origin = layout_left(pe, &para->style);
    if (page_has_lines) y += para->style.space_before_pt;

    /* Keep-with-next: if this paragraph plus the first line of the next one
     * will not fit, start a new page before it rather than stranding it. */
    if (page_has_lines && wp_document_style_of(pe->doc, pi)->keep_with_next && pi + 1 < pe->ncache) {
      PangoRectangle ext;
      pango_layout_get_extents(c->layout, NULL, &ext);
      PangoLayoutIter *nit = pango_layout_get_iter(pe->cache[pi + 1].layout);
      int ny0, ny1;
      pango_layout_iter_get_line_yrange(nit, &ny0, &ny1);
      pango_layout_iter_free(nit);
      double need = ext.height / PS * factor + 2 * pad + para->style.space_after_pt +
                    pe->doc->paras[pi + 1].style.space_before_pt +
                    (ny1 - ny0) / PS * spacing_factor(&pe->doc->paras[pi + 1]);
      if (y + need > bottom) {
        push_page(pe);
        page++;
        y = top;
        page_has_lines = false;
      }
    }

    PangoLayoutIter *it = pango_layout_get_iter(c->layout);
    int li = 0;
    do {
      PangoRectangle logical;
      int y0, y1;
      pango_layout_iter_get_line_extents(it, NULL, &logical);
      pango_layout_iter_get_line_yrange(it, &y0, &y1);
      int base = pango_layout_iter_get_baseline(it);
      PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
      /* Every line box grows by the factor, with the extra space above the
       * glyphs, as Word and LibreOffice lay out proportional spacing. */
      double natural = (y1 - y0) / PS;
      double h = natural * factor;
      double lead = li == 0 ? pad : 0;   /* the box's top padding travels with the first line */

      if (page_has_lines && y + lead + h > bottom) {
        push_page(pe);
        page++;
        y = top;
        page_has_lines = false;
      }
      y += lead;

      push_line(pe, (LineRec){
        .para = pi, .line = li, .page = page,
        .x = origin + logical.x / PS, .y = y, .origin = origin,
        .baseline = (base - y0) / PS + (h - natural), .height = h, .width = logical.width / PS,
        .start = (size_t)pango_layout_line_get_start_index(line),
        .len = (size_t)pango_layout_line_get_length(line),
      });
      pe->pages[page].nlines++;
      y += h;
      page_has_lines = true;
      li++;
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);

    y += pad + para->style.space_after_pt;
    c->nlines = pe->nlines - c->first_line;
  }
}

static void ensure(WpPangoEngine *pe)
{
  if (pe->doc && memcmp(&pe->ps, &pe->doc->page, sizeof pe->ps) != 0) {
    pe->ps = pe->doc->page;   /* page geometry changed: every layout width is stale */
    drop_layouts(pe, 0);
  }
  if (!pe->dirty) return;
  sync_cache(pe);
  /* Numbers depend only on the paragraphs before, and invalidation drops
   * every cache entry from the edited paragraph on, so a cached label is
   * always current. */
  int *numbers = pe->ncache ? malloc(pe->ncache * sizeof *numbers) : NULL;
  if (numbers) wp_document_list_numbers(pe->doc, numbers);
  for (size_t i = 0; i < pe->ncache; i++) {
    if (pe->cache[i].layout) continue;
    pe->cache[i].layout = build_layout(pe, i);
    pe->cache[i].number = numbers ? numbers[i] : 0;
    if (pe->doc->paras[i].style.list_kind != WP_LIST_NONE) pe->cache[i].label = build_label(pe, i, pe->cache[i].number);
  }
  free(numbers);
  paginate(pe);
  pe->dirty = false;
}

/* ---- geometry helpers ---------------------------------------------------- */

static PangoLayoutLine *get_line(WpPangoEngine *pe, const LineRec *r)
{
  return pango_layout_get_line_readonly(pe->cache[r->para].layout, r->line);
}

static const LineRec *line_for_pos(WpPangoEngine *pe, WpPos pos, size_t *gidx)
{
  if (pos.para >= pe->ncache) return NULL;
  const ParaCache *c = &pe->cache[pos.para];
  for (size_t k = 0; k < c->nlines; k++) {
    const LineRec *r = &pe->lines[c->first_line + k];
    if (pos.offset < r->start + r->len || k == c->nlines - 1) {
      if (gidx) *gidx = c->first_line + k;
      return r;
    }
  }
  return NULL;
}

static bool is_last_line(WpPangoEngine *pe, const LineRec *r)
{
  return (size_t)r->line == pe->cache[r->para].nlines - 1;
}

static WpPos line_x_to_pos(WpPangoEngine *pe, const LineRec *r, double x)
{
  const WpParagraph *p = &pe->doc->paras[r->para];
  int index = 0, trailing = 0;
  pango_layout_line_x_to_index(get_line(pe, r), (int)((x - r->x) * PS), &index, &trailing);

  size_t off = (size_t)index;
  size_t end = r->start + r->len;
  while (trailing-- > 0 && off < end) off = wp_utf8_next(p->text, p->len, off);
  if (off > end) off = end;
  /* Clicking past the end of a wrapped line lands before its trailing space
   * rather than at the start of the next line. */
  if (off == end && !is_last_line(pe, r) && end > r->start && p->text[end - 1] == ' ')
    off = end - 1;
  return (WpPos){ r->para, off };
}

/* ---- vtable -------------------------------------------------------------- */

static void pe_destroy(WpLayoutEngine *e)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  drop_layouts(pe, 0);
  free(pe->cache);
  free(pe->lines);
  free(pe->pages);
  g_clear_object(&pe->ctx);
  free(pe);
}

static void pe_set_document(WpLayoutEngine *e, WpDocument *doc)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  drop_layouts(pe, 0);
  pe->doc = doc;
}

static void pe_invalidate(WpLayoutEngine *e, size_t first)
{
  drop_layouts((WpPangoEngine *)e, first);
}

static void pe_set_comment_marks(WpLayoutEngine *e, bool shown)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  if (pe->no_comment_marks == !shown) return;
  pe->no_comment_marks = !shown;
  drop_layouts(pe, 0);   /* the marks are in the layouts' attributes */
}

static size_t pe_page_count(WpLayoutEngine *e)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  return pe->npages;
}

static void pe_render_page(WpLayoutEngine *e, size_t page, cairo_t *cr)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  if (page >= pe->npages) return;
  const PageRec *pg = &pe->pages[page];

  /* Boxes first: one per stretch of a boxed paragraph's lines on this page,
   * padded above its first line and below its last. */
  for (size_t i = 0; i < pg->nlines;) {
    const LineRec *first = &pe->lines[pg->first_line + i];
    size_t j = i;
    while (j + 1 < pg->nlines && pe->lines[pg->first_line + j + 1].para == first->para) j++;
    const LineRec *last = &pe->lines[pg->first_line + j];
    const WpParaStyle *st = &pe->doc->paras[first->para].style;
    if (wp_para_style_boxed(st)) {
      double x = pe->ps.margin_left + st->indent_left_pt;
      double w = pe->ps.width - pe->ps.margin_right - st->indent_right_pt - x;
      double y0 = first->y - (first->line == 0 ? st->padding_pt : 0);
      double y1 = last->y + last->height + (is_last_line(pe, last) ? st->padding_pt : 0);
      cairo_save(cr);
      if (wp_color_set(st->background)) {
        cairo_set_source_rgb(cr, wp_color_r(st->background), wp_color_g(st->background), wp_color_b(st->background));
        cairo_rectangle(cr, x, y0, w, y1 - y0);
        cairo_fill(cr);
      }
      if (st->border_left_pt > 0) {
        uint32_t c = wp_color_set(st->border_color) ? st->border_color : WP_COLOR(0, 0, 0);
        cairo_set_source_rgb(cr, wp_color_r(c), wp_color_g(c), wp_color_b(c));
        cairo_rectangle(cr, x, y0, st->border_left_pt, y1 - y0);
        cairo_fill(cr);
      }
      cairo_restore(cr);
    }
    i = j + 1;
  }

  for (size_t i = 0; i < pg->nlines; i++) {
    const LineRec *r = &pe->lines[pg->first_line + i];
    PangoLayout *label = pe->cache[r->para].label;
    if (label && r->line == 0) {   /* the list label hangs to the left of the first line */
      cairo_move_to(cr, r->origin - WP_LIST_HANG_PT, r->y + r->baseline);
      pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(label, 0));
    }
    cairo_move_to(cr, r->x, r->y + r->baseline);
    pango_cairo_show_layout_line(cr, get_line(pe, r));
  }
}

static bool pe_hit_test(WpLayoutEngine *e, size_t page, double x, double y, WpPos *out)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  if (page >= pe->npages || pe->pages[page].nlines == 0) return false;
  const PageRec *pg = &pe->pages[page];
  const LineRec *best = &pe->lines[pg->first_line];
  for (size_t i = 0; i < pg->nlines; i++) {
    const LineRec *r = &pe->lines[pg->first_line + i];
    if (r->y <= y) best = r; else break;
  }
  *out = line_x_to_pos(pe, best, x);
  return true;
}

static bool pe_caret_rect(WpLayoutEngine *e, WpPos pos, size_t *page, WpRect *rect)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  const LineRec *r = line_for_pos(pe, pos, NULL);
  if (!r) return false;
  int xp = 0;
  pango_layout_line_index_to_x(get_line(pe, r), (int)pos.offset, FALSE, &xp);
  *page = r->page;
  *rect = (WpRect){ r->x + xp / PS, r->y, 1.0, r->height };
  return true;
}

static bool pe_move_vertical(WpLayoutEngine *e, WpPos pos, int dir, double *x_goal, WpPos *out)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  size_t g = 0;
  const LineRec *r = line_for_pos(pe, pos, &g);
  if (!r) return false;
  if (*x_goal < 0) {
    size_t pg; WpRect cr;
    if (pe_caret_rect(e, pos, &pg, &cr)) *x_goal = cr.x;
  }
  if (dir < 0 && g == 0) return false;
  if (dir > 0 && g + 1 >= pe->nlines) return false;
  *out = line_x_to_pos(pe, &pe->lines[dir < 0 ? g - 1 : g + 1], *x_goal);
  return true;
}

static bool pe_line_bounds(WpLayoutEngine *e, WpPos pos, WpPos *start, WpPos *end)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  const LineRec *r = line_for_pos(pe, pos, NULL);
  if (!r) return false;
  const WpParagraph *p = &pe->doc->paras[r->para];
  size_t s = r->start, en = r->start + r->len;
  if (!is_last_line(pe, r) && en > s && p->text[en - 1] == ' ') en--;
  *start = (WpPos){ r->para, s };
  *end   = (WpPos){ r->para, en };
  return true;
}

static void pe_selection_rects(WpLayoutEngine *e, WpPos a, WpPos b, size_t page, WpRectFn fn, void *user)
{
  WpPangoEngine *pe = (WpPangoEngine *)e;
  ensure(pe);
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  if (page >= pe->npages) return;
  const PageRec *pg = &pe->pages[page];

  for (size_t i = 0; i < pg->nlines; i++) {
    const LineRec *r = &pe->lines[pg->first_line + i];
    if (r->para < a.para || r->para > b.para) continue;

    size_t sel_s = r->para == a.para ? a.offset : 0;
    size_t sel_e = r->para == b.para ? b.offset : SIZE_MAX;   /* SIZE_MAX: through the paragraph break */
    size_t ls = r->start, le = ls + r->len;
    size_t s = sel_s > ls ? sel_s : ls;
    size_t en = sel_e < le ? sel_e : le;

    if (s < en) {
      int *ranges = NULL, n = 0;
      pango_layout_line_get_x_ranges(get_line(pe, r), (int)s, (int)en, &ranges, &n);
      for (int k = 0; k < n; k++) {
        WpRect rect = { r->origin + ranges[2 * k] / PS, r->y,
                        (ranges[2 * k + 1] - ranges[2 * k]) / PS, r->height };
        fn(&rect, user);
      }
      g_free(ranges);
    }
    if (sel_e == SIZE_MAX && is_last_line(pe, r)) {
      WpRect rect = { r->x + r->width, r->y, 5.0, r->height };   /* paragraph-break marker */
      fn(&rect, user);
    }
  }
}

static const WpLayoutEngineVTable pango_vtable = {
  .name            = "pango",
  .destroy         = pe_destroy,
  .set_document    = pe_set_document,
  .invalidate      = pe_invalidate,
  .page_count      = pe_page_count,
  .render_page     = pe_render_page,
  .hit_test        = pe_hit_test,
  .caret_rect      = pe_caret_rect,
  .move_vertical   = pe_move_vertical,
  .line_bounds     = pe_line_bounds,
  .selection_rects = pe_selection_rects,
  .set_comment_marks = pe_set_comment_marks,
};

WpLayoutEngine *wp_layout_pango_new(void)
{
  WpPangoEngine *pe = calloc(1, sizeof *pe);
  pe->base.vt = &pango_vtable;

  /* 72 dpi makes Pango units equal points, and unhinted metrics keep the
   * layout identical at every zoom level. */
  pe->ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
  pango_cairo_context_set_resolution(pe->ctx, 72.0);
  cairo_font_options_t *fo = cairo_font_options_create();
  cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
  pango_cairo_context_set_font_options(pe->ctx, fo);
  cairo_font_options_destroy(fo);

  pe->dirty = true;
  return &pe->base;
}
