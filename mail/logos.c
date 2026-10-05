#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <string.h>
#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>
#endif
#include "logos.h"

#define RETRY (7 * 24 * 3600)   /* seconds before a domain with none is asked again */
#define MAX_BYTES (512 * 1024)  /* bigger isn't a logo */

/* A domain's logo: being looked for, there, or not. */
struct logo {
	cairo_surface_t *surface;  /* NULL: none, or not yet */
	bool done;
};

static struct {
	GHashTable *logos;         /* domain -> struct logo */
	GQueue waiting;            /* domains to look up, one at a time */
	bool busy;
	SoupSession *soup;
	char *dir;
	void (*ready)(void *data);
	void *data;
} lg;

/* ---- Domains ---------------------------------------------------------------- */

static char *domain_of(const char *address) {
	const char *at = address != NULL ? strrchr(address, '@') : NULL;
	return at != NULL && at[1] != '\0' ? g_ascii_strdown(at + 1, -1) : NULL;
}

/* The domain an organisation registered: news.example.co.uk ->
 * example.co.uk. A guess, from the shape of the name. */
static char *organisation(const char *domain) {
	char **labels = g_strsplit(domain, ".", -1);
	int n = g_strv_length(labels);
	static const char *const second[] = { "co", "com", "org", "net", "ac",
		"gov", "edu" };
	int keep = 2;
	if (n >= 3 && strlen(labels[n - 1]) == 2) {
		for (size_t i = 0; i < G_N_ELEMENTS(second); i++) {
			if (strcmp(labels[n - 2], second[i]) == 0) {
				keep = 3;
			}
		}
	}
	char *org = n > keep ? g_strjoinv(".", labels + n - keep) : g_strdup(domain);
	g_strfreev(labels);
	return org;
}

static char *png_path(const char *domain) {
	char *name = g_strconcat(domain, ".png", NULL);
	char *path = g_build_filename(lg.dir, name, NULL);
	g_free(name);
	return path;
}

static char *none_path(const char *domain) {
	char *name = g_strconcat(domain, ".none", NULL);
	char *path = g_build_filename(lg.dir, name, NULL);
	g_free(name);
	return path;
}

/* ---- Pictures ---------------------------------------------------------------- */

/* Drawn LOGO_SIZE square, centred, its aspect kept. */
static cairo_surface_t *fit(cairo_surface_t *src, double w, double h) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		LOGO_SIZE, LOGO_SIZE);
	cairo_t *cr = cairo_create(out);
	double scale = MIN(LOGO_SIZE / w, LOGO_SIZE / h);
	cairo_translate(cr, (LOGO_SIZE - w * scale) / 2, (LOGO_SIZE - h * scale) / 2);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

#ifdef HAVE_RSVG
static cairo_surface_t *from_svg(GBytes *bytes) {
	gsize len;
	const guint8 *data = g_bytes_get_data(bytes, &len);
	RsvgHandle *h = rsvg_handle_new_from_data(data, len, NULL);
	if (h == NULL) {
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		LOGO_SIZE, LOGO_SIZE);
	cairo_t *cr = cairo_create(out);
	RsvgRectangle box = { 0, 0, LOGO_SIZE, LOGO_SIZE };
	bool ok = rsvg_handle_render_document(h, cr, &box, NULL);
	cairo_destroy(cr);
	g_object_unref(h);
	if (!ok) {
		cairo_surface_destroy(out);
		return NULL;
	}
	return out;
}
#endif

/* PNG, JPEG, ICO...: as GTK reads them. */
static cairo_surface_t *from_image(GBytes *bytes) {
	GdkTexture *t = gdk_texture_new_from_bytes(bytes, NULL);
	if (t == NULL) {
		return NULL;
	}
	int w = gdk_texture_get_width(t), h = gdk_texture_get_height(t);
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_surface_flush(img);
	gdk_texture_download(t, cairo_image_surface_get_data(img),
		cairo_image_surface_get_stride(img));
	cairo_surface_mark_dirty(img);
	g_object_unref(t);
	cairo_surface_t *out = w >= 8 && h >= 8 ? fit(img, w, h) : NULL;
	cairo_surface_destroy(img);
	return out;
}

static cairo_surface_t *decode(GBytes *bytes) {
	gsize len;
	const char *data = g_bytes_get_data(bytes, &len);
	if (len == 0 || len > MAX_BYTES) {
		return NULL;
	}
	char *head = g_strndup(data, MIN(len, 512));
	bool svg = strstr(head, "<svg") != NULL || strstr(head, "<?xml") != NULL;
	g_free(head);
#ifdef HAVE_RSVG
	if (svg) {
		return from_svg(bytes);
	}
#else
	if (svg) {
		return NULL;
	}
#endif
	return from_image(bytes);
}

/* ---- Looking ----------------------------------------------------------------- */

/* One domain's search: where it's got to. */
struct search {
	char *domain, *org;
	GPtrArray *urls;           /* still to try */
	guint next;
	SoupMessage *msg;
	bool bimi_org_tried;
};

static void next_domain(void);
static void try_next_url(struct search *s);

static void finish(struct search *s, cairo_surface_t *surface) {
	struct logo *l = g_hash_table_lookup(lg.logos, s->domain);
	l->surface = surface;
	l->done = true;
	if (surface != NULL) {
		char *path = png_path(s->domain);
		cairo_surface_write_to_png(surface, path);
		g_free(path);
	} else {
		char *path = none_path(s->domain);
		g_file_set_contents(path, "", 0, NULL);
		g_free(path);
	}
	g_free(s->domain);
	g_free(s->org);
	g_ptr_array_unref(s->urls);
	g_free(s);
	lg.busy = false;
	if (lg.ready != NULL) {
		lg.ready(lg.data);
	}
	next_domain();
}

static void fetched(GObject *src, GAsyncResult *res, gpointer data) {
	struct search *s = data;
	GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res,
		NULL);
	int status = soup_message_get_status(s->msg);
	g_clear_object(&s->msg);
	cairo_surface_t *surface = bytes != NULL && status >= 200 && status < 300 ?
		decode(bytes) : NULL;
	if (bytes != NULL) {
		g_bytes_unref(bytes);
	}
	if (surface != NULL) {
		finish(s, surface);
	} else {
		try_next_url(s);
	}
}

static void try_next_url(struct search *s) {
	while (s->next < s->urls->len) {
		const char *url = s->urls->pdata[s->next++];
		s->msg = soup_message_new(SOUP_METHOD_GET, url);
		if (s->msg != NULL) {
			soup_session_send_and_read_async(lg.soup, s->msg, G_PRIORITY_LOW,
				NULL, fetched, s);
			return;
		}
	}
	finish(s, NULL);
}

/* The website's icons, after BIMI. */
static void add_icon_urls(struct search *s) {
	const char *hosts[] = { s->org, NULL };
	char *www = g_strconcat("www.", s->org, NULL);
	hosts[1] = www;
	static const char *const files[] = { "apple-touch-icon.png", "favicon.ico" };
	for (size_t f = 0; f < G_N_ELEMENTS(files); f++) {
		for (size_t h = 0; h < 2; h++) {
			g_ptr_array_add(s->urls,
				g_strdup_printf("https://%s/%s", hosts[h], files[f]));
		}
	}
	g_free(www);
}

static void bimi_found(GObject *src, GAsyncResult *res, gpointer data);

static void ask_bimi(struct search *s, const char *domain) {
	char *name = g_strconcat("default._bimi.", domain, NULL);
	g_resolver_lookup_records_async(g_resolver_get_default(), name,
		G_RESOLVER_RECORD_TXT, NULL, bimi_found, s);
	g_free(name);
}

/* "v=BIMI1; l=https://.../logo.svg; a=..." -> the l= URL. */
static char *bimi_url(GList *records) {
	char *url = NULL;
	for (GList *r = records; r != NULL && url == NULL; r = r->next) {
		GVariantIter *it;
		const char *part;
		GString *txt = g_string_new(NULL);
		g_variant_get(r->data, "(as)", &it);
		while (g_variant_iter_next(it, "&s", &part)) {
			g_string_append(txt, part);
		}
		g_variant_iter_free(it);
		if (g_str_has_prefix(txt->str, "v=BIMI1")) {
			char **tags = g_strsplit(txt->str, ";", -1);
			for (int i = 0; tags[i] != NULL && url == NULL; i++) {
				char *t = g_strstrip(tags[i]);
				if (g_str_has_prefix(t, "l=https://")) {
					url = g_strdup(t + 2);
				}
			}
			g_strfreev(tags);
		}
		g_string_free(txt, TRUE);
	}
	return url;
}

static void bimi_found(GObject *src, GAsyncResult *res, gpointer data) {
	struct search *s = data;
	GList *records = g_resolver_lookup_records_finish(G_RESOLVER(src), res, NULL);
	char *url = bimi_url(records);
	g_list_free_full(records, (GDestroyNotify)g_variant_unref);
	if (url == NULL && !s->bimi_org_tried && strcmp(s->org, s->domain) != 0) {
		/* news.example.org's own, else example.org's. */
		s->bimi_org_tried = true;
		ask_bimi(s, s->org);
		return;
	}
	if (url != NULL) {
		g_ptr_array_insert(s->urls, 0, url);
	}
	add_icon_urls(s);
	try_next_url(s);
}

static void next_domain(void) {
	if (lg.busy) {
		return;
	}
	char *domain = g_queue_pop_head(&lg.waiting);
	if (domain == NULL) {
		return;
	}
	lg.busy = true;
	struct search *s = g_new0(struct search, 1);
	s->domain = domain;
	s->org = organisation(domain);
	s->urls = g_ptr_array_new_with_free_func(g_free);
	ask_bimi(s, domain);
}

/* ---- The cache ---------------------------------------------------------------- */

void logos_init(void (*ready)(void *data), void *data) {
	lg.ready = ready;
	lg.data = data;
	lg.logos = g_hash_table_new(g_str_hash, g_str_equal);
	g_queue_init(&lg.waiting);
	lg.dir = g_build_filename(g_get_user_cache_dir(), "gemmail", "logos", NULL);
	g_mkdir_with_parents(lg.dir, 0700);
	lg.soup = soup_session_new_with_options("user-agent", "GemMail",
		"timeout", 15, NULL);
}

cairo_surface_t *logo_for(const char *address) {
	char *domain = domain_of(address);
	if (domain == NULL || lg.logos == NULL) {
		g_free(domain);
		return NULL;
	}
	struct logo *l = g_hash_table_lookup(lg.logos, domain);
	if (l != NULL) {
		g_free(domain);
		return l->surface;
	}
	l = g_new0(struct logo, 1);
	g_hash_table_insert(lg.logos, g_strdup(domain), l);
	/* On disk from before: a logo, or "none" (asked again after a while). */
	char *path = png_path(domain);
	if (g_file_test(path, G_FILE_TEST_EXISTS)) {
		cairo_surface_t *s = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
			l->surface = s;
			l->done = true;
		} else {
			cairo_surface_destroy(s);
		}
	}
	g_free(path);
	char *none = none_path(domain);
	GStatBuf st;
	if (!l->done && g_stat(none, &st) == 0 &&
			g_get_real_time() / G_USEC_PER_SEC - st.st_mtime < RETRY) {
		l->done = true;
	}
	g_free(none);
	if (!l->done) {
		g_queue_push_tail(&lg.waiting, domain);
		next_domain();
	} else {
		g_free(domain);
	}
	return l->surface;
}
