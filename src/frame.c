/*
 * GEM-style window frame, drawn with cairo in pure black and white.
 *
 * Layout, for a frame W x H pixels wrapping content of cw x ch (G = gadget),
 * with both scroll bars and a sizer:
 *
 *   +---+---------------- title ----------------+---+
 *   | X |  :::::::::::: [ Title ] :::::::::::::  | <> |   y = 1 .. G
 *   +---+----------------------------------------+---+   y = G+1
 *   |                                            | ^ |
 *   |               client content               |[ ]|  <- slider
 *   |                                            | v |
 *   +---+------------------------------------+---+---+
 *   | < |:::::[    ]::::::::::::::::::::::::::| > | # |
 *   +---+------------------------------------+---+---+
 *
 * A window only has the scroll bars its client reports (gemwm-scroll-v1).
 * Without a vertical bar the right side is a 1px border; with no bars at
 * all a sizer sits in a bottom row of its own, and without a sizer either
 * the frame is just the title bar and a thin border.
 *
 * Lines between parts are 1px and shared by their neighbours, like GEM does.
 */
#include <cairo.h>
#include <stdlib.h>
#include <string.h>
#include "cairo_buffer.h"
#include "frame.h"

#define G GEM_GADGET

struct rect { int x, y, w, h; };

struct frame_layout {
	int w, h; /* total frame size */
	bool column, row; /* right scroll bar column, bottom row */
	struct rect title, closer, fuller, sizer, up, down, vtrack, vslider,
		left, right, htrack, hslider;
};

static bool has_row(const struct frame_style *s) {
	return s->h.on || (s->sizer && !s->v.on);
}

void frame_extents(const struct frame_style *style, int *right, int *bottom) {
	*right = style->v.on ? GEM_GADGET + 2 : 1;
	*bottom = has_row(style) ? GEM_GADGET + 2 : 1;
}

/* The slider inside a track: as long as the visible share of the content
 * (at least a gadget), placed by the scroll position. It fills the track
 * when everything fits. */
static struct rect slider_in(struct rect track, const struct frame_axis *a,
		bool vertical) {
	int len = vertical ? track.h : track.w;
	int size = len, offset = 0;
	if (a->total > a->visible && a->total > 0 && len > 0) {
		size = (int)((double)len * a->visible / a->total);
		if (size < G) {
			size = G < len ? G : len;
		}
		int range = a->total - a->visible;
		int pos = a->position < 0 ? 0 : a->position > range ? range : a->position;
		offset = (int)((double)(len - size) * pos / range + 0.5);
	}
	struct rect r = track;
	if (vertical) {
		r.y += offset;
		r.h = size;
	} else {
		r.x += offset;
		r.w = size;
	}
	return r;
}

static void frame_layout(const struct frame_style *s, int cw, int ch,
		struct frame_layout *l) {
	int right, bottom;
	frame_extents(s, &right, &bottom);
	int W = cw + FRAME_LEFT + right;
	int H = ch + FRAME_TOP + bottom;
	memset(l, 0, sizeof(*l));
	l->w = W;
	l->h = H;
	l->column = s->v.on;
	l->row = has_row(s);
	l->closer = (struct rect){ 1, 1, G, G };
	l->fuller = (struct rect){ W - G - 1, 1, G, G };
	l->title = (struct rect){ G + 2, 1, W - 2 * G - 4, G };

	/* The sizer takes the bottom-right corner: the end of the bottom row,
	 * or the end of the scroll column when there is no row. */
	int corner_x = W - G - 1, corner_y = H - G - 1;
	if (s->sizer) {
		l->sizer = (struct rect){ corner_x, corner_y, G, G };
	}

	if (l->column) {
		int end = l->row ? H - G - 3 : H - 2;      /* last row of the column */
		if (s->sizer && !l->row) {
			end -= G + 1;                          /* the sizer is below */
		}
		l->up = (struct rect){ corner_x, G + 2, G, G };
		l->down = (struct rect){ corner_x, end - G + 1, G, G };
		l->vtrack = (struct rect){ corner_x, 2 * G + 3, G, end - 3 * G - 3 };
		l->vslider = slider_in(l->vtrack, &s->v, true);
	}
	if (s->h.on) {
		int end = l->column || s->sizer ? W - G - 3 : W - 2;
		l->left = (struct rect){ 1, corner_y, G, G };
		l->right = (struct rect){ end - G + 1, corner_y, G, G };
		l->htrack = (struct rect){ G + 2, corner_y, end - 2 * G - 2, G };
		l->hslider = slider_in(l->htrack, &s->h, false);
	}
}

#define IN(r, px, py) \
	((r).w > 0 && (r).h > 0 && \
	 (px) >= (r).x && (px) < (r).x + (r).w && \
	 (py) >= (r).y && (py) < (r).y + (r).h)

enum frame_part frame_part_at(const struct frame_style *style, int cw, int ch,
		int x, int y) {
	struct frame_layout l;
	frame_layout(style, cw, ch, &l);
	if (x < 0 || y < 0 || x >= l.w || y >= l.h) {
		return FRAME_PART_NONE;
	}
	if (IN(l.closer, x, y)) return FRAME_PART_CLOSER;
	if (IN(l.fuller, x, y)) return FRAME_PART_FULLER;
	if (IN(l.title, x, y)) return FRAME_PART_TITLE;
	if (IN(l.sizer, x, y)) return FRAME_PART_SIZER;
	if (IN(l.up, x, y)) return FRAME_PART_UP;
	if (IN(l.down, x, y)) return FRAME_PART_DOWN;
	if (IN(l.vslider, x, y)) return FRAME_PART_VSLIDER;
	if (IN(l.vtrack, x, y)) {
		return y < l.vslider.y ? FRAME_PART_PAGE_UP : FRAME_PART_PAGE_DOWN;
	}
	if (IN(l.left, x, y)) return FRAME_PART_LEFT;
	if (IN(l.right, x, y)) return FRAME_PART_RIGHT;
	if (IN(l.hslider, x, y)) return FRAME_PART_HSLIDER;
	if (IN(l.htrack, x, y)) {
		return x < l.hslider.x ? FRAME_PART_PAGE_LEFT : FRAME_PART_PAGE_RIGHT;
	}
	return FRAME_PART_BORDER;
}

void frame_slider_size(const struct frame_style *style, int cw, int ch,
		bool vertical, int *track, int *slider) {
	struct frame_layout l;
	frame_layout(style, cw, ch, &l);
	*track = vertical ? l.vtrack.h : l.htrack.w;
	*slider = vertical ? l.vslider.h : l.hslider.w;
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

	/* [font] in the config, which puts it in our environment. */
	const char *font = getenv("GEMWM_FONT");
	const char *size = getenv("GEMWM_FONT_SIZE");
	cairo_select_font_face(cr, font ? font : "monospace",
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size && atoi(size) > 0 ? atoi(size) : 14);
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

static void draw_slider(cairo_t *cr, struct rect r) {
	white(cr);
	fill_rect(cr, r.x, r.y, r.w, r.h);
	black(cr);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
	cairo_stroke(cr);
}

void frame_draw(struct wlr_scene_buffer *node, const struct frame_style *style,
		int cw, int ch, const char *title, bool active) {
	struct frame_layout l;
	frame_layout(style, cw, ch, &l);

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
	/* Left of the fuller; with a scroll column it continues to the bottom. */
	vline(cr, l.w - G - 2, 0, l.column ? l.h : G + 1);
	if (l.row) {
		hline(cr, 0, l.h - G - 2, l.w);
		if (!l.column && l.sizer.w > 0) {
			vline(cr, l.sizer.x - 1, l.sizer.y, G);   /* left of the sizer */
		}
	}
	if (l.column) {
		hline(cr, l.up.x, l.up.y + G, G);
		hline(cr, l.down.x, l.down.y - 1, G);
		if (!l.row && l.sizer.w > 0) {
			hline(cr, l.sizer.x, l.sizer.y - 1, G);   /* above the sizer */
		}
	}
	if (style->h.on) {
		vline(cr, l.left.x + G, l.left.y, G);
		vline(cr, l.right.x - 1, l.right.y, G);
	}

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
		if (l.sizer.w > 0) {
			draw_sizer(cr, l.sizer.x, l.sizer.y);
		}
		if (l.column) {
			draw_arrow(cr, l.up.x, l.up.y, FRAME_PART_UP);
			draw_arrow(cr, l.down.x, l.down.y, FRAME_PART_DOWN);
			if (l.vtrack.h > 0) {
				draw_slider(cr, l.vslider);
			}
		}
		if (style->h.on) {
			draw_arrow(cr, l.left.x, l.left.y, FRAME_PART_LEFT);
			draw_arrow(cr, l.right.x, l.right.y, FRAME_PART_RIGHT);
			if (l.htrack.w > 0) {
				draw_slider(cr, l.hslider);
			}
		}
	}

	cairo_destroy(cr);
	cairo_buffer_submit(buffer, node);
}
