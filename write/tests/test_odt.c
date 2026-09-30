#include <math.h>
/* Round-trip a formatted document through ODT and compare. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/document.h"
#include "io/odt.h"
#include "io/zip.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void set_align(WpParaStyle *st, void *u) { st->align = *(WpAlign *)u; }
static void set_spacing(WpParaStyle *st, void *u) { (void)u; st->space_before_pt = 6; st->space_after_pt = 12; st->line_spacing = 1.5f; }
static void set_indent(WpParaStyle *st, void *u) { (void)u; st->indent_left_pt = 20; st->indent_right_pt = 10; st->background = WP_COLOR(0xff, 0xee, 0xcc); st->padding_pt = 4; }
static void set_hanging(WpParaStyle *st, void *u)
{
  (void)u;
  st->indent_left_pt = 72; st->indent_first_pt = -72;
  wp_para_style_add_tab(st, 0, WP_TAB_LEFT);
  wp_para_style_add_tab(st, 144, WP_TAB_DECIMAL);
  wp_para_style_add_tab(st, 100, WP_TAB_RIGHT);
}
static void set_size(WpTextAttrs *a, void *u) { a->size_pt = *(float *)u; }
static void set_family(WpTextAttrs *a, void *u) { a->family = u; }

/* Run attributes are stored relative to the paragraph's base, and a writer
 * may normalise redundant ones away, so compare what the reader would see. */
static WpTextAttrs effective(const WpDocument *d, size_t para, const WpTextAttrs *a)
{
  WpTextAttrs base = wp_document_para_base_attrs(d, para), r = *a;
  r.flags |= base.flags;
  if (r.size_pt <= 0) r.size_pt = base.size_pt;
  if (!r.family) r.family = base.family;
  return r;
}

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "roundtrip.odt";

  WpDocument *d = wp_document_new();
  d->page = (WpPageSetup){ 595.28, 841.89, 56.7, 56.7, 70.9, 70.9 };
  WpPos p = wp_document_insert_text(d, (WpPos){0,0}, "Title here", 10, NULL);
  wp_document_set_flags(d, (WpPos){0,0}, p, WP_ATTR_BOLD, true);
  float big = 18; wp_document_apply_attrs(d, (WpPos){0,0}, p, set_size, &big);
  WpTextAttrs plain = { 0, 0, NULL };
  p = wp_document_insert_text(d, p, "\nA  double  space, <tags> & \"quotes\", tab\there, ünïcödé.", (size_t)-1 == 0 ? 0 : strlen("\nA  double  space, <tags> & \"quotes\", tab\there, ünïcödé."), &plain);
  wp_document_set_flags(d, (WpPos){1,3}, (WpPos){1,16}, WP_ATTR_ITALIC | WP_ATTR_UNDERLINE, true);
  p = wp_document_insert_text(d, p, "\n  leading and trailing spaces  ", strlen("\n  leading and trailing spaces  "), &plain);
  p = wp_document_insert_text(d, p, "\n", 1, &plain);                          /* empty paragraph */
  p = wp_document_insert_text(d, p, "\nMono family run", strlen("\nMono family run"), &plain);
  wp_document_apply_attrs(d, (WpPos){4,0}, (WpPos){4,4}, set_family, (void *)wp_intern("Monospace"));
  wp_document_set_style_name(d, (WpPos){0,0}, (WpPos){0,0}, "Title");
  wp_document_set_style_name(d, (WpPos){4,0}, (WpPos){4,0}, "Heading 2");
  WpAlign al = WP_ALIGN_RIGHT;   wp_document_apply_para_style(d, (WpPos){0,0}, (WpPos){0,0}, set_align, &al);   /* local override on a Title */
  al = WP_ALIGN_JUSTIFY;         wp_document_apply_para_style(d, (WpPos){1,0}, (WpPos){1,0}, set_align, &al);
  wp_document_apply_para_style(d, (WpPos){2,0}, (WpPos){2,0}, set_spacing, NULL);
  p = wp_document_insert_text(d, p, "\nA quotation", strlen("\nA quotation"), &plain);
  wp_document_set_style_name(d, (WpPos){5,0}, (WpPos){5,0}, "Quote");
  p = wp_document_insert_text(d, p, "\nA boxed note", strlen("\nA boxed note"), &plain);
  wp_document_set_style_name(d, (WpPos){6,0}, (WpPos){6,0}, "Note");
  p = wp_document_insert_text(d, p, "\nIndented by hand", strlen("\nIndented by hand"), &plain);
  wp_document_apply_para_style(d, (WpPos){7,0}, (WpPos){7,0}, set_indent, NULL);
  wp_document_apply_para_style(d, (WpPos){3, 0}, (WpPos){3, 0}, set_hanging, NULL);
  uint32_t c1 = wp_document_add_comment(d, (WpPos){1,2}, (WpPos){2,9}, "Ann Author", "2026-03-04T05:06:07", "Across <two> paragraphs\nSecond line");
  uint32_t r1 = wp_document_add_reply(d, c1, "Rae", "2026-03-04T05:07:00", "Agreed & noted");
  uint32_t c2 = wp_document_add_comment(d, (WpPos){4,5}, (WpPos){4,11}, "Bob", "2026-03-04T05:06:08", "family");

  char *err = NULL;
  CHECK(wp_odt_write(d, path, &err));
  if (err) { printf("write error: %s\n", err); free(err); err = NULL; }

  WpDocument *r = wp_document_new();
  CHECK(wp_odt_read(r, path, &err));
  if (err) { printf("read error: %s\n", err); free(err); err = NULL; }

  char *t1 = wp_document_get_all_text(d, NULL), *t2 = wp_document_get_all_text(r, NULL);
  CHECK(strcmp(t1, t2) == 0);
  if (strcmp(t1, t2)) printf("expected:\n%s\ngot:\n%s\n", t1, t2);
  free(t1); free(t2);

  CHECK(r->nparas == d->nparas);
  for (size_t i = 0; i < d->nparas && i < r->nparas; i++) {
    const WpParagraph *a = &d->paras[i], *b = &r->paras[i];
    CHECK(a->nruns == b->nruns);
    for (size_t k = 0; k < a->nruns && k < b->nruns; k++) {
      CHECK(a->runs[k].len == b->runs[k].len);
      WpTextAttrs ea = effective(d, i, &a->runs[k].attrs), eb = effective(r, i, &b->runs[k].attrs);
      CHECK(wp_attrs_equal(&ea, &eb));
      if (!wp_attrs_equal(&ea, &eb))
        printf("  para %zu run %zu: flags %u/%u size %g/%g family %s/%s\n", i, k,
               a->runs[k].attrs.flags, b->runs[k].attrs.flags, a->runs[k].attrs.size_pt, b->runs[k].attrs.size_pt,
               a->runs[k].attrs.family ? a->runs[k].attrs.family : "-", b->runs[k].attrs.family ? b->runs[k].attrs.family : "-");
    }
    CHECK(a->style_name == b->style_name);
    if (a->style_name != b->style_name) printf("  para %zu style %s/%s\n", i, a->style_name, b->style_name);
    CHECK(a->style.align == b->style.align);
    CHECK(a->style.space_before_pt == b->style.space_before_pt);
    CHECK(a->style.space_after_pt == b->style.space_after_pt);
    CHECK(a->style.line_spacing == b->style.line_spacing);
    CHECK(fabs(a->style.indent_left_pt - b->style.indent_left_pt) < 0.01 && fabs(a->style.indent_right_pt - b->style.indent_right_pt) < 0.01);
    CHECK(fabs(a->style.padding_pt - b->style.padding_pt) < 0.01 && fabs(a->style.border_left_pt - b->style.border_left_pt) < 0.01);
    CHECK(a->style.border_color == b->style.border_color && a->style.background == b->style.background);
    CHECK(fabs(a->style.indent_first_pt - b->style.indent_first_pt) < 0.01);
    CHECK(a->style.ntabs == b->style.ntabs && memcmp(a->style.tabs, b->style.tabs, sizeof a->style.tabs) == 0);
    if (a->style.ntabs != b->style.ntabs) printf("  para %zu tabs %d/%d\n", i, a->style.ntabs, b->style.ntabs);
    if (a->style.background != b->style.background) printf("  para %zu background %x/%x\n", i, a->style.background, b->style.background);
  }
  CHECK(strcmp(wp_document_style_of(r, 5)->name, "Quote") == 0 && (wp_document_para_base_attrs(r, 5).flags & WP_ATTR_ITALIC));
  CHECK(strcmp(wp_document_style_of(r, 6)->name, "Note") == 0 && wp_para_style_boxed(&r->paras[6].style));
  CHECK(strcmp(wp_document_style_of(r, 7)->name, WP_STYLE_STANDARD) == 0 && r->paras[7].style.indent_left_pt == 20);
  WpPos ca, cb;
  CHECK(r->ncomments == 3);
  CHECK(wp_document_comment(r, r1) && wp_document_comment(r, r1)->parent == c1);
  CHECK(wp_document_comment(r, r1) && strcmp(wp_document_comment(r, r1)->text, "Agreed & noted") == 0);
  CHECK(wp_document_comment(r, r1) && strcmp(wp_document_comment(r, r1)->author, "Rae") == 0);
  CHECK(wp_document_comment(r, c2) && wp_document_comment(r, c2)->parent == 0);
  CHECK(wp_document_comment(r, c1) && strcmp(wp_document_comment(r, c1)->author, "Ann Author") == 0);
  CHECK(wp_document_comment(r, c1) && strcmp(wp_document_comment(r, c1)->date, "2026-03-04T05:06:07") == 0);
  CHECK(wp_document_comment(r, c1) && strcmp(wp_document_comment(r, c1)->text, "Across <two> paragraphs\nSecond line") == 0);
  CHECK(wp_document_comment_range(r, c1, &ca, &cb) && ca.para == 1 && ca.offset == 2 && cb.para == 2 && cb.offset == 9);
  CHECK(wp_document_comment(r, c2) && strcmp(wp_document_comment(r, c2)->text, "family") == 0);
  CHECK(wp_document_comment_range(r, c2, &ca, &cb) && ca.para == 4 && ca.offset == 5 && cb.offset == 11);
  CHECK(wp_document_style_of(r, 4)->outline_level == 2);
  CHECK(wp_document_find_style(r, "Heading 2")->text.size_pt == 16.0f);
  CHECK(r->default_attrs.family == d->default_attrs.family);
  CHECK(r->default_attrs.size_pt == d->default_attrs.size_pt);
  CHECK(fabs(r->page.width - d->page.width) < 0.01 && fabs(r->page.height - d->page.height) < 0.01);
  CHECK(fabs(r->page.margin_left - d->page.margin_left) < 0.01 && fabs(r->page.margin_top - d->page.margin_top) < 0.01);

  /* A file as another editor writes it: a point comment (no range) lands on
   * the next character, and a ranged one ends where its end marker is. */
  {
    static const char *content =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
      " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
      " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
      " xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\" office:version=\"1.3\">"
      "<office:styles><style:style style:name=\"Quotations\" style:display-name=\"Block Quotation\" style:family=\"paragraph\">"
      "<style:paragraph-properties fo:margin-left=\"1cm\" fo:margin-right=\"1cm\" fo:text-indent=\"0.5cm\">"
      "<style:tab-stops><style:tab-stop style:position=\"2cm\"/><style:tab-stop style:position=\"1cm\" style:type=\"center\"/>"
      "<style:tab-stop style:position=\"3cm\" style:type=\"char\" style:char=\",\"/></style:tab-stops>"
      "</style:paragraph-properties></style:style></office:styles>"
      "<office:body><office:text>"
      "<text:p>One <office:annotation><dc:creator>Cy</dc:creator><dc:date>2026-01-01T00:00:00</dc:date><text:p>point</text:p></office:annotation>two "
      "<office:annotation office:name=\"__Annotation__1\"><dc:creator>Di</dc:creator><text:p>range</text:p></office:annotation>"
      "<office:annotation loext:parent-name=\"__Annotation__1\"><dc:creator>Ed</dc:creator><text:p>linked reply</text:p></office:annotation>"
      "<office:annotation><dc:creator>Flo</dc:creator><text:p>adjacent reply</text:p></office:annotation>three</text:p>"
      "<text:p>four<office:annotation-end office:name=\"__Annotation__1\"/> five</text:p>"
      "<text:p text:style-name=\"Quotations\">quoted</text:p>"
      "</office:text></office:body></office:document-content>";
    g_autofree char *foreign = g_strconcat(path, ".foreign.odt", NULL);
    WpZipWriter *z = wp_zip_writer_open(foreign, &err);
    CHECK(z != NULL);
    if (z) {
      const char *mime = "application/vnd.oasis.opendocument.text";
      wp_zip_writer_add(z, "mimetype", mime, strlen(mime), true);
      wp_zip_writer_add(z, "content.xml", content, strlen(content), false);
      CHECK(wp_zip_writer_close(z, &err));
    }
    WpDocument *f = wp_document_new();
    CHECK(wp_odt_read(f, foreign, &err));
    if (err) { printf("read error: %s\n", err); free(err); err = NULL; }
    char *ft = wp_document_get_all_text(f, NULL);
    CHECK(strcmp(ft, "One two three\nfour five\nquoted") == 0);
    CHECK(strcmp(wp_document_style_of(f, 2)->name, "Quote") == 0);
    CHECK(fabs(f->paras[2].style.indent_left_pt - 72 / 2.54) < 0.05);   /* the file's 1cm wins over our default */
    CHECK(fabs(f->paras[2].style.indent_first_pt - 36 / 2.54) < 0.05);
    CHECK(f->paras[2].style.ntabs == 3 && f->paras[2].style.tabs[0].kind == WP_TAB_CENTER &&
          fabs(f->paras[2].style.tabs[1].pos_pt - 144 / 2.54) < 0.05 && f->paras[2].style.tabs[2].kind == WP_TAB_DECIMAL);
    free(ft);
    CHECK(f->ncomments == 4);
    CHECK(wp_document_comment(f, 3) && wp_document_comment(f, 3)->parent == 2 && strcmp(wp_document_comment(f, 3)->text, "linked reply") == 0);
    CHECK(wp_document_comment(f, 4) && wp_document_comment(f, 4)->parent == 2 && strcmp(wp_document_comment(f, 4)->author, "Flo") == 0);
    CHECK(wp_document_comment(f, 1) && wp_document_comment(f, 1)->parent == 0);
    CHECK(wp_document_comment_range(f, 1, &ca, &cb) && ca.para == 0 && ca.offset == 4 && cb.offset == 5);   /* "t" of two */
    CHECK(wp_document_comment(f, 1) && strcmp(wp_document_comment(f, 1)->text, "point") == 0);
    CHECK(wp_document_comment_range(f, 2, &ca, &cb) && ca.para == 0 && ca.offset == 8 && cb.para == 1 && cb.offset == 4);
    CHECK(wp_document_comment(f, 2) && strcmp(wp_document_comment(f, 2)->author, "Di") == 0);
    wp_document_free(f);
  }

  /* Lists round-trip: nesting, mixed kinds under one list, a list that
   * starts below the top level, and the numbering they imply. */
  {
    WpDocument *l = wp_document_new();
    wp_document_insert_text(l, (WpPos){0, 0}, "one\ntwo\nsub\nthree\nbody\ndeep", 24, NULL);
    wp_document_set_style_name(l, (WpPos){0, 0}, (WpPos){1, 0}, WP_STYLE_LIST_NUMBER);
    l->paras[1].style.list_level = 1; l->paras[1].style.indent_left_pt = wp_list_indent(1);
    wp_document_set_style_name(l, (WpPos){2, 0}, (WpPos){2, 0}, WP_STYLE_LIST_BULLET);
    l->paras[2].style.list_level = 2; l->paras[2].style.indent_left_pt = wp_list_indent(2);
    wp_document_set_style_name(l, (WpPos){3, 0}, (WpPos){3, 0}, WP_STYLE_LIST_NUMBER);
    wp_document_set_style_name(l, (WpPos){5, 0}, (WpPos){5, 0}, WP_STYLE_LIST_BULLET);
    l->paras[5].style.list_level = 1; l->paras[5].style.indent_left_pt = 100;   /* an indent of its own */
    g_autofree char *lpath = g_strconcat(path, ".lists.odt", NULL);
    CHECK(wp_odt_write(l, lpath, &err));
    WpDocument *m = wp_document_new();
    CHECK(wp_odt_read(m, lpath, &err));
    if (err) { printf("read error: %s\n", err); free(err); err = NULL; }
    CHECK(m->nparas == 6);
    static const struct { WpListKind kind; int level; const char *style; } want[] = {
      { WP_LIST_NUMBER, 0, WP_STYLE_LIST_NUMBER }, { WP_LIST_NUMBER, 1, WP_STYLE_LIST_NUMBER },
      { WP_LIST_BULLET, 2, WP_STYLE_LIST_BULLET }, { WP_LIST_NUMBER, 0, WP_STYLE_LIST_NUMBER },
      { WP_LIST_NONE, 0, WP_STYLE_STANDARD },      { WP_LIST_BULLET, 1, WP_STYLE_LIST_BULLET },
    };
    for (size_t i = 0; i < 6 && i < m->nparas; i++) {
      CHECK(m->paras[i].style.list_kind == want[i].kind);
      CHECK(m->paras[i].style.list_level == want[i].level);
      CHECK(strcmp(wp_document_style_of(m, i)->name, want[i].style) == 0);
      CHECK(fabs(m->paras[i].style.indent_left_pt - l->paras[i].style.indent_left_pt) < 0.01);
      if (m->paras[i].style.list_kind != want[i].kind || m->paras[i].style.list_level != want[i].level)
        printf("  para %zu list %d/%d level %d/%d\n", i, m->paras[i].style.list_kind, want[i].kind, m->paras[i].style.list_level, want[i].level);
    }
    int num[6];
    wp_document_list_numbers(m, num);
    CHECK(num[0] == 1 && num[1] == 1 && num[2] == 0 && num[3] == 2 && num[4] == 0 && num[5] == 0);
    wp_document_free(l);
    wp_document_free(m);
  }

  /* A list as LibreOffice writes one: the style is on the outer list only,
   * the paragraphs set no indent so the level's label alignment gives it,
   * a second paragraph in an item is a continuation, a list-header has no
   * label, and its List Bullet paragraph style numbers on its own. */
  {
    static const char *content =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
      " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
      " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.3\">"
      "<office:styles><style:style style:name=\"List_20_Bullet\" style:display-name=\"List Bullet\" style:family=\"paragraph\"/></office:styles>"
      "<office:automatic-styles>"
      "<text:list-style style:name=\"L1\">"
      "<text:list-level-style-number text:level=\"1\" style:num-format=\"1\"><style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
      "<style:list-level-label-alignment fo:margin-left=\"1.27cm\" fo:text-indent=\"-0.635cm\"/></style:list-level-properties></text:list-level-style-number>"
      "<text:list-level-style-bullet text:level=\"2\" text:bullet-char=\"\xe2\x80\xa2\"><style:list-level-properties text:space-before=\"1cm\" text:min-label-width=\"0.5cm\"/></text:list-level-style-bullet>"
      "</text:list-style>"
      "</office:automatic-styles>"
      "<office:body><office:text>"
      "<text:list text:style-name=\"L1\">"
      "<text:list-header><text:p>header</text:p></text:list-header>"
      "<text:list-item><text:p>first</text:p><text:p>more of first</text:p>"
      "<text:list><text:list-item><text:p>inner</text:p></text:list-item></text:list></text:list-item>"
      "<text:list-item><text:h text:outline-level=\"1\">heading</text:h></text:list-item>"
      "</text:list>"
      "<text:p text:style-name=\"List_20_Bullet\">styled</text:p>"
      "</office:text></office:body></office:document-content>";
    g_autofree char *foreign = g_strconcat(path, ".lists-foreign.odt", NULL);
    WpZipWriter *z = wp_zip_writer_open(foreign, &err);
    CHECK(z != NULL);
    if (z) {
      const char *mime = "application/vnd.oasis.opendocument.text";
      wp_zip_writer_add(z, "mimetype", mime, strlen(mime), true);
      wp_zip_writer_add(z, "content.xml", content, strlen(content), false);
      CHECK(wp_zip_writer_close(z, &err));
    }
    WpDocument *f = wp_document_new();
    CHECK(wp_odt_read(f, foreign, &err));
    if (err) { printf("read error: %s\n", err); free(err); err = NULL; }
    char *ft = wp_document_get_all_text(f, NULL);
    CHECK(strcmp(ft, "header\nfirst\nmore of first\ninner\nheading\nstyled") == 0);
    free(ft);
    CHECK(f->nparas == 6);
    if (f->nparas == 6) {
      CHECK(f->paras[0].style.list_kind == WP_LIST_NONE && fabs(f->paras[0].style.indent_left_pt - 36) < 0.05);
      CHECK(f->paras[1].style.list_kind == WP_LIST_NUMBER && f->paras[1].style.list_level == 0 && fabs(f->paras[1].style.indent_left_pt - 36) < 0.05);
      CHECK(f->paras[2].style.list_kind == WP_LIST_NONE && fabs(f->paras[2].style.indent_left_pt - 36) < 0.05);
      CHECK(f->paras[3].style.list_kind == WP_LIST_BULLET && f->paras[3].style.list_level == 1 && fabs(f->paras[3].style.indent_left_pt - 1.5 * 72 / 2.54) < 0.05);
      CHECK(f->paras[4].style.list_kind == WP_LIST_NONE && strcmp(wp_document_style_of(f, 4)->name, "Heading 1") == 0);
      CHECK(f->paras[5].style.list_kind == WP_LIST_BULLET && strcmp(wp_document_style_of(f, 5)->name, WP_STYLE_LIST_BULLET) == 0);
      CHECK(fabs(f->paras[5].style.indent_left_pt - 36) < 0.05);
    }
    wp_document_free(f);
  }

  /* A missing file and a non-zip file must fail cleanly. */
  CHECK(!wp_odt_read(r, "/nonexistent/file.odt", &err)); free(err); err = NULL;

  wp_document_free(d);
  wp_document_free(r);
  printf(failures ? "%d failure(s)\n" : "all odt tests passed\n", failures);
  return failures ? 1 : 0;
}
