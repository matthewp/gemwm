#include <string.h>
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "logos.h"
#include "magazine.h"

#define FRAME (LOGO_SIZE + 4)    /* an icon's box */
#define TILE_W 136
#define TILE_H (GEM_PAD + FRAME + 4 + 2 * GEM_ROW_H + GEM_PAD)
#define TITLE_H (GEM_ROW_H + 2)  /* the title row, with its rule */

enum level { SHELF, ISSUES };
enum { HIT_TILE, HIT_ISSUE, HIT_BACK };

struct publication {
	char *key, *name, *address;
	GPtrArray *issues;           /* struct filed, borrowed from m->filed */
	int unread;
};

struct magazine {
	GtkWidget *box, *area, *bar;
	GtkAdjustment *adj;          /* how far down, in pixels */
	struct cache *cache;
	char *category;
	GPtrArray *filed;            /* struct filed: the category's messages */
	GPtrArray *pubs;             /* struct publication, latest first */
	GHashTable *addresses;       /* "mailbox\nuid" -> address, read once */
	enum level level;
	int pub;                     /* the publication open (ISSUES) */
	int selected;                /* tile or issue, or -1 */
	GArray *hits;
	magazine_open_fn open;
	void *data;
};

/* ---- The publications ----------------------------------------------------- */

static void publication_free(gpointer p) {
	struct publication *pub = p;
	g_free(pub->key);
	g_free(pub->name);
	g_free(pub->address);
	g_ptr_array_unref(pub->issues);
	g_free(pub);
}

/* A message's sender's address, from its headers, read once. */
static const char *address_of(struct magazine *m, struct filed *f) {
	char *key = g_strdup_printf("%s\n%u", f->mailbox, f->s.uid);
	const char *address = g_hash_table_lookup(m->addresses, key);
	if (address == NULL && !g_hash_table_contains(m->addresses, key)) {
		char *found = cache_from_address(m->cache, f->mailbox, f->s.uid);
		g_hash_table_insert(m->addresses, key, found); /* NULL too: tried */
		return found;
	}
	g_free(key);
	return address;
}

/* By who they're from (the address; the name if there's no address).
 * filed is newest first, so a publication's first issue is its latest,
 * and the publications come out latest first. */
static void group(struct magazine *m) {
	g_clear_pointer(&m->pubs, g_ptr_array_unref);
	m->pubs = g_ptr_array_new_with_free_func(publication_free);
	GHashTable *by_key = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint i = 0; i < m->filed->len; i++) {
		struct filed *f = m->filed->pdata[i];
		const char *address = address_of(m, f);
		const char *key = address != NULL ? address :
			f->s.from != NULL ? f->s.from : "";
		struct publication *pub = g_hash_table_lookup(by_key, key);
		if (pub == NULL) {
			pub = g_new0(struct publication, 1);
			pub->key = g_strdup(key);
			pub->address = g_strdup(address);
			pub->name = g_strdup(f->s.from != NULL && f->s.from[0] != '\0' ?
				f->s.from : address != NULL ? address : "(unknown)");
			pub->issues = g_ptr_array_new();
			g_ptr_array_add(m->pubs, pub);
			g_hash_table_insert(by_key, pub->key, pub);
		}
		g_ptr_array_add(pub->issues, f);
		pub->unread += !f->s.seen;
	}
	g_hash_table_destroy(by_key);
}

/* ---- Where things are ------------------------------------------------------- */

static int columns(int w) {
	return MAX(1, (w - GEM_PAD) / TILE_W);
}

static int count(struct magazine *m) {
	if (m->pubs == NULL) {
		return 0;
	}
	if (m->level == SHELF) {
		return m->pubs->len;
	}
	struct publication *pub = m->pub < (int)m->pubs->len ?
		m->pubs->pdata[m->pub] : NULL;
	return pub != NULL ? (int)pub->issues->len : 0;
}

/* How tall it all is, below the title row. */
static int content_height(struct magazine *m, int w) {
	int n = count(m);
	if (m->level == SHELF) {
		return GEM_PAD + (n + columns(w) - 1) / columns(w) * TILE_H;
	}
	return n * GEM_ROW_H + GEM_PAD;
}

static int offset(struct magazine *m) {
	return (int)gtk_adjustment_get_value(m->adj);
}

/* The scroll bar fitted to what's there, out of the drawing. */
static gboolean fit_bar(gpointer data) {
	struct magazine *m = data;
	int w = gtk_widget_get_width(m->area), h = gtk_widget_get_height(m->area);
	int page = MAX(h - TITLE_H, 1), total = content_height(m, w);
	double value = CLAMP(gtk_adjustment_get_value(m->adj), 0,
		MAX(total - page, 0));
	gtk_adjustment_configure(m->adj, value, 0, MAX(total, page), GEM_ROW_H,
		MAX(page - GEM_ROW_H, GEM_ROW_H), page);
	return G_SOURCE_REMOVE;
}

/* The selected tile or issue scrolled into view. */
static void reveal(struct magazine *m) {
	int w = gtk_widget_get_width(m->area), h = gtk_widget_get_height(m->area);
	int page = h - TITLE_H, top, bottom;
	if (m->selected < 0) {
		return;
	}
	if (m->level == SHELF) {
		top = GEM_PAD + m->selected / columns(w) * TILE_H;
		bottom = top + TILE_H;
	} else {
		top = m->selected * GEM_ROW_H;
		bottom = top + GEM_ROW_H;
	}
	if (top < offset(m)) {
		gtk_adjustment_set_value(m->adj, top);
	} else if (bottom > offset(m) + page) {
		gtk_adjustment_set_value(m->adj, bottom - page);
	}
}

/* ---- Drawing ------------------------------------------------------------------ */

static char *date_of(gint64 unix) {
	if (unix == 0) {
		return g_strdup("");
	}
	GDateTime *d = g_date_time_new_from_unix_local(unix);
	char *s = g_date_time_format(d, "%e %b %Y");
	g_date_time_unref(d);
	return g_strstrip(s);
}

/* No logo: the initials, white on black. */
static void badge(cairo_t *cr, const char *name, int x, int y) {
	gem_black(cr);
	gem_fill(cr, x, y, LOGO_SIZE, LOGO_SIZE);
	char initials[16] = "";
	int n = 0;
	bool start = true;
	for (const char *p = name; *p != '\0' && n < 2; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (g_unichar_isalnum(c) && start) {
			n++;
			g_unichar_to_utf8(g_unichar_toupper(c), initials + strlen(initials));
		}
		start = !g_unichar_isalnum(c);
	}
	cairo_save(cr);
	cairo_set_font_size(cr, 32);
	cairo_text_extents_t te;
	cairo_text_extents(cr, initials, &te);
	gem_white(cr);
	cairo_move_to(cr, x + (LOGO_SIZE - te.width) / 2 - te.x_bearing,
		y + (LOGO_SIZE - te.height) / 2 - te.y_bearing);
	cairo_show_text(cr, initials);
	cairo_restore(cr);
	gem_black(cr);
}

static void paint_tile(struct magazine *m, cairo_t *cr, int i, int x, int y) {
	struct publication *pub = m->pubs->pdata[i];
	int fx = x + (TILE_W - FRAME) / 2, fy = y + GEM_PAD;
	gem_white(cr);
	gem_fill(cr, fx, fy, FRAME, FRAME);
	gem_black(cr);
	gem_frame(cr, fx, fy, FRAME, FRAME, 1);
	cairo_surface_t *logo = logo_for(pub->address);
	if (logo != NULL) {
		cairo_save(cr);
		cairo_set_source_surface(cr, logo, fx + 2, fy + 2);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		badge(cr, pub->name, fx + 2, fy + 2);
	}
	if (pub->unread > 0) {
		char n[16];
		g_snprintf(n, sizeof n, "%d", pub->unread);
		int nw = (int)gem_text_width(cr, n) + 6;
		gem_black(cr);
		gem_fill(cr, fx + FRAME - nw + 4, fy - 4, nw, GEM_ROW_H - 2);
		gem_white(cr);
		gem_text(cr, n, fx + FRAME - nw + 7, fy - 4, GEM_ROW_H - 2);
		gem_black(cr);
	}
	/* The name, two lines at most, inverted when selected (as GEM's
	 * desktop icons are). */
	char **lines = gem_wrap(pub->name, TILE_W - 8);
	int ly = fy + FRAME + 4;
	for (int l = 0; lines[l] != NULL && l < 2; l++, ly += GEM_ROW_H) {
		int lw = MIN((int)gem_text_width(cr, lines[l]), TILE_W - 8);
		int lx = x + (TILE_W - lw) / 2;
		if (i == m->selected) {
			gem_black(cr);
			gem_fill(cr, lx - 2, ly, lw + 4, GEM_ROW_H);
			gem_white(cr);
		}
		gem_text_clipped(cr, lines[l], lx, ly, lw, GEM_ROW_H);
		gem_black(cr);
	}
	g_strfreev(lines);
	gem_hit_add(m->hits, x, y, TILE_W, TILE_H, HIT_TILE, i);
}

static void paint_shelf(struct magazine *m, cairo_t *cr, int w, int h) {
	int cols = columns(w);
	int left = (w - cols * TILE_W) / 2;
	for (int i = 0; i < (int)m->pubs->len; i++) {
		int x = left + i % cols * TILE_W;
		int y = TITLE_H + GEM_PAD + i / cols * TILE_H - offset(m);
		if (y + TILE_H < TITLE_H || y > h) {
			continue;
		}
		paint_tile(m, cr, i, x, y);
	}
}

static void diamond(cairo_t *cr, int cx, int cy) {
	cairo_move_to(cr, cx, cy - 4);
	cairo_line_to(cr, cx + 4, cy);
	cairo_line_to(cr, cx, cy + 4);
	cairo_line_to(cr, cx - 4, cy);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void paint_issues(struct magazine *m, cairo_t *cr, int w, int h) {
	struct publication *pub = m->pubs->pdata[m->pub];
	int date_w = (int)gem_measure("30 Sep 2026") + 2 * GEM_PAD;
	for (guint i = 0; i < pub->issues->len; i++) {
		struct filed *f = pub->issues->pdata[i];
		int y = TITLE_H + i * GEM_ROW_H - offset(m);
		if (y + GEM_ROW_H < TITLE_H || y > h) {
			continue;
		}
		gem_black(cr);
		if ((int)i == m->selected) {
			gem_fill(cr, 0, y, w, GEM_ROW_H);
			gem_white(cr);
		}
		if (!f->s.seen) {
			diamond(cr, 10, y + GEM_ROW_H / 2);
		}
		char *date = date_of(f->s.date);
		gem_text(cr, date, 20, y, GEM_ROW_H);
		g_free(date);
		gem_text_clipped(cr, f->s.subject != NULL && f->s.subject[0] != '\0' ?
			f->s.subject : "(no subject)", 20 + date_w, y,
			w - 20 - date_w - GEM_PAD, GEM_ROW_H);
		gem_hit_add(m->hits, 0, y, w, GEM_ROW_H, HIT_ISSUE, i);
	}
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct magazine *m = data;
	g_array_set_size(m->hits, 0);
	if ((int)gtk_adjustment_get_page_size(m->adj) != h - TITLE_H ||
			(int)gtk_adjustment_get_upper(m->adj) <
			MIN(content_height(m, w), h - TITLE_H)) {
		g_idle_add(fit_bar, m);
	}
	if (m->pubs == NULL || m->pubs->len == 0) {
		const char *empty = "No newsletters yet: they're found as mail is "
			"categorized.";
		gem_black(cr);
		gem_text(cr, empty, MAX(GEM_PAD, (w - (int)gem_text_width(cr, empty)) / 2),
			h / 3, GEM_ROW_H);
		return;
	}
	if (m->level == SHELF) {
		paint_shelf(m, cr, w, h);
	} else {
		paint_issues(m, cr, w, h);
	}
	/* The title row over it all: where you are, and the way back. */
	gem_white(cr);
	gem_fill(cr, 0, 0, w, TITLE_H);
	gem_black(cr);
	for (int x = 0; x < w; x += 2) {
		gem_fill(cr, x, TITLE_H - 2, 1, 1);
	}
	char *title;
	int x = GEM_PAD;
	if (m->level == SHELF) {
		title = g_strdup_printf("Newsletters: %u publications", m->pubs->len);
	} else {
		struct publication *pub = m->pubs->pdata[m->pub];
		const char *back = "< Newsletters";
		int bw = (int)gem_text_width(cr, back) + GEM_PAD;
		gem_frame(cr, x, 1, bw, GEM_ROW_H - 2, 1);
		gem_text(cr, back, x + GEM_PAD / 2, 0, GEM_ROW_H);
		gem_hit_add(m->hits, x, 0, bw, GEM_ROW_H, HIT_BACK, 0);
		x += bw + GEM_PAD;
		title = g_strdup_printf("%s: %u issues", pub->name, pub->issues->len);
	}
	gem_text_clipped(cr, title, x, 0, w - x - GEM_PAD, GEM_ROW_H);
	g_free(title);
}

/* ---- Moving about ------------------------------------------------------------- */

static void open_selected(struct magazine *m) {
	if (m->selected < 0 || m->selected >= count(m)) {
		return;
	}
	if (m->level == SHELF) {
		m->pub = m->selected;
		m->level = ISSUES;
		m->selected = 0;
		gtk_adjustment_set_value(m->adj, 0);
		g_idle_add(fit_bar, m);
		gtk_widget_queue_draw(m->area);
		return;
	}
	struct publication *pub = m->pubs->pdata[m->pub];
	struct filed *f = pub->issues->pdata[m->selected];
	char *mailbox = g_strdup(f->mailbox);
	m->open(mailbox, f->s.uid, m->data);
	g_free(mailbox);
}

static bool back(struct magazine *m) {
	if (m->level != ISSUES) {
		return false;
	}
	m->level = SHELF;
	m->selected = m->pub;
	gtk_adjustment_set_value(m->adj, 0);
	g_idle_add(fit_bar, m);
	reveal(m);
	gtk_widget_queue_draw(m->area);
	return true;
}

static void select_item(struct magazine *m, int i) {
	int n = count(m);
	if (n == 0) {
		return;
	}
	m->selected = CLAMP(i, 0, n - 1);
	reveal(m);
	gtk_widget_queue_draw(m->area);
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct magazine *m = data;
	gtk_widget_grab_focus(m->area);
	const struct gem_hit *hit = gem_hit_at(m->hits, x, y);
	if (hit == NULL) {
		return;
	}
	if (hit->id == HIT_BACK) {
		back(m);
		return;
	}
	m->selected = hit->index;
	gtk_widget_queue_draw(m->area);
	if (n == 2) {
		open_selected(m);
	}
}

static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	struct magazine *m = data;
	int step = m->level == SHELF ? columns(gtk_widget_get_width(m->area)) : 1;
	int page = MAX(1, (gtk_widget_get_height(m->area) - TITLE_H) /
		(m->level == SHELF ? TILE_H : GEM_ROW_H)) * step;
	int at = m->selected < 0 ? 0 : m->selected;
	switch (keyval) {
	case GDK_KEY_Left:
		if (m->level == SHELF) {
			select_item(m, at - 1);
		}
		return TRUE;
	case GDK_KEY_Right:
		if (m->level == SHELF) {
			select_item(m, m->selected < 0 ? 0 : at + 1);
		}
		return TRUE;
	case GDK_KEY_Up: select_item(m, at - step); return TRUE;
	case GDK_KEY_Down: select_item(m, m->selected < 0 ? 0 : at + step); return TRUE;
	case GDK_KEY_Page_Up: select_item(m, at - page); return TRUE;
	case GDK_KEY_Page_Down: select_item(m, at + page); return TRUE;
	case GDK_KEY_Home: select_item(m, 0); return TRUE;
	case GDK_KEY_End: select_item(m, count(m) - 1); return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		open_selected(m);
		return TRUE;
	case GDK_KEY_Escape:
	case GDK_KEY_BackSpace:
		return back(m);
	}
	return FALSE;
}

static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct magazine *m = data;
	gtk_adjustment_set_value(m->adj, gtk_adjustment_get_value(m->adj) +
		dy * 3 * GEM_ROW_H);
	return TRUE;
}

/* ---- The view ---------------------------------------------------------------- */

void magazine_load(struct magazine *m, const char *category) {
	/* Where you were, by who and which, to find again. */
	char *pub_key = m->level == ISSUES && m->pubs != NULL &&
		m->pub < (int)m->pubs->len ?
		g_strdup(((struct publication *)m->pubs->pdata[m->pub])->key) : NULL;
	if (g_strcmp0(category, m->category) != 0) {
		g_free(m->category);
		m->category = g_strdup(category);
		g_free(pub_key);
		pub_key = NULL;
	}
	g_clear_pointer(&m->pubs, g_ptr_array_unref);
	g_clear_pointer(&m->filed, g_ptr_array_unref);
	m->filed = cache_in_category(m->cache, category);
	group(m);
	m->level = SHELF;
	for (guint i = 0; pub_key != NULL && i < m->pubs->len; i++) {
		if (strcmp(((struct publication *)m->pubs->pdata[i])->key, pub_key) == 0) {
			m->level = ISSUES;
			m->pub = i;
		}
	}
	g_free(pub_key);
	if (m->selected >= count(m)) {
		m->selected = count(m) - 1;
	}
	g_idle_add(fit_bar, m);
	gtk_widget_queue_draw(m->area);
}

void magazine_redraw(struct magazine *m) {
	gtk_widget_queue_draw(m->area);
}

void magazine_focus(struct magazine *m) {
	gtk_widget_grab_focus(m->area);
}

GtkWidget *magazine_widget(struct magazine *m) {
	return m->box;
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct magazine *m = data;
	g_clear_pointer(&m->pubs, g_ptr_array_unref);
	g_clear_pointer(&m->filed, g_ptr_array_unref);
	g_hash_table_destroy(m->addresses);
	g_array_unref(m->hits);
	g_object_unref(m->adj);
	g_free(m->category);
	g_free(m);
}

struct magazine *magazine_new(struct cache *cache, magazine_open_fn open,
		void *data) {
	struct magazine *m = g_new0(struct magazine, 1);
	m->cache = cache;
	m->open = open;
	m->data = data;
	m->selected = -1;
	m->hits = gem_hits_new();
	m->addresses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	m->adj = g_object_ref_sink(gtk_adjustment_new(0, 0, 0, GEM_ROW_H,
		GEM_ROW_H, 0));
	m->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	m->area = gem_pixel_area_new(0, 0, paint, m);
	gtk_widget_set_hexpand(m->area, TRUE);
	gtk_widget_set_vexpand(m->area, TRUE);
	gtk_widget_set_focusable(m->area, TRUE);
	g_signal_connect_swapped(m->adj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), m->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), m);
	gtk_widget_add_controller(m->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), m);
	gtk_widget_add_controller(m->area, keys);
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(wheel), m);
	gtk_widget_add_controller(m->area, scroll);
	gtk_box_append(GTK_BOX(m->box), m->area);
	m->bar = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, m->adj);
	gtk_box_append(GTK_BOX(m->box), m->bar);
	g_signal_connect(m->box, "destroy", G_CALLBACK(destroyed), m);
	return m;
}
