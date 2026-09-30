#include "gem-alert.h"
#include "gem-ui.h"

#define ICON 32
#define TEXT_W 400       /* messages wrap at this */
#define MAX_BUTTONS 3

/* ---- Holding a window ---------------------------------------------------- */

/* What had the focus before the first box went up, to give it back. */
struct hold {
	int boxes;
	GtkWidget *focus;
};

static struct hold *hold_of(GtkOverlay *host) {
	struct hold *h = g_object_get_data(G_OBJECT(host), "gem-hold");
	if (h == NULL) {
		h = g_new0(struct hold, 1);
		g_object_set_data_full(G_OBJECT(host), "gem-hold", h, g_free);
	}
	return h;
}

void gem_hold(GtkOverlay *host, GtkWidget *box) {
	struct hold *h = hold_of(host);
	if (h->boxes++ == 0) {
		GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(host));
		GtkWidget *focus = root != NULL ? gtk_root_get_focus(root) : NULL;
		g_set_weak_pointer(&h->focus, focus);
		gtk_widget_set_sensitive(gtk_overlay_get_child(host), FALSE);
	}
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_overlay_add_overlay(host, box);
}

void gem_release(GtkOverlay *host, GtkWidget *box) {
	struct hold *h = hold_of(host);
	/* Likely from the box's own click or key handler: it goes once that's
	 * returned. */
	g_idle_add_once((GSourceOnceFunc)g_object_unref, g_object_ref(box));
	gtk_overlay_remove_overlay(host, box);
	if (--h->boxes == 0) {
		gtk_widget_set_sensitive(gtk_overlay_get_child(host), TRUE);
		if (h->focus != NULL) {
			gtk_widget_grab_focus(h->focus);
		}
		g_clear_weak_pointer(&h->focus);
	}
}

bool gem_alert_up(GtkOverlay *host) {
	struct hold *h = g_object_get_data(G_OBJECT(host), "gem-hold");
	return h != NULL && h->boxes > 0;
}

/* ---- The alert ----------------------------------------------------------- */

struct alert {
	GtkOverlay *host;
	GtkWidget *area;
	enum gem_alert_icon icon;
	char **lines;
	char *buttons[MAX_BUTTONS + 1];
	int n_buttons, def, cancel, button_w;
	GArray *hits;
	gem_alert_fn done;
	void *data;
};

/* The marks cut out of the icons' black squares. */
static const char *const exclamation[] = {
	"######", "######", "######", "######", "######", "######", "######",
	".####.", ".####.", ".####.", ".####.", "..##..", "..##..", "......",
	"......", "######", "######", "######", "######", "######",
};

static const char *const question[] = {
	"....########....",
	"..############..",
	".#####....#####.",
	"#####......#####",
	"#####......#####",
	"...........#####",
	"..........#####.",
	".........#####..",
	"........#####...",
	".......#####....",
	"......#####.....",
	"......#####.....",
	"......#####.....",
	"......#####.....",
	"................",
	"................",
	"......#####.....",
	"......#####.....",
	"......#####.....",
	"......#####.....",
};

static void paint_icon(cairo_t *cr, enum gem_alert_icon icon, int x, int y) {
	gem_black(cr);
	switch (icon) {
	case GEM_ALERT_NONE:
		break;
	case GEM_ALERT_NOTE:
		gem_fill(cr, x, y, ICON, ICON);
		gem_white(cr);
		gem_bitmap(cr, exclamation, G_N_ELEMENTS(exclamation), x + 13, y + 6);
		break;
	case GEM_ALERT_QUESTION:
		gem_fill(cr, x, y, ICON, ICON);
		gem_white(cr);
		gem_bitmap(cr, question, G_N_ELEMENTS(question), x + 8, y + 6);
		break;
	case GEM_ALERT_STOP: {
		/* An octagon with a bar across it. */
		int c = 9;
		for (int r = 0; r < ICON; r++) {
			int inset = r < c ? c - r : r >= ICON - c ? r - (ICON - c) + 1 : 0;
			gem_fill(cr, x + inset, y + r, ICON - 2 * inset, 1);
		}
		gem_white(cr);
		gem_fill(cr, x + 6, y + ICON / 2 - 3, ICON - 12, 6);
		break;
	}
	}
	gem_black(cr);
}

static int text_x(struct alert *a) {
	return GEM_BORDER + 2 * GEM_PAD +
		(a->icon != GEM_ALERT_NONE ? ICON + 2 * GEM_PAD : 0);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct alert *a = data;
	g_array_set_size(a->hits, 0);
	gem_dialog_frame(cr, w, h);
	int top = GEM_BORDER + 2 * GEM_PAD;
	paint_icon(cr, a->icon, GEM_BORDER + 2 * GEM_PAD, top);
	for (int i = 0; a->lines[i] != NULL; i++) {
		gem_text(cr, a->lines[i], text_x(a), top + i * GEM_ROW_H, GEM_ROW_H);
	}
	int by = h - GEM_BORDER - 2 * GEM_PAD - GEM_BUTTON_H;
	int bx = w - GEM_BORDER - 2 * GEM_PAD -
		a->n_buttons * a->button_w - (a->n_buttons - 1) * GEM_PAD;
	for (int i = 0; i < a->n_buttons; i++) {
		gem_button(cr, a->hits, bx + i * (a->button_w + GEM_PAD), by,
			a->button_w, a->buttons[i], i, i == a->def);
	}
}

static void answer(struct alert *a, int button) {
	gem_release(a->host, a->area);
	if (a->done != NULL) {
		a->done(button, a->data);
	}
	g_strfreev(a->lines);
	for (int i = 0; i < a->n_buttons; i++) {
		g_free(a->buttons[i]);
	}
	g_array_unref(a->hits);
	g_free(a);
}

static void released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct alert *a = data;
	const struct gem_hit *hit = gem_hit_at(a->hits, x, y);
	if (hit != NULL) {
		answer(a, hit->id);
	}
}

static gboolean key(GtkEventControllerKey *c, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	struct alert *a = data;
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && a->def >= 0) {
		answer(a, a->def);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape && a->cancel >= 0) {
		answer(a, a->cancel);
		return TRUE;
	}
	return TRUE; /* nothing else gets past it */
}

void gem_alert(GtkOverlay *host, enum gem_alert_icon icon, const char *text,
		const char *const *buttons, int def, int cancel, gem_alert_fn done,
		void *data) {
	struct alert *a = g_new0(struct alert, 1);
	a->host = host;
	a->icon = icon;
	a->lines = gem_wrap(text, TEXT_W);
	for (; a->n_buttons < MAX_BUTTONS && buttons[a->n_buttons] != NULL;
			a->n_buttons++) {
		a->buttons[a->n_buttons] = g_strdup(buttons[a->n_buttons]);
		a->button_w = MAX(a->button_w, gem_button_width(buttons[a->n_buttons]));
	}
	a->def = def;
	a->cancel = cancel;
	a->done = done;
	a->data = data;
	a->hits = gem_hits_new();

	int text_w = 0, n = g_strv_length(a->lines);
	for (int i = 0; i < n; i++) {
		text_w = MAX(text_w, (int)gem_measure(a->lines[i]));
	}
	int buttons_w = a->n_buttons * a->button_w + (a->n_buttons - 1) * GEM_PAD;
	int w = MAX(text_x(a) + text_w, GEM_BORDER + 2 * GEM_PAD + buttons_w) +
		2 * GEM_PAD + GEM_BORDER;
	int body = MAX(n * GEM_ROW_H, icon != GEM_ALERT_NONE ? ICON : 0);
	int h = 2 * GEM_BORDER + 6 * GEM_PAD + body + GEM_BUTTON_H;
	a->area = gem_pixel_area_new(w, h, paint, a);
	gtk_widget_set_focusable(a->area, TRUE);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(released), a);
	gtk_widget_add_controller(a->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), a);
	gtk_widget_add_controller(a->area, keys);
	gem_hold(host, a->area);
	gtk_widget_grab_focus(a->area);
}
