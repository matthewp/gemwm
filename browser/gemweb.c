/*
 * GemWeb: a web browser whose chrome is drawn the way GemWM draws GEM
 * windows. WebKitGTK renders the pages; everything around them (tabs,
 * buttons, the info line) is our own cairo drawing, rendered at 1x and
 * pixel-doubled like GemWM's frames. The only stock widget is the URL
 * field, styled flat and black-bordered.
 *
 * Layout, top to bottom (GemWM draws the title bar around all of it):
 *
 *   tabs       | [x] Example Domain :::::: | [x] Other page |  +  |
 *   toolbar    | < | > | R | H | https://example.com           |
 *   info line  | Loading... 45%                                  |
 *   page       (a WebKitWebView per tab, in a GtkStack)
 *
 * Under GemWM, its File, View and Go menus are in the menu bar
 * (lib/app-menu.c).
 */
#include <cairo.h>
#include <gdk/wayland/gdkwayland.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <webkit/webkit.h>
#include "adblock.h"
#include "app-menu.h"
#include "gem-print.h"
#include "gemwm-scroll-v1-client-protocol.h"
#include "cookies.h"
#include "history.h"
#include "passwords.h"

#define TAB_H 20       /* 19px of tabs plus a 1px line */
#define TOOL_H 21      /* the toolbar's buttons; the box adds a 1px line */
#define INFO_H 19      /* 18px of info line plus a 1px line */
#define GADGET 19      /* a GEM gadget box, as in GemWM */
#define TAB_MIN_W 90
#define TAB_MAX_W 220
#define PLUS_W 24      /* the "+" new-tab box */
#define FONT_SIZE font_size /* GEMWM_FONT_SIZE, else 14 */
#define BUTTONS 4      /* back, forward, reload, home */
#define DEFAULT_HOME "https://www.google.com/"
#define DEFAULT_SEARCH "https://duckduckgo.com/?q=%s"
#define SUGGEST_ROWS 8 /* the address field's list of pages from history */
#define ROW_H 19
#define BUTTON_H 22    /* a dialog's buttons */
#define FIELD_H 24     /* and its password field */
#define PASSWORD_WORLD "gemweb" /* the JavaScript world passwords are filled in */
#define PAD 8

static const char *font_family;
static int font_size;

struct browser;

enum dialog { DIALOG_NONE, DIALOG_CLEAR_HISTORY, DIALOG_UNLOCK, DIALOG_SCRIPT };
enum field { FIELD_NONE, FIELD_PASSWORD, FIELD_TEXT };

struct tab {
	struct browser *browser;
	WebKitWebView *view;
	WebKitUserContentManager *content;
	/* The page's scroll state, as its script last reported it (CSS px). */
	int scroll_x, scroll_y, view_w, view_h, doc_w, doc_h;
	/* The page has video or sound (noticed only if we can't play it), or
	 * it's a new window's first page: both show what to install. */
	bool media;
	int loads;
	/* History: the address last recorded, whether that made a new entry,
	 * and whether the page loading was typed in (or chosen from the list). */
	char *recorded;
	bool recorded_new;
	bool typed;
	guint record_timer;
	char *failed; /* an address that didn't load, shown as an error page */
	bool login;   /* the page has a login field */
	bool reading; /* in Reader View */
	bool reader_loading; /* the load starting is the reader's page */
	WebKitUserContentFilter *ads; /* the ad filter it has on, or NULL */
};

struct browser {
	GtkWidget *window;
	GtkWidget *tabbar, *toolbar, *buttons, *entry, *info, *stack;
	GPtrArray *tabs; /* struct tab * */
	int active;
	char *hover_link;      /* link under the pointer, for the info line */
	char *download_status; /* last download message */
	char *notice;          /* a passing one: passwords, Reader View */
	bool entry_edited;     /* the user typed in the address field */
	bool entry_setting;    /* we are changing it, not the user */
	struct gemwm_scroll_v1 *scroll; /* GemWM draws our scroll bars */
	struct app_menu *menu;          /* and shows our menus */

	/* Completing addresses: what the user typed (without the inline
	 * completion), that completion and where it goes, and the list of
	 * pages under the field, which may have a row selected. */
	GtkWidget *box, *list;
	char *typed;
	char *completion, *completion_uri;
	bool inserted; /* the last edit added text, so completing is welcome */
	guint complete_idle;
	GPtrArray *suggestions;
	int selected;

	/* A dialog over the window: Clear History, unlocking passwords, or a
	 * page's alert(), confirm() or prompt(). Its lines, its buttons (the
	 * second the default; no first for just OK), the one that does it,
	 * and maybe a field. */
	GtkWidget *dialog_box, *dialog_area, *dialog_field;
	enum dialog dialog;
	GPtrArray *dialog_lines;
	const char *dialog_labels[2];
	int dialog_act;
	enum field dialog_field_kind;
	char *dialog_error;
	int dialog_field_y;
	int dialog_buttons[2][4]; /* x, y, w, h */
	WebKitScriptDialog *script; /* the page's, while it's up */

	/* Passwords: the key gadget, the list of logins when a site has
	 * several, the host they're for, and what's happening. */
	GtkWidget *key, *logins_list;
	GPtrArray *logins;
	int login_selected;
	char *login_host;
	GCancellable *password_cancel;
	bool looking_up, unlocking;

	/* The find bar, along the bottom, and what WebKit found. */
	GtkWidget *findbar, *find_entry, *find_info;
	guint find_matches;
	bool find_failed;

	/* A page (a video) is fullscreen: the chrome is hidden. */
	bool fullscreen;
	bool find_was_open;

	/* A web app's window (gemweb --app): just its page, named after it. */
	bool app;
	char *app_uri, *app_name;

	/* The right-click menu: its items, the one under the pointer, what
	 * was clicked on, and where the pointer is (in the overlay). */
	GtkWidget *overlay, *ctx;
	GArray *ctx_items; /* struct ctx_item */
	int ctx_hover;
	char *ctx_link, *ctx_image, *ctx_media;
	double pointer_x, pointer_y;
};

/* Shared by every window: cookies and logins, the GEM scroll-bar style,
 * and settings. */
static struct {
	GtkApplication *app;
	WebKitNetworkSession *session;
	WebKitUserStyleSheet *style;
	WebKitUserScript *scroll_script;
	WebKitSettings *settings;
	/* Under GemWM, the window frame's GEM scroll bars scroll the page. */
	struct gemwm_scroll_manager_v1 *scroll_manager;
	/* What's missing to play video and sound, or NULL if nothing. */
	char *media_missing;
	WebKitUserScript *media_script;
	char *start_page;
	/* Passwords, if a command is set: its session key once unlocked, kept
	 * (in memory only) until GemWeb quits or Lock Passwords. */
	bool passwords;
	char *password_session;
	WebKitUserScript *login_script;
	char *readability; /* Readability.js, and the call that runs it */
	/* Ad blocking: WebKit's compiled filter, where it's kept, and whether
	 * the lists are being fetched and compiled afresh. */
	WebKitUserContentFilterStore *ads_store;
	WebKitUserContentFilter *ads_filter;
	char *ads_dir;
	bool ads_updating;
} shared;

static struct tab *tab_new(struct browser *b, WebKitWebView *related,
	const char *uri, bool select);
static void tab_close(struct browser *b, int index);
static void tab_select(struct browser *b, int index);
static void load_home(struct tab *t);
static void sync_key(struct browser *b);
static void on_login_message(WebKitUserContentManager *content,
	JSCValue *value, struct tab *t);
static bool find_open(struct browser *b);
static void find_search(struct browser *b);
static void find_connect(struct tab *t);
static gboolean on_enter_fullscreen(WebKitWebView *view, struct tab *t);
static gboolean on_leave_fullscreen(WebKitWebView *view, struct tab *t);
static void ads_apply(struct tab *t);
static void sync_info(struct browser *b);
static gboolean on_script_dialog(WebKitWebView *view, WebKitScriptDialog *d,
	struct tab *t);
static char *site_of(const char *uri);
static gboolean on_context_menu(WebKitWebView *view, WebKitContextMenu *menu,
	WebKitHitTestResult *hit, struct tab *t);
static bool app_owns(struct browser *b, const char *uri);
static void open_in_browser(const char *uri);
static struct browser *app_window(const char *uri, const char *name);
static GtkWidget *pixel_area(struct browser *b, int height,
	GtkDrawingAreaDrawFunc draw, GCallback pressed);

/* GEM-style scroll bars for web pages: a dithered track, a white slider
 * and boxed arrows; and GEM's black selection, which also marks what
 * Find found. User-level, so sites that style their own win. */
static const char page_css[] =
	"::selection { background: #000; color: #fff; }"
	"::-webkit-scrollbar { width: 19px; height: 19px; background: #fff; }"
	"::-webkit-scrollbar-track {"
	"  background: repeating-conic-gradient(#000 0% 25%, #fff 0% 50%) 0 0 / 2px 2px; }"
	"::-webkit-scrollbar-track:vertical { border-left: 1px solid #000; }"
	"::-webkit-scrollbar-track:horizontal { border-top: 1px solid #000; }"
	"::-webkit-scrollbar-thumb { background: #fff; border: 1px solid #000; }"
	"::-webkit-scrollbar-corner { background: #fff; border: 1px solid #000; }"
	"::-webkit-scrollbar-button:single-button { display: block; width: 19px;"
	"  height: 19px; box-sizing: border-box; border: 1px solid #000;"
	"  background: #fff no-repeat center; }"
	"::-webkit-scrollbar-button:single-button:vertical:decrement { background-image:"
	"  url(\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='11' height='7'><path d='M5.5 0L11 7H0z'/></svg>\"); }"
	"::-webkit-scrollbar-button:single-button:vertical:increment { background-image:"
	"  url(\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='11' height='7'><path d='M0 0H11L5.5 7z'/></svg>\"); }"
	"::-webkit-scrollbar-button:single-button:horizontal:decrement { background-image:"
	"  url(\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='7' height='11'><path d='M0 5.5L7 0V11z'/></svg>\"); }"
	"::-webkit-scrollbar-button:single-button:horizontal:increment { background-image:"
	"  url(\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='7' height='11'><path d='M0 0L7 5.5L0 11z'/></svg>\"); }";

/* Under GemWM the window's own scroll bars are GEM ones in the frame, so
 * the page's main scroll bar goes; scrolling boxes inside pages keep the
 * GEM-styled ones above. */
static const char frame_scroll_css[] = "html { scrollbar-width: none; }";

/* Tells GemWeb how the page is scrolled, whenever that changes: position,
 * viewport and document size, in CSS pixels. */
static const char scroll_script[] =
	"(function () {"
	"  var queued = false;"
	"  function send() {"
	"    queued = false;"
	"    var d = document.scrollingElement || document.documentElement;"
	"    if (!d) return;"
	"    window.webkit.messageHandlers.gemwmScroll.postMessage([window.scrollX,"
	"      window.scrollY, window.innerWidth, window.innerHeight,"
	"      d.scrollWidth, d.scrollHeight]);"
	"  }"
	"  function queue() {"
	"    if (!queued) { queued = true; requestAnimationFrame(send); }"
	"  }"
	"  addEventListener('scroll', queue, { passive: true });"
	"  addEventListener('resize', queue);"
	"  addEventListener('load', queue);"
	"  if (window.ResizeObserver && document.documentElement)"
	"    new ResizeObserver(queue).observe(document.documentElement);"
	"  queue();"
	"})();";

/* A new tab: a GEM dialog box on a dithered desk. */
static const char start_page[] =
	"<!doctype html><html><head><title>New Tab</title><style>"
	"html,body{margin:0;height:100%}"
	"body{background:repeating-conic-gradient(#000 0% 25%,#fff 0% 50%) 0 0/2px 2px;"
	"display:flex;align-items:center;justify-content:center;"
	"font:14px monospace}"
	".box{background:#fff;border:1px solid #000;box-shadow:2px 2px 0 #000;"
	"padding:16px 28px;text-align:center}"
	"h1{font-size:14px;font-weight:normal;margin:0 0 12px}"
	"p{margin:12px 0 0}"
	"</style></head><body><div class=box><h1>GemWeb</h1>"
	"Type an address or a search above.";
static const char start_page_end[] = "</div></body></html>";

/* Tells GemWeb when a page has video or sound, so it can say why they won't
 * play. Only added when GStreamer plugins are missing. Pages like YouTube
 * make their <video> late, so it looks again after loading. */
static const char media_script[] =
	"(function () {"
	"  var sent = false;"
	"  function send() {"
	"    if (sent) return;"
	"    sent = true;"
	"    window.webkit.messageHandlers.gemwebMedia.postMessage(0);"
	"  }"
	"  function look() { if (document.querySelector('video,audio')) send(); }"
	"  function media(e) { if (e.target instanceof HTMLMediaElement) send(); }"
	"  addEventListener('loadstart', media, true);"
	"  addEventListener('error', media, true);"
	"  addEventListener('load', function () { look(); setTimeout(look, 3000); });"
	"  look();"
	"})();";

/* WebKit plays video and sound through GStreamer, but its plugins are
 * packages of their own that distributions don't always install with it;
 * without them video pages stall. Each package is missing if any element it
 * needs is (or, for any_of, if none is). */
static char *find_missing_media(void) {
	static const struct {
		const char *package;
		bool any_of;
		const char *elements[5];
	} needs[] = {
		{ "gst-plugins-base", false, { "opusdec", "vorbisdec" } },
		{ "gst-plugins-good", false,
			{ "autoaudiosink", "qtdemux", "matroskademux", "vp9dec" } },
		/* H.264: FFmpeg's, or the graphics card's. */
		{ "gst-libav", true, { "avdec_h264", "vah264dec", "openh264dec" } },
	};
	GError *error = NULL;
	if (!gst_init_check(NULL, NULL, &error)) {
		g_warning("GStreamer didn't start, so can't check its plugins: %s",
			error->message);
		g_error_free(error);
		return NULL;
	}
	GString *missing = g_string_new(NULL);
	for (guint i = 0; i < G_N_ELEMENTS(needs); i++) {
		int found = 0, wanted = 0;
		for (int j = 0; needs[i].elements[j] != NULL; j++) {
			GstElementFactory *f = gst_element_factory_find(needs[i].elements[j]);
			found += f != NULL;
			wanted++;
			if (f != NULL) {
				gst_object_unref(f);
			}
		}
		if (needs[i].any_of ? found == 0 : found < wanted) {
			g_string_append_printf(missing, "%s%s", missing->len ? ", " : "",
				needs[i].package);
		}
	}
	return g_string_free(missing, missing->len == 0);
}

/* ---- Drawing, GEM style ------------------------------------------------- */

/* Draws into a 1x image and pixel-doubles it onto the widget, so the
 * chrome has the same chunky pixels as GemWM's frames on HiDPI screens. */
static void paint_pixelated(cairo_t *cr, int w, int h,
		void (*paint)(struct browser *, cairo_t *, int, int),
		struct browser *b) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *c = cairo_create(img);
	cairo_set_antialias(c, CAIRO_ANTIALIAS_NONE);
	cairo_select_font_face(c, font_family, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(c, FONT_SIZE);
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	cairo_set_font_options(c, opts);
	cairo_font_options_destroy(opts);
	cairo_set_source_rgb(c, 1, 1, 1);
	cairo_paint(c);
	paint(b, c, w, h);
	cairo_destroy(c);

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(img);
}

static void black(cairo_t *cr) { cairo_set_source_rgb(cr, 0, 0, 0); }
static void white(cairo_t *cr) { cairo_set_source_rgb(cr, 1, 1, 1); }

static void fill(cairo_t *cr, double x, double y, double w, double h) {
	if (w > 0 && h > 0) {
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
	}
}

/* A repeating 1-bit pattern, e.g. {"#.", ".#"} for a 50% dither. With
 * `mask`, '.' is transparent (for greying things out). */
static cairo_pattern_t *dither(const char *rows[], int w, int h, bool mask) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s) / 4;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			px[y * stride + x] = rows[y][x] == '#' ? 0xff000000 :
				mask ? 0x00000000 : 0xffffffff;
		}
	}
	cairo_surface_mark_dirty(s);
	cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
	cairo_surface_destroy(s);
	cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
	return p;
}

/* Text vertically centred in a row starting at y. */
static void text(cairo_t *cr, const char *s, double x, double y, double row_h) {
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	cairo_move_to(cr, x,
		y + (int)((row_h - (fe.ascent + fe.descent)) / 2 + fe.ascent));
	cairo_show_text(cr, s);
}

static double text_width(cairo_t *cr, const char *s) {
	cairo_text_extents_t te;
	cairo_text_extents(cr, s, &te);
	return te.x_advance;
}

/* GemWM's closer glyph: a box with an X in it. */
static void draw_closer(cairo_t *cr, double x, double y) {
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 4.5, y + 4.5, GADGET - 9, GADGET - 9);
	cairo_move_to(cr, x + 4.5, y + 4.5);
	cairo_line_to(cr, x + GADGET - 4.5, y + GADGET - 4.5);
	cairo_move_to(cr, x + GADGET - 4.5, y + 4.5);
	cairo_line_to(cr, x + 4.5, y + GADGET - 4.5);
	cairo_stroke(cr);
}

static const char *tab_title(struct tab *t) {
	const char *title = webkit_web_view_get_title(t->view);
	if (title != NULL && title[0] != '\0') {
		return title;
	}
	const char *uri = webkit_web_view_get_uri(t->view);
	return uri != NULL && strcmp(uri, "about:blank") != 0 ? uri : "New Tab";
}

/* ---- Tab bar ------------------------------------------------------------ */

static int tab_width(struct browser *b, int w) {
	int n = b->tabs->len;
	int tw = n > 0 ? (w - PLUS_W) / n : TAB_MAX_W;
	return tw < TAB_MIN_W ? TAB_MIN_W : tw > TAB_MAX_W ? TAB_MAX_W : tw;
}

static void paint_tabbar(struct browser *b, cairo_t *cr, int w, int h) {
	int tw = tab_width(b, w);
	for (guint i = 0; i < b->tabs->len; i++) {
		struct tab *t = g_ptr_array_index(b->tabs, i);
		int x = i * tw;
		bool active = (int)i == b->active;

		/* The active tab is inverted, as GEM shows an open menu title. */
		if (active) {
			black(cr);
			fill(cr, x, 0, tw - 1, h - 1);
		}
		cairo_save(cr);
		cairo_rectangle(cr, x + GADGET + 1, 0, tw - GADGET - 2, h - 1);
		cairo_clip(cr);
		if (active) {
			white(cr);
		} else {
			black(cr);
		}
		text(cr, tab_title(t), x + GADGET + 7, 0, h - 1);
		cairo_restore(cr);

		if (active) {
			white(cr);
			draw_closer(cr, x, 0);
			fill(cr, x + GADGET, 0, 1, h - 1);  /* after the closer */
		} else {
			black(cr);
			draw_closer(cr, x, 0);
			fill(cr, x + GADGET, 0, 1, h - 1);
		}
		black(cr);
		fill(cr, x + tw - 1, 0, 1, h - 1);      /* between tabs */
	}
	/* The "+" box. */
	int px = b->tabs->len * tw;
	black(cr);
	fill(cr, px + PLUS_W / 2 - 4, h / 2 - 1, 9, 1);
	fill(cr, px + PLUS_W / 2, h / 2 - 5, 1, 9);
	fill(cr, px + PLUS_W - 1, 0, 1, h - 1);
	fill(cr, 0, h - 1, w, 1);
}

static void draw_tabbar(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_tabbar, data);
}

static void tabbar_pressed(GtkGestureClick *gesture, int n_press,
		double x, double y, gpointer data) {
	struct browser *b = data;
	int w = gtk_widget_get_width(b->tabbar);
	int tw = tab_width(b, w);
	int n = b->tabs->len;
	guint button = gtk_gesture_single_get_current_button(
		GTK_GESTURE_SINGLE(gesture));
	int i = (int)x / tw;
	if (i >= n) {
		if (x < n * tw + PLUS_W && button == GDK_BUTTON_PRIMARY) {
			tab_new(b, NULL, NULL, true);
		}
		return;
	}
	bool on_closer = x - i * tw < GADGET;
	if (button == GDK_BUTTON_MIDDLE ||
			(button == GDK_BUTTON_PRIMARY && on_closer)) {
		tab_close(b, i);
	} else if (button == GDK_BUTTON_PRIMARY) {
		tab_select(b, i);
	}
}

/* ---- Toolbar buttons ---------------------------------------------------- */

static struct tab *active_tab(struct browser *b) {
	return b->active >= 0 && b->active < (int)b->tabs->len ?
		g_ptr_array_index(b->tabs, b->active) : NULL;
}

static void paint_buttons(struct browser *b, cairo_t *cr, int w, int h) {
	static const char *grey_rows[] = { "#.", ".#" };
	struct tab *t = active_tab(b);
	bool back = t && webkit_web_view_can_go_back(t->view);
	bool forward = t && webkit_web_view_can_go_forward(t->view);
	bool loading = t && webkit_web_view_is_loading(t->view);
	double c = GADGET / 2.0, cy = h / 2.0;

	for (int i = 0; i < BUTTONS; i++) {
		double x = i * (GADGET + 1);
		bool enabled = i == 0 ? back : i == 1 ? forward : t != NULL;
		/* Disabled buttons are drawn through a dither, like GEM. */
		cairo_push_group(cr);
		black(cr);
		if (i == 0) {
			cairo_move_to(cr, x + 5, cy);
			cairo_line_to(cr, x + GADGET - 6, cy - 5);
			cairo_line_to(cr, x + GADGET - 6, cy + 5);
			cairo_close_path(cr);
			cairo_fill(cr);
		} else if (i == 1) {
			cairo_move_to(cr, x + GADGET - 5, cy);
			cairo_line_to(cr, x + 6, cy - 5);
			cairo_line_to(cr, x + 6, cy + 5);
			cairo_close_path(cr);
			cairo_fill(cr);
		} else if (i == 3) {
			/* Home: a little house with a door. */
			cairo_move_to(cr, x + c, cy - 6);
			cairo_line_to(cr, x + c + 7, cy + 1);
			cairo_line_to(cr, x + c - 7, cy + 1);
			cairo_close_path(cr);
			cairo_fill(cr);
			fill(cr, x + c - 5, cy, 10, 6);
			white(cr);
			fill(cr, x + c - 1, cy + 2, 3, 4);
			black(cr);
		} else if (loading) {
			/* Stop: GemWM's closer X. */
			cairo_set_line_width(cr, 1);
			cairo_move_to(cr, x + 5.5, cy - 4.5);
			cairo_line_to(cr, x + GADGET - 5.5, cy + 4.5);
			cairo_move_to(cr, x + GADGET - 5.5, cy - 4.5);
			cairo_line_to(cr, x + 5.5, cy + 4.5);
			cairo_stroke(cr);
		} else {
			/* Reload: an open circle with an arrowhead. */
			cairo_set_line_width(cr, 1);
			cairo_arc(cr, x + c, cy, 5, -1.2, 4.3);
			cairo_stroke(cr);
			cairo_move_to(cr, x + c + 2, cy - 7);
			cairo_line_to(cr, x + c + 6, cy - 5);
			cairo_line_to(cr, x + c + 2, cy - 2);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		cairo_pop_group_to_source(cr);
		if (enabled) {
			cairo_paint(cr);
		} else {
			cairo_pattern_t *grey = dither(grey_rows, 2, 2, true);
			cairo_mask(cr, grey);
			cairo_pattern_destroy(grey);
		}
		black(cr);
		fill(cr, x + GADGET, 0, 1, h);
	}
	(void)w;
}

static void draw_buttons(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_buttons, data);
}

static void buttons_pressed(GtkGestureClick *gesture, int n_press,
		double x, double y, gpointer data) {
	struct browser *b = data;
	struct tab *t = active_tab(b);
	if (t == NULL) {
		return;
	}
	switch ((int)x / (GADGET + 1)) {
	case 0:
		webkit_web_view_go_back(t->view);
		break;
	case 1:
		webkit_web_view_go_forward(t->view);
		break;
	case 2:
		if (webkit_web_view_is_loading(t->view)) {
			webkit_web_view_stop_loading(t->view);
		} else {
			webkit_web_view_reload(t->view);
		}
		break;
	case 3:
		load_home(t);
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
		break;
	}
}

/* ---- Info line ---------------------------------------------------------- */

static void paint_info(struct browser *b, cairo_t *cr, int w, int h) {
	static const char *progress_rows[] = { "#.", ".#" };
	struct tab *t = active_tab(b);
	char buf[512];
	const char *msg = NULL;

	if (b->hover_link != NULL) {
		msg = b->hover_link;
	} else if (t != NULL && webkit_web_view_is_loading(t->view)) {
		double p = webkit_web_view_get_estimated_load_progress(t->view);
		/* Progress fills the line with a dither from the left. */
		cairo_pattern_t *pat = dither(progress_rows, 2, 2, false);
		cairo_set_source(cr, pat);
		fill(cr, 0, 0, (int)(w * p), h - 1);
		cairo_pattern_destroy(pat);
		snprintf(buf, sizeof(buf), "Loading... %d%%", (int)(p * 100));
		msg = buf;
	} else if (b->notice != NULL) {
		msg = b->notice;
	} else if (b->download_status != NULL) {
		msg = b->download_status;
	} else if (t != NULL && t->media && shared.media_missing != NULL) {
		snprintf(buf, sizeof(buf), "Can't play video or sound: install %s",
			shared.media_missing);
		msg = buf;
	}
	if (msg != NULL) {
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, w - 4, h - 1);
		cairo_clip(cr);
		white(cr);
		fill(cr, 2, 0, text_width(cr, msg) + 8, h - 1);
		black(cr);
		text(cr, msg, 6, 0, h - 1);
		cairo_restore(cr);
	}
	black(cr);
	fill(cr, 0, h - 1, w, 1);
}

static void draw_info(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_info, data);
}

static void set_status(char **field, char *value) {
	g_free(*field);
	*field = value;
}

/* A web app's window shows the info line only when it has something to
 * say: a download, or a notice. */
static void sync_info(struct browser *b) {
	if (b->app) {
		gtk_widget_set_visible(b->info, !b->fullscreen &&
			(b->download_status != NULL || b->notice != NULL));
	}
	gtk_widget_queue_draw(b->info);
}

/* ---- Keeping the chrome in sync ----------------------------------------- */

static bool entry_focused(struct browser *b) {
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(b->window));
	return focus != NULL &&
		(focus == b->entry || gtk_widget_is_ancestor(focus, b->entry));
}

/* Puts text in the address field as ours, not the user's. With focus
 * there, it's selected so that typing replaces it. */
static void set_entry(struct browser *b, const char *text) {
	b->entry_setting = true;
	gtk_editable_set_text(GTK_EDITABLE(b->entry), text);
	b->entry_setting = false;
	b->entry_edited = false;
	if (entry_focused(b)) {
		gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
	}
}

static gboolean complete(gpointer data);

static void entry_changed(GtkEditable *editable, struct browser *b) {
	if (b->entry_setting) {
		return;
	}
	b->entry_edited = true;
	if (entry_focused(b) && b->complete_idle == 0) {
		b->complete_idle = g_idle_add(complete, b);
	}
}

/* Shows the page's address, unless the user is typing a new one. */
static void sync_entry(struct browser *b) {
	struct tab *t = active_tab(b);
	if (t == NULL || (entry_focused(b) && b->entry_edited)) {
		return;
	}
	const char *uri = webkit_web_view_get_uri(t->view);
	set_entry(b, uri != NULL && strcmp(uri, "about:blank") != 0 ? uri : "");
}

/* The tab bar only appears once there are two tabs (Ctrl+T makes the
 * second); a single page gets the whole window. */
static void sync_tabbar(struct browser *b) {
	gtk_widget_set_visible(b->tabbar, !b->fullscreen && !b->app &&
		b->tabs->len >= 2);
	gtk_widget_queue_draw(b->tabbar);
}

/* Tells GemWM how the active tab is scrolled, for the frame's scroll bars.
 * The vertical bar is always there; the horizontal one only for pages
 * wider than the window (it takes height, not width, so it can't make the
 * page reflow in and out of needing it). */
static void report_scroll(struct browser *b) {
	struct tab *t = b->active >= 0 && b->active < (int)b->tabs->len ?
		g_ptr_array_index(b->tabs, b->active) : NULL;
	if (b->scroll == NULL || t == NULL) {
		return;
	}
	int doc_h = t->doc_h > t->view_h ? t->doc_h : t->view_h;
	gemwm_scroll_v1_set_axis(b->scroll, GEMWM_SCROLL_V1_AXIS_VERTICAL,
		t->scroll_y, t->view_h, doc_h > 0 ? doc_h : 1);
	gemwm_scroll_v1_set_axis(b->scroll, GEMWM_SCROLL_V1_AXIS_HORIZONTAL,
		t->scroll_x, t->view_w, t->doc_w > t->view_w + 1 ? t->doc_w : 0);
}

static void on_scroll_message(WebKitUserContentManager *content,
		JSCValue *value, struct tab *t) {
	int *fields[] = { &t->scroll_x, &t->scroll_y, &t->view_w, &t->view_h,
		&t->doc_w, &t->doc_h };
	for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
		JSCValue *v = jsc_value_object_get_property_at_index(value, i);
		*fields[i] = jsc_value_to_int32(v);
		g_object_unref(v);
	}
	if (t->browser->active >= 0 &&
			g_ptr_array_index(t->browser->tabs, t->browser->active) == t) {
		report_scroll(t->browser);
	}
}

/* The user worked a scroll bar in GemWM's frame. */
static void on_scroll_to(void *data, struct gemwm_scroll_v1 *scroll,
		uint32_t axis, int32_t position) {
	struct browser *b = data;
	if (b->active < 0 || b->active >= (int)b->tabs->len) {
		return;
	}
	struct tab *t = g_ptr_array_index(b->tabs, b->active);
	char js[96];
	if (axis == GEMWM_SCROLL_V1_AXIS_VERTICAL) {
		snprintf(js, sizeof(js), "window.scrollTo(window.scrollX, %d)", position);
	} else {
		snprintf(js, sizeof(js), "window.scrollTo(%d, window.scrollY)", position);
	}
	webkit_web_view_evaluate_javascript(t->view, js, -1, NULL, NULL, NULL,
		NULL, NULL);
}

static const struct gemwm_scroll_v1_listener scroll_listener = {
	.scroll_to = on_scroll_to,
};

/* Once the window is on screen it has a Wayland surface to hang the scroll
 * bars on. */
static void window_mapped(GtkWidget *window, struct browser *b) {
	if (shared.scroll_manager == NULL || b->scroll != NULL) {
		return;
	}
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (!GDK_IS_WAYLAND_SURFACE(surface)) {
		return;
	}
	b->scroll = gemwm_scroll_manager_v1_get_scroll(shared.scroll_manager,
		gdk_wayland_surface_get_wl_surface(surface));
	gemwm_scroll_v1_add_listener(b->scroll, &scroll_listener, b);
	report_scroll(b);
}

static void sync_all(struct browser *b) {
	struct tab *t = active_tab(b);
	if (t != NULL) {
		gtk_window_set_title(GTK_WINDOW(b->window), tab_title(t));
	}
	sync_entry(b);
	sync_tabbar(b);
	sync_key(b);
	report_scroll(b);
	app_menu_update(b->menu);
	gtk_widget_queue_draw(b->buttons);
	gtk_widget_queue_draw(b->info);
}

static bool is_active(struct tab *t) {
	return active_tab(t->browser) == t;
}

/* ---- History ------------------------------------------------------------ */

/* Records the page shown in history, once per address it goes to. */
static void record_visit(struct tab *t) {
	const char *uri = webkit_web_view_get_uri(t->view);
	if (uri == NULL || g_strcmp0(uri, t->recorded) == 0 ||
			g_strcmp0(uri, t->failed) == 0 ||
			!(g_str_has_prefix(uri, "https://") ||
			g_str_has_prefix(uri, "http://") ||
			g_str_has_prefix(uri, "file://"))) {
		return;
	}
	t->recorded_new = history_visit(uri, t->typed);
	t->typed = false;
	g_free(t->recorded);
	t->recorded = g_strdup(uri);
	const char *title = webkit_web_view_get_title(t->view);
	if (title != NULL && title[0] != '\0') {
		history_set_title(uri, title);
	}
}

static gboolean record_later(gpointer data) {
	struct tab *t = data;
	t->record_timer = 0;
	if (!webkit_web_view_is_loading(t->view)) {
		record_visit(t);
	}
	return G_SOURCE_REMOVE;
}

/* A page is recorded when it commits (after any redirects), or, for pages
 * that change their address themselves (YouTube's videos), once the
 * address has settled. */
static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event,
		struct tab *t) {
	const char *uri = webkit_web_view_get_uri(view);
	if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_REDIRECTED) {
		ads_apply(t);
	}
	if (event == WEBKIT_LOAD_STARTED) {
		/* Reading ends when anything but the reader's page loads. */
		t->reading = t->reader_loading;
		t->reader_loading = false;
		app_menu_update(t->browser->menu);
	}
	if (event == WEBKIT_LOAD_STARTED && g_strcmp0(uri, t->failed) != 0) {
		g_clear_pointer(&t->failed, g_free);
	} else if (event == WEBKIT_LOAD_COMMITTED) {
		record_visit(t);
	}
}

/* A page that didn't load (a mistyped address) isn't worth suggesting. */
static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
		const char *uri, GError *error, struct tab *t) {
	if (g_error_matches(error, WEBKIT_NETWORK_ERROR,
			WEBKIT_NETWORK_ERROR_CANCELLED)) {
		return FALSE;
	}
	g_free(t->failed);
	t->failed = g_strdup(uri);
	if (t->recorded_new && g_strcmp0(uri, t->recorded) == 0) {
		history_forget(uri);
		t->recorded_new = false;
	}
	return FALSE;
}

static void on_title(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	const char *title = webkit_web_view_get_title(view);
	if (t->recorded != NULL && title != NULL && title[0] != '\0' &&
			g_strcmp0(webkit_web_view_get_uri(view), t->recorded) == 0) {
		history_set_title(t->recorded, title);
	}
	if (is_active(t)) {
		gtk_window_set_title(GTK_WINDOW(t->browser->window), tab_title(t));
	}
	gtk_widget_queue_draw(t->browser->tabbar);
}

static void on_uri(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	if (t->record_timer != 0) {
		g_source_remove(t->record_timer);
	}
	t->record_timer = g_timeout_add(500, record_later, t);
	if (is_active(t)) {
		sync_entry(t->browser);
	}
	gtk_widget_queue_draw(t->browser->tabbar);
}

static void on_progress(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	if (strcmp(pspec->name, "is-loading") == 0 &&
			webkit_web_view_is_loading(view)) {
		if (t->loads++ > 0) {
			t->media = false;
		}
		t->login = false;
		if (is_active(t)) {
			set_status(&t->browser->notice, NULL);
			sync_key(t->browser);
		}
	}
	if (is_active(t)) {
		gtk_widget_queue_draw(t->browser->info);
		gtk_widget_queue_draw(t->browser->buttons);
	}
}

static void on_media_message(WebKitUserContentManager *content,
		JSCValue *value, struct tab *t) {
	t->media = true;
	if (is_active(t)) {
		gtk_widget_queue_draw(t->browser->info);
	}
}

static void on_mouse_target(WebKitWebView *view, WebKitHitTestResult *hit,
		guint modifiers, struct tab *t) {
	struct browser *b = t->browser;
	const char *link = webkit_hit_test_result_context_is_link(hit) ?
		webkit_hit_test_result_get_link_uri(hit) : NULL;
	if (g_strcmp0(link, b->hover_link) != 0) {
		set_status(&b->hover_link, g_strdup(link));
		gtk_widget_queue_draw(b->info);
	}
}

static GtkWidget *on_create(WebKitWebView *view, WebKitNavigationAction *action,
		struct tab *t) {
	if (t->browser->app) {
		/* A link out of the app goes to the browser; its own pop-ups
		 * (a sign-in window) are more of the app. */
		const char *uri = webkit_uri_request_get_uri(
			webkit_navigation_action_get_request(action));
		if (webkit_navigation_action_get_navigation_type(action) ==
				WEBKIT_NAVIGATION_TYPE_LINK_CLICKED && !app_owns(t->browser, uri)) {
			open_in_browser(uri);
			return NULL;
		}
		struct browser *popup = app_window(t->browser->app_uri,
			t->browser->app_name);
		struct tab *pt = tab_new(popup, view, NULL, true);
		gtk_window_present(GTK_WINDOW(popup->window));
		return GTK_WIDGET(pt->view);
	}
	/* target=_blank and window.open(): a new tab, related to this one. */
	return GTK_WIDGET(tab_new(t->browser, view, NULL, true)->view);
}

static void on_close(WebKitWebView *view, struct tab *t) {
	guint index;
	if (g_ptr_array_find(t->browser->tabs, t, &index)) {
		tab_close(t->browser, index);
	}
}

/* Middle-click or Ctrl+click on a link opens it in a background tab. */
static gboolean on_decide_policy(WebKitWebView *view,
		WebKitPolicyDecision *decision, WebKitPolicyDecisionType type,
		struct tab *t) {
	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		return FALSE;
	}
	WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	if (webkit_navigation_action_get_navigation_type(action) !=
			WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
		return FALSE;
	}
	if (t->browser->app) {
		/* A link out of the app opens in the browser (as does a middle
		 * or Ctrl click); redirects and sign-ins stay in the app. */
		const char *uri = webkit_uri_request_get_uri(
			webkit_navigation_action_get_request(action));
		guint button = webkit_navigation_action_get_mouse_button(action);
		guint mods = webkit_navigation_action_get_modifiers(action);
		if (!app_owns(t->browser, uri) || button == GDK_BUTTON_MIDDLE ||
				(mods & GDK_CONTROL_MASK)) {
			open_in_browser(uri);
			webkit_policy_decision_ignore(decision);
			return TRUE;
		}
		return FALSE;
	}
	guint button = webkit_navigation_action_get_mouse_button(action);
	guint mods = webkit_navigation_action_get_modifiers(action);
	if (button == GDK_BUTTON_MIDDLE || (mods & GDK_CONTROL_MASK)) {
		const char *uri = webkit_uri_request_get_uri(
			webkit_navigation_action_get_request(action));
		tab_new(t->browser, NULL, uri, false);
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	return FALSE;
}

/* ---- Downloads ---------------------------------------------------------- */

/* The window a download reports to: the one whose page started it, or
 * else the active one; NULL once there are none. */
static struct browser *download_browser(WebKitDownload *download) {
	WebKitWebView *view = webkit_download_get_web_view(download);
	struct tab *t = view ? g_object_get_data(G_OBJECT(view), "tab") : NULL;
	if (t != NULL) {
		return t->browser;
	}
	GtkWindow *window = gtk_application_get_active_window(shared.app);
	return window ? g_object_get_data(G_OBJECT(window), "browser") : NULL;
}

/* Shows a download message (which it frees) in the right info line. */
static void download_status(WebKitDownload *download, char *message) {
	struct browser *b = download_browser(download);
	if (b == NULL) {
		g_free(message);
		return;
	}
	set_status(&b->download_status, message);
	sync_info(b);
}

static gboolean on_decide_destination(WebKitDownload *download,
		const char *suggested, gpointer data) {
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
	char *fallback = NULL;
	if (dir == NULL) {
		dir = fallback = g_build_filename(g_get_home_dir(), "Downloads", NULL);
	}
	g_mkdir_with_parents(dir, 0755);
	char *base = g_path_get_basename(suggested && *suggested ? suggested : "download");
	char *path = g_build_filename(dir, base, NULL);
	/* Never overwrite: "file.zip" becomes "file (1).zip". */
	for (int n = 1; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; n++) {
		const char *dot = strrchr(base, '.');
		int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
		char *name = g_strdup_printf("%.*s (%d)%s", stem, base, n,
			dot && dot != base ? dot : "");
		g_free(path);
		path = g_build_filename(dir, name, NULL);
		g_free(name);
	}
	webkit_download_set_destination(download, path);
	download_status(download,
		g_strdup_printf("Downloading %s...", strrchr(path, '/') + 1));
	g_free(path);
	g_free(base);
	g_free(fallback);
	return TRUE;
}

static void on_download_progress(WebKitDownload *download, GParamSpec *pspec,
		gpointer data) {
	const char *dest = webkit_download_get_destination(download);
	download_status(download, g_strdup_printf("Downloading %s... %d%%",
		dest ? strrchr(dest, '/') + 1 : "",
		(int)(webkit_download_get_estimated_progress(download) * 100)));
}

static void on_download_finished(WebKitDownload *download, gpointer data) {
	const char *dest = webkit_download_get_destination(download);
	if (dest != NULL) {
		download_status(download, g_strdup_printf("Saved %s", dest));
	}
}

static void on_download_failed(WebKitDownload *download, GError *error,
		gpointer data) {
	download_status(download,
		g_strdup_printf("Download failed: %s", error->message));
}

static void on_download_started(WebKitNetworkSession *session,
		WebKitDownload *download, gpointer data) {
	g_signal_connect(download, "decide-destination",
		G_CALLBACK(on_decide_destination), NULL);
	g_signal_connect(download, "notify::estimated-progress",
		G_CALLBACK(on_download_progress), NULL);
	g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), NULL);
	g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), NULL);
}

/* ---- Tabs --------------------------------------------------------------- */

/* ---- Settings ----------------------------------------------------------- */

static char *settings_path(void);

/* A setting from ~/.config/gemweb/settings, under [group], or NULL.
 * $env, if set, wins (for scripts and tests). The file is read each time,
 * so an edit applies to the next page loaded. */
static char *setting(const char *group, const char *key, const char *env) {
	const char *value = env != NULL ? g_getenv(env) : NULL;
	if (value != NULL) {
		return g_strdup(value);
	}
	char *path = settings_path();
	GKeyFile *kf = g_key_file_new();
	char *s = NULL;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		s = g_key_file_get_string(kf, group, key, NULL);
	}
	g_key_file_free(kf);
	g_free(path);
	return s != NULL ? g_strstrip(s) : NULL;
}

static char *settings_path(void) {
	return g_build_filename(g_get_user_config_dir(), "gemweb", "settings",
		NULL);
}

/* A yes-or-no setting: true, yes, on or 1; false, no, off or 0. */
static bool setting_bool(const char *group, const char *key, bool otherwise) {
	char *v = setting(group, key, NULL);
	bool on = otherwise;
	if (v != NULL) {
		on = g_ascii_strcasecmp(v, "true") == 0 ||
			g_ascii_strcasecmp(v, "yes") == 0 ||
			g_ascii_strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0;
	}
	g_free(v);
	return on;
}

/* Sets (or, for NULL, removes) a setting from the menus, keeping the rest
 * of the file, comments and all. */
static void setting_write(const char *group, const char *key,
		const char *value) {
	char *path = settings_path();
	GKeyFile *kf = g_key_file_new();
	GError *error = NULL;
	g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	if (value != NULL) {
		g_key_file_set_string(kf, group, key, value);
	} else {
		g_key_file_remove_key(kf, group, key, NULL);
		gsize n = 0;
		g_strfreev(g_key_file_get_keys(kf, group, &n, NULL));
		if (n == 0) {
			g_key_file_remove_group(kf, group, NULL);
		}
	}
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	if (!g_key_file_save_to_file(kf, path, &error)) {
		g_printerr("gemweb: can't save %s: %s\n", path, error->message);
		g_error_free(error);
	}
	g_free(dir);
	g_key_file_free(kf);
	g_free(path);
}

/* The search engine: search = a URL with %s for the query, or one of these
 * names. */
static char *search_engine(void) {
	static const struct { const char *name, *url; } engines[] = {
		{ "duckduckgo", "https://duckduckgo.com/?q=%s" },
		{ "google", "https://www.google.com/search?q=%s" },
		{ "bing", "https://www.bing.com/search?q=%s" },
		{ "brave", "https://search.brave.com/search?q=%s" },
		{ "startpage", "https://www.startpage.com/do/search?q=%s" },
		{ "kagi", "https://kagi.com/search?q=%s" },
	};
	char *engine = setting("General", "search", "GEMWEB_SEARCH");
	for (guint i = 0; engine != NULL && i < G_N_ELEMENTS(engines); i++) {
		if (g_ascii_strcasecmp(engine, engines[i].name) == 0) {
			g_free(engine);
			return g_strdup(engines[i].url);
		}
	}
	if (engine == NULL || strstr(engine, "%s") == NULL) {
		if (engine != NULL) {
			g_printerr("gemweb: search = %s has no %%s for the query; "
				"using DuckDuckGo\n", engine);
		}
		g_free(engine);
		return g_strdup(DEFAULT_SEARCH);
	}
	return engine;
}

/* What the user typed, as a URI: addresses as-is, bare names get https://,
 * anything else is a search with the search engine. */
static char *input_to_uri(const char *input, bool *search) {
	char *s = g_strstrip(g_strdup(input));
	char *uri;
	if (search != NULL) {
		*search = false;
	}
	if (s[0] == '\0') {
		uri = NULL;
	} else if (strstr(s, "://") != NULL || g_str_has_prefix(s, "about:") ||
			g_str_has_prefix(s, "data:")) {
		uri = g_strdup(s);
	} else if (s[0] == '/' || s[0] == '~') {
		char *path = s[0] == '~' ?
			g_build_filename(g_get_home_dir(), s + 1, NULL) : g_strdup(s);
		uri = g_filename_to_uri(path, NULL, NULL);
		g_free(path);
	} else if (strchr(s, ' ') == NULL &&
			(strchr(s, '.') != NULL || g_str_has_prefix(s, "localhost"))) {
		uri = g_strconcat("https://", s, NULL);
	} else {
		char *engine = search_engine();
		char *q = g_uri_escape_string(s, NULL, TRUE);
		const char *at = strstr(engine, "%s");
		uri = g_strdup_printf("%.*s%s%s", (int)(at - engine), engine, q, at + 2);
		g_free(q);
		g_free(engine);
		if (search != NULL) {
			*search = true;
		}
	}
	g_free(s);
	return uri;
}

/* The home page: home = an address, or "start" (or nothing) for GemWeb's
 * own start page; Google if it isn't set. */
static void load_home(struct tab *t) {
	if (t->browser->app) {
		webkit_web_view_load_uri(t->view, t->browser->app_uri);
		return;
	}
	char *home = setting("General", "home", "GEMWEB_HOME");
	if (home == NULL) {
		home = g_strdup(DEFAULT_HOME);
	}
	char *uri = strcmp(home, "start") == 0 || strcmp(home, "about:blank") == 0 ?
		NULL : input_to_uri(home, NULL);
	g_free(home);
	if (uri != NULL) {
		webkit_web_view_load_uri(t->view, uri);
	} else {
		webkit_web_view_load_html(t->view, shared.start_page, "about:blank");
	}
	g_free(uri);
}

/* Loads what was typed, or the home page for nothing. */
static void load_input(struct tab *t, const char *input) {
	char *uri = input ? input_to_uri(input, NULL) : NULL;
	if (input == NULL) {
		load_home(t);
	} else if (uri != NULL) {
		webkit_web_view_load_uri(t->view, uri);
	} else {
		webkit_web_view_load_html(t->view, shared.start_page, "about:blank");
	}
	g_free(uri);
}

static void tab_select(struct browser *b, int index) {
	if (index < 0 || index >= (int)b->tabs->len) {
		return;
	}
	struct tab *old = active_tab(b);
	if (old != NULL && find_open(b)) {
		webkit_find_controller_search_finish(
			webkit_web_view_get_find_controller(old->view));
	}
	b->active = index;
	struct tab *t = g_ptr_array_index(b->tabs, index);
	gtk_stack_set_visible_child(GTK_STACK(b->stack), GTK_WIDGET(t->view));
	if (find_open(b)) {
		find_search(b);
	}
	set_status(&b->hover_link, NULL);
	/* A blank tab wants an address; a page wants the keyboard. */
	const char *uri = webkit_web_view_get_uri(t->view);
	if (uri == NULL || strcmp(uri, "about:blank") == 0) {
		gtk_widget_grab_focus(b->entry);
		set_entry(b, "");
	} else {
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
	}
	sync_all(b);
}

static struct tab *tab_new(struct browser *b, WebKitWebView *related,
		const char *input, bool select) {
	struct tab *t = g_new0(struct tab, 1);
	t->browser = b;
	t->media = b->tabs->len == 0;
	/* Each tab has its own content manager so its scroll reports say which
	 * tab they come from. */
	t->content = webkit_user_content_manager_new();
	webkit_user_content_manager_add_style_sheet(t->content, shared.style);
	if (shared.scroll_manager != NULL) {
		webkit_user_content_manager_add_script(t->content, shared.scroll_script);
		webkit_user_content_manager_register_script_message_handler(t->content,
			"gemwmScroll", NULL);
		g_signal_connect(t->content, "script-message-received::gemwmScroll",
			G_CALLBACK(on_scroll_message), t);
	}
	if (shared.login_script != NULL) {
		webkit_user_content_manager_add_script(t->content, shared.login_script);
		webkit_user_content_manager_register_script_message_handler(t->content,
			"gemwebLogin", PASSWORD_WORLD);
		g_signal_connect(t->content, "script-message-received::gemwebLogin",
			G_CALLBACK(on_login_message), t);
	}
	if (shared.media_script != NULL) {
		webkit_user_content_manager_add_script(t->content, shared.media_script);
		webkit_user_content_manager_register_script_message_handler(t->content,
			"gemwebMedia", NULL);
		g_signal_connect(t->content, "script-message-received::gemwebMedia",
			G_CALLBACK(on_media_message), t);
	}
	if (related != NULL) {
		t->view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
			"related-view", related,
			"user-content-manager", t->content, NULL);
	} else {
		t->view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
			"network-session", shared.session,
			"user-content-manager", t->content,
			"settings", shared.settings, NULL);
	}
	gtk_widget_set_vexpand(GTK_WIDGET(t->view), TRUE);
	g_object_set_data(G_OBJECT(t->view), "tab", t);
	g_signal_connect(t->view, "notify::title", G_CALLBACK(on_title), t);
	g_signal_connect(t->view, "notify::uri", G_CALLBACK(on_uri), t);
	g_signal_connect(t->view, "notify::estimated-load-progress",
		G_CALLBACK(on_progress), t);
	g_signal_connect(t->view, "notify::is-loading", G_CALLBACK(on_progress), t);
	g_signal_connect(t->view, "mouse-target-changed",
		G_CALLBACK(on_mouse_target), t);
	g_signal_connect(t->view, "create", G_CALLBACK(on_create), t);
	g_signal_connect(t->view, "close", G_CALLBACK(on_close), t);
	g_signal_connect(t->view, "decide-policy", G_CALLBACK(on_decide_policy), t);
	g_signal_connect(t->view, "load-changed", G_CALLBACK(on_load_changed), t);
	g_signal_connect(t->view, "load-failed", G_CALLBACK(on_load_failed), t);
	g_signal_connect(t->view, "script-dialog", G_CALLBACK(on_script_dialog), t);
	g_signal_connect(t->view, "context-menu", G_CALLBACK(on_context_menu), t);
	find_connect(t);
	g_signal_connect(t->view, "enter-fullscreen",
		G_CALLBACK(on_enter_fullscreen), t);
	g_signal_connect(t->view, "leave-fullscreen",
		G_CALLBACK(on_leave_fullscreen), t);

	gtk_stack_add_child(GTK_STACK(b->stack), GTK_WIDGET(t->view));
	/* New tabs open next to the current one. */
	guint at = b->active >= 0 ? (guint)b->active + 1 : b->tabs->len;
	g_ptr_array_insert(b->tabs, at, t);
	if ((int)at <= b->active) {
		b->active++;
	}
	sync_tabbar(b);
	if (related == NULL) {
		load_input(t, input);
	}
	if (select) {
		tab_select(b, at);
		/* A new tab shows the home page but waits for an address: the
		 * home page's address is shown, selected, so typing replaces it. */
		if (related == NULL && input == NULL) {
			gtk_widget_grab_focus(b->entry);
			b->entry_edited = false;
			sync_entry(b);
		}
	} else {
		gtk_widget_queue_draw(b->tabbar);
	}
	return t;
}

static void tab_close(struct browser *b, int index) {
	struct tab *t = g_ptr_array_index(b->tabs, index);
	g_ptr_array_remove_index(b->tabs, index);
	g_object_set_data(G_OBJECT(t->view), "tab", NULL);
	g_signal_handlers_disconnect_by_data(t->content, t);
	g_object_unref(t->content);
	if (t->record_timer != 0) {
		g_source_remove(t->record_timer);
	}
	g_free(t->recorded);
	g_free(t->failed);
	gtk_stack_remove(GTK_STACK(b->stack), GTK_WIDGET(t->view));
	g_free(t);
	if (b->tabs->len == 0) {
		gtk_window_destroy(GTK_WINDOW(b->window));
		return;
	}
	if (b->active > index || b->active >= (int)b->tabs->len) {
		b->active--;
	}
	tab_select(b, b->active);
}

/* ---- Input -------------------------------------------------------------- */

/* ---- Completing addresses ----------------------------------------------- */

/* As the user types, the address completes to a site (or page) visited
 * before, the rest selected, and a list of pages from history drops down
 * over the page. Up and Down choose from it, Shift+Delete forgets one. */

static void list_hide(struct browser *b) {
	g_clear_pointer(&b->suggestions, history_entries_free);
	b->selected = -1;
	gtk_widget_set_visible(b->list, FALSE);
}

/* Shows the pages for text under the address field (the top pages for
 * none), keeping the selected row if it's still there. */
static void list_show(struct browser *b, const char *text) {
	if (b->suggestions != NULL) {
		history_entries_free(b->suggestions);
	}
	b->suggestions = history_suggest(text, SUGGEST_ROWS);
	int n = b->suggestions->len;
	if (n == 0) {
		list_hide(b);
		return;
	}
	b->selected = MIN(b->selected, n - 1);
	/* Its top line sits on the toolbar's bottom one, its left on the
	 * address field's border. */
	gtk_widget_set_margin_top(b->list,
		(gtk_widget_get_visible(b->tabbar) ? TAB_H : 0) + TOOL_H);
	gtk_widget_set_margin_start(b->list, BUTTONS * (GADGET + 1) - 1);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->list),
		n * ROW_H + 2);
	gtk_widget_set_visible(b->list, TRUE);
	gtk_widget_queue_draw(b->list);
}

static struct history_entry *selected_entry(struct browser *b) {
	if (b->suggestions == NULL || b->selected < 0 ||
			b->selected >= (int)b->suggestions->len) {
		return NULL;
	}
	return g_ptr_array_index(b->suggestions, b->selected);
}

/* Puts text in the field while completing: the user's still editing. */
static void entry_put(struct browser *b, const char *text) {
	b->entry_setting = true;
	gtk_editable_set_text(GTK_EDITABLE(b->entry), text);
	gtk_editable_set_position(GTK_EDITABLE(b->entry), -1);
	b->entry_setting = false;
}

/* The field shows the selected row's address, or what was typed. */
static void show_selected(struct browser *b) {
	struct history_entry *e = selected_entry(b);
	entry_put(b, e != NULL ? e->uri : b->typed != NULL ? b->typed : "");
	gtk_widget_queue_draw(b->list);
}

static void forget_completion(struct browser *b) {
	g_clear_pointer(&b->completion, g_free);
	g_clear_pointer(&b->completion_uri, g_free);
}

/* After an edit: completes inline if the edit added to the end, and
 * refreshes the list. */
static gboolean complete(gpointer data) {
	struct browser *b = data;
	GtkEditable *e = GTK_EDITABLE(b->entry);
	b->complete_idle = 0;
	const char *text = gtk_editable_get_text(e);
	g_free(b->typed);
	b->typed = g_strdup(text);
	forget_completion(b);
	b->selected = -1;
	int len = g_utf8_strlen(text, -1);
	char *completed, *uri;
	if (b->inserted && gtk_editable_get_position(e) == len &&
			!gtk_editable_get_selection_bounds(e, NULL, NULL) &&
			history_complete(text, &completed, &uri)) {
		b->completion = completed;
		b->completion_uri = uri;
		entry_put(b, completed);
		gtk_editable_select_region(e, len, -1);
	}
	if (text[0] != '\0') {
		list_show(b, b->typed);
	} else {
		list_hide(b);
	}
	return G_SOURCE_REMOVE;
}

/* Whether the user's last edit added text or took it away: only adding
 * completes, or Backspace would put back what it removed. */
static void entry_inserted(GtkEditable *editable, const char *text, int len,
		int *position, struct browser *b) {
	if (!b->entry_setting) {
		b->inserted = true;
	}
}

static void entry_deleted(GtkEditable *editable, int start, int end,
		struct browser *b) {
	if (!b->entry_setting) {
		b->inserted = false;
	}
}

static void entry_focus_left(GtkEventControllerFocus *focus, struct browser *b) {
	list_hide(b);
}

/* Goes to uri in the active tab; typed if it came from the address field
 * rather than a search. */
static void go_to(struct browser *b, const char *uri, bool typed) {
	struct tab *t = active_tab(b);
	list_hide(b);
	forget_completion(b);
	if (t == NULL || uri == NULL) {
		return;
	}
	t->typed = typed;
	/* Focus the page first, so the address field shows the real URI as
	 * soon as the load starts instead of what was typed. */
	gtk_widget_grab_focus(GTK_WIDGET(t->view));
	webkit_web_view_load_uri(t->view, uri);
	sync_entry(b);
}

static void entry_activate(GtkEntry *entry, struct browser *b) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
	struct history_entry *e = selected_entry(b);
	bool search = false;
	char *uri;
	if (e != NULL) {
		uri = g_strdup(e->uri);
	} else if (b->completion != NULL && strcmp(text, b->completion) == 0) {
		uri = g_strdup(b->completion_uri);
	} else {
		uri = input_to_uri(text, &search);
	}
	go_to(b, uri, !search);
	g_free(uri);
}

/* Up and Down move through the list (Down opens it), Shift+Delete forgets
 * the selected page, and Escape first closes the list, then puts the
 * page's address back. */
static gboolean entry_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	bool open = gtk_widget_get_visible(b->list);
	switch (keyval) {
	case GDK_KEY_Down:
	case GDK_KEY_Up:
		if (!open) {
			if (keyval == GDK_KEY_Up) {
				return FALSE;
			}
			/* An untouched address lists the top pages. */
			const char *text = gtk_editable_get_text(GTK_EDITABLE(b->entry));
			g_free(b->typed);
			b->typed = g_strdup(text);
			b->selected = -1;
			list_show(b, b->entry_edited ? text : "");
			return TRUE;
		}
		int n = b->suggestions->len;
		/* -1 is back in the field, with what was typed. */
		b->selected += keyval == GDK_KEY_Down ? 1 : -1;
		if (b->selected < -1) {
			b->selected = n - 1;
		} else if (b->selected >= n) {
			b->selected = -1;
		}
		forget_completion(b);
		show_selected(b);
		return TRUE;
	case GDK_KEY_Delete:
	case GDK_KEY_KP_Delete:
		if (!open || !(state & GDK_SHIFT_MASK) || selected_entry(b) == NULL) {
			return FALSE;
		}
		history_forget(selected_entry(b)->uri);
		list_show(b, b->typed != NULL ? b->typed : "");
		show_selected(b);
		return TRUE;
	case GDK_KEY_Escape:
		if (open || b->completion != NULL) {
			list_hide(b);
			forget_completion(b);
			entry_put(b, b->typed != NULL ? b->typed : "");
			return TRUE;
		}
		struct tab *t = active_tab(b);
		if (t != NULL) {
			gtk_widget_grab_focus(GTK_WIDGET(t->view));
			b->entry_edited = false;
			sync_entry(b);
		}
		return TRUE;
	}
	return FALSE;
}

/* The list: the address on the left, the title on the right. */
static void paint_list(struct browser *b, cairo_t *cr, int w, int h) {
	black(cr);
	fill(cr, 0, 0, w, 1);
	fill(cr, 0, 0, 1, h);
	fill(cr, w - 1, 0, 1, h);
	fill(cr, 0, h - 1, w, 1);
	if (b->suggestions == NULL) {
		return;
	}
	int split = w * 11 / 20;
	for (guint i = 0; i < b->suggestions->len; i++) {
		struct history_entry *e = g_ptr_array_index(b->suggestions, i);
		int y = 1 + i * ROW_H;
		black(cr);
		if ((int)i == b->selected) {
			fill(cr, 1, y, w - 2, ROW_H);
			white(cr);
		}
		cairo_save(cr);
		cairo_rectangle(cr, 1, y, split - 1 - PAD, ROW_H);
		cairo_clip(cr);
		text(cr, history_bare(e->uri), 6, y, ROW_H);
		cairo_restore(cr);
		if (e->title != NULL) {
			cairo_save(cr);
			cairo_rectangle(cr, split, y, w - 1 - split - 4, ROW_H);
			cairo_clip(cr);
			text(cr, e->title, split, y, ROW_H);
			cairo_restore(cr);
		}
	}
}

static void draw_list(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_list, data);
}

static int list_row(struct browser *b, double y) {
	int row = ((int)y - 1) / ROW_H;
	return y >= 1 && b->suggestions != NULL &&
		row < (int)b->suggestions->len ? row : -1;
}

/* The pointer selects rows too; the field keeps what was typed. */
static void list_motion(GtkEventControllerMotion *motion, double x, double y,
		struct browser *b) {
	int row = list_row(b, y);
	if (row >= 0 && row != b->selected) {
		b->selected = row;
		gtk_widget_queue_draw(b->list);
	}
}

static void list_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	int row = list_row(b, y);
	if (row >= 0) {
		struct history_entry *e = g_ptr_array_index(b->suggestions, row);
		char *uri = g_strdup(e->uri);
		go_to(b, uri, true);
		g_free(uri);
	}
}

/* Keyboard shortcuts and menu items; the values are the menu item ids. */
enum action {
	ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_FOCUS_URL, ACT_NEXT_TAB, ACT_PREV_TAB,
	ACT_RELOAD, ACT_BACK, ACT_FORWARD, ACT_ZOOM_IN, ACT_ZOOM_OUT,
	ACT_ZOOM_RESET, ACT_QUIT, ACT_HOME, ACT_NEW_WINDOW, ACT_CLEAR_HISTORY,
	ACT_FILL_PASSWORD, ACT_LOCK_PASSWORDS, ACT_READER, ACT_FIND,
	ACT_FIND_NEXT, ACT_FIND_PREV, ACT_PRINT, ACT_BLOCK_ADS, ACT_BLOCK_ADS_SITE,
	ACT_OPEN_IN_BROWSER,
};

static struct browser *browser_new(GtkApplication *app);

/* ---- Dialogs ------------------------------------------------------------- */

/* A GEM alert box over the window: its lines, maybe a field and a line for
 * what went wrong, then the buttons at the right, the second the default
 * with a thicker border. Return takes the default, Escape the one that
 * doesn't act. */

#define DIALOG_TEXT_W 440 /* page messages wrap at this */
#define DIALOG_MAX_LINES 14

static double measure(const char *s) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(img);
	cairo_select_font_face(cr, font_family, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE);
	double w = text_width(cr, s);
	cairo_destroy(cr);
	cairo_surface_destroy(img);
	return w;
}

/* Adds text to lines, wrapped at width: at spaces, or inside a word too
 * long for a line; its own newlines kept. At most DIALOG_MAX_LINES. */
static void wrap_text(GPtrArray *lines, const char *text, double width) {
	char **paras = g_strsplit(text, "\n", -1);
	for (int p = 0; paras[p] != NULL; p++) {
		GString *line = g_string_new(NULL);
		char **words = g_strsplit(g_strstrip(paras[p]), " ", -1);
		for (int i = 0; words[i] != NULL; i++) {
			const char *w = words[i];
			if (w[0] == '\0') {
				continue;
			}
			GString *try = g_string_new(line->str);
			g_string_append_printf(try, "%s%s", line->len ? " " : "", w);
			if (measure(try->str) <= width || line->len == 0) {
				g_string_assign(line, try->str);
			} else {
				g_ptr_array_add(lines, g_string_free(line, FALSE));
				line = g_string_new(w);
			}
			g_string_free(try, TRUE);
			/* A word wider than a line breaks where it must. */
			while (measure(line->str) > width && line->len > 1) {
				glong n = g_utf8_strlen(line->str, -1);
				while (n > 1) {
					char *head = g_utf8_substring(line->str, 0, --n);
					bool fits = measure(head) <= width;
					if (fits) {
						g_ptr_array_add(lines, head);
						g_string_erase(line, 0,
							g_utf8_offset_to_pointer(line->str, n) - line->str);
						break;
					}
					g_free(head);
				}
			}
		}
		g_strfreev(words);
		g_ptr_array_add(lines, g_string_free(line, FALSE));
	}
	g_strfreev(paras);
	if (lines->len > DIALOG_MAX_LINES) {
		g_ptr_array_set_size(lines, DIALOG_MAX_LINES);
		char *last = lines->pdata[DIALOG_MAX_LINES - 1];
		lines->pdata[DIALOG_MAX_LINES - 1] = g_strconcat(last, "...", NULL);
		g_free(last);
	}
}

static int button_width(struct browser *b) {
	double w = measure(b->dialog_labels[1]);
	if (b->dialog_labels[0] != NULL) {
		w = MAX(w, measure(b->dialog_labels[0]));
	}
	return (int)w + 2 * PAD;
}

/* Sizes the box and places the field for the dialog shown. */
static void dialog_layout(struct browser *b) {
	bool field = b->dialog_field_kind != FIELD_NONE;
	int n = b->dialog_lines->len;
	double tw = field ? 280 : 0;
	for (int i = 0; i < n; i++) {
		tw = MAX(tw, measure(b->dialog_lines->pdata[i]));
	}
	if (b->dialog_error != NULL) {
		tw = MAX(tw, measure(b->dialog_error));
	}
	int bw = button_width(b);
	int w = MAX((int)tw, 2 * bw + PAD) + 4 * PAD + 6;
	int y = 3 + 2 * PAD + n * 18;
	if (field) {
		b->dialog_field_y = y + PAD / 2;
		y = b->dialog_field_y + FIELD_H + PAD / 2 + 18; /* then the error */
		gtk_widget_set_margin_start(b->dialog_field, 3 + 2 * PAD);
		gtk_widget_set_margin_top(b->dialog_field, b->dialog_field_y);
		gtk_widget_set_size_request(b->dialog_field, w - 2 * (3 + 2 * PAD),
			FIELD_H);
	}
	gtk_widget_set_visible(b->dialog_field, field);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->dialog_area), w);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->dialog_area),
		y + PAD + BUTTON_H + 2 * PAD + 3);
	gtk_widget_queue_draw(b->dialog_area);
}

static void paint_dialog(struct browser *b, cairo_t *cr, int w, int h) {
	black(cr);
	fill(cr, 0, 0, w, 1);
	fill(cr, 0, h - 1, w, 1);
	fill(cr, 0, 0, 1, h);
	fill(cr, w - 1, 0, 1, h);
	fill(cr, 3, 3, w - 6, 2);
	fill(cr, 3, h - 5, w - 6, 2);
	fill(cr, 3, 3, 2, h - 6);
	fill(cr, w - 5, 3, 2, h - 6);
	for (guint i = 0; i < b->dialog_lines->len; i++) {
		text(cr, b->dialog_lines->pdata[i], 3 + 2 * PAD, 3 + 2 * PAD + i * 18,
			18);
	}
	if (b->dialog_field_kind != FIELD_NONE && b->dialog_error != NULL) {
		text(cr, b->dialog_error, 3 + 2 * PAD,
			b->dialog_field_y + FIELD_H + PAD / 2, 18);
	}
	int bw = button_width(b);
	int by = h - 3 - 2 * PAD - BUTTON_H;
	int bx = w - 3 - 2 * PAD - 2 * bw - PAD;
	for (int i = 0; i < 2; i++) {
		int *r = b->dialog_buttons[i];
		r[0] = r[1] = r[2] = r[3] = 0;
		if (b->dialog_labels[i] == NULL) {
			continue;
		}
		int x = bx + i * (bw + PAD), t = i == 1 ? 2 : 1;
		fill(cr, x, by, bw, t);
		fill(cr, x, by + BUTTON_H - t, bw, t);
		fill(cr, x, by, t, BUTTON_H);
		fill(cr, x + bw - t, by, t, BUTTON_H);
		text(cr, b->dialog_labels[i],
			x + (bw - text_width(cr, b->dialog_labels[i])) / 2, by, BUTTON_H);
		r[0] = x, r[1] = by, r[2] = bw, r[3] = BUTTON_H;
	}
}

static void draw_dialog(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_dialog, data);
}

static bool dialog_up(struct browser *b) {
	return b->dialog != DIALOG_NONE;
}

/* Shows (or, for DIALOG_NONE, takes away) the dialog set up in b. It holds
 * the window: the rest of it takes no input meanwhile. */
static void dialog_open(struct browser *b, enum dialog dialog) {
	b->dialog = dialog;
	g_clear_pointer(&b->dialog_error, g_free);
	if (dialog != DIALOG_NONE) {
		gtk_entry_set_visibility(GTK_ENTRY(b->dialog_field),
			b->dialog_field_kind != FIELD_PASSWORD);
		gtk_entry_set_input_purpose(GTK_ENTRY(b->dialog_field),
			b->dialog_field_kind == FIELD_PASSWORD ?
			GTK_INPUT_PURPOSE_PASSWORD : GTK_INPUT_PURPOSE_FREE_FORM);
		dialog_layout(b);
		list_hide(b);
	} else {
		gtk_editable_set_text(GTK_EDITABLE(b->dialog_field), "");
	}
	gtk_widget_set_sensitive(b->box, dialog == DIALOG_NONE);
	gtk_widget_set_visible(b->dialog_box, dialog != DIALOG_NONE);
	if (dialog != DIALOG_NONE && b->dialog_field_kind != FIELD_NONE) {
		gtk_widget_grab_focus(b->dialog_field);
		gtk_editable_select_region(GTK_EDITABLE(b->dialog_field), 0, -1);
	} else if (dialog == DIALOG_NONE && active_tab(b) != NULL) {
		/* Back to the page, not the first thing that takes focus. */
		gtk_widget_grab_focus(GTK_WIDGET(active_tab(b)->view));
	}
	app_menu_update(b->menu);
}

/* One of GemWeb's own dialogs. */
static void dialog_show(struct browser *b, enum dialog dialog) {
	static const struct {
		const char *lines[3];
		const char *labels[2];
		int act;
		enum field field;
	} dialogs[] = {
		[DIALOG_CLEAR_HISTORY] = {
			{ "Clear all history?", "Pages you've visited won't be suggested",
				"any more. Cookies and logins stay." },
			{ "Clear", "Cancel" }, 0, FIELD_NONE },
		[DIALOG_UNLOCK] = {
			{ "Your passwords are locked.", "Master password:" },
			{ "Cancel", "Unlock" }, 1, FIELD_PASSWORD },
	};
	if (dialog != DIALOG_NONE) {
		g_ptr_array_set_size(b->dialog_lines, 0);
		for (int i = 0; i < 3 && dialogs[dialog].lines[i] != NULL; i++) {
			g_ptr_array_add(b->dialog_lines, g_strdup(dialogs[dialog].lines[i]));
		}
		b->dialog_labels[0] = dialogs[dialog].labels[0];
		b->dialog_labels[1] = dialogs[dialog].labels[1];
		b->dialog_act = dialogs[dialog].act;
		b->dialog_field_kind = dialogs[dialog].field;
		gtk_editable_set_text(GTK_EDITABLE(b->dialog_field), "");
	}
	dialog_open(b, dialog);
}

static void dialog_error(struct browser *b, const char *error) {
	g_free(b->dialog_error);
	b->dialog_error = g_strdup(error);
	dialog_layout(b);
}

/* ---- A page's own dialogs: alert(), confirm(), prompt(), leaving ---- */

/* WebKit waits for the answer, which comes when a button's pressed. */
static void script_answer(struct browser *b, bool yes) {
	WebKitScriptDialog *d = b->script;
	b->script = NULL;
	if (d == NULL) {
		return;
	}
	switch (webkit_script_dialog_get_dialog_type(d)) {
	case WEBKIT_SCRIPT_DIALOG_CONFIRM:
	case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
		webkit_script_dialog_confirm_set_confirmed(d, yes);
		break;
	case WEBKIT_SCRIPT_DIALOG_PROMPT:
		if (yes) {
			webkit_script_dialog_prompt_set_text(d,
				gtk_editable_get_text(GTK_EDITABLE(b->dialog_field)));
		}
		break;
	default:
		break;
	}
	webkit_script_dialog_close(d);
	webkit_script_dialog_unref(d);
}

static gboolean on_script_dialog(WebKitWebView *view, WebKitScriptDialog *d,
		struct tab *t) {
	struct browser *b = t->browser;
	if (dialog_up(b)) {
		return FALSE; /* one at a time: WebKit's own, this once */
	}
	/* A tab behind asks: it comes forward. */
	guint index;
	if (!is_active(t) && g_ptr_array_find(b->tabs, t, &index)) {
		tab_select(b, index);
	}
	WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(d);
	g_ptr_array_set_size(b->dialog_lines, 0);
	b->dialog_field_kind = FIELD_NONE;
	if (type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) {
		/* Staying is safe: it's the default. */
		g_ptr_array_add(b->dialog_lines, g_strdup("Leave this page?"));
		g_ptr_array_add(b->dialog_lines,
			g_strdup("Changes you made may not be saved."));
		b->dialog_labels[0] = "Leave";
		b->dialog_labels[1] = "Stay";
		b->dialog_act = 0;
	} else {
		char *site = site_of(webkit_web_view_get_uri(view));
		char *says = g_strdup_printf("%s says:", site ? site : "This page");
		g_ptr_array_add(b->dialog_lines, says);
		g_free(site);
		const char *message = webkit_script_dialog_get_message(d);
		if (message != NULL && message[0] != '\0') {
			wrap_text(b->dialog_lines, message, DIALOG_TEXT_W);
		}
		b->dialog_labels[0] = type == WEBKIT_SCRIPT_DIALOG_ALERT ? NULL :
			"Cancel";
		b->dialog_labels[1] = "OK";
		b->dialog_act = 1;
		if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
			const char *text = webkit_script_dialog_prompt_get_default_text(d);
			b->dialog_field_kind = FIELD_TEXT;
			gtk_editable_set_text(GTK_EDITABLE(b->dialog_field),
				text != NULL ? text : "");
		}
	}
	b->script = webkit_script_dialog_ref(d);
	dialog_open(b, DIALOG_SCRIPT);
	return TRUE;
}

static void unlock_submit(struct browser *b, char *password);
static void unlock_cancelled(struct browser *b);

static void dialog_answer(struct browser *b, int button) {
	enum dialog dialog = b->dialog;
	bool yes = button == b->dialog_act;
	if (dialog == DIALOG_UNLOCK && yes) {
		if (!b->unlocking) {
			GtkEditable *field = GTK_EDITABLE(b->dialog_field);
			char *password = g_strdup(gtk_editable_get_text(field));
			gtk_editable_set_text(field, "");
			unlock_submit(b, password);
		}
		return;
	}
	if (dialog == DIALOG_SCRIPT) {
		script_answer(b, yes); /* before the field's emptied */
	}
	dialog_open(b, DIALOG_NONE);
	if (dialog == DIALOG_CLEAR_HISTORY && yes) {
		history_clear();
	} else if (dialog == DIALOG_UNLOCK) {
		unlock_cancelled(b);
	}
}

static void dialog_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	for (int i = 0; i < 2; i++) {
		int *r = b->dialog_buttons[i];
		if (r[2] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] &&
				y < r[1] + r[3]) {
			dialog_answer(b, i);
			return;
		}
	}
}

/* Return takes the default button; Escape the one that doesn't act (or,
 * with only OK, OK). Other keys go to the field, if there is one. */
static gboolean dialog_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	if (!dialog_up(b)) {
		return FALSE;
	}
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		dialog_answer(b, 1);
	} else if (keyval == GDK_KEY_Escape) {
		dialog_answer(b, b->dialog_labels[0] == NULL ? 1 :
			b->dialog_act == 0 ? 1 : 0);
	} else {
		return b->dialog_field_kind == FIELD_NONE;
	}
	return TRUE;
}

/* ---- Passwords ------------------------------------------------------------ */

/* GemWeb fills in logins from a password manager's command-line tool (see
 * passwords.h). A key gadget at the end of the toolbar shows when the page
 * has a login field; pressing it asks the tool for the page's host,
 * unlocking it first if need be, then fills the page's fields, from a list
 * if the host has several logins. Nothing is filled without the press, and
 * only into the page the lookup was for. */

/* Runs in its own JavaScript world, which the page's scripts can't see or
 * change: says whether the page has a login field, as that changes. */
static const char login_script[] =
	"(function () {"
	"  var last = null, queued = false;"
	"  function check() {"
	"    queued = false;"
	"    var has = Array.prototype.some.call(document.querySelectorAll("
	"      'input[type=password], input[autocomplete~=username]'),"
	"      function (e) { return e.getClientRects().length > 0; });"
	"    if (has !== last) {"
	"      last = has;"
	"      window.webkit.messageHandlers.gemwebLogin.postMessage(has);"
	"    }"
	"  }"
	"  function queue() {"
	"    if (!queued) { queued = true; setTimeout(check, 300); }"
	"  }"
	"  new MutationObserver(queue).observe(document.documentElement,"
	"    { subtree: true, childList: true, attributes: true,"
	"      attributeFilter: ['type', 'style', 'class', 'hidden'] });"
	"  addEventListener('load', queue);"
	"  check();"
	"})();";

/* Fills a login, given host, username and password, in that same world.
 * The password field is the focused one or the first shown; the username
 * field is the text field before it (or, on a page asking only for a
 * username, that field). Values are set the way typing sets them, so
 * pages' scripts see them. Returns whether it found somewhere to fill. */
static const char fill_script[] =
	"if (location.hostname !== host) return false;"
	"function shown(e) {"
	"  return !e.disabled && !e.readOnly && e.getClientRects().length > 0 &&"
	"    getComputedStyle(e).visibility !== 'hidden';"
	"}"
	"function isUser(e) {"
	"  return ['text', 'email', 'tel'].indexOf(e.type) >= 0;"
	"}"
	"var inputs = Array.prototype.filter.call("
	"  document.querySelectorAll('input'), shown);"
	"var active = document.activeElement;"
	"var focused = active && inputs.indexOf(active) >= 0 ? active : null;"
	"var pass = focused && focused.type === 'password' ? focused :"
	"  inputs.find(function (e) { return e.type === 'password'; });"
	"var user = null;"
	"if (pass) {"
	"  for (var i = inputs.indexOf(pass) - 1; i >= 0 && !user; i--) {"
	"    if (isUser(inputs[i])) user = inputs[i];"
	"  }"
	"} else {"
	"  user = inputs.find(function (e) {"
	"    return /username/.test(e.autocomplete); }) ||"
	"    (focused && isUser(focused) ? focused : null);"
	"}"
	"var set = Object.getOwnPropertyDescriptor("
	"  HTMLInputElement.prototype, 'value').set;"
	"function put(e, v) {"
	"  e.focus();"
	"  set.call(e, v);"
	"  e.dispatchEvent(new Event('input', { bubbles: true }));"
	"  e.dispatchEvent(new Event('change', { bubbles: true }));"
	"}"
	"if (user && username) put(user, username);"
	"if (pass) put(pass, password);"
	"return !!(user || pass);";

static char *password_command(const char *key) {
	return setting("Passwords", key, NULL);
}

static void show_notice(struct browser *b, char *message) {
	set_status(&b->notice, message);
	sync_info(b);
}

static void sync_key(struct browser *b) {
	struct tab *t = active_tab(b);
	gtk_widget_set_visible(b->key, shared.passwords && t != NULL && t->login);
	gtk_widget_queue_draw(b->key);
	app_menu_update(b->menu);
}

static void on_login_message(WebKitUserContentManager *content,
		JSCValue *value, struct tab *t) {
	t->login = jsc_value_to_boolean(value);
	if (is_active(t)) {
		sync_key(t->browser);
	}
}

/* The host to look up for the page, or NULL (and why) if it isn't a page
 * to fill a password into: only https, or http on this machine. */
static char *page_host(struct tab *t, const char **why) {
	const char *uri = webkit_web_view_get_uri(t->view);
	GUri *u = uri != NULL ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
	char *host = NULL;
	*why = "This page can't have a password";
	if (u != NULL && g_uri_get_host(u) != NULL) {
		const char *scheme = g_uri_get_scheme(u), *h = g_uri_get_host(u);
		bool local = strcmp(h, "localhost") == 0 ||
			strcmp(h, "127.0.0.1") == 0 || strcmp(h, "::1") == 0;
		if (strcmp(scheme, "https") == 0 ||
				(strcmp(scheme, "http") == 0 && local)) {
			host = g_strdup(h);
		} else if (strcmp(scheme, "http") == 0) {
			*why = "Not filling a password into an unencrypted (http) page";
		}
	}
	if (u != NULL) {
		g_uri_unref(u);
	}
	return host;
}

static void logins_hide(struct browser *b) {
	g_clear_pointer(&b->logins, logins_free);
	gtk_widget_set_visible(b->logins_list, FALSE);
}

static void filled(GObject *source, GAsyncResult *result, gpointer data) {
	WebKitWebView *view = WEBKIT_WEB_VIEW(source);
	struct tab *t = g_object_get_data(G_OBJECT(view), "tab");
	GError *error = NULL;
	JSCValue *value = webkit_web_view_call_async_javascript_function_finish(
		view, result, &error);
	if (t != NULL) {
		bool ok = value != NULL && jsc_value_to_boolean(value);
		show_notice(t->browser, g_strdup(ok ? NULL :
			"Couldn't find where to fill the password"));
	}
	g_clear_object(&value);
	g_clear_error(&error);
}

/* Fills login into the page, if it's still on the host looked up. */
static void fill_login(struct browser *b, struct login *login) {
	struct tab *t = active_tab(b);
	const char *why;
	char *host = t != NULL ? page_host(t, &why) : NULL;
	if (host == NULL || g_strcmp0(host, b->login_host) != 0) {
		show_notice(b, g_strdup("The page changed, so nothing was filled"));
		g_free(host);
		return;
	}
	GVariantDict args;
	g_variant_dict_init(&args, NULL);
	g_variant_dict_insert(&args, "host", "s", host);
	g_variant_dict_insert(&args, "username", "s", login->username);
	g_variant_dict_insert(&args, "password", "s", login->password);
	gtk_widget_grab_focus(GTK_WIDGET(t->view));
	webkit_web_view_call_async_javascript_function(t->view, fill_script, -1,
		g_variant_dict_end(&args), PASSWORD_WORLD, NULL, NULL, filled, NULL);
	g_free(host);
}

/* ---- The list of logins, when a site has several ---- */

static void paint_logins(struct browser *b, cairo_t *cr, int w, int h) {
	black(cr);
	fill(cr, 0, 0, w, 1);
	fill(cr, 0, 0, 1, h);
	fill(cr, w - 1, 0, 1, h);
	fill(cr, 0, h - 1, w, 1);
	for (guint i = 0; b->logins != NULL && i < b->logins->len; i++) {
		struct login *l = g_ptr_array_index(b->logins, i);
		int y = 1 + i * ROW_H;
		black(cr);
		if ((int)i == b->login_selected) {
			fill(cr, 1, y, w - 2, ROW_H);
			white(cr);
		}
		text(cr, l->username[0] != '\0' ? l->username : "(no username)",
			PAD, y, ROW_H);
		text(cr, l->name, w - PAD - text_width(cr, l->name), y, ROW_H);
	}
}

static void draw_logins(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_logins, data);
}

/* Drops the list down from the key gadget, like a menu. */
static void logins_show(struct browser *b, GPtrArray *logins) {
	int w = 0;
	b->logins = g_ptr_array_new();
	for (guint i = 0; i < logins->len; i++) {
		struct login *l = g_ptr_array_index(logins, i), *c = g_new(struct login, 1);
		c->name = g_strdup(l->name);
		c->username = g_strdup(l->username);
		c->password = g_strdup(l->password);
		g_ptr_array_add(b->logins, c);
		w = MAX(w, measure(c->username[0] ? c->username : "(no username)") +
			measure(c->name) + 4 * PAD);
	}
	b->login_selected = 0;
	gtk_widget_set_margin_top(b->logins_list,
		(gtk_widget_get_visible(b->tabbar) ? TAB_H : 0) + TOOL_H);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->logins_list),
		MAX(w, 240));
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->logins_list),
		b->logins->len * ROW_H + 2);
	gtk_widget_set_visible(b->logins_list, TRUE);
	gtk_widget_grab_focus(b->logins_list);
	gtk_widget_queue_draw(b->logins_list);
}

static void logins_choose(struct browser *b, int i) {
	if (b->logins != NULL && i >= 0 && i < (int)b->logins->len) {
		struct login *l = g_ptr_array_index(b->logins, i);
		fill_login(b, l);
	}
	logins_hide(b);
}

static int logins_row(struct browser *b, double y) {
	int row = ((int)y - 1) / ROW_H;
	return y >= 1 && b->logins != NULL && row < (int)b->logins->len ? row : -1;
}

static void logins_motion(GtkEventControllerMotion *motion, double x,
		double y, struct browser *b) {
	int row = logins_row(b, y);
	if (row >= 0 && row != b->login_selected) {
		b->login_selected = row;
		gtk_widget_queue_draw(b->logins_list);
	}
}

static void logins_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	int row = logins_row(b, y);
	if (row >= 0) {
		logins_choose(b, row);
	}
}

static gboolean logins_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	int n = b->logins != NULL ? (int)b->logins->len : 0;
	switch (keyval) {
	case GDK_KEY_Down:
		b->login_selected = (b->login_selected + 1) % MAX(n, 1);
		break;
	case GDK_KEY_Up:
		b->login_selected = (b->login_selected + n - 1) % MAX(n, 1);
		break;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		logins_choose(b, b->login_selected);
		return TRUE;
	case GDK_KEY_Escape:
		logins_hide(b);
		return TRUE;
	default:
		return FALSE;
	}
	gtk_widget_queue_draw(b->logins_list);
	return TRUE;
}

static void logins_focus_left(GtkEventControllerFocus *focus,
		struct browser *b) {
	logins_hide(b);
}

/* ---- Looking up and unlocking ---- */

static void lookup(struct browser *b);

static void found(enum passwords_result result, GPtrArray *logins,
		const char *error, void *data) {
	struct browser *b = data;
	b->looking_up = false;
	gtk_widget_queue_draw(b->key);
	if (result == PASSWORDS_LOCKED) {
		char *unlock = password_command("unlock");
		if (unlock != NULL) {
			show_notice(b, NULL);
			dialog_show(b, DIALOG_UNLOCK);
		} else {
			show_notice(b, g_strdup("Your passwords are locked"));
		}
		g_free(unlock);
	} else if (result == PASSWORDS_FAILED) {
		show_notice(b, g_strdup_printf("Passwords: %s", error));
	} else if (logins->len == 0) {
		show_notice(b, g_strdup_printf("No saved password for %s",
			b->login_host));
	} else if (logins->len == 1) {
		show_notice(b, NULL);
		fill_login(b, g_ptr_array_index(logins, 0));
	} else {
		show_notice(b, NULL);
		logins_show(b, logins);
	}
}

static void lookup(struct browser *b) {
	char *command = password_command("command");
	if (command == NULL) {
		return;
	}
	b->looking_up = true;
	gtk_widget_queue_draw(b->key);
	show_notice(b, g_strdup_printf("Looking up passwords for %s...",
		b->login_host));
	passwords_lookup(command, shared.password_session, b->login_host,
		b->password_cancel, found, b);
	g_free(command);
}

/* The key gadget: looks up the page's logins. */
static void fill_password(struct browser *b) {
	struct tab *t = active_tab(b);
	const char *why;
	char *host = t != NULL ? page_host(t, &why) : NULL;
	if (b->looking_up || b->unlocking) {
		g_free(host);
		return;
	}
	if (host == NULL) {
		show_notice(b, g_strdup(why));
		return;
	}
	g_free(b->login_host);
	b->login_host = host;
	lookup(b);
}

static void unlocked(const char *session, const char *error, void *data) {
	struct browser *b = data;
	b->unlocking = false;
	if (session == NULL) {
		dialog_error(b, error);
		gtk_widget_grab_focus(b->dialog_field);
		return;
	}
	secret_free(shared.password_session);
	shared.password_session = g_strdup(session);
	dialog_show(b, DIALOG_NONE);
	lookup(b);
}

static void unlock_submit(struct browser *b, char *password) {
	char *command = password_command("unlock");
	if (command == NULL) {
		secret_free(password);
		dialog_show(b, DIALOG_NONE);
		return;
	}
	b->unlocking = true;
	dialog_error(b, "Unlocking...");
	passwords_unlock(command, password, b->password_cancel, unlocked, b);
	g_free(command);
}

static void unlock_cancelled(struct browser *b) {
	show_notice(b, NULL);
}

static void lock_passwords(void) {
	g_clear_pointer(&shared.password_session, secret_free);
}

/* ---- Reader View ---------------------------------------------------------- */

/* View > Reader View shows just a page's article, found by Mozilla's
 * Readability.js (Firefox's Reader View) run on a copy of the page, in the
 * passwords' own JavaScript world. The article is shown as a page of its
 * own, GEM style, at the page's address, with scripts blocked; choosing
 * Reader View again goes back to the page. */

#define READER_WORLD PASSWORD_WORLD

/* Only for what's probably an article (Readability's own test, which
 * Firefox uses too): a home page's links aren't one. */
static const char reader_parse[] =
	"\n;(function () {"
	"  if (!isProbablyReaderable(document)) return null;"
	"  var a = new Readability(document.cloneNode(true)).parse();"
	"  return a && a.content ? { title: a.title || document.title,"
	"    byline: a.byline || '', site: a.siteName || location.hostname,"
	"    content: a.content, lang: a.lang || '', dir: a.dir || '' } : null;"
	"})();";

/* The reader's page: the article in a white box on the dithered desk,
 * headed in GemWM's font, the text in a book face. */
static const char reader_css[] =
	"html { background: repeating-conic-gradient(#000 0%% 25%%, #fff 0%% 50%%)"
	"  0 0 / 2px 2px; }"
	"body { margin: 0; padding: 24px 16px; }"
	"main { max-width: 38em; margin: 0 auto; background: #fff; color: #000;"
	"  border: 1px solid #000; box-shadow: 2px 2px 0 #000;"
	"  padding: 8px 40px 40px; font: 19px/1.6 serif; }"
	"header { font-family: '%s', monospace; font-size: %dpx;"
	"  border-bottom: 2px solid #000; margin: 0 -40px 24px; padding: 0 40px 12px; }"
	"header h1 { font: inherit; font-size: %dpx; line-height: 1.3;"
	"  margin: 12px 0 8px; }"
	"header .site, header .byline { margin: 0; }"
	"a { color: #000; }"
	"img, video, svg, iframe { max-width: 100%%; height: auto; }"
	"figure { margin: 1em 0; }"
	"figcaption { font-size: 15px; }"
	"pre, code { font: 15px monospace; }"
	"pre { overflow: auto; border: 1px solid #000; padding: 8px; }"
	"blockquote { margin: 1em 0; padding-left: 1em; border-left: 2px solid #000; }"
	"table { border-collapse: collapse; }"
	"td, th { border: 1px solid #000; padding: 2px 6px; }";

static char *js_string(JSCValue *object, const char *name) {
	JSCValue *v = jsc_value_object_get_property(object, name);
	char *s = jsc_value_is_string(v) ? jsc_value_to_string(v) : g_strdup("");
	g_object_unref(v);
	return s;
}

struct reading {
	WebKitWebView *view;
	char *uri; /* the page read */
};

static void reader_parsed(GObject *source, GAsyncResult *result,
		gpointer data) {
	struct reading *r = data;
	struct tab *t = g_object_get_data(G_OBJECT(r->view), "tab");
	GError *error = NULL;
	JSCValue *article = webkit_web_view_evaluate_javascript_finish(r->view,
		result, &error);
	/* Only if the tab's still there, on the page read. */
	if (t != NULL && g_strcmp0(webkit_web_view_get_uri(r->view), r->uri) == 0) {
		if (article == NULL || !jsc_value_is_object(article)) {
			show_notice(t->browser,
				g_strdup("No article found on this page"));
		} else {
			char *title = js_string(article, "title");
			char *byline = js_string(article, "byline");
			char *site = js_string(article, "site");
			char *content = js_string(article, "content");
			char *lang = js_string(article, "lang");
			char *dir = js_string(article, "dir");
			char *css = g_strdup_printf(reader_css, font_family,
				FONT_SIZE, FONT_SIZE * 2);
			char *head = g_markup_printf_escaped(
				"<!doctype html><html lang=\"%s\" dir=\"%s\"><head>"
				"<meta charset=\"utf-8\">"
				"<meta name=\"viewport\" content=\"width=device-width\">"
				"<meta http-equiv=\"Content-Security-Policy\" content=\""
				"default-src 'none'; img-src * data:; media-src *;"
				" style-src 'unsafe-inline'\">"
				"<title>%s</title>", lang, dir, title);
			char *top = g_markup_printf_escaped("<header><p class=\"site\">%s"
				"</p><h1>%s</h1><p class=\"byline\">%s</p></header>",
				site, title, byline);
			char *page = g_strconcat(head, "<style>", css,
				"</style></head><body><main>", top, content,
				"</main></body></html>", NULL);
			g_free(head);
			g_free(top);
			t->reader_loading = true;
			webkit_web_view_load_alternate_html(r->view, page, r->uri, r->uri);
			g_free(page);
			g_free(css);
			g_free(title);
			g_free(byline);
			g_free(site);
			g_free(content);
			g_free(lang);
			g_free(dir);
		}
	}
	g_clear_object(&article);
	g_clear_error(&error);
	g_object_unref(r->view);
	g_free(r->uri);
	g_free(r);
}

static bool can_read(struct tab *t) {
	const char *uri = t != NULL ? webkit_web_view_get_uri(t->view) : NULL;
	return uri != NULL && (g_str_has_prefix(uri, "https://") ||
		g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "file://"));
}

static void reader_toggle(struct browser *b) {
	struct tab *t = active_tab(b);
	if (t == NULL) {
		return;
	}
	if (t->reading) {
		/* Back to the page: the address never changed. */
		webkit_web_view_reload(t->view);
		return;
	}
	if (!can_read(t)) {
		return;
	}
	if (shared.readability == NULL) {
		GBytes *js = g_resources_lookup_data(
			"/org/gemwm/GemWeb/readability/Readability.js",
			G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
		GBytes *check = g_resources_lookup_data(
			"/org/gemwm/GemWeb/readability/Readability-readerable.js",
			G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
		shared.readability = g_strconcat(g_bytes_get_data(js, NULL), "\n;",
			g_bytes_get_data(check, NULL), reader_parse, NULL);
		g_bytes_unref(js);
		g_bytes_unref(check);
	}
	struct reading *r = g_new(struct reading, 1);
	r->view = g_object_ref(t->view);
	r->uri = g_strdup(webkit_web_view_get_uri(t->view));
	webkit_web_view_evaluate_javascript(t->view, shared.readability, -1,
		READER_WORLD, "gemweb:readability", NULL, reader_parsed, r);
}

/* ---- Finding in the page -------------------------------------------------- */

/* Ctrl+F opens a find bar along the bottom of the window: "Find:", the
 * text, how many matches, then gadgets for the previous and next match and
 * to close it. WebKit finds as you type, ignoring case, round the page. */

#define FIND_OPTIONS (WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | \
	WEBKIT_FIND_OPTIONS_WRAP_AROUND)
#define FIND_MAX 1000     /* matches counted; more says "1000+" */
#define FIND_LABEL_W 52   /* "Find:" */
#define FIND_COUNT_W 110  /* "1000+ matches", "Not found" */

static bool find_open(struct browser *b) {
	return gtk_widget_get_visible(b->findbar);
}

static const char *find_text(struct browser *b) {
	return gtk_editable_get_text(GTK_EDITABLE(b->find_entry));
}

static WebKitFindController *finder(struct tab *t) {
	return webkit_web_view_get_find_controller(t->view);
}

/* Finds the text afresh in the active tab: its first match, and a count. */
static void find_search(struct browser *b) {
	struct tab *t = active_tab(b);
	const char *text = find_text(b);
	b->find_matches = 0;
	b->find_failed = false;
	if (t != NULL && text[0] == '\0') {
		webkit_find_controller_search_finish(finder(t));
	} else if (t != NULL) {
		webkit_find_controller_search(finder(t), text, FIND_OPTIONS, FIND_MAX);
		webkit_find_controller_count_matches(finder(t), text, FIND_OPTIONS,
			FIND_MAX);
	}
	gtk_widget_queue_draw(b->find_info);
	app_menu_update(b->menu);
}

static void find_step(struct browser *b, bool back) {
	struct tab *t = active_tab(b);
	if (t == NULL || find_text(b)[0] == '\0') {
		return;
	}
	if (g_strcmp0(webkit_find_controller_get_search_text(finder(t)),
			find_text(b)) != 0) {
		find_search(b);
	} else if (back) {
		webkit_find_controller_search_previous(finder(t));
	} else {
		webkit_find_controller_search_next(finder(t));
	}
}

static void find_show(struct browser *b) {
	bool was_open = find_open(b);
	gtk_widget_set_visible(b->findbar, TRUE);
	gtk_widget_grab_focus(b->find_entry);
	gtk_editable_select_region(GTK_EDITABLE(b->find_entry), 0, -1);
	if (!was_open && find_text(b)[0] != '\0') {
		find_search(b);
	}
}

/* Closing takes the highlights away and gives the page the keyboard. */
static void find_close(struct browser *b) {
	struct tab *t = active_tab(b);
	gtk_widget_set_visible(b->findbar, FALSE);
	if (t != NULL) {
		webkit_find_controller_search_finish(finder(t));
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
	}
}

static void on_found(WebKitFindController *fc, guint count, struct tab *t) {
	if (is_active(t)) {
		t->browser->find_failed = false;
		gtk_widget_queue_draw(t->browser->find_info);
	}
}

static void on_not_found(WebKitFindController *fc, struct tab *t) {
	if (is_active(t)) {
		t->browser->find_failed = true;
		t->browser->find_matches = 0;
		gtk_widget_queue_draw(t->browser->find_info);
	}
}

static void on_counted(WebKitFindController *fc, guint count, struct tab *t) {
	if (is_active(t)) {
		t->browser->find_matches = count;
		gtk_widget_queue_draw(t->browser->find_info);
	}
}

static void find_connect(struct tab *t) {
	WebKitFindController *fc = finder(t);
	g_signal_connect(fc, "found-text", G_CALLBACK(on_found), t);
	g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_not_found), t);
	g_signal_connect(fc, "counted-matches", G_CALLBACK(on_counted), t);
}

static void paint_find_label(struct browser *b, cairo_t *cr, int w, int h) {
	black(cr);
	text(cr, "Find:", 6, 0, h);
}

static void draw_find_label(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_find_label, data);
}

/* The count ("Not found" inverted), then the previous, next and close
 * gadgets, each boxed off on its left. */
static void paint_find_info(struct browser *b, cairo_t *cr, int w, int h) {
	char count[32] = "";
	if (b->find_failed) {
		g_strlcpy(count, "Not found", sizeof(count));
	} else if (b->find_matches > 0) {
		snprintf(count, sizeof(count), "%u%s match%s", b->find_matches,
			b->find_matches >= FIND_MAX ? "+" : "",
			b->find_matches == 1 ? "" : "es");
	}
	black(cr);
	fill(cr, 0, 0, 1, h);
	if (count[0] != '\0') {
		if (b->find_failed) {
			fill(cr, 3, 2, text_width(cr, count) + 8, h - 4);
			white(cr);
		}
		text(cr, count, 7, 0, h);
		black(cr);
	}
	double cy = h / 2.0;
	int x = w - 3 * (GADGET + 1);
	for (int i = 0; i < 3; i++, x += GADGET + 1) {
		fill(cr, x, 0, 1, h);
		double c = x + 1 + GADGET / 2.0;
		if (i == 0) {
			cairo_move_to(cr, c, cy - 4);
			cairo_line_to(cr, c + 5, cy + 3);
			cairo_line_to(cr, c - 5, cy + 3);
		} else if (i == 1) {
			cairo_move_to(cr, c, cy + 4);
			cairo_line_to(cr, c + 5, cy - 3);
			cairo_line_to(cr, c - 5, cy - 3);
		} else {
			draw_closer(cr, x + 1, (h - GADGET) / 2.0);
			continue;
		}
		cairo_close_path(cr);
		cairo_fill(cr);
	}
}

static void draw_find_info(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_find_info, data);
}

static void find_info_pressed(GtkGestureClick *gesture, int n_press,
		double x, double y, struct browser *b) {
	int w = gtk_widget_get_width(b->find_info);
	int i = ((int)x - (w - 3 * (GADGET + 1))) / (GADGET + 1);
	if (x < w - 3 * (GADGET + 1)) {
		return;
	} else if (i == 0 || i == 1) {
		find_step(b, i == 0);
	} else {
		find_close(b);
	}
}

static void find_changed(GtkEditable *editable, struct browser *b) {
	find_search(b);
}

/* Return finds the next match, Shift+Return the one before; Escape
 * closes the bar. */
static gboolean find_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		find_step(b, state & GDK_SHIFT_MASK);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape) {
		find_close(b);
		return TRUE;
	}
	return FALSE;
}

/* The bar: built hidden, under the page. */
static GtkWidget *findbar_new(struct browser *b) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bar, "gem-findbar");
	GtkWidget *label = pixel_area(b, TOOL_H, draw_find_label, NULL);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(label), FIND_LABEL_W);
	b->find_entry = gtk_entry_new();
	gtk_widget_add_css_class(b->find_entry, "gem-url");
	gtk_widget_set_hexpand(b->find_entry, TRUE);
	g_signal_connect(b->find_entry, "changed", G_CALLBACK(find_changed), b);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(find_key), b);
	gtk_widget_add_controller(b->find_entry, keys);
	b->find_info = pixel_area(b, TOOL_H, draw_find_info,
		G_CALLBACK(find_info_pressed));
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->find_info),
		FIND_COUNT_W + 3 * (GADGET + 1));
	gtk_box_append(GTK_BOX(bar), label);
	gtk_box_append(GTK_BOX(bar), b->find_entry);
	gtk_box_append(GTK_BOX(bar), b->find_info);
	gtk_widget_set_visible(bar, FALSE);
	return bar;
}

/* ---- Ad blocking ---------------------------------------------------------- */

/* Ads and trackers are blocked by WebKit itself, with EasyList and
 * EasyPrivacy converted to its rules (adblock.c) and compiled into a store
 * in GemWeb's data folder; the lists are fetched afresh every few days.
 * [Ad Blocking] enabled turns it on or off, and [Ad Blocking Sites] holds
 * sites that differ; the View menu sets both. A tab has the filter on or
 * off as each page starts loading, by the page's site. */

#define ADS_ID "ads"
#define ADS_LISTS "https://easylist.to/easylist/easylist.txt;" \
	"https://easylist.to/easylist/easyprivacy.txt"
#define ADS_REFRESH (4 * 24 * 60 * 60) /* seconds */

/* A page's site, for its setting: its host, without "www.". */
static char *site_of(const char *uri) {
	GUri *u = uri != NULL ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
	char *site = NULL;
	if (u != NULL && g_uri_get_host(u) != NULL &&
			(strcmp(g_uri_get_scheme(u), "https") == 0 ||
			strcmp(g_uri_get_scheme(u), "http") == 0)) {
		const char *h = g_uri_get_host(u);
		site = g_ascii_strdown(g_str_has_prefix(h, "www.") ? h + 4 : h, -1);
	}
	if (u != NULL) {
		g_uri_unref(u);
	}
	return site;
}

static bool ads_enabled(void) {
	return setting_bool("Ad Blocking", "enabled", true);
}

/* Whether ads are blocked on site: its own setting, or the nearest
 * enclosing domain's (news.example.com, then example.com), or else the
 * setting for everywhere. from_parent skips the site's own. */
static bool ads_blocked_on(const char *site, bool from_parent) {
	/* The site, then each domain it's in, short of the last (com). */
	const char *d = site;
	for (bool skip = from_parent; d != NULL; skip = false) {
		const char *dot = strchr(d, '.');
		if (!skip) {
			char *v = setting("Ad Blocking Sites", d, NULL);
			if (v != NULL) {
				g_free(v);
				return setting_bool("Ad Blocking Sites", d, true);
			}
		}
		d = dot != NULL && strchr(dot + 1, '.') != NULL ? dot + 1 : NULL;
	}
	return ads_enabled();
}

/* Puts the filter on or takes it off for the tab's page. */
static void ads_apply(struct tab *t) {
	char *site = site_of(webkit_web_view_get_uri(t->view));
	WebKitUserContentFilter *want = shared.ads_filter != NULL &&
		site != NULL && ads_blocked_on(site, false) ? shared.ads_filter : NULL;
	g_free(site);
	if (want == t->ads) {
		return;
	}
	webkit_user_content_manager_remove_all_filters(t->content);
	if (want != NULL) {
		webkit_user_content_manager_add_filter(t->content, want);
	}
	t->ads = want;
}

static void ads_apply_all(void) {
	for (GList *w = gtk_application_get_windows(shared.app); w != NULL;
			w = w->next) {
		struct browser *b = g_object_get_data(G_OBJECT(w->data), "browser");
		for (guint i = 0; b != NULL && i < b->tabs->len; i++) {
			ads_apply(g_ptr_array_index(b->tabs, i));
		}
	}
}

static void ads_use(WebKitUserContentFilter *filter) {
	WebKitUserContentFilter *old = shared.ads_filter;
	shared.ads_filter = filter;
	ads_apply_all();
	if (old != NULL) {
		webkit_user_content_filter_unref(old);
	}
}

static char *ads_stamp(void) {
	return g_build_filename(shared.ads_dir, "updated", NULL);
}

struct ads_update {
	SoupSession *session;
	char **urls;
	int next, fetched;
	struct adblock *converter;
};

static void ads_fetch_next(struct ads_update *u);

static void ads_saved(GObject *source, GAsyncResult *result, gpointer data) {
	GError *error = NULL;
	WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(
		shared.ads_store, result, &error);
	shared.ads_updating = false;
	if (filter == NULL) {
		g_printerr("gemweb: ad blocking lists didn't compile: %s\n",
			error->message);
		g_error_free(error);
		return;
	}
	char *stamp = ads_stamp();
	g_file_set_contents(stamp, "", 0, NULL);
	g_free(stamp);
	ads_use(filter);
}

static void ads_fetched(GObject *source, GAsyncResult *result, gpointer data) {
	struct ads_update *u = data;
	GError *error = NULL;
	GBytes *body = soup_session_send_and_read_finish(u->session, result,
		&error);
	SoupMessage *msg = soup_session_get_async_result_message(u->session,
		result);
	const char *url = u->urls[u->next - 1];
	if (body == NULL) {
		g_printerr("gemweb: can't fetch %s: %s\n", url, error->message);
		g_error_free(error);
	} else if (soup_message_get_status(msg) != SOUP_STATUS_OK) {
		g_printerr("gemweb: can't fetch %s: %u\n", url,
			soup_message_get_status(msg));
	} else {
		gsize len;
		const char *data = g_bytes_get_data(body, &len);
		char *text = g_strndup(data, len);
		adblock_add_list(u->converter, text);
		g_free(text);
		u->fetched++;
	}
	g_clear_pointer(&body, g_bytes_unref);
	ads_fetch_next(u);
}

/* Fetches the lists one by one, then converts and compiles them. The old
 * filter stays until the new one's ready, or if it can't be made. */
static void ads_fetch_next(struct ads_update *u) {
	while (u->urls[u->next] != NULL) {
		SoupMessage *msg = soup_message_new("GET", g_strstrip(u->urls[u->next++]));
		if (msg != NULL) {
			soup_session_send_and_read_async(u->session, msg, G_PRIORITY_LOW,
				NULL, ads_fetched, u);
			g_object_unref(msg);
			return;
		}
	}
	guint n = 0;
	GBytes *rules = adblock_finish(u->converter, &n);
	if (u->fetched > 0) {
		webkit_user_content_filter_store_save(shared.ads_store, ADS_ID, rules,
			NULL, ads_saved, NULL);
	} else {
		shared.ads_updating = false;
	}
	g_bytes_unref(rules);
	g_object_unref(u->session);
	g_strfreev(u->urls);
	g_free(u);
}

static void ads_update(void) {
	if (shared.ads_updating) {
		return;
	}
	shared.ads_updating = true;
	char *lists = setting("Ad Blocking", "lists", NULL);
	struct ads_update *u = g_new0(struct ads_update, 1);
	u->session = soup_session_new_with_options("user-agent", "GemWeb", NULL);
	u->urls = g_strsplit(lists != NULL ? lists : ADS_LISTS, ";", -1);
	u->converter = adblock_new();
	g_free(lists);
	ads_fetch_next(u);
}

/* Lists more than a few days old (or never fetched) are fetched again. */
static bool ads_stale(void) {
	char *stamp = ads_stamp();
	GStatBuf st;
	bool stale = g_stat(stamp, &st) != 0 ||
		g_get_real_time() / G_USEC_PER_SEC - st.st_mtime > ADS_REFRESH;
	g_free(stamp);
	return stale;
}

static void ads_loaded(GObject *source, GAsyncResult *result, gpointer data) {
	WebKitUserContentFilter *filter = webkit_user_content_filter_store_load_finish(
		shared.ads_store, result, NULL);
	if (filter != NULL) {
		ads_use(filter);
	}
	if (ads_enabled() && (filter == NULL || ads_stale())) {
		ads_update();
	}
}

static void ads_init(const char *data) {
	shared.ads_dir = g_build_filename(data, "adblock", NULL);
	shared.ads_store = webkit_user_content_filter_store_new(shared.ads_dir);
	webkit_user_content_filter_store_load(shared.ads_store, ADS_ID, NULL,
		ads_loaded, NULL);
}

/* View > Block Ads: everywhere, then the page's site differently. Either
 * reloads the page to show the change. */
static void ads_toggle(struct browser *b, bool here) {
	struct tab *t = active_tab(b);
	char *site = t != NULL ? site_of(webkit_web_view_get_uri(t->view)) : NULL;
	if (!here) {
		setting_write("Ad Blocking", "enabled", ads_enabled() ? "false" : "true");
	} else if (site != NULL) {
		bool on = !ads_blocked_on(site, false);
		/* Same as it'd be anyway: no need to say so. */
		setting_write("Ad Blocking Sites", site,
			on == ads_blocked_on(site, true) ? NULL : on ? "true" : "false");
	}
	if (shared.ads_filter == NULL || (ads_enabled() && ads_stale())) {
		ads_update();
	}
	ads_apply_all();
	if (t != NULL && site != NULL) {
		webkit_web_view_reload(t->view);
	}
	g_free(site);
	app_menu_update(b->menu);
}

/* ---- Printing ------------------------------------------------------------- */

/* File > Print...: GemWM's print dialog, then WebKit prints the page with
 * what was chosen, to a printer or a PDF in Documents. */

static void print_failed(WebKitPrintOperation *op, GError *error,
		gpointer data) {
	g_object_set_data_full(G_OBJECT(op), "error", g_strdup(error->message),
		g_free);
}

static void print_finished(WebKitPrintOperation *op, gpointer data) {
	gem_print_job_done(data, g_object_get_data(G_OBJECT(op), "error"));
	g_object_unref(op);
}

static void print_run(GtkPrintSettings *settings, struct gem_print_job *job,
		void *data) {
	WebKitPrintOperation *op = webkit_print_operation_new(data);
	webkit_print_operation_set_print_settings(op, settings);
	g_signal_connect(op, "failed", G_CALLBACK(print_failed), job);
	g_signal_connect(op, "finished", G_CALLBACK(print_finished), job);
	webkit_print_operation_print(op);
}

/* What happened, in the info line, if the tab's still there. */
static void print_done(const char *message, void *data) {
	struct tab *t = g_object_get_data(G_OBJECT(data), "tab");
	if (t != NULL && message != NULL) {
		show_notice(t->browser, g_strdup(message));
	}
	g_object_unref(data);
}

static void print_page(struct browser *b) {
	struct tab *t = active_tab(b);
	if (t != NULL) {
		gem_print_dialog_run(GTK_WINDOW(b->window), tab_title(t), print_run,
			print_done, g_object_ref(t->view));
	}
}

/* ---- The right-click menu --------------------------------------------------- */

/* In place of WebKit's GTK menu, a GEM drop-down at the pointer, with what
 * fits what was clicked: a link, an image, a video, a text field, a
 * selection, or the page itself. */

enum ctx {
	CTX_SEPARATOR, CTX_OPEN_LINK_TAB, CTX_OPEN_LINK_WINDOW, CTX_COPY_LINK,
	CTX_DOWNLOAD_LINK, CTX_OPEN_IMAGE, CTX_SAVE_IMAGE, CTX_COPY_IMAGE,
	CTX_OPEN_MEDIA, CTX_COPY_MEDIA, CTX_UNDO, CTX_REDO, CTX_CUT, CTX_COPY,
	CTX_PASTE, CTX_SELECT_ALL, CTX_SEARCH, CTX_BACK, CTX_FORWARD, CTX_RELOAD,
	CTX_READER, CTX_PRINT, CTX_INSPECT,
};

struct ctx_item {
	enum ctx action;
	const char *label;
	bool enabled, checked;
};

#define CTX_PAD 16 /* left of a label, room for a check mark */

static void ctx_add(struct browser *b, enum ctx action, const char *label,
		bool enabled) {
	GArray *items = b->ctx_items;
	if (action == CTX_SEPARATOR && (items->len == 0 ||
			g_array_index(items, struct ctx_item, items->len - 1).action ==
			CTX_SEPARATOR)) {
		return;
	}
	struct ctx_item item = { action, label, enabled, false };
	g_array_append_val(items, item);
}

static void ctx_hide(struct browser *b) {
	gtk_widget_set_visible(b->ctx, FALSE);
}

static void paint_ctx(struct browser *b, cairo_t *cr, int w, int h) {
	static const char *grey_rows[] = { "#.", ".#" };
	cairo_pattern_t *grey = dither(grey_rows, 2, 2, true);
	black(cr);
	fill(cr, 0, 0, w, 1);
	fill(cr, 0, h - 1, w, 1);
	fill(cr, 0, 0, 1, h);
	fill(cr, w - 1, 0, 1, h);
	for (guint i = 0; i < b->ctx_items->len; i++) {
		struct ctx_item *item = &g_array_index(b->ctx_items, struct ctx_item, i);
		int y = 1 + i * ROW_H;
		if (item->action == CTX_SEPARATOR) {
			/* GEM's dotted rule. */
			cairo_save(cr);
			cairo_rectangle(cr, 1, y + ROW_H / 2, w - 2, 1);
			cairo_clip(cr);
			black(cr);
			cairo_mask(cr, grey);
			cairo_restore(cr);
			continue;
		}
		bool hover = item->enabled && (int)i == b->ctx_hover;
		if (hover) {
			black(cr);
			fill(cr, 1, y, w - 2, ROW_H);
		}
		cairo_push_group(cr);
		hover ? white(cr) : black(cr);
		text(cr, item->label, CTX_PAD, y, ROW_H);
		if (item->checked) {
			cairo_set_line_width(cr, 2);
			cairo_move_to(cr, 4, y + ROW_H / 2.0);
			cairo_line_to(cr, 7, y + ROW_H / 2.0 + 3);
			cairo_line_to(cr, 12, y + ROW_H / 2.0 - 4);
			cairo_stroke(cr);
		}
		cairo_pop_group_to_source(cr);
		if (item->enabled) {
			cairo_paint(cr);
		} else {
			cairo_mask(cr, grey);
		}
	}
	cairo_pattern_destroy(grey);
}

static void draw_ctx(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_ctx, data);
}

/* Opens at the pointer, kept inside the window: upward if there's no room
 * below. */
static void ctx_show(struct browser *b) {
	double w = 0;
	for (guint i = 0; i < b->ctx_items->len; i++) {
		struct ctx_item *item = &g_array_index(b->ctx_items, struct ctx_item, i);
		if (item->label != NULL) {
			w = MAX(w, measure(item->label));
		}
	}
	int mw = (int)w + CTX_PAD + 2 * PAD, mh = b->ctx_items->len * ROW_H + 2;
	int ow = gtk_widget_get_width(b->overlay);
	int oh = gtk_widget_get_height(b->overlay);
	int x = (int)b->pointer_x, y = (int)b->pointer_y;
	x = MAX(0, MIN(x, ow - mw));
	y = y + mh <= oh ? y : MAX(0, y - mh);
	gtk_widget_set_margin_start(b->ctx, x);
	gtk_widget_set_margin_top(b->ctx, y);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->ctx), mw);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->ctx), mh);
	b->ctx_hover = -1;
	gtk_widget_set_visible(b->ctx, TRUE);
	gtk_widget_queue_draw(b->ctx);
}

static gboolean on_context_menu(WebKitWebView *view, WebKitContextMenu *menu,
		WebKitHitTestResult *hit, struct tab *t) {
	struct browser *b = t->browser;
	if (dialog_up(b)) {
		return TRUE;
	}
	g_array_set_size(b->ctx_items, 0);
	set_status(&b->ctx_link, webkit_hit_test_result_context_is_link(hit) ?
		g_strdup(webkit_hit_test_result_get_link_uri(hit)) : NULL);
	set_status(&b->ctx_image, webkit_hit_test_result_context_is_image(hit) ?
		g_strdup(webkit_hit_test_result_get_image_uri(hit)) : NULL);
	set_status(&b->ctx_media, webkit_hit_test_result_context_is_media(hit) ?
		g_strdup(webkit_hit_test_result_get_media_uri(hit)) : NULL);
	bool editable = webkit_hit_test_result_context_is_editable(hit);
	bool selection = webkit_hit_test_result_context_is_selection(hit);

	if (b->ctx_link != NULL) {
		ctx_add(b, CTX_OPEN_LINK_TAB, b->app ? "Open Link in Browser" :
			"Open Link in New Tab", true);
		ctx_add(b, CTX_OPEN_LINK_WINDOW, "Open Link in New Window", true);
		ctx_add(b, CTX_COPY_LINK, "Copy Link Address", true);
		ctx_add(b, CTX_DOWNLOAD_LINK, "Download Link", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	}
	if (b->ctx_image != NULL) {
		ctx_add(b, CTX_OPEN_IMAGE, "Open Image in New Tab", true);
		ctx_add(b, CTX_SAVE_IMAGE, "Save Image", true);
		ctx_add(b, CTX_COPY_IMAGE, "Copy Image Address", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	}
	if (b->ctx_media != NULL) {
		ctx_add(b, CTX_OPEN_MEDIA, "Open Video in New Tab", true);
		ctx_add(b, CTX_COPY_MEDIA, "Copy Video Address", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	}
	if (editable) {
		ctx_add(b, CTX_UNDO, "Undo", true);
		ctx_add(b, CTX_REDO, "Redo", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
		ctx_add(b, CTX_CUT, "Cut", true);
		ctx_add(b, CTX_COPY, "Copy", true);
		ctx_add(b, CTX_PASTE, "Paste", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
		ctx_add(b, CTX_SELECT_ALL, "Select All", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	} else if (selection) {
		ctx_add(b, CTX_COPY, "Copy", true);
		ctx_add(b, CTX_SEARCH, "Search the Web for It", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	}
	if (b->ctx_items->len == 0) {
		ctx_add(b, CTX_BACK, "Back", webkit_web_view_can_go_back(view));
		ctx_add(b, CTX_FORWARD, "Forward", webkit_web_view_can_go_forward(view));
		ctx_add(b, CTX_RELOAD, "Reload", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
		ctx_add(b, CTX_READER, "Reader View", t->reading || can_read(t));
		g_array_index(b->ctx_items, struct ctx_item, b->ctx_items->len - 1)
			.checked = t->reading;
		ctx_add(b, CTX_PRINT, "Print...", true);
		ctx_add(b, CTX_SEPARATOR, NULL, false);
	}
	ctx_add(b, CTX_INSPECT, "Inspect Page", true);
	ctx_show(b);
	return TRUE; /* not WebKit's */
}

/* A link opened elsewhere: a new tab behind this one, or from an app, the
 * browser. */
static void open_link(struct browser *b, const char *uri) {
	if (b->app) {
		open_in_browser(uri);
	} else {
		tab_new(b, NULL, uri, false);
	}
}

static void ctx_copy(struct browser *b, const char *text) {
	gdk_clipboard_set_text(gtk_widget_get_clipboard(b->window), text);
}

static void searched(GObject *source, GAsyncResult *result, gpointer data) {
	WebKitWebView *view = WEBKIT_WEB_VIEW(source);
	struct tab *t = g_object_get_data(G_OBJECT(view), "tab");
	JSCValue *v = webkit_web_view_evaluate_javascript_finish(view, result, NULL);
	if (t != NULL && v != NULL && jsc_value_is_string(v)) {
		char *text = jsc_value_to_string(v);
		char *engine = search_engine();
		char *q = g_uri_escape_string(g_strstrip(text), NULL, TRUE);
		const char *at = strstr(engine, "%s");
		char *uri = g_strdup_printf("%.*s%s%s", (int)(at - engine), engine,
			q, at + 2);
		if (text[0] != '\0') {
			open_link(t->browser, uri);
		}
		g_free(uri);
		g_free(q);
		g_free(engine);
		g_free(text);
	}
	g_clear_object(&v);
}

static void ctx_run(struct browser *b, int index) {
	struct tab *t = active_tab(b);
	if (t == NULL || index < 0 || index >= (int)b->ctx_items->len) {
		return;
	}
	struct ctx_item *item = &g_array_index(b->ctx_items, struct ctx_item, index);
	if (!item->enabled) {
		return;
	}
	enum ctx action = item->action;
	ctx_hide(b);
	gtk_widget_grab_focus(GTK_WIDGET(t->view));
	WebKitWebView *view = t->view;
	switch (action) {
	case CTX_OPEN_LINK_TAB:
		open_link(b, b->ctx_link);
		break;
	case CTX_OPEN_LINK_WINDOW: {
		struct browser *nb = browser_new(shared.app);
		tab_new(nb, NULL, b->ctx_link, true);
		gtk_window_present(GTK_WINDOW(nb->window));
		break;
	}
	case CTX_COPY_LINK:
		ctx_copy(b, b->ctx_link);
		break;
	case CTX_DOWNLOAD_LINK:
		webkit_web_view_download_uri(view, b->ctx_link);
		break;
	case CTX_OPEN_IMAGE:
		open_link(b, b->ctx_image);
		break;
	case CTX_SAVE_IMAGE:
		webkit_web_view_download_uri(view, b->ctx_image);
		break;
	case CTX_COPY_IMAGE:
		ctx_copy(b, b->ctx_image);
		break;
	case CTX_OPEN_MEDIA:
		open_link(b, b->ctx_media);
		break;
	case CTX_COPY_MEDIA:
		ctx_copy(b, b->ctx_media);
		break;
	case CTX_UNDO:
		webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_UNDO);
		break;
	case CTX_REDO:
		webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_REDO);
		break;
	case CTX_CUT:
		webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_CUT);
		break;
	case CTX_COPY:
		webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_COPY);
		break;
	case CTX_PASTE:
		webkit_web_view_execute_editing_command(view,
			WEBKIT_EDITING_COMMAND_PASTE);
		break;
	case CTX_SELECT_ALL:
		webkit_web_view_execute_editing_command(view,
			WEBKIT_EDITING_COMMAND_SELECT_ALL);
		break;
	case CTX_SEARCH:
		webkit_web_view_evaluate_javascript(view,
			"window.getSelection().toString()", -1, PASSWORD_WORLD, NULL, NULL,
			searched, NULL);
		break;
	case CTX_BACK:
		webkit_web_view_go_back(view);
		break;
	case CTX_FORWARD:
		webkit_web_view_go_forward(view);
		break;
	case CTX_RELOAD:
		webkit_web_view_reload(view);
		break;
	case CTX_READER:
		reader_toggle(b);
		break;
	case CTX_PRINT:
		print_page(b);
		break;
	case CTX_INSPECT:
		webkit_web_inspector_show(webkit_web_view_get_inspector(view));
		break;
	case CTX_SEPARATOR:
		break;
	}
}

static int ctx_row(struct browser *b, double y) {
	int row = ((int)y - 1) / ROW_H;
	return y >= 1 && row < (int)b->ctx_items->len ? row : -1;
}

static void ctx_motion(GtkEventControllerMotion *motion, double x, double y,
		struct browser *b) {
	int row = ctx_row(b, y);
	if (row != b->ctx_hover) {
		b->ctx_hover = row;
		gtk_widget_queue_draw(b->ctx);
	}
}

static void ctx_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	ctx_run(b, ctx_row(b, y));
}

/* Up and Down go through the items that can be chosen, Return chooses,
 * Escape closes. */
static gboolean ctx_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	int n = b->ctx_items->len;
	if (keyval == GDK_KEY_Escape) {
		ctx_hide(b);
		return TRUE;
	}
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		ctx_run(b, b->ctx_hover);
		return TRUE;
	}
	if (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down) {
		return FALSE;
	}
	int step = keyval == GDK_KEY_Down ? 1 : -1;
	int i = b->ctx_hover < 0 ? (step > 0 ? -1 : n) : b->ctx_hover;
	for (int tries = 0; tries < n; tries++) {
		i = (i + step + n) % n;
		struct ctx_item *item = &g_array_index(b->ctx_items, struct ctx_item, i);
		if (item->action != CTX_SEPARATOR && item->enabled) {
			b->ctx_hover = i;
			break;
		}
	}
	gtk_widget_queue_draw(b->ctx);
	return TRUE;
}

/* A click anywhere else closes it (and goes on to what it was on). The
 * page keeps the keyboard focus, so it can't close on losing that. */
static void ctx_click_away(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	graphene_rect_t r;
	if (gtk_widget_get_visible(b->ctx) &&
			gtk_widget_compute_bounds(b->ctx, b->overlay, &r) &&
			!graphene_rect_contains_point(&r, &GRAPHENE_POINT_INIT(x, y))) {
		ctx_hide(b);
	}
}

/* While it's open, the window's keys are its: any but its own close it. */
static gboolean ctx_window_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	if (!gtk_widget_get_visible(b->ctx)) {
		return FALSE;
	}
	if (ctx_key(ctrl, keyval, keycode, state, b)) {
		return TRUE;
	}
	ctx_hide(b);
	return FALSE;
}

/* Where the pointer is, for where the menu opens. */
static void pointer_moved(GtkEventControllerMotion *motion, double x,
		double y, struct browser *b) {
	b->pointer_x = x;
	b->pointer_y = y;
}

static GtkWidget *ctx_new(struct browser *b) {
	b->ctx_items = g_array_new(FALSE, TRUE, sizeof(struct ctx_item));
	GtkWidget *ctx = pixel_area(b, 0, draw_ctx, G_CALLBACK(ctx_pressed));
	gtk_widget_set_halign(ctx, GTK_ALIGN_START);
	gtk_widget_set_valign(ctx, GTK_ALIGN_START);
	gtk_widget_set_visible(ctx, FALSE);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(ctx_motion), b);
	gtk_widget_add_controller(ctx, motion);
	return ctx;
}

/* ---- Fullscreen ----------------------------------------------------------- */

/* A page going fullscreen (a video, say) gets the whole window, which
 * WebKit then makes fullscreen: the tabs, toolbar, info line and find bar
 * hide until it leaves. */
static gboolean on_enter_fullscreen(WebKitWebView *view, struct tab *t) {
	struct browser *b = t->browser;
	b->fullscreen = true;
	b->find_was_open = find_open(b);
	list_hide(b);
	logins_hide(b);
	gtk_widget_set_visible(b->tabbar, FALSE);
	gtk_widget_set_visible(b->toolbar, FALSE);
	gtk_widget_set_visible(b->info, FALSE);
	gtk_widget_set_visible(b->findbar, FALSE);
	return FALSE;
}

static gboolean on_leave_fullscreen(WebKitWebView *view, struct tab *t) {
	struct browser *b = t->browser;
	b->fullscreen = false;
	sync_tabbar(b);
	gtk_widget_set_visible(b->toolbar, !b->app);
	gtk_widget_set_visible(b->info, !b->app);
	sync_info(b);
	gtk_widget_set_visible(b->findbar, b->find_was_open);
	return FALSE;
}

/* A key: a ring and a toothed shaft; inverted while looking up. */
static void paint_key(struct browser *b, cairo_t *cr, int w, int h) {
	double cy = h / 2.0;
	black(cr);
	fill(cr, 0, 0, 1, h);
	if (b->looking_up) {
		fill(cr, 1, 0, w - 1, h);
		white(cr);
	}
	cairo_set_line_width(cr, 2);
	cairo_arc(cr, 7, cy, 3, 0, 2 * G_PI);
	cairo_stroke(cr);
	fill(cr, 10, (int)cy - 1, 7, 2);
	fill(cr, 13, (int)cy + 1, 2, 2);
	fill(cr, 16, (int)cy + 1, 1, 3);
}

static void draw_key(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_key, data);
}

static void key_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	fill_password(b);
}

static gboolean shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
	struct browser *b = g_object_get_data(G_OBJECT(widget), "browser");
	struct tab *t = active_tab(b);
	int n = b->tabs->len;
	if (dialog_up(b) && GPOINTER_TO_INT(data) != ACT_QUIT) {
		return TRUE;
	}
	/* An app's window has one page, and no address field. */
	if (b->app && (GPOINTER_TO_INT(data) == ACT_NEW_TAB ||
			GPOINTER_TO_INT(data) == ACT_FOCUS_URL ||
			GPOINTER_TO_INT(data) == ACT_NEXT_TAB ||
			GPOINTER_TO_INT(data) == ACT_PREV_TAB)) {
		return TRUE;
	}
	switch (GPOINTER_TO_INT(data)) {
	case ACT_NEW_WINDOW: {
		struct browser *nb = browser_new(shared.app);
		tab_new(nb, NULL, NULL, true);
		gtk_window_present(GTK_WINDOW(nb->window));
		break;
	}
	case ACT_NEW_TAB:
		tab_new(b, NULL, NULL, true);
		break;
	case ACT_CLOSE_TAB:
		tab_close(b, b->active);
		break;
	case ACT_FOCUS_URL:
		gtk_widget_grab_focus(b->entry);
		gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
		break;
	case ACT_NEXT_TAB:
		tab_select(b, (b->active + 1) % n);
		break;
	case ACT_PREV_TAB:
		tab_select(b, (b->active + n - 1) % n);
		break;
	case ACT_RELOAD:
		webkit_web_view_reload(t->view);
		break;
	case ACT_BACK:
		webkit_web_view_go_back(t->view);
		break;
	case ACT_FORWARD:
		webkit_web_view_go_forward(t->view);
		break;
	case ACT_ZOOM_IN:
		webkit_web_view_set_zoom_level(t->view,
			webkit_web_view_get_zoom_level(t->view) * 1.1);
		break;
	case ACT_ZOOM_OUT:
		webkit_web_view_set_zoom_level(t->view,
			webkit_web_view_get_zoom_level(t->view) / 1.1);
		break;
	case ACT_ZOOM_RESET:
		webkit_web_view_set_zoom_level(t->view, 1.0);
		break;
	case ACT_HOME:
		load_home(t);
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
		break;
	case ACT_QUIT:
		gtk_window_destroy(GTK_WINDOW(b->window));
		break;
	case ACT_CLEAR_HISTORY:
		dialog_show(b, DIALOG_CLEAR_HISTORY);
		break;
	case ACT_FILL_PASSWORD:
		fill_password(b);
		break;
	case ACT_READER:
		reader_toggle(b);
		break;
	case ACT_PRINT:
		print_page(b);
		break;
	case ACT_OPEN_IN_BROWSER:
		open_in_browser(webkit_web_view_get_uri(t->view));
		break;
	case ACT_BLOCK_ADS:
	case ACT_BLOCK_ADS_SITE:
		ads_toggle(b, GPOINTER_TO_INT(data) == ACT_BLOCK_ADS_SITE);
		break;
	case ACT_FIND:
		find_show(b);
		break;
	case ACT_FIND_NEXT:
	case ACT_FIND_PREV:
		if (!find_open(b)) {
			find_show(b);
		}
		find_step(b, GPOINTER_TO_INT(data) == ACT_FIND_PREV);
		break;
	case ACT_LOCK_PASSWORDS:
		lock_passwords();
		show_notice(b, g_strdup("Passwords locked"));
		break;
	}
	return TRUE;
}

/* The menus in GemWM's menu bar. The shortcuts shown are GEM style: ^T is
 * Ctrl+T. */
static void build_menus(struct app_menu *m, void *data) {
	struct browser *b = data;
	struct tab *t = active_tab(b);
	bool back = t != NULL && webkit_web_view_can_go_back(t->view);
	bool forward = t != NULL && webkit_web_view_can_go_forward(t->view);
	uint32_t one_tab = b->tabs->len < 2 ? APP_MENU_DISABLED : 0;

	app_menu_add_menu(m, "File");
	if (b->app) {
		app_menu_add_item(m, ACT_OPEN_IN_BROWSER, "Open in Browser", NULL,
			t != NULL ? 0 : APP_MENU_DISABLED);
	} else {
		app_menu_add_item(m, ACT_NEW_WINDOW, "New Window", "^N", 0);
		app_menu_add_item(m, ACT_NEW_TAB, "New Tab", "^T", 0);
		app_menu_add_item(m, ACT_FOCUS_URL, "Open Location...", "^L", 0);
	}
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_PRINT, "Print...", "^P",
		t != NULL ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	if (!b->app) {
		app_menu_add_item(m, ACT_CLOSE_TAB, "Close Tab", "^W", 0);
	}
	app_menu_add_item(m, ACT_QUIT, "Close Window", "^Q", 0);

	app_menu_add_menu(m, "View");
	app_menu_add_item(m, ACT_RELOAD, "Reload", "^R", 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_READER, "Reader View", "^Alt+R",
		t != NULL && t->reading ? APP_MENU_CHECKED :
		can_read(t) ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	char *site = t != NULL ? site_of(webkit_web_view_get_uri(t->view)) : NULL;
	char *here = g_strdup_printf("Block Ads on %s", site ? site : "This Site");
	app_menu_add_item(m, ACT_BLOCK_ADS, "Block Ads", NULL,
		ads_enabled() ? APP_MENU_CHECKED : 0);
	app_menu_add_item(m, ACT_BLOCK_ADS_SITE, here, NULL, site == NULL ?
		APP_MENU_DISABLED : ads_blocked_on(site, false) ? APP_MENU_CHECKED : 0);
	g_free(here);
	g_free(site);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_FIND, "Find...", "^F", 0);
	app_menu_add_item(m, ACT_FIND_NEXT, "Find Again", "^G",
		find_text(b)[0] != '\0' ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_ZOOM_IN, "Zoom In", "^+", 0);
	app_menu_add_item(m, ACT_ZOOM_OUT, "Zoom Out", "^-", 0);
	app_menu_add_item(m, ACT_ZOOM_RESET, "Actual Size", "^0", 0);

	app_menu_add_menu(m, "Go");
	app_menu_add_item(m, ACT_BACK, "Back", "Alt+Left",
		back ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_FORWARD, "Forward", "Alt+Right",
		forward ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_HOME, "Home", "Alt+Home", 0);
	app_menu_add_separator(m);
	if (!b->app) {
		app_menu_add_item(m, ACT_NEXT_TAB, "Next Tab", "^Tab", one_tab);
		app_menu_add_item(m, ACT_PREV_TAB, "Previous Tab", "^Shift+Tab",
			one_tab);
		app_menu_add_separator(m);
	}
	app_menu_add_item(m, ACT_CLEAR_HISTORY, "Clear History...", NULL,
		dialog_up(b) ? APP_MENU_DISABLED : 0);
	if (shared.passwords) {
		app_menu_add_separator(m);
		app_menu_add_item(m, ACT_FILL_PASSWORD, "Fill Password", NULL,
			gtk_widget_get_visible(b->key) ? 0 : APP_MENU_DISABLED);
		app_menu_add_item(m, ACT_LOCK_PASSWORDS, "Lock Passwords", NULL,
			shared.password_session != NULL ? 0 : APP_MENU_DISABLED);
	}
}

static void menu_activate(uint32_t id, void *data) {
	struct browser *b = data;
	if (dialog_up(b) && id != ACT_QUIT) {
		return;
	}
	if (active_tab(b) != NULL || id == ACT_NEW_WINDOW || id == ACT_NEW_TAB ||
			id == ACT_QUIT || id == ACT_CLEAR_HISTORY ||
			id == ACT_LOCK_PASSWORDS) {
		shortcut(b->window, NULL, GINT_TO_POINTER(id));
	}
}

static void add_shortcuts(GtkWidget *window) {
	static const struct { const char *trigger; enum action action; } keys[] = {
		{ "<Control>n", ACT_NEW_WINDOW },
		{ "<Control>t", ACT_NEW_TAB },
		{ "<Control>w", ACT_CLOSE_TAB },
		{ "<Control>l", ACT_FOCUS_URL },
		{ "F6", ACT_FOCUS_URL },
		{ "<Control>Tab", ACT_NEXT_TAB },
		{ "<Control>Page_Down", ACT_NEXT_TAB },
		{ "<Control><Shift>ISO_Left_Tab", ACT_PREV_TAB },
		{ "<Control><Shift>Tab", ACT_PREV_TAB },
		{ "<Control>Page_Up", ACT_PREV_TAB },
		{ "<Control>r", ACT_RELOAD },
		{ "<Control><Alt>r", ACT_READER },
		{ "<Control>f", ACT_FIND },
		{ "<Control>p", ACT_PRINT },
		{ "<Control>g", ACT_FIND_NEXT },
		{ "F3", ACT_FIND_NEXT },
		{ "<Control><Shift>g", ACT_FIND_PREV },
		{ "<Shift>F3", ACT_FIND_PREV },
		{ "F5", ACT_RELOAD },
		{ "<Alt>Left", ACT_BACK },
		{ "<Alt>Right", ACT_FORWARD },
		{ "<Alt>Home", ACT_HOME },
		{ "<Control>plus", ACT_ZOOM_IN },
		{ "<Control>equal", ACT_ZOOM_IN },
		{ "<Control>minus", ACT_ZOOM_OUT },
		{ "<Control>0", ACT_ZOOM_RESET },
		{ "<Control>q", ACT_QUIT },
	};
	GtkEventController *ctrl = gtk_shortcut_controller_new();
	/* Capture phase: ours win over the page's own key handling. */
	gtk_event_controller_set_propagation_phase(ctrl, GTK_PHASE_CAPTURE);
	for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
		gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(ctrl),
			gtk_shortcut_new(gtk_shortcut_trigger_parse_string(keys[i].trigger),
				gtk_callback_action_new(shortcut,
					GINT_TO_POINTER(keys[i].action), NULL)));
	}
	gtk_widget_add_controller(window, ctrl);
}

/* ---- Window ------------------------------------------------------------- */

static GtkWidget *pixel_area(struct browser *b, int height,
		GtkDrawingAreaDrawFunc draw, GCallback pressed) {
	GtkWidget *area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), height);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, b, NULL);
	if (pressed != NULL) {
		GtkGesture *click = gtk_gesture_click_new();
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
		g_signal_connect(click, "pressed", pressed, b);
		gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	}
	return area;
}

static void load_css(void) {
	char *css = g_strdup_printf(
		"window.gemweb { background: #fff; }"
		".gem-toolbar { background: #fff; border-bottom: 1px solid #000; }"
		"entry.gem-url { background: #fff; color: #000; border: none;"
		"  border-left: 1px solid #000; border-radius: 0; box-shadow: none;"
		"  outline: none; min-height: %dpx; margin: 0; padding: 0 6px;"
		"  font-family: \"%s\"; font-size: %dpx; caret-color: #000; }"
		"entry.gem-url:focus-within { box-shadow: none; outline: none; }"
		"entry.gem-url text selection { background: #000; color: #fff; }"
		".gem-findbar { background: #fff; border-top: 1px solid #000; }"
		".gem-field { background: #fff; color: #000; border: 1px solid #000;"
		"  border-radius: 0; box-shadow: none; outline: none; min-height: %dpx;"
		"  margin: 0; padding: 0 6px; font-family: \"%s\"; font-size: %dpx;"
		"  caret-color: #000; }"
		".gem-field:focus-within { box-shadow: none; outline: none; }"
		".gem-field text selection { background: #000; color: #fff; }",
		TOOL_H, font_family, FONT_SIZE, FIELD_H - 2, font_family, FONT_SIZE);
	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(provider);
	g_free(css);
}

static void window_destroyed(GtkWidget *window, struct browser *b) {
	if (b->scroll != NULL) {
		gemwm_scroll_v1_destroy(b->scroll);
	}
	for (guint i = 0; i < b->tabs->len; i++) {
		struct tab *t = g_ptr_array_index(b->tabs, i);
		if (t->record_timer != 0) {
			g_source_remove(t->record_timer);
			t->record_timer = 0;
		}
	}
	if (b->complete_idle != 0) {
		g_source_remove(b->complete_idle);
	}
	/* A lookup or unlock still running is stopped, not answered. */
	g_cancellable_cancel(b->password_cancel);
	g_object_unref(b->password_cancel);
	g_clear_pointer(&b->logins, logins_free);
	g_free(b->login_host);
	g_free(b->notice);
	g_free(b->dialog_error);
	/* A page still waiting for an answer gets "no". */
	script_answer(b, false);
	g_ptr_array_unref(b->dialog_lines);
	g_array_unref(b->ctx_items);
	g_free(b->ctx_link);
	g_free(b->ctx_image);
	g_free(b->ctx_media);
	g_free(b->app_uri);
	g_free(b->app_name);
	g_ptr_array_free(b->tabs, TRUE);
	g_clear_pointer(&b->suggestions, history_entries_free);
	g_free(b->typed);
	g_free(b->completion);
	g_free(b->completion_uri);
	g_free(b->hover_link);
	g_free(b->download_status);
	g_free(b);
}

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, gemwm_scroll_manager_v1_interface.name) == 0) {
		shared.scroll_manager = wl_registry_bind(registry, name,
			&gemwm_scroll_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* Asks the compositor, over GTK's own Wayland connection, whether it draws
 * scroll bars for us (GemWM does). */
static void find_scroll_manager(void) {
	GdkDisplay *display = gdk_display_get_default();
	if (!GDK_IS_WAYLAND_DISPLAY(display)) {
		return;
	}
	struct wl_display *wl = gdk_wayland_display_get_wl_display(display);
	struct wl_registry *registry = wl_display_get_registry(wl);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(wl);
	wl_registry_destroy(registry);
}

static void shared_init(GtkApplication *app) {
	shared.app = app;
	char *data = g_build_filename(g_get_user_data_dir(), "gemweb", NULL);
	char *cache = g_build_filename(g_get_user_cache_dir(), "gemweb", NULL);
	g_mkdir_with_parents(data, 0700);
	shared.session = webkit_network_session_new(data, cache);
	ads_init(data);
	char *cookies;
	cookies_prepare(data, &cookies);
	webkit_cookie_manager_set_persistent_storage(
		webkit_network_session_get_cookie_manager(shared.session),
		cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
	g_signal_connect(shared.session, "download-started",
		G_CALLBACK(on_download_started), NULL);
	g_free(cookies);
	char *history = g_build_filename(data, "history.sqlite", NULL);
	history_open(history);
	g_free(history);
	g_free(data);
	g_free(cache);

	find_scroll_manager();
	char *css = g_strconcat(page_css,
		shared.scroll_manager != NULL ? frame_scroll_css : "", NULL);
	shared.style = webkit_user_style_sheet_new(css,
		WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER,
		NULL, NULL);
	g_free(css);
	shared.scroll_script = webkit_user_script_new(scroll_script,
		WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);

	shared.media_missing = find_missing_media();
	char *note = NULL;
	if (shared.media_missing != NULL) {
		g_printerr("gemweb: video and sound won't play; install %s\n",
			shared.media_missing);
		shared.media_script = webkit_user_script_new(media_script,
			WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
		note = g_strdup_printf("<p>Video and sound won't play until<br>"
			"%s %s installed.</p>", shared.media_missing,
			strchr(shared.media_missing, ',') ? "are" : "is");
	}
	shared.start_page = g_strconcat(start_page, note ? note : "",
		start_page_end, NULL);
	g_free(note);

	char *passwords = password_command("command");
	shared.passwords = passwords != NULL;
	g_free(passwords);
	if (shared.passwords) {
		shared.login_script = webkit_user_script_new_for_world(login_script,
			WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, PASSWORD_WORLD,
			NULL, NULL);
	}

	shared.settings = webkit_settings_new();
	webkit_settings_set_enable_developer_extras(shared.settings, TRUE);
}

static struct browser *browser_new(GtkApplication *app) {
	struct browser *b = g_new0(struct browser, 1);
	b->tabs = g_ptr_array_new_with_free_func(NULL);
	b->active = -1;

	b->window = gtk_application_window_new(app);
	/* GemWM draws the title bar and frame. */
	gtk_window_set_decorated(GTK_WINDOW(b->window), FALSE);
	gtk_window_set_default_size(GTK_WINDOW(b->window), 1000, 700);
	gtk_widget_add_css_class(b->window, "gemweb");
	g_object_set_data(G_OBJECT(b->window), "browser", b);
	g_signal_connect(b->window, "destroy", G_CALLBACK(window_destroyed), b);
	g_signal_connect(b->window, "map", G_CALLBACK(window_mapped), b);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	b->box = box;
	b->selected = -1;
	b->tabbar = pixel_area(b, TAB_H, draw_tabbar, G_CALLBACK(tabbar_pressed));

	GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	b->toolbar = toolbar;
	gtk_widget_add_css_class(toolbar, "gem-toolbar");
	b->buttons = pixel_area(b, TOOL_H, draw_buttons, G_CALLBACK(buttons_pressed));
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->buttons),
		BUTTONS * (GADGET + 1) - 1);
	b->entry = gtk_entry_new();
	gtk_widget_add_css_class(b->entry, "gem-url");
	gtk_widget_set_hexpand(b->entry, TRUE);
	g_signal_connect(b->entry, "activate", G_CALLBACK(entry_activate), b);
	g_signal_connect(b->entry, "changed", G_CALLBACK(entry_changed), b);
	GtkEditable *text = gtk_editable_get_delegate(GTK_EDITABLE(b->entry));
	g_signal_connect(text, "insert-text", G_CALLBACK(entry_inserted), b);
	g_signal_connect(text, "delete-text", G_CALLBACK(entry_deleted), b);
	GtkEventController *keys = gtk_event_controller_key_new();
	/* Before the entry's own keys: Up and Down move it to the start and
	 * end otherwise. */
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(entry_key), b);
	gtk_widget_add_controller(b->entry, keys);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(entry_focus_left), b);
	gtk_widget_add_controller(b->entry, focus);
	b->key = pixel_area(b, TOOL_H, draw_key, G_CALLBACK(key_pressed));
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->key), GADGET + 1);
	gtk_widget_set_tooltip_text(b->key, "Fill Password");
	gtk_widget_set_visible(b->key, FALSE);
	gtk_box_append(GTK_BOX(toolbar), b->buttons);
	gtk_box_append(GTK_BOX(toolbar), b->entry);
	gtk_box_append(GTK_BOX(toolbar), b->key);

	b->info = pixel_area(b, INFO_H, draw_info, NULL);
	b->stack = gtk_stack_new();
	gtk_widget_set_vexpand(b->stack, TRUE);

	gtk_box_append(GTK_BOX(box), b->tabbar);
	gtk_box_append(GTK_BOX(box), toolbar);
	gtk_box_append(GTK_BOX(box), b->info);
	gtk_box_append(GTK_BOX(box), b->stack);
	b->findbar = findbar_new(b);
	gtk_box_append(GTK_BOX(box), b->findbar);

	/* Over it all: the address field's list, and the alert. */
	GtkWidget *overlay = gtk_overlay_new();
	b->overlay = overlay;
	gtk_overlay_set_child(GTK_OVERLAY(overlay), box);
	GtkEventController *pointer = gtk_event_controller_motion_new();
	gtk_event_controller_set_propagation_phase(pointer, GTK_PHASE_CAPTURE);
	g_signal_connect(pointer, "motion", G_CALLBACK(pointer_moved), b);
	gtk_widget_add_controller(overlay, pointer);
	b->list = pixel_area(b, 0, draw_list, G_CALLBACK(list_pressed));
	gtk_widget_set_valign(b->list, GTK_ALIGN_START);
	gtk_widget_set_visible(b->list, FALSE);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(list_motion), b);
	gtk_widget_add_controller(b->list, motion);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->list);
	b->logins_list = pixel_area(b, 0, draw_logins, G_CALLBACK(logins_pressed));
	gtk_widget_set_halign(b->logins_list, GTK_ALIGN_END);
	gtk_widget_set_valign(b->logins_list, GTK_ALIGN_START);
	gtk_widget_set_focusable(b->logins_list, TRUE);
	gtk_widget_set_visible(b->logins_list, FALSE);
	motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(logins_motion), b);
	gtk_widget_add_controller(b->logins_list, motion);
	GtkEventController *logins_keys = gtk_event_controller_key_new();
	g_signal_connect(logins_keys, "key-pressed", G_CALLBACK(logins_key), b);
	gtk_widget_add_controller(b->logins_list, logins_keys);
	GtkEventController *logins_focus = gtk_event_controller_focus_new();
	g_signal_connect(logins_focus, "leave", G_CALLBACK(logins_focus_left), b);
	gtk_widget_add_controller(b->logins_list, logins_focus);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->logins_list);
	b->ctx = ctx_new(b);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->ctx);
	GtkGesture *away = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(away), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(away),
		GTK_PHASE_CAPTURE);
	g_signal_connect(away, "pressed", G_CALLBACK(ctx_click_away), b);
	gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(away));
	GtkEventController *ctx_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(ctx_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(ctx_keys, "key-pressed", G_CALLBACK(ctx_window_key), b);
	gtk_widget_add_controller(b->window, ctx_keys);

	/* The dialog: its drawing, with a password field laid over it. */
	b->dialog_lines = g_ptr_array_new_with_free_func(g_free);
	b->dialog_box = gtk_overlay_new();
	b->dialog_area = pixel_area(b, 0, draw_dialog, G_CALLBACK(dialog_pressed));
	gtk_overlay_set_child(GTK_OVERLAY(b->dialog_box), b->dialog_area);
	b->dialog_field = gtk_entry_new();
	gtk_widget_add_css_class(b->dialog_field, "gem-field");
	gtk_widget_set_halign(b->dialog_field, GTK_ALIGN_START);
	gtk_widget_set_valign(b->dialog_field, GTK_ALIGN_START);
	gtk_overlay_add_overlay(GTK_OVERLAY(b->dialog_box), b->dialog_field);
	gtk_widget_set_halign(b->dialog_box, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(b->dialog_box, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(b->dialog_box, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->dialog_box);
	gtk_window_set_child(GTK_WINDOW(b->window), overlay);
	GtkEventController *dialog_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(dialog_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(dialog_keys, "key-pressed", G_CALLBACK(dialog_key), b);
	gtk_widget_add_controller(b->window, dialog_keys);
	b->password_cancel = g_cancellable_new();
	add_shortcuts(b->window);
	b->menu = app_menu_new(b->window, build_menus, menu_activate, b);
	return b;
}

/* ---- Web apps ------------------------------------------------------------- */

/* `gemweb --app URL [--name NAME]`: a window with just the page, no tabs or
 * address field, named NAME in the menu bar (its own Wayland app ID,
 * org.gemwm.GemWeb.NAME). It shares the browser's logins. Links to other
 * sites open in the browser; the app's own pop-ups (signing in) and
 * redirects stay. */

/* A host's site, for what an app covers: its last two names (x.com), or
 * three when the second-last is short, as in bbc.co.uk. */
static char *base_domain(const char *host) {
	char **parts = g_strsplit(host, ".", -1);
	int n = g_strv_length(parts);
	int keep = n >= 3 && strlen(parts[n - 2]) <= 3 && strlen(parts[n - 1]) == 2 ?
		3 : 2;
	char *base = g_strjoinv(".", parts + MAX(n - keep, 0));
	g_strfreev(parts);
	return base;
}

/* Whether uri is the app's: on its site, or not a web address at all. */
static bool app_owns(struct browser *b, const char *uri) {
	char *site = site_of(uri), *app = site_of(b->app_uri);
	bool owns = site == NULL || app == NULL;
	if (!owns) {
		char *s1 = base_domain(site), *s2 = base_domain(app);
		owns = strcmp(s1, s2) == 0;
		g_free(s1);
		g_free(s2);
	}
	g_free(site);
	g_free(app);
	return owns;
}

/* The browser window last used (not an app's), if there is one. */
static struct browser *browser_window(void) {
	for (GList *w = gtk_application_get_windows(shared.app); w != NULL;
			w = w->next) {
		struct browser *b = g_object_get_data(G_OBJECT(w->data), "browser");
		if (b != NULL && !b->app) {
			return b;
		}
	}
	return NULL;
}

static void open_in_browser(const char *uri) {
	struct browser *b = browser_window();
	if (b == NULL) {
		b = browser_new(shared.app);
	}
	tab_new(b, NULL, uri, true);
	gtk_window_present(GTK_WINDOW(b->window));
}

/* The menu bar names a window after its app ID's last part. */
static void app_realized(GtkWidget *window, struct browser *b) {
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (GDK_IS_WAYLAND_TOPLEVEL(surface)) {
		char *id = g_strconcat("org.gemwm.GemWeb.", b->app_name, NULL);
		g_strdelimit(id + strlen("org.gemwm.GemWeb."), "./", '-');
		gdk_wayland_toplevel_set_application_id(GDK_TOPLEVEL(surface), id);
		g_free(id);
	}
}

/* A new app window, without a tab yet. */
static struct browser *app_window(const char *uri, const char *name) {
	struct browser *b = browser_new(shared.app);
	b->app = true;
	b->app_uri = g_strdup(uri);
	b->app_name = g_strdup(name);
	gtk_window_set_title(GTK_WINDOW(b->window), name);
	/* GTK sets the application's own ID as the window's realized; this
	 * comes after, and before it's mapped, so GemWM sees the app's. */
	g_signal_connect_after(b->window, "realize", G_CALLBACK(app_realized), b);
	g_signal_connect(b->window, "map", G_CALLBACK(app_realized), b);
	sync_tabbar(b);
	gtk_widget_set_visible(b->toolbar, FALSE);
	sync_info(b);
	app_menu_update(b->menu);
	return b;
}

/* An app's name, if it isn't given: its site's (x.com -> X). */
static char *app_default_name(const char *uri) {
	char *site = site_of(uri);
	char *base = site != NULL ? base_domain(site) : g_strdup("Web App");
	char *dot = strchr(base, '.');
	if (dot != NULL && dot != base) {
		*dot = '\0';
	}
	base[0] = g_ascii_toupper(base[0]);
	g_free(site);
	return base;
}

/* Opens the app, or brings its window forward if it's open. */
static void app_open(const char *input, const char *name) {
	char *uri = input_to_uri(input, NULL);
	if (uri == NULL) {
		return;
	}
	char *app_name = name != NULL ? g_strdup(name) : app_default_name(uri);
	for (GList *w = gtk_application_get_windows(shared.app); w != NULL;
			w = w->next) {
		struct browser *b = g_object_get_data(G_OBJECT(w->data), "browser");
		if (b != NULL && b->app && strcmp(b->app_name, app_name) == 0) {
			gtk_window_present(GTK_WINDOW(b->window));
			g_free(app_name);
			g_free(uri);
			return;
		}
	}
	struct browser *b = app_window(uri, app_name);
	tab_new(b, NULL, uri, true);
	gtk_window_present(GTK_WINDOW(b->window));
	g_free(app_name);
	g_free(uri);
}

/* Every `gemweb [URL...]` lands here, in the first instance: URLs open as
 * tabs in the existing window. */
static int command_line(GApplication *app, GApplicationCommandLine *cmdline) {
	int argc;
	char **argv = g_application_command_line_get_arguments(cmdline, &argc);
	/* A second `gemweb` hands its arguments to this one and exits: say so
	 * on its terminal, or it looks as if a fresh GemWeb started. */
	if (g_application_command_line_get_is_remote(cmdline)) {
		g_application_command_line_printerr(cmdline,
			"gemweb: opening in the GemWeb already running (pid %d)\n",
			(int)getpid());
	}
	if (shared.app == NULL) {
		load_css();
		shared_init(GTK_APPLICATION(app));
	}
	const char *app_uri = NULL, *name = NULL;
	GPtrArray *uris = g_ptr_array_new();
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
			app_uri = argv[++i];
		} else if (g_str_has_prefix(argv[i], "--app=")) {
			app_uri = argv[i] + 6;
		} else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			name = argv[++i];
		} else if (g_str_has_prefix(argv[i], "--name=")) {
			name = argv[i] + 7;
		} else {
			g_ptr_array_add(uris, argv[i]);
		}
	}
	if (app_uri != NULL) {
		app_open(app_uri, name);
	}
	/* Starting GemWeb opens a new window (tabs are made inside it). Links
	 * handed over by other programs open as tabs in the last-used window,
	 * as other browsers do. */
	if (app_uri == NULL || uris->len > 0) {
		struct browser *b = uris->len > 0 ? browser_window() : NULL;
		if (b == NULL) {
			b = browser_new(shared.app);
		}
		if (uris->len == 0) {
			tab_new(b, NULL, NULL, true);
		}
		for (guint i = 0; i < uris->len; i++) {
			tab_new(b, NULL, uris->pdata[i], i == uris->len - 1);
		}
		gtk_window_present(GTK_WINDOW(b->window));
	}
	g_ptr_array_free(uris, TRUE);
	g_strfreev(argv);
	return 0;
}

int main(int argc, char *argv[]) {
	gem_print_setup();
	font_family = g_getenv("GEMWM_FONT") ? g_getenv("GEMWM_FONT") : "monospace";
	font_size = g_getenv("GEMWM_FONT_SIZE") ? atoi(g_getenv("GEMWM_FONT_SIZE")) : 0;
	font_size = font_size > 0 ? font_size : 14;
	GtkApplication *app = gtk_application_new("org.gemwm.GemWeb",
		G_APPLICATION_HANDLES_COMMAND_LINE);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
