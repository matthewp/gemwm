#include "odt.h"

#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>

#include "io/zip.h"

#define NS_OFFICE   "urn:oasis:names:tc:opendocument:xmlns:office:1.0"
#define NS_STYLE    "urn:oasis:names:tc:opendocument:xmlns:style:1.0"
#define NS_TEXT     "urn:oasis:names:tc:opendocument:xmlns:text:1.0"
#define NS_TABLE    "urn:oasis:names:tc:opendocument:xmlns:table:1.0"
#define NS_DRAW     "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
#define NS_FO       "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
#define NS_SVG      "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"
#define NS_MANIFEST "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
#define NS_LOEXT    "urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0"

#define MIMETYPE "application/vnd.oasis.opendocument.text"

/* ========================================================================= */
/* Reading                                                                    */
/* ========================================================================= */

/* A set of formatting properties, each with a "present" bit so that style
 * inheritance can be resolved by merging. */
enum {
  H_BOLD = 1, H_ITALIC = 2, H_UNDERLINE = 4, H_SIZE = 8, H_SIZE_PCT = 16, H_FAMILY = 32,
  H_ALIGN = 64, H_BEFORE = 128, H_AFTER = 256, H_LINESP = 512, H_KEEP = 1024,
  H_MLEFT = 2048, H_MRIGHT = 4096, H_PADDING = 8192, H_BORDER = 16384, H_BG = 32768,
  H_TINDENT = 65536, H_TABS = 131072,
};

typedef struct Props {
  unsigned    has;
  bool        bold, italic, underline;
  double      size_pt, size_pct;
  const char *family;        /* interned */
  WpAlign     align;
  double      before, after, linesp;
  bool        keep;
  double      mleft, mright, padding;
  double      border_w;        /* 0 = none */
  uint32_t    border_color, bg; /* WP_COLOR, 0 = none */
  double      tindent;         /* fo:text-indent */
  int         ntabs;           /* a style's tab stops replace its parent's */
  WpTabStop   tabs[WP_MAX_TABS];
} Props;

typedef struct Style {
  char  *name, *family, *parent, *display;
  int    outline;          /* style:default-outline-level, 0 if none */
  bool   named;            /* from office:styles rather than automatic-styles */
  const char *mapped;      /* our named style this maps to (interned), or NULL */
  Props  own, resolved;
  bool   done, busy;
} Style;

typedef struct OpenComment { char *name; uint32_t id; } OpenComment;

/* A text:list-style: the kind of label at each level, and where the level
 * puts its text (from label-alignment's margin-left, or the older
 * space-before plus min-label-width). */
typedef struct ListStyle {
  WpListKind kind[WP_LIST_MAX_LEVEL + 1];
  bool       has_margin[WP_LIST_MAX_LEVEL + 1];
  double     margin[WP_LIST_MAX_LEVEL + 1];
} ListStyle;

/* The list a paragraph is in, while walking the body. */
typedef struct ListInfo {
  WpListKind kind;
  int        level;
  bool       item;      /* first paragraph of a list item: gets the label */
  bool       has_margin;
  double     margin;    /* the level's text position, when the list style gives one */
} ListInfo;

typedef struct Reader {
  WpDocument *doc;
  GHashTable *styles;      /* "family/name" -> Style* */
  GHashTable *fonts;       /* style:font-face name -> interned svg:font-family */
  Style      *default_para, *default_text;
  size_t      nparas;      /* paragraphs emitted so far */
  bool        last_space;  /* whitespace collapsing state within the paragraph */
  GHashTable *ends;        /* office:name of every office:annotation-end in the body */
  GHashTable *names;       /* office:name -> comment id, for reply links */
  GArray     *open;        /* OpenComment: ranged comments whose end has not been seen */
  uint32_t    point;       /* a comment without a range, waiting for the next character */
  uint32_t    last_thread; /* thread of the most recent annotation, while no text has followed it */
  GHashTable *list_styles; /* text:list-style name -> ListStyle* */
  /* Open text:list elements, outermost first: the style each uses (a nested
   * list without one inherits) and whether the next paragraph in its
   * current item is the item's first. */
  const ListStyle *list_style[WP_LIST_MAX_LEVEL + 2];
  bool        list_first[WP_LIST_MAX_LEVEL + 2];
  int         list_depth;
} Reader;

/* ---- xml helpers -------------------------------------------------------- */

static bool is_el(const xmlNode *n, const char *ns, const char *name)
{
  return n->type == XML_ELEMENT_NODE && n->ns && n->ns->href &&
         strcmp((const char *)n->ns->href, ns) == 0 && strcmp((const char *)n->name, name) == 0;
}

static char *attr(const xmlNode *n, const char *ns, const char *name)
{
  xmlChar *v = xmlGetNsProp(n, BAD_CAST name, BAD_CAST ns);
  if (!v) return NULL;
  char *s = g_strdup((const char *)v);
  xmlFree(v);
  return s;
}

static xmlNode *child(const xmlNode *n, const char *ns, const char *name)
{
  for (xmlNode *c = n ? n->children : NULL; c; c = c->next)
    if (is_el(c, ns, name)) return c;
  return NULL;
}

/* "12pt", "0.5in", "1.27cm" -> points. Returns false if not a length. */
static bool parse_length(const char *s, double *out)
{
  if (!s) return false;
  char *end;
  double v = g_ascii_strtod(s, &end);
  if (end == s) return false;
  while (*end == ' ') end++;
  double k;
  if (!strcmp(end, "pt")) k = 1;
  else if (!strcmp(end, "in")) k = 72;
  else if (!strcmp(end, "cm")) k = 72 / 2.54;
  else if (!strcmp(end, "mm")) k = 72 / 25.4;
  else if (!strcmp(end, "pc")) k = 12;
  else if (!strcmp(end, "px")) k = 0.75;
  else if (!*end) k = 1;
  else return false;
  *out = v * k;
  return true;
}

static const char *font_family_of(Reader *r, const char *font_name)
{
  const char *fam = g_hash_table_lookup(r->fonts, font_name);
  return fam ? fam : wp_intern(font_name);
}

/* Strip quotes from an svg:font-family value like "'Liberation Serif'" or
 * "Arial, sans-serif" (keep the first name). */
static const char *clean_family(const char *s)
{
  g_autofree char *t = g_strdup(s);
  char *comma = strchr(t, ',');
  if (comma) *comma = 0;
  g_strstrip(t);
  size_t n = strlen(t);
  if (n >= 2 && (t[0] == '\'' || t[0] == '"') && t[n - 1] == t[0]) { t[n - 1] = 0; memmove(t, t + 1, n - 1); }
  return wp_intern(t);
}

/* "#rrggbb" -> WP_COLOR; "transparent" or anything else -> 0. */
static uint32_t parse_color(const char *s)
{
  if (!s || s[0] != '#' || strlen(s) != 7) return 0;
  char *end;
  unsigned long v = strtoul(s + 1, &end, 16);
  if (*end) return 0;
  return WP_COLOR((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

/* fo:border value: "0.5pt solid #3584e4", or "none". */
static void parse_border(const char *s, double *width, uint32_t *color)
{
  *width = 0;
  *color = 0;
  if (!s || !strcmp(s, "none")) return;
  g_auto(GStrv) words = g_strsplit(s, " ", -1);
  for (char **w = words; *w; w++) {
    double v;
    if (**w == '#') *color = parse_color(*w);
    else if (parse_length(*w, &v) && v > 0) *width = v;
  }
}

/* ---- style parsing ------------------------------------------------------ */

static void parse_props(Reader *r, const xmlNode *style_node, Props *p)
{
  for (xmlNode *c = style_node->children; c; c = c->next) {
    if (is_el(c, NS_STYLE, "text-properties")) {
      g_autofree char *w = attr(c, NS_FO, "font-weight");
      if (w) { p->bold = !strcmp(w, "bold") || atoi(w) >= 600; p->has |= H_BOLD; }
      g_autofree char *st = attr(c, NS_FO, "font-style");
      if (st) { p->italic = !strcmp(st, "italic") || !strcmp(st, "oblique"); p->has |= H_ITALIC; }
      g_autofree char *ul = attr(c, NS_STYLE, "text-underline-style");
      if (ul) { p->underline = strcmp(ul, "none") != 0; p->has |= H_UNDERLINE; }
      g_autofree char *sz = attr(c, NS_FO, "font-size");
      if (sz) {
        char *end; double v = g_ascii_strtod(sz, &end);
        if (*end == '%') { p->size_pct = v / 100.0; p->has |= H_SIZE_PCT; p->has &= ~H_SIZE; }
        else if (parse_length(sz, &v)) { p->size_pt = v; p->has |= H_SIZE; p->has &= ~H_SIZE_PCT; }
      }
      g_autofree char *fn = attr(c, NS_STYLE, "font-name");
      g_autofree char *ff = attr(c, NS_FO, "font-family");
      if (fn) { p->family = font_family_of(r, fn); p->has |= H_FAMILY; }
      else if (ff) { p->family = clean_family(ff); p->has |= H_FAMILY; }
    } else if (is_el(c, NS_STYLE, "paragraph-properties")) {
      g_autofree char *al = attr(c, NS_FO, "text-align");
      if (al) {
        p->has |= H_ALIGN;
        if (!strcmp(al, "center")) p->align = WP_ALIGN_CENTER;
        else if (!strcmp(al, "end") || !strcmp(al, "right")) p->align = WP_ALIGN_RIGHT;
        else if (!strcmp(al, "justify")) p->align = WP_ALIGN_JUSTIFY;
        else p->align = WP_ALIGN_LEFT;
      }
      g_autofree char *mt = attr(c, NS_FO, "margin-top");
      if (mt && parse_length(mt, &p->before)) p->has |= H_BEFORE;
      g_autofree char *mb = attr(c, NS_FO, "margin-bottom");
      if (mb && parse_length(mb, &p->after)) p->has |= H_AFTER;
      g_autofree char *lh = attr(c, NS_FO, "line-height");
      if (lh) {
        char *end; double v = g_ascii_strtod(lh, &end);
        if (*end == '%') { p->linesp = v / 100.0; p->has |= H_LINESP; }
        else if (!strcmp(lh, "normal")) { p->linesp = 1.0; p->has |= H_LINESP; }
      }
      g_autofree char *kw = attr(c, NS_FO, "keep-with-next");
      if (kw) { p->keep = !strcmp(kw, "always"); p->has |= H_KEEP; }
      g_autofree char *ml = attr(c, NS_FO, "margin-left");
      if (ml && parse_length(ml, &p->mleft)) p->has |= H_MLEFT;
      g_autofree char *mr = attr(c, NS_FO, "margin-right");
      if (mr && parse_length(mr, &p->mright)) p->has |= H_MRIGHT;
      g_autofree char *pd = attr(c, NS_FO, "padding");
      g_autofree char *pl = attr(c, NS_FO, "padding-left");
      if ((pd && parse_length(pd, &p->padding)) || (pl && parse_length(pl, &p->padding))) p->has |= H_PADDING;
      /* Only a rule down the left edge is kept; a full border becomes one. */
      g_autofree char *bl = attr(c, NS_FO, "border-left");
      g_autofree char *ba = attr(c, NS_FO, "border");
      if (bl || ba) { parse_border(bl ? bl : ba, &p->border_w, &p->border_color); p->has |= H_BORDER; }
      g_autofree char *bg = attr(c, NS_FO, "background-color");
      if (bg) { p->bg = parse_color(bg); p->has |= H_BG; }
      g_autofree char *ti = attr(c, NS_FO, "text-indent");
      if (ti && parse_length(ti, &p->tindent)) p->has |= H_TINDENT;
      xmlNode *stops = child(c, NS_STYLE, "tab-stops");
      if (stops) {
        p->has |= H_TABS;
        p->ntabs = 0;
        memset(p->tabs, 0, sizeof p->tabs);
        for (xmlNode *t = stops->children; t; t = t->next) {
          if (!is_el(t, NS_STYLE, "tab-stop")) continue;
          g_autofree char *pos = attr(t, NS_STYLE, "position");
          g_autofree char *type = attr(t, NS_STYLE, "type");
          double v;
          if (!pos || !parse_length(pos, &v)) continue;
          WpTabKind k = WP_TAB_LEFT;
          if (type && !strcmp(type, "center")) k = WP_TAB_CENTER;
          else if (type && !strcmp(type, "right")) k = WP_TAB_RIGHT;
          else if (type && !strcmp(type, "char")) k = WP_TAB_DECIMAL;
          WpParaStyle tmp = { 0 };
          tmp.ntabs = p->ntabs;
          memcpy(tmp.tabs, p->tabs, sizeof tmp.tabs);
          wp_para_style_add_tab(&tmp, (float)v, k);
          p->ntabs = tmp.ntabs;
          memcpy(p->tabs, tmp.tabs, sizeof p->tabs);
        }
      }
    }
  }
}

static Props merge(const Props *base, const Props *over)
{
  Props r = *base;
  if (over->has & H_BOLD)      { r.bold = over->bold;           r.has |= H_BOLD; }
  if (over->has & H_ITALIC)    { r.italic = over->italic;       r.has |= H_ITALIC; }
  if (over->has & H_UNDERLINE) { r.underline = over->underline; r.has |= H_UNDERLINE; }
  if (over->has & H_FAMILY)    { r.family = over->family;       r.has |= H_FAMILY; }
  if (over->has & H_ALIGN)     { r.align = over->align;         r.has |= H_ALIGN; }
  if (over->has & H_BEFORE)    { r.before = over->before;       r.has |= H_BEFORE; }
  if (over->has & H_AFTER)     { r.after = over->after;         r.has |= H_AFTER; }
  if (over->has & H_LINESP)    { r.linesp = over->linesp;       r.has |= H_LINESP; }
  if (over->has & H_KEEP)      { r.keep = over->keep;           r.has |= H_KEEP; }
  if (over->has & H_MLEFT)     { r.mleft = over->mleft;         r.has |= H_MLEFT; }
  if (over->has & H_MRIGHT)    { r.mright = over->mright;       r.has |= H_MRIGHT; }
  if (over->has & H_PADDING)   { r.padding = over->padding;     r.has |= H_PADDING; }
  if (over->has & H_BORDER)    { r.border_w = over->border_w; r.border_color = over->border_color; r.has |= H_BORDER; }
  if (over->has & H_BG)        { r.bg = over->bg;               r.has |= H_BG; }
  if (over->has & H_TINDENT)   { r.tindent = over->tindent;     r.has |= H_TINDENT; }
  if (over->has & H_TABS)      { r.ntabs = over->ntabs; memcpy(r.tabs, over->tabs, sizeof r.tabs); r.has |= H_TABS; }
  if (over->has & H_SIZE)      { r.size_pt = over->size_pt;     r.has |= H_SIZE; }
  else if (over->has & H_SIZE_PCT) {
    r.size_pt = ((base->has & H_SIZE) ? base->size_pt : 12.0) * over->size_pct;
    r.has |= H_SIZE;
  }
  return r;
}

static Style *lookup_style(Reader *r, const char *family, const char *name)
{
  if (!name) return NULL;
  g_autofree char *key = g_strdup_printf("%s/%s", family, name);
  return g_hash_table_lookup(r->styles, key);
}

static const Props *resolve(Reader *r, Style *s)
{
  static const Props empty = { 0 };
  if (!s) return &empty;
  if (s->done) return &s->resolved;
  if (s->busy) return &empty;   /* inheritance cycle */
  s->busy = true;

  Style *parent = lookup_style(r, s->family, s->parent);
  if (!parent) {
    if (!strcmp(s->family, "paragraph") && s != r->default_para) parent = r->default_para;
    else if (!strcmp(s->family, "text") && s != r->default_text) parent = r->default_text;
  }
  s->resolved = merge(resolve(r, parent), &s->own);
  s->busy = false;
  s->done = true;
  return &s->resolved;
}

static void free_style(gpointer p)
{
  Style *s = p;
  g_free(s->name);
  g_free(s->family);
  g_free(s->parent);
  g_free(s->display);
  g_free(s);
}

/* ODF encodes spaces in style names as _20_; display-name carries the real one. */
static char *decode_style_name(const char *name)
{
  GString *out = g_string_new(NULL);
  for (const char *c = name; *c; c++) {
    if (g_str_has_prefix(c, "_20_")) { g_string_append_c(out, ' '); c += 3; }
    else g_string_append_c(out, *c);
  }
  return g_string_free(out, FALSE);
}

static void collect_fonts(Reader *r, const xmlNode *root)
{
  xmlNode *decls = child(root, NS_OFFICE, "font-face-decls");
  for (xmlNode *c = decls ? decls->children : NULL; c; c = c->next) {
    if (!is_el(c, NS_STYLE, "font-face")) continue;
    g_autofree char *name = attr(c, NS_STYLE, "name");
    g_autofree char *fam = attr(c, NS_SVG, "font-family");
    if (name) g_hash_table_insert(r->fonts, g_strdup(name), (gpointer)clean_family(fam ? fam : name));
  }
}

static void collect_styles(Reader *r, const xmlNode *container, bool named)
{
  for (xmlNode *c = container ? container->children : NULL; c; c = c->next) {
    if (is_el(c, NS_STYLE, "default-style")) {
      g_autofree char *fam = attr(c, NS_STYLE, "family");
      if (!fam) continue;
      Style *s = g_new0(Style, 1);
      s->family = g_strdup(fam);
      parse_props(r, c, &s->own);
      if (!strcmp(fam, "paragraph")) { g_clear_pointer(&r->default_para, free_style); r->default_para = s; }
      else if (!strcmp(fam, "text")) { g_clear_pointer(&r->default_text, free_style); r->default_text = s; }
      else free_style(s);
    } else if (is_el(c, NS_STYLE, "style")) {
      g_autofree char *name = attr(c, NS_STYLE, "name");
      g_autofree char *fam = attr(c, NS_STYLE, "family");
      if (!name || !fam) continue;
      Style *s = g_new0(Style, 1);
      s->name = g_strdup(name);
      s->family = g_strdup(fam);
      s->parent = attr(c, NS_STYLE, "parent-style-name");
      s->display = attr(c, NS_STYLE, "display-name");
      if (!s->display) s->display = decode_style_name(name);
      g_autofree char *ol = attr(c, NS_STYLE, "default-outline-level");
      s->outline = ol ? atoi(ol) : 0;
      s->named = named;
      parse_props(r, c, &s->own);
      g_hash_table_insert(r->styles, g_strdup_printf("%s/%s", fam, name), s);
    }
  }
}

static void collect_list_styles(Reader *r, const xmlNode *container)
{
  for (xmlNode *c = container ? container->children : NULL; c; c = c->next) {
    if (!is_el(c, NS_TEXT, "list-style")) continue;
    g_autofree char *name = attr(c, NS_STYLE, "name");
    if (!name) continue;
    ListStyle *ls = g_new0(ListStyle, 1);
    for (int l = 0; l <= WP_LIST_MAX_LEVEL; l++) ls->kind[l] = WP_LIST_BULLET;
    for (xmlNode *lv = c->children; lv; lv = lv->next) {
      bool number = is_el(lv, NS_TEXT, "list-level-style-number");
      if (!number && !is_el(lv, NS_TEXT, "list-level-style-bullet") && !is_el(lv, NS_TEXT, "list-level-style-image")) continue;
      g_autofree char *lvl = attr(lv, NS_TEXT, "level");
      int level = lvl ? atoi(lvl) - 1 : -1;
      if (level < 0 || level > WP_LIST_MAX_LEVEL) continue;
      ls->kind[level] = number ? WP_LIST_NUMBER : WP_LIST_BULLET;
      xmlNode *props = child(lv, NS_STYLE, "list-level-properties");
      xmlNode *align = child(props, NS_STYLE, "list-level-label-alignment");
      double v, w;
      if (align) {
        g_autofree char *ml = attr(align, NS_FO, "margin-left");
        if (ml && parse_length(ml, &v)) { ls->margin[level] = v; ls->has_margin[level] = true; }
      } else if (props) {
        g_autofree char *sb = attr(props, NS_TEXT, "space-before");
        g_autofree char *mw = attr(props, NS_TEXT, "min-label-width");
        v = w = 0;
        bool any = sb && parse_length(sb, &v);
        if (mw && parse_length(mw, &w)) any = true;
        if (any) { ls->margin[level] = v + w; ls->has_margin[level] = true; }
      }
    }
    g_hash_table_insert(r->list_styles, g_strdup(name), ls);
  }
}

/* Which of our built-in styles a file paragraph style corresponds to. */
static const char *builtin_for(const Style *s)
{
  if (s->outline >= 1) return wp_intern(s->outline == 1 ? "Heading 1" : s->outline == 2 ? "Heading 2" : "Heading 3");
  if (!s->display) return NULL;
  if (!g_ascii_strcasecmp(s->display, "Title")) return wp_intern("Title");
  /* LibreOffice's Quotations (shown as Block Quotation) and Word's Quote styles */
  if (!g_ascii_strcasecmp(s->display, "Quote") || !g_ascii_strcasecmp(s->display, "Quotations") ||
      !g_ascii_strcasecmp(s->display, "Block Quotation") || !g_ascii_strcasecmp(s->display, "Intense Quote"))
    return wp_intern("Quote");
  if (!g_ascii_strcasecmp(s->display, "Note")) return wp_intern("Note");
  /* LibreOffice's list paragraph styles carry their own numbering */
  if (!g_ascii_strcasecmp(s->display, "List Bullet")) return wp_intern(WP_STYLE_LIST_BULLET);
  if (!g_ascii_strcasecmp(s->display, "List Number")) return wp_intern(WP_STYLE_LIST_NUMBER);
  return NULL;
}

/* Paragraph properties a file style or automatic style sets, applied over ours. */
static void apply_para_props(WpParaStyle *st, const Props *pp)
{
  if (pp->has & H_ALIGN)   st->align = pp->align;
  if (pp->has & H_BEFORE)  st->space_before_pt = (float)pp->before;
  if (pp->has & H_AFTER)   st->space_after_pt = (float)pp->after;
  if (pp->has & H_LINESP && pp->linesp > 0) st->line_spacing = (float)pp->linesp;
  if (pp->has & H_MLEFT)   st->indent_left_pt = (float)pp->mleft;
  if (pp->has & H_MRIGHT)  st->indent_right_pt = (float)pp->mright;
  if (pp->has & H_PADDING) st->padding_pt = (float)pp->padding;
  if (pp->has & H_BORDER)  { st->border_left_pt = (float)pp->border_w; st->border_color = pp->border_color; }
  if (pp->has & H_BG)      st->background = pp->bg;
  if (pp->has & H_TINDENT) st->indent_first_pt = (float)pp->tindent;
  if (pp->has & H_TABS)    { st->ntabs = pp->ntabs; memcpy(st->tabs, pp->tabs, sizeof st->tabs); }
}

/* Import the file's definitions of the styles we map to (Heading 1..3,
 * Title) so the document's built-ins look like the file's, then remember
 * the mapping for every style whose parent chain reaches one of them. */
static void map_named_styles(Reader *r)
{
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, r->styles);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    Style *s = val;
    if (strcmp(s->family, "paragraph") != 0) continue;
    const char *target = s->named ? builtin_for(s) : NULL;
    if (!target) continue;
    s->mapped = target;
    const WpNamedStyle *cur = wp_document_find_style(r->doc, target);
    if (!cur) continue;
    WpNamedStyle ns = *cur;
    const Props *pp = resolve(r, s);
    apply_para_props(&ns.para, pp);
    if (pp->has & H_KEEP)   ns.keep_with_next = pp->keep;
    ns.text.flags = ((pp->has & H_BOLD) && pp->bold ? WP_ATTR_BOLD : 0) |
                    ((pp->has & H_ITALIC) && pp->italic ? WP_ATTR_ITALIC : 0) |
                    ((pp->has & H_UNDERLINE) && pp->underline ? WP_ATTR_UNDERLINE : 0);
    ns.text.size_pt = (pp->has & H_SIZE) && pp->size_pt > 0 && pp->size_pt != r->doc->default_attrs.size_pt ? (float)pp->size_pt : 0;
    ns.text.family = (pp->has & H_FAMILY) && pp->family != r->doc->default_attrs.family ? pp->family : NULL;
    wp_document_add_style(r->doc, &ns);
  }
}

/* Our style for a paragraph: from an explicit outline level, else by walking
 * the style's parent chain to a mapped style. */
static const char *style_for_para(Reader *r, const char *style_name, int outline_level)
{
  if (outline_level >= 1) return wp_intern(outline_level == 1 ? "Heading 1" : outline_level == 2 ? "Heading 2" : "Heading 3");
  int guard = 0;
  for (Style *s = lookup_style(r, "paragraph", style_name); s && guard < 32; s = lookup_style(r, "paragraph", s->parent), guard++)
    if (s->mapped) return s->mapped;
  return wp_intern(WP_STYLE_STANDARD);
}

static void read_page_setup(Reader *r, const xmlNode *root)
{
  xmlNode *masters = child(root, NS_OFFICE, "master-styles");
  xmlNode *master = child(masters, NS_STYLE, "master-page");
  g_autofree char *layout_name = master ? attr(master, NS_STYLE, "page-layout-name") : NULL;

  xmlNode *autos = child(root, NS_OFFICE, "automatic-styles");
  for (xmlNode *c = autos ? autos->children : NULL; c; c = c->next) {
    if (!is_el(c, NS_STYLE, "page-layout")) continue;
    g_autofree char *name = attr(c, NS_STYLE, "name");
    if (layout_name && name && strcmp(name, layout_name) != 0) continue;
    xmlNode *pp = child(c, NS_STYLE, "page-layout-properties");
    if (!pp) continue;
    WpPageSetup ps = r->doc->page;
    struct { const char *a; double *dst; } m[] = {
      { "page-width", &ps.width }, { "page-height", &ps.height },
      { "margin-top", &ps.margin_top }, { "margin-bottom", &ps.margin_bottom },
      { "margin-left", &ps.margin_left }, { "margin-right", &ps.margin_right },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(m); i++) {
      g_autofree char *v = attr(pp, NS_FO, m[i].a);
      double d;
      if (v && parse_length(v, &d) && d >= 0) *m[i].dst = d;
    }
    if (ps.width > 0 && ps.height > 0) r->doc->page = ps;
    return;
  }
}

/* ---- body ----------------------------------------------------------------- */

/* Run attributes are stored relative to the paragraph's base (document
 * defaults + named style), so a bold heading has plain runs. */
static WpTextAttrs attrs_from_props(Reader *r, const Props *p)
{
  WpTextAttrs base = wp_document_para_base_attrs(r->doc, r->doc->nparas - 1);
  WpTextAttrs a = { 0, 0, NULL, 0 };
  if (r->open->len) a.comment = g_array_index(r->open, OpenComment, r->open->len - 1).id;
  if ((p->has & H_BOLD) && p->bold) a.flags |= WP_ATTR_BOLD;
  if ((p->has & H_ITALIC) && p->italic) a.flags |= WP_ATTR_ITALIC;
  if ((p->has & H_UNDERLINE) && p->underline) a.flags |= WP_ATTR_UNDERLINE;
  a.flags &= ~base.flags;
  if ((p->has & H_SIZE) && p->size_pt > 0 && (float)p->size_pt != base.size_pt) a.size_pt = (float)p->size_pt;
  if ((p->has & H_FAMILY) && p->family && p->family != base.family) a.family = p->family;
  return a;
}

static void set_comment_fn(WpTextAttrs *a, void *user) { a->comment = *(uint32_t *)user; }

static void append_text(Reader *r, const char *s, size_t n, const WpTextAttrs *attrs)
{
  if (n == 0) return;
  r->last_thread = 0;   /* text between two annotations separates them */
  WpPos start = wp_document_end(r->doc);
  wp_document_insert_text(r->doc, start, s, n, attrs);
  if (r->point && *s != '\n') {   /* a point comment lands on the character after it */
    WpPos one = { start.para, wp_utf8_next(s, n, 0) + start.offset };
    wp_document_apply_attrs(r->doc, start, one, set_comment_fn, &r->point);
    r->point = 0;
  }
}

/* A point comment with nothing after it in its paragraph goes on the
 * character before it, or on the empty paragraph itself. */
static void flush_point_comment(Reader *r)
{
  if (!r->point) return;
  WpPos end = wp_document_end(r->doc), a = end;
  if (end.offset > 0) a.offset = wp_utf8_prev(r->doc->paras[end.para].text, end.offset);
  wp_document_apply_attrs(r->doc, a, end, set_comment_fn, &r->point);
  r->point = 0;
}

/* <office:annotation>: creator, date and the comment's paragraphs. A named
 * one whose office:annotation-end exists covers the text up to it; any
 * other is a point comment. A reply names its parent in loext:parent-name
 * (LibreOffice), or simply follows the parent's annotation with no text in
 * between, which is how every editor lays a thread out. */
static void read_annotation(Reader *r, const xmlNode *n)
{
  g_autofree char *name = attr(n, NS_OFFICE, "name");
  g_autofree char *parent_name = attr(n, NS_LOEXT, "parent-name");
  g_autofree char *creator = NULL, *date = NULL;
  GString *text = g_string_new(NULL);
  bool first = true;
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    xmlChar *content = xmlNodeGetContent(c);
    const char *v = content ? (const char *)content : "";
    if (!strcmp((const char *)c->name, "creator") && !creator) creator = g_strdup(v);
    else if (!strcmp((const char *)c->name, "date") && !date) date = g_strdup(v);
    else if (is_el(c, NS_TEXT, "p") || is_el(c, NS_TEXT, "list")) {
      if (!first) g_string_append_c(text, '\n');
      g_string_append(text, v);
      first = false;
    }
    xmlFree(content);
  }
  uint32_t parent = 0;
  if (parent_name) parent = GPOINTER_TO_UINT(g_hash_table_lookup(r->names, parent_name));
  if (!parent && !parent_name) parent = r->last_thread;
  parent = wp_document_comment_root(r->doc, parent);
  uint32_t id = wp_document_new_comment_record(r->doc, parent, creator, date, text->str);
  g_string_free(text, TRUE);
  if (name) g_hash_table_insert(r->names, g_strdup(name), GUINT_TO_POINTER(id));
  if (parent) { r->last_thread = parent; return; }   /* a reply marks no text of its own */
  r->last_thread = id;
  if (name && g_hash_table_contains(r->ends, name)) {
    OpenComment oc = { g_strdup(name), id };
    g_array_append_val(r->open, oc);
  } else {
    flush_point_comment(r);
    r->point = id;
  }
}

static void end_annotation(Reader *r, const xmlNode *n)
{
  g_autofree char *name = attr(n, NS_OFFICE, "name");
  for (guint i = r->open->len; name && i > 0; i--) {
    OpenComment *oc = &g_array_index(r->open, OpenComment, i - 1);
    if (strcmp(oc->name, name) == 0) { g_free(oc->name); g_array_remove_index(r->open, i - 1); break; }
  }
}

static void collect_annotation_ends(Reader *r, const xmlNode *n)
{
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    if (is_el(c, NS_OFFICE, "annotation-end")) {
      char *name = attr(c, NS_OFFICE, "name");
      if (name) g_hash_table_add(r->ends, name);
    } else {
      collect_annotation_ends(r, c);
    }
  }
}

/* Append a text node with ODF whitespace collapsing. */
static void append_collapsed(Reader *r, const char *s, const WpTextAttrs *attrs)
{
  GString *out = g_string_new(NULL);
  for (; *s; s++) {
    bool ws = *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r';
    if (ws) { if (!r->last_space) g_string_append_c(out, ' '); r->last_space = true; }
    else { g_string_append_c(out, *s); r->last_space = false; }
  }
  append_text(r, out->str, out->len, attrs);
  g_string_free(out, TRUE);
}

static void set_attrs_fn(WpTextAttrs *a, void *user) { *a = *(WpTextAttrs *)user; }
static void set_style_fn(WpParaStyle *st, void *user) { *st = *(WpParaStyle *)user; }

/* list may be NULL. An item takes our list style for its kind, then the
 * file's paragraph properties over it as any paragraph does; its level and
 * kind come from the list. Its indent is the paragraph's own when it sets
 * one, else the list level's, else what the level normally has, and never
 * so small that the label would sit in the page margin. A later paragraph
 * of the same item is plain text at the item's indent. */
static void begin_paragraph(Reader *r, const Props *pp, const char *style_name, const ListInfo *list)
{
  static const WpTextAttrs none = { 0, 0, NULL, 0 };
  flush_point_comment(r);
  if (r->nparas > 0) append_text(r, "\n", 1, &none);
  r->nparas++;
  WpPos end = wp_document_end(r->doc);
  if (list && list->item) style_name = list->kind == WP_LIST_NUMBER ? WP_STYLE_LIST_NUMBER : WP_STYLE_LIST_BULLET;
  wp_document_set_style_name(r->doc, end, end, style_name);
  WpTextAttrs base = attrs_from_props(r, pp);
  wp_document_apply_attrs(r->doc, end, end, set_attrs_fn, &base);   /* attrs of the empty paragraph */

  WpParaStyle st = wp_document_style_of(r->doc, end.para)->para;
  apply_para_props(&st, pp);
  if (list) {
    st.list_kind = list->item ? list->kind : WP_LIST_NONE;
    st.list_level = list->item ? list->level : 0;
    if (list->item) st.indent_first_pt = 0;   /* the label hangs; the file's text-indent placed it */
    if (!(pp->has & H_MLEFT)) st.indent_left_pt = (float)(list->has_margin ? list->margin : wp_list_indent(list->level));
    if (list->item && st.indent_left_pt < WP_LIST_HANG_PT) st.indent_left_pt = WP_LIST_HANG_PT;
  } else if (st.list_kind != WP_LIST_NONE) {
    if (!(pp->has & H_MLEFT)) st.indent_left_pt = wp_list_indent(0);   /* LibreOffice's List Bullet style outside a text:list */
    st.indent_first_pt = 0;
  }
  wp_document_apply_para_style(r->doc, end, end, set_style_fn, &st);
  r->last_space = true;   /* leading whitespace is ignored */
}

static void walk_inline(Reader *r, const xmlNode *n, const Props *para_props, const Props *cur)
{
  const char *style_name = r->doc->paras[r->doc->nparas - 1].style_name;
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) {
      WpTextAttrs a = attrs_from_props(r, cur);
      append_collapsed(r, (const char *)c->content, &a);
    } else if (is_el(c, NS_TEXT, "span")) {
      g_autofree char *sn = attr(c, NS_TEXT, "style-name");
      Props next = merge(cur, resolve(r, lookup_style(r, "text", sn)));
      walk_inline(r, c, para_props, &next);
    } else if (is_el(c, NS_TEXT, "s")) {
      g_autofree char *cnt = attr(c, NS_TEXT, "c");
      int k = cnt ? atoi(cnt) : 1;
      WpTextAttrs a = attrs_from_props(r, cur);
      for (int i = 0; i < k; i++) append_text(r, " ", 1, &a);
      r->last_space = true;
    } else if (is_el(c, NS_TEXT, "tab")) {
      WpTextAttrs a = attrs_from_props(r, cur);
      append_text(r, "\t", 1, &a);
      r->last_space = false;
    } else if (is_el(c, NS_TEXT, "line-break")) {
      begin_paragraph(r, para_props, style_name, NULL);   /* closest thing our model has */
    } else if (is_el(c, NS_OFFICE, "annotation")) {
      read_annotation(r, c);
    } else if (is_el(c, NS_OFFICE, "annotation-end")) {
      end_annotation(r, c);
    } else if (is_el(c, NS_TEXT, "note") || is_el(c, NS_DRAW, "frame") || is_el(c, NS_TEXT, "tracked-changes")) {
      continue;
    } else if (c->type == XML_ELEMENT_NODE) {
      walk_inline(r, c, para_props, cur);   /* links, bookmarks, fields: keep their text */
    }
  }
}

/* The list the next paragraph belongs to, from the open text:list elements.
 * A heading keeps its heading style; a list-header's paragraphs and the
 * later paragraphs of an item are plain text at the list's indent. */
static bool current_list(Reader *r, bool heading, ListInfo *out)
{
  if (r->list_depth == 0 || heading) return false;
  int depth = r->list_depth, level = depth - 1;
  if (level > WP_LIST_MAX_LEVEL) level = WP_LIST_MAX_LEVEL;
  const ListStyle *ls = r->list_style[depth];
  out->kind = ls ? ls->kind[level] : WP_LIST_BULLET;
  out->level = level;
  out->item = r->list_first[depth];
  out->has_margin = ls && ls->has_margin[level];
  out->margin = ls ? ls->margin[level] : 0;
  r->list_first[depth] = false;
  return true;
}

static void walk_block(Reader *r, const xmlNode *n)
{
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    if (is_el(c, NS_TEXT, "p") || is_el(c, NS_TEXT, "h")) {
      g_autofree char *sn = attr(c, NS_TEXT, "style-name");
      g_autofree char *ol = attr(c, NS_TEXT, "outline-level");
      Props pp = *resolve(r, lookup_style(r, "paragraph", sn));
      if (!sn) pp = *resolve(r, r->default_para);
      int outline = is_el(c, NS_TEXT, "h") && ol ? atoi(ol) : 0;
      ListInfo li;
      bool in_list = current_list(r, outline > 0, &li);
      begin_paragraph(r, &pp, style_for_para(r, sn, outline), in_list ? &li : NULL);
      walk_inline(r, c, &pp, &pp);
    } else if (is_el(c, NS_TEXT, "list")) {
      g_autofree char *sn = attr(c, NS_TEXT, "style-name");
      const ListStyle *ls = sn ? g_hash_table_lookup(r->list_styles, sn) : NULL;
      if (r->list_depth < WP_LIST_MAX_LEVEL + 1) {
        r->list_depth++;
        r->list_style[r->list_depth] = ls ? ls : r->list_style[r->list_depth - 1];
        r->list_first[r->list_depth] = false;
        walk_block(r, c);
        r->list_depth--;
      } else {
        walk_block(r, c);   /* deeper than any editor nests: flatten into the innermost level */
      }
    } else if (is_el(c, NS_TEXT, "list-item") || is_el(c, NS_TEXT, "list-header")) {
      if (r->list_depth > 0) r->list_first[r->list_depth] = is_el(c, NS_TEXT, "list-item");
      walk_block(r, c);
    } else if (is_el(c, NS_TEXT, "tracked-changes") || is_el(c, NS_OFFICE, "forms") ||
               is_el(c, NS_TEXT, "sequence-decls") || is_el(c, NS_TEXT, "variable-decls") ||
               is_el(c, NS_TEXT, "user-field-decls")) {
      continue;
    } else {
      walk_block(r, c);   /* lists, list items, sections, tables, frames */
    }
  }
}

bool wp_odt_read(WpDocument *doc, const char *path, char **err)
{
  char *content = NULL, *styles = NULL;
  size_t clen = 0, slen = 0;
  if (!wp_zip_read_entry(path, "content.xml", &content, &clen, err)) return false;
  wp_zip_read_entry(path, "styles.xml", &styles, &slen, NULL);   /* optional */

  xmlDoc *cdoc = xmlReadMemory(content, (int)clen, "content.xml", NULL, XML_PARSE_NONET | XML_PARSE_HUGE);
  xmlDoc *sdoc = styles ? xmlReadMemory(styles, (int)slen, "styles.xml", NULL, XML_PARSE_NONET | XML_PARSE_HUGE) : NULL;
  free(content);
  free(styles);
  if (!cdoc) { if (err) *err = strdup("content.xml is not well-formed XML"); if (sdoc) xmlFreeDoc(sdoc); return false; }

  Reader r = { 0 };
  r.doc = doc;
  r.styles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_style);
  r.fonts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  r.ends = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  r.names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  r.open = g_array_new(FALSE, FALSE, sizeof(OpenComment));
  r.list_styles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  xmlNode *croot = xmlDocGetRootElement(cdoc);
  xmlNode *sroot = sdoc ? xmlDocGetRootElement(sdoc) : NULL;
  if (sroot) { collect_fonts(&r, sroot); collect_styles(&r, child(sroot, NS_OFFICE, "styles"), true); collect_styles(&r, child(sroot, NS_OFFICE, "automatic-styles"), false); }
  collect_fonts(&r, croot);
  collect_styles(&r, child(croot, NS_OFFICE, "styles"), true);
  collect_styles(&r, child(croot, NS_OFFICE, "automatic-styles"), false);
  if (sroot) { collect_list_styles(&r, child(sroot, NS_OFFICE, "styles")); collect_list_styles(&r, child(sroot, NS_OFFICE, "automatic-styles")); }
  collect_list_styles(&r, child(croot, NS_OFFICE, "styles"));
  collect_list_styles(&r, child(croot, NS_OFFICE, "automatic-styles"));

  /* Suspend change notifications while we rebuild the document. */
  WpDocumentChangedFn saved_fn = doc->on_change;
  void *saved_user = doc->on_change_user;
  doc->on_change = NULL;

  wp_document_reset_styles(doc);
  wp_document_clear(doc);
  doc->page = wp_page_setup_letter();
  const Props *dp = resolve(&r, r.default_para);
  if (dp->has & H_SIZE && dp->size_pt > 0) doc->default_attrs.size_pt = (float)dp->size_pt;
  if (dp->has & H_FAMILY && dp->family) doc->default_attrs.family = dp->family;
  map_named_styles(&r);
  if (sroot) read_page_setup(&r, sroot);

  xmlNode *body = child(croot, NS_OFFICE, "body");
  xmlNode *text = child(body, NS_OFFICE, "text");
  if (text) collect_annotation_ends(&r, text);
  if (text) walk_block(&r, text);
  flush_point_comment(&r);

  doc->on_change = saved_fn;
  doc->on_change_user = saved_user;
  wp_document_changed(doc, 0);

  g_hash_table_destroy(r.styles);
  g_hash_table_destroy(r.fonts);
  g_hash_table_destroy(r.ends);
  g_hash_table_destroy(r.names);
  g_hash_table_destroy(r.list_styles);
  for (guint i = 0; i < r.open->len; i++) g_free(g_array_index(r.open, OpenComment, i).name);
  g_array_free(r.open, TRUE);
  g_clear_pointer(&r.default_para, free_style);
  g_clear_pointer(&r.default_text, free_style);
  xmlFreeDoc(cdoc);
  if (sdoc) xmlFreeDoc(sdoc);
  return true;
}

/* ========================================================================= */
/* Writing                                                                    */
/* ========================================================================= */

#define XMLNS_COMMON \
  " xmlns:office=\"" NS_OFFICE "\" xmlns:style=\"" NS_STYLE "\" xmlns:text=\"" NS_TEXT "\"" \
  " xmlns:table=\"" NS_TABLE "\" xmlns:draw=\"" NS_DRAW "\" xmlns:fo=\"" NS_FO "\" xmlns:svg=\"" NS_SVG "\"" \
  " xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"" \
  " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" xmlns:loext=\"" NS_LOEXT "\" office:version=\"1.3\""

static void xml_escape(GString *out, const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '&': g_string_append(out, "&amp;"); break;
    case '<': g_string_append(out, "&lt;"); break;
    case '>': g_string_append(out, "&gt;"); break;
    case '"': g_string_append(out, "&quot;"); break;
    default:
      if (c < 0x20 && c != '\t') break;   /* control characters are not allowed in XML */
      g_string_append_c(out, (char)c);
    }
  }
}

static void append_pt(GString *out, const char *attr, double v)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(buf, sizeof buf, "%.3f", v);
  /* trim trailing zeros for tidiness */
  char *dot = strchr(buf, '.');
  if (dot) { char *e = buf + strlen(buf) - 1; while (e > dot && *e == '0') *e-- = 0; if (e == dot) *e = 0; }
  g_string_append_printf(out, " %s=\"%spt\"", attr, buf);
}

typedef struct Uniq { GArray *items; size_t elem; int (*cmp)(const void *, const void *); } Uniq;
typedef struct ParaKey { const char *style_name; WpParaStyle st; } ParaKey;

static void encode_style_name(GString *out, const char *name)
{
  for (const char *c = name; *c; c++) {
    if (*c == ' ') g_string_append(out, "_20_");
    else xml_escape(out, c, 1);
  }
}

static int uniq_index(Uniq *u, const void *item)
{
  for (guint i = 0; i < u->items->len; i++)
    if (u->cmp(u->items->data + i * u->elem, item) == 0) return (int)i;
  g_array_append_vals(u->items, item, 1);
  return (int)u->items->len - 1;
}

static int cmp_attrs(const void *a, const void *b) { return wp_attrs_equal(a, b) ? 0 : 1; }

/* Where each comment's text starts and ends, as (paragraph, run) pairs, so
 * write_para_text can open the annotation before the first run and close it
 * after the last. Ids no record exists for are left out and written as
 * plain text. */
typedef struct Anchor { uint32_t id; size_t p0, r0, p1, r1; } Anchor;

static GArray *find_anchors(const WpDocument *doc)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(Anchor));
  for (size_t pi = 0; pi < doc->nparas; pi++) {
    const WpParagraph *p = &doc->paras[pi];
    for (size_t ri = 0; ri < p->nruns; ri++) {
      uint32_t id = p->runs[ri].attrs.comment;
      if (!id || !wp_document_comment(doc, id)) continue;
      Anchor *a = NULL;
      for (guint k = 0; k < out->len; k++)
        if (g_array_index(out, Anchor, k).id == id) a = &g_array_index(out, Anchor, k);
      if (!a) { Anchor n = { id, pi, ri, pi, ri }; g_array_append_val(out, n); }
      else { a->p1 = pi; a->r1 = ri; }
    }
  }
  return out;
}

static const Anchor *anchor_for(GArray *anchors, uint32_t id)
{
  for (guint k = 0; k < anchors->len; k++)
    if (g_array_index(anchors, Anchor, k).id == id) return &g_array_index(anchors, Anchor, k);
  return NULL;
}

static void write_annotation(GString *out, const WpComment *c)
{
  g_string_append_printf(out, "<office:annotation office:name=\"cmt%u\"", c->id);
  if (c->parent) g_string_append_printf(out, " loext:parent-name=\"cmt%u\"", c->parent);
  g_string_append(out, "><dc:creator>");
  xml_escape(out, c->author, strlen(c->author));
  g_string_append(out, "</dc:creator><dc:date>");
  xml_escape(out, c->date, strlen(c->date));
  g_string_append(out, "</dc:date>");
  const char *t = c->text;
  for (;;) {
    const char *nl = strchr(t, '\n');
    size_t n = nl ? (size_t)(nl - t) : strlen(t);
    g_string_append(out, "<text:p>");
    xml_escape(out, t, n);
    g_string_append(out, "</text:p>");
    if (!nl) break;
    t = nl + 1;
  }
  g_string_append(out, "</office:annotation>");
}

/* A thread: the comment, then its replies straight after it. */
static void write_thread(GString *out, const WpDocument *doc, uint32_t id)
{
  write_annotation(out, wp_document_comment(doc, id));
  for (size_t i = 0; i < doc->ncomments; i++)
    if (doc->comments[i].parent == id) write_annotation(out, &doc->comments[i]);
}
static int cmp_style(const void *a, const void *b)
{
  const ParaKey *x = a, *y = b;
  return x->style_name == y->style_name ? memcmp(&x->st, &y->st, sizeof x->st) : 1;
}

static void write_font_decls(GString *out, const WpDocument *doc, Uniq *runs)
{
  g_string_append(out, "<office:font-face-decls>");
  GPtrArray *seen = g_ptr_array_new();
  g_ptr_array_add(seen, (gpointer)doc->default_attrs.family);
  for (guint i = 0; i < runs->items->len; i++) {
    const WpTextAttrs *a = &g_array_index(runs->items, WpTextAttrs, i);
    if (a->family && !g_ptr_array_find(seen, a->family, NULL)) g_ptr_array_add(seen, (gpointer)a->family);
  }
  for (guint i = 0; i < seen->len; i++) {
    const char *f = seen->pdata[i];
    g_string_append(out, "<style:font-face style:name=\"");
    xml_escape(out, f, strlen(f));
    g_string_append(out, "\" svg:font-family=\"");
    xml_escape(out, f, strlen(f));
    g_string_append(out, "\"/>");
  }
  g_ptr_array_free(seen, TRUE);
  g_string_append(out, "</office:font-face-decls>");
}

static void write_text_props(GString *out, const WpTextAttrs *a)
{
  g_string_append(out, "<style:text-properties");
  if (a->flags & WP_ATTR_BOLD)
    g_string_append(out, " fo:font-weight=\"bold\" style:font-weight-asian=\"bold\" style:font-weight-complex=\"bold\"");
  if (a->flags & WP_ATTR_ITALIC)
    g_string_append(out, " fo:font-style=\"italic\" style:font-style-asian=\"italic\" style:font-style-complex=\"italic\"");
  if (a->flags & WP_ATTR_UNDERLINE)
    g_string_append(out, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"");
  if (a->size_pt > 0) {
    append_pt(out, "fo:font-size", a->size_pt);
    append_pt(out, "style:font-size-asian", a->size_pt);
    append_pt(out, "style:font-size-complex", a->size_pt);
  }
  if (a->family) {
    g_string_append(out, " style:font-name=\"");
    xml_escape(out, a->family, strlen(a->family));
    g_string_append(out, "\"");
  }
  g_string_append(out, "/>");
}

static void write_para_props(GString *out, const WpParaStyle *st, bool keep_with_next)
{
  g_string_append(out, "<style:paragraph-properties");
  static const char *align[] = { "start", "center", "end", "justify" };
  g_string_append_printf(out, " fo:text-align=\"%s\"", align[st->align]);
  append_pt(out, "fo:margin-top", st->space_before_pt);
  append_pt(out, "fo:margin-bottom", st->space_after_pt);
  if (st->line_spacing > 0 && st->line_spacing != 1.0f)
    g_string_append_printf(out, " fo:line-height=\"%d%%\"", (int)(st->line_spacing * 100 + 0.5));
  if (keep_with_next) g_string_append(out, " fo:keep-with-next=\"always\"");
  /* Written even when zero or none, so an automatic style can override its parent. */
  append_pt(out, "fo:margin-left", st->indent_left_pt);
  append_pt(out, "fo:margin-right", st->indent_right_pt);
  append_pt(out, "fo:padding", st->padding_pt);
  if (st->border_left_pt > 0) {
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.2f", st->border_left_pt);
    uint32_t c = wp_color_set(st->border_color) ? st->border_color : WP_COLOR(0, 0, 0);
    g_string_append_printf(out, " fo:border-left=\"%spt solid #%06x\" fo:border-right=\"none\" fo:border-top=\"none\" fo:border-bottom=\"none\"", buf, c & 0xffffff);
  } else {
    g_string_append(out, " fo:border=\"none\"");
  }
  if (wp_color_set(st->background)) g_string_append_printf(out, " fo:background-color=\"#%06x\"", st->background & 0xffffff);
  else g_string_append(out, " fo:background-color=\"transparent\"");
  /* A list item's label hangs to the left of its text. Written on the
   * paragraph so that readers take the paragraph's indents over the list
   * level's, and the text lands where GemWrite puts it. */
  append_pt(out, "fo:text-indent", st->list_kind != WP_LIST_NONE ? -WP_LIST_HANG_PT : st->indent_first_pt);
  /* Tab stops replace the parent style's, so an empty table is written too. */
  g_string_append(out, "><style:tab-stops>");
  for (int i = 0; i < st->ntabs; i++) {
    static const char *types[] = { "left", "center", "right", "char" };
    g_string_append(out, "<style:tab-stop");
    append_pt(out, "style:position", st->tabs[i].pos_pt);
    g_string_append_printf(out, " style:type=\"%s\"", types[st->tabs[i].kind]);
    if (st->tabs[i].kind == WP_TAB_DECIMAL) g_string_append(out, " style:char=\".\"");
    g_string_append(out, "/>");
  }
  g_string_append(out, "</style:tab-stops></style:paragraph-properties>");
}

/* ---- lists --------------------------------------------------------------- */

/* A list as the file holds it: consecutive items, with the kind of label at
 * each level fixed for the whole list, since ODF ties the kind to the list
 * style's level. Levels no item uses are NONE until the style is written. */
typedef struct ListKey { WpListKind kind[WP_LIST_MAX_LEVEL + 1]; } ListKey;

static int cmp_list(const void *a, const void *b) { return memcmp(a, b, sizeof(ListKey)); }

static int clamp_level(const WpParaStyle *st)
{
  return st->list_level < 0 ? 0 : st->list_level > WP_LIST_MAX_LEVEL ? WP_LIST_MAX_LEVEL : st->list_level;
}

/* The items from `first` that can share one list: the run stops at an
 * ordinary paragraph, or where an item's kind conflicts with what its level
 * already has. Returns one past the last item and fills the key. */
static size_t list_run_end(const WpDocument *doc, size_t first, ListKey *key)
{
  memset(key, 0, sizeof *key);
  size_t i = first;
  for (; i < doc->nparas; i++) {
    const WpParaStyle *st = &doc->paras[i].style;
    if (st->list_kind == WP_LIST_NONE) break;
    int level = clamp_level(st);
    if (key->kind[level] == WP_LIST_NONE) key->kind[level] = st->list_kind;
    else if (key->kind[level] != st->list_kind) break;
  }
  return i;
}

static void write_list_style(GString *out, const ListKey *key, int index)
{
  static const char *bullet_chars[] = { "\xe2\x80\xa2", "\xe2\x97\xa6", "\xe2\x96\xaa" };
  static const char *num_formats[] = { "1", "a", "i" };
  g_string_append_printf(out, "<text:list-style style:name=\"L%d\">", index);
  WpListKind fallback = WP_LIST_BULLET;
  for (int l = 0; l <= WP_LIST_MAX_LEVEL; l++) if (key->kind[l] != WP_LIST_NONE) { fallback = key->kind[l]; break; }
  for (int l = 0; l <= WP_LIST_MAX_LEVEL; l++) {
    /* an unused level takes the kind of the nearest used one above it */
    WpListKind kind = key->kind[l] != WP_LIST_NONE ? key->kind[l] : fallback;
    fallback = kind;
    if (kind == WP_LIST_NUMBER)
      g_string_append_printf(out, "<text:list-level-style-number text:level=\"%d\" style:num-suffix=\".\" style:num-format=\"%s\">", l + 1, num_formats[l % 3]);
    else
      g_string_append_printf(out, "<text:list-level-style-bullet text:level=\"%d\" text:bullet-char=\"%s\">", l + 1, bullet_chars[l % 3]);
    g_string_append(out, "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                         "<style:list-level-label-alignment text:label-followed-by=\"listtab\"");
    append_pt(out, "text:list-tab-stop-position", wp_list_indent(l));
    append_pt(out, "fo:text-indent", -WP_LIST_HANG_PT);
    append_pt(out, "fo:margin-left", wp_list_indent(l));
    g_string_append(out, "/></style:list-level-properties>");
    g_string_append(out, kind == WP_LIST_NUMBER ? "</text:list-level-style-number>" : "</text:list-level-style-bullet>");
  }
  g_string_append(out, "</text:list-style>");
}

/* Nesting as ODF wants it: a deeper list sits inside the item before it,
 * and a run that starts below the top opens empty items to reach its
 * level. `depth` counts open text:list elements; the innermost one always
 * has an open item once anything has been written. */
typedef struct ListWriter { int depth; bool item_open; } ListWriter;

static void list_close_to(GString *body, ListWriter *lw, int depth)
{
  while (lw->depth > depth) {
    if (lw->item_open) g_string_append(body, "</text:list-item>");
    g_string_append(body, "</text:list>");
    lw->depth--;
    lw->item_open = lw->depth > 0;   /* back inside the item that held the nested list */
  }
}

static void list_open_item(GString *body, ListWriter *lw, int level, int style_index)
{
  int want = level + 1;
  list_close_to(body, lw, want);
  if (lw->depth == want) {
    if (lw->item_open) g_string_append(body, "</text:list-item>");
    g_string_append(body, "<text:list-item>");
    lw->item_open = true;
  }
  while (lw->depth < want) {
    if (lw->depth > 0 && !lw->item_open) { g_string_append(body, "<text:list-item>"); lw->item_open = true; }
    if (lw->depth == 0) g_string_append_printf(body, "<text:list text:style-name=\"L%d\">", style_index);
    else g_string_append(body, "<text:list>");
    g_string_append(body, "<text:list-item>");
    lw->depth++;
    lw->item_open = true;
  }
}

static void write_named_styles(GString *out, const WpDocument *doc)
{
  for (size_t i = 0; i < doc->nstyles; i++) {
    const WpNamedStyle *ns = &doc->styles[i];
    g_string_append(out, "<style:style style:name=\"");
    encode_style_name(out, ns->name);
    g_string_append(out, "\" style:display-name=\"");
    xml_escape(out, ns->name, strlen(ns->name));
    g_string_append(out, "\" style:family=\"paragraph\" style:class=\"text\"");
    if (strcmp(ns->name, WP_STYLE_STANDARD) != 0) g_string_append(out, " style:parent-style-name=\"Standard\"");
    if (ns->next) { g_string_append(out, " style:next-style-name=\""); encode_style_name(out, ns->next); g_string_append(out, "\""); }
    if (ns->outline_level > 0) g_string_append_printf(out, " style:default-outline-level=\"%d\"", ns->outline_level);
    g_string_append(out, ">");
    write_para_props(out, &ns->para, ns->keep_with_next);
    write_text_props(out, &ns->text);
    g_string_append(out, "</style:style>");
  }
}

/* Paragraph text with ODF whitespace encoding: runs of spaces, and any space
 * at the start or end of the paragraph, become <text:s>. */
static void write_para_text(GString *out, const WpDocument *doc, size_t pi, Uniq *runs, GArray *anchors)
{
  const WpParagraph *p = &doc->paras[pi];
  size_t off = 0;
  for (size_t ri = 0; ri < p->nruns; ri++) {
    const WpRun *run = &p->runs[ri];
    const Anchor *anchor = anchor_for(anchors, run->attrs.comment);
    if (anchor && anchor->p0 == pi && anchor->r0 == ri) write_thread(out, doc, anchor->id);
    if (run->len == 0) continue;
    WpTextAttrs key = run->attrs;   /* the comment is an annotation, not a text style */
    key.comment = 0;
    bool styled = key.flags || key.size_pt > 0 || key.family;
    if (styled) g_string_append_printf(out, "<text:span text:style-name=\"T%d\">", uniq_index(runs, &key) + 1);

    size_t i = off, end = off + run->len;
    while (i < end) {
      if (p->text[i] == ' ') {
        size_t j = i;
        while (j < end && p->text[j] == ' ') j++;
        size_t n = j - i;
        /* One literal space is safe only when it is the first of its run of
         * spaces in the paragraph's text (a run boundary may fall inside the
         * run of spaces) and touches neither end of the paragraph. */
        bool literal_ok = i > 0 && p->text[i - 1] != ' ' && j < p->len;
        if (literal_ok) { g_string_append_c(out, ' '); n--; }
        if (n == 1) g_string_append(out, "<text:s/>");
        else if (n > 1) g_string_append_printf(out, "<text:s text:c=\"%zu\"/>", n);
        i = j;
      } else if (p->text[i] == '\t') {
        g_string_append(out, "<text:tab/>");
        i++;
      } else {
        size_t j = i;
        while (j < end && p->text[j] != ' ' && p->text[j] != '\t') j++;
        xml_escape(out, p->text + i, j - i);
        i = j;
      }
    }
    if (styled) g_string_append(out, "</text:span>");
    if (anchor && anchor->p1 == pi && anchor->r1 == ri)
      g_string_append_printf(out, "<office:annotation-end office:name=\"cmt%u\"/>", anchor->id);
    off = end;
  }
}

bool wp_odt_write(const WpDocument *doc, const char *path, char **err)
{
  Uniq runs = { g_array_new(FALSE, FALSE, sizeof(WpTextAttrs)), sizeof(WpTextAttrs), cmp_attrs };
  Uniq paras = { g_array_new(FALSE, FALSE, sizeof(ParaKey)), sizeof(ParaKey), cmp_style };
  Uniq lists = { g_array_new(FALSE, FALSE, sizeof(ListKey)), sizeof(ListKey), cmp_list };
  GArray *anchors = find_anchors(doc);

  /* Body first so the automatic styles it references are known. */
  GString *body = g_string_new("<office:body><office:text>");
  ListWriter lw = { 0, false };
  size_t run_end = 0;
  int list_index = 0;
  for (size_t i = 0; i < doc->nparas; i++) {
    const WpParagraph *p = &doc->paras[i];
    const WpNamedStyle *ns = wp_document_style_of(doc, i);
    ParaKey key = { ns->name, p->style };
    int pidx = uniq_index(&paras, &key) + 1;
    if (p->style.list_kind != WP_LIST_NONE) {
      if (i >= run_end) {   /* a new list: close the previous one, pick its style */
        list_close_to(body, &lw, 0);
        ListKey lk;
        run_end = list_run_end(doc, i, &lk);
        list_index = uniq_index(&lists, &lk) + 1;
      }
      list_open_item(body, &lw, clamp_level(&p->style), list_index);
    } else {
      list_close_to(body, &lw, 0);
    }
    if (ns->outline_level > 0)
      g_string_append_printf(body, "<text:h text:style-name=\"P%d\" text:outline-level=\"%d\">", pidx, ns->outline_level);
    else
      g_string_append_printf(body, "<text:p text:style-name=\"P%d\">", pidx);
    write_para_text(body, doc, i, &runs, anchors);
    g_string_append(body, ns->outline_level > 0 ? "</text:h>" : "</text:p>");
  }
  list_close_to(body, &lw, 0);
  g_string_append(body, "</office:text></office:body>");

  GString *content = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" XMLNS_COMMON ">");
  write_font_decls(content, doc, &runs);
  g_string_append(content, "<office:automatic-styles>");
  for (guint i = 0; i < paras.items->len; i++) {
    const ParaKey *k = &g_array_index(paras.items, ParaKey, i);
    g_string_append_printf(content, "<style:style style:name=\"P%u\" style:family=\"paragraph\" style:parent-style-name=\"", i + 1);
    encode_style_name(content, k->style_name);
    g_string_append(content, "\">");
    write_para_props(content, &k->st, false);
    g_string_append(content, "</style:style>");
  }
  for (guint i = 0; i < runs.items->len; i++) {
    g_string_append_printf(content, "<style:style style:name=\"T%u\" style:family=\"text\">", i + 1);
    write_text_props(content, &g_array_index(runs.items, WpTextAttrs, i));
    g_string_append(content, "</style:style>");
  }
  for (guint i = 0; i < lists.items->len; i++)
    write_list_style(content, &g_array_index(lists.items, ListKey, i), (int)i + 1);
  g_string_append(content, "</office:automatic-styles>");
  g_string_append(content, body->str);
  g_string_append(content, "</office:document-content>");

  GString *styles = g_string_new("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" XMLNS_COMMON ">");
  write_font_decls(styles, doc, &runs);
  g_string_append(styles, "<office:styles><style:default-style style:family=\"paragraph\">");
  WpParaStyle def = doc->default_style;
  write_para_props(styles, &def, false);
  WpTextAttrs defa = { 0, doc->default_attrs.size_pt, doc->default_attrs.family };
  write_text_props(styles, &defa);
  g_string_append(styles, "</style:default-style>");
  write_named_styles(styles, doc);
  g_string_append(styles, "</office:styles><office:automatic-styles>"
                          "<style:page-layout style:name=\"pm1\"><style:page-layout-properties");
  append_pt(styles, "fo:page-width", doc->page.width);
  append_pt(styles, "fo:page-height", doc->page.height);
  g_string_append_printf(styles, " style:print-orientation=\"%s\"", doc->page.width > doc->page.height ? "landscape" : "portrait");
  append_pt(styles, "fo:margin-top", doc->page.margin_top);
  append_pt(styles, "fo:margin-bottom", doc->page.margin_bottom);
  append_pt(styles, "fo:margin-left", doc->page.margin_left);
  append_pt(styles, "fo:margin-right", doc->page.margin_right);
  g_string_append(styles, "/></style:page-layout></office:automatic-styles>"
                          "<office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"/></office:master-styles>"
                          "</office:document-styles>");

  const char *manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"" NS_MANIFEST "\" manifest:version=\"1.3\">"
    "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"" MIMETYPE "\"/>"
    "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
    "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
    "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>"
    "</manifest:manifest>";
  const char *meta =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta" XMLNS_COMMON "><office:meta><meta:generator>GemWrite/" GEMWRITE_VERSION "</meta:generator></office:meta></office:document-meta>";

  bool ok = false;
  WpZipWriter *z = wp_zip_writer_open(path, err);
  if (z) {
    ok = wp_zip_writer_add(z, "mimetype", MIMETYPE, strlen(MIMETYPE), true) &&
         wp_zip_writer_add(z, "META-INF/manifest.xml", manifest, strlen(manifest), false) &&
         wp_zip_writer_add(z, "content.xml", content->str, content->len, false) &&
         wp_zip_writer_add(z, "styles.xml", styles->str, styles->len, false) &&
         wp_zip_writer_add(z, "meta.xml", meta, strlen(meta), false);
    if (!wp_zip_writer_close(z, ok ? err : NULL)) ok = false;
  }

  g_string_free(body, TRUE);
  g_string_free(content, TRUE);
  g_string_free(styles, TRUE);
  g_array_free(runs.items, TRUE);
  g_array_free(paras.items, TRUE);
  g_array_free(lists.items, TRUE);
  g_array_free(anchors, TRUE);
  return ok;
}
