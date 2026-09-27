/*
 * GEM-style window frame, drawn with cairo in pure black and white.
 *
 * Layout, for a frame W x H pixels wrapping content of cw x ch (G = gadget):
 *
 *   +---+---------------- title ----------------+---+
 *   | X |  :::::::::::: [ Title ] :::::::::::::  | <> |   y = 1 .. G
 *   +---+----------------------------------------+---+   y = G+1
 *   |                                            | ^ |
 *   |               client content               |:::|
 *   |                                            | v |
 *   +---+------------------------------------+---+---+
 *   | < |::::::::::::::::::::::::::::::::::::| > | # |
 *   +---+------------------------------------+---+---+
 *
 * Lines between parts are 1px and shared by their neighbours, like GEM does.
 */
#include <cairo.h>
#include <stdlib.h>
#include <string.h>
#include "cairo_buffer.h"
#include "frame.h"

#define G GEM_GADGET

struct frame_layout {
	int w, h; /* total frame size */
	struct { int x, y, w, h; } title, closer, fuller, sizer,
		up, down, vtrack, left, right, htrack;
};

static void frame_layout(int cw, int ch, struct frame_layout *l) {
	int W = cw + FRAME_LEFT + FRAME_RIGHT;
	int H = ch + FRAME_TOP + FRAME_BOTTOM;
	l->w = W;
	l->h = H;
	l->closer.x = 1;          l->closer.y = 1;
	l->closer.w = G;          l->closer.h = G;
	l->fuller.x = W - G - 1;  l->fuller.y = 1;
	l->fuller.w = G;          l->fuller.h = G;
	l->title.x = G + 2;       l->title.y = 1;
	l->title.w = W - 2 * G - 4; l->title.h = G;

	/* Vertical scroll bar, right of the content. */
	int sx = W - G - 1;
	l->up.x = sx;             l->up.y = G + 2;
	l->up.w = G;              l->up.h = G;
	l->down.x = sx;           l->down.y = H - 2 * G - 2;
	l->down.w = G;            l->down.h = G;
	l->vtrack.x = sx;         l->vtrack.y = 2 * G + 3;
	l->vtrack.w = G;          l->vtrack.h = ch - 2 * G - 2;

	/* Horizontal scroll bar, below the content. */
	int sy = H - G - 1;
	l->left.x = 1;            l->left.y = sy;
	l->left.w = G;            l->left.h = G;
	l->right.x = W - 2 * G - 2; l->right.y = sy;
	l->right.w = G;           l->right.h = G;
	l->htrack.x = G + 2;      l->htrack.y = sy;
	l->htrack.w = cw - 2 * G - 2; l->htrack.h = G;

	l->sizer.x = sx;          l->sizer.y = sy;
	l->sizer.w = G;           l->sizer.h = G;
}

#define IN(r, px, py) \
	((px) >= (r).x && (px) < (r).x + (r).w && \
	 (py) >= (r).y && (py) < (r).y + (r).h)

enum frame_part frame_part_at(int cw, int ch, int x, int y) {
	struct frame_layout l;
	frame_layout(cw, ch, &l);
	if (x < 0 || y < 0 || x >= l.w || y >= l.h) {
		return FRAME_PART_NONE;
	}
	if (IN(l.closer, x, y)) return FRAME_PART_CLOSER;
	if (IN(l.fuller, x, y)) return FRAME_PART_FULLER;
	if (IN(l.title, x, y)) return FRAME_PART_TITLE;
	if (IN(l.sizer, x, y)) return FRAME_PART_SIZER;
	if (IN(l.up, x, y)) return FRAME_PART_UP;
	if (IN(l.down, x, y)) return FRAME_PART_DOWN;
	if (IN(l.vtrack, x, y)) return FRAME_PART_VTRACK;
	if (IN(l.left, x, y)) return FRAME_PART_LEFT;
	if (IN(l.right, x, y)) return FRAME_PART_RIGHT;
	if (IN(l.htrack, x, y)) return FRAME_PART_HTRACK;
	return FRAME_PART_BORDER;
}

/* Builds a repeating 1-bit fill pattern from rows of '#' (black) and '.'. */
static cairo_pattern_t *fill_pattern(const char *rows[], int w, int h) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s) / 4;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			px[y * stride + x] = rows[y][x] == '#' ? 0xff000000 : 0xffffffff;
		}
	}
	cairo_surface_mark_dirty(s);
	cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
	cairo_surface_destroy(s);
	cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
	return p;
}

static void black(cairo_t *cr) { cairo_set_source_rgb(cr, 0, 0, 0); }
static void white(cairo_t *cr) { cairo_set_source_rgb(cr, 1, 1, 1); }

static void fill_rect(cairo_t *cr, int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) {
		return;
	}
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void hline(cairo_t *cr, int x, int y, int w) { fill_rect(cr, x, y, w, 1); }
static void vline(cairo_t *cr, int x, int y, int h) { fill_rect(cr, x, y, 1, h); }

static void triangle(cairo_t *cr, double x1, double y1, double x2, double y2,
		double x3, double y3) {
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_line_to(cr, x3, y3);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void draw_arrow(cairo_t *cr, int x, int y, enum frame_part dir) {
	double c = G / 2.0;
	black(cr);
	switch (dir) {
	case FRAME_PART_UP:
		triangle(cr, x + c, y + 4, x + G - 4, y + G - 5, x + 4, y + G - 5);
		break;
	case FRAME_PART_DOWN:
		triangle(cr, x + c, y + G - 4, x + G - 4, y + 5, x + 4, y + 5);
		break;
	case FRAME_PART_LEFT:
		triangle(cr, x + 4, y + c, x + G - 5, y + 4, x + G - 5, y + G - 4);
		break;
	case FRAME_PART_RIGHT:
		triangle(cr, x + G - 4, y + c, x + 5, y + 4, x + 5, y + G - 4);
		break;
	default:
		break;
	}
}

static void draw_closer(cairo_t *cr, int x, int y) {
	black(cr);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 4.5, y + 4.5, G - 9, G - 9);
	cairo_move_to(cr, x + 4.5, y + 4.5);
	cairo_line_to(cr, x + G - 4.5, y + G - 4.5);
	cairo_move_to(cr, x + G - 4.5, y + 4.5);
	cairo_line_to(cr, x + 4.5, y + G - 4.5);
	cairo_stroke(cr);
}

static void draw_fuller(cairo_t *cr, int x, int y) {
	double c = G / 2.0;
	black(cr);
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, x + c, y + 3.5);
	cairo_line_to(cr, x + G - 3.5, y + c);
	cairo_line_to(cr, x + c, y + G - 3.5);
	cairo_line_to(cr, x + 3.5, y + c);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

static void draw_sizer(cairo_t *cr, int x, int y) {
	black(cr);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 3.5, y + 3.5, G - 7, G - 7);
	cairo_rectangle(cr, x + 3.5, y + 3.5, 6, 6);
	cairo_stroke(cr);
}

static void draw_title(cairo_t *cr, const struct frame_layout *l,
		const char *title, bool active) {
	static const char *title_rows[] = { "#.", "..", ".#", ".." };
	if (active) {
		cairo_pattern_t *p = fill_pattern(title_rows, 2, 4);
		cairo_set_source(cr, p);
		fill_rect(cr, l->title.x, l->title.y, l->title.w, l->title.h);
		cairo_pattern_destroy(p);
	}
	if (title == NULL || title[0] == '\0') {
		return;
	}

	const char *font = getenv("GEMWM_FONT");
	cairo_select_font_face(cr, font ? font : "monospace",
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	cairo_set_font_options(cr, opts);
	cairo_font_options_destroy(opts);

	cairo_text_extents_t te;
	cairo_font_extents_t fe;
	cairo_text_extents(cr, title, &te);
	cairo_font_extents(cr, &fe);

	/* GEM draws the title on a white box, padded by one space each side. */
	int pad = 8;
	int box_w = (int)te.x_advance + 2 * pad;
	if (box_w > l->title.w) {
		box_w = l->title.w;
	}
	int box_x = l->title.x + (l->title.w - box_w) / 2;

	cairo_save(cr);
	cairo_rectangle(cr, l->title.x, l->title.y, l->title.w, l->title.h);
	cairo_clip(cr);
	white(cr);
	fill_rect(cr, box_x, l->title.y, box_w, l->title.h);
	black(cr);
	int baseline = l->title.y +
		(int)((l->title.h - (fe.ascent + fe.descent)) / 2 + fe.ascent);
	cairo_rectangle(cr, box_x, l->title.y, box_w, l->title.h);
	cairo_clip(cr);
	cairo_move_to(cr, box_x + pad, baseline);
	cairo_show_text(cr, title);
	cairo_restore(cr);
}

void frame_draw(struct wlr_scene_buffer *node, int cw, int ch,
		const char *title, bool active) {
	struct frame_layout l;
	frame_layout(cw, ch, &l);

	struct cairo_buffer *buffer = cairo_buffer_create(l.w, l.h);
	if (buffer == NULL) {
		return;
	}
	cairo_t *cr = cairo_create(buffer->surface);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

	/* Everything outside the lines and gadgets is white. The content area is
	 * covered by the client, so its colour doesn't matter. */
	white(cr);
	cairo_paint(cr);

	black(cr);
	/* Outer border. */
	hline(cr, 0, 0, l.w);
	hline(cr, 0, l.h - 1, l.w);
	vline(cr, 0, 0, l.h);
	vline(cr, l.w - 1, 0, l.h);
	/* Title bar separators. */
	hline(cr, 0, G + 1, l.w);
	vline(cr, G + 1, 0, G + 1);
	/* The line left of the fuller continues down the scroll bar column. */
	vline(cr, l.w - G - 2, 0, l.h);
	/* Line above the horizontal scroll bar. */
	hline(cr, 0, l.h - G - 2, l.w);
	/* Separators around arrows. */
	hline(cr, l.up.x, l.up.y + G, G);
	hline(cr, l.down.x, l.down.y - 1, G);
	vline(cr, l.left.x + G, l.left.y, G);
	vline(cr, l.right.x - 1, l.right.y, G);

	draw_title(cr, &l, title, active);

	/* GEM hides the gadgets of windows that aren't on top. */
	if (active) {
		static const char *track_rows[] = { "#.", ".#" };
		cairo_pattern_t *p = fill_pattern(track_rows, 2, 2);
		cairo_set_source(cr, p);
		fill_rect(cr, l.vtrack.x, l.vtrack.y, l.vtrack.w, l.vtrack.h);
		fill_rect(cr, l.htrack.x, l.htrack.y, l.htrack.w, l.htrack.h);
		cairo_pattern_destroy(p);

		draw_closer(cr, l.closer.x, l.closer.y);
		draw_fuller(cr, l.fuller.x, l.fuller.y);
		draw_sizer(cr, l.sizer.x, l.sizer.y);
		draw_arrow(cr, l.up.x, l.up.y, FRAME_PART_UP);
		draw_arrow(cr, l.down.x, l.down.y, FRAME_PART_DOWN);
		draw_arrow(cr, l.left.x, l.left.y, FRAME_PART_LEFT);
		draw_arrow(cr, l.right.x, l.right.y, FRAME_PART_RIGHT);
	}

	cairo_destroy(cr);
	cairo_buffer_submit(buffer, node);
}
