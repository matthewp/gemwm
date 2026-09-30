#include "gem-draw.h"

void gem_black(cairo_t *cr) { cairo_set_source_rgb(cr, 0, 0, 0); }
void gem_white(cairo_t *cr) { cairo_set_source_rgb(cr, 1, 1, 1); }

void gem_fill(cairo_t *cr, double x, double y, double w, double h) {
	if (w > 0 && h > 0) {
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
	}
}

void gem_frame(cairo_t *cr, int x, int y, int w, int h, int t) {
	gem_fill(cr, x, y, w, t);
	gem_fill(cr, x, y + h - t, w, t);
	gem_fill(cr, x, y, t, h);
	gem_fill(cr, x + w - t, y, t, h);
}

double gem_text_width(cairo_t *cr, const char *s) {
	cairo_text_extents_t te;
	cairo_text_extents(cr, s, &te);
	return te.x_advance;
}

void gem_text(cairo_t *cr, const char *s, double x, double y, double row_h) {
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	cairo_move_to(cr, x,
		y + (int)((row_h - (fe.ascent + fe.descent)) / 2 + fe.ascent));
	cairo_show_text(cr, s);
}

void gem_grey_out(cairo_t *cr, int x, int y, int w, int h) {
	static cairo_pattern_t *checker;
	if (checker == NULL) {
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 2, 2);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		px[0] = 255;
		px[stride + 1] = 255;
		cairo_surface_mark_dirty(s);
		checker = cairo_pattern_create_for_surface(s);
		cairo_surface_destroy(s);
		cairo_pattern_set_extend(checker, CAIRO_EXTEND_REPEAT);
		cairo_pattern_set_filter(checker, CAIRO_FILTER_NEAREST);
	}
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	gem_white(cr);
	cairo_mask(cr, checker);
	cairo_restore(cr);
}

void gem_bitmap(cairo_t *cr, const char *const rows[], int n_rows, int x, int y) {
	for (int r = 0; r < n_rows; r++) {
		for (int c = 0; rows[r][c] != '\0'; c++) {
			if (rows[r][c] == '#') {
				gem_fill(cr, x + c, y + r, 1, 1);
			}
		}
	}
}

const char *gem_font_family(void) {
	static const char *font;
	if (font == NULL) {
		font = g_getenv("GEMWM_FONT") ? g_getenv("GEMWM_FONT") : "monospace";
	}
	return font;
}

int gem_font_size(void) {
	static int size;
	if (size == 0) {
		size = g_getenv("GEMWM_FONT_SIZE") ? atoi(g_getenv("GEMWM_FONT_SIZE")) : 0;
		size = size > 0 ? size : 14;
	}
	return size;
}

void gem_set_font(cairo_t *cr) {
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_select_font_face(cr, gem_font_family(), CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, gem_font_size());
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	cairo_set_font_options(cr, opts);
	cairo_font_options_destroy(opts);
}

double gem_measure(const char *s) {
	static cairo_t *cr;
	if (cr == NULL) {
		cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		cr = cairo_create(img);
		cairo_surface_destroy(img);
		gem_set_font(cr);
	}
	return gem_text_width(cr, s);
}

void gem_draw_pixelated(cairo_t *cr, int w, int h,
		void (*paint)(cairo_t *cr, int w, int h, void *data), void *data) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *c = cairo_create(img);
	gem_set_font(c);
	gem_white(c);
	cairo_paint(c);
	paint(c, w, h, data);
	cairo_destroy(c);

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(img);
}
