/* commands.c — see commands.h. */
#include "edit/commands.h"

#include "io/markdown.h"

#include <cairo.h>
#include <cairo-pdf.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------- */

static bool fail(char **err, const char *msg)
{
  if (err) *err = strdup(msg);
  return false;
}

static bool failf(char **err, const char *fmt, const char *arg)
{
  if (err) *err = g_strdup_printf(fmt, arg);
  return false;
}

static size_t byte_to_char(const WpParagraph *p, size_t off)
{
  return (size_t)g_utf8_pointer_to_offset(p->text, p->text + MIN(off, p->len));
}

static size_t char_to_byte(const WpParagraph *p, size_t chars)
{
  size_t n = (size_t)g_utf8_strlen(p->text, (gssize)p->len);
  if (chars > n) chars = n;
  return (size_t)(g_utf8_offset_to_pointer(p->text, (glong)chars) - p->text);
}

bool wp_editor_parse_pos(WpEditor *ed, const char *spec, WpPos *out, char **err)
{
  WpDocument *doc = wp_editor_document(ed);
  if (!strcmp(spec, "start"))  { *out = (WpPos){ 0, 0 }; return true; }
  if (!strcmp(spec, "end"))    { *out = wp_document_end(doc); return true; }
  if (!strcmp(spec, "caret"))  { *out = wp_editor_caret(ed); return true; }
  if (!strcmp(spec, "anchor")) { *out = wp_editor_anchor(ed); return true; }

  char *rest = NULL;
  unsigned long para = strtoul(spec, &rest, 10);
  if (rest == spec || (*rest && *rest != ':'))
    return failf(err, "Bad position '%s' (expected start, end, caret, anchor, P or P:C)", spec);
  if (para >= wp_document_para_count(doc))
    return failf(err, "Paragraph %s is out of range", spec);
  unsigned long chars = 0;
  if (*rest == ':') {
    char *end = NULL;
    chars = strtoul(rest + 1, &end, 10);
    if (end == rest + 1 || *end) return failf(err, "Bad character offset in '%s'", spec);
  }
  *out = (WpPos){ para, char_to_byte(wp_document_para(doc, para), chars) };
  return true;
}

const char *wp_default_spell_language(void)
{
  static const char *cached;
  static bool looked;
  if (looked) return cached;
  looked = true;
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "gemwrite", "settings.ini", NULL);
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) return NULL;
  g_autofree char *lang = g_key_file_get_string(kf, "general", "spell-language", NULL);
  if (lang && *lang) cached = wp_intern(lang);
  return cached;
}

bool wp_editor_ensure_spell(WpEditor *ed, char **err)
{
  if (wp_editor_spell(ed)) return true;
  WpSpell *sp = wp_spell_new(wp_default_spell_language(), err);
  if (!sp) return false;
  wp_editor_set_spell(ed, sp);
  return true;
}

const char *wp_default_author(void)
{
  static const char *cached;
  if (cached) return cached;
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "gemwrite", "settings.ini", NULL);
  g_autofree char *from_settings = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    from_settings = g_key_file_get_string(kf, "general", "author", NULL);
  const char *name = from_settings && *from_settings ? from_settings : g_get_real_name();
  if (!name || !*name || !strcmp(name, "Unknown")) name = g_get_user_name();
  cached = wp_intern(name ? name : "");
  return cached;
}

GVariant *wp_editor_pos_variant(WpEditor *ed, WpPos p)
{
  const WpParagraph *para = wp_document_para(wp_editor_document(ed), p.para);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "para", g_variant_new_uint32((guint32)p.para));
  g_variant_builder_add(&b, "{sv}", "offset", g_variant_new_uint32((guint32)byte_to_char(para, p.offset)));
  return g_variant_builder_end(&b);
}

static bool parse_align(const char *s, WpAlign *out, char **err)
{
  static const struct { const char *name; WpAlign a; } t[] = {
    { "left", WP_ALIGN_LEFT }, { "center", WP_ALIGN_CENTER }, { "centre", WP_ALIGN_CENTER },
    { "right", WP_ALIGN_RIGHT }, { "justify", WP_ALIGN_JUSTIFY },
  };
  for (size_t i = 0; i < G_N_ELEMENTS(t); i++)
    if (!g_ascii_strcasecmp(s, t[i].name)) { *out = t[i].a; return true; }
  return failf(err, "Unknown alignment '%s' (left, center, right, justify)", s);
}

static const char *align_name(WpAlign a)
{
  switch (a) {
  case WP_ALIGN_CENTER:  return "center";
  case WP_ALIGN_RIGHT:   return "right";
  case WP_ALIGN_JUSTIFY: return "justify";
  default:               return "left";
  }
}

static bool parse_unit(const char *s, WpMoveUnit *out, char **err)
{
  static const struct { const char *name; WpMoveUnit u; } t[] = {
    { "char", WP_MOVE_CHAR }, { "word", WP_MOVE_WORD }, { "line", WP_MOVE_LINE },
    { "line-edge", WP_MOVE_LINE_EDGE }, { "page", WP_MOVE_PAGE }, { "doc", WP_MOVE_DOC },
  };
  for (size_t i = 0; i < G_N_ELEMENTS(t); i++)
    if (!strcmp(s, t[i].name)) { *out = t[i].u; return true; }
  return failf(err, "Unknown unit '%s' (char, word, line, line-edge, page, doc)", s);
}

/* ---- file ---------------------------------------------------------------- */

static bool cmd_open(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  return wp_editor_load(ed, g_variant_get_string(a, NULL), err);
}
static bool cmd_save(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  return wp_editor_save(ed, NULL, err);
}
static bool cmd_save_as(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  return wp_editor_save(ed, g_variant_get_string(a, NULL), err);
}
/* An export writes a copy in another format; the document keeps its own
 * file and its modified state. */
static bool cmd_export_markdown(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  return wp_markdown_write(wp_editor_document(ed), g_variant_get_string(a, NULL), err);
}

/* Every page, as printed, into a PDF of the document's page size. */
static bool cmd_export_pdf(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *path = g_variant_get_string(a, NULL);
  WpDocument *doc = wp_editor_document(ed);
  WpLayoutEngine *e = wp_editor_engine(ed);
  cairo_surface_t *s = cairo_pdf_surface_create(path, doc->page.width, doc->page.height);
  cairo_t *cr = cairo_create(s);
  wp_layout_set_comment_marks(e, false);
  for (size_t i = 0; i < wp_layout_page_count(e); i++) {
    cairo_set_source_rgb(cr, 0, 0, 0);
    wp_layout_render_page(e, i, cr);
    cairo_show_page(cr);
  }
  wp_layout_set_comment_marks(e, true);
  cairo_destroy(cr);
  cairo_surface_finish(s);
  cairo_status_t st = cairo_surface_status(s);
  cairo_surface_destroy(s);
  if (st != CAIRO_STATUS_SUCCESS) return failf(err, "Could not write PDF: %s", cairo_status_to_string(st));
  return true;
}

/* ---- editing ------------------------------------------------------------- */

static bool cmd_insert_text(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  gsize len = 0;
  const char *s = g_variant_get_string(a, &len);
  wp_editor_insert_text(ed, s, (long)len);
  return true;
}
static bool cmd_new_paragraph(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  wp_editor_insert_text(ed, "\n", 1);
  return true;
}
static bool cmd_delete_backward(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_delete(ed, -1); return true; }
static bool cmd_delete_forward(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { wp_editor_delete(ed, +1); return true; }
static bool cmd_delete_selection(WpEditor *ed, GVariant *a, GVariant **out, char **err){ wp_editor_delete_selection(ed); return true; }
static bool cmd_cut(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_cut(ed)) return fail(err, "Nothing is selected");
  return true;
}
static bool cmd_copy(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_copy(ed)) return fail(err, "Nothing is selected");
  return true;
}
static bool cmd_paste(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_paste(ed)) return fail(err, "The clipboard is empty");
  return true;
}
static bool cmd_select_all(WpEditor *ed, GVariant *a, GVariant **out, char **err)      { wp_editor_select_all(ed); return true; }

static bool cmd_goto(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpPos p;
  if (!wp_editor_parse_pos(ed, g_variant_get_string(a, NULL), &p, err)) return false;
  wp_editor_set_caret(ed, p, false);
  return true;
}

static bool cmd_select(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *from, *to;
  g_variant_get(a, "(&s&s)", &from, &to);
  WpPos pa, pb;
  if (!wp_editor_parse_pos(ed, from, &pa, err) || !wp_editor_parse_pos(ed, to, &pb, err)) return false;
  wp_editor_select(ed, pa, pb);
  return true;
}

static bool do_move(WpEditor *ed, GVariant *a, bool extend, char **err)
{
  const char *unit_s; gint32 dir;
  g_variant_get(a, "(&si)", &unit_s, &dir);
  WpMoveUnit unit;
  if (!parse_unit(unit_s, &unit, err)) return false;
  if (dir == 0) return fail(err, "Direction must be negative (back) or positive (forward)");
  int steps = ABS(dir);
  for (int i = 0; i < steps; i++)
    if (!wp_editor_move(ed, unit, dir < 0 ? -1 : 1, extend)) break;
  return true;
}
static bool cmd_move(WpEditor *ed, GVariant *a, GVariant **out, char **err)   { return do_move(ed, a, false, err); }
static bool cmd_extend(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return do_move(ed, a, true, err); }

static bool selection_variant(WpEditor *ed, GVariant **out)
{
  WpPos s, e;
  wp_editor_get_selection(ed, &s, &e);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "start", wp_editor_pos_variant(ed, s));
  g_variant_builder_add(&b, "{sv}", "end", wp_editor_pos_variant(ed, e));
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_find(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *needle = g_variant_get_string(a, NULL);
  if (!*needle) return fail(err, "Nothing to find");
  if (!wp_editor_find(ed, needle)) return failf(err, "'%s' not found", needle);
  return selection_variant(ed, out);
}

static bool cmd_match_case(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  unsigned flags = wp_editor_search_flags(ed) & ~WP_FIND_MATCH_CASE;
  if (g_variant_get_boolean(a)) flags |= WP_FIND_MATCH_CASE;
  wp_editor_set_search(ed, wp_editor_search_term(ed), flags);
  return true;
}

static bool find_step(WpEditor *ed, int dir, GVariant **out, char **err)
{
  const char *term = wp_editor_search_term(ed);
  if (!term) return fail(err, "Nothing to find: no search term has been set");
  if (!wp_editor_find_next(ed, dir)) return failf(err, "'%s' not found", term);
  return selection_variant(ed, out);
}
static bool cmd_find_next(WpEditor *ed, GVariant *a, GVariant **out, char **err)     { return find_step(ed, 1, out, err); }
static bool cmd_find_previous(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return find_step(ed, -1, out, err); }

static bool cmd_replace(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *with = g_variant_get_string(a, NULL);
  if (!wp_editor_search_term(ed)) return fail(err, "Nothing to replace: no search term has been set");
  if (!wp_editor_replace(ed, with)) return fail(err, "The selection is not a match of the search term");
  return true;
}

static bool cmd_replace_all(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *what, *with;
  g_variant_get(a, "(&s&s)", &what, &with);
  if (!*what) return fail(err, "Nothing to find");
  wp_editor_set_search(ed, what, wp_editor_search_flags(ed));
  size_t n = wp_editor_replace_all(ed, with);
  if (!n) return failf(err, "'%s' not found", what);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "replaced", g_variant_new_uint64(n));
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_reload(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return wp_editor_reload(ed, err); }

static bool cmd_undo(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_undo(ed)) return fail(err, "Nothing to undo");
  return true;
}
static bool cmd_redo(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_redo(ed)) return fail(err, "Nothing to redo");
  return true;
}

/* ---- formatting ---------------------------------------------------------- */

static bool cmd_bold(WpEditor *ed, GVariant *a, GVariant **out, char **err)      { wp_editor_toggle_attr(ed, WP_ATTR_BOLD); return true; }
static bool cmd_italic(WpEditor *ed, GVariant *a, GVariant **out, char **err)    { wp_editor_toggle_attr(ed, WP_ATTR_ITALIC); return true; }
static bool cmd_underline(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_toggle_attr(ed, WP_ATTR_UNDERLINE); return true; }

static bool cmd_set_font(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  wp_editor_set_family(ed, g_variant_get_string(a, NULL));
  return true;
}

static bool cmd_set_size(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  double pt = g_variant_get_double(a);
  if (pt < 4 || pt > 400) return fail(err, "Font size must be between 4 and 400 points");
  wp_editor_set_size(ed, (float)pt);
  return true;
}

/* Sizes font-bigger and font-smaller step through. */
static const float font_steps[] = { 6, 7, 8, 9, 10, 10.5, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 26, 28, 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 88, 96 };

static void step_font_size(WpEditor *ed, int dir)
{
  float cur = wp_editor_effective_attrs(ed).size_pt, next = cur;
  size_t n = G_N_ELEMENTS(font_steps);
  if (dir > 0) { for (size_t i = 0; i < n; i++) if (font_steps[i] > cur + 0.01f) { next = font_steps[i]; break; } }
  else         { for (size_t i = n; i-- > 0;)  if (font_steps[i] < cur - 0.01f) { next = font_steps[i]; break; } }
  if (next != cur) wp_editor_set_size(ed, next);
}
static bool cmd_font_bigger(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { step_font_size(ed, +1); return true; }
static bool cmd_font_smaller(WpEditor *ed, GVariant *a, GVariant **out, char **err) { step_font_size(ed, -1); return true; }

static bool cmd_align(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpAlign al;
  if (!parse_align(g_variant_get_string(a, NULL), &al, err)) return false;
  wp_editor_set_align(ed, al);
  return true;
}

/* Line spacing is a multiple of the font's line height; the presets are the
 * ones every word processor offers. */
#define WP_LINE_SPACING_MIN 0.5
#define WP_LINE_SPACING_MAX 10.0
static bool set_line_spacing(WpEditor *ed, double factor, char **err)
{
  if (!(factor >= WP_LINE_SPACING_MIN && factor <= WP_LINE_SPACING_MAX))
    return fail(err, "Line spacing must be between 0.5 and 10 (1 is single, 2 is double)");
  wp_editor_set_line_spacing(ed, (float)factor);
  return true;
}
static bool cmd_line_spacing(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { return set_line_spacing(ed, g_variant_get_double(a), err); }
static bool cmd_spacing_single(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return set_line_spacing(ed, 1.0, err); }
static bool cmd_spacing_1_5(WpEditor *ed, GVariant *a, GVariant **out, char **err)    { return set_line_spacing(ed, 1.5, err); }
static bool cmd_spacing_double(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return set_line_spacing(ed, 2.0, err); }

/* Paragraphs store a float; report it rounded so 1.15 prints as 1.15. */
static double line_spacing_value(float f)
{
  return round((double)f * 100.0) / 100.0;
}

static bool set_style(WpEditor *ed, const char *name, char **err)
{
  if (!wp_document_find_style(wp_editor_document(ed), name)) return failf(err, "Unknown paragraph style '%s'", name);
  wp_editor_set_style_name(ed, name);
  return true;
}
static bool cmd_set_style(WpEditor *ed, GVariant *a, GVariant **out, char **err)   { return set_style(ed, g_variant_get_string(a, NULL), err); }
static bool cmd_style_body(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { return set_style(ed, WP_STYLE_STANDARD, err); }
static bool cmd_style_title(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return set_style(ed, "Title", err); }
static bool cmd_style_h1(WpEditor *ed, GVariant *a, GVariant **out, char **err)    { return set_style(ed, "Heading 1", err); }
static bool cmd_style_h2(WpEditor *ed, GVariant *a, GVariant **out, char **err)    { return set_style(ed, "Heading 2", err); }
static bool cmd_style_h3(WpEditor *ed, GVariant *a, GVariant **out, char **err)    { return set_style(ed, "Heading 3", err); }
static bool cmd_style_quote(WpEditor *ed, GVariant *a, GVariant **out, char **err) { return set_style(ed, "Quote", err); }
static bool cmd_style_note(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { return set_style(ed, "Note", err); }

/* ---- lists --------------------------------------------------------------- */

static bool cmd_list_bullet(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_set_list(ed, WP_LIST_BULLET); return true; }
static bool cmd_list_number(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_set_list(ed, WP_LIST_NUMBER); return true; }
/* Indenting as far as it goes is not an error: the toolbar button would
 * otherwise put up a dialog for a press that had nothing left to do. */
static bool cmd_indent(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { wp_editor_indent(ed, +1); return true; }
static bool cmd_outdent(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_indent(ed, -1); return true; }

/* ---- indents and tab stops ----------------------------------------------- */

static bool cmd_indent_left(WpEditor *ed, GVariant *a, GVariant **out, char **err)  { wp_editor_set_indent(ed, WP_INDENT_LEFT, (float)g_variant_get_double(a)); return true; }
static bool cmd_indent_right(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_set_indent(ed, WP_INDENT_RIGHT, (float)g_variant_get_double(a)); return true; }
static bool cmd_indent_first(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_set_indent(ed, WP_INDENT_FIRST, (float)g_variant_get_double(a)); return true; }

static const char *tab_kind_names[] = { "left", "center", "right", "decimal" };

static bool cmd_tab_add(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  double pos; const char *type;
  g_variant_get(a, "(d&s)", &pos, &type);
  if (pos < 0) return fail(err, "A tab stop's position is measured from the left indent and cannot be negative");
  for (size_t k = 0; k < G_N_ELEMENTS(tab_kind_names); k++) {
    if (g_ascii_strcasecmp(type, tab_kind_names[k]) == 0 || (k == WP_TAB_CENTER && !g_ascii_strcasecmp(type, "centre"))) {
      wp_editor_add_tab(ed, (float)pos, (WpTabKind)k);
      return true;
    }
  }
  return failf(err, "Unknown tab stop type '%s' (left, center, right, decimal)", type);
}

static bool cmd_tab_remove(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_remove_tab(ed, (float)g_variant_get_double(a))) return fail(err, "No tab stop at that position (see attrs)");
  return true;
}

static bool cmd_tab_clear(WpEditor *ed, GVariant *a, GVariant **out, char **err) { wp_editor_clear_tabs(ed); return true; }

static double pt_value(float f) { return round((double)f * 100.0) / 100.0 + 0.0; }   /* + 0.0 turns -0 into 0 */

static const char *list_kind_name(WpListKind k)
{
  return k == WP_LIST_BULLET ? "bullet" : k == WP_LIST_NUMBER ? "number" : "none";
}

/* ---- queries ------------------------------------------------------------- */

static bool cmd_text(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  size_t len = 0;
  char *t = wp_document_get_all_text(wp_editor_document(ed), &len);
  *out = g_variant_new_string(t);
  free(t);
  return true;
}

static bool cmd_selection(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpPos s, e;
  wp_editor_get_selection(ed, &s, &e);
  size_t len = 0;
  char *t = wp_document_get_text(wp_editor_document(ed), s, e, &len);
  *out = g_variant_new_string(t);
  free(t);
  return true;
}

static bool cmd_caret(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "caret", wp_editor_pos_variant(ed, wp_editor_caret(ed)));
  g_variant_builder_add(&b, "{sv}", "anchor", wp_editor_pos_variant(ed, wp_editor_anchor(ed)));
  g_variant_builder_add(&b, "{sv}", "has_selection", g_variant_new_boolean(wp_editor_has_selection(ed)));
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_info(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpDocument *doc = wp_editor_document(ed);
  const char *path = wp_editor_path(ed);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "path", g_variant_new_string(path ? path : ""));
  g_variant_builder_add(&b, "{sv}", "modified", g_variant_new_boolean(wp_editor_modified(ed)));
  g_variant_builder_add(&b, "{sv}", "paragraphs", g_variant_new_uint32((guint32)wp_document_para_count(doc)));
  g_variant_builder_add(&b, "{sv}", "words", g_variant_new_uint32((guint32)wp_editor_word_count(ed, (WpPos){ 0, 0 }, wp_document_end(doc))));
  g_variant_builder_add(&b, "{sv}", "pages", g_variant_new_uint32((guint32)wp_editor_page_count(ed)));
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_paragraphs(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpDocument *doc = wp_editor_document(ed);
  GVariantBuilder list;
  g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
  size_t n = wp_document_para_count(doc);
  for (size_t i = 0; i < n; i++) {
    const WpParagraph *p = wp_document_para(doc, i);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "index", g_variant_new_uint32((guint32)i));
    g_variant_builder_add(&b, "{sv}", "style", g_variant_new_string(wp_document_style_of(doc, i)->name));
    g_variant_builder_add(&b, "{sv}", "align", g_variant_new_string(align_name(p->style.align)));
    g_variant_builder_add(&b, "{sv}", "spacing", g_variant_new_double(line_spacing_value(p->style.line_spacing > 0 ? p->style.line_spacing : 1.0f)));
    g_variant_builder_add(&b, "{sv}", "indent", g_variant_new_double(round(p->style.indent_left_pt * 100.0) / 100.0));
    g_variant_builder_add(&b, "{sv}", "list", g_variant_new_string(list_kind_name(p->style.list_kind)));
    g_variant_builder_add(&b, "{sv}", "level", g_variant_new_int32(p->style.list_kind == WP_LIST_NONE ? 0 : p->style.list_level));
    g_variant_builder_add(&b, "{sv}", "text", g_variant_new_string(p->text));
    g_variant_builder_add_value(&list, g_variant_builder_end(&b));
  }
  *out = g_variant_builder_end(&list);
  return true;
}

static bool cmd_styles(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpDocument *doc = wp_editor_document(ed);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_STRING_ARRAY);
  for (size_t i = 0; i < doc->nstyles; i++) g_variant_builder_add(&b, "s", doc->styles[i].name);
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_attrs(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpTextAttrs t = wp_editor_effective_attrs(ed);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "bold", g_variant_new_boolean((t.flags & WP_ATTR_BOLD) != 0));
  g_variant_builder_add(&b, "{sv}", "italic", g_variant_new_boolean((t.flags & WP_ATTR_ITALIC) != 0));
  g_variant_builder_add(&b, "{sv}", "underline", g_variant_new_boolean((t.flags & WP_ATTR_UNDERLINE) != 0));
  g_variant_builder_add(&b, "{sv}", "family", g_variant_new_string(t.family ? t.family : ""));
  g_variant_builder_add(&b, "{sv}", "size", g_variant_new_double(t.size_pt));
  g_variant_builder_add(&b, "{sv}", "style", g_variant_new_string(wp_editor_current_style_name(ed)));
  g_variant_builder_add(&b, "{sv}", "align", g_variant_new_string(align_name(wp_editor_current_align(ed))));
  g_variant_builder_add(&b, "{sv}", "spacing", g_variant_new_double(line_spacing_value(wp_editor_current_line_spacing(ed))));
  const WpParaStyle *ps = &wp_document_para(wp_editor_document(ed), wp_editor_caret(ed).para)->style;
  g_variant_builder_add(&b, "{sv}", "indent", g_variant_new_double(pt_value(ps->indent_left_pt)));
  g_variant_builder_add(&b, "{sv}", "indent_right", g_variant_new_double(pt_value(ps->indent_right_pt)));
  g_variant_builder_add(&b, "{sv}", "indent_first", g_variant_new_double(pt_value(ps->list_kind == WP_LIST_NONE ? ps->indent_first_pt : 0)));
  GVariantBuilder tabs;
  g_variant_builder_init(&tabs, G_VARIANT_TYPE("aa{sv}"));
  for (int i = 0; i < ps->ntabs; i++) {
    GVariantBuilder t;
    g_variant_builder_init(&t, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&t, "{sv}", "pos", g_variant_new_double(pt_value(ps->tabs[i].pos_pt)));
    g_variant_builder_add(&t, "{sv}", "type", g_variant_new_string(tab_kind_names[ps->tabs[i].kind]));
    g_variant_builder_add_value(&tabs, g_variant_builder_end(&t));
  }
  g_variant_builder_add(&b, "{sv}", "tabs", g_variant_builder_end(&tabs));
  g_variant_builder_add(&b, "{sv}", "list", g_variant_new_string(list_kind_name(wp_editor_current_list_kind(ed))));
  g_variant_builder_add(&b, "{sv}", "level", g_variant_new_int32(wp_editor_current_list_level(ed)));
  g_variant_builder_add(&b, "{sv}", "comment", g_variant_new_uint32(wp_editor_comment_at_caret(ed)));
  *out = g_variant_builder_end(&b);
  return true;
}

static bool cmd_render(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  const char *path; guint32 page;
  g_variant_get(a, "(&su)", &path, &page);
  WpDocument *doc = wp_editor_document(ed);
  WpLayoutEngine *e = wp_editor_engine(ed);
  if (page >= wp_layout_page_count(e)) return fail(err, "Page number is out of range (pages are numbered from 0)");
  const double scale = 2.0;   /* 144 dpi: legible without being huge */
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(doc->page.width * scale), (int)(doc->page.height * scale));
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);
  cairo_scale(cr, scale, scale);
  cairo_set_source_rgb(cr, 0, 0, 0);
  wp_layout_render_page(e, page, cr);
  cairo_destroy(cr);
  cairo_status_t st = cairo_surface_write_to_png(s, path);
  cairo_surface_destroy(s);
  if (st != CAIRO_STATUS_SUCCESS) return failf(err, "Could not write PNG: %s", cairo_status_to_string(st));
  return true;
}

/* ---- comments ------------------------------------------------------------ */

static GVariant *id_result(uint32_t id)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "id", g_variant_new_uint32(id));
  return g_variant_builder_end(&b);
}

static bool no_such_comment(char **err, guint32 id)
{
  g_autofree char *s = g_strdup_printf("%u", id);
  return failf(err, "There is no comment %s (see comments)", s);
}

static bool cmd_comment_add(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_has_selection(ed)) return fail(err, "Select the text to comment on first");
  const char *author = wp_editor_author(ed) ? wp_editor_author(ed) : wp_default_author();
  uint32_t id = wp_editor_add_comment(ed, author, NULL, g_variant_get_string(a, NULL));
  *out = id_result(id);
  return true;
}

static bool cmd_comment_reply(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  guint32 id; const char *text;
  g_variant_get(a, "(u&s)", &id, &text);
  const char *author = wp_editor_author(ed) ? wp_editor_author(ed) : wp_default_author();
  uint32_t rid = wp_editor_add_reply(ed, id, author, NULL, text);
  if (!rid) return no_such_comment(err, id);
  *out = id_result(rid);
  return true;
}

static bool cmd_comment_edit(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  guint32 id; const char *text;
  g_variant_get(a, "(u&s)", &id, &text);
  return wp_editor_set_comment_text(ed, id, text) || no_such_comment(err, id);
}

static bool cmd_comment_delete(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  guint32 id = g_variant_get_uint32(a);
  return wp_editor_remove_comment(ed, id) || no_such_comment(err, id);
}

static bool cmd_comment_select(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  guint32 id = g_variant_get_uint32(a);
  return wp_editor_select_comment(ed, id) || no_such_comment(err, id);
}

static void add_comment_entry(GVariantBuilder *list, WpEditor *ed, const WpComment *c, WpPos s, WpPos e)
{
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "id", g_variant_new_uint32(c->id));
  g_variant_builder_add(&b, "{sv}", "parent", g_variant_new_uint32(c->parent));
  g_variant_builder_add(&b, "{sv}", "author", g_variant_new_string(c->author));
  g_variant_builder_add(&b, "{sv}", "date", g_variant_new_string(c->date));
  g_variant_builder_add(&b, "{sv}", "text", g_variant_new_string(c->text));
  g_variant_builder_add(&b, "{sv}", "start", wp_editor_pos_variant(ed, s));
  g_variant_builder_add(&b, "{sv}", "end", wp_editor_pos_variant(ed, e));
  g_variant_builder_add_value(list, g_variant_builder_end(&b));
}

/* Comments in document order, each followed by its replies; orphans left out. */
static bool cmd_comments(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpDocument *doc = wp_editor_document(ed);
  GVariantBuilder list;
  g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
  size_t n = doc->ncomments;
  /* order by start position: pick the smallest unemitted start each round (n is tiny) */
  g_autofree bool *done = g_new0(bool, n ? n : 1);
  for (;;) {
    size_t best = n; WpPos bs = { 0, 0 }, be = { 0, 0 };
    for (size_t i = 0; i < n; i++) {
      WpPos s, e;
      if (done[i] || doc->comments[i].parent || !wp_document_comment_range(doc, doc->comments[i].id, &s, &e)) continue;
      if (best == n || wp_pos_cmp(s, bs) < 0) { best = i; bs = s; be = e; }
    }
    if (best == n) break;
    done[best] = true;
    add_comment_entry(&list, ed, &doc->comments[best], bs, be);
    for (size_t i = 0; i < n; i++)
      if (doc->comments[i].parent == doc->comments[best].id) add_comment_entry(&list, ed, &doc->comments[i], bs, be);
  }
  *out = g_variant_builder_end(&list);
  return true;
}

/* ---- spelling ------------------------------------------------------------ */

static bool no_misspelling_here(char **err)
{
  return fail(err, "The caret is not on a misspelled word (see misspellings)");
}

static bool cmd_spell_add(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_ensure_spell(ed, err)) return false;
  return wp_editor_spell_add(ed) || no_misspelling_here(err);
}

static bool cmd_spell_ignore(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_ensure_spell(ed, err)) return false;
  return wp_editor_spell_ignore(ed) || no_misspelling_here(err);
}

static bool cmd_spell_language(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  WpSpell *sp = wp_spell_new(g_variant_get_string(a, NULL), err);
  if (!sp) return false;
  wp_editor_set_spell(ed, sp);
  return true;
}

static bool cmd_spell_languages(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  size_t n;
  char **langs = wp_spell_languages(&n);
  *out = g_variant_new_strv((const char *const *)langs, (gssize)n);
  g_strfreev(langs);
  return true;
}

/* Every misspelled word with its position and the dictionary's suggestions. */
static bool cmd_misspellings(WpEditor *ed, GVariant *a, GVariant **out, char **err)
{
  if (!wp_editor_ensure_spell(ed, err)) return false;
  WpDocument *doc = wp_editor_document(ed);
  WpSpell *sp = wp_editor_spell(ed);
  GVariantBuilder list;
  g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
  for (size_t pi = 0; pi < wp_document_para_count(doc); pi++) {
    size_t n;
    const WpSpan *spans = wp_editor_misspellings(ed, pi, &n);
    const WpParagraph *p = wp_document_para(doc, pi);
    for (size_t i = 0; i < n; i++) {
      g_autofree char *word = g_strndup(p->text + spans[i].start, spans[i].end - spans[i].start);
      size_t ns;
      char **sug = wp_spell_suggest(sp, word, strlen(word), &ns);
      GVariantBuilder b;
      g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&b, "{sv}", "word", g_variant_new_string(word));
      g_variant_builder_add(&b, "{sv}", "start", wp_editor_pos_variant(ed, (WpPos){ pi, spans[i].start }));
      g_variant_builder_add(&b, "{sv}", "end", wp_editor_pos_variant(ed, (WpPos){ pi, spans[i].end }));
      g_variant_builder_add(&b, "{sv}", "suggestions", g_variant_new_strv((const char *const *)sug, (gssize)ns));
      g_variant_builder_add_value(&list, g_variant_builder_end(&b));
      g_strfreev(sug);
    }
  }
  *out = g_variant_builder_end(&list);
  return true;
}

/* ---- the table ----------------------------------------------------------- */

static const WpCommand commands[] = {
  /* name             params   usage             summary               group              accel */
  { "open",           "s",     "FILE",           "Open",               "File",            "<Control>o",            cmd_open },
  { "save",           NULL,    NULL,             "Save",               "File",            "<Control>s",            cmd_save },
  { "save-as",        "s",     "FILE",           "Save As",            "File",            "<Control><Shift>s",     cmd_save_as },
  { "reload",         NULL,    NULL,             "Reload from Disk",   "File",            NULL,                    cmd_reload },
  { "export-markdown","s",     "FILE",           "Export as Markdown", "File",            NULL,                    cmd_export_markdown },
  { "export-pdf",     "s",     "FILE",           "Export as PDF",      "File",            NULL,                    cmd_export_pdf },

  { "undo",           NULL,    NULL,             "Undo",               "Editing",         "<Control>z",            cmd_undo },
  { "redo",           NULL,    NULL,             "Redo",               "Editing",         "<Control><Shift>z",     cmd_redo, "<Control>y" },
  { "cut",            NULL,    NULL,             "Cut",                "Editing",         "<Control>x",            cmd_cut, "<Shift>Delete" },
  { "copy",           NULL,    NULL,             "Copy",               "Editing",         "<Control>c",            cmd_copy, "<Control>Insert" },
  { "paste",          NULL,    NULL,             "Paste",              "Editing",         "<Control>v",            cmd_paste, "<Shift>Insert" },
  { "insert-text",    "s",     "TEXT",           "Insert Text",        "Editing",         NULL,                    cmd_insert_text },
  { "new-paragraph",  NULL,    NULL,             "New Paragraph",      "Editing",         NULL,                    cmd_new_paragraph },
  { "delete-backward",NULL,    NULL,             "Delete Backward",    "Editing",         NULL,                    cmd_delete_backward },
  { "delete-forward", NULL,    NULL,             "Delete Forward",     "Editing",         NULL,                    cmd_delete_forward },
  { "delete-selection",NULL,   NULL,             "Delete Selection",   "Editing",         NULL,                    cmd_delete_selection },
  { "select-all",     NULL,    NULL,             "Select All",         "Editing",         "<Control>a",            cmd_select_all },
  { "goto",           "s",     "POS",            "Move Caret",         "Editing",         NULL,                    cmd_goto },
  { "select",         "(ss)",  "FROM TO",        "Select Range",       "Editing",         NULL,                    cmd_select },
  { "move",           "(si)",  "UNIT COUNT",     "Move by Unit",       "Editing",         NULL,                    cmd_move },
  { "extend",         "(si)",  "UNIT COUNT",     "Extend Selection",   "Editing",         NULL,                    cmd_extend },
  { "find",           "s",     "TEXT",           "Find",               "Editing",         NULL,                    cmd_find },
  { "find-next",      NULL,    NULL,             "Find Next",          "Editing",         "<Control>g",            cmd_find_next },
  { "find-previous",  NULL,    NULL,             "Find Previous",      "Editing",         "<Control><Shift>g",     cmd_find_previous },
  { "match-case",     "b",     "true|false",     "Match Case",         "Editing",         NULL,                    cmd_match_case },
  { "replace",        "s",     "TEXT",           "Replace",            "Editing",         NULL,                    cmd_replace },
  { "replace-all",    "(ss)",  "FIND TEXT",      "Replace All",        "Editing",         NULL,                    cmd_replace_all },

  { "bold",           NULL,    NULL,             "Bold",               "Formatting",      "<Control>b",            cmd_bold },
  { "italic",         NULL,    NULL,             "Italic",             "Formatting",      "<Control>i",            cmd_italic },
  { "underline",      NULL,    NULL,             "Underline",          "Formatting",      "<Control>u",            cmd_underline },
  { "set-font",       "s",     "FAMILY",         "Set Font",           "Formatting",      NULL,                    cmd_set_font },
  { "set-size",       "d",     "POINTS",         "Set Font Size",      "Formatting",      NULL,                    cmd_set_size },
  { "font-bigger",    NULL,    NULL,             "Larger Font",        "Formatting",      "<Control>bracketright", cmd_font_bigger },
  { "font-smaller",   NULL,    NULL,             "Smaller Font",       "Formatting",      "<Control>bracketleft",  cmd_font_smaller },
  { "align",          "s",     "left|center|right|justify", "Align Paragraph", "Formatting", NULL,               cmd_align },
  { "line-spacing",   "d",     "FACTOR",         "Set Line Spacing",   "Formatting",      NULL,                    cmd_line_spacing },
  { "spacing-single", NULL,    NULL,             "Single Spacing",     "Formatting",      "<Control>1",            cmd_spacing_single },
  { "spacing-1.5",    NULL,    NULL,             "1.5 Line Spacing",   "Formatting",      "<Control>5",            cmd_spacing_1_5 },
  { "spacing-double", NULL,    NULL,             "Double Spacing",     "Formatting",      "<Control>2",            cmd_spacing_double },
  { "indent",         NULL,    NULL,             "Increase Indent",    "Formatting",      "<Control>m",            cmd_indent },
  { "outdent",        NULL,    NULL,             "Decrease Indent",    "Formatting",      "<Control><Shift>m",     cmd_outdent },
  { "indent-left",    "d",     "POINTS",         "Set Left Indent",    "Formatting",      NULL,                    cmd_indent_left },
  { "indent-right",   "d",     "POINTS",         "Set Right Indent",   "Formatting",      NULL,                    cmd_indent_right },
  { "indent-first",   "d",     "POINTS",         "Set First Line Indent", "Formatting",   NULL,                    cmd_indent_first },
  { "tab-add",        "(ds)",  "POINTS left|center|right|decimal", "Add Tab Stop", "Formatting", NULL,             cmd_tab_add },
  { "tab-remove",     "d",     "POINTS",         "Remove Tab Stop",    "Formatting",      NULL,                    cmd_tab_remove },
  { "tab-clear",      NULL,    NULL,             "Clear Tab Stops",    "Formatting",      NULL,                    cmd_tab_clear },

  { "set-style",      "s",     "NAME",           "Set Paragraph Style","Paragraph Style", NULL,                    cmd_set_style },
  { "style-body",     NULL,    NULL,             "Body Text",          "Paragraph Style", "<Control><Alt>0",       cmd_style_body },
  { "style-title",    NULL,    NULL,             "Title",              "Paragraph Style", "<Control><Alt>t",       cmd_style_title },
  { "style-h1",       NULL,    NULL,             "Heading 1",          "Paragraph Style", "<Control><Alt>1",       cmd_style_h1 },
  { "style-h2",       NULL,    NULL,             "Heading 2",          "Paragraph Style", "<Control><Alt>2",       cmd_style_h2 },
  { "style-h3",       NULL,    NULL,             "Heading 3",          "Paragraph Style", "<Control><Alt>3",       cmd_style_h3 },
  { "style-quote",    NULL,    NULL,             "Quote",              "Paragraph Style", "<Control><Alt>q",       cmd_style_quote },
  { "style-note",     NULL,    NULL,             "Note",               "Paragraph Style", "<Control><Alt>n",       cmd_style_note },

  { "list-bullet",    NULL,    NULL,             "Bulleted List",      "Lists",           "<Control><Alt>8",       cmd_list_bullet },
  { "list-number",    NULL,    NULL,             "Numbered List",      "Lists",           "<Control><Alt>7",       cmd_list_number },

  { "comment-add",    "s",     "TEXT",           "Add Comment",        "Comments",        NULL,                    cmd_comment_add },
  { "comment-reply",  "(us)",  "ID TEXT",        "Reply to Comment",   "Comments",        NULL,                    cmd_comment_reply },
  { "comment-edit",   "(us)",  "ID TEXT",        "Edit Comment",       "Comments",        NULL,                    cmd_comment_edit },
  { "comment-delete", "u",     "ID",             "Delete Comment",     "Comments",        NULL,                    cmd_comment_delete },
  { "comment-select", "u",     "ID",             "Select Comment",     "Comments",        NULL,                    cmd_comment_select },

  { "spell-add",      NULL,    NULL,             "Add to Dictionary",  "Spelling",        NULL,                    cmd_spell_add },
  { "spell-ignore",   NULL,    NULL,             "Ignore Word",        "Spelling",        NULL,                    cmd_spell_ignore },
  { "spell-language", "s",     "LANG",           "Set Spelling Language", "Spelling",     NULL,                    cmd_spell_language },

  { "text",           NULL,    NULL,             "Document Text",      "Query",           NULL,                    cmd_text },
  { "selection",      NULL,    NULL,             "Selected Text",      "Query",           NULL,                    cmd_selection },
  { "caret",          NULL,    NULL,             "Caret Position",     "Query",           NULL,                    cmd_caret },
  { "info",           NULL,    NULL,             "Document Info",      "Query",           NULL,                    cmd_info },
  { "paragraphs",     NULL,    NULL,             "List Paragraphs",    "Query",           NULL,                    cmd_paragraphs },
  { "styles",         NULL,    NULL,             "List Styles",        "Query",           NULL,                    cmd_styles },
  { "attrs",          NULL,    NULL,             "Formatting at Caret","Query",           NULL,                    cmd_attrs },
  { "comments",       NULL,    NULL,             "List Comments",      "Query",           NULL,                    cmd_comments },
  { "render",         "(su)",  "PNG PAGE",       "Render Page to PNG", "Query",           NULL,                    cmd_render },
  { "misspellings",   NULL,    NULL,             "List Misspellings",  "Query",           NULL,                    cmd_misspellings },
  { "spell-languages",NULL,    NULL,             "List Spelling Languages", "Query",      NULL,                    cmd_spell_languages },
};

const WpCommand *wp_commands(size_t *count)
{
  if (count) *count = G_N_ELEMENTS(commands);
  return commands;
}

const WpCommand *wp_command_find(const char *name)
{
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
    if (!strcmp(commands[i].name, name)) return &commands[i];
  return NULL;
}

bool wp_command_run(WpEditor *ed, const char *name, GVariant *arg, GVariant **out, char **err)
{
  const WpCommand *cmd = wp_command_find(name);
  if (!cmd) return failf(err, "Unknown command '%s'", name);
  if (cmd->params && (!arg || !g_variant_is_of_type(arg, G_VARIANT_TYPE(cmd->params))))
    return failf(err, "Command '%s' was given the wrong kind of argument", name);
  GVariant *result = NULL;
  bool ok = cmd->run(ed, arg, &result, err);
  if (out) *out = result;
  else if (result) g_variant_unref(g_variant_ref_sink(result));
  return ok;
}

/* ---- argv parsing -------------------------------------------------------- */

int wp_command_arity(const WpCommand *cmd)
{
  if (!cmd->params) return 0;
  const GVariantType *t = G_VARIANT_TYPE(cmd->params);
  if (!g_variant_type_is_tuple(t)) return 1;
  return (int)g_variant_type_n_items(t);
}

static bool parse_leaf(const GVariantType *t, const char *word, GVariant **out, char **err)
{
  char *end = NULL;
  if (g_variant_type_equal(t, G_VARIANT_TYPE_STRING)) {
    *out = g_variant_new_string(word);
    return true;
  }
  if (g_variant_type_equal(t, G_VARIANT_TYPE_DOUBLE)) {
    double d = g_ascii_strtod(word, &end);
    if (end == word || *end) return failf(err, "'%s' is not a number", word);
    *out = g_variant_new_double(d);
    return true;
  }
  if (g_variant_type_equal(t, G_VARIANT_TYPE_INT32)) {
    long v = strtol(word, &end, 10);
    if (end == word || *end) return failf(err, "'%s' is not an integer", word);
    *out = g_variant_new_int32((gint32)v);
    return true;
  }
  if (g_variant_type_equal(t, G_VARIANT_TYPE_UINT32)) {
    unsigned long v = strtoul(word, &end, 10);
    if (end == word || *end || word[0] == '-') return failf(err, "'%s' is not a non-negative integer", word);
    *out = g_variant_new_uint32((guint32)v);
    return true;
  }
  if (g_variant_type_equal(t, G_VARIANT_TYPE_BOOLEAN)) {
    if (!g_ascii_strcasecmp(word, "true") || !strcmp(word, "1"))  { *out = g_variant_new_boolean(TRUE); return true; }
    if (!g_ascii_strcasecmp(word, "false") || !strcmp(word, "0")) { *out = g_variant_new_boolean(FALSE); return true; }
    return failf(err, "'%s' is not true or false", word);
  }
  return fail(err, "Unsupported parameter type in command table");
}

bool wp_command_parse_args(const WpCommand *cmd, char **argv, GVariant **out, char **err)
{
  if (!cmd->params) { *out = NULL; return true; }
  const GVariantType *t = G_VARIANT_TYPE(cmd->params);
  if (!g_variant_type_is_tuple(t)) return parse_leaf(t, argv[0], out, err);

  GVariantBuilder b;
  g_variant_builder_init(&b, t);
  int i = 0;
  for (const GVariantType *child = g_variant_type_first(t); child; child = g_variant_type_next(child), i++) {
    GVariant *leaf = NULL;
    if (!parse_leaf(child, argv[i], &leaf, err)) { g_variant_builder_clear(&b); return false; }
    g_variant_builder_add_value(&b, leaf);
  }
  *out = g_variant_builder_end(&b);
  return true;
}
