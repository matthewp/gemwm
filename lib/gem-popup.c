#include <string.h>
#include "gem-popup.h"
#include "gem-ui.h"

#define ITEM_H 18        /* as the menu bar's */
#define ITEM_PAD 16      /* GEM indents items by two characters */
#define SHORTCUT_GAP 16

struct item {
	int id;              /* -1: a separator */
	char *label, *shortcut;
	uint32_t flags;
};

struct gem_popup {
	GtkWidget *parent, *popover, *area;
	GArray *items;       /* struct item */
	int hover, chosen;
	gem_popup_fn fn;
	void *data;
};

static void item_clear(gpointer p) {
	struct item *item = p;
	g_free(item->label);
	g_free(item->shortcut);
}

static bool selectable(struct gem_popup *p, int i) {
	if (i < 0 || i >= (int)p->items->len) {
		return false;
	}
	struct item *item = &g_array_index(p->items, struct item, i);
	return item->id >= 0 && !(item->flags & GEM_POPUP_DISABLED);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct gem_popup *p = data;
	gem_black(cr);
	gem_frame(cr, 0, 0, w, h, 1);
	for (guint i = 0; i < p->items->len; i++) {
		struct item *item = &g_array_index(p->items, struct item, i);
		int y = 1 + i * ITEM_H;
		if (item->id < 0) {
			/* GEM's row of dashes, as a dotted rule. */
			for (int x = 1; x < w - 1; x += 2) {
				gem_fill(cr, x, y + ITEM_H / 2, 1, 1);
			}
			continue;
		}
		bool enabled = !(item->flags & GEM_POPUP_DISABLED);
		bool inverted = enabled && (int)i == p->hover;
		if (inverted) {
			gem_fill(cr, 1, y, w - 2, ITEM_H);
			gem_white(cr);
		}
		gem_text(cr, item->label, ITEM_PAD, y, ITEM_H);
		if (item->shortcut != NULL && item->shortcut[0] != '\0') {
			gem_text(cr, item->shortcut,
				w - ITEM_PAD - (int)gem_text_width(cr, item->shortcut), y, ITEM_H);
		}
		if (item->flags & GEM_POPUP_CHECKED) {
			cairo_set_line_width(cr, 2);
			cairo_move_to(cr, 4, y + ITEM_H / 2.0);
			cairo_line_to(cr, 7, y + ITEM_H / 2.0 + 3);
			cairo_line_to(cr, 12, y + ITEM_H / 2.0 - 4);
			cairo_stroke(cr);
		}
		if (!enabled) {
			gem_grey_out(cr, 1, y, w - 2, ITEM_H);
		}
		gem_black(cr);
	}
}

static int row_at(double y) {
	return y < 1 ? -1 : ((int)y - 1) / ITEM_H;
}

static void set_hover(struct gem_popup *p, int i) {
	if (i != p->hover) {
		p->hover = i;
		gtk_widget_queue_draw(p->area);
	}
}

static void motion(GtkEventControllerMotion *c, double x, double y,
		gpointer data) {
	struct gem_popup *p = data;
	int i = row_at(y);
	set_hover(p, selectable(p, i) ? i : -1);
}

static void leave(GtkEventControllerMotion *c, gpointer data) {
	set_hover(data, -1);
}

static void choose(struct gem_popup *p, int i) {
	p->chosen = g_array_index(p->items, struct item, i).id;
	gtk_popover_popdown(GTK_POPOVER(p->popover));
}

static void released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct gem_popup *p = data;
	int i = row_at(y);
	if (selectable(p, i)) {
		choose(p, i);
	}
}

/* The next selectable row from i in direction dir, or i. */
static int next_row(struct gem_popup *p, int i, int dir) {
	int n = p->items->len;
	for (int k = 0, j = i; k < n; k++) {
		j = (j + dir + n) % n;
		if (selectable(p, j)) {
			return j;
		}
	}
	return i;
}

static gboolean key(GtkEventControllerKey *c, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	struct gem_popup *p = data;
	switch (keyval) {
	case GDK_KEY_Up:
		set_hover(p, next_row(p, p->hover < 0 ? 0 : p->hover, -1));
		return TRUE;
	case GDK_KEY_Down:
		set_hover(p, next_row(p, p->hover < 0 ? -1 : p->hover, 1));
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_space:
		if (selectable(p, p->hover)) {
			choose(p, p->hover);
		}
		return TRUE;
	}
	return FALSE;
}

static void closed(GtkPopover *popover, gpointer data) {
	struct gem_popup *p = data;
	int id = p->chosen;
	p->chosen = -1;
	p->hover = -1;
	if (p->fn != NULL) {
		p->fn(id, p->data);
	}
}

static void parent_destroyed(GtkWidget *w, gpointer data) {
	struct gem_popup *p = data;
	gtk_widget_unparent(p->popover);
	g_array_free(p->items, TRUE);
	g_free(p);
}

struct gem_popup *gem_popup_new(GtkWidget *parent, gem_popup_fn chosen,
		void *data) {
	struct gem_popup *p = g_new0(struct gem_popup, 1);
	p->parent = parent;
	p->fn = chosen;
	p->data = data;
	p->hover = p->chosen = -1;
	p->items = g_array_new(FALSE, TRUE, sizeof(struct item));
	g_array_set_clear_func(p->items, item_clear);
	p->popover = gtk_popover_new();
	gtk_popover_set_has_arrow(GTK_POPOVER(p->popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(p->popover), GTK_POS_BOTTOM);
	gtk_widget_set_halign(p->popover, GTK_ALIGN_START);
	gtk_widget_add_css_class(p->popover, "gem-popup");
	gtk_widget_set_parent(p->popover, parent);
	p->area = gem_pixel_area_new(0, 0, paint, p);
	gtk_widget_set_focusable(p->area, TRUE);
	gtk_popover_set_child(GTK_POPOVER(p->popover), p->area);
	GtkEventController *move = gtk_event_controller_motion_new();
	g_signal_connect(move, "enter", G_CALLBACK(motion), p);
	g_signal_connect(move, "motion", G_CALLBACK(motion), p);
	g_signal_connect(move, "leave", G_CALLBACK(leave), p);
	gtk_widget_add_controller(p->area, move);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	g_signal_connect(click, "released", G_CALLBACK(released), p);
	gtk_widget_add_controller(p->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), p);
	gtk_widget_add_controller(p->popover, keys);
	g_signal_connect(p->popover, "closed", G_CALLBACK(closed), p);
	g_signal_connect(parent, "destroy", G_CALLBACK(parent_destroyed), p);
	return p;
}

void gem_popup_clear(struct gem_popup *p) {
	g_array_set_size(p->items, 0);
}

void gem_popup_add(struct gem_popup *p, int id, const char *label,
		const char *shortcut, uint32_t flags) {
	struct item item = { id, g_strdup(label), g_strdup(shortcut), flags };
	g_array_append_val(p->items, item);
}

void gem_popup_add_separator(struct gem_popup *p) {
	struct item item = { -1, NULL, NULL, 0 };
	g_array_append_val(p->items, item);
}

void gem_popup_show(struct gem_popup *p, double x, double y) {
	int label_w = 0, shortcut_w = 0;
	for (guint i = 0; i < p->items->len; i++) {
		struct item *item = &g_array_index(p->items, struct item, i);
		if (item->id < 0) {
			continue;
		}
		label_w = MAX(label_w, (int)gem_measure(item->label));
		if (item->shortcut != NULL && item->shortcut[0] != '\0') {
			shortcut_w = MAX(shortcut_w, (int)gem_measure(item->shortcut));
		}
	}
	int w = 2 * ITEM_PAD + label_w + (shortcut_w > 0 ? SHORTCUT_GAP + shortcut_w : 0);
	int h = p->items->len * ITEM_H + 2;
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), w + 2);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), h);
	/* Above the point when there's no room for it below in the window. */
	GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(p->parent));
	graphene_point_t at = GRAPHENE_POINT_INIT((float)x, (float)y), in_root;
	bool up = root != NULL &&
		gtk_widget_compute_point(p->parent, root, &at, &in_root) &&
		in_root.y + h > gtk_widget_get_height(root) && in_root.y >= h;
	gtk_popover_set_position(GTK_POPOVER(p->popover),
		up ? GTK_POS_TOP : GTK_POS_BOTTOM);
	gtk_popover_set_pointing_to(GTK_POPOVER(p->popover),
		&(GdkRectangle){ (int)x, (int)y, 1, 1 });
	p->hover = -1;
	p->chosen = -1;
	gtk_popover_popup(GTK_POPOVER(p->popover));
	gtk_widget_grab_focus(p->area);
}

bool gem_popup_shown(struct gem_popup *p) {
	return gtk_widget_get_visible(p->popover);
}
