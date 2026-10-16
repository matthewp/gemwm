/*
 * GemPod: podcasts, drawn as GEM would have. The podcasts down the left
 * (New Episodes, every podcast's fresh ones, at the top), the one picked's
 * episodes on the right, and what's playing along the bottom: its artwork
 * in the ST's colours, back 15 seconds, play, on 30, where it's got to,
 * and the speed.
 *
 * It syncs with a gpodder sync server (sync.c), File > Sync Settings:
 * subscriptions, and where you got to in each episode, shared with your
 * phone's AntennaPod and the like. Feeds are read here (feed.c), episodes
 * play streamed or downloaded (player.c), and everything's kept in SQLite
 * (store.c). The password goes to the Secret Service if there's one, else
 * in ~/.config/gempod/settings, readable only by you.
 *
 * It's an MPRIS player (org.mpris.MediaPlayer2.gempod), so the media keys
 * work: Next and Previous go on 30 and back 15 seconds.
 */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libsecret/secret.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <math.h>
#include <string.h>
#include "app-menu.h"
#include "dither.h"
#include "feed.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-file.h"
#include "gem-popup.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "player.h"
#include "store.h"
#include "sync.h"

#define LEFT_W 250
#define PODCAST_H 40
#define EPISODE_H 38
#define ART_SMALL 32
#define ART_BIG 64
#define DECK_H (ART_BIG + 2 * GEM_PAD + 1)
#define INFO_H (GEM_ROW_H + 1)
#define BUTTON_H 22
#define BACK 15
#define ON 30
#define FETCHING 4                   /* feeds read at once */
#define SAVE_EVERY 15                /* seconds playing between saves */
#define SYNC_EVERY (15 * 60)
#define SYNC_AFTER 20                /* seconds after a change */
#define REFRESH_EVERY (60 * 60)

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_NAME "org.mpris.MediaPlayer2.gempod"
/* The left list's first rows, before the podcasts: what's started (from
 * any device, as synced), then what's new. */
#define LISTENING "\001listening"
#define NEW_EPISODES ""
#define TOP_ROWS 2

static const double speeds[] = { 0.8, 1.0, 1.2, 1.5, 1.75, 2.0 };

/* Artwork, fetched once and kept in ~/.cache/gempod/art, dithered. */
struct art {
	cairo_surface_t *small, *big;
	bool loading;
};

static struct {
	GtkApplication *app;
	GtkWidget *window, *left, *right, *deck, *info;
	GtkOverlay *host;
	GtkAdjustment *ladj, *radj;
	struct app_menu *menu;
	struct gem_popup *popup, *lpopup;
	GArray *deck_hits;
	SoupSession *soup;

	GPtrArray *podcasts;       /* struct podcast */
	char *picked;              /* a podcast's url, LISTENING or NEW_EPISODES */
	int started_count;         /* episodes in Currently Listening */
	GPtrArray *episodes;       /* struct episode, of the picked */
	int cursor;                /* in episodes */
	bool right_focus;

	/* Playing. */
	struct episode *playing;   /* a copy */
	int started;               /* where this play began, for sync */
	int queued;                /* the place last queued for sync, or -1 */
	gint64 saved_at;
	bool scrubbing;
	double scrub_to;

	/* Feeds being read. */
	GQueue to_fetch;           /* urls */
	int fetching, fetch_total, fetch_done, fetch_new;

	/* Sync. */
	struct sync_account account;
	bool auto_sync;
	bool syncing, sync_again;
	char *sync_status;
	guint sync_soon;

	/* Downloads, one at a time. */
	GQueue to_download;        /* urls */
	char *downloading;

	GHashTable *art;           /* url -> struct art */
	char *message;
	guint message_id;
	GDBusConnection *bus;
} ui;

static void reload(void);
static void sync_later(int seconds);
static void say(char *message);
static void mpris_changed(const char *prop, GVariant *v);
static GVariant *metadata(void);
static void refresh(const char *url);
static void start_download(void);

/* ---- Small things ------------------------------------------------------------ */

static char *minutes(double s) {
	int t = (int)MAX(s, 0);
	return t >= 3600 ? g_strdup_printf("%d:%02d:%02d", t / 3600, t / 60 % 60,
		t % 60) : g_strdup_printf("%d:%02d", t / 60, t % 60);
}

static char *when(gint64 t) {
	if (t <= 0) {
		return g_strdup("");
	}
	GDateTime *d = g_date_time_new_from_unix_local(t);
	GDateTime *now = g_date_time_new_now_local();
	char *s = g_date_time_format(d, g_date_time_get_year(d) ==
		g_date_time_get_year(now) ? "%-d %b" : "%-d %b %Y");
	g_date_time_unref(now);
	g_date_time_unref(d);
	return s;
}

static const char *podcast_name(const struct podcast *p) {
	return p->title != NULL && p->title[0] != '\0' ? p->title : p->url;
}

static struct podcast *podcast_of(const char *url) {
	for (guint i = 0; ui.podcasts != NULL && i < ui.podcasts->len; i++) {
		struct podcast *p = ui.podcasts->pdata[i];
		if (strcmp(p->url, url) == 0) {
			return p;
		}
	}
	return NULL;
}

static bool showing_new(void) {
	return ui.picked == NULL || strcmp(ui.picked, NEW_EPISODES) == 0;
}

static bool showing_listening(void) {
	return ui.picked != NULL && strcmp(ui.picked, LISTENING) == 0;
}

/* Episodes from every podcast, not one's. */
static bool showing_all(void) {
	return showing_new() || showing_listening();
}

/* The left list's row i: LISTENING, NEW_EPISODES or a podcast's url. */
static const char *left_row(int i) {
	return i == 0 ? LISTENING : i == 1 ? NEW_EPISODES :
		((struct podcast *)ui.podcasts->pdata[i - TOP_ROWS])->url;
}

static struct episode *cursor_episode(void) {
	return ui.cursor >= 0 && ui.episodes != NULL &&
		ui.cursor < (int)ui.episodes->len ? ui.episodes->pdata[ui.cursor] : NULL;
}

/* ---- Settings and the password ------------------------------------------------ */

static const SecretSchema *schema(void) {
	static const SecretSchema s = {
		.name = "org.gemwm.Pod",
		.flags = SECRET_SCHEMA_NONE,
		.attributes = {
			{ "server", SECRET_SCHEMA_ATTRIBUTE_STRING },
			{ "username", SECRET_SCHEMA_ATTRIBUTE_STRING },
			{ NULL, 0 },
		},
	};
	return &s;
}

static char *settings_path(void) {
	return g_build_filename(g_get_user_config_dir(), "gempod", "settings", NULL);
}

static bool account_set(void) {
	return ui.account.server != NULL && ui.account.server[0] != '\0' &&
		ui.account.username != NULL && ui.account.username[0] != '\0' &&
		ui.account.password != NULL;
}

static char *default_device(void) {
	char *host = g_ascii_strdown(g_get_host_name(), -1);
	for (char *c = host; *c; c++) {
		if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_') {
			*c = '-';
		}
	}
	char *d = g_strconcat("gempod-", host, NULL);
	g_free(host);
	return d;
}

static void load_settings(void) {
	GKeyFile *kf = g_key_file_new();
	char *path = settings_path();
	g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
	ui.account.server = g_key_file_get_string(kf, "sync", "server", NULL);
	ui.account.username = g_key_file_get_string(kf, "sync", "username", NULL);
	ui.account.device = g_key_file_get_string(kf, "sync", "device", NULL);
	if (ui.account.device == NULL) {
		ui.account.device = default_device();
	}
	ui.account.caption = g_strdup_printf("GemPod on %s", g_get_host_name());
	GError *e = NULL;
	ui.auto_sync = g_key_file_get_boolean(kf, "sync", "automatic", &e);
	if (e != NULL) {
		ui.auto_sync = true;
		g_error_free(e);
	}
	ui.account.password = g_key_file_get_string(kf, "sync", "password", NULL);
	if (ui.account.password == NULL && ui.account.server != NULL &&
			ui.account.username != NULL) {
		ui.account.password = secret_password_lookup_sync(schema(), NULL, NULL,
			"server", ui.account.server, "username", ui.account.username, NULL);
	}
	g_key_file_unref(kf);
	g_free(path);
}

/* Saved; the password to the Secret Service if it's there (in_file: it
 * wasn't, so it went in the settings file). */
static void save_settings(bool *in_file) {
	GKeyFile *kf = g_key_file_new();
	if (ui.account.server != NULL) {
		g_key_file_set_string(kf, "sync", "server", ui.account.server);
		g_key_file_set_string(kf, "sync", "username", ui.account.username);
	}
	g_key_file_set_string(kf, "sync", "device", ui.account.device);
	g_key_file_set_boolean(kf, "sync", "automatic", ui.auto_sync);
	bool file = false;
	if (account_set()) {
		GError *e = NULL;
		if (!secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT,
				"GemPod sync", ui.account.password, NULL, &e,
				"server", ui.account.server, "username", ui.account.username,
				NULL)) {
			g_clear_error(&e);
			g_key_file_set_string(kf, "sync", "password", ui.account.password);
			file = true;
		}
	}
	char *path = settings_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	char *data = g_key_file_to_data(kf, NULL, NULL);
	g_file_set_contents_full(path, data, -1, G_FILE_SET_CONTENTS_CONSISTENT,
		0600, NULL);
	g_free(data);
	g_free(dir);
	g_free(path);
	g_key_file_unref(kf);
	if (in_file != NULL) {
		*in_file = file;
	}
}

/* ---- Artwork ---------------------------------------------------------------- */

static char *art_path(const char *url) {
	char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url, -1);
	char *path = g_build_filename(g_get_user_cache_dir(), "gempod", "art", sum,
		NULL);
	g_free(sum);
	return path;
}

static cairo_surface_t *scaled(GdkPixbuf *pb, int size) {
	GdkPixbuf *sq = gdk_pixbuf_scale_simple(pb, size, size, GDK_INTERP_BILINEAR);
	GdkPixbuf *rgba = gdk_pixbuf_add_alpha(sq, FALSE, 0, 0, 0);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
	guint32 *dst = (guint32 *)cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s) / 4;
	int rs = gdk_pixbuf_get_rowstride(rgba);
	const guchar *px = gdk_pixbuf_read_pixels(rgba);
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			const guchar *q = px + y * rs + x * 4;
			dst[y * ds + x] = 0xff000000u | q[0] << 16 | q[1] << 8 | q[2];
		}
	}
	cairo_surface_mark_dirty(s);
	dither_surface(s, 1, DITHER_ST16, DITHER_DIFFUSE);
	g_object_unref(rgba);
	g_object_unref(sq);
	return s;
}

struct art_job {
	char *url;
	cairo_surface_t *small, *big;
};

static void art_decode(GTask *task, gpointer src, gpointer data, GCancellable *c) {
	struct art_job *j = data;
	char *path = art_path(j->url);
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
	g_free(path);
	if (pb != NULL) {
		j->small = scaled(pb, ART_SMALL);
		j->big = scaled(pb, ART_BIG);
		g_object_unref(pb);
	}
	g_task_return_boolean(task, TRUE);
}

static void art_decoded(GObject *src, GAsyncResult *res, gpointer data) {
	struct art_job *j = g_task_get_task_data(G_TASK(res));
	struct art *a = g_hash_table_lookup(ui.art, j->url);
	if (a != NULL) {
		a->loading = false;
		a->small = j->small;
		a->big = j->big;
		j->small = j->big = NULL;
		gtk_widget_queue_draw(ui.left);
		gtk_widget_queue_draw(ui.deck);
	}
}

static void art_job_free(gpointer data) {
	struct art_job *j = data;
	g_clear_pointer(&j->small, cairo_surface_destroy);
	g_clear_pointer(&j->big, cairo_surface_destroy);
	g_free(j->url);
	g_free(j);
}

static void art_decode_start(const char *url) {
	struct art_job *j = g_new0(struct art_job, 1);
	j->url = g_strdup(url);
	GTask *task = g_task_new(NULL, NULL, art_decoded, NULL);
	g_task_set_task_data(task, j, art_job_free);
	g_task_run_in_thread(task, art_decode);
	g_object_unref(task);
}

static void art_fetched(GObject *src, GAsyncResult *res, gpointer data) {
	char *url = data;
	GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
	if (body != NULL && g_bytes_get_size(body) > 0) {
		char *path = art_path(url);
		char *dir = g_path_get_dirname(path);
		g_mkdir_with_parents(dir, 0700);
		g_file_set_contents(path, g_bytes_get_data(body, NULL),
			g_bytes_get_size(body), NULL);
		g_free(dir);
		g_free(path);
		art_decode_start(url);
	}
	if (body != NULL) {
		g_bytes_unref(body);
	}
	g_free(url);
}

/* The picture at url, dithered, if it's ready; asked for if not. */
static struct art *art_for(const char *url) {
	if (url == NULL || url[0] == '\0') {
		return NULL;
	}
	struct art *a = g_hash_table_lookup(ui.art, url);
	if (a != NULL) {
		return a;
	}
	a = g_new0(struct art, 1);
	a->loading = true;
	g_hash_table_insert(ui.art, g_strdup(url), a);
	char *path = art_path(url);
	if (g_file_test(path, G_FILE_TEST_EXISTS)) {
		art_decode_start(url);
	} else {
		SoupMessage *msg = soup_message_new("GET", url);
		if (msg != NULL) {
			soup_session_send_and_read_async(ui.soup, msg, G_PRIORITY_LOW, NULL,
				art_fetched, g_strdup(url));
			g_object_unref(msg);
		}
	}
	g_free(path);
	return a;
}

static void art_free(gpointer data) {
	struct art *a = data;
	g_clear_pointer(&a->small, cairo_surface_destroy);
	g_clear_pointer(&a->big, cairo_surface_destroy);
	g_free(a);
}

/* ---- Drawing: little pictures -------------------------------------------------- */

static void triangle(cairo_t *cr, int x, int y, int h) {
	for (int r = 0; r < h; r++) {
		gem_fill(cr, x, y + r, r < h / 2 ? r + 1 : h - r, 1);
	}
}

static void diamond(cairo_t *cr, int x, int y) {
	for (int r = 0; r < 7; r++) {
		int half = r < 4 ? r : 6 - r;
		gem_fill(cr, x + 3 - half, y + r, 2 * half + 1, 1);
	}
}

/* A podcast with no artwork (yet): a microphone, on GEM's grey. */
static const char *const mic[] = {
	"......######......",
	".....#......#.....",
	"....#.##..##.#....",
	"....#........#....",
	"....#.##..##.#....",
	"....#........#....",
	"....#.##..##.#....",
	"....#........#....",
	"..#.#........#.#..",
	"..#..#......#..#..",
	"..#...######...#..",
	"...#....##....#...",
	"....##..##..##....",
	"......######......",
	"........##........",
	"........##........",
	".....########.....",
};

static void no_art(cairo_t *cr, int x, int y, int size) {
	gem_white(cr);
	gem_fill(cr, x, y, size, size);
	gem_black(cr);
	gem_dither(cr, x + 1, y + 1, size - 2, size - 2);
	int n = G_N_ELEMENTS(mic), w = (int)strlen(mic[0]);
	int scale = MAX(1, (size - 8) / n);
	int ox = x + (size - w * scale) / 2, oy = y + (size - n * scale) / 2;
	/* White behind it, so it stands out of the grey. */
	gem_white(cr);
	gem_fill(cr, ox - scale, oy - scale, (w + 2) * scale, (n + 2) * scale);
	gem_black(cr);
	for (int r = 0; r < n; r++) {
		for (int c = 0; c < w; c++) {
			if (mic[r][c] == '#') {
				gem_fill(cr, ox + c * scale, oy + r * scale, scale, scale);
			}
		}
	}
}

/* Currently Listening's picture: headphones. */
static const char *const phones[] = {
	".....######.....",
	"...##......##...",
	"..#..........#..",
	".#............#.",
	".#............#.",
	"#..............#",
	"#..............#",
	"####........####",
	"#..#........#..#",
	"#..#........#..#",
	"#..#........#..#",
	"#..#........#..#",
	"####........####",
};

static void headphones(cairo_t *cr, int x, int y) {
	gem_white(cr);
	gem_fill(cr, x, y, ART_SMALL, ART_SMALL);
	gem_black(cr);
	int n = G_N_ELEMENTS(phones), w = (int)strlen(phones[0]);
	int ox = x + (ART_SMALL - w) / 2, oy = y + (ART_SMALL - n) / 2;
	for (int r = 0; r < n; r++) {
		for (int c = 0; c < w; c++) {
			if (phones[r][c] == '#') {
				gem_fill(cr, ox + c, oy + r, 1, 1);
			}
		}
	}
	/* Its cups, filled. */
	gem_fill(cr, ox + 1, oy + 8, 2, 4);
	gem_fill(cr, ox + w - 3, oy + 8, 2, 4);
}

/* The New Episodes row's picture: an in-tray. */
static void tray(cairo_t *cr, int x, int y) {
	gem_white(cr);
	gem_fill(cr, x, y, ART_SMALL, ART_SMALL);
	gem_black(cr);
	gem_frame(cr, x + 3, y + 14, 26, 14, 1);
	gem_fill(cr, x + 3, y + 20, 8, 1);
	gem_fill(cr, x + 21, y + 20, 8, 1);
	gem_fill(cr, x + 11, y + 20, 1, 3);
	gem_fill(cr, x + 20, y + 20, 1, 3);
	gem_fill(cr, x + 11, y + 23, 10, 1);
	gem_fill(cr, x + 15, y + 3, 2, 9);         /* an arrow in */
	for (int r = 0; r < 4; r++) {
		gem_fill(cr, x + 12 + r, y + 9 + r, 8 - 2 * r, 1);
	}
}

static void picture(cairo_t *cr, const char *url, int x, int y, int size) {
	struct art *a = art_for(url);
	cairo_surface_t *s = a == NULL ? NULL : size == ART_SMALL ? a->small : a->big;
	if (s != NULL) {
		cairo_set_source_surface(cr, s, x, y);
		cairo_paint(cr);
	} else {
		no_art(cr, x, y, size);
	}
	gem_black(cr);
	gem_frame(cr, x, y, size, size, 1);
}

/* ---- Drawing: the podcasts ------------------------------------------------------ */

static double lscroll(void) {
	return gtk_adjustment_get_value(ui.ladj);
}

static double rscroll(void) {
	return gtk_adjustment_get_value(ui.radj);
}

static int n_left(void) {
	return TOP_ROWS + (int)ui.podcasts->len;
}

static void fit(GtkWidget *area, GtkAdjustment *adj, int rows, int row_h) {
	int h = gtk_widget_get_height(area);
	int total = MAX(rows * row_h + 4, h);
	gtk_adjustment_configure(adj, CLAMP(gtk_adjustment_get_value(adj), 0,
		MAX(total - h, 0)), 0, total, row_h, MAX(h - row_h, row_h), h);
}

static void paint_left(cairo_t *cr, int w, int h, void *data) {
	int y0 = (int)lscroll();
	int fresh_total = 0;
	for (guint i = 0; i < ui.podcasts->len; i++) {
		fresh_total += ((struct podcast *)ui.podcasts->pdata[i])->fresh;
	}
	for (int i = 0; i < n_left(); i++) {
		int y = 2 + i * PODCAST_H - y0;
		if (y + PODCAST_H < 0 || y > h) {
			continue;
		}
		struct podcast *p = i >= TOP_ROWS ? ui.podcasts->pdata[i - TOP_ROWS] : NULL;
		bool picked = g_strcmp0(ui.picked, left_row(i)) == 0;
		int ax = GEM_PAD, ay = y + (PODCAST_H - ART_SMALL) / 2;
		if (p != NULL) {
			picture(cr, p->image, ax, ay, ART_SMALL);
		} else {
			if (i == 0) {
				headphones(cr, ax, ay);
			} else {
				tray(cr, ax, ay);
			}
			gem_frame(cr, ax, ay, ART_SMALL, ART_SMALL, 1);
		}
		int tx = ax + ART_SMALL + GEM_PAD, tw = w - tx - GEM_PAD;
		if (picked) {
			gem_black(cr);
			gem_fill(cr, tx - 3, y + 1, w - tx + 3, PODCAST_H - 2);
			gem_white(cr);
		} else {
			gem_black(cr);
		}
		const char *name = p != NULL ? podcast_name(p) :
			i == 0 ? "Currently Listening" : "New Episodes";
		int fresh = p != NULL ? p->fresh : i == 0 ? ui.started_count : fresh_total;
		char *count = fresh > 0 ? g_strdup_printf("%d", fresh) : NULL;
		int cw = count != NULL ? (int)gem_text_width(cr, count) + GEM_PAD : 0;
		gem_text_clipped(cr, name, tx, y + 2, tw - cw, GEM_ROW_H);
		if (count != NULL) {
			gem_text(cr, count, w - GEM_PAD - (int)gem_text_width(cr, count), y + 2,
				GEM_ROW_H);
		}
		const char *second = i == 0 ? "Started, on any device" :
			p == NULL ? "From every podcast" :
			p->fetched == 0 ? "Not read yet" : p->author;
		if (second != NULL) {
			gem_text_clipped(cr, second, tx, y + 2 + GEM_ROW_H - 2, tw, GEM_ROW_H);
		}
		g_free(count);
		gem_black(cr);
	}
	if (ui.podcasts->len == 0) {
		int ty = 2 + TOP_ROWS * PODCAST_H + GEM_PAD - y0;
		gem_text(cr, "No podcasts yet:", GEM_PAD, ty, GEM_ROW_H);
		gem_text(cr, "File > Add Podcast.", GEM_PAD, ty + GEM_ROW_H, GEM_ROW_H);
		gem_grey_out(cr, 0, ty, w, 2 * GEM_ROW_H);
	}
}

/* ---- Drawing: the episodes ------------------------------------------------------- */

static bool is_playing(const struct episode *e) {
	return ui.playing != NULL && player_state() != PLAYER_STOPPED &&
		strcmp(ui.playing->url, e->url) == 0;
}

static void paint_right(cairo_t *cr, int w, int h, void *data) {
	int y0 = (int)rscroll();
	gem_black(cr);
	gem_fill(cr, 0, 0, 1, h);   /* the line between the lists */
	if (ui.episodes->len == 0) {
		const char *s = showing_listening() ? "Nothing started." :
			showing_new() ? "Nothing new." : "No episodes.";
		gem_text(cr, s, (w - (int)gem_text_width(cr, s)) / 2, h / 3, GEM_ROW_H);
		gem_grey_out(cr, 1, h / 3, w - 1, GEM_ROW_H);
		return;
	}
	for (guint i = 0; i < ui.episodes->len; i++) {
		int y = 2 + (int)i * EPISODE_H - y0;
		if (y + EPISODE_H < 0 || y > h) {
			continue;
		}
		struct episode *e = ui.episodes->pdata[i];
		bool picked = (int)i == ui.cursor;
		gem_black(cr);
		if (picked) {
			gem_fill(cr, 1, y, w - 1, EPISODE_H);
			gem_white(cr);
		}
		/* Its state: playing, fresh, started (how far, as a bar). */
		int gx = GEM_PAD, gy = y + 6;
		if (is_playing(e)) {
			if (player_state() == PLAYER_PAUSED) {
				gem_fill(cr, gx, gy, 2, 8);
				gem_fill(cr, gx + 4, gy, 2, 8);
			} else {
				triangle(cr, gx, gy, 9);
			}
		} else if (e->fresh) {
			diamond(cr, gx, gy + 1);
		}
		int len = e->total > 0 ? e->total : e->duration;
		int tx = gx + 14, tw = w - tx - GEM_PAD;
		char *date = when(e->published);
		int dw = (int)gem_text_width(cr, date);
		gem_text_clipped(cr, e->title != NULL ? e->title : e->url, tx, y + 1,
			tw - dw - GEM_PAD, GEM_ROW_H);
		if (e->fresh) {
			gem_text_clipped(cr, e->title != NULL ? e->title : e->url, tx + 1, y + 1,
				tw - dw - GEM_PAD - 1, GEM_ROW_H);   /* bold */
		}
		gem_text(cr, date, w - GEM_PAD - dw, y + 1, GEM_ROW_H);
		g_free(date);
		/* The second line: how long, or how much is left, and whose. */
		GString *s = g_string_new(NULL);
		if (e->played) {
			g_string_append(s, "Played");
		} else if (e->position > 0 && len > 0) {
			char *left = minutes(len - e->position);
			g_string_append_printf(s, "%s left", left);
			g_free(left);
		} else if (len > 0) {
			char *l = minutes(len);
			g_string_append(s, l);
			g_free(l);
		}
		if (e->file != NULL) {
			g_string_append(s, s->len > 0 ? ", downloaded" : "Downloaded");
		} else if (ui.downloading != NULL && strcmp(ui.downloading, e->url) == 0) {
			g_string_append(s, s->len > 0 ? ", downloading..." : "Downloading...");
		}
		if (showing_all() && e->podcast_title != NULL) {
			g_string_append_printf(s, "%s%s", s->len > 0 ? "  -  " : "",
				e->podcast_title);
		}
		int ly = y + GEM_ROW_H - 2;
		int bar = e->position > 0 && len > 0 && !e->played ? 40 : 0;
		if (bar > 0) {
			gem_frame(cr, tx, ly + 6, bar, 7, 1);
			gem_fill(cr, tx + 1, ly + 7, (bar - 2) * MIN(e->position, len) / len, 5);
		}
		gem_text_clipped(cr, s->str, tx + (bar > 0 ? bar + GEM_PAD : 0), ly,
			tw - (bar > 0 ? bar + GEM_PAD : 0), GEM_ROW_H);
		g_string_free(s, TRUE);
		if (e->played && !picked) {
			gem_grey_out(cr, tx, y + 1, tw, EPISODE_H - 2);
		}
		gem_black(cr);
		for (int x = GEM_PAD; x < w - GEM_PAD; x += 2) {   /* dotted rule */
			gem_fill(cr, x, y + EPISODE_H - 1, 1, 1);
		}
	}
}

/* ---- Drawing: what's playing ------------------------------------------------------ */

enum { HIT_BACK = 1, HIT_PLAY, HIT_ON, HIT_BAR, HIT_SPEED };

static void paint_deck(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(ui.deck_hits, 0);
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	struct episode *e = ui.playing;
	int x = GEM_PAD, y = GEM_PAD + 1;
	if (e != NULL) {
		struct podcast *p = podcast_of(e->podcast);
		picture(cr, e->image != NULL ? e->image : p != NULL ? p->image : NULL, x, y,
			ART_BIG);
	} else {
		no_art(cr, x, y, ART_BIG);
		gem_black(cr);
		gem_frame(cr, x, y, ART_BIG, ART_BIG, 1);
	}
	int tx = x + ART_BIG + GEM_PAD, tw = w - tx - GEM_PAD;
	char *speed = g_strdup_printf("%gx", player_speed());
	int sw = MAX(gem_button_width(speed), 52);
	if (e != NULL) {
		gem_text_clipped(cr, e->title != NULL ? e->title : e->url, tx, y, tw - sw -
			GEM_PAD, GEM_ROW_H);
		gem_text_clipped(cr, e->title != NULL ? e->title : e->url, tx + 1, y,
			tw - sw - GEM_PAD - 1, GEM_ROW_H);
		struct podcast *p = podcast_of(e->podcast);
		const char *who = player_buffering() ? "Waiting for the network..." :
			player_error() != NULL ? player_error() :
			p != NULL ? podcast_name(p) : "";
		gem_text_clipped(cr, who, tx, y + GEM_ROW_H, tw - sw - GEM_PAD, GEM_ROW_H);
	} else {
		gem_text(cr, "Nothing playing.", tx, y, GEM_ROW_H);
		gem_text(cr, "Double-click an episode to play it.", tx, y + GEM_ROW_H,
			GEM_ROW_H);
		gem_grey_out(cr, tx, y, tw, 2 * GEM_ROW_H);
	}
	/* The speed, a button that goes round them. */
	gem_button(cr, ui.deck_hits, w - GEM_PAD - sw, y, sw, speed, HIT_SPEED, false);
	g_free(speed);

	/* Back 15, play, on 30, then where it's got to. */
	int by = y + ART_BIG - BUTTON_H;
	int bx = tx;
	gem_button(cr, ui.deck_hits, bx, by, 40, "-15", HIT_BACK, false);
	bx += 44;
	gem_white(cr);
	gem_fill(cr, bx, by, 40, BUTTON_H);
	gem_black(cr);
	gem_frame(cr, bx, by, 40, BUTTON_H, 2);
	if (player_state() == PLAYER_PLAYING) {
		gem_fill(cr, bx + 14, by + 6, 3, 10);
		gem_fill(cr, bx + 22, by + 6, 3, 10);
	} else {
		triangle(cr, bx + 16, by + 6, 11);
	}
	gem_hit_add(ui.deck_hits, bx, by, 40, BUTTON_H, HIT_PLAY, 0);
	bx += 44;
	gem_button(cr, ui.deck_hits, bx, by, 40, "+30", HIT_ON, false);
	bx += 40 + GEM_PAD;
	double len = player_length() > 0 ? player_length() :
		e != NULL ? (e->total > 0 ? e->total : e->duration) : 0;
	double pos = ui.scrubbing ? ui.scrub_to : player_state() != PLAYER_STOPPED ?
		player_position() : e != NULL ? e->position : 0;
	char *now = minutes(pos), *total = minutes(len);
	int nw = (int)gem_measure("0:00:00");
	gem_text(cr, now, bx + nw - (int)gem_text_width(cr, now), by + 1, BUTTON_H);
	int barx = bx + nw + GEM_PAD;
	int tw2 = (int)gem_text_width(cr, total);
	int barw = w - barx - GEM_PAD - tw2 - GEM_PAD;
	gem_text(cr, total, w - GEM_PAD - tw2, by + 1, BUTTON_H);
	g_free(now);
	g_free(total);
	if (barw > 20) {
		int bt = by + (BUTTON_H - 11) / 2;
		gem_dither(cr, barx + 1, bt + 1, barw - 2, 9);
		gem_frame(cr, barx, bt, barw, 11, 1);
		if (len > 0) {
			gem_fill(cr, barx + 1, bt + 1, (int)((barw - 2) * CLAMP(pos / len, 0, 1)),
				9);
		}
		gem_hit_add(ui.deck_hits, barx, by, barw, BUTTON_H, HIT_BAR, 0);
	}
}

static void paint_info(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	char *s;
	if (ui.message != NULL) {
		s = g_strdup(ui.message);
	} else if (ui.fetching > 0 || !g_queue_is_empty(&ui.to_fetch)) {
		s = g_strdup_printf("Reading feeds: %d of %d...", ui.fetch_done,
			ui.fetch_total);
	} else if (ui.syncing) {
		s = g_strdup("Syncing...");
	} else if (ui.downloading != NULL) {
		struct episode *e = store_episode(ui.downloading);
		s = g_strdup_printf("Downloading %s...", e != NULL && e->title != NULL ?
			e->title : ui.downloading);
		episode_free(e);
	} else {
		int n = (int)ui.podcasts->len;
		s = g_strdup_printf("%d %s%s%s", n, n == 1 ? "podcast" : "podcasts",
			ui.sync_status != NULL ? ", " : "",
			ui.sync_status != NULL ? ui.sync_status : "");
	}
	gem_text_clipped(cr, s, GEM_PAD, 1, w - 2 * GEM_PAD, h - 1);
	g_free(s);
}

static void draw_left(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_left, NULL);
}

static void draw_right(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_right, NULL);
}

static void draw_deck(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_deck, NULL);
}

static gboolean message_gone(gpointer data) {
	ui.message_id = 0;
	g_clear_pointer(&ui.message, g_free);
	gtk_widget_queue_draw(ui.info);
	return G_SOURCE_REMOVE;
}

static void say(char *message) {
	g_free(ui.message);
	ui.message = message;
	if (ui.message_id != 0) {
		g_source_remove(ui.message_id);
	}
	ui.message_id = g_timeout_add(5000, message_gone, NULL);
	gtk_widget_queue_draw(ui.info);
}

static void alert(enum gem_alert_icon icon, const char *text) {
	static const char *const ok[] = { "OK", NULL };
	gem_alert(ui.host, icon, text, ok, 0, 0, NULL, NULL);
}

/* ---- Showing what's there ---------------------------------------------------------- */

static void load_episodes(void) {
	char *keep = cursor_episode() != NULL ? g_strdup(cursor_episode()->url) : NULL;
	g_clear_pointer(&ui.episodes, g_ptr_array_unref);
	ui.episodes = showing_listening() ? store_episodes_started() :
		store_episodes(showing_new() ? NULL : ui.picked);
	ui.cursor = -1;
	for (guint i = 0; keep != NULL && i < ui.episodes->len; i++) {
		if (strcmp(((struct episode *)ui.episodes->pdata[i])->url, keep) == 0) {
			ui.cursor = (int)i;
		}
	}
	g_free(keep);
}

/* Everything read again from the store, and drawn. */
static void reload(void) {
	g_clear_pointer(&ui.podcasts, g_ptr_array_unref);
	ui.podcasts = store_podcasts();
	GPtrArray *started = store_episodes_started();
	ui.started_count = (int)started->len;
	g_ptr_array_unref(started);
	if (!showing_all() && podcast_of(ui.picked) == NULL) {
		g_free(ui.picked);
		ui.picked = g_strdup(NEW_EPISODES);
	}
	load_episodes();
	if (ui.playing != NULL) {
		struct episode *e = store_episode(ui.playing->url);
		if (e != NULL) {
			episode_free(ui.playing);
			ui.playing = e;
		}
	}
	if (ui.window == NULL) {
		return;
	}
	fit(ui.left, ui.ladj, n_left(), PODCAST_H);
	fit(ui.right, ui.radj, (int)ui.episodes->len, EPISODE_H);
	gtk_widget_queue_draw(ui.left);
	gtk_widget_queue_draw(ui.right);
	gtk_widget_queue_draw(ui.deck);
	gtk_widget_queue_draw(ui.info);
	app_menu_update(ui.menu);
}

static void pick_podcast(const char *url) {
	g_free(ui.picked);
	ui.picked = g_strdup(url);
	ui.cursor = -1;
	gtk_adjustment_set_value(ui.radj, 0);
	load_episodes();
	fit(ui.right, ui.radj, (int)ui.episodes->len, EPISODE_H);
	gtk_widget_queue_draw(ui.left);
	gtk_widget_queue_draw(ui.right);
	app_menu_update(ui.menu);
}

/* ---- Playing --------------------------------------------------------------------- */

/* Where it's got to, kept, and queued for the sync server. */
static void save_place(void) {
	if (ui.playing == NULL || player_state() == PLAYER_STOPPED) {
		return;
	}
	int pos = (int)player_position();
	if (pos == ui.queued) {
		return; /* paused there, and said so already */
	}
	int total = (int)player_length();
	store_set_position(ui.playing->url, pos, total);
	struct episode *e = store_episode(ui.playing->url);
	if (e != NULL) {
		store_queue_action(e, "play", ui.started, pos, total > 0 ? total :
			e->total);
		episode_free(e);
	}
	ui.queued = pos;
	ui.started = pos;
	ui.saved_at = g_get_monotonic_time();
	sync_later(SYNC_AFTER);
	if (ui.window != NULL) {
		load_episodes();
		gtk_widget_queue_draw(ui.right);
	}
}

static void play_episode(const struct episode *e) {
	save_place();
	struct episode *fresh = store_episode(e->url);
	if (fresh == NULL) {
		return;
	}
	episode_free(ui.playing);
	ui.playing = fresh;
	int at = fresh->played ? 0 : fresh->position;
	ui.started = at;
	ui.queued = -1;
	ui.saved_at = g_get_monotonic_time();
	if (fresh->played) {
		store_set_played(fresh->url, false);
	}
	store_set_fresh(fresh->url, false); /* started: not new any more */
	store_set("playing", fresh->url);
	const char *source = fresh->file != NULL &&
		g_file_test(fresh->file, G_FILE_TEST_EXISTS) ? fresh->file : fresh->url;
	player_open(source, at, false);
	reload();
	mpris_changed("Metadata", metadata());
}

static void toggle_play(void) {
	if (ui.playing == NULL) {
		struct episode *e = cursor_episode();
		if (e != NULL) {
			play_episode(e);
		}
		return;
	}
	if (player_state() == PLAYER_STOPPED) {
		play_episode(ui.playing);
	} else {
		bool pause = player_state() == PLAYER_PLAYING;
		if (pause) {
			save_place();
		} else {
			ui.queued = -1;
		}
		player_pause(pause);
	}
}

static void skip(double by) {
	if (player_state() == PLAYER_STOPPED) {
		return;
	}
	player_seek(player_position() + by);
}

static void set_speed(double s) {
	player_set_speed(s);
	char *kept = g_strdup_printf("%g", player_speed());
	store_set("speed", kept);
	g_free(kept);
	gtk_widget_queue_draw(ui.deck);
	app_menu_update(ui.menu);
}

static void next_speed(void) {
	double now = player_speed();
	for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
		if (speeds[i] > now + 0.01) {
			set_speed(speeds[i]);
			return;
		}
	}
	set_speed(speeds[0]);
}

static void player_changed(void *data) {
	if (ui.window == NULL) {
		return; /* the speed, restored before there's a window */
	}
	gtk_widget_queue_draw(ui.deck);
	gtk_widget_queue_draw(ui.right);
	app_menu_update(ui.menu);
	static const char *const states[] = { "Stopped", "Playing", "Paused" };
	mpris_changed("PlaybackStatus", g_variant_new_string(states[player_state()]));
}

static void player_ended(void *data) {
	if (ui.playing == NULL) {
		return;
	}
	int total = (int)player_length();
	store_set_played(ui.playing->url, true);
	struct episode *e = store_episode(ui.playing->url);
	if (e != NULL) {
		total = total > 0 ? total : e->total > 0 ? e->total : e->duration;
		store_queue_action(e, "play", ui.started, total, total);
		episode_free(e);
	}
	sync_later(SYNC_AFTER);
	reload();
}

/* Twice a second: the bar moves, and every so often the place is kept. */
static gboolean tick(gpointer data) {
	if (player_state() == PLAYER_PLAYING) {
		gtk_widget_queue_draw(ui.deck);
		if (g_get_monotonic_time() - ui.saved_at > SAVE_EVERY * G_USEC_PER_SEC) {
			int pos = (int)player_position();
			store_set_position(ui.playing->url, pos, (int)player_length());
			ui.saved_at = g_get_monotonic_time();
		}
	}
	return G_SOURCE_CONTINUE;
}

/* ---- Episodes: played, downloaded ------------------------------------------------ */

static void mark_played(struct episode *e, bool played) {
	store_set_played(e->url, played);
	struct episode *now = store_episode(e->url);
	if (now != NULL) {
		int len = now->total > 0 ? now->total : now->duration;
		if (played) {
			store_queue_action(now, "play", 0, len, len);
		} else {
			store_queue_action(now, "new", -1, -1, -1);
		}
		episode_free(now);
	}
	sync_later(SYNC_AFTER);
}

static char *slug(const char *s) {
	GString *out = g_string_new(NULL);
	for (const char *c = s != NULL ? s : "episode"; *c && out->len < 80; c++) {
		g_string_append_c(out, strchr("/\\:*?\"<>|", *c) != NULL ? '-' : *c);
	}
	return g_strstrip(g_string_free(out, FALSE));
}

static char *download_path(const struct episode *e) {
	struct podcast *p = podcast_of(e->podcast);
	char *dir_name = slug(p != NULL ? podcast_name(p) : "Podcast");
	char *name = slug(e->title);
	const char *ext = strrchr(e->url, '.');
	char *extension = NULL;
	if (ext != NULL && strlen(ext) <= 6 && strchr(ext, '/') == NULL) {
		extension = g_strndup(ext, strcspn(ext, "?#"));
	} else {
		extension = g_strdup(e->mime != NULL && strstr(e->mime, "mpeg") ? ".mp3" :
			e->mime != NULL && strstr(e->mime, "mp4") ? ".m4a" : ".audio");
	}
	char *file = g_strconcat(name, extension, NULL);
	char *path = g_build_filename(g_get_user_data_dir(), "gempod", "downloads",
		dir_name, file, NULL);
	g_free(file);
	g_free(extension);
	g_free(name);
	g_free(dir_name);
	return path;
}

struct download {
	char *url, *path, *part;
	GOutputStream *out;
};

static void download_done(struct download *d, const char *error) {
	if (error == NULL) {
		g_rename(d->part, d->path);
		store_set_file(d->url, d->path);
		struct episode *e = store_episode(d->url);
		if (e != NULL) {
			store_queue_action(e, "download", -1, -1, -1);
			episode_free(e);
		}
	} else {
		g_unlink(d->part);
		say(g_strdup_printf("Couldn't download it: %s", error));
	}
	g_clear_pointer(&ui.downloading, g_free);
	g_clear_object(&d->out);
	g_free(d->url);
	g_free(d->path);
	g_free(d->part);
	g_free(d);
	reload();
	start_download();
}

static void spliced(GObject *src, GAsyncResult *res, gpointer data) {
	GError *error = NULL;
	g_output_stream_splice_finish(G_OUTPUT_STREAM(src), res, &error);
	download_done(data, error != NULL ? error->message : NULL);
	g_clear_error(&error);
}

static void download_sent(GObject *src, GAsyncResult *res, gpointer data) {
	struct download *d = data;
	GError *error = NULL;
	GInputStream *in = soup_session_send_finish(SOUP_SESSION(src), res, &error);
	if (in == NULL) {
		download_done(d, error->message);
		g_error_free(error);
		return;
	}
	char *dir = g_path_get_dirname(d->path);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	GFile *f = g_file_new_for_path(d->part);
	d->out = G_OUTPUT_STREAM(g_file_replace(f, NULL, FALSE, G_FILE_CREATE_NONE,
		NULL, &error));
	g_object_unref(f);
	if (d->out == NULL) {
		g_object_unref(in);
		download_done(d, error->message);
		g_error_free(error);
		return;
	}
	g_output_stream_splice_async(d->out, in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
		G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, G_PRIORITY_LOW, NULL, spliced, d);
	g_object_unref(in);
}

static void start_download(void) {
	if (ui.downloading != NULL || g_queue_is_empty(&ui.to_download)) {
		return;
	}
	char *url = g_queue_pop_head(&ui.to_download);
	struct episode *e = store_episode(url);
	if (e == NULL) {
		g_free(url);
		start_download();
		return;
	}
	SoupMessage *msg = soup_message_new("GET", url);
	if (msg == NULL) {
		episode_free(e);
		g_free(url);
		start_download();
		return;
	}
	struct download *d = g_new0(struct download, 1);
	d->url = url;
	d->path = download_path(e);
	d->part = g_strconcat(d->path, ".part", NULL);
	ui.downloading = g_strdup(url);
	soup_session_send_async(ui.soup, msg, G_PRIORITY_LOW, NULL, download_sent, d);
	g_object_unref(msg);
	episode_free(e);
	gtk_widget_queue_draw(ui.info);
	gtk_widget_queue_draw(ui.right);
}

static void download(struct episode *e) {
	if (e->file == NULL && (ui.downloading == NULL ||
			strcmp(ui.downloading, e->url) != 0)) {
		g_queue_push_tail(&ui.to_download, g_strdup(e->url));
		start_download();
	}
}

static void delete_download(struct episode *e) {
	if (e->file == NULL) {
		return;
	}
	g_unlink(e->file);
	store_set_file(e->url, NULL);
	store_queue_action(e, "delete", -1, -1, -1);
	reload();
}

/* ---- Reading feeds ---------------------------------------------------------------- */

static void fetch_next(void);

static void read_feed(struct feed *f, const char *error, void *data) {
	char *url = data;
	ui.fetching--;
	ui.fetch_done++;
	struct podcast *had = store_podcast(url);
	if (had != NULL && f != NULL) {
		if (!f->unchanged) {
			struct podcast p = f->podcast;
			if (p.title == NULL) {
				p.title = had->title;
			}
			p.fetched = g_get_real_time() / G_USEC_PER_SEC;
			store_update_podcast(&p);
			bool first = had->fetched == 0;
			for (guint i = 0; i < f->episodes->len; i++) {
				struct episode *e = f->episodes->pdata[i];
				e->fresh = first && i == 0;
				if (store_merge_episode(e, first) && (!first || i == 0)) {
					ui.fetch_new++;
				}
			}
		} else {
			had->fetched = g_get_real_time() / G_USEC_PER_SEC;
			store_update_podcast(had);
		}
	} else if (had != NULL && error != NULL) {
		say(g_strdup_printf("%s: %s", podcast_name(had), error));
	}
	podcast_free(had);
	g_free(url);
	reload();
	fetch_next();
	if (ui.fetching == 0 && g_queue_is_empty(&ui.to_fetch)) {
		store_set_int("refreshed", g_get_real_time() / G_USEC_PER_SEC);
		if (ui.fetch_new > 0) {
			say(g_strdup_printf("%d new %s", ui.fetch_new,
				ui.fetch_new == 1 ? "episode" : "episodes"));
		}
		ui.fetch_total = ui.fetch_done = ui.fetch_new = 0;
	}
}

static void fetch_next(void) {
	while (ui.fetching < FETCHING && !g_queue_is_empty(&ui.to_fetch)) {
		char *url = g_queue_pop_head(&ui.to_fetch);
		struct podcast *p = store_podcast(url);
		ui.fetching++;
		feed_fetch(ui.soup, url, p != NULL ? p->etag : NULL,
			p != NULL ? p->modified : NULL, read_feed, url);
		podcast_free(p);
	}
	gtk_widget_queue_draw(ui.info);
}

/* One podcast's feed read again, or (url NULL) every one's. */
static void refresh(const char *url) {
	GPtrArray *all = store_podcasts();
	for (guint i = 0; i < all->len; i++) {
		struct podcast *p = all->pdata[i];
		if (url != NULL && strcmp(url, p->url) != 0) {
			continue;
		}
		if (g_queue_find_custom(&ui.to_fetch, p->url, (GCompareFunc)strcmp) == NULL) {
			g_queue_push_tail(&ui.to_fetch, g_strdup(p->url));
			ui.fetch_total++;
		}
	}
	g_ptr_array_unref(all);
	fetch_next();
}

static gboolean refresh_due(gpointer data) {
	refresh(NULL);
	return G_SOURCE_CONTINUE;
}

/* ---- Subscribing ------------------------------------------------------------------ */

static void subscribe(const char *given) {
	char *url = g_strstrip(g_strdup(given));
	if (url[0] == '\0') {
		g_free(url);
		return;
	}
	if (strstr(url, "://") == NULL) {
		char *with = g_strconcat("https://", url, NULL);
		g_free(url);
		url = with;
	}
	if (!store_has_podcast(url)) {
		store_add_podcast(url);
		store_queue_subscription(url, true);
		sync_later(SYNC_AFTER);
	}
	reload();
	pick_podcast(url);
	refresh(url);
	g_free(url);
}

static void unsubscribe(const char *url) {
	if (ui.playing != NULL && strcmp(ui.playing->podcast, url) == 0) {
		player_stop();
		g_clear_pointer(&ui.playing, episode_free);
	}
	store_remove_podcast(url);
	store_queue_subscription(url, false);
	sync_later(SYNC_AFTER);
	reload();
}

/* ---- Syncing ----------------------------------------------------------------------- */

static void synced(const struct sync_result *r, const char *error, void *data) {
	ui.syncing = false;
	g_free(ui.sync_status);
	if (error != NULL) {
		ui.sync_status = g_strdup_printf("not synced: %s", error);
	} else {
		GDateTime *now = g_date_time_new_now_local();
		char *t = g_date_time_format(now, "%H:%M");
		ui.sync_status = g_strdup_printf("synced at %s", t);
		g_free(t);
		g_date_time_unref(now);
		for (int i = 0; r->new_podcasts != NULL && r->new_podcasts[i] != NULL; i++) {
			refresh(r->new_podcasts[i]);
		}
	}
	reload();
	if (ui.sync_again) {
		ui.sync_again = false;
		sync_later(1);
	}
}

static void sync_now(void) {
	if (!account_set()) {
		return;
	}
	if (ui.syncing) {
		ui.sync_again = true;
		return;
	}
	save_place();
	ui.syncing = true;
	gtk_widget_queue_draw(ui.info);
	sync_run(ui.soup, &ui.account, synced, NULL);
}

static gboolean sync_fire(gpointer data) {
	ui.sync_soon = 0;
	sync_now();
	return G_SOURCE_REMOVE;
}

/* A sync in a little while, if syncing's automatic: changes made close
 * together go in one. */
static void sync_later(int seconds) {
	if (!ui.auto_sync || !account_set()) {
		return;
	}
	if (ui.sync_soon != 0) {
		g_source_remove(ui.sync_soon);
	}
	ui.sync_soon = g_timeout_add_seconds(seconds, sync_fire, NULL);
}

static gboolean sync_due(gpointer data) {
	if (ui.auto_sync) {
		sync_now();
	}
	return G_SOURCE_CONTINUE;
}

/* ---- Dialogs: a form of fields ------------------------------------------------------- */

#define FORM_W 460
#define FORM_FIELDS 4

struct form;
typedef void (*form_fn)(struct form *f, int button);

struct form {
	GtkWidget *box, *area;
	GtkWidget *entries[FORM_FIELDS];
	const char *labels[FORM_FIELDS];
	int n_fields;
	char *title;
	char *check_label;
	bool check;
	char **lines;          /* below the fields: what to know */
	char *status;          /* the answer to the last try */
	const char *buttons[3];
	int n_buttons, def;
	GArray *hits;
	form_fn done;
};

enum { FORM_CHECK = 100 };

static int form_label_w(struct form *f) {
	int w = 0;
	for (int i = 0; i < f->n_fields; i++) {
		w = MAX(w, (int)gem_measure(f->labels[i]));
	}
	return w + GEM_PAD;
}

static int form_lines(struct form *f) {
	int n = 0;
	for (int i = 0; f->lines != NULL && f->lines[i] != NULL; i++) {
		char **w = gem_wrap(f->lines[i], FORM_W - 4 * GEM_PAD);
		n += g_strv_length(w);
		g_strfreev(w);
	}
	if (f->status != NULL) {
		char **w = gem_wrap(f->status, FORM_W - 4 * GEM_PAD);
		n += g_strv_length(w);
		g_strfreev(w);
	}
	return n;
}

static int form_field_y(int i) {
	return 2 * GEM_PAD + GEM_ROW_H + GEM_PAD + i * (GEM_FIELD_H + 4);
}

static int form_height(struct form *f) {
	return form_field_y(f->n_fields) + (f->check_label != NULL ? GEM_ROW_H + 4 : 0)
		+ GEM_PAD + form_lines(f) * GEM_ROW_H + GEM_PAD + BUTTON_H + 2 * GEM_PAD;
}

static void form_paint(cairo_t *cr, int w, int h, void *data) {
	struct form *f = data;
	g_array_set_size(f->hits, 0);
	gem_dialog_frame(cr, w, h);
	gem_black(cr);
	gem_text(cr, f->title, (w - (int)gem_text_width(cr, f->title)) / 2,
		2 * GEM_PAD, GEM_ROW_H);
	for (int i = 0; i < f->n_fields; i++) {
		gem_text(cr, f->labels[i], 2 * GEM_PAD,
			form_field_y(i) + (GEM_FIELD_H - GEM_ROW_H) / 2, GEM_ROW_H);
	}
	int y = form_field_y(f->n_fields);
	if (f->check_label != NULL) {
		gem_checkbox(cr, f->hits, 2 * GEM_PAD + form_label_w(f), y + 2,
			f->check_label, f->check, FORM_CHECK);
		y += GEM_ROW_H + 4;
	}
	y += GEM_PAD;
	for (int i = 0; f->lines != NULL && f->lines[i] != NULL; i++) {
		char **wrapped = gem_wrap(f->lines[i], w - 4 * GEM_PAD);
		for (int k = 0; wrapped[k] != NULL; k++, y += GEM_ROW_H) {
			gem_text(cr, wrapped[k], 2 * GEM_PAD, y, GEM_ROW_H);
		}
		g_strfreev(wrapped);
	}
	if (f->status != NULL) {
		char **wrapped = gem_wrap(f->status, w - 4 * GEM_PAD);
		for (int k = 0; wrapped[k] != NULL; k++, y += GEM_ROW_H) {
			gem_text(cr, wrapped[k], 2 * GEM_PAD, y, GEM_ROW_H);
			gem_text(cr, wrapped[k], 2 * GEM_PAD + 1, y, GEM_ROW_H);
		}
		g_strfreev(wrapped);
	}
	int bw = 0;
	for (int i = 0; i < f->n_buttons; i++) {
		bw = MAX(bw, gem_button_width(f->buttons[i]));
	}
	int bx = w - 2 * GEM_PAD - f->n_buttons * (bw + GEM_PAD) + GEM_PAD;
	int by = h - 2 * GEM_PAD - BUTTON_H;
	for (int i = 0; i < f->n_buttons; i++) {
		gem_button(cr, f->hits, bx + i * (bw + GEM_PAD), by, bw, f->buttons[i], i,
			i == f->def);
	}
}

static void form_relayout(struct form *f) {
	gtk_widget_set_size_request(f->area, FORM_W, form_height(f));
	gtk_widget_queue_draw(f->area);
}

static void form_close(struct form *f) {
	gem_release(ui.host, f->box);
	g_array_unref(f->hits);
	g_free(f->title);
	g_free(f->check_label);
	g_free(f->status);
	g_strfreev(f->lines);
	g_free(f);
}

static const char *form_text(struct form *f, int i) {
	return gtk_editable_get_text(GTK_EDITABLE(f->entries[i]));
}

static void form_status(struct form *f, const char *status) {
	g_free(f->status);
	f->status = g_strdup(status);
	form_relayout(f);
}

static void form_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct form *f = data;
	const struct gem_hit *hit = gem_hit_at(f->hits, x, y);
	if (hit == NULL) {
		return;
	}
	if (hit->id == FORM_CHECK) {
		f->check = !f->check;
		gtk_widget_queue_draw(f->area);
		return;
	}
	f->done(f, hit->id);
}

static void form_activate(GtkEntry *e, gpointer data) {
	struct form *f = data;
	f->done(f, f->def);
}

static gboolean form_key(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	struct form *f = data;
	if (key == GDK_KEY_Escape) {
		f->done(f, 0); /* the first button's Cancel */
		return TRUE;
	}
	return FALSE;
}

/* fields: label, value, and whether it's a password, NULL-ended. buttons:
 * the first cancels, def is Return's. */
static struct form *form_new(const char *title, const char *const *fields,
		const char *check_label, bool check, char **lines,
		const char *const *buttons, int def, form_fn done) {
	struct form *f = g_new0(struct form, 1);
	f->title = g_strdup(title);
	f->check_label = g_strdup(check_label);
	f->check = check;
	f->lines = g_strdupv(lines);
	f->done = done;
	f->def = def;
	f->hits = gem_hits_new();
	for (int i = 0; buttons[i] != NULL && i < 3; i++) {
		f->buttons[f->n_buttons++] = buttons[i];
	}
	for (int i = 0; fields[3 * i] != NULL && i < FORM_FIELDS; i++) {
		f->labels[f->n_fields++] = fields[3 * i];
	}
	f->box = gtk_overlay_new();
	f->area = gem_pixel_area_new(FORM_W, form_height(f), form_paint, f);
	gtk_overlay_set_child(GTK_OVERLAY(f->box), f->area);
	int fx = 2 * GEM_PAD + form_label_w(f);
	for (int i = 0; i < f->n_fields; i++) {
		f->entries[i] = gem_field_new(fx, form_field_y(i), FORM_W - fx - 2 * GEM_PAD);
		gtk_editable_set_text(GTK_EDITABLE(f->entries[i]),
			fields[3 * i + 1] != NULL ? fields[3 * i + 1] : "");
		if (fields[3 * i + 2] != NULL) {
			gtk_entry_set_visibility(GTK_ENTRY(f->entries[i]), FALSE);
			gtk_entry_set_input_purpose(GTK_ENTRY(f->entries[i]),
				GTK_INPUT_PURPOSE_PASSWORD);
		}
		g_signal_connect(f->entries[i], "activate", G_CALLBACK(form_activate), f);
		gtk_overlay_add_overlay(GTK_OVERLAY(f->box), f->entries[i]);
	}
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(form_pressed), f);
	gtk_widget_add_controller(f->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(form_key), f);
	gtk_widget_add_controller(f->box, keys);
	gem_hold(ui.host, f->box);
	if (f->n_fields > 0) {
		gtk_widget_grab_focus(f->entries[0]);
	}
	return f;
}

/* ---- Sync Settings ------------------------------------------------------------------ */

static struct form *settings_form;

static void checked(const struct sync_result *r, const char *error, void *data) {
	struct form *f = settings_form;
	struct sync_account *a = data;
	if (f == NULL) {
		g_free(a->server);
		g_free(a->username);
		g_free(a->password);
		g_free(a->device);
		g_free(a->caption);
		g_free(a);
		return;
	}
	if (error != NULL) {
		form_status(f, error);
		g_free(a->server);
		g_free(a->username);
		g_free(a->password);
		g_free(a->device);
		g_free(a->caption);
		g_free(a);
		return;
	}
	/* Signed in. Another server or person starts from the beginning. */
	bool other = g_strcmp0(a->server, ui.account.server) != 0 ||
		g_strcmp0(a->username, ui.account.username) != 0;
	g_free(ui.account.server);
	g_free(ui.account.username);
	g_free(ui.account.password);
	g_free(ui.account.device);
	ui.account.server = a->server;
	ui.account.username = a->username;
	ui.account.password = a->password;
	ui.account.device = a->device;
	g_free(a->caption);
	g_free(a);
	ui.auto_sync = f->check;
	if (other) {
		store_set_int("subs_since", 0);
		store_set_int("episodes_since", 0);
		store_set_int("synced", 0);
	}
	bool in_file = false;
	save_settings(&in_file);
	settings_form = NULL;
	form_close(f);
	say(g_strdup(in_file ?
		"Signed in. (No keyring: the password's kept in ~/.config/gempod/settings.)" :
		"Signed in. The password's in your keyring."));
	sync_now();
}

static void settings_done(struct form *f, int button) {
	const char *label = f->buttons[button];
	if (strcmp(label, "Cancel") == 0) {
		settings_form = NULL;
		form_close(f);
		return;
	}
	if (strcmp(label, "Sign Out") == 0) {
		if (ui.account.server != NULL && ui.account.username != NULL) {
			secret_password_clear_sync(schema(), NULL, NULL, "server",
				ui.account.server, "username", ui.account.username, NULL);
		}
		g_clear_pointer(&ui.account.server, g_free);
		g_clear_pointer(&ui.account.username, g_free);
		g_clear_pointer(&ui.account.password, g_free);
		store_set_int("subs_since", 0);
		store_set_int("episodes_since", 0);
		store_set_int("synced", 0);
		g_clear_pointer(&ui.sync_status, g_free);
		save_settings(NULL);
		settings_form = NULL;
		form_close(f);
		say(g_strdup("Signed out: GemPod won't sync."));
		reload();
		return;
	}
	/* Sign In: checked before it's kept. */
	struct sync_account *a = g_new0(struct sync_account, 1);
	a->server = sync_server_url(form_text(f, 0));
	a->username = g_strstrip(g_strdup(form_text(f, 1)));
	a->password = g_strdup(form_text(f, 2));
	a->device = g_strstrip(g_strdup(form_text(f, 3)));
	if (a->device[0] == '\0') {
		g_free(a->device);
		a->device = default_device();
	}
	a->caption = g_strdup(ui.account.caption);
	if (a->server[0] == '\0' || a->username[0] == '\0') {
		form_status(f, "The server and your username, please.");
		g_free(a->server);
		g_free(a->username);
		g_free(a->password);
		g_free(a->device);
		g_free(a->caption);
		g_free(a);
		return;
	}
	ui.auto_sync = f->check;
	form_status(f, "Signing in...");
	sync_check(ui.soup, a, checked, a);
}

static void show_settings(void) {
	const char *fields[] = {
		"Server:", ui.account.server, NULL,
		"Username:", ui.account.username, NULL,
		"Password:", ui.account.password, "secret",
		"Device:", ui.account.device, NULL,
		NULL,
	};
	char *lines[] = {
		(char *)"A gpodder sync server (gpodder.net, or your own, like barking "
			"or opodsync) keeps your podcasts, and where you got to in each, "
			"the same here and on your phone.",
		NULL,
	};
	const char *signed_in[] = { "Cancel", "Sign Out", "Sign In", NULL };
	const char *not_in[] = { "Cancel", "Sign In", NULL };
	settings_form = form_new("Sync Settings", fields, "Sync automatically",
		ui.auto_sync, lines, account_set() ? signed_in : not_in,
		account_set() ? 2 : 1, settings_done);
	if (ui.sync_status != NULL) {
		char *s = g_strdup_printf("Last: %s.", ui.sync_status);
		form_status(settings_form, s);
		g_free(s);
	}
}

/* ---- Add Podcast, Remove Podcast ------------------------------------------------------ */

static void add_done(struct form *f, int button) {
	if (button == 1) {
		char *url = g_strdup(form_text(f, 0));
		form_close(f);
		subscribe(url);
		g_free(url);
	} else {
		form_close(f);
	}
}

static void add_podcast(void) {
	const char *fields[] = { "Feed:", "", NULL, NULL };
	char *lines[] = { (char *)"The podcast's feed address (RSS or Atom), as its "
		"web page or the app it's in gives it.", NULL };
	const char *buttons[] = { "Cancel", "Add", NULL };
	form_new("Add Podcast", fields, NULL, false, lines, buttons, 1, add_done);
}

static void remove_answered(int button, void *data) {
	char *url = data;
	if (button == 1) {
		unsubscribe(url);
	}
	g_free(url);
}

static void remove_podcast(void) {
	if (showing_all()) {
		return;
	}
	struct podcast *p = podcast_of(ui.picked);
	if (p == NULL) {
		return;
	}
	char *text = g_strdup_printf("Unsubscribe from %s?\nIts downloads are "
		"deleted too.", podcast_name(p));
	static const char *const buttons[] = { "Cancel", "Unsubscribe", NULL };
	gem_alert(ui.host, GEM_ALERT_QUESTION, text, buttons, 0, 0, remove_answered,
		g_strdup(p->url));
	g_free(text);
}

/* ---- OPML ----------------------------------------------------------------------------- */

static void import_outlines(xmlNode *n, int *added) {
	for (; n != NULL; n = n->next) {
		if (n->type == XML_ELEMENT_NODE &&
				strcmp((const char *)n->name, "outline") == 0) {
			xmlChar *url = xmlGetProp(n, (const xmlChar *)"xmlUrl");
			if (url != NULL && !store_has_podcast((const char *)url)) {
				store_add_podcast((const char *)url);
				store_queue_subscription((const char *)url, true);
				(*added)++;
			}
			xmlFree(url);
		}
		import_outlines(n->children, added);
	}
}

static void opml_chosen(const char *path, void *data) {
	if (path == NULL) {
		return;
	}
	xmlDocPtr doc = xmlReadFile(path, NULL, XML_PARSE_NOERROR |
		XML_PARSE_NOWARNING | XML_PARSE_NONET | XML_PARSE_RECOVER);
	if (doc == NULL) {
		alert(GEM_ALERT_STOP, "That isn't an OPML file.");
		return;
	}
	int added = 0;
	import_outlines(xmlDocGetRootElement(doc), &added);
	xmlFreeDoc(doc);
	say(g_strdup_printf("%d %s added", added, added == 1 ? "podcast" : "podcasts"));
	reload();
	refresh(NULL);
	sync_later(SYNC_AFTER);
}

static char *xml_escape(const char *s) {
	return g_markup_escape_text(s != NULL ? s : "", -1);
}

static void opml_save(const char *path, void *data) {
	if (path == NULL) {
		return;
	}
	GString *s = g_string_new("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<opml version=\"2.0\">\n  <head><title>GemPod subscriptions</title></head>\n"
		"  <body>\n");
	for (guint i = 0; i < ui.podcasts->len; i++) {
		struct podcast *p = ui.podcasts->pdata[i];
		char *title = xml_escape(podcast_name(p)), *url = xml_escape(p->url);
		g_string_append_printf(s, "    <outline type=\"rss\" text=\"%s\" "
			"xmlUrl=\"%s\"/>\n", title, url);
		g_free(title);
		g_free(url);
	}
	g_string_append(s, "  </body>\n</opml>\n");
	char *full = g_str_has_suffix(path, ".opml") ? g_strdup(path) :
		g_strconcat(path, ".opml", NULL);
	GError *e = NULL;
	if (!g_file_set_contents(full, s->str, -1, &e)) {
		alert(GEM_ALERT_STOP, e->message);
		g_error_free(e);
	} else {
		say(g_strdup_printf("Saved %s", full));
	}
	g_free(full);
	g_string_free(s, TRUE);
}

/* ---- Show Notes ------------------------------------------------------------------------ */

static GtkWidget *notes_box;

static void notes_close(void) {
	if (notes_box != NULL) {
		gem_release(ui.host, notes_box);
		notes_box = NULL;
	}
}

static void notes_paint(cairo_t *cr, int w, int h, void *data) {
	const char *title = data;
	gem_dialog_frame(cr, w, h);
	gem_black(cr);
	gem_text_clipped(cr, title, 2 * GEM_PAD, GEM_PAD + 4, w - 4 * GEM_PAD,
		GEM_ROW_H);
	gem_text_clipped(cr, title, 2 * GEM_PAD + 1, GEM_PAD + 4, w - 4 * GEM_PAD - 1,
		GEM_ROW_H);
	GArray *hits = g_object_get_data(G_OBJECT(notes_box), "hits");
	g_array_set_size(hits, 0);
	int bw = gem_button_width("OK");
	gem_button(cr, hits, w - 2 * GEM_PAD - bw, h - 2 * GEM_PAD - BUTTON_H, bw, "OK",
		1, true);
}

static void notes_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	GArray *hits = g_object_get_data(G_OBJECT(notes_box), "hits");
	if (gem_hit_at(hits, x, y) != NULL) {
		notes_close();
	}
}

static gboolean notes_key(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	if (key == GDK_KEY_Escape || key == GDK_KEY_Return) {
		notes_close();
		return TRUE;
	}
	return FALSE;
}

static void show_notes(struct episode *e) {
	if (e == NULL || notes_box != NULL) {
		return;
	}
	int w = 520, h = 400;
	notes_box = gtk_overlay_new();
	g_object_set_data_full(G_OBJECT(notes_box), "hits", gem_hits_new(),
		(GDestroyNotify)g_array_unref);
	char *title = g_strdup(e->title != NULL ? e->title : e->url);
	GtkWidget *area = gem_pixel_area_new(w, h, notes_paint, title);
	g_object_set_data_full(G_OBJECT(area), "title", title, g_free);
	gtk_overlay_set_child(GTK_OVERLAY(notes_box), area);
	GtkWidget *view = gtk_text_view_new();
	gtk_widget_add_css_class(view, "gem-text");
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	GString *text = g_string_new(NULL);
	char *date = when(e->published);
	struct podcast *p = podcast_of(e->podcast);
	g_string_append_printf(text, "%s%s%s\n\n", p != NULL ? podcast_name(p) : "",
		date[0] != '\0' ? ", " : "", date);
	g_free(date);
	g_string_append(text, e->notes != NULL ? e->notes : "No notes.");
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
		text->str, -1);
	g_string_free(text, TRUE);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
		GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(row), scroller);
	gtk_box_append(GTK_BOX(row), gem_scrollbar_new(GTK_ORIENTATION_VERTICAL,
		gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller))));
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	gtk_widget_set_valign(row, GTK_ALIGN_START);
	gtk_widget_set_margin_start(row, 2 * GEM_PAD);
	gtk_widget_set_margin_top(row, GEM_PAD + 4 + GEM_ROW_H + GEM_PAD);
	gtk_widget_set_size_request(row, w - 4 * GEM_PAD,
		h - (GEM_PAD + 4 + GEM_ROW_H + GEM_PAD) - BUTTON_H - 4 * GEM_PAD);
	gtk_overlay_add_overlay(GTK_OVERLAY(notes_box), row);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(notes_pressed), NULL);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(notes_key), NULL);
	gtk_widget_add_controller(notes_box, keys);
	gem_hold(ui.host, notes_box);
	gtk_widget_grab_focus(view);
}

/* ---- Menus -------------------------------------------------------------------------- */

enum {
	ACT_ADD = 1, ACT_IMPORT, ACT_EXPORT, ACT_REFRESH_ALL, ACT_SYNC, ACT_SETTINGS,
	ACT_QUIT, ACT_REFRESH, ACT_ALL_PLAYED, ACT_REMOVE, ACT_PLAY, ACT_NOTES,
	ACT_PLAYED, ACT_UNPLAYED, ACT_DOWNLOAD, ACT_DELETE_DOWNLOAD, ACT_PLAY_PAUSE,
	ACT_BACK, ACT_ON,
	ACT_SPEED = 100, /* speeds[], in order */
};

static void build_menus(struct app_menu *m, void *data) {
	struct episode *e = cursor_episode();
	uint32_t ep = e != NULL ? 0 : APP_MENU_DISABLED;
	uint32_t pod = showing_all() ? APP_MENU_DISABLED : 0;
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_ADD, "Add Podcast...", "^N", 0);
	app_menu_add_item(m, ACT_IMPORT, "Import OPML...", NULL, 0);
	app_menu_add_item(m, ACT_EXPORT, "Export OPML...", NULL,
		ui.podcasts->len > 0 ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_REFRESH_ALL, "Refresh All", "^R",
		ui.podcasts->len > 0 ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_SYNC, "Sync Now", "^S",
		account_set() && !ui.syncing ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_SETTINGS, "Sync Settings...", NULL, 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_QUIT, "Quit", "^Q", 0);
	app_menu_add_menu(m, "Podcast");
	app_menu_add_item(m, ACT_REFRESH, "Refresh", NULL, pod);
	app_menu_add_item(m, ACT_ALL_PLAYED, "Mark All Played", NULL,
		ui.episodes->len > 0 ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_REMOVE, "Unsubscribe...", NULL, pod);
	app_menu_add_menu(m, "Episode");
	app_menu_add_item(m, ACT_PLAY, "Play", "Return", ep);
	app_menu_add_item(m, ACT_NOTES, "Show Notes...", "^I", ep);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_PLAYED, "Mark as Played", "^M",
		e != NULL && !e->played ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_UNPLAYED, "Mark as Unplayed", "^U",
		e != NULL && (e->played || e->position > 0) ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_DOWNLOAD, "Download", "^D",
		e != NULL && e->file == NULL ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_DELETE_DOWNLOAD, "Delete Download", NULL,
		e != NULL && e->file != NULL ? 0 : APP_MENU_DISABLED);
	app_menu_add_menu(m, "Controls");
	bool any = ui.playing != NULL || e != NULL;
	app_menu_add_item(m, ACT_PLAY_PAUSE, player_state() == PLAYER_PLAYING ?
		"Pause" : "Play", "Space", any ? 0 : APP_MENU_DISABLED);
	uint32_t on = player_state() != PLAYER_STOPPED ? 0 : APP_MENU_DISABLED;
	app_menu_add_item(m, ACT_BACK, "Back 15 Seconds", "Left", on);
	app_menu_add_item(m, ACT_ON, "On 30 Seconds", "Right", on);
	app_menu_add_separator(m);
	app_menu_add_submenu(m, "Speed", 0);
	for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
		char *label = g_strdup_printf("%gx", speeds[i]);
		app_menu_add_item(m, ACT_SPEED + i, label, NULL,
			fabs(player_speed() - speeds[i]) < 0.01 ? APP_MENU_CHECKED : 0);
		g_free(label);
	}
	app_menu_end_submenu(m);
}

static void run(int id) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	struct episode *e = cursor_episode();
	if (id >= ACT_SPEED && id < ACT_SPEED + (int)G_N_ELEMENTS(speeds)) {
		set_speed(speeds[id - ACT_SPEED]);
		return;
	}
	switch (id) {
	case ACT_ADD: add_podcast(); break;
	case ACT_IMPORT:
		gem_file_select(ui.host, "Import OPML", false, NULL, NULL,
			"*.opml,*.xml,*.OPML", opml_chosen, NULL);
		break;
	case ACT_EXPORT:
		gem_file_select(ui.host, "Export OPML", true, NULL, "Podcasts.opml",
			"*.opml", opml_save, NULL);
		break;
	case ACT_REFRESH_ALL: refresh(NULL); break;
	case ACT_SYNC: sync_now(); break;
	case ACT_SETTINGS: show_settings(); break;
	case ACT_QUIT: g_application_quit(G_APPLICATION(ui.app)); break;
	case ACT_REFRESH: if (!showing_all()) refresh(ui.picked); break;
	case ACT_ALL_PLAYED:
		for (guint i = 0; i < ui.episodes->len; i++) {
			struct episode *x = ui.episodes->pdata[i];
			if (!x->played) {
				mark_played(x, true);
			}
		}
		reload();
		break;
	case ACT_REMOVE: remove_podcast(); break;
	case ACT_PLAY: if (e != NULL) play_episode(e); break;
	case ACT_NOTES: show_notes(e); break;
	case ACT_PLAYED: if (e != NULL) { mark_played(e, true); reload(); } break;
	case ACT_UNPLAYED: if (e != NULL) { mark_played(e, false); reload(); } break;
	case ACT_DOWNLOAD: if (e != NULL) download(e); break;
	case ACT_DELETE_DOWNLOAD: if (e != NULL) delete_download(e); break;
	case ACT_PLAY_PAUSE: toggle_play(); break;
	case ACT_BACK: skip(-BACK); break;
	case ACT_ON: skip(ON); break;
	}
}

static void menu_activate(uint32_t id, void *data) {
	run((int)id);
}

static void popup_chosen(int id, void *data) {
	if (id > 0) {
		run(id);
	}
}

/* ---- The pointer and keys -------------------------------------------------------------- */

static void left_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	ui.right_focus = false;
	gtk_widget_grab_focus(ui.left);
	int i = (int)((y + lscroll() - 2) / PODCAST_H);
	if (i < 0 || i >= n_left()) {
		return;
	}
	pick_podcast(left_row(i));
	if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) ==
			GDK_BUTTON_SECONDARY && i >= TOP_ROWS) {
		gem_popup_clear(ui.lpopup);
		gem_popup_add(ui.lpopup, ACT_REFRESH, "Refresh", NULL, 0);
		gem_popup_add(ui.lpopup, ACT_ALL_PLAYED, "Mark All Played", NULL, 0);
		gem_popup_add_separator(ui.lpopup);
		gem_popup_add(ui.lpopup, ACT_REMOVE, "Unsubscribe...", NULL, 0);
		gem_popup_show(ui.lpopup, x, y);
	}
}

static void right_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	ui.right_focus = true;
	gtk_widget_grab_focus(ui.right);
	int i = (int)((y + rscroll() - 2) / EPISODE_H);
	if (i < 0 || i >= (int)ui.episodes->len) {
		ui.cursor = -1;
		gtk_widget_queue_draw(ui.right);
		app_menu_update(ui.menu);
		return;
	}
	ui.cursor = i;
	gtk_widget_queue_draw(ui.right);
	app_menu_update(ui.menu);
	struct episode *e = ui.episodes->pdata[i];
	if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) ==
			GDK_BUTTON_SECONDARY) {
		gem_popup_clear(ui.popup);
		gem_popup_add(ui.popup, ACT_PLAY, "Play", NULL, 0);
		gem_popup_add(ui.popup, ACT_NOTES, "Show Notes...", NULL, 0);
		gem_popup_add_separator(ui.popup);
		gem_popup_add(ui.popup, e->played ? ACT_UNPLAYED : ACT_PLAYED,
			e->played ? "Mark as Unplayed" : "Mark as Played", NULL, 0);
		gem_popup_add(ui.popup, e->file != NULL ? ACT_DELETE_DOWNLOAD : ACT_DOWNLOAD,
			e->file != NULL ? "Delete Download" : "Download", NULL, 0);
		gem_popup_show(ui.popup, x, y);
		return;
	}
	if (n == 2) {
		play_episode(e);
	}
}

static void deck_seek_x(double x) {
	const struct gem_hit *bar = NULL;
	for (guint i = 0; i < ui.deck_hits->len; i++) {
		const struct gem_hit *h = &g_array_index(ui.deck_hits, struct gem_hit, i);
		if (h->id == HIT_BAR) {
			bar = h;
		}
	}
	double len = player_length();
	if (bar == NULL || len <= 0) {
		return;
	}
	ui.scrub_to = CLAMP((x - bar->x) / MAX(bar->w, 1), 0, 1) * len;
}

static void deck_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(ui.host)) {
		return;
	}
	const struct gem_hit *hit = gem_hit_at(ui.deck_hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_BACK: skip(-BACK); break;
	case HIT_ON: skip(ON); break;
	case HIT_PLAY: toggle_play(); break;
	case HIT_SPEED: next_speed(); break;
	}
}

static void scrub_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	const struct gem_hit *hit = gem_hit_at(ui.deck_hits, x, y);
	if (hit == NULL || hit->id != HIT_BAR || player_state() == PLAYER_STOPPED ||
			player_length() <= 0) {
		gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	ui.scrubbing = true;
	deck_seek_x(x);
	gtk_widget_queue_draw(ui.deck);
}

static void scrub_update(GtkGestureDrag *g, double dx, double dy, gpointer d) {
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	deck_seek_x(sx + dx);
	gtk_widget_queue_draw(ui.deck);
}

static void scrub_end(GtkGestureDrag *g, double dx, double dy, gpointer d) {
	if (ui.scrubbing) {
		ui.scrubbing = false;
		player_seek(ui.scrub_to);
		save_place();
	}
}

static gboolean scrolled(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	GtkAdjustment *adj = data;
	gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + dy * 60);
	return TRUE;
}

static void move_cursor(int to) {
	if (ui.right_focus) {
		if (ui.episodes->len == 0) {
			return;
		}
		ui.cursor = CLAMP(to, 0, (int)ui.episodes->len - 1);
		int h = gtk_widget_get_height(ui.right);
		double y = 2 + ui.cursor * EPISODE_H;
		if (y < rscroll()) {
			gtk_adjustment_set_value(ui.radj, y - 2);
		} else if (y + EPISODE_H > rscroll() + h) {
			gtk_adjustment_set_value(ui.radj, y + EPISODE_H - h + 2);
		}
		gtk_widget_queue_draw(ui.right);
		app_menu_update(ui.menu);
		return;
	}
	pick_podcast(left_row(CLAMP(to, 0, n_left() - 1)));
}

static int left_index(void) {
	for (int i = 0; i < n_left(); i++) {
		if (g_strcmp0(left_row(i), ui.picked) == 0) {
			return i;
		}
	}
	return 0;
}

static gboolean key_pressed(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	if (gem_alert_up(ui.host)) {
		return FALSE;
	}
	if (mods & GDK_CONTROL_MASK) {
		switch (gdk_keyval_to_lower(key)) {
		case GDK_KEY_n: run(ACT_ADD); return TRUE;
		case GDK_KEY_r: run(ACT_REFRESH_ALL); return TRUE;
		case GDK_KEY_s: run(ACT_SYNC); return TRUE;
		case GDK_KEY_q: case GDK_KEY_w: run(ACT_QUIT); return TRUE;
		case GDK_KEY_i: run(ACT_NOTES); return TRUE;
		case GDK_KEY_m: run(ACT_PLAYED); return TRUE;
		case GDK_KEY_u: run(ACT_UNPLAYED); return TRUE;
		case GDK_KEY_d: run(ACT_DOWNLOAD); return TRUE;
		}
		return FALSE;
	}
	int cur = ui.right_focus ? ui.cursor : left_index();
	switch (key) {
	case GDK_KEY_space: run(ACT_PLAY_PAUSE); return TRUE;
	case GDK_KEY_Return: case GDK_KEY_KP_Enter:
		if (ui.right_focus) {
			run(ACT_PLAY);
		} else {
			ui.right_focus = true;
			move_cursor(0);
		}
		return TRUE;
	case GDK_KEY_Left: run(ACT_BACK); return TRUE;
	case GDK_KEY_Right: run(ACT_ON); return TRUE;
	case GDK_KEY_Up: move_cursor(cur < 0 ? 0 : cur - 1); return TRUE;
	case GDK_KEY_Down: move_cursor(cur < 0 ? 0 : cur + 1); return TRUE;
	case GDK_KEY_Tab:
		ui.right_focus = !ui.right_focus;
		if (ui.right_focus && ui.cursor < 0) {
			move_cursor(0);
		}
		return TRUE;
	}
	return FALSE;
}

/* ---- MPRIS ------------------------------------------------------------------------- */

static const char mpris_xml[] =
	"<node>"
	" <interface name='org.mpris.MediaPlayer2'>"
	"  <method name='Raise'/><method name='Quit'/>"
	"  <property name='CanQuit' type='b' access='read'/>"
	"  <property name='CanRaise' type='b' access='read'/>"
	"  <property name='HasTrackList' type='b' access='read'/>"
	"  <property name='Identity' type='s' access='read'/>"
	"  <property name='DesktopEntry' type='s' access='read'/>"
	"  <property name='SupportedUriSchemes' type='as' access='read'/>"
	"  <property name='SupportedMimeTypes' type='as' access='read'/>"
	" </interface>"
	" <interface name='org.mpris.MediaPlayer2.Player'>"
	"  <method name='Next'/><method name='Previous'/><method name='Pause'/>"
	"  <method name='PlayPause'/><method name='Stop'/><method name='Play'/>"
	"  <method name='Seek'><arg type='x' name='Offset' direction='in'/></method>"
	"  <method name='SetPosition'><arg type='o' name='TrackId' direction='in'/>"
	"   <arg type='x' name='Position' direction='in'/></method>"
	"  <method name='OpenUri'><arg type='s' name='Uri' direction='in'/></method>"
	"  <signal name='Seeked'><arg type='x' name='Position'/></signal>"
	"  <property name='PlaybackStatus' type='s' access='read'/>"
	"  <property name='Rate' type='d' access='readwrite'/>"
	"  <property name='Metadata' type='a{sv}' access='read'/>"
	"  <property name='Volume' type='d' access='readwrite'/>"
	"  <property name='Position' type='x' access='read'/>"
	"  <property name='MinimumRate' type='d' access='read'/>"
	"  <property name='MaximumRate' type='d' access='read'/>"
	"  <property name='CanGoNext' type='b' access='read'/>"
	"  <property name='CanGoPrevious' type='b' access='read'/>"
	"  <property name='CanPlay' type='b' access='read'/>"
	"  <property name='CanPause' type='b' access='read'/>"
	"  <property name='CanSeek' type='b' access='read'/>"
	"  <property name='CanControl' type='b' access='read'/>"
	" </interface>"
	"</node>";

static GVariant *metadata(void) {
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	struct episode *e = ui.playing;
	g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(
		e != NULL ? "/org/gemwm/Pod/episode" :
		"/org/mpris/MediaPlayer2/TrackList/NoTrack"));
	if (e != NULL) {
		struct podcast *p = podcast_of(e->podcast);
		g_variant_builder_add(&b, "{sv}", "xesam:title",
			g_variant_new_string(e->title != NULL ? e->title : e->url));
		if (p != NULL) {
			g_variant_builder_add(&b, "{sv}", "xesam:album",
				g_variant_new_string(podcast_name(p)));
			const char *who[] = { p->author != NULL ? p->author : podcast_name(p),
				NULL };
			g_variant_builder_add(&b, "{sv}", "xesam:artist",
				g_variant_new_strv(who, -1));
			const char *art = e->image != NULL ? e->image : p->image;
			if (art != NULL) {
				g_variant_builder_add(&b, "{sv}", "mpris:artUrl",
					g_variant_new_string(art));
			}
		}
		int len = e->total > 0 ? e->total : e->duration;
		if (len > 0) {
			g_variant_builder_add(&b, "{sv}", "mpris:length",
				g_variant_new_int64((gint64)len * G_USEC_PER_SEC));
		}
		g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(e->url));
	}
	return g_variant_builder_end(&b);
}

static void mpris_changed(const char *prop, GVariant *v) {
	if (ui.bus == NULL) {
		g_variant_unref(g_variant_ref_sink(v));
		return;
	}
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", prop, v);
	g_dbus_connection_emit_signal(ui.bus, NULL, MPRIS_PATH,
		"org.freedesktop.DBus.Properties", "PropertiesChanged",
		g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &b, NULL),
		NULL);
}

static void mpris_call(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *call, gpointer data) {
	if (strcmp(method, "Raise") == 0) {
		gtk_window_present(GTK_WINDOW(ui.window));
	} else if (strcmp(method, "Quit") == 0) {
		g_application_quit(G_APPLICATION(ui.app));
	} else if (strcmp(method, "Next") == 0) {
		skip(ON);
	} else if (strcmp(method, "Previous") == 0) {
		skip(-BACK);
	} else if (strcmp(method, "Pause") == 0) {
		if (player_state() == PLAYER_PLAYING) {
			toggle_play();
		}
	} else if (strcmp(method, "Play") == 0) {
		if (player_state() != PLAYER_PLAYING) {
			toggle_play();
		}
	} else if (strcmp(method, "PlayPause") == 0) {
		toggle_play();
	} else if (strcmp(method, "Stop") == 0) {
		save_place();
		player_stop();
	} else if (strcmp(method, "Seek") == 0) {
		gint64 off;
		g_variant_get(params, "(x)", &off);
		skip((double)off / G_USEC_PER_SEC);
	} else if (strcmp(method, "SetPosition") == 0) {
		const char *tid;
		gint64 pos;
		g_variant_get(params, "(&ox)", &tid, &pos);
		player_seek((double)pos / G_USEC_PER_SEC);
	}
	g_dbus_method_invocation_return_value(call, NULL);
}

static GVariant *mpris_get(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *prop, GError **error,
		gpointer data) {
	if (strcmp(prop, "CanQuit") == 0 || strcmp(prop, "CanRaise") == 0 ||
			strcmp(prop, "CanSeek") == 0 || strcmp(prop, "CanControl") == 0 ||
			strcmp(prop, "CanPlay") == 0 || strcmp(prop, "CanPause") == 0 ||
			strcmp(prop, "CanGoNext") == 0 || strcmp(prop, "CanGoPrevious") == 0) {
		return g_variant_new_boolean(TRUE);
	}
	if (strcmp(prop, "HasTrackList") == 0) {
		return g_variant_new_boolean(FALSE);
	}
	if (strcmp(prop, "Identity") == 0) {
		return g_variant_new_string("GemPod");
	}
	if (strcmp(prop, "DesktopEntry") == 0) {
		return g_variant_new_string("org.gemwm.Pod");
	}
	if (strcmp(prop, "SupportedUriSchemes") == 0 ||
			strcmp(prop, "SupportedMimeTypes") == 0) {
		return g_variant_new_strv(NULL, 0);
	}
	if (strcmp(prop, "PlaybackStatus") == 0) {
		static const char *const states[] = { "Stopped", "Playing", "Paused" };
		return g_variant_new_string(states[player_state()]);
	}
	if (strcmp(prop, "Rate") == 0) {
		return g_variant_new_double(player_speed());
	}
	if (strcmp(prop, "MinimumRate") == 0) {
		return g_variant_new_double(0.5);
	}
	if (strcmp(prop, "MaximumRate") == 0) {
		return g_variant_new_double(3.0);
	}
	if (strcmp(prop, "Volume") == 0) {
		return g_variant_new_double(1.0);
	}
	if (strcmp(prop, "Metadata") == 0) {
		return metadata();
	}
	if (strcmp(prop, "Position") == 0) {
		return g_variant_new_int64((gint64)(player_position() * G_USEC_PER_SEC));
	}
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "%s", prop);
	return NULL;
}

static gboolean mpris_set(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *prop, GVariant *value,
		GError **error, gpointer data) {
	if (strcmp(prop, "Rate") == 0) {
		set_speed(g_variant_get_double(value));
	}
	return TRUE;
}

static void mpris_start(void) {
	ui.bus = g_application_get_dbus_connection(G_APPLICATION(ui.app));
	if (ui.bus == NULL) {
		return;
	}
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(mpris_xml, NULL);
	static const GDBusInterfaceVTable vtable = { mpris_call, mpris_get, mpris_set,
		{ 0 } };
	for (int i = 0; info->interfaces[i] != NULL; i++) {
		g_dbus_connection_register_object(ui.bus, MPRIS_PATH, info->interfaces[i],
			&vtable, NULL, NULL, NULL);
	}
	g_dbus_node_info_unref(info);
	g_bus_own_name_on_connection(ui.bus, MPRIS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		NULL, NULL, NULL, NULL);
}

/* ---- The window ---------------------------------------------------------------------- */

static void resized(GtkDrawingArea *a, int w, int h, gpointer data) {
	fit(ui.left, ui.ladj, n_left(), PODCAST_H);
	fit(ui.right, ui.radj, (int)ui.episodes->len, EPISODE_H);
}

static GtkWidget *list(GtkWidget **area, GtkAdjustment **adj,
		GtkDrawingAreaDrawFunc draw, GCallback pressed, int width) {
	*area = gtk_drawing_area_new();
	gtk_widget_set_focusable(*area, TRUE);
	gtk_widget_set_vexpand(*area, TRUE);
	gtk_widget_set_hexpand(*area, width == 0);
	if (width > 0) {
		gtk_widget_set_size_request(*area, width, -1);
	}
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(*area), draw, NULL, NULL);
	g_signal_connect(*area, "resize", G_CALLBACK(resized), NULL);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	g_signal_connect(click, "pressed", pressed, NULL);
	gtk_widget_add_controller(*area, GTK_EVENT_CONTROLLER(click));
	*adj = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
	g_signal_connect_swapped(*adj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), *area);
	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(wheel, "scroll", G_CALLBACK(scrolled), *adj);
	gtk_widget_add_controller(*area, wheel);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(row, width == 0);
	gtk_box_append(GTK_BOX(row), *area);
	gtk_box_append(GTK_BOX(row), gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, *adj));
	return row;
}

static gboolean close_request(GtkWindow *w, gpointer data) {
	g_application_quit(G_APPLICATION(ui.app));
	return TRUE;
}

static void build_window(void) {
	ui.window = gtk_application_window_new(ui.app);
	gtk_widget_add_css_class(ui.window, "gem");
	gtk_window_set_title(GTK_WINDOW(ui.window), "GemPod");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 820, 560);
	ui.deck_hits = gem_hits_new();

	GtkWidget *lists = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(lists, TRUE);
	gtk_box_append(GTK_BOX(lists), list(&ui.left, &ui.ladj, draw_left,
		G_CALLBACK(left_pressed), LEFT_W));
	gtk_box_append(GTK_BOX(lists), list(&ui.right, &ui.radj, draw_right,
		G_CALLBACK(right_pressed), 0));

	ui.deck = gtk_drawing_area_new();
	gtk_widget_set_size_request(ui.deck, 520, DECK_H);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.deck), draw_deck, NULL,
		NULL);
	GtkGesture *dclick = gtk_gesture_click_new();
	g_signal_connect(dclick, "pressed", G_CALLBACK(deck_pressed), NULL);
	gtk_widget_add_controller(ui.deck, GTK_EVENT_CONTROLLER(dclick));
	GtkGesture *scrub = gtk_gesture_drag_new();
	g_signal_connect(scrub, "drag-begin", G_CALLBACK(scrub_begin), NULL);
	g_signal_connect(scrub, "drag-update", G_CALLBACK(scrub_update), NULL);
	g_signal_connect(scrub, "drag-end", G_CALLBACK(scrub_end), NULL);
	gtk_widget_add_controller(ui.deck, GTK_EVENT_CONTROLLER(scrub));

	ui.info = gem_pixel_area_new(0, INFO_H, paint_info, NULL);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(outer), lists);
	gtk_box_append(GTK_BOX(outer), ui.deck);
	gtk_box_append(GTK_BOX(outer), ui.info);
	ui.host = GTK_OVERLAY(gtk_overlay_new());
	gtk_overlay_set_child(ui.host, outer);
	gtk_window_set_child(GTK_WINDOW(ui.window), GTK_WIDGET(ui.host));

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	g_signal_connect(ui.window, "close-request", G_CALLBACK(close_request), NULL);

	ui.popup = gem_popup_new(ui.right, popup_chosen, NULL);
	ui.lpopup = gem_popup_new(ui.left, popup_chosen, NULL);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);
	gtk_window_present(GTK_WINDOW(ui.window));
	gtk_widget_grab_focus(ui.left);
	reload();
}

/* ---- The application -------------------------------------------------------------------- */

static void activate(GtkApplication *app, gpointer data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	build_window();
	if (ui.podcasts->len == 0 && !account_set()) {
		say(g_strdup("File > Add Podcast, or File > Sync Settings to bring "
			"yours from a sync server."));
	}
	/* A sync first (it may bring podcasts), then the feeds read if it's
	 * been a while. */
	if (account_set() && ui.auto_sync) {
		sync_now();
	}
	if (g_get_real_time() / G_USEC_PER_SEC - store_get_int("refreshed") >
			REFRESH_EVERY / 2) {
		refresh(NULL);
	}
}

/* A feed address given (gempod URL): subscribed. */
static void open_files(GApplication *app, GFile **files, int n,
		const char *hint, gpointer data) {
	activate(GTK_APPLICATION(app), NULL);
	for (int i = 0; i < n; i++) {
		char *uri = g_file_get_uri(files[i]);
		if (g_str_has_prefix(uri, "feed:")) {
			char *rest = g_strdup(uri + 5);
			g_free(uri);
			uri = g_str_has_prefix(rest, "//") ? g_strconcat("https:", rest, NULL) :
				g_strdup(rest);
			g_free(rest);
		}
		if (g_str_has_prefix(uri, "http")) {
			subscribe(uri);
		}
		g_free(uri);
	}
}

static void startup(GApplication *app, gpointer data) {
	gem_ui_load_css();
	char *error = NULL;
	if (!store_open(&error)) {
		g_printerr("gempod: can't open its database: %s\n", error);
		g_free(error);
	}
	ui.soup = soup_session_new_with_options("user-agent", "GemPod/1.0",
		"timeout", 60, NULL);
	ui.art = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, art_free);
	ui.picked = g_strdup(NEW_EPISODES);
	ui.cursor = -1;
	ui.queued = -1;
	g_queue_init(&ui.to_fetch);
	g_queue_init(&ui.to_download);
	load_settings();
	player_init(player_changed, player_ended, NULL);
	char *speed = store_get("speed");
	if (speed != NULL) {
		player_set_speed(g_ascii_strtod(speed, NULL));
		g_free(speed);
	}
	/* What was playing last time, ready where it was left. */
	char *last = store_get("playing");
	if (last != NULL) {
		ui.playing = store_episode(last);
		g_free(last);
	}
	ui.podcasts = store_podcasts();
	/* What you're in the middle of first, if anything. */
	GPtrArray *started = store_episodes_started();
	if (started->len > 0) {
		g_free(ui.picked);
		ui.picked = g_strdup(LISTENING);
	}
	g_ptr_array_unref(started);
	ui.episodes = store_episodes(NULL);
	mpris_start();
	g_timeout_add(500, tick, NULL);
	g_timeout_add_seconds(SYNC_EVERY, sync_due, NULL);
	g_timeout_add_seconds(REFRESH_EVERY, refresh_due, NULL);
}

static void shutdown(GApplication *app, gpointer data) {
	save_place();
	if (ui.playing != NULL) {
		store_set("playing", ui.playing->url);
	}
	player_finish();
	store_close();
}

int main(int argc, char *argv[]) {
	g_set_application_name("GemPod");
	ui.app = gtk_application_new("org.gemwm.Pod", G_APPLICATION_HANDLES_OPEN);
	g_application_set_option_context_parameter_string(G_APPLICATION(ui.app),
		"[FEED...]");
	g_signal_connect(ui.app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(ui.app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(ui.app, "open", G_CALLBACK(open_files), NULL);
	g_signal_connect(ui.app, "shutdown", G_CALLBACK(shutdown), NULL);
	int status = g_application_run(G_APPLICATION(ui.app), argc, argv);
	g_object_unref(ui.app);
	return status;
}
