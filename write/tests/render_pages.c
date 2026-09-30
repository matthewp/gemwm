/* Offline check of the Pango engine: lay out a long document, report page
 * count, round-trip caret geometry, and render page 1 to a PNG. */
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/document.h"
#include "layout/layout_pango.h"
#include "io/odt.h"

int main(int argc, char **argv)
{
  const char *out = argc > 1 ? argv[1] : "page1.png";
  WpDocument *d = wp_document_new();
  WpLayoutEngine *e = wp_layout_pango_new();
  wp_layout_set_document(e, d);

  if (argc > 2) {   /* render page 1 of a real .odt instead of the synthetic sample */
    char *err = NULL;
    if (!wp_odt_read(d, argv[2], &err)) { fprintf(stderr, "error: %s\n", err); return 1; }
    printf("paragraphs=%zu pages=%zu\n", d->nparas, wp_layout_page_count(e));
    double z = 2.0;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(d->page.width * z), (int)(d->page.height * z));
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
    cairo_scale(cr, z, z);
    cairo_set_source_rgb(cr, 0, 0, 0);
    wp_layout_render_page(e, 0, cr);
    cairo_destroy(cr);
    cairo_surface_write_to_png(s, out);
    cairo_surface_destroy(s);
    printf("wrote %s\n", out);
    wp_layout_destroy(e);
    wp_document_free(d);
    return 0;
  }

  const char *lorem = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
    "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
    "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit.";

  WpPos p = wp_document_insert_text(d, (WpPos){0,0}, "A Heading", 9, NULL);
  wp_document_set_flags(d, (WpPos){0,0}, p, WP_ATTR_BOLD, true);
  WpTextAttrs body = { 0, 0, NULL };
  for (int i = 0; i < 40; i++) {
    p = wp_document_insert_text(d, p, "\n", 1, &body);
    p = wp_document_insert_text(d, p, lorem, strlen(lorem), &body);
  }
  /* italic + underline a bit of paragraph 2, centre paragraph 3, justify 4 */
  wp_document_set_flags(d, (WpPos){2, 6}, (WpPos){2, 40}, WP_ATTR_ITALIC | WP_ATTR_UNDERLINE, true);
  struct { WpAlign a; } al;
  void set_align(WpParaStyle *st, void *u) { st->align = ((typeof(&al))u)->a; }
  al.a = WP_ALIGN_CENTER;  wp_document_apply_para_style(d, (WpPos){3,0}, (WpPos){3,0}, set_align, &al);
  al.a = WP_ALIGN_JUSTIFY; wp_document_apply_para_style(d, (WpPos){4,0}, (WpPos){4,0}, set_align, &al);

  size_t n = wp_layout_page_count(e);
  printf("paragraphs=%zu pages=%zu\n", d->nparas, n);

  /* caret -> rect -> hit test round trip on a few positions */
  int bad = 0;
  for (size_t para = 0; para < d->nparas; para += 7) {
    for (size_t off = 0; off <= d->paras[para].len; off += 37) {
      WpPos pos = { para, off }, back;
      size_t page; WpRect r;
      if (!wp_layout_caret_rect(e, pos, &page, &r)) { bad++; continue; }
      if (!wp_layout_hit_test(e, page, r.x + 0.1, r.y + r.h / 2, &back)) { bad++; continue; }
      if (!wp_pos_eq(pos, back)) {
        /* caret at a wrap boundary legitimately resolves to the trailing space */
        if (!(back.para == pos.para && back.offset + 1 == pos.offset)) {
          printf("round-trip mismatch (%zu,%zu) -> (%zu,%zu)\n", pos.para, pos.offset, back.para, back.offset);
          bad++;
        }
      }
    }
  }
  printf("round-trip failures=%d\n", bad);

  /* vertical movement walks every line exactly once */
  WpPos cur = { 0, 0 }, next; double xg = -1; size_t steps = 0;
  while (wp_layout_move_vertical(e, cur, 1, &xg, &next)) { cur = next; steps++; }
  printf("lines walked down=%zu (last pos %zu,%zu)\n", steps + 1, cur.para, cur.offset);

  /* render page 0 at 2x */
  double z = 2.0;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(612 * z), (int)(792 * z));
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
  cairo_scale(cr, z, z);
  cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.35);
  void fill(const WpRect *r, void *u) { cairo_rectangle(u, r->x, r->y, r->w, r->h); cairo_fill(u); }
  wp_layout_selection_rects(e, (WpPos){1, 20}, (WpPos){2, 15}, 0, fill, cr);
  cairo_set_source_rgb(cr, 0, 0, 0);
  wp_layout_render_page(e, 0, cr);
  cairo_destroy(cr);
  cairo_surface_write_to_png(s, out);
  cairo_surface_destroy(s);
  printf("wrote %s\n", out);

  wp_layout_destroy(e);
  wp_document_free(d);
  return bad ? 1 : 0;
}
