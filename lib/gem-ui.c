#include <string.h>
#include "gem-ui.h"

void gem_ui_load_css(void) {
	char *css = g_strdup_printf(
		"window.gem { background: #fff; color: #000; }"
		".gem-field { background: #fff; color: #000; border: 1px solid #000;"
		"  border-radius: 0; box-shadow: none; outline: none;"
		"  min-height: %dpx; padding: 0 6px; font-family: \"%s\";"
		"  font-size: %dpx; caret-color: #000; min-width: 0; }"
		".gem-field > text { min-width: 0; }"
		".gem-field:focus-within { box-shadow: none; outline: none; }"
		".gem-field text selection, .gem-field selection {"
		"  background: #000; color: #fff; }"
		"textview.gem-text, textview.gem-text text { background: #fff;"
		"  color: #000; font-family: \"%s\"; font-size: %dpx;"
		"  caret-color: #000; }"
		"textview.gem-text text selection { background: #000; color: #fff; }"
		"label.gem-text { color: #000; font-family: \"%s\";"
		"  font-size: %dpx; }"
		"popover.gem-popup, popover.gem-popup > contents { background: none;"
		"  border: none; border-radius: 0; box-shadow: none; padding: 0;"
		"  margin: 0; }",
		GEM_FIELD_H - 2, gem_font_family(), gem_font_size(),
		gem_font_family(), gem_font_size(), gem_font_family(), gem_font_size());
	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(provider);
	g_free(css);
}

GtkWidget *gem_field_new(int x, int y, int w) {
	GtkWidget *f = gtk_entry_new();
	gtk_widget_add_css_class(f, "gem-field");
	/* Or GTK makes it as wide as its idea of a field. */
	gtk_editable_set_width_chars(GTK_EDITABLE(f), 1);
	gtk_widget_set_halign(f, GTK_ALIGN_START);
	gtk_widget_set_valign(f, GTK_ALIGN_START);
	gtk_widget_set_margin_start(f, x);
	gtk_widget_set_margin_top(f, y);
	gtk_widget_set_size_request(f, w, GEM_FIELD_H);
	return f;
}

/* ---- Hits ----------------------------------------------------------------- */

GArray *gem_hits_new(void) {
	return g_array_new(FALSE, FALSE, sizeof(struct gem_hit));
}

void gem_hit_add(GArray *hits, int x, int y, int w, int h, int id, int index) {
	struct gem_hit hit = { x, y, w, h, id, index };
	g_array_append_val(hits, hit);
}

const struct gem_hit *gem_hit_at(GArray *hits, double x, double y) {
	/* The last drawn is on top. */
	for (guint i = hits != NULL ? hits->len : 0; i-- > 0;) {
		const struct gem_hit *h = &g_array_index(hits, struct gem_hit, i);
		if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) {
			return h;
		}
	}
	return NULL;
}

/* ---- Pixel areas ---------------------------------------------------------- */

struct pixel_area {
	gem_paint_fn paint;
	void *data;
};

static void draw_pixel_area(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	struct pixel_area *p = data;
	gem_draw_pixelated(cr, w, h, p->paint, p->data);
}

GtkWidget *gem_pixel_area_new(int w, int h, gem_paint_fn paint, void *data) {
	GtkWidget *area = gtk_drawing_area_new();
	if (w > 0) {
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), w);
	}
	if (h > 0) {
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), h);
	}
	struct pixel_area *p = g_new(struct pixel_area, 1);
	p->paint = paint;
	p->data = data;
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_pixel_area, p,
		g_free);
	return area;
}

/* ---- Drawing -------------------------------------------------------------- */

int gem_button_width(const char *label) {
	return (int)gem_measure(label) + 2 * GEM_PAD;
}

int gem_button(cairo_t *cr, GArray *hits, int x, int y, int w,
		const char *label, int id, bool is_default) {
	if (w <= 0) {
		w = gem_button_width(label);
	}
	gem_white(cr);
	gem_fill(cr, x, y, w, GEM_BUTTON_H);
	gem_black(cr);
	gem_frame(cr, x, y, w, GEM_BUTTON_H, is_default ? 2 : 1);
	gem_text(cr, label, x + (w - (int)gem_text_width(cr, label)) / 2, y,
		GEM_BUTTON_H);
	if (hits != NULL) {
		gem_hit_add(hits, x, y, w, GEM_BUTTON_H, id, 0);
	}
	return w;
}

int gem_checkbox(cairo_t *cr, GArray *hits, int x, int y, const char *label,
		bool on, int id) {
	int box = 13, by = y + (GEM_ROW_H - box) / 2;
	gem_black(cr);
	gem_frame(cr, x, by, box, box, 1);
	if (on) {
		for (int i = 2; i < box - 2; i++) {
			gem_fill(cr, x + i, by + i, 1, 1);
			gem_fill(cr, x + box - 1 - i, by + i, 1, 1);
		}
	}
	gem_text(cr, label, x + box + GEM_PAD / 2 + 2, y, GEM_ROW_H);
	int w = box + GEM_PAD / 2 + 2 + (int)gem_text_width(cr, label);
	if (hits != NULL) {
		gem_hit_add(hits, x, y, w, GEM_ROW_H, id, 0);
	}
	return w;
}

void gem_dialog_frame(cairo_t *cr, int w, int h) {
	gem_white(cr);
	gem_fill(cr, 0, 0, w, h);
	gem_black(cr);
	gem_frame(cr, 0, 0, w, h, 1);
	gem_frame(cr, GEM_BORDER, GEM_BORDER, w - 2 * GEM_BORDER,
		h - 2 * GEM_BORDER, 2);
}

void gem_text_clipped(cairo_t *cr, const char *s, int x, int y, int w, int h) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, MAX(w, 0), h);
	cairo_clip(cr);
	gem_text(cr, s, x, y, h);
	cairo_restore(cr);
}

void gem_dither(cairo_t *cr, int x, int y, int w, int h) {
	for (int j = y; j < y + h; j++) {
		for (int i = x + ((j + x) & 1); i < x + w; i += 2) {
			gem_fill(cr, i, j, 1, 1);
		}
	}
}

/* ---- Wrapping ------------------------------------------------------------- */

/* The longest start of s (at least a character) no wider than width. */
static char *longest_fit(const char *s, int width) {
	glong n = g_utf8_strlen(s, -1);
	while (n > 1) {
		char *head = g_utf8_substring(s, 0, n);
		if (gem_measure(head) <= width) {
			return head;
		}
		g_free(head);
		n--;
	}
	return g_utf8_substring(s, 0, 1);
}

char **gem_wrap(const char *text, int width) {
	GPtrArray *lines = g_ptr_array_new();
	char **paras = g_strsplit(text != NULL ? text : "", "\n", -1);
	for (int p = 0; paras[p] != NULL; p++) {
		GString *line = g_string_new(NULL);
		char **words = g_strsplit(paras[p], " ", -1);
		for (int i = 0; words[i] != NULL; i++) {
			if (words[i][0] == '\0') {
				continue;
			}
			char *try = line->len > 0 ?
				g_strconcat(line->str, " ", words[i], NULL) : g_strdup(words[i]);
			if (line->len > 0 && gem_measure(try) > width) {
				g_ptr_array_add(lines, g_string_free(line, FALSE));
				line = g_string_new(words[i]);
			} else {
				g_string_assign(line, try);
			}
			g_free(try);
			/* A word wider than a line breaks where it must. */
			while (gem_measure(line->str) > width && g_utf8_strlen(line->str, -1) > 1) {
				char *head = longest_fit(line->str, width);
				g_string_erase(line, 0, strlen(head));
				g_ptr_array_add(lines, head);
			}
		}
		g_strfreev(words);
		g_ptr_array_add(lines, g_string_free(line, FALSE));
	}
	g_strfreev(paras);
	g_ptr_array_add(lines, NULL);
	return (char **)g_ptr_array_free(lines, FALSE);
}
