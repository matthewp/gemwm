/*
 * GemView: a PDF viewer, drawn as GEM would have. The pages stand on GEM's
 * grey, one under another, each outlined in black with a shadow, beside
 * GEM's scroll bars, over an info line with the page you're on and the
 * zoom. Its menus are in GemWM's menu bar.
 *
 * Poppler reads and draws the PDFs (the engine Evince uses); everything
 * around them is GemWM's. A page is drawn at the screen's real
 * resolution, so its text is sharp however far it's zoomed, and kept, so
 * scrolling back is instant. Links work: to the web, in the browser; to
 * elsewhere in the document, there. Find (^F) boxes every match on the
 * pages showing. A file that changes on disk (a LaTeX run) is read again
 * where you were, and each file opens at the page and zoom it was left
 * at, kept in ~/.local/state/gemwm/gemview.
 */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <poppler.h>
#include <stdbool.h>
#include <string.h>
#include "app-menu.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-file.h"
#include "gem-print.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define MARGIN 16        /* around the pages */
#define GAP 14           /* between them */
#define SHADOW 3
#define INFO_H 20
#define FIND_H 30
#define CACHE_N 8        /* pages kept drawn */
#define STEP 48          /* an arrow's scroll */
#define PT_PX (96.0 / 72.0) /* 100%: a point is 1/72 inch, a pixel 1/96 */
#define THUMB_W 96       /* the thumbnails: a page this wide */
#define SIDE_W (THUMB_W + 44)
#define THUMB_GAP 10     /* above a thumbnail */
#define THUMB_NUM 18     /* its number, below it */

static const int zooms[] = { 50, 67, 80, 100, 125, 150, 200, 300, 400 };

enum fit { FIT_WIDTH, FIT_PAGE, FIT_NONE };

struct cached {
	int page;
	double scale;      /* pixels a point, the screen's own */
	cairo_surface_t *surface;
	guint64 used;
};

struct match {
	int page;
	PopplerRectangle r; /* in points, from the page's top */
};

struct view {
	GtkWidget *window, *area, *hbar, *info, *find_row, *find_area, *find_field;
	GtkOverlay *host;
	GtkAdjustment *vadj, *hadj;
	struct app_menu *menu;

	char *path;
	GFileMonitor *monitor;
	PopplerDocument *doc;
	int n;
	double *pw, *ph;   /* page sizes, in points */
	double *top;       /* where each page starts, in pixels */
	double doc_w, doc_h;
	double scale;      /* pixels a point at this zoom */
	enum fit fit;
	int zoom;          /* percent, for FIT_NONE */
	/* Where to open, until the window has a size to lay the pages out
	 * in: a page, and how far down it (a fraction); -1, nowhere. */
	int pending_page;
	double pending_into;
	int area_w, area_h;

	struct cached cache[CACHE_N];
	guint64 clock;

	/* Links, per page, read when first needed. */
	GList **links;
	PopplerLinkMapping *hover;

	/* Find: what, the matches so far, and which is current. */
	bool finding;
	char *find_text;
	GArray *matches;   /* struct match, in document order */
	int searched;      /* pages searched so far, from find_from */
	int find_from;
	int current;       /* index into matches, or -1 */

	/* The thumbnails, down the left (View > Thumbnails). */
	GtkWidget *side, *side_area;
	GtkAdjustment *side_adj;
	cairo_surface_t **thumb;   /* each page, small, once drawn */
	double *thumb_top, *thumb_h;
	double thumb_dev;          /* the screen scale they were drawn for */
	double side_h;
	int side_area_h;
	int side_current;          /* the page shown as current, or -1 */

	char *message;
	bool pressed_link;
	double press_x, press_y;
};

static GList *views;

/* ---- Remembered: each file's page and zoom ------------------------------- */

static char *state_path(void) {
	return g_build_filename(g_get_user_state_dir(), "gemwm", "gemview", NULL);
}

static GKeyFile *state_load(void) {
	GKeyFile *kf = g_key_file_new();
	char *path = state_path();
	g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
	g_free(path);
	return kf;
}

static int current_page(struct view *v);

/* GemView's own settings, in the same file under [settings]. */
static bool setting_thumbs(void) {
	GKeyFile *kf = state_load();
	GError *e = NULL;
	bool on = g_key_file_get_boolean(kf, "settings", "thumbnails", &e);
	if (e != NULL) {
		on = false;
		g_error_free(e);
	}
	g_key_file_unref(kf);
	return on;
}

static void state_save(GKeyFile *kf) {
	char *path = state_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_key_file_save_to_file(kf, path, NULL);
	g_free(dir);
	g_free(path);
}

static void save_thumbs(bool on) {
	GKeyFile *kf = state_load();
	g_key_file_set_boolean(kf, "settings", "thumbnails", on);
	state_save(kf);
	g_key_file_unref(kf);
}

/* Where you were: the page, how far down it, and the zoom. The last 200
 * files are kept. */
static void remember(struct view *v) {
	if (v->doc == NULL || v->path == NULL) {
		return;
	}
	GKeyFile *kf = state_load();
	int p = current_page(v);
	double into = (gtk_adjustment_get_value(v->vadj) - v->top[p]) /
		(v->ph[p] * v->scale);
	g_key_file_set_integer(kf, v->path, "page", p);
	g_key_file_set_double(kf, v->path, "into", CLAMP(into, -1, 1));
	g_key_file_set_string(kf, v->path, "zoom", v->fit == FIT_WIDTH ? "width" :
		v->fit == FIT_PAGE ? "page" : "percent");
	g_key_file_set_integer(kf, v->path, "percent", v->zoom);
	g_key_file_set_int64(kf, v->path, "when", g_get_real_time() / G_USEC_PER_SEC);
	gsize n = 0;
	char **groups = g_key_file_get_groups(kf, &n);
	if (n > 200) {
		/* The one longest unopened goes. */
		gint64 oldest = G_MAXINT64;
		const char *which = NULL;
		for (gsize i = 0; i < n; i++) {
			if (groups[i][0] != '/') {
				continue; /* [settings], not a file */
			}
			gint64 when = g_key_file_get_int64(kf, groups[i], "when", NULL);
			if (when < oldest) {
				oldest = when;
				which = groups[i];
			}
		}
		if (which != NULL) {
			g_key_file_remove_group(kf, which, NULL);
		}
	}
	g_strfreev(groups);
	state_save(kf);
	g_key_file_unref(kf);
}

/* ---- Layout ------------------------------------------------------------- */

static double max_width(struct view *v) {
	double w = 0;
	for (int i = 0; i < v->n; i++) {
		w = MAX(w, v->pw[i]);
	}
	return w;
}

static int current_page(struct view *v) {
	if (v->n == 0) {
		return 0;
	}
	if (v->pending_page >= 0 || v->scale <= 0) {
		return CLAMP(v->pending_page, 0, v->n - 1);
	}
	/* The page at a third of the way down the window. */
	double y = gtk_adjustment_get_value(v->vadj) + v->area_h / 3.0;
	int p = 0;
	while (p + 1 < v->n && v->top[p + 1] <= y) {
		p++;
	}
	return p;
}

static void cache_clear(struct view *v) {
	for (int i = 0; i < CACHE_N; i++) {
		g_clear_pointer(&v->cache[i].surface, cairo_surface_destroy);
	}
}

/* The pages' places for the window's size and the zoom, keeping the
 * place you're reading where it is. */
static void relayout(struct view *v) {
	if (v->doc == NULL || v->area_w <= 0 || v->area_h <= 0) {
		return;
	}
	int keep = current_page(v);
	double into = v->top != NULL && v->scale > 0 ?
		(gtk_adjustment_get_value(v->vadj) - v->top[keep]) / v->scale : 0;
	if (v->pending_page >= 0) {
		keep = CLAMP(v->pending_page, 0, v->n - 1);
		into = v->pending_into * v->ph[keep];
		v->pending_page = -1;
	}
	/* Sideways, the same part in the middle; the middle, if it's a fresh
	 * layout or nothing was wider than the window. */
	double upper = gtk_adjustment_get_upper(v->hadj);
	double hfrac = v->scale > 0 && v->pending_page < 0 && upper > v->area_w + 1 ?
		(gtk_adjustment_get_value(v->hadj) + v->area_w / 2.0) / upper : 0.5;

	double old = v->scale;
	double room_w = v->area_w - 2 * MARGIN - SHADOW;
	double room_h = v->area_h - 2 * MARGIN - SHADOW;
	double widest = max_width(v);
	if (v->fit == FIT_WIDTH) {
		v->scale = room_w / widest;
	} else if (v->fit == FIT_PAGE) {
		v->scale = MIN(room_w / widest, room_h / v->ph[keep]);
	} else {
		v->scale = v->zoom / 100.0 * PT_PX;
	}
	v->scale = MAX(v->scale, 0.05);
	if (fabs(old - v->scale) > 1e-9) {
		cache_clear(v);
	}
	double y = MARGIN;
	for (int i = 0; i < v->n; i++) {
		v->top[i] = y;
		y += v->ph[i] * v->scale + GAP;
	}
	v->doc_h = y - GAP + MARGIN + SHADOW;
	v->doc_w = widest * v->scale + 2 * MARGIN + SHADOW;

	gtk_adjustment_configure(v->vadj, 0, 0, MAX(v->doc_h, v->area_h), STEP,
		MAX(STEP, v->area_h - STEP), v->area_h);
	gtk_adjustment_set_value(v->vadj, v->top[keep] + into * v->scale);
	gtk_adjustment_configure(v->hadj, 0, 0, MAX(v->doc_w, v->area_w), STEP,
		MAX(STEP, v->area_w - STEP), v->area_w);
	gtk_adjustment_set_value(v->hadj, hfrac * MAX(v->doc_w, v->area_w) -
		v->area_w / 2.0);
	gtk_widget_set_visible(v->hbar, v->doc_w > v->area_w + 0.5);
	gtk_widget_queue_draw(v->area);
	gtk_widget_queue_draw(v->info);
}

/* A page's left edge, in the window. */
static double page_x(struct view *v, int p) {
	double width = MAX(v->doc_w, v->area_w);
	return (width - SHADOW - v->pw[p] * v->scale) / 2 -
		gtk_adjustment_get_value(v->hadj);
}

static double page_y(struct view *v, int p) {
	return v->top[p] - gtk_adjustment_get_value(v->vadj);
}

/* ---- Drawing ------------------------------------------------------------ */

/* A page, drawn at the screen's resolution: from the cache, or now. */
static cairo_surface_t *page_surface(struct view *v, int p, double dev) {
	double s = v->scale * dev;
	struct cached *free_slot = &v->cache[0];
	for (int i = 0; i < CACHE_N; i++) {
		struct cached *c = &v->cache[i];
		if (c->surface != NULL && c->page == p && fabs(c->scale - s) < 1e-9) {
			c->used = ++v->clock;
			return c->surface;
		}
		if (c->surface == NULL || c->used < free_slot->used) {
			free_slot = c;
		}
	}
	int w = (int)ceil(v->pw[p] * s), h = (int)ceil(v->ph[p] * s);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
		MAX(w, 1), MAX(h, 1));
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_scale(cr, s, s);
	PopplerPage *page = poppler_document_get_page(v->doc, p);
	if (page != NULL) {
		poppler_page_render(page, cr);
		g_object_unref(page);
	}
	cairo_destroy(cr);
	cairo_surface_set_device_scale(surface, dev, dev);
	g_clear_pointer(&free_slot->surface, cairo_surface_destroy);
	*free_slot = (struct cached){ p, s, surface, ++v->clock };
	return surface;
}

/* GEM's grey, every other pixel black: a pattern, drawn in whole pixels. */
static cairo_pattern_t *grey_pattern(void) {
	static cairo_pattern_t *grey;
	if (grey == NULL) {
		cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2, 2);
		cairo_t *t = cairo_create(tile);
		cairo_set_source_rgb(t, 1, 1, 1);
		cairo_paint(t);
		cairo_set_source_rgb(t, 0, 0, 0);
		cairo_rectangle(t, 0, 0, 1, 1);
		cairo_rectangle(t, 1, 1, 1, 1);
		cairo_fill(t);
		cairo_destroy(t);
		grey = cairo_pattern_create_for_surface(tile);
		cairo_surface_destroy(tile);
		cairo_pattern_set_extend(grey, CAIRO_EXTEND_REPEAT);
		cairo_pattern_set_filter(grey, CAIRO_FILTER_NEAREST);
	}
	return grey;
}

/* A rectangle on page p, given in points from the page's top, in the
 * window. */
static void page_rect(struct view *v, int p, const PopplerRectangle *r,
		double *x, double *y, double *w, double *h) {
	*x = page_x(v, p) + r->x1 * v->scale;
	*y = page_y(v, p) + r->y1 * v->scale;
	*w = (r->x2 - r->x1) * v->scale;
	*h = (r->y2 - r->y1) * v->scale;
}

static void draw_matches(struct view *v, cairo_t *cr, int first, int last) {
	if (v->matches == NULL) {
		return;
	}
	for (guint i = 0; i < v->matches->len; i++) {
		struct match *m = &g_array_index(v->matches, struct match, i);
		if (m->page < first || m->page > last) {
			continue;
		}
		double x, y, w, h;
		page_rect(v, m->page, &m->r, &x, &y, &w, &h);
		x = floor(x) - 1;
		y = floor(y) - 1;
		w = ceil(w) + 2;
		h = ceil(h) + 2;
		if ((int)i == v->current) {
			/* The current match inverted, as GEM selects. */
			cairo_save(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_rectangle(cr, x, y, w, h);
			cairo_fill(cr);
			cairo_restore(cr);
		} else {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_set_line_width(cr, 1);
			cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
			cairo_stroke(cr);
		}
	}
}

/* The window's size changed: the pages laid out again for it. */
static void resized(GtkDrawingArea *area, int w, int h, struct view *v) {
	v->area_w = w;
	v->area_h = h;
	relayout(v);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	struct view *v = data;
	cairo_set_source(cr, grey_pattern());
	cairo_paint(cr);
	if (v->doc == NULL) {
		const char *hint = "File > Open... a PDF";
		cairo_t *c = cr;
		gem_set_font(c);
		double tw = gem_text_width(c, hint);
		cairo_set_source_rgb(c, 1, 1, 1);
		cairo_rectangle(c, (w - tw) / 2 - 8, h / 2 - 12, tw + 16, 24);
		cairo_fill(c);
		gem_black(c);
		gem_frame(c, (int)((w - tw) / 2 - 8), h / 2 - 12, (int)tw + 16, 24, 1);
		gem_text(c, hint, (w - tw) / 2, h / 2 - 12, 24);
		return;
	}
	double dev = gtk_widget_get_scale_factor(GTK_WIDGET(area));
	GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(
		GTK_WIDGET(area)));
	if (surface != NULL) {
		dev = gdk_surface_get_scale(surface);
	}
	double vy = gtk_adjustment_get_value(v->vadj);
	int first = -1, last = -1;
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	for (int p = 0; p < v->n; p++) {
		double py = v->top[p] - vy, ph = v->ph[p] * v->scale;
		if (py + ph + SHADOW < 0) {
			continue;
		}
		if (py > h) {
			break;
		}
		if (first < 0) {
			first = p;
		}
		last = p;
		double px = page_x(v, p), pw = v->pw[p] * v->scale;
		/* Pixel-aligned: the page, its outline, its shadow. */
		px = floor(px);
		py = floor(py);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_rectangle(cr, px + SHADOW, py + SHADOW, ceil(pw), ceil(ph));
		cairo_fill(cr);
		cairo_set_source_surface(cr, page_surface(v, p, dev), px, py);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, 1);
		cairo_rectangle(cr, px - 0.5, py - 0.5, ceil(pw) + 1, ceil(ph) + 1);
		cairo_stroke(cr);
	}
	if (first >= 0) {
		draw_matches(v, cr, first, last);
	}
}

static void paint_info(cairo_t *cr, int w, int h, void *data) {
	struct view *v = data;
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	char *left = v->message != NULL ? g_strdup(v->message) :
		v->doc != NULL ? g_strdup_printf("Page %d of %d", current_page(v) + 1,
			v->n) : g_strdup("");
	gem_text(cr, left, GEM_PAD, 1, h - 1);
	char *right = v->fit == FIT_WIDTH ? g_strdup("Fit Width") :
		v->fit == FIT_PAGE ? g_strdup("Fit Page") :
		g_strdup_printf("%d%%", v->zoom);
	if (v->doc != NULL && v->fit != FIT_NONE) {
		char *with = g_strdup_printf("%s (%d%%)", right,
			(int)round(v->scale / PT_PX * 100));
		g_free(right);
		right = with;
	}
	double rw = gem_text_width(cr, right);
	gem_text(cr, right, w - GEM_PAD - rw, 1, h - 1);
	double lw = gem_text_width(cr, left);
	/* A link under the pointer: where it goes, in the middle. */
	if (v->hover != NULL && v->hover->action != NULL &&
			v->hover->action->type == POPPLER_ACTION_URI) {
		const char *uri = v->hover->action->uri.uri;
		gem_text_clipped(cr, uri, (int)(GEM_PAD * 3 + lw), 1,
			(int)(w - rw - lw - GEM_PAD * 6), h - 1);
	}
	g_free(left);
	g_free(right);
}

static void paint_find(cairo_t *cr, int w, int h, void *data) {
	struct view *v = data;
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	gem_text(cr, "Find:", GEM_PAD, 1, h - 1);
	if (v->find_text != NULL && v->find_text[0] != '\0' && v->matches != NULL) {
		char *count;
		if (v->matches->len == 0) {
			count = g_strdup(v->searched < v->n ? "Looking..." : "Not found");
		} else {
			count = g_strdup_printf("%d of %u%s", v->current + 1, v->matches->len,
				v->searched < v->n ? "+" : "");
		}
		gem_text(cr, count, w - GEM_PAD - gem_text_width(cr, count), 1, h - 1);
		g_free(count);
	}
}

static void set_message(struct view *v, const char *m) {
	g_free(v->message);
	v->message = g_strdup(m);
	gtk_widget_queue_draw(v->info);
}

/* ---- Moving about ------------------------------------------------------- */

static void scroll_to(struct view *v, double y) {
	gtk_adjustment_set_value(v->vadj, y);
}

static void go_page(struct view *v, int p) {
	if (v->doc == NULL) {
		return;
	}
	p = CLAMP(p, 0, v->n - 1);
	scroll_to(v, v->top[p] - MARGIN);
}

static void set_zoom(struct view *v, enum fit fit, int zoom) {
	v->fit = fit;
	v->zoom = zoom;
	relayout(v);
	app_menu_update(v->menu);
}

static void zoom_step(struct view *v, int dir) {
	int now = (int)round(v->scale / PT_PX * 100);
	int next = now;
	if (dir > 0) {
		for (guint i = 0; i < G_N_ELEMENTS(zooms); i++) {
			if (zooms[i] > now) {
				next = zooms[i];
				break;
			}
		}
	} else {
		for (int i = (int)G_N_ELEMENTS(zooms) - 1; i >= 0; i--) {
			if (zooms[i] < now) {
				next = zooms[i];
				break;
			}
		}
	}
	set_zoom(v, FIT_NONE, next);
}

/* ---- Links -------------------------------------------------------------- */

static GList *page_links(struct view *v, int p) {
	if (v->links[p] == NULL) {
		PopplerPage *page = poppler_document_get_page(v->doc, p);
		GList *l = page != NULL ? poppler_page_get_link_mapping(page) : NULL;
		/* PDF counts up from the bottom; we count down from the top. */
		for (GList *i = l; i != NULL; i = i->next) {
			PopplerLinkMapping *m = i->data;
			double y1 = v->ph[p] - m->area.y2, y2 = v->ph[p] - m->area.y1;
			m->area.y1 = y1;
			m->area.y2 = y2;
		}
		v->links[p] = l != NULL ? l : g_list_prepend(NULL, NULL);
		if (page != NULL) {
			g_object_unref(page);
		}
	}
	return v->links[p]->data == NULL ? NULL : v->links[p];
}

static PopplerLinkMapping *link_at(struct view *v, double x, double y) {
	if (v->doc == NULL) {
		return NULL;
	}
	for (int p = 0; p < v->n; p++) {
		double py = page_y(v, p), ph = v->ph[p] * v->scale;
		if (y < py || y >= py + ph) {
			continue;
		}
		double px = page_x(v, p);
		double ptx = (x - px) / v->scale, pty = (y - py) / v->scale;
		for (GList *i = page_links(v, p); i != NULL; i = i->next) {
			PopplerLinkMapping *m = i->data;
			if (ptx >= m->area.x1 && ptx <= m->area.x2 &&
					pty >= m->area.y1 && pty <= m->area.y2) {
				return m;
			}
		}
		break;
	}
	return NULL;
}

static void go_dest(struct view *v, PopplerDest *dest) {
	PopplerDest *named = NULL;
	if (dest->type == POPPLER_DEST_NAMED) {
		named = poppler_document_find_dest(v->doc, dest->named_dest);
		if (named == NULL) {
			return;
		}
		dest = named;
	}
	int p = CLAMP(dest->page_num - 1, 0, v->n - 1);
	double y = v->top[p] - MARGIN;
	if (dest->change_top) {
		y = v->top[p] + (v->ph[p] - dest->top) * v->scale - MARGIN;
	}
	scroll_to(v, y);
	if (named != NULL) {
		poppler_dest_free(named);
	}
}

static void follow(struct view *v, PopplerAction *a) {
	if (a->type == POPPLER_ACTION_URI && a->uri.uri != NULL) {
		GtkUriLauncher *l = gtk_uri_launcher_new(a->uri.uri);
		gtk_uri_launcher_launch(l, GTK_WINDOW(v->window), NULL, NULL, NULL);
		g_object_unref(l);
	} else if (a->type == POPPLER_ACTION_GOTO_DEST && a->goto_dest.dest != NULL) {
		go_dest(v, a->goto_dest.dest);
	}
}

/* ---- Thumbnails --------------------------------------------------------- */

/* Each page's place in the list: a picture THUMB_W wide (or as tall, for
 * a page on its side), its number under it. */
static void side_layout(struct view *v) {
	if (v->doc == NULL || v->thumb_top == NULL) {
		return;
	}
	double y = 0;
	for (int p = 0; p < v->n; p++) {
		y += THUMB_GAP;
		v->thumb_top[p] = y;
		double aspect = v->ph[p] / MAX(v->pw[p], 1);
		v->thumb_h[p] = aspect >= 1 ? THUMB_W * aspect : THUMB_W;
		y += v->thumb_h[p] + THUMB_NUM;
	}
	v->side_h = y + THUMB_GAP;
	int h = MAX(v->side_area_h, 1);
	gtk_adjustment_configure(v->side_adj, gtk_adjustment_get_value(v->side_adj),
		0, MAX(v->side_h, h), 40, MAX(40, h - 40), h);
	gtk_widget_queue_draw(v->side_area);
}

static double thumb_w(struct view *v, int p) {
	return v->thumb_h[p] * v->pw[p] / MAX(v->ph[p], 1);
}

static cairo_surface_t *thumb_surface(struct view *v, int p, double dev) {
	if (fabs(v->thumb_dev - dev) > 1e-9) {
		for (int i = 0; i < v->n; i++) {
			g_clear_pointer(&v->thumb[i], cairo_surface_destroy);
		}
		v->thumb_dev = dev;
	}
	if (v->thumb[p] == NULL) {
		double s = v->thumb_h[p] / v->ph[p] * dev;
		int w = MAX(1, (int)ceil(v->pw[p] * s)), h = MAX(1, (int)ceil(v->ph[p] * s));
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
			w, h);
		cairo_t *cr = cairo_create(surface);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_paint(cr);
		cairo_scale(cr, s, s);
		PopplerPage *page = poppler_document_get_page(v->doc, p);
		if (page != NULL) {
			poppler_page_render(page, cr);
			g_object_unref(page);
		}
		cairo_destroy(cr);
		cairo_surface_set_device_scale(surface, dev, dev);
		v->thumb[p] = surface;
	}
	return v->thumb[p];
}

static void side_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		void *data) {
	struct view *v = data;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	if (v->doc == NULL || v->thumb_top == NULL) {
		return;
	}
	double dev = gtk_widget_get_scale_factor(GTK_WIDGET(area));
	GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(
		GTK_WIDGET(area)));
	if (surface != NULL) {
		dev = gdk_surface_get_scale(surface);
	}
	double sy = gtk_adjustment_get_value(v->side_adj);
	int current = current_page(v);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	gem_set_font(cr);
	for (int p = 0; p < v->n; p++) {
		double ty = floor(v->thumb_top[p] - sy), th = ceil(v->thumb_h[p]);
		if (ty + th + THUMB_NUM < 0) {
			continue;
		}
		if (ty > h) {
			break;
		}
		double tw = ceil(thumb_w(v, p));
		double tx = floor((w - tw) / 2);
		/* The shadow, the page, its outline: thick for the current one. */
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_rectangle(cr, tx + 2, ty + 2, tw, th);
		cairo_fill(cr);
		cairo_set_source_surface(cr, thumb_surface(v, p, dev), tx, ty);
		cairo_paint(cr);
		int t = p == current ? 2 : 1;
		gem_black(cr);
		gem_frame(cr, (int)tx - t, (int)ty - t, (int)tw + 2 * t, (int)th + 2 * t, t);
		char num[16];
		g_snprintf(num, sizeof num, "%d", p + 1);
		double nw = gem_text_width(cr, num);
		double ny = ty + th + 3;
		if (p == current) {
			/* Selected, as GEM selects: inverted. */
			gem_fill(cr, (w - nw) / 2 - 4, ny, nw + 8, THUMB_NUM - 4);
			gem_white(cr);
		}
		gem_text(cr, num, (w - nw) / 2, ny, THUMB_NUM - 4);
	}
	/* The panel's edge. */
	gem_black(cr);
	gem_fill(cr, w - 1, 0, 1, h);
}

static void side_resized(GtkDrawingArea *area, int w, int h, struct view *v) {
	v->side_area_h = h;
	side_layout(v);
}

/* The current page's thumbnail kept in sight as you read. */
static void side_follow(struct view *v) {
	if (v->doc == NULL || v->thumb_top == NULL ||
			!gtk_widget_get_visible(v->side)) {
		return;
	}
	int p = current_page(v);
	if (p == v->side_current) {
		return;
	}
	v->side_current = p;
	double sy = gtk_adjustment_get_value(v->side_adj);
	double top = v->thumb_top[p] - THUMB_GAP;
	double bottom = v->thumb_top[p] + v->thumb_h[p] + THUMB_NUM;
	if (top < sy || bottom > sy + v->side_area_h) {
		gtk_adjustment_set_value(v->side_adj, top - v->side_area_h / 3.0);
	}
	gtk_widget_queue_draw(v->side_area);
}

static void side_pressed(GtkGestureClick *g, int n, double x, double y,
		struct view *v) {
	if (v->doc == NULL || v->thumb_top == NULL) {
		return;
	}
	double at = y + gtk_adjustment_get_value(v->side_adj);
	for (int p = 0; p < v->n; p++) {
		if (at >= v->thumb_top[p] - THUMB_GAP / 2.0 &&
				at < v->thumb_top[p] + v->thumb_h[p] + THUMB_NUM + THUMB_GAP / 2.0) {
			go_page(v, p);
			return;
		}
	}
}

static gboolean side_scrolled(GtkEventControllerScroll *c, double dx,
		double dy, struct view *v) {
	GdkScrollUnit unit = gtk_event_controller_scroll_get_unit(c);
	gtk_adjustment_set_value(v->side_adj, gtk_adjustment_get_value(v->side_adj) +
		dy * (unit == GDK_SCROLL_UNIT_WHEEL ? STEP : 1));
	return TRUE;
}

static void side_moved(GtkAdjustment *adj, struct view *v) {
	gtk_widget_queue_draw(v->side_area);
}

static void show_thumbs(struct view *v, bool on) {
	gtk_widget_set_visible(v->side, on);
	v->side_current = -1;
	side_follow(v);
	app_menu_update(v->menu);
}

/* ---- Find --------------------------------------------------------------- */

static void find_reset(struct view *v) {
	if (v->matches != NULL) {
		g_array_set_size(v->matches, 0);
	}
	v->searched = 0;
	v->current = -1;
	v->find_from = current_page(v);
}

/* Searches the next page not yet searched; true if there was one. */
static bool find_more(struct view *v) {
	if (v->doc == NULL || v->find_text == NULL || v->find_text[0] == '\0' ||
			v->searched >= v->n) {
		return false;
	}
	int p = (v->find_from + v->searched) % v->n;
	v->searched++;
	PopplerPage *page = poppler_document_get_page(v->doc, p);
	if (page == NULL) {
		return true;
	}
	GList *found = poppler_page_find_text_with_options(page, v->find_text,
		POPPLER_FIND_DEFAULT);
	/* In document order: the pages from find_from on, then the ones before
	 * it, so keep each page's matches together in page order. */
	GArray *mine = g_array_new(FALSE, FALSE, sizeof(struct match));
	for (GList *i = found; i != NULL; i = i->next) {
		PopplerRectangle *r = i->data;
		struct match m = { p, { r->x1, v->ph[p] - r->y2, r->x2, v->ph[p] - r->y1 } };
		g_array_append_val(mine, m);
	}
	g_list_free_full(found, (GDestroyNotify)poppler_rectangle_free);
	g_object_unref(page);
	/* Top to bottom, then left to right. */
	for (guint i = 1; i < mine->len; i++) {
		for (guint j = i; j > 0; j--) {
			struct match *a = &g_array_index(mine, struct match, j - 1);
			struct match *b = &g_array_index(mine, struct match, j);
			if (a->r.y1 > b->r.y1 + 2 || (fabs(a->r.y1 - b->r.y1) <= 2 &&
					a->r.x1 > b->r.x1)) {
				struct match t = *a;
				*a = *b;
				*b = t;
			}
		}
	}
	g_array_append_vals(v->matches, mine->data, mine->len);
	g_array_free(mine, TRUE);
	return true;
}

static void show_match(struct view *v) {
	if (v->current < 0) {
		return;
	}
	struct match *m = &g_array_index(v->matches, struct match, v->current);
	double y = v->top[m->page] + m->r.y1 * v->scale;
	double vy = gtk_adjustment_get_value(v->vadj);
	if (y < vy + 20 || y > vy + v->area_h - 40) {
		scroll_to(v, y - v->area_h / 3.0);
	}
	double x = page_x(v, m->page) + gtk_adjustment_get_value(v->hadj) +
		m->r.x1 * v->scale;
	double hx = gtk_adjustment_get_value(v->hadj);
	if (x < hx + 20 || x > hx + v->area_w - 60) {
		gtk_adjustment_set_value(v->hadj, x - v->area_w / 3.0);
	}
	gtk_widget_queue_draw(v->area);
}

/* The next (dir 1) or previous match, searching on through the pages
 * until one turns up. */
static void find_step(struct view *v, int dir) {
	if (v->find_text == NULL || v->find_text[0] == '\0') {
		return;
	}
	if (dir > 0) {
		while ((guint)(v->current + 1) >= v->matches->len && find_more(v)) {
		}
		if (v->matches->len > 0) {
			v->current = (v->current + 1) % (int)v->matches->len;
		}
	} else {
		/* Back past the first: the whole document, then the last. */
		if (v->current <= 0) {
			while (find_more(v)) {
			}
			v->current = (int)v->matches->len - 1;
		} else {
			v->current--;
		}
	}
	show_match(v);
	gtk_widget_queue_draw(v->find_area);
}

static void find_changed(GtkEditable *e, struct view *v) {
	g_free(v->find_text);
	v->find_text = g_strdup(gtk_editable_get_text(e));
	find_reset(v);
	find_step(v, 1);
	gtk_widget_queue_draw(v->area);
}

static void find_activate(GtkEntry *e, struct view *v) {
	find_step(v, 1);
}

static void find_show(struct view *v, bool show) {
	v->finding = show;
	gtk_widget_set_visible(v->find_row, show);
	if (show) {
		gtk_widget_grab_focus(v->find_field);
		gtk_editable_select_region(GTK_EDITABLE(v->find_field), 0, -1);
	} else {
		if (v->matches != NULL) {
			g_array_set_size(v->matches, 0);
		}
		v->current = -1;
		gtk_widget_grab_focus(v->area);
		gtk_widget_queue_draw(v->area);
	}
	app_menu_update(v->menu);
}

/* ---- Opening ------------------------------------------------------------ */

static void close_doc(struct view *v) {
	if (v->doc == NULL) {
		return;
	}
	cache_clear(v);
	for (int p = 0; p < v->n; p++) {
		if (v->links[p] != NULL && v->links[p]->data != NULL) {
			poppler_page_free_link_mapping(v->links[p]);
		} else if (v->links[p] != NULL) {
			g_list_free(v->links[p]);
		}
	}
	g_clear_pointer(&v->links, g_free);
	for (int p = 0; v->thumb != NULL && p < v->n; p++) {
		g_clear_pointer(&v->thumb[p], cairo_surface_destroy);
	}
	g_clear_pointer(&v->thumb, g_free);
	g_clear_pointer(&v->thumb_top, g_free);
	g_clear_pointer(&v->thumb_h, g_free);
	v->hover = NULL;
	g_clear_pointer(&v->pw, g_free);
	g_clear_pointer(&v->ph, g_free);
	g_clear_pointer(&v->top, g_free);
	g_clear_object(&v->doc);
	v->n = 0;
}

static bool load(struct view *v, const char *path, GError **error) {
	char *uri = g_filename_to_uri(path, NULL, error);
	if (uri == NULL) {
		return false;
	}
	PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, error);
	g_free(uri);
	if (doc == NULL) {
		return false;
	}
	close_doc(v);
	v->doc = doc;
	v->n = poppler_document_get_n_pages(doc);
	v->pw = g_new0(double, MAX(v->n, 1));
	v->ph = g_new0(double, MAX(v->n, 1));
	v->top = g_new0(double, MAX(v->n, 1));
	v->links = g_new0(GList *, MAX(v->n, 1));
	v->thumb = g_new0(cairo_surface_t *, MAX(v->n, 1));
	v->thumb_top = g_new0(double, MAX(v->n, 1));
	v->thumb_h = g_new0(double, MAX(v->n, 1));
	for (int p = 0; p < v->n; p++) {
		PopplerPage *page = poppler_document_get_page(doc, p);
		poppler_page_get_size(page, &v->pw[p], &v->ph[p]);
		g_object_unref(page);
	}
	return true;
}

static void file_changed(GFileMonitor *m, GFile *file, GFile *other,
		GFileMonitorEvent event, struct view *v);

static void watch(struct view *v) {
	g_clear_object(&v->monitor);
	GFile *file = g_file_new_for_path(v->path);
	v->monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
	g_object_unref(file);
	if (v->monitor != NULL) {
		g_signal_connect(v->monitor, "changed", G_CALLBACK(file_changed), v);
	}
}

static void title_from_doc(struct view *v) {
	char *title = v->doc != NULL ? poppler_document_get_title(v->doc) : NULL;
	char *base = v->path != NULL ? g_path_get_basename(v->path) : g_strdup("GemView");
	gtk_window_set_title(GTK_WINDOW(v->window),
		title != NULL && title[0] != '\0' ? title : base);
	g_free(title);
	g_free(base);
}

static void open_path(struct view *v, const char *path) {
	GError *error = NULL;
	if (v->doc != NULL) {
		remember(v);
	}
	if (!load(v, path, &error)) {
		static const char *const ok[] = { "OK", NULL };
		char *text = g_strdup_printf("Can't open %s:\n%s", path,
			error->code == POPPLER_ERROR_ENCRYPTED ?
			"it's locked with a password." : error->message);
		gem_alert(v->host, GEM_ALERT_STOP, text, ok, 0, 0, NULL, NULL);
		g_free(text);
		g_error_free(error);
		return;
	}
	g_free(v->path);
	v->path = g_strdup(path);
	watch(v);
	title_from_doc(v);

	/* Where it was left. */
	GKeyFile *kf = state_load();
	int page = 0;
	double into = 0;
	v->fit = FIT_WIDTH;
	v->zoom = 100;
	if (g_key_file_has_group(kf, path)) {
		page = g_key_file_get_integer(kf, path, "page", NULL);
		into = g_key_file_get_double(kf, path, "into", NULL);
		char *zoom = g_key_file_get_string(kf, path, "zoom", NULL);
		v->fit = g_strcmp0(zoom, "page") == 0 ? FIT_PAGE :
			g_strcmp0(zoom, "percent") == 0 ? FIT_NONE : FIT_WIDTH;
		g_free(zoom);
		int pct = g_key_file_get_integer(kf, path, "percent", NULL);
		v->zoom = pct >= 25 && pct <= 800 ? pct : 100;
	}
	g_key_file_unref(kf);
	v->side_current = -1;
	gtk_adjustment_set_value(v->side_adj, 0);
	side_layout(v);
	v->scale = 0;
	v->pending_page = CLAMP(page, 0, MAX(v->n - 1, 0));
	v->pending_into = into;
	gtk_adjustment_set_value(v->vadj, 0);
	relayout(v);
	if (v->finding) {
		find_reset(v);
	}
	set_message(v, NULL);
	app_menu_update(v->menu);
	gtk_widget_queue_draw(v->area);
}

/* Written again (a LaTeX run): read it again, where you were. A file in
 * mid-write may not read yet; the next change tries again. */
static gboolean reload(void *data) {
	struct view *v = data;
	if (v->path == NULL) {
		return G_SOURCE_REMOVE;
	}
	int p = current_page(v);
	double into = v->n > 0 ? (gtk_adjustment_get_value(v->vadj) - v->top[p]) /
		v->scale : 0;
	GError *error = NULL;
	if (!load(v, v->path, &error)) {
		g_error_free(error);
		return G_SOURCE_REMOVE;
	}
	title_from_doc(v);
	v->side_current = -1;
	side_layout(v);
	relayout(v);
	p = CLAMP(p, 0, MAX(v->n - 1, 0));
	if (v->n > 0) {
		scroll_to(v, v->top[p] + into * v->scale);
	}
	if (v->finding) {
		find_reset(v);
	}
	set_message(v, "Read again: it changed on disk.");
	gtk_widget_queue_draw(v->area);
	return G_SOURCE_REMOVE;
}

static void file_changed(GFileMonitor *m, GFile *file, GFile *other,
		GFileMonitorEvent event, struct view *v) {
	if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
			event == G_FILE_MONITOR_EVENT_CREATED) {
		g_timeout_add(300, reload, v);
	}
}

static void chose_file(const char *path, void *data) {
	struct view *v = data;
	if (path != NULL) {
		open_path(v, path);
	}
}

static void ask_open(struct view *v) {
	char *folder = v->path != NULL ? g_path_get_dirname(v->path) : NULL;
	gem_file_choose(v->host, "Open a PDF", GEM_FILE_OPEN, folder, NULL,
		"*.pdf,*.PDF", "Open", chose_file, v);
	g_free(folder);
}

/* ---- Printing ----------------------------------------------------------- */

static void begin_print(GtkPrintOperation *op, GtkPrintContext *ctx,
		struct view *v) {
	gtk_print_operation_set_n_pages(op, v->n);
}

/* Each page, as big as fits the paper's printable area, centred. */
static void draw_page(GtkPrintOperation *op, GtkPrintContext *ctx, int p,
		struct view *v) {
	cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
	double w = gtk_print_context_get_width(ctx);
	double h = gtk_print_context_get_height(ctx);
	double s = MIN(w / v->pw[p], h / v->ph[p]);
	cairo_translate(cr, (w - v->pw[p] * s) / 2, (h - v->ph[p] * s) / 2);
	cairo_scale(cr, s, s);
	PopplerPage *page = poppler_document_get_page(v->doc, p);
	if (page != NULL) {
		poppler_page_render_for_printing(page, cr);
		g_object_unref(page);
	}
}

static void printed(const char *message, void *data) {
	struct view *v = data;
	if (message != NULL) {
		set_message(v, message);
	}
}

static void print(struct view *v) {
	if (v->doc == NULL) {
		return;
	}
	GtkPrintOperation *op = gtk_print_operation_new();
	char *base = g_path_get_basename(v->path);
	gtk_print_operation_set_job_name(op, base);
	g_free(base);
	gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
	g_signal_connect(op, "begin-print", G_CALLBACK(begin_print), v);
	g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), v);
	gem_print_dialog(GTK_WINDOW(v->window), op, NULL, 0, printed, v);
}

/* ---- Menus and keys ----------------------------------------------------- */

enum {
	ACT_OPEN = 1, ACT_PRINT, ACT_CLOSE, ACT_FIND, ACT_FIND_NEXT, ACT_FIND_PREV,
	ACT_FIT_WIDTH, ACT_FIT_PAGE, ACT_ACTUAL, ACT_ZOOM_IN, ACT_ZOOM_OUT,
	ACT_NEXT, ACT_PREV, ACT_FIRST, ACT_LAST, ACT_THUMBS,
};

static void build_menus(struct app_menu *m, void *data) {
	struct view *v = data;
	uint32_t doc = v->doc != NULL ? 0 : APP_MENU_DISABLED;
	uint32_t found = v->doc != NULL && v->find_text != NULL &&
		v->find_text[0] != '\0' ? 0 : APP_MENU_DISABLED;
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_OPEN, "Open...", "^O", 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_PRINT, "Print...", "^P", doc);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
	app_menu_add_menu(m, "Edit");
	app_menu_add_item(m, ACT_FIND, "Find...", "^F", doc);
	app_menu_add_item(m, ACT_FIND_NEXT, "Find Next", "^G", found);
	app_menu_add_item(m, ACT_FIND_PREV, "Find Previous", "^Shift+G", found);
	app_menu_add_menu(m, "View");
	app_menu_add_item(m, ACT_THUMBS, "Thumbnails", "F9",
		gtk_widget_get_visible(v->side) ? APP_MENU_CHECKED : 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_FIT_WIDTH, "Fit Width", NULL,
		doc | (v->fit == FIT_WIDTH ? APP_MENU_CHECKED : 0));
	app_menu_add_item(m, ACT_FIT_PAGE, "Fit Page", NULL,
		doc | (v->fit == FIT_PAGE ? APP_MENU_CHECKED : 0));
	app_menu_add_item(m, ACT_ACTUAL, "Actual Size", "^0",
		doc | (v->fit == FIT_NONE && v->zoom == 100 ? APP_MENU_CHECKED : 0));
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_ZOOM_IN, "Zoom In", "^+", doc);
	app_menu_add_item(m, ACT_ZOOM_OUT, "Zoom Out", "^-", doc);
	app_menu_add_menu(m, "Go");
	app_menu_add_item(m, ACT_PREV, "Previous Page", "PgUp", doc);
	app_menu_add_item(m, ACT_NEXT, "Next Page", "PgDn", doc);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_FIRST, "First Page", "Home", doc);
	app_menu_add_item(m, ACT_LAST, "Last Page", "End", doc);
}

static void act(struct view *v, int id) {
	if (gem_alert_up(v->host)) {
		return;
	}
	if (v->doc == NULL && id != ACT_OPEN && id != ACT_CLOSE &&
			id != ACT_THUMBS) {
		return;
	}
	switch (id) {
	case ACT_OPEN:
		ask_open(v);
		break;
	case ACT_PRINT:
		print(v);
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(v->window));
		break;
	case ACT_FIND:
		find_show(v, true);
		break;
	case ACT_FIND_NEXT:
	case ACT_FIND_PREV:
		if (!v->finding) {
			find_show(v, true);
		}
		find_step(v, id == ACT_FIND_NEXT ? 1 : -1);
		break;
	case ACT_FIT_WIDTH:
		set_zoom(v, FIT_WIDTH, v->zoom);
		break;
	case ACT_FIT_PAGE:
		set_zoom(v, FIT_PAGE, v->zoom);
		break;
	case ACT_ACTUAL:
		set_zoom(v, FIT_NONE, 100);
		break;
	case ACT_ZOOM_IN:
	case ACT_ZOOM_OUT:
		zoom_step(v, id == ACT_ZOOM_IN ? 1 : -1);
		break;
	case ACT_NEXT:
	case ACT_PREV:
		go_page(v, current_page(v) + (id == ACT_NEXT ? 1 : -1));
		break;
	case ACT_FIRST:
		go_page(v, 0);
		break;
	case ACT_LAST:
		go_page(v, v->n - 1);
		break;
	case ACT_THUMBS: {
		bool on = !gtk_widget_get_visible(v->side);
		save_thumbs(on);
		/* Every window, as it's one setting. */
		for (GList *l = views; l != NULL; l = l->next) {
			show_thumbs(l->data, on);
		}
		break;
	}
	}
}

static void menu_activate(uint32_t id, void *data) {
	act(data, (int)id);
}

static gboolean key_pressed(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, struct view *v) {
	if (gem_alert_up(v->host)) {
		return FALSE;
	}
	bool ctrl = mods & GDK_CONTROL_MASK, shift = mods & GDK_SHIFT_MASK;
	if (ctrl) {
		switch (gdk_keyval_to_lower(key)) {
		case GDK_KEY_o: act(v, ACT_OPEN); return TRUE;
		case GDK_KEY_p: act(v, ACT_PRINT); return TRUE;
		case GDK_KEY_w: case GDK_KEY_q: act(v, ACT_CLOSE); return TRUE;
		case GDK_KEY_f: act(v, ACT_FIND); return TRUE;
		case GDK_KEY_g: act(v, shift ? ACT_FIND_PREV : ACT_FIND_NEXT); return TRUE;
		case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
			act(v, ACT_ZOOM_IN);
			return TRUE;
		case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
			act(v, ACT_ZOOM_OUT);
			return TRUE;
		case GDK_KEY_0: act(v, ACT_ACTUAL); return TRUE;
		case GDK_KEY_Home: act(v, ACT_FIRST); return TRUE;
		case GDK_KEY_End: act(v, ACT_LAST); return TRUE;
		}
		return FALSE;
	}
	if (v->finding && gtk_widget_has_focus(v->find_field)) {
		if (key == GDK_KEY_Escape) {
			find_show(v, false);
			return TRUE;
		}
		if (key == GDK_KEY_Return && shift) {
			find_step(v, -1);
			return TRUE;
		}
		return FALSE;
	}
	double vy = gtk_adjustment_get_value(v->vadj);
	double page = gtk_adjustment_get_page_increment(v->vadj);
	switch (key) {
	case GDK_KEY_F9: act(v, ACT_THUMBS); return TRUE;
	case GDK_KEY_Escape:
		if (v->finding) {
			find_show(v, false);
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_Down: scroll_to(v, vy + STEP); return TRUE;
	case GDK_KEY_Up: scroll_to(v, vy - STEP); return TRUE;
	case GDK_KEY_Left:
		gtk_adjustment_set_value(v->hadj, gtk_adjustment_get_value(v->hadj) - STEP);
		return TRUE;
	case GDK_KEY_Right:
		gtk_adjustment_set_value(v->hadj, gtk_adjustment_get_value(v->hadj) + STEP);
		return TRUE;
	case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down:
		scroll_to(v, vy + page);
		return TRUE;
	case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
		scroll_to(v, vy - page);
		return TRUE;
	case GDK_KEY_space:
		scroll_to(v, vy + (shift ? -page : page));
		return TRUE;
	case GDK_KEY_Home: act(v, ACT_FIRST); return TRUE;
	case GDK_KEY_End: act(v, ACT_LAST); return TRUE;
	case GDK_KEY_plus: case GDK_KEY_equal: act(v, ACT_ZOOM_IN); return TRUE;
	case GDK_KEY_minus: act(v, ACT_ZOOM_OUT); return TRUE;
	}
	return FALSE;
}

/* The wheel scrolls, sideways too; with Ctrl, it zooms. */
static gboolean scrolled(GtkEventControllerScroll *c, double dx, double dy,
		struct view *v) {
	GdkModifierType mods = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(c));
	if (mods & GDK_CONTROL_MASK) {
		if (dy != 0) {
			zoom_step(v, dy < 0 ? 1 : -1);
		}
		return TRUE;
	}
	GdkScrollUnit unit = gtk_event_controller_scroll_get_unit(c);
	double k = unit == GDK_SCROLL_UNIT_WHEEL ? STEP : 1;
	scroll_to(v, gtk_adjustment_get_value(v->vadj) + dy * k);
	gtk_adjustment_set_value(v->hadj, gtk_adjustment_get_value(v->hadj) + dx * k);
	return TRUE;
}

static void motion(GtkEventControllerMotion *c, double x, double y,
		struct view *v) {
	PopplerLinkMapping *m = link_at(v, x, y);
	if (m != v->hover) {
		v->hover = m;
		gtk_widget_set_cursor_from_name(v->area, m != NULL ? "pointer" : NULL);
		gtk_widget_queue_draw(v->info);
	}
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		struct view *v) {
	gtk_widget_grab_focus(v->area);
	v->pressed_link = link_at(v, x, y) != NULL;
	v->press_x = x;
	v->press_y = y;
}

static void released(GtkGestureClick *g, int n, double x, double y,
		struct view *v) {
	if (!v->pressed_link || fabs(x - v->press_x) > 4 || fabs(y - v->press_y) > 4) {
		return;
	}
	v->pressed_link = false;
	PopplerLinkMapping *m = link_at(v, x, y);
	if (m != NULL && m->action != NULL) {
		follow(v, m->action);
	}
}

static void moved(GtkAdjustment *adj, struct view *v) {
	gtk_widget_queue_draw(v->area);
	if (adj == v->vadj) {
		side_follow(v);
	}
	if (v->message != NULL && adj == v->vadj) {
		g_clear_pointer(&v->message, g_free);
	}
	gtk_widget_queue_draw(v->info);
}

/* ---- Windows ------------------------------------------------------------ */

static gboolean close_request(GtkWindow *w, struct view *v) {
	remember(v);
	return FALSE;
}

static void view_free(void *data, GObject *gone) {
	struct view *v = data;
	views = g_list_remove(views, v);
	close_doc(v);
	g_clear_object(&v->monitor);
	g_clear_pointer(&v->matches, g_array_unref);
	g_free(v->find_text);
	g_free(v->message);
	g_free(v->path);
	g_free(v);
}

static struct view *view_new(GtkApplication *app) {
	struct view *v = g_new0(struct view, 1);
	v->fit = FIT_WIDTH;
	v->zoom = 100;
	v->current = -1;
	v->pending_page = -1;
	v->matches = g_array_new(FALSE, FALSE, sizeof(struct match));
	v->window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(v->window), "GemView");
	gtk_window_set_default_size(GTK_WINDOW(v->window), 760, 900);

	v->vadj = gtk_adjustment_new(0, 0, 1, STEP, 100, 1);
	v->hadj = gtk_adjustment_new(0, 0, 1, STEP, 100, 1);
	g_signal_connect(v->vadj, "value-changed", G_CALLBACK(moved), v);
	g_signal_connect(v->hadj, "value-changed", G_CALLBACK(moved), v);

	v->area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->area), draw, v, NULL);
	g_signal_connect(v->area, "resize", G_CALLBACK(resized), v);
	gtk_widget_set_hexpand(v->area, TRUE);
	gtk_widget_set_vexpand(v->area, TRUE);
	gtk_widget_set_focusable(v->area, TRUE);
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scrolled), v);
	gtk_widget_add_controller(v->area, scroll);
	GtkEventController *mo = gtk_event_controller_motion_new();
	g_signal_connect(mo, "motion", G_CALLBACK(motion), v);
	gtk_widget_add_controller(v->area, mo);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), v);
	g_signal_connect(click, "released", G_CALLBACK(released), v);
	gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(click));

	/* The thumbnails: a list down the left, its own scroll bar beside it. */
	v->side_adj = gtk_adjustment_new(0, 0, 1, 40, 100, 1);
	g_signal_connect(v->side_adj, "value-changed", G_CALLBACK(side_moved), v);
	v->side_area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->side_area), side_draw, v,
		NULL);
	gtk_widget_set_size_request(v->side_area, SIDE_W, -1);
	gtk_widget_set_vexpand(v->side_area, TRUE);
	g_signal_connect(v->side_area, "resize", G_CALLBACK(side_resized), v);
	GtkGesture *side_click = gtk_gesture_click_new();
	g_signal_connect(side_click, "pressed", G_CALLBACK(side_pressed), v);
	gtk_widget_add_controller(v->side_area, GTK_EVENT_CONTROLLER(side_click));
	GtkEventController *side_scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(side_scroll, "scroll", G_CALLBACK(side_scrolled), v);
	gtk_widget_add_controller(v->side_area, side_scroll);
	v->side = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(v->side), v->side_area);
	gtk_box_append(GTK_BOX(v->side),
		gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->side_adj));
	v->side_current = -1;
	gtk_widget_set_visible(v->side, setting_thumbs());

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_attach(GTK_GRID(grid), v->area, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid),
		gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->vadj), 1, 0, 1, 1);
	v->hbar = gem_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, v->hadj);
	gtk_widget_set_visible(v->hbar, FALSE);
	gtk_grid_attach(GTK_GRID(grid), v->hbar, 0, 1, 1, 1);

	/* Find: a row over the info line, its field over the row. */
	v->find_area = gem_pixel_area_new(0, FIND_H, paint_find, v);
	v->find_row = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(v->find_row), v->find_area);
	v->find_field = gem_field_new(GEM_PAD + (int)gem_measure("Find: ") + 4,
		(FIND_H - GEM_FIELD_H) / 2 + 1, 300);
	gtk_overlay_add_overlay(GTK_OVERLAY(v->find_row), v->find_field);
	g_signal_connect(v->find_field, "changed", G_CALLBACK(find_changed), v);
	g_signal_connect(v->find_field, "activate", G_CALLBACK(find_activate), v);
	gtk_widget_set_visible(v->find_row, FALSE);

	v->info = gem_pixel_area_new(0, INFO_H, paint_info, v);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(middle), v->side);
	gtk_box_append(GTK_BOX(middle), grid);
	gtk_widget_set_hexpand(grid, TRUE);
	gtk_widget_set_vexpand(middle, TRUE);
	gtk_box_append(GTK_BOX(outer), middle);
	gtk_box_append(GTK_BOX(outer), v->find_row);
	gtk_box_append(GTK_BOX(outer), v->info);
	v->host = GTK_OVERLAY(gtk_overlay_new());
	gtk_overlay_set_child(v->host, outer);
	gtk_window_set_child(GTK_WINDOW(v->window), GTK_WIDGET(v->host));

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), v);
	gtk_widget_add_controller(v->window, keys);
	g_signal_connect(v->window, "close-request", G_CALLBACK(close_request), v);
	g_object_weak_ref(G_OBJECT(v->window), view_free, v);

	v->menu = app_menu_new(v->window, build_menus, menu_activate, v);
	views = g_list_append(views, v);
	gtk_window_present(GTK_WINDOW(v->window));
	gtk_widget_grab_focus(v->area);
	return v;
}

/* ---- The application ---------------------------------------------------- */

static void activate(GtkApplication *app, void *data) {
	/* With nothing to show: a window, and the item selector to pick one. */
	struct view *v = view_new(app);
	ask_open(v);
}

static void open_files(GApplication *app, GFile **files, int n,
		const char *hint, void *data) {
	for (int i = 0; i < n; i++) {
		char *path = g_file_get_path(files[i]);
		if (path == NULL) {
			continue;
		}
		/* Already open: that window, forward. */
		struct view *found = NULL;
		for (GList *l = views; l != NULL; l = l->next) {
			struct view *v = l->data;
			if (g_strcmp0(v->path, path) == 0) {
				found = v;
			}
		}
		if (found == NULL) {
			found = view_new(GTK_APPLICATION(app));
			open_path(found, path);
		}
		gtk_window_present(GTK_WINDOW(found->window));
		g_free(path);
	}
}

static void startup(GApplication *app, void *data) {
	gem_ui_load_css();
}

int main(int argc, char *argv[]) {
	gem_print_setup();
	GtkApplication *app = gtk_application_new("org.gemwm.GemView",
		G_APPLICATION_HANDLES_OPEN);
	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
