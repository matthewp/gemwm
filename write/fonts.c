#include <math.h>
#include <string.h>
#include "fonts.h"
#include "gem-alert.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define BOX_W 480
#define INNER (GEM_BORDER + 2 * GEM_PAD)
#define LIST_W 280
#define ROWS 10
#define PREVIEW_H 56
#define SAMPLE "The quick brown fox jumps"

enum { HIT_ROW, HIT_OK, HIT_CANCEL };

struct fonts {
	GtkOverlay *host;
	WpEditor *ed;
	GtkWidget *box, *area, *family_field, *size_field, *preview;
	GtkAdjustment *adj;
	GPtrArray *families;  /* char *: the generic ones, then all, sorted */
	int selected;
	GArray *hits;
	bool typing;          /* the family field's being set, not typed */
};

static int field_y(void) { return INNER + GEM_ROW_H + GEM_PAD; }
static int list_y(void) { return field_y() + GEM_FIELD_H + GEM_PAD; }
static int list_h(void) { return ROWS * GEM_ROW_H + 2; }
static int right_x(void) { return INNER + LIST_W + 2 * GEM_PAD; }
static int right_w(void) { return BOX_W - INNER - right_x(); }
static int preview_y(void) { return list_y() + list_h() + GEM_PAD; }
static int box_h(void) { return preview_y() + PREVIEW_H + INNER; }

static int cmp_names(gconstpointer a, gconstpointer b) {
	return g_utf8_collate(*(const char *const *)a, *(const char *const *)b);
}

static GPtrArray *list_families(GtkWidget *w) {
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	PangoFontFamily **fams = NULL;
	int n = 0;
	pango_context_list_families(gtk_widget_get_pango_context(w), &fams, &n);
	for (int i = 0; i < n; i++) {
		g_ptr_array_add(names, g_strdup(pango_font_family_get_name(fams[i])));
	}
	g_free(fams);
	g_ptr_array_sort(names, cmp_names);
	static const char *const generic[] = { "Sans", "Serif", "Monospace" };
	for (int i = G_N_ELEMENTS(generic) - 1; i >= 0; i--) {
		g_ptr_array_insert(names, 0, g_strdup(generic[i]));
	}
	return names;
}

/* ---- Drawing --------------------------------------------------------------- */

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct fonts *f = data;
	g_array_set_size(f->hits, 0);
	gem_dialog_frame(cr, w, h);
	const char *title = "Font";
	gem_text(cr, title, (w - (int)gem_text_width(cr, title)) / 2, INNER,
		GEM_ROW_H);
	gem_text(cr, "Size:", right_x(), field_y(), GEM_FIELD_H);

	int lx = INNER, ly = list_y();
	gem_frame(cr, lx, ly, LIST_W, list_h(), 1);
	int rows_w = LIST_W - GEM_GADGET - 1;
	int top = (int)gtk_adjustment_get_value(f->adj);
	for (int r = 0; r < ROWS && top + r < (int)f->families->len; r++) {
		int i = top + r, y = ly + 1 + r * GEM_ROW_H;
		gem_black(cr);
		if (i == f->selected) {
			gem_fill(cr, lx + 1, y, rows_w - 1, GEM_ROW_H);
			gem_white(cr);
		}
		gem_text_clipped(cr, f->families->pdata[i], lx + GEM_PAD, y,
			rows_w - 2 * GEM_PAD, GEM_ROW_H);
		gem_hit_add(f->hits, lx + 1, y, rows_w - 1, GEM_ROW_H, HIT_ROW, i);
	}
	gem_black(cr);
	gem_frame(cr, INNER, preview_y(), w - 2 * INNER, PREVIEW_H, 1);

	int bw = MAX(gem_button_width("Cancel"), gem_button_width("OK")) + GEM_PAD;
	int bx = right_x() + (right_w() - bw) / 2;
	int by = ly + list_h() - 2 * GEM_BUTTON_H - GEM_PAD;
	gem_button(cr, f->hits, bx, by, bw, "OK", HIT_OK, true);
	gem_button(cr, f->hits, bx, by + GEM_BUTTON_H + GEM_PAD, bw, "Cancel",
		HIT_CANCEL, false);
}

/* The sample, in the font as it'll be: smooth, not in pixels. */
static void draw_preview(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	struct fonts *f = data;
	if (f->selected < 0) {
		return;
	}
	double size = g_ascii_strtod(gtk_editable_get_text(
		GTK_EDITABLE(f->size_field)), NULL);
	size = CLAMP(size > 0 ? size : 12, 4, 72);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, f->families->pdata[f->selected]);
	/* At 100%: a point is 96/72 pixels. */
	pango_font_description_set_absolute_size(fd, size * 96 / 72 * PANGO_SCALE);
	pango_layout_set_font_description(layout, fd);
	pango_layout_set_text(layout, SAMPLE, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_move_to(cr, GEM_PAD, (h - th) / 2.0);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(fd);
	g_object_unref(layout);
}

/* ---- Choosing -------------------------------------------------------------- */

static void select_family(struct fonts *f, int i, bool fill_field) {
	if (i < 0 || i >= (int)f->families->len) {
		return;
	}
	f->selected = i;
	int top = (int)gtk_adjustment_get_value(f->adj);
	if (i < top) {
		gtk_adjustment_set_value(f->adj, i);
	} else if (i >= top + ROWS) {
		gtk_adjustment_set_value(f->adj, i - ROWS + 1);
	}
	if (fill_field) {
		f->typing = true;
		gtk_editable_set_text(GTK_EDITABLE(f->family_field), f->families->pdata[i]);
		f->typing = false;
	}
	gtk_widget_queue_draw(f->area);
	gtk_widget_queue_draw(f->preview);
}

/* Where family is in the list; one the document names that isn't
 * installed is added, so it can still be kept. */
static int index_of(struct fonts *f, const char *family) {
	for (guint i = 0; i < f->families->len; i++) {
		if (g_ascii_strcasecmp(f->families->pdata[i], family) == 0) {
			return i;
		}
	}
	g_ptr_array_add(f->families, g_strdup(family));
	return f->families->len - 1;
}

static void close_box(struct fonts *f, bool ok) {
	if (ok && f->selected >= 0) {
		double size = g_ascii_strtod(gtk_editable_get_text(
			GTK_EDITABLE(f->size_field)), NULL);
		WpTextAttrs eff = wp_editor_effective_attrs(f->ed);
		const char *family = f->families->pdata[f->selected];
		if (g_strcmp0(family, eff.family) != 0) {
			wp_editor_set_family(f->ed, family);
		}
		if (size >= 1 && size <= 999 && fabs(size - eff.size_pt) > 0.01) {
			wp_editor_set_size(f->ed, (float)size);
		}
	}
	gem_release(f->host, f->box);
	g_ptr_array_unref(f->families);
	g_array_unref(f->hits);
	g_object_unref(f->adj);
	g_free(f);
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct fonts *f = data;
	const struct gem_hit *hit = gem_hit_at(f->hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_ROW:
		select_family(f, hit->index, true);
		if (n == 2) {
			close_box(f, true);
		}
		break;
	case HIT_OK: close_box(f, true); break;
	case HIT_CANCEL: close_box(f, false); break;
	}
}

/* Typing a family goes to the first that starts with it. */
static void family_typed(GtkEditable *e, gpointer data) {
	struct fonts *f = data;
	if (f->typing) {
		return;
	}
	const char *typed = gtk_editable_get_text(e);
	size_t n = strlen(typed);
	for (guint i = 0; i < f->families->len && n > 0; i++) {
		if (g_ascii_strncasecmp(f->families->pdata[i], typed, n) == 0) {
			select_family(f, i, false);
			return;
		}
	}
}

static void size_typed(GtkEditable *e, gpointer data) {
	gtk_widget_queue_draw(((struct fonts *)data)->preview);
}

static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct fonts *f = data;
	switch (keyval) {
	case GDK_KEY_Escape:
		close_box(f, false);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		close_box(f, true);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		select_family(f, f->selected + (keyval == GDK_KEY_Up ? -1 : 1), true);
		return TRUE;
	case GDK_KEY_Page_Up:
	case GDK_KEY_Page_Down:
		select_family(f, CLAMP(f->selected + (keyval == GDK_KEY_Page_Up ? -ROWS : ROWS),
			0, (int)f->families->len - 1), true);
		return TRUE;
	}
	return FALSE;
}

static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct fonts *f = data;
	gtk_adjustment_set_value(f->adj, gtk_adjustment_get_value(f->adj) + dy * 3);
	return TRUE;
}

static void scrolled(GtkAdjustment *adj, gpointer data) {
	gtk_widget_queue_draw(((struct fonts *)data)->area);
}

static GtkWidget *place(GtkWidget *w, int x, int y, int width, int height) {
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_widget_set_valign(w, GTK_ALIGN_START);
	gtk_widget_set_margin_start(w, x);
	gtk_widget_set_margin_top(w, y);
	gtk_widget_set_size_request(w, width, height);
	return w;
}

void font_dialog(GtkOverlay *host, WpEditor *ed) {
	struct fonts *f = g_new0(struct fonts, 1);
	f->host = host;
	f->ed = ed;
	f->hits = gem_hits_new();
	f->families = list_families(GTK_WIDGET(host));
	f->adj = g_object_ref_sink(gtk_adjustment_new(0, 0, f->families->len, 1,
		ROWS - 1, ROWS));
	g_signal_connect(f->adj, "value-changed", G_CALLBACK(scrolled), f);

	f->box = gtk_overlay_new();
	f->area = gem_pixel_area_new(BOX_W, box_h(), paint, f);
	gtk_overlay_set_child(GTK_OVERLAY(f->box), f->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), f);
	gtk_widget_add_controller(f->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(wheel), f);
	gtk_widget_add_controller(f->area, scroll);

	f->family_field = gem_field_new(INNER, field_y(), LIST_W);
	g_signal_connect(f->family_field, "changed", G_CALLBACK(family_typed), f);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), f->family_field);
	int label_w = (int)gem_measure("Size:") + GEM_PAD;
	f->size_field = gem_field_new(right_x() + label_w, field_y(), right_w() - label_w);
	g_signal_connect(f->size_field, "changed", G_CALLBACK(size_typed), f);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), f->size_field);

	GtkWidget *bar = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, f->adj);
	gtk_widget_set_vexpand(bar, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), place(bar,
		INNER + LIST_W - GEM_GADGET - 1, list_y(), GEM_GADGET + 1, list_h()));

	f->preview = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(f->preview), draw_preview, f,
		NULL);
	gtk_widget_set_can_target(f->preview, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), place(f->preview, INNER + 1,
		preview_y() + 1, BOX_W - 2 * INNER - 2, PREVIEW_H - 2));

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), f);
	gtk_widget_add_controller(f->box, keys);

	WpTextAttrs eff = wp_editor_effective_attrs(ed);
	char size[16];
	g_snprintf(size, sizeof size, "%g", eff.size_pt);
	gtk_editable_set_text(GTK_EDITABLE(f->size_field), size);
	int i = index_of(f, eff.family != NULL ? eff.family : "Serif");
	gtk_adjustment_set_upper(f->adj, f->families->len);
	gtk_adjustment_set_value(f->adj, MAX(0, i - ROWS / 2));
	select_family(f, i, true);
	gem_hold(host, f->box);
	gtk_widget_grab_focus(f->family_field);
	gtk_editable_select_region(GTK_EDITABLE(f->family_field), 0, -1);
}
