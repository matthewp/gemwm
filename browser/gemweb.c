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
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <webkit/webkit.h>
#include "app-menu.h"
#include "gemwm-scroll-v1-client-protocol.h"
#include "history.h"

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
#define SUGGEST_ROWS 8 /* the address field's list of pages from history */
#define ROW_H 19
#define BUTTON_H 22    /* an alert's buttons */
#define PAD 8

static const char *font_family;
static int font_size;

struct browser;

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
};

struct browser {
	GtkWidget *window;
	GtkWidget *tabbar, *buttons, *entry, *info, *stack;
	GPtrArray *tabs; /* struct tab * */
	int active;
	char *hover_link;      /* link under the pointer, for the info line */
	char *download_status; /* last download message */
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

	/* The Clear History alert, over the window. */
	GtkWidget *alert;
	int alert_buttons[2][4]; /* Clear, Cancel: x, y, w, h */
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
} shared;

static struct tab *tab_new(struct browser *b, WebKitWebView *related,
	const char *uri, bool select);
static void tab_close(struct browser *b, int index);
static void tab_select(struct browser *b, int index);
static void load_home(struct tab *t);

/* GEM-style scroll bars for web pages: a dithered track, a white slider
 * and boxed arrows. User-level, so sites that style their own win. */
static const char page_css[] =
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
	gtk_widget_set_visible(b->tabbar, b->tabs->len >= 2);
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
			webkit_web_view_is_loading(view) && t->loads++ > 0) {
		t->media = false;
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
	gtk_widget_queue_draw(b->info);
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

/* What the user typed, as a URI: addresses as-is, bare names get https://,
 * anything else is a search ($GEMWEB_SEARCH, with %s for the query). */
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
		const char *engine = g_getenv("GEMWEB_SEARCH");
		if (engine == NULL || strstr(engine, "%s") == NULL) {
			engine = "https://duckduckgo.com/?q=%s";
		}
		char *q = g_uri_escape_string(s, NULL, TRUE);
		const char *at = strstr(engine, "%s");
		uri = g_strdup_printf("%.*s%s%s", (int)(at - engine), engine, q, at + 2);
		g_free(q);
		if (search != NULL) {
			*search = true;
		}
	}
	g_free(s);
	return uri;
}

/* The home page: $GEMWEB_HOME, or Google. "about:blank" (or empty) means
 * GemWeb's own start page. */
static const char *home_page(void) {
	const char *home = g_getenv("GEMWEB_HOME");
	return home != NULL ? home : DEFAULT_HOME;
}

static void load_home(struct tab *t) {
	const char *home = home_page();
	char *uri = strcmp(home, "about:blank") == 0 ? NULL : input_to_uri(home, NULL);
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
	b->active = index;
	struct tab *t = g_ptr_array_index(b->tabs, index);
	gtk_stack_set_visible_child(GTK_STACK(b->stack), GTK_WIDGET(t->view));
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
};

static struct browser *browser_new(GtkApplication *app);

/* ---- The Clear History alert --------------------------------------------- */

static const char *const alert_lines[] = {
	"Clear all history?",
	"Pages you've visited won't be suggested",
	"any more. Cookies and logins stay.",
};
static const char *const alert_labels[] = { "Clear", "Cancel" };

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

/* A GEM alert: a box in a box, the lines, then the buttons at the right,
 * the safe one (Cancel) the default with a thicker border. Sizes itself. */
static void paint_alert(struct browser *b, cairo_t *cr, int w, int h) {
	black(cr);
	fill(cr, 0, 0, w, 1);
	fill(cr, 0, h - 1, w, 1);
	fill(cr, 0, 0, 1, h);
	fill(cr, w - 1, 0, 1, h);
	fill(cr, 3, 3, w - 6, 2);
	fill(cr, 3, h - 5, w - 6, 2);
	fill(cr, 3, 3, 2, h - 6);
	fill(cr, w - 5, 3, 2, h - 6);
	for (guint i = 0; i < G_N_ELEMENTS(alert_lines); i++) {
		text(cr, alert_lines[i], 3 + 2 * PAD, 3 + 2 * PAD + i * 18, 18);
	}
	int bw = MAX(text_width(cr, alert_labels[0]),
		text_width(cr, alert_labels[1])) + 2 * PAD;
	int by = h - 3 - 2 * PAD - BUTTON_H;
	int bx = w - 3 - 2 * PAD - 2 * bw - PAD;
	for (int i = 0; i < 2; i++) {
		int x = bx + i * (bw + PAD), t = i == 1 ? 2 : 1;
		fill(cr, x, by, bw, t);
		fill(cr, x, by + BUTTON_H - t, bw, t);
		fill(cr, x, by, t, BUTTON_H);
		fill(cr, x + bw - t, by, t, BUTTON_H);
		text(cr, alert_labels[i],
			x + (bw - text_width(cr, alert_labels[i])) / 2, by, BUTTON_H);
		int *r = b->alert_buttons[i];
		r[0] = x, r[1] = by, r[2] = bw, r[3] = BUTTON_H;
	}
}

static void draw_alert(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	paint_pixelated(cr, w, h, paint_alert, data);
}

/* The alert holds the window: the rest of it takes no input meanwhile. */
static void alert_show(struct browser *b, bool show) {
	if (show) {
		double tw = 0;
		for (guint i = 0; i < G_N_ELEMENTS(alert_lines); i++) {
			tw = MAX(tw, measure(alert_lines[i]));
		}
		int bw = MAX(measure(alert_labels[0]), measure(alert_labels[1])) +
			2 * PAD;
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->alert),
			MAX((int)tw, 2 * bw + PAD) + 4 * PAD + 6);
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->alert),
			G_N_ELEMENTS(alert_lines) * 18 + BUTTON_H + 6 * PAD + 6);
		list_hide(b);
	}
	gtk_widget_set_sensitive(b->box, !show);
	gtk_widget_set_visible(b->alert, show);
	app_menu_update(b->menu);
}

static void alert_answer(struct browser *b, bool clear) {
	alert_show(b, false);
	if (clear) {
		history_clear();
	}
}

static void alert_pressed(GtkGestureClick *gesture, int n_press, double x,
		double y, struct browser *b) {
	for (int i = 0; i < 2; i++) {
		int *r = b->alert_buttons[i];
		if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
			alert_answer(b, i == 0);
		}
	}
}

/* Return takes the default, Cancel; so does Escape. */
static gboolean alert_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	if (!gtk_widget_get_visible(b->alert)) {
		return FALSE;
	}
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
			keyval == GDK_KEY_Escape) {
		alert_answer(b, false);
	}
	return TRUE;
}

static gboolean shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
	struct browser *b = g_object_get_data(G_OBJECT(widget), "browser");
	struct tab *t = active_tab(b);
	int n = b->tabs->len;
	if (gtk_widget_get_visible(b->alert) && GPOINTER_TO_INT(data) != ACT_QUIT) {
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
		alert_show(b, true);
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
	app_menu_add_item(m, ACT_NEW_WINDOW, "New Window", "^N", 0);
	app_menu_add_item(m, ACT_NEW_TAB, "New Tab", "^T", 0);
	app_menu_add_item(m, ACT_FOCUS_URL, "Open Location...", "^L", 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE_TAB, "Close Tab", "^W", 0);
	app_menu_add_item(m, ACT_QUIT, "Close Window", "^Q", 0);

	app_menu_add_menu(m, "View");
	app_menu_add_item(m, ACT_RELOAD, "Reload", "^R", 0);
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
	app_menu_add_item(m, ACT_NEXT_TAB, "Next Tab", "^Tab", one_tab);
	app_menu_add_item(m, ACT_PREV_TAB, "Previous Tab", "^Shift+Tab", one_tab);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLEAR_HISTORY, "Clear History...", NULL,
		gtk_widget_get_visible(b->alert) ? APP_MENU_DISABLED : 0);
}

static void menu_activate(uint32_t id, void *data) {
	struct browser *b = data;
	if (gtk_widget_get_visible(b->alert) && id != ACT_QUIT) {
		return;
	}
	if (active_tab(b) != NULL || id == ACT_NEW_WINDOW || id == ACT_NEW_TAB ||
			id == ACT_QUIT || id == ACT_CLEAR_HISTORY) {
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
		"entry.gem-url text selection { background: #000; color: #fff; }",
		TOOL_H, font_family, FONT_SIZE);
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
	shared.session = webkit_network_session_new(data, cache);
	char *cookies = g_build_filename(data, "cookies.sqlite", NULL);
	webkit_cookie_manager_set_persistent_storage(
		webkit_network_session_get_cookie_manager(shared.session),
		cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	g_signal_connect(shared.session, "download-started",
		G_CALLBACK(on_download_started), NULL);
	g_free(cookies);
	g_mkdir_with_parents(data, 0700);
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
	gtk_box_append(GTK_BOX(toolbar), b->buttons);
	gtk_box_append(GTK_BOX(toolbar), b->entry);

	b->info = pixel_area(b, INFO_H, draw_info, NULL);
	b->stack = gtk_stack_new();
	gtk_widget_set_vexpand(b->stack, TRUE);

	gtk_box_append(GTK_BOX(box), b->tabbar);
	gtk_box_append(GTK_BOX(box), toolbar);
	gtk_box_append(GTK_BOX(box), b->info);
	gtk_box_append(GTK_BOX(box), b->stack);

	/* Over it all: the address field's list, and the alert. */
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), box);
	b->list = pixel_area(b, 0, draw_list, G_CALLBACK(list_pressed));
	gtk_widget_set_valign(b->list, GTK_ALIGN_START);
	gtk_widget_set_visible(b->list, FALSE);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(list_motion), b);
	gtk_widget_add_controller(b->list, motion);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->list);
	b->alert = pixel_area(b, 0, draw_alert, G_CALLBACK(alert_pressed));
	gtk_widget_set_halign(b->alert, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(b->alert, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(b->alert, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b->alert);
	gtk_window_set_child(GTK_WINDOW(b->window), overlay);
	GtkEventController *alert_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(alert_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(alert_keys, "key-pressed", G_CALLBACK(alert_key), b);
	gtk_widget_add_controller(b->window, alert_keys);
	add_shortcuts(b->window);
	b->menu = app_menu_new(b->window, build_menus, menu_activate, b);
	return b;
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
	/* Starting GemWeb opens a new window (tabs are made inside it). Links
	 * handed over by other programs open as tabs in the last-used window,
	 * as other browsers do. */
	struct browser *b = NULL;
	if (argc >= 2) {
		GtkWindow *window = gtk_application_get_active_window(shared.app);
		b = window ? g_object_get_data(G_OBJECT(window), "browser") : NULL;
	}
	if (b == NULL) {
		b = browser_new(shared.app);
	}
	if (argc < 2) {
		tab_new(b, NULL, NULL, true);
	}
	for (int i = 1; i < argc; i++) {
		tab_new(b, NULL, argv[i], i == argc - 1);
	}
	gtk_window_present(GTK_WINDOW(b->window));
	g_strfreev(argv);
	return 0;
}

int main(int argc, char *argv[]) {
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
