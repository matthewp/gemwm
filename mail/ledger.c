#include <stdio.h>
#include <string.h>
#include "bills.h"
#include "gem-draw.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "ledger.h"

#define TITLE_H (2 * GEM_ROW_H + 2) /* the title, the columns' names, a rule */
#define BOX 13                       /* the paid check box */

enum { HIT_ROW, HIT_PAID };

/* A bill: the newest of its messages (a reminder of the same bill, same
 * payee, due date and amount, is the same row). */
struct row {
	struct ledger_entry *e;
	GPtrArray *same;          /* struct ledger_entry: all its messages */
	char *when;               /* YYYY-MM-DD: due, else sent */
	bool paid;
};

/* What's drawn, a line at a time: a month's heading, or a row. */
struct line {
	int row;                  /* or -1: a heading */
	int month;                /* the heading's, in months */
};

struct month {
	char *name;               /* "October 2026" */
	char *total, *unpaid;     /* amounts, formatted */
};

struct ledger {
	GtkWidget *box, *area, *bar;
	GtkAdjustment *adj;
	struct cache *cache;
	GPtrArray *entries;       /* struct ledger_entry, owned */
	GArray *rows;             /* struct row */
	GArray *lines;            /* struct line */
	GPtrArray *months;        /* struct month */
	int selected;             /* a row, or -1 */
	int reading;
	GArray *hits;
	ledger_open_fn open;
	void *data;
};

static void row_clear(gpointer p) {
	struct row *r = p;
	g_ptr_array_unref(r->same);
	g_free(r->when);
}

static void month_free(gpointer p) {
	struct month *m = p;
	g_free(m->name);
	g_free(m->total);
	g_free(m->unpaid);
	g_free(m);
}

/* ---- Making the rows ------------------------------------------------------- */

static char *sent_day(gint64 unix) {
	GDateTime *d = g_date_time_new_from_unix_local(unix);
	char *s = g_date_time_format(d, "%Y-%m-%d");
	g_date_time_unref(d);
	return s;
}

static char *today(void) {
	GDateTime *d = g_date_time_new_now_local();
	char *s = g_date_time_format(d, "%Y-%m-%d");
	g_date_time_unref(d);
	return s;
}

static bool same_bill(struct ledger_entry *a, struct ledger_entry *b) {
	return a->read && b->read && a->payee != NULL && b->payee != NULL &&
		a->due != NULL && g_strcmp0(a->due, b->due) == 0 &&
		a->amount == b->amount && g_ascii_strcasecmp(a->payee, b->payee) == 0;
}

static int by_when(gconstpointer a, gconstpointer b) {
	const struct row *x = a, *y = b;
	int c = strcmp(y->when, x->when); /* newest first */
	return c != 0 ? c : (x->e->s.date < y->e->s.date) -
		(x->e->s.date > y->e->s.date);
}

/* Amounts in one currency each, joined: "$120.00 + 5.00 EUR". */
static char *amounts(GHashTable *sums) {
	GString *s = g_string_new(NULL);
	GList *keys = g_list_sort(g_hash_table_get_keys(sums),
		(GCompareFunc)g_strcmp0);
	for (GList *k = keys; k != NULL; k = k->next) {
		gint64 *cents = g_hash_table_lookup(sums, k->data);
		char *a = bills_format_amount(*cents, ((char *)k->data)[0] ?
			k->data : NULL);
		g_string_append_printf(s, "%s%s", s->len ? " + " : "", a);
		g_free(a);
	}
	g_list_free(keys);
	return g_string_free(s, FALSE);
}

static void add_to(GHashTable *sums, const char *currency, gint64 cents) {
	const char *key = currency != NULL ? currency : "";
	gint64 *sum = g_hash_table_lookup(sums, key);
	if (sum == NULL) {
		sum = g_new0(gint64, 1);
		g_hash_table_insert(sums, g_strdup(key), sum);
	}
	*sum += cents;
}

static void lay_out(struct ledger *l) {
	g_array_set_size(l->lines, 0);
	g_ptr_array_set_size(l->months, 0);
	GHashTable *total = NULL, *unpaid = NULL;
	char *month = NULL;
	for (guint i = 0; i <= l->rows->len; i++) {
		struct row *r = i < l->rows->len ? &g_array_index(l->rows, struct row, i) :
			NULL;
		if (r == NULL || month == NULL || strncmp(r->when, month, 7) != 0) {
			/* The month before's totals; then this one's heading. */
			if (month != NULL) {
				struct month *m = g_ptr_array_index(l->months, l->months->len - 1);
				m->total = amounts(total);
				m->unpaid = amounts(unpaid);
				g_hash_table_destroy(total);
				g_hash_table_destroy(unpaid);
			}
			g_free(month);
			month = NULL;
			if (r == NULL) {
				break;
			}
			month = g_strndup(r->when, 7);
			int y = 0, mo = 1;
			sscanf(month, "%d-%d", &y, &mo);
			GDateTime *d = g_date_time_new_local(y, CLAMP(mo, 1, 12), 1, 0, 0, 0);
			struct month *m = g_new0(struct month, 1);
			m->name = g_date_time_format(d, "%B %Y");
			g_date_time_unref(d);
			g_ptr_array_add(l->months, m);
			struct line heading = { -1, l->months->len - 1 };
			g_array_append_val(l->lines, heading);
			total = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			unpaid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		}
		struct line line = { i, l->months->len - 1 };
		g_array_append_val(l->lines, line);
		if (r->e->amount >= 0) {
			add_to(total, r->e->currency, r->e->amount);
			if (!r->paid && !r->e->autopay) {
				add_to(unpaid, r->e->currency, r->e->amount);
			}
		}
	}
	g_free(month);
}

static void make_rows(struct ledger *l) {
	if (l->rows->len > 0) {
		g_array_set_size(l->rows, 0);
	}
	for (guint i = 0; i < l->entries->len; i++) {
		struct ledger_entry *e = l->entries->pdata[i];
		struct row *found = NULL;
		for (guint j = 0; j < l->rows->len && found == NULL; j++) {
			struct row *r = &g_array_index(l->rows, struct row, j);
			if (same_bill(r->e, e)) {
				found = r;
			}
		}
		if (found != NULL) {
			g_ptr_array_add(found->same, e); /* older: the newest is shown */
			found->paid |= e->paid;
			continue;
		}
		struct row r = { e, g_ptr_array_new(), e->due != NULL ?
			g_strdup(e->due) : sent_day(e->s.date), e->paid };
		g_ptr_array_add(r.same, e);
		g_array_append_val(l->rows, r);
	}
	g_array_sort(l->rows, by_when);
	lay_out(l);
}

/* ---- Where things are ---------------------------------------------------- */

static int offset(struct ledger *l) {
	return (int)gtk_adjustment_get_value(l->adj);
}

static int content_height(struct ledger *l) {
	return l->lines->len * GEM_ROW_H + GEM_PAD;
}

static gboolean fit_bar(gpointer data) {
	struct ledger *l = data;
	int h = gtk_widget_get_height(l->area);
	int page = MAX(h - TITLE_H, 1), total = content_height(l);
	double value = CLAMP(gtk_adjustment_get_value(l->adj), 0,
		MAX(total - page, 0));
	gtk_adjustment_configure(l->adj, value, 0, MAX(total, page), GEM_ROW_H,
		MAX(page - GEM_ROW_H, GEM_ROW_H), page);
	return G_SOURCE_REMOVE;
}

/* The line a row's on. */
static int line_of(struct ledger *l, int row) {
	for (guint i = 0; i < l->lines->len; i++) {
		if (g_array_index(l->lines, struct line, i).row == row) {
			return i;
		}
	}
	return 0;
}

static void reveal(struct ledger *l) {
	if (l->selected < 0) {
		return;
	}
	int page = gtk_widget_get_height(l->area) - TITLE_H;
	int line = line_of(l, l->selected);
	/* With its month's heading, if it's the first. */
	int top = (line > 0 && g_array_index(l->lines, struct line, line - 1).row < 0 ?
		line - 1 : line) * GEM_ROW_H;
	int bottom = (line + 1) * GEM_ROW_H;
	if (top < offset(l)) {
		gtk_adjustment_set_value(l->adj, top);
	} else if (bottom > offset(l) + page) {
		gtk_adjustment_set_value(l->adj, bottom - page);
	}
}

/* ---- Drawing ---------------------------------------------------------------- */

struct columns {
	int paid, due, payee, period, amount, right;
};

static struct columns columns(int w) {
	struct columns c;
	c.paid = GEM_PAD;
	c.due = c.paid + 44;
	c.payee = c.due + (int)gem_measure("Sent 30 Sep") + GEM_PAD;
	c.right = w - GEM_PAD;
	c.amount = c.right - (int)gem_measure("$00,000.00") - 2 * GEM_PAD;
	int rest = c.amount - c.payee;
	c.period = c.payee + rest / 2;
	return c;
}

static void box(cairo_t *cr, int x, int y, bool on) {
	int by = y + (GEM_ROW_H - BOX) / 2;
	gem_frame(cr, x, by, BOX, BOX, 1);
	for (int i = 2; on && i < BOX - 2; i++) {
		gem_fill(cr, x + i, by + i, 1, 1);
		gem_fill(cr, x + BOX - 1 - i, by + i, 1, 1);
	}
}

/* A word in a frame, as the list's categories are. */
static int tag(cairo_t *cr, const char *s, int right, int y) {
	int w = (int)gem_text_width(cr, s) + 6;
	gem_frame(cr, right - w, y + 2, w, GEM_ROW_H - 4, 1);
	gem_text(cr, s, right - w + 3, y, GEM_ROW_H);
	return w;
}

static void right_text(cairo_t *cr, const char *s, int right, int y) {
	gem_text(cr, s, right - (int)gem_text_width(cr, s), y, GEM_ROW_H);
}

static void paint_heading(struct ledger *l, cairo_t *cr, struct month *m,
		struct columns c, int w, int y) {
	gem_black(cr);
	gem_fill(cr, 0, y + GEM_ROW_H - 1, w, 1);
	gem_text(cr, m->name, c.paid, y, GEM_ROW_H);
	gem_text(cr, m->name, c.paid + 1, y, GEM_ROW_H); /* bold */
	char *s = m->unpaid[0] != '\0' ?
		g_strdup_printf("Unpaid %s   Total %s", m->unpaid, m->total) :
		m->total[0] != '\0' ? g_strdup_printf("Total %s", m->total) :
		g_strdup("");
	right_text(cr, s, c.right, y);
	g_free(s);
}

static void paint_row(struct ledger *l, cairo_t *cr, int i, struct columns c,
		int w, int y, const char *now) {
	struct row *r = &g_array_index(l->rows, struct row, i);
	struct ledger_entry *e = r->e;
	bool selected = i == l->selected;
	gem_black(cr);
	if (selected) {
		gem_fill(cr, 0, y, w, GEM_ROW_H);
		gem_white(cr);
	} else {
		/* The ledger's ruling: a dotted line under each. */
		for (int x = c.paid; x < c.right; x += 2) {
			gem_fill(cr, x, y + GEM_ROW_H - 1, 1, 1);
		}
	}
	if (e->autopay && !r->paid) {
		gem_text(cr, "Auto", c.paid, y, GEM_ROW_H);
	} else {
		box(cr, c.paid + 6, y, r->paid);
		gem_hit_add(l->hits, c.paid, y, c.due - c.paid, GEM_ROW_H, HIT_PAID, i);
	}
	/* Due, or (no due date) when it came. */
	int dy = 0, dm = 0, dd = 0;
	sscanf(r->when, "%d-%d-%d", &dy, &dm, &dd);
	GDateTime *d = g_date_time_new_local(dy, CLAMP(dm, 1, 12), CLAMP(dd, 1, 31),
		0, 0, 0);
	char *day = g_date_time_format(d, "%e %b");
	g_date_time_unref(d);
	char *date = g_strdup_printf("%s %s", e->due != NULL ? "Due" : "Sent",
		g_strstrip(day));
	g_free(day);
	gem_text(cr, date, c.due, y, GEM_ROW_H);
	g_free(date);
	if (!e->read) {
		gem_text_clipped(cr, e->s.from, c.payee, y, c.period - c.payee - GEM_PAD,
			GEM_ROW_H);
		gem_text_clipped(cr, "Reading...", c.period, y, c.amount - c.period,
			GEM_ROW_H);
		if (!selected) {
			gem_grey_out(cr, c.payee, y, c.right - c.payee, GEM_ROW_H - 1);
		}
		gem_hit_add(l->hits, c.due, y, w - c.due, GEM_ROW_H, HIT_ROW, i);
		return;
	}
	gem_text_clipped(cr, e->payee != NULL ? e->payee : e->s.from, c.payee, y,
		c.period - c.payee - GEM_PAD, GEM_ROW_H);
	/* What it's for; overdue, if it is. */
	int pw = c.amount - c.period - GEM_PAD;
	if (!r->paid && !e->autopay && e->due != NULL && strcmp(e->due, now) < 0) {
		pw -= tag(cr, "Overdue", c.amount - GEM_PAD, y) + GEM_PAD;
	}
	gem_text_clipped(cr, e->period != NULL ? e->period : e->s.subject, c.period,
		y, pw, GEM_ROW_H);
	char *amount = bills_format_amount(e->amount, e->currency);
	right_text(cr, amount, c.right, y);
	g_free(amount);
	gem_hit_add(l->hits, c.due, y, w - c.due, GEM_ROW_H, HIT_ROW, i);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct ledger *l = data;
	g_array_set_size(l->hits, 0);
	if ((int)gtk_adjustment_get_page_size(l->adj) != h - TITLE_H) {
		g_idle_add(fit_bar, l);
	}
	struct columns c = columns(w);
	if (l->rows->len == 0) {
		const char *empty = l->reading > 0 ? "Reading your bills..." :
			"No bills yet: they're found as mail is categorized.";
		gem_black(cr);
		gem_text(cr, empty, MAX(GEM_PAD, (w - (int)gem_text_width(cr, empty)) / 2),
			h / 3, GEM_ROW_H);
		return;
	}
	char *now = today();
	for (guint i = 0; i < l->lines->len; i++) {
		struct line *line = &g_array_index(l->lines, struct line, i);
		int y = TITLE_H + i * GEM_ROW_H - offset(l);
		if (y + GEM_ROW_H < TITLE_H || y > h) {
			continue;
		}
		if (line->row < 0) {
			paint_heading(l, cr, l->months->pdata[line->month], c, w, y);
		} else {
			paint_row(l, cr, line->row, c, w, y, now);
		}
	}
	g_free(now);
	/* The title and the columns' names, over it all. */
	gem_white(cr);
	gem_fill(cr, 0, 0, w, TITLE_H);
	gem_black(cr);
	char *title = l->reading > 0 ?
		g_strdup_printf("Bills: %u (reading %d...)", l->rows->len, l->reading) :
		g_strdup_printf("Bills: %u", l->rows->len);
	gem_text(cr, title, GEM_PAD, 0, GEM_ROW_H);
	g_free(title);
	int y = GEM_ROW_H;
	gem_text(cr, "Paid", c.paid, y, GEM_ROW_H);
	gem_text(cr, "Date", c.due, y, GEM_ROW_H);
	gem_text(cr, "Payee", c.payee, y, GEM_ROW_H);
	gem_text(cr, "For", c.period, y, GEM_ROW_H);
	right_text(cr, "Amount", c.right, y);
	for (int x = 0; x < w; x += 2) {
		gem_fill(cr, x, TITLE_H - 2, 1, 1);
	}
}

/* ---- Doing things --------------------------------------------------------- */

static void toggle_paid(struct ledger *l, int i) {
	if (i < 0 || i >= (int)l->rows->len) {
		return;
	}
	struct row *r = &g_array_index(l->rows, struct row, i);
	if (!r->e->read || (r->e->autopay && !r->paid)) {
		return;
	}
	r->paid = !r->paid;
	for (guint j = 0; j < r->same->len; j++) {
		struct ledger_entry *e = r->same->pdata[j];
		e->paid = r->paid;
		cache_set_bill_paid(l->cache, e->mailbox, e->s.uid, r->paid);
	}
	lay_out(l); /* the month's unpaid */
	gtk_widget_queue_draw(l->area);
}

static void open_selected(struct ledger *l) {
	if (l->selected < 0 || l->selected >= (int)l->rows->len) {
		return;
	}
	struct ledger_entry *e = g_array_index(l->rows, struct row, l->selected).e;
	char *mailbox = g_strdup(e->mailbox);
	l->open(mailbox, e->s.uid, l->data);
	g_free(mailbox);
}

static void select_row(struct ledger *l, int i) {
	if (l->rows->len == 0) {
		return;
	}
	l->selected = CLAMP(i, 0, (int)l->rows->len - 1);
	reveal(l);
	gtk_widget_queue_draw(l->area);
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct ledger *l = data;
	gtk_widget_grab_focus(l->area);
	const struct gem_hit *hit = gem_hit_at(l->hits, x, y);
	if (hit == NULL) {
		return;
	}
	l->selected = hit->index;
	if (hit->id == HIT_PAID) {
		toggle_paid(l, hit->index);
		return;
	}
	gtk_widget_queue_draw(l->area);
	if (n == 2) {
		open_selected(l);
	}
}

static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct ledger *l = data;
	int page = MAX(1, (gtk_widget_get_height(l->area) - TITLE_H) / GEM_ROW_H - 1);
	int at = l->selected < 0 ? -1 : l->selected;
	switch (keyval) {
	case GDK_KEY_Up: select_row(l, at < 0 ? 0 : at - 1); return TRUE;
	case GDK_KEY_Down: select_row(l, at + 1); return TRUE;
	case GDK_KEY_Page_Up: select_row(l, at - page); return TRUE;
	case GDK_KEY_Page_Down: select_row(l, at + page); return TRUE;
	case GDK_KEY_Home: select_row(l, 0); return TRUE;
	case GDK_KEY_End: select_row(l, l->rows->len - 1); return TRUE;
	case GDK_KEY_space:
		toggle_paid(l, l->selected);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		open_selected(l);
		return TRUE;
	}
	return FALSE;
}

static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct ledger *l = data;
	gtk_adjustment_set_value(l->adj, gtk_adjustment_get_value(l->adj) +
		dy * 3 * GEM_ROW_H);
	return TRUE;
}

/* ---- The view -------------------------------------------------------------- */

void ledger_load(struct ledger *l, int reading) {
	/* The selection, by message, to find again. */
	char *mailbox = NULL;
	guint32 uid = 0;
	if (l->selected >= 0 && l->selected < (int)l->rows->len) {
		struct ledger_entry *e = g_array_index(l->rows, struct row, l->selected).e;
		mailbox = g_strdup(e->mailbox);
		uid = e->s.uid;
	}
	l->reading = reading;
	g_array_set_size(l->rows, 0);
	g_clear_pointer(&l->entries, g_ptr_array_unref);
	l->entries = cache_ledger(l->cache);
	make_rows(l);
	l->selected = -1;
	for (guint i = 0; mailbox != NULL && i < l->rows->len; i++) {
		struct row *r = &g_array_index(l->rows, struct row, i);
		for (guint j = 0; j < r->same->len; j++) {
			struct ledger_entry *e = r->same->pdata[j];
			if (e->s.uid == uid && strcmp(e->mailbox, mailbox) == 0) {
				l->selected = i;
			}
		}
	}
	g_free(mailbox);
	g_idle_add(fit_bar, l);
	gtk_widget_queue_draw(l->area);
}

void ledger_focus(struct ledger *l) {
	gtk_widget_grab_focus(l->area);
}

GtkWidget *ledger_widget(struct ledger *l) {
	return l->box;
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct ledger *l = data;
	g_array_unref(l->rows);
	g_array_unref(l->lines);
	g_ptr_array_unref(l->months);
	g_clear_pointer(&l->entries, g_ptr_array_unref);
	g_array_unref(l->hits);
	g_object_unref(l->adj);
	g_free(l);
}

struct ledger *ledger_new(struct cache *cache, ledger_open_fn open, void *data) {
	struct ledger *l = g_new0(struct ledger, 1);
	l->cache = cache;
	l->open = open;
	l->data = data;
	l->selected = -1;
	l->rows = g_array_new(FALSE, FALSE, sizeof(struct row));
	g_array_set_clear_func(l->rows, row_clear);
	l->lines = g_array_new(FALSE, FALSE, sizeof(struct line));
	l->months = g_ptr_array_new_with_free_func(month_free);
	l->hits = gem_hits_new();
	l->adj = g_object_ref_sink(gtk_adjustment_new(0, 0, 0, GEM_ROW_H,
		GEM_ROW_H, 0));
	l->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	l->area = gem_pixel_area_new(0, 0, paint, l);
	gtk_widget_set_hexpand(l->area, TRUE);
	gtk_widget_set_vexpand(l->area, TRUE);
	gtk_widget_set_focusable(l->area, TRUE);
	g_signal_connect_swapped(l->adj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), l->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), l);
	gtk_widget_add_controller(l->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), l);
	gtk_widget_add_controller(l->area, keys);
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(wheel), l);
	gtk_widget_add_controller(l->area, scroll);
	gtk_box_append(GTK_BOX(l->box), l->area);
	l->bar = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, l->adj);
	gtk_box_append(GTK_BOX(l->box), l->bar);
	g_signal_connect(l->box, "destroy", G_CALLBACK(destroyed), l);
	return l;
}
