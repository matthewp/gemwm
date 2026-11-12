#include <string.h>
#include "flagview.h"
#include "gem-draw.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "imap.h"

#define TITLE_H (2 * GEM_ROW_H + 2) /* the title, the columns' names, a rule */

struct row {
	char *mailbox, *folder, *from, *subject;
	guint32 uid;
	gint64 date;
	bool seen;
};

struct flagview {
	GtkWidget *box, *area, *bar;
	GtkAdjustment *adj;
	GArray *rows;             /* struct row */
	int selected;             /* a row, or -1 */
	bool searching;
	GArray *hits;
	flagview_fn open, unflag;
	void *data;
};

static void row_clear(gpointer p) {
	struct row *r = p;
	g_free(r->mailbox);
	g_free(r->folder);
	g_free(r->from);
	g_free(r->subject);
}

static char *short_date(gint64 unix) {
	if (unix == 0) {
		return g_strdup("");
	}
	GDateTime *when = g_date_time_new_from_unix_local(unix);
	GDateTime *now = g_date_time_new_now_local();
	const char *format = g_date_time_get_year(when) != g_date_time_get_year(now) ?
		"%Y-%m-%d" : g_date_time_get_day_of_year(when) ==
		g_date_time_get_day_of_year(now) ? "%H:%M" : "%b %e";
	char *s = g_date_time_format(when, format);
	g_date_time_unref(when);
	g_date_time_unref(now);
	return s;
}

/* ---- Scrolling ------------------------------------------------------------- */

static int offset(struct flagview *v) {
	return (int)gtk_adjustment_get_value(v->adj);
}

static gboolean fit_bar(gpointer data) {
	struct flagview *v = data;
	int h = gtk_widget_get_height(v->area);
	int page = MAX(h - TITLE_H, 1), total = v->rows->len * GEM_ROW_H + GEM_PAD;
	double value = CLAMP(gtk_adjustment_get_value(v->adj), 0,
		MAX(total - page, 0));
	gtk_adjustment_configure(v->adj, value, 0, MAX(total, page), GEM_ROW_H,
		MAX(page - GEM_ROW_H, GEM_ROW_H), page);
	return G_SOURCE_REMOVE;
}

static void reveal(struct flagview *v) {
	if (v->selected < 0) {
		return;
	}
	int page = gtk_widget_get_height(v->area) - TITLE_H;
	int top = v->selected * GEM_ROW_H, bottom = top + GEM_ROW_H;
	if (top < offset(v)) {
		gtk_adjustment_set_value(v->adj, top);
	} else if (bottom > offset(v) + page) {
		gtk_adjustment_set_value(v->adj, bottom - page);
	}
}

/* ---- Drawing ---------------------------------------------------------------- */

struct columns {
	int from, subject, folder, date, right;
};

static struct columns columns(int w) {
	struct columns c;
	c.right = w - GEM_PAD;
	c.date = c.right - (int)gem_measure("2026-10-31");
	c.folder = c.date - GEM_PAD - MIN(140, w / 6);
	c.from = 32;
	c.subject = c.from + MIN(200, w / 4) + GEM_PAD;
	return c;
}

/* The flag, as GemMail's list draws it: a pennant on a pole. */
static void flag_mark(cairo_t *cr, int x, int y) {
	static const char *const rows[] = {
		"#######", "######.", "#####..", "######.", "#######",
		"#......", "#......", "#......", "#......", "#......",
	};
	gem_bitmap(cr, rows, G_N_ELEMENTS(rows), x, y);
}

static void diamond(cairo_t *cr, int cx, int cy) {
	cairo_move_to(cr, cx, cy - 4);
	cairo_line_to(cr, cx + 4, cy);
	cairo_line_to(cr, cx, cy + 4);
	cairo_line_to(cr, cx - 4, cy);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct flagview *v = data;
	g_array_set_size(v->hits, 0);
	if ((int)gtk_adjustment_get_page_size(v->adj) != h - TITLE_H) {
		g_idle_add(fit_bar, v);
	}
	struct columns c = columns(w);
	if (v->rows->len == 0) {
		const char *empty = v->searching ? "Looking in every folder..." :
			"Nothing's flagged. Flag a message (F) to find it here.";
		gem_black(cr);
		gem_text(cr, empty, MAX(GEM_PAD, (w - (int)gem_text_width(cr, empty)) / 2),
			h / 3, GEM_ROW_H);
	}
	for (guint i = 0; i < v->rows->len; i++) {
		struct row *r = &g_array_index(v->rows, struct row, i);
		int y = TITLE_H + i * GEM_ROW_H - offset(v);
		if (y + GEM_ROW_H < TITLE_H || y > h) {
			continue;
		}
		gem_black(cr);
		if ((int)i == v->selected) {
			gem_fill(cr, 0, y, w, GEM_ROW_H);
			gem_white(cr);
		}
		/* As GemMail's list has them: unread's diamond, then the flag. */
		if (!r->seen) {
			diamond(cr, 10, y + GEM_ROW_H / 2);
		}
		flag_mark(cr, 19, y + (GEM_ROW_H - 10) / 2);
		gem_text_clipped(cr, r->from, c.from, y, c.subject - c.from - GEM_PAD,
			GEM_ROW_H);
		gem_text_clipped(cr, r->subject[0] ? r->subject : "(no subject)",
			c.subject, y, c.folder - c.subject - GEM_PAD, GEM_ROW_H);
		gem_text_clipped(cr, r->folder, c.folder, y, c.date - c.folder - GEM_PAD,
			GEM_ROW_H);
		char *date = short_date(r->date);
		gem_text(cr, date, c.right - gem_text_width(cr, date), y, GEM_ROW_H);
		g_free(date);
		gem_hit_add(v->hits, 0, y, w, GEM_ROW_H, 0, i);
	}
	/* The title and the columns' names, over it all. */
	gem_white(cr);
	gem_fill(cr, 0, 0, w, TITLE_H);
	gem_black(cr);
	char *title = v->searching ?
		g_strdup_printf("Flagged: %u (looking in every folder...)", v->rows->len) :
		g_strdup_printf("Flagged: %u", v->rows->len);
	gem_text(cr, title, GEM_PAD, 0, GEM_ROW_H);
	g_free(title);
	int y = GEM_ROW_H;
	gem_text(cr, "From", c.from, y, GEM_ROW_H);
	gem_text(cr, "Subject", c.subject, y, GEM_ROW_H);
	gem_text(cr, "Folder", c.folder, y, GEM_ROW_H);
	gem_text(cr, "Date", c.right - gem_text_width(cr, "Date"), y, GEM_ROW_H);
	for (int x = 0; x < w; x += 2) {
		gem_fill(cr, x, TITLE_H - 2, 1, 1);
	}
}

/* ---- Doing things --------------------------------------------------------- */

static void select_row(struct flagview *v, int i) {
	if (v->rows->len == 0) {
		return;
	}
	v->selected = CLAMP(i, 0, (int)v->rows->len - 1);
	reveal(v);
	gtk_widget_queue_draw(v->area);
}

static void open_selected(struct flagview *v) {
	if (v->selected < 0 || v->selected >= (int)v->rows->len) {
		return;
	}
	struct row *r = &g_array_index(v->rows, struct row, v->selected);
	char *mailbox = g_strdup(r->mailbox);
	v->open(mailbox, r->uid, v->data);
	g_free(mailbox);
}

/* Unflagged: off the list, the selection on the one that took its place. */
static void unflag_selected(struct flagview *v) {
	if (v->selected < 0 || v->selected >= (int)v->rows->len) {
		return;
	}
	struct row *r = &g_array_index(v->rows, struct row, v->selected);
	char *mailbox = g_strdup(r->mailbox);
	guint32 uid = r->uid;
	g_array_remove_index(v->rows, v->selected);
	if (v->selected >= (int)v->rows->len) {
		v->selected = (int)v->rows->len - 1;
	}
	v->unflag(mailbox, uid, v->data);
	g_free(mailbox);
	g_idle_add(fit_bar, v);
	gtk_widget_queue_draw(v->area);
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct flagview *v = data;
	gtk_widget_grab_focus(v->area);
	const struct gem_hit *hit = gem_hit_at(v->hits, x, y);
	if (hit == NULL) {
		return;
	}
	v->selected = hit->index;
	gtk_widget_queue_draw(v->area);
	if (n == 2) {
		open_selected(v);
	}
}

static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct flagview *v = data;
	int page = MAX(1, (gtk_widget_get_height(v->area) - TITLE_H) / GEM_ROW_H - 1);
	int at = v->selected < 0 ? -1 : v->selected;
	if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
		return FALSE;
	}
	switch (keyval) {
	case GDK_KEY_Up: select_row(v, at < 0 ? 0 : at - 1); return TRUE;
	case GDK_KEY_Down: select_row(v, at + 1); return TRUE;
	case GDK_KEY_Page_Up: select_row(v, at - page); return TRUE;
	case GDK_KEY_Page_Down: select_row(v, at + page); return TRUE;
	case GDK_KEY_Home: select_row(v, 0); return TRUE;
	case GDK_KEY_End: select_row(v, v->rows->len - 1); return TRUE;
	case GDK_KEY_f:
		unflag_selected(v);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		open_selected(v);
		return TRUE;
	}
	return FALSE;
}

static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct flagview *v = data;
	gtk_adjustment_set_value(v->adj, gtk_adjustment_get_value(v->adj) +
		dy * 3 * GEM_ROW_H);
	return TRUE;
}

/* ---- The view -------------------------------------------------------------- */

void flagview_load(struct flagview *v, GPtrArray *messages, GHashTable *names,
		bool searching) {
	/* The selection, by message, to find again. */
	char *mailbox = NULL;
	guint32 uid = 0;
	if (v->selected >= 0 && v->selected < (int)v->rows->len) {
		struct row *r = &g_array_index(v->rows, struct row, v->selected);
		mailbox = g_strdup(r->mailbox);
		uid = r->uid;
	}
	v->searching = searching;
	g_array_set_size(v->rows, 0);
	v->selected = -1;
	for (guint i = 0; messages != NULL && i < messages->len; i++) {
		struct summary *s = messages->pdata[i];
		const char *folder = names != NULL ?
			g_hash_table_lookup(names, s->mailbox) : NULL;
		struct row r = {
			.mailbox = g_strdup(s->mailbox),
			.folder = g_strdup(folder != NULL ? folder : s->mailbox),
			.from = g_strdup(s->from != NULL ? s->from : ""),
			.subject = g_strdup(s->subject != NULL ? s->subject : ""),
			.uid = s->uid,
			.date = s->date,
			.seen = s->seen,
		};
		g_array_append_val(v->rows, r);
		if (mailbox != NULL && s->uid == uid && strcmp(s->mailbox, mailbox) == 0) {
			v->selected = v->rows->len - 1;
		}
	}
	if (v->selected < 0 && v->rows->len > 0) {
		v->selected = 0;
	}
	g_free(mailbox);
	g_idle_add(fit_bar, v);
	gtk_widget_queue_draw(v->area);
}

void flagview_focus(struct flagview *v) {
	gtk_widget_grab_focus(v->area);
}

GtkWidget *flagview_widget(struct flagview *v) {
	return v->box;
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct flagview *v = data;
	g_array_unref(v->rows);
	g_array_unref(v->hits);
	g_object_unref(v->adj);
	g_free(v);
}

struct flagview *flagview_new(flagview_fn open, flagview_fn unflag,
		void *data) {
	struct flagview *v = g_new0(struct flagview, 1);
	v->open = open;
	v->unflag = unflag;
	v->data = data;
	v->selected = -1;
	v->rows = g_array_new(FALSE, FALSE, sizeof(struct row));
	g_array_set_clear_func(v->rows, row_clear);
	v->hits = gem_hits_new();
	v->adj = g_object_ref_sink(gtk_adjustment_new(0, 0, 0, GEM_ROW_H,
		GEM_ROW_H, 0));
	v->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	v->area = gem_pixel_area_new(0, 0, paint, v);
	gtk_widget_set_hexpand(v->area, TRUE);
	gtk_widget_set_vexpand(v->area, TRUE);
	gtk_widget_set_focusable(v->area, TRUE);
	g_signal_connect_swapped(v->adj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), v->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), v);
	gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), v);
	gtk_widget_add_controller(v->area, keys);
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(wheel), v);
	gtk_widget_add_controller(v->area, scroll);
	gtk_box_append(GTK_BOX(v->box), v->area);
	v->bar = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->adj);
	gtk_box_append(GTK_BOX(v->box), v->bar);
	g_signal_connect(v->box, "destroy", G_CALLBACK(destroyed), v);
	return v;
}
