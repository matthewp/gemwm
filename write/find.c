#include <string.h>
#include "find.h"
#include "gem-ui.h"

#define BOX_W 420
#define INNER (GEM_BORDER + 2 * GEM_PAD)
#define LABEL_W 88
#define TITLE_H (GEM_ROW_H + 2)

enum { HIT_CLOSE, HIT_CASE, HIT_NEXT, HIT_PREVIOUS, HIT_REPLACE, HIT_ALL };

struct find {
	GtkOverlay *host;
	WpPageView *view;
	WpEditor *ed;
	GtkWidget *box, *area, *find_field, *replace_field;
	GArray *hits;
	bool replace, match_case, shown;
	char *count;         /* "3 of 12" */
};

static int find_y(void) { return GEM_BORDER + TITLE_H + GEM_PAD; }
static int replace_y(void) { return find_y() + GEM_FIELD_H + GEM_PAD; }
static int options_y(struct find *f) {
	return (f->replace ? replace_y() : find_y()) + GEM_FIELD_H + GEM_PAD;
}
static int buttons_y(struct find *f) { return options_y(f) + GEM_ROW_H + GEM_PAD; }
static int box_h(struct find *f) {
	return buttons_y(f) + GEM_BUTTON_H + 2 * GEM_PAD + GEM_BORDER;
}

/* ---- Drawing --------------------------------------------------------------- */

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct find *f = data;
	g_array_set_size(f->hits, 0);
	gem_dialog_frame(cr, w, h);
	/* A title bar with a close box, as a window's. */
	int b = GEM_BORDER + 2;
	gem_fill(cr, b, b + TITLE_H - 1, w - 2 * b, 1);
	gem_frame(cr, b, b, GEM_ROW_H + 1, TITLE_H, 1);
	for (int i = 4; i < GEM_ROW_H - 3; i++) {
		gem_fill(cr, b + i, b + i, 1, 1);
		gem_fill(cr, b + GEM_ROW_H - i, b + i, 1, 1);
	}
	gem_hit_add(f->hits, b, b, GEM_ROW_H + 1, TITLE_H, HIT_CLOSE, 0);
	const char *title = f->replace ? "Find and Replace" : "Find";
	gem_text(cr, title, (w - (int)gem_text_width(cr, title)) / 2, b, GEM_ROW_H);

	gem_text(cr, "Find:", INNER, find_y(), GEM_FIELD_H);
	if (f->replace) {
		gem_text(cr, "Replace:", INNER, replace_y(), GEM_FIELD_H);
	}
	gem_checkbox(cr, f->hits, INNER, options_y(f), "Match case", f->match_case,
		HIT_CASE);
	if (f->count != NULL) {
		gem_text(cr, f->count, w - INNER - (int)gem_text_width(cr, f->count),
			options_y(f), GEM_ROW_H);
	}

	static const struct { const char *label; int id; bool replace; } buttons[] = {
		{ "Replace All", HIT_ALL, true }, { "Replace", HIT_REPLACE, true },
		{ "Previous", HIT_PREVIOUS, false }, { "Next", HIT_NEXT, false },
	};
	int x = w - INNER, y = buttons_y(f);
	for (int i = G_N_ELEMENTS(buttons) - 1; i >= 0; i--) {
		if (buttons[i].replace && !f->replace) {
			continue;
		}
		x -= gem_button_width(buttons[i].label);
		gem_button(cr, f->hits, x, y, 0, buttons[i].label, buttons[i].id,
			buttons[i].id == HIT_NEXT);
		x -= GEM_PAD;
	}
}

/* ---- Searching ------------------------------------------------------------- */

static void set_count(struct find *f, char *count) {
	g_free(f->count);
	f->count = count;
	gtk_widget_queue_draw(f->area);
}

void find_update(struct find *f) {
	if (!f->shown) {
		return;
	}
	char *text = NULL;
	if (wp_editor_search_term(f->ed) != NULL) {
		size_t current, total = wp_editor_search_count(f->ed, &current);
		if (total == 0) {
			text = g_strdup("No matches");
		} else if (current > 0) {
			text = g_strdup_printf("%zu of %zu", current, total);
		} else {
			text = g_strdup_printf(total == 1 ? "%zu match" : "%zu matches", total);
		}
	}
	set_count(f, text);
}

static void set_term(struct find *f) {
	wp_editor_set_search(f->ed, gtk_editable_get_text(GTK_EDITABLE(f->find_field)),
		f->match_case ? WP_FIND_MATCH_CASE : 0);
}

/* The match at or after the selection's start, so as the term grows the
 * same match stays selected. */
static void find_here(struct find *f) {
	set_term(f);
	if (wp_editor_search_term(f->ed) != NULL) {
		wp_editor_find_current(f->ed);
	}
	find_update(f);
}

void find_step(struct find *f, int dir) {
	if (!f->shown) {
		f->shown = true;
		gtk_widget_set_visible(f->box, TRUE);
		set_term(f);
	}
	if (wp_editor_search_term(f->ed) == NULL || !wp_editor_find_next(f->ed, dir)) {
		gtk_widget_error_bell(f->area);
	}
	find_update(f);
}

static void replace_one(struct find *f) {
	const char *with = gtk_editable_get_text(GTK_EDITABLE(f->replace_field));
	if (wp_editor_search_term(f->ed) == NULL) {
		find_here(f);
	}
	/* The selected match, and on to the next; with none selected, just on. */
	if (!wp_editor_replace(f->ed, with)) {
		find_step(f, 1);
	}
	find_update(f);
}

static void replace_all(struct find *f) {
	set_term(f);
	size_t n = wp_editor_replace_all(f->ed,
		gtk_editable_get_text(GTK_EDITABLE(f->replace_field)));
	if (n == 0) {
		gtk_widget_error_bell(f->area);
		find_update(f);
		return;
	}
	set_count(f, g_strdup_printf("Replaced %zu", n));
}

/* ---- Showing ---------------------------------------------------------------- */

static void layout(struct find *f) {
	gtk_widget_set_visible(f->replace_field, f->replace);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(f->area), box_h(f));
	gtk_widget_queue_draw(f->area);
}

void find_show(struct find *f, bool replace) {
	f->replace = replace;
	f->shown = true;
	layout(f);
	gtk_widget_set_visible(f->box, TRUE);
	/* Text selected within a paragraph is what to find. */
	WpEditor *ed = f->ed;
	WpPos a, b;
	wp_editor_get_selection(ed, &a, &b);
	if (wp_editor_has_selection(ed) && a.para == b.para) {
		const WpParagraph *p = wp_document_para(wp_editor_document(ed), a.para);
		char *sel = g_strndup(p->text + a.offset, b.offset - a.offset);
		gtk_editable_set_text(GTK_EDITABLE(f->find_field), sel);
		g_free(sel);
	}
	bool have = gtk_editable_get_text(GTK_EDITABLE(f->find_field))[0] != '\0';
	gtk_widget_grab_focus(replace && have ? f->replace_field : f->find_field);
	gtk_editable_select_region(GTK_EDITABLE(f->find_field), 0, -1);
	find_here(f);
}

void find_hide(struct find *f) {
	if (!f->shown) {
		return;
	}
	f->shown = false;
	gtk_widget_set_visible(f->box, FALSE);
	wp_editor_set_search(f->ed, NULL, f->match_case ? WP_FIND_MATCH_CASE : 0);
	gtk_widget_grab_focus(GTK_WIDGET(f->view));
}

bool find_shown(struct find *f) {
	return f->shown;
}

/* ---- Input ------------------------------------------------------------------ */

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct find *f = data;
	const struct gem_hit *hit = gem_hit_at(f->hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_CLOSE: find_hide(f); break;
	case HIT_CASE:
		f->match_case = !f->match_case;
		find_here(f);
		break;
	case HIT_NEXT: find_step(f, 1); break;
	case HIT_PREVIOUS: find_step(f, -1); break;
	case HIT_REPLACE: replace_one(f); break;
	case HIT_ALL: replace_all(f); break;
	}
}

static void find_changed(GtkEditable *e, gpointer data) {
	find_here(data);
}

/* Escape closes; Return finds the next (Shift, the previous), or in the
 * Replace field replaces. */
static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct find *f = data;
	if (keyval == GDK_KEY_Escape) {
		find_hide(f);
		return TRUE;
	}
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		GtkWidget *focus = gtk_root_get_focus(gtk_widget_get_root(f->box));
		if (focus != NULL && gtk_widget_is_ancestor(focus, f->replace_field)) {
			replace_one(f);
		} else {
			find_step(f, state & GDK_SHIFT_MASK ? -1 : 1);
		}
		return TRUE;
	}
	return FALSE;
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct find *f = data;
	g_array_unref(f->hits);
	g_free(f->count);
	g_free(f);
}

struct find *find_new(GtkOverlay *host, WpPageView *view) {
	struct find *f = g_new0(struct find, 1);
	f->host = host;
	f->view = view;
	f->ed = wp_page_view_get_editor(view);
	f->hits = gem_hits_new();
	f->box = gtk_overlay_new();
	gtk_widget_set_halign(f->box, GTK_ALIGN_END);
	gtk_widget_set_valign(f->box, GTK_ALIGN_START);
	gtk_widget_set_margin_end(f->box, GEM_GADGET + 1 + GEM_PAD);
	gtk_widget_set_margin_top(f->box, GEM_PAD);
	f->area = gem_pixel_area_new(BOX_W, box_h(f), paint, f);
	gtk_overlay_set_child(GTK_OVERLAY(f->box), f->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), f);
	gtk_widget_add_controller(f->area, GTK_EVENT_CONTROLLER(click));
	f->find_field = gem_field_new(INNER + LABEL_W, find_y(),
		BOX_W - 2 * INNER - LABEL_W);
	g_signal_connect(f->find_field, "changed", G_CALLBACK(find_changed), f);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), f->find_field);
	f->replace_field = gem_field_new(INNER + LABEL_W, replace_y(),
		BOX_W - 2 * INNER - LABEL_W);
	gtk_overlay_add_overlay(GTK_OVERLAY(f->box), f->replace_field);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), f);
	gtk_widget_add_controller(f->box, keys);
	gtk_widget_set_visible(f->box, FALSE);
	gtk_overlay_add_overlay(host, f->box);
	g_signal_connect(f->box, "destroy", G_CALLBACK(destroyed), f);
	return f;
}
