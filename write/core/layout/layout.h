/* layout.h — the layout engine boundary.
 *
 * A layout engine turns a WpDocument (text plus its page setup) into pages, and
 * answers geometric questions about them. Everything above this interface
 * (the view, the window) is engine-agnostic; everything below it may be
 * replaced wholesale — a C++ or Rust engine only needs to fill in the
 * vtable. Cairo is the one shared dependency, chosen because any engine can
 * draw into a cairo context.
 *
 * Units: points (1/72 in). Page coordinates have their origin at the
 * top-left corner of the page. Positions are document positions (WpPos).
 */
#ifndef WP_LAYOUT_H
#define WP_LAYOUT_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "doc/document.h"

typedef struct WpRect { double x, y, w, h; } WpRect;

typedef void (*WpRectFn)(const WpRect *r, void *user);

typedef struct WpLayoutEngine WpLayoutEngine;

typedef struct WpLayoutEngineVTable {
  const char *name;

  void   (*destroy)(WpLayoutEngine *e);
  /* The engine reads page setup from doc->page. */
  void   (*set_document)(WpLayoutEngine *e, WpDocument *doc);
  /* Paragraphs >= first_para changed (inserted, removed, or edited). */
  void   (*invalidate)(WpLayoutEngine *e, size_t first_para);

  size_t (*page_count)(WpLayoutEngine *e);
  /* Draw the content of one page. The view has already painted the page
   * background and set the source colour for text. */
  void   (*render_page)(WpLayoutEngine *e, size_t page, cairo_t *cr);

  /* Point on a page -> nearest document position. */
  bool   (*hit_test)(WpLayoutEngine *e, size_t page, double x, double y, WpPos *out);
  /* Document position -> caret rectangle and the page it is on. */
  bool   (*caret_rect)(WpLayoutEngine *e, WpPos pos, size_t *page, WpRect *rect);
  /* Move to the line above (dir < 0) or below (dir > 0). x_goal is the
   * desired x in page coordinates; if negative on entry it is initialised
   * from pos. Returns false when there is no such line. */
  bool   (*move_vertical)(WpLayoutEngine *e, WpPos pos, int dir, double *x_goal, WpPos *out);
  /* Start and end positions of the visual line containing pos. */
  bool   (*line_bounds)(WpLayoutEngine *e, WpPos pos, WpPos *start, WpPos *end);
  /* Rectangles covering the selection [a, b) that fall on `page`. */
  void   (*selection_rects)(WpLayoutEngine *e, WpPos a, WpPos b, size_t page, WpRectFn fn, void *user);
  /* Whether commented text is marked when drawn (on by default). Printing
   * turns the marks off: they're for the screen, not the paper. */
  void   (*set_comment_marks)(WpLayoutEngine *e, bool shown);
} WpLayoutEngineVTable;

struct WpLayoutEngine {
  const WpLayoutEngineVTable *vt;
};

static inline void wp_layout_destroy(WpLayoutEngine *e) { if (e) e->vt->destroy(e); }
static inline void wp_layout_set_document(WpLayoutEngine *e, WpDocument *d) { e->vt->set_document(e, d); }
static inline void wp_layout_invalidate(WpLayoutEngine *e, size_t first) { e->vt->invalidate(e, first); }
static inline size_t wp_layout_page_count(WpLayoutEngine *e) { return e->vt->page_count(e); }
static inline void wp_layout_render_page(WpLayoutEngine *e, size_t page, cairo_t *cr) { e->vt->render_page(e, page, cr); }
static inline bool wp_layout_hit_test(WpLayoutEngine *e, size_t page, double x, double y, WpPos *out) { return e->vt->hit_test(e, page, x, y, out); }
static inline bool wp_layout_caret_rect(WpLayoutEngine *e, WpPos pos, size_t *page, WpRect *r) { return e->vt->caret_rect(e, pos, page, r); }
static inline bool wp_layout_move_vertical(WpLayoutEngine *e, WpPos pos, int dir, double *xg, WpPos *out) { return e->vt->move_vertical(e, pos, dir, xg, out); }
static inline bool wp_layout_line_bounds(WpLayoutEngine *e, WpPos pos, WpPos *s, WpPos *end) { return e->vt->line_bounds(e, pos, s, end); }
static inline void wp_layout_set_comment_marks(WpLayoutEngine *e, bool shown) { e->vt->set_comment_marks(e, shown); }
static inline void wp_layout_selection_rects(WpLayoutEngine *e, WpPos a, WpPos b, size_t page, WpRectFn fn, void *u) { e->vt->selection_rects(e, a, b, page, fn, u); }

#endif
