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
 */
#include <cairo.h>
#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <string.h>
#include <webkit/webkit.h>
#include "gemwm-scroll-v1-client-protocol.h"

#define TAB_H 20       /* 19px of tabs plus a 1px line */
#define TOOL_H 21      /* the toolbar's buttons; the box adds a 1px line */
#define INFO_H 19      /* 18px of info line plus a 1px line */
#define GADGET 19      /* a GEM gadget box, as in GemWM */
#define TAB_MIN_W 90
#define TAB_MAX_W 220
#define PLUS_W 24      /* the "+" new-tab box */
#define FONT_SIZE 14
#define BUTTONS 4      /* back, forward, reload, home */
#define DEFAULT_HOME "https://www.google.com/"

static const char *font_family;

struct browser;

struct tab {
	struct browser *browser;
	WebKitWebView *view;
	WebKitUserContentManager *content;
	/* The page's scroll state, as its script last reported it (CSS px). */
	int scroll_x, scroll_y, view_w, view_h, doc_w, doc_h;
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
	"</style></head><body><div class=box><h1>GemWeb</h1>"
	"Type an address or a search above.</div></body></html>";

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

static void entry_changed(GtkEditable *editable, struct browser *b) {
	if (!b->entry_setting) {
		b->entry_edited = true;
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
	gtk_widget_queue_draw(b->buttons);
	gtk_widget_queue_draw(b->info);
}

static bool is_active(struct tab *t) {
	return active_tab(t->browser) == t;
}

static void on_title(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	if (is_active(t)) {
		gtk_window_set_title(GTK_WINDOW(t->browser->window), tab_title(t));
	}
	gtk_widget_queue_draw(t->browser->tabbar);
}

static void on_uri(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	if (is_active(t)) {
		sync_entry(t->browser);
	}
	gtk_widget_queue_draw(t->browser->tabbar);
}

static void on_progress(WebKitWebView *view, GParamSpec *pspec, struct tab *t) {
	if (is_active(t)) {
		gtk_widget_queue_draw(t->browser->info);
		gtk_widget_queue_draw(t->browser->buttons);
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
static char *input_to_uri(const char *input) {
	char *s = g_strstrip(g_strdup(input));
	char *uri;
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
		const char *search = g_getenv("GEMWEB_SEARCH");
		if (search == NULL || strstr(search, "%s") == NULL) {
			search = "https://duckduckgo.com/?q=%s";
		}
		char *q = g_uri_escape_string(s, NULL, TRUE);
		const char *at = strstr(search, "%s");
		uri = g_strdup_printf("%.*s%s%s", (int)(at - search), search, q, at + 2);
		g_free(q);
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
	char *uri = strcmp(home, "about:blank") == 0 ? NULL : input_to_uri(home);
	if (uri != NULL) {
		webkit_web_view_load_uri(t->view, uri);
	} else {
		webkit_web_view_load_html(t->view, start_page, "about:blank");
	}
	g_free(uri);
}

/* Loads what was typed, or the home page for nothing. */
static void load_input(struct tab *t, const char *input) {
	char *uri = input ? input_to_uri(input) : NULL;
	if (input == NULL) {
		load_home(t);
	} else if (uri != NULL) {
		webkit_web_view_load_uri(t->view, uri);
	} else {
		webkit_web_view_load_html(t->view, start_page, "about:blank");
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

static void entry_activate(GtkEntry *entry, struct browser *b) {
	struct tab *t = active_tab(b);
	char *uri = input_to_uri(gtk_editable_get_text(GTK_EDITABLE(entry)));
	if (t != NULL && uri != NULL) {
		/* Focus the page first, so the address field shows the real URI
		 * as soon as the load starts instead of what was typed. */
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
		webkit_web_view_load_uri(t->view, uri);
		sync_entry(b);
	}
	g_free(uri);
}

/* Escape in the address field puts the page's address back. */
static gboolean entry_key(GtkEventControllerKey *ctrl, guint keyval,
		guint keycode, GdkModifierType state, struct browser *b) {
	if (keyval != GDK_KEY_Escape) {
		return FALSE;
	}
	struct tab *t = active_tab(b);
	if (t != NULL) {
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
		b->entry_edited = false;
		sync_entry(b);
	}
	return TRUE;
}

enum action {
	ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_FOCUS_URL, ACT_NEXT_TAB, ACT_PREV_TAB,
	ACT_RELOAD, ACT_BACK, ACT_FORWARD, ACT_ZOOM_IN, ACT_ZOOM_OUT,
	ACT_ZOOM_RESET, ACT_QUIT, ACT_HOME,
};

static gboolean shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
	struct browser *b = g_object_get_data(G_OBJECT(widget), "browser");
	struct tab *t = active_tab(b);
	int n = b->tabs->len;
	switch (GPOINTER_TO_INT(data)) {
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
	}
	return TRUE;
}

static void add_shortcuts(GtkWidget *window) {
	static const struct { const char *trigger; enum action action; } keys[] = {
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
	g_ptr_array_free(b->tabs, TRUE);
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
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(entry_key), b);
	gtk_widget_add_controller(b->entry, keys);
	gtk_box_append(GTK_BOX(toolbar), b->buttons);
	gtk_box_append(GTK_BOX(toolbar), b->entry);

	b->info = pixel_area(b, INFO_H, draw_info, NULL);
	b->stack = gtk_stack_new();
	gtk_widget_set_vexpand(b->stack, TRUE);

	gtk_box_append(GTK_BOX(box), b->tabbar);
	gtk_box_append(GTK_BOX(box), toolbar);
	gtk_box_append(GTK_BOX(box), b->info);
	gtk_box_append(GTK_BOX(box), b->stack);
	gtk_window_set_child(GTK_WINDOW(b->window), box);
	add_shortcuts(b->window);
	return b;
}

/* Every `gemweb [URL...]` lands here, in the first instance: URLs open as
 * tabs in the existing window. */
static int command_line(GApplication *app, GApplicationCommandLine *cmdline) {
	int argc;
	char **argv = g_application_command_line_get_arguments(cmdline, &argc);
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
	GtkApplication *app = gtk_application_new("org.gemwm.GemWeb",
		G_APPLICATION_HANDLES_COMMAND_LINE);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
