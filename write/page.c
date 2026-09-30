/*
 * The page canvas (see page.h). Text geometry all comes from the layout
 * engine; this knows where the pages are, draws them and what's on them,
 * and turns input into editor calls.
 *
 * The pages are drawn as the document will print, with its own fonts,
 * smoothly; around them it's GEM: the grey dither, a black frame and
 * shadow on each page, the selection inverted.
 */
#include <math.h>
#include <string.h>
#include "gem-ui.h"
#include "layout/layout.h"
#include "page.h"

#define GAP 24.0              /* pixels between and around pages */
#define MIN_FIT_WIDTH 160.0   /* the narrowest the page is fitted to */
#define ZOOM_100 (96.0 / 72.0) /* a point is a pixel at 96 dpi */

struct _WpPageView {
	GtkWidget parent;

	WpEditor *ed;
	WpDocument *doc;          /* the editor's */
	WpLayoutEngine *engine;   /* the editor's */
	double zoom;              /* points to pixels */
	gboolean fit_width;

	/* What the press that started a drag selected (1 character, 2 word,
	 * 3 paragraph), so the drag grows the selection by whole ones. */
	int press_unit;
	WpPos press_start, press_end;

	GtkIMContext *im;
	GtkGesture *drag;
	GtkWidget *comment_pop;   /* the floating Comment button */
	guint comment_id;         /* its pending update */
	gboolean menu_up;

	wp_page_menu_fn menu_fn;
	wp_page_comment_fn comment_fn;
	void *handler_data;

	guint blink_id;
	gboolean caret_on;
	gboolean focused;
	gboolean scroll_pending;
	guint scroll_tick;        /* the tick that does the pending scroll */
	gboolean typing;          /* the last thing was an edit, not a move */
	gboolean edited;          /* an edit since the last state change */
	WpPos last_caret;
	int last_width;
	GtkAdjustment *vadj;      /* the viewport's, redrawn on; weak */
};

G_DEFINE_FINAL_TYPE(WpPageView, wp_page_view, GTK_TYPE_WIDGET)

enum { SIG_STATE_CHANGED, SIG_DOCUMENT_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- Pixels and points ---------------------------------------------------- */

static double page_stride(WpPageView *v) {
	return v->doc->page.height * v->zoom + GAP;
}

/* The zoom that fits the page in width pixels, never over 100%. */
static double zoom_for_width(WpPageView *v, double width) {
	if (!v->fit_width) {
		return v->zoom;
	}
	double z = (width - 2 * GAP) / v->doc->page.width;
	return fmin(fmax(z, 0.1), ZOOM_100);
}

/* Pages sit on whole pixels, so their frames are crisp. */
static void page_origin(WpPageView *v, size_t page, double *x0, double *y0) {
	double w = gtk_widget_get_width(GTK_WIDGET(v));
	double pw = v->doc->page.width * v->zoom;
	*x0 = floor(fmax(GAP, (w - pw) / 2.0));
	*y0 = floor(GAP + page * page_stride(v));
}

static gboolean widget_to_pos(WpPageView *v, double wx, double wy, WpPos *out) {
	size_t n = wp_layout_page_count(v->engine);
	if (n == 0) {
		return FALSE;
	}
	double i = floor((wy - GAP) / page_stride(v));
	size_t page = i < 0 ? 0 : (size_t)i >= n ? n - 1 : (size_t)i;
	double x0, y0;
	page_origin(v, page, &x0, &y0);
	double px = (wx - x0) / v->zoom;
	double py = fmin(fmax((wy - y0) / v->zoom, 0.0), v->doc->page.height);
	return wp_layout_hit_test(v->engine, page, px, py, out);
}

/* ---- The caret ------------------------------------------------------------ */

static gboolean blink(gpointer data) {
	WpPageView *v = data;
	v->caret_on = !v->caret_on;
	gtk_widget_queue_draw(GTK_WIDGET(v));
	return G_SOURCE_CONTINUE;
}

static void restart_blink(WpPageView *v) {
	g_clear_handle_id(&v->blink_id, g_source_remove);
	v->caret_on = TRUE;
	if (v->focused) {
		v->blink_id = g_timeout_add(530, blink, v);
	}
}

static void scroll_to_caret(WpPageView *v) {
	GtkWidget *vp = gtk_widget_get_ancestor(GTK_WIDGET(v), GTK_TYPE_VIEWPORT);
	size_t page;
	WpRect r;
	if (vp == NULL ||
			!wp_layout_caret_rect(v->engine, wp_editor_caret(v->ed), &page, &r)) {
		return;
	}
	double x0, y0;
	page_origin(v, page, &x0, &y0);
	double y = y0 + r.y * v->zoom, h = r.h * v->zoom;
	GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vp));
	gtk_adjustment_clamp_page(adj, y - GAP, y + h + GAP);
}

static gboolean scroll_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	WpPageView *v = data;
	v->scroll_tick = 0;
	v->scroll_pending = FALSE;
	scroll_to_caret(v);
	return G_SOURCE_REMOVE;
}

/* ---- The floating Comment button ------------------------------------------ */

/* A GEM button beside the end of the selection, while the view has the
 * focus, nothing's being dragged and no menu is up. */
static gboolean update_comment_button(gpointer data) {
	WpPageView *v = data;
	v->comment_id = 0;
	WpPos a, b;
	wp_editor_get_selection(v->ed, &a, &b);
	size_t page;
	WpRect r;
	gboolean show = v->focused && v->comment_fn != NULL &&
		wp_editor_has_selection(v->ed) && !gtk_gesture_is_active(v->drag) &&
		!v->menu_up && wp_layout_caret_rect(v->engine, b, &page, &r);
	if (!show) {
		if (gtk_widget_get_visible(v->comment_pop)) {
			gtk_popover_popdown(GTK_POPOVER(v->comment_pop));
		}
		return G_SOURCE_REMOVE;
	}
	double x0, y0;
	page_origin(v, page, &x0, &y0);
	GdkRectangle at = { (int)(x0 + r.x * v->zoom), (int)(y0 + r.y * v->zoom),
		1, (int)ceil(r.h * v->zoom) };
	gtk_popover_set_pointing_to(GTK_POPOVER(v->comment_pop), &at);
	if (!gtk_widget_get_visible(v->comment_pop)) {
		gtk_popover_popup(GTK_POPOVER(v->comment_pop));
	}
	return G_SOURCE_REMOVE;
}

/* A little later, so a run of selection changes moves it once. */
static void schedule_comment_button(WpPageView *v) {
	if (v->comment_id == 0) {
		v->comment_id = g_timeout_add(60, update_comment_button, v);
	}
}

static void paint_comment_button(cairo_t *cr, int w, int h, void *data) {
	gem_button(cr, NULL, 0, 0, w, "Comment", 0, false);
}

static void comment_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	WpPageView *v = data;
	if (v->comment_fn != NULL) {
		v->comment_fn(v, v->handler_data);
	}
}

/* ---- The editor's news ---------------------------------------------------- */

static void on_editor_state(WpEditor *ed, void *data) {
	WpPageView *v = data;
	/* An edit starts or continues typing; moving the caret without one
	 * ends it. The editor also reports the state after an edit's undo
	 * step is recorded, with nothing edited and the caret where it was:
	 * that's neither. */
	WpPos caret = wp_editor_caret(ed);
	if (v->edited) {
		v->typing = TRUE;
	} else if (!wp_pos_eq(caret, v->last_caret)) {
		v->typing = FALSE;
	}
	v->edited = FALSE;
	v->last_caret = caret;
	restart_blink(v);
	v->scroll_pending = TRUE;
	gtk_widget_queue_resize(GTK_WIDGET(v));
	schedule_comment_button(v);
	g_signal_emit(v, signals[SIG_STATE_CHANGED], 0);
}

static void on_editor_document(WpEditor *ed, size_t first_para, void *data) {
	WpPageView *v = data;
	v->edited = TRUE;
	gtk_widget_queue_resize(GTK_WIDGET(v));
	g_signal_emit(v, signals[SIG_DOCUMENT_CHANGED], 0);
}

/* ---- The menu ------------------------------------------------------------- */

static void menu_at(WpPageView *v, double x, double y) {
	gtk_widget_grab_focus(GTK_WIDGET(v));
	if (v->menu_fn != NULL) {
		v->menu_fn(v, x, y, v->handler_data);
	}
}

/* A misspelled word under the pointer is selected, for its corrections;
 * otherwise a click in the selection keeps it and one outside moves the
 * caret there, so Cut and Copy act on what the pointer's over. */
static void secondary_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	WpPageView *v = data;
	WpPos p, a, b, ma, mb;
	if (widget_to_pos(v, x, y, &p)) {
		wp_editor_get_selection(v->ed, &a, &b);
		gboolean inside = wp_editor_has_selection(v->ed) &&
			wp_pos_cmp(a, p) <= 0 && wp_pos_cmp(p, b) <= 0;
		if (wp_editor_misspelled_at(v->ed, p, &ma, &mb)) {
			wp_editor_select(v->ed, ma, mb);
		} else if (!inside) {
			wp_editor_set_caret(v->ed, p, FALSE);
		}
	}
	menu_at(v, x, y);
}

/* The Menu key and Shift+F10: at the caret. */
static void menu_at_caret(WpPageView *v) {
	WpPos caret = wp_pos_min(wp_editor_caret(v->ed), wp_editor_anchor(v->ed));
	WpPos a, b;
	if (!wp_editor_has_selection(v->ed) &&
			wp_editor_misspelled_at(v->ed, caret, &a, &b)) {
		wp_editor_select(v->ed, a, b);
	}
	size_t page;
	WpRect r;
	double x = GAP, y = GAP;
	if (wp_layout_caret_rect(v->engine, wp_editor_caret(v->ed), &page, &r)) {
		double x0, y0;
		page_origin(v, page, &x0, &y0);
		x = x0 + r.x * v->zoom;
		y = y0 + (r.y + r.h) * v->zoom;
	}
	menu_at(v, x, y);
}

/* ---- Input ---------------------------------------------------------------- */

static void on_commit(GtkIMContext *im, const char *str, gpointer data) {
	wp_editor_insert_text(WP_PAGE_VIEW(data)->ed, str, -1);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	WpPageView *v = data;
	gboolean shift = (state & GDK_SHIFT_MASK) != 0;
	gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
	WpEditor *ed = v->ed;

	switch (keyval) {
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		wp_editor_insert_text(ed, "\n", 1);
		return TRUE;
	case GDK_KEY_Tab:
		/* At the start of a list item, Tab nests it; elsewhere it's a tab. */
		if (wp_editor_current_list_kind(ed) != WP_LIST_NONE &&
				!wp_editor_has_selection(ed) && wp_editor_caret(ed).offset == 0) {
			wp_editor_indent(ed, +1);
		} else {
			wp_editor_insert_text(ed, "\t", 1);
		}
		return TRUE;
	case GDK_KEY_ISO_Left_Tab: /* Shift+Tab */
		if (wp_editor_current_list_kind(ed) != WP_LIST_NONE) {
			wp_editor_indent(ed, -1);
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_BackSpace:
		wp_editor_delete(ed, -1);
		return TRUE;
	case GDK_KEY_Delete:
		if (shift) {
			return FALSE; /* Shift+Delete is Cut */
		}
		wp_editor_delete(ed, +1);
		return TRUE;
	case GDK_KEY_Menu:
		menu_at_caret(v);
		return TRUE;
	case GDK_KEY_F10:
		if (!shift) {
			return FALSE;
		}
		menu_at_caret(v);
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_Right:
		wp_editor_move(ed, ctrl ? WP_MOVE_WORD : WP_MOVE_CHAR,
			keyval == GDK_KEY_Left ? -1 : 1, shift);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		wp_editor_move(ed, WP_MOVE_LINE, keyval == GDK_KEY_Up ? -1 : 1, shift);
		return TRUE;
	case GDK_KEY_Home:
	case GDK_KEY_End:
		wp_editor_move(ed, ctrl ? WP_MOVE_DOC : WP_MOVE_LINE_EDGE,
			keyval == GDK_KEY_Home ? -1 : 1, shift);
		return TRUE;
	case GDK_KEY_Page_Up:
	case GDK_KEY_Page_Down:
		wp_editor_move(ed, WP_MOVE_PAGE, keyval == GDK_KEY_Page_Up ? -1 : 1,
			shift);
		return TRUE;
	}
	return FALSE;
}

/* The press is on_click's (it knows how many clicks); the drag only takes
 * the focus and grows the selection. */
static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	gtk_widget_grab_focus(GTK_WIDGET(data));
}

static void on_drag_end(GtkGestureDrag *g, double x, double y, gpointer data) {
	schedule_comment_button(data);
}

/* The word or paragraph around p. */
static void unit_bounds(WpPageView *v, int unit, WpPos p, WpPos *s, WpPos *e) {
	if (unit >= 3) {
		*s = (WpPos){ p.para, 0 };
		*e = (WpPos){ p.para, wp_document_para(v->doc, p.para)->len };
	} else {
		wp_document_word_bounds(v->doc, p, s, e);
	}
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	WpPageView *v = data;
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	WpPos p;
	if (!widget_to_pos(v, sx + dx, sy + dy, &p)) {
		return;
	}
	if (v->press_unit <= 1) {
		wp_editor_set_caret(v->ed, p, TRUE);
		return;
	}
	/* By words or paragraphs: the one under the pointer joins the one
	 * pressed, so both ends cover whole ones. */
	WpPos s, e;
	unit_bounds(v, v->press_unit, p, &s, &e);
	if (wp_pos_cmp(p, v->press_start) < 0) {
		wp_editor_select(v->ed, v->press_end, s);
	} else {
		wp_editor_select(v->ed, v->press_start, e);
	}
}

/* A click places the caret (Shift extends), a double click selects the
 * word, a triple click the paragraph. */
static void on_click(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	WpPageView *v = data;
	WpPos p;
	if (!widget_to_pos(v, x, y, &p)) {
		return;
	}
	v->press_unit = n >= 3 ? 3 : n;
	if (n == 1) {
		GdkModifierType st = gtk_event_controller_get_current_event_state(
			GTK_EVENT_CONTROLLER(g));
		wp_editor_set_caret(v->ed, p, (st & GDK_SHIFT_MASK) != 0);
		v->press_start = v->press_end = p;
		return;
	}
	unit_bounds(v, v->press_unit, p, &v->press_start, &v->press_end);
	wp_editor_select(v->ed, v->press_start, v->press_end);
}

/* Ctrl and the wheel zoom. */
static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	WpPageView *v = data;
	GdkModifierType st = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(c));
	if (!(st & GDK_CONTROL_MASK)) {
		return GDK_EVENT_PROPAGATE;
	}
	wp_page_view_set_zoom(v, v->zoom * (dy < 0 ? 1.1 : 1 / 1.1));
	return GDK_EVENT_STOP;
}

static void on_focus_enter(GtkEventControllerFocus *c, gpointer data) {
	WpPageView *v = data;
	v->focused = TRUE;
	gtk_im_context_focus_in(v->im);
	restart_blink(v);
	schedule_comment_button(v);
	gtk_widget_queue_draw(GTK_WIDGET(v));
}

static void on_focus_leave(GtkEventControllerFocus *c, gpointer data) {
	WpPageView *v = data;
	v->focused = FALSE;
	gtk_im_context_focus_out(v->im);
	g_clear_handle_id(&v->blink_id, g_source_remove);
	schedule_comment_button(v);
	gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* ---- Size ------------------------------------------------------------------ */

static void measure(GtkWidget *w, GtkOrientation o, int for_size, int *min,
		int *nat, int *min_baseline, int *nat_baseline) {
	WpPageView *v = WP_PAGE_VIEW(w);
	size_t n = wp_layout_page_count(v->engine);
	if (o == GTK_ORIENTATION_HORIZONTAL) {
		if (v->fit_width) {
			*min = (int)ceil(MIN_FIT_WIDTH + 2 * GAP);
			*nat = (int)ceil(v->doc->page.width * ZOOM_100 + 2 * GAP);
		} else {
			*min = *nat = (int)ceil(v->doc->page.width * v->zoom + 2 * GAP);
		}
	} else {
		double z = for_size > 0 ? zoom_for_width(v, for_size) : v->zoom;
		*min = *nat = (int)ceil(n * (v->doc->page.height * z + GAP) + GAP);
	}
	*min_baseline = *nat_baseline = -1;
}

static GtkSizeRequestMode get_request_mode(GtkWidget *w) {
	return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

/* Only what the viewport shows is drawn, and GTK reuses it as the viewport
 * scrolls, so what scrolls into view needs drawing then. */
static void on_vadj_changed(GtkAdjustment *adj, gpointer data) {
	gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void track_scrolling(WpPageView *v) {
	GtkWidget *vp = gtk_widget_get_ancestor(GTK_WIDGET(v), GTK_TYPE_VIEWPORT);
	GtkAdjustment *adj = vp ? gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vp)) :
		NULL;
	if (adj == v->vadj) {
		return;
	}
	/* A viewport scrolls to whatever takes the focus, which for this
	 * widget (the whole document) is the top. The caret is what's kept in
	 * view, by scroll_to_caret. */
	if (vp != NULL) {
		gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(vp), FALSE);
	}
	if (v->vadj != NULL) {
		g_signal_handlers_disconnect_by_func(v->vadj, on_vadj_changed, v);
	}
	v->vadj = adj;
	if (adj != NULL) {
		g_signal_connect_object(adj, "value-changed", G_CALLBACK(on_vadj_changed),
			v, 0);
	}
}

static void size_allocate(GtkWidget *w, int width, int height, int baseline) {
	WpPageView *v = WP_PAGE_VIEW(w);
	track_scrolling(v);
	double z = zoom_for_width(v, width);
	/* Where the page is follows the width, and the ruler follows the page. */
	if (z != v->zoom || width != v->last_width) {
		v->zoom = z;
		v->last_width = width;
		gtk_widget_queue_draw(w);
		g_signal_emit(v, signals[SIG_STATE_CHANGED], 0);
	}
	/* The viewport has placed us for this frame already; scrolling now
	 * would leave it a frame behind. The next frame does it. */
	if (v->scroll_pending && v->scroll_tick == 0) {
		v->scroll_tick = gtk_widget_add_tick_callback(w, scroll_tick, v, NULL);
	}
	gtk_popover_present(GTK_POPOVER(v->comment_pop));
}

/* ---- Drawing --------------------------------------------------------------- */

static void fill_rect(const WpRect *r, void *data) {
	cairo_t *cr = data;
	cairo_rectangle(cr, r->x, r->y, r->w, r->h);
	cairo_fill(cr);
}

/* A search match: a box around it. */
static void box_rect(const WpRect *r, void *data) {
	cairo_t *cr = data;
	cairo_rectangle(cr, r->x, r->y, r->w, r->h);
	cairo_stroke(cr);
}

/* A misspelled word: a zigzag under it, two pixels high (the line's
 * width is a pixel). */
static void zigzag(const WpRect *r, void *data) {
	cairo_t *cr = data;
	double px = cairo_get_line_width(cr), step = 2 * px;
	double y = r->y + r->h - px / 2, x1 = r->x + r->w;
	cairo_move_to(cr, r->x, y);
	gboolean up = TRUE;
	for (double x = r->x + step; x < x1 + step; x += step, up = !up) {
		cairo_line_to(cr, fmin(x, x1), up ? y - 2 * px : y);
	}
	cairo_stroke(cr);
}

static size_t page_of_para(WpPageView *v, size_t para) {
	size_t page;
	WpRect r;
	return wp_layout_caret_rect(v->engine, (WpPos){ para, 0 }, &page, &r) ?
		page : SIZE_MAX;
}

/* The first paragraph that may be on a page, found by bisecting on where
 * each starts (the one before may run onto it), so only the paragraphs
 * from there are checked for misspellings and matches. */
static size_t first_para_on_page(WpPageView *v, size_t page) {
	size_t lo = 0, hi = wp_document_para_count(v->doc);
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (page_of_para(v, mid) < page) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo > 0 ? lo - 1 : 0;
}

/* Boxes around the search's matches; not the selected one, which the
 * inverted selection shows. */
static void draw_matches(WpPageView *v, size_t page, cairo_t *cr) {
	if (wp_editor_search_term(v->ed) == NULL) {
		return;
	}
	WpPos a, b;
	wp_editor_get_selection(v->ed, &a, &b);
	size_t n = wp_document_para_count(v->doc);
	cairo_save(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1 / v->zoom);
	for (size_t pi = first_para_on_page(v, page); pi < n; pi++) {
		if (page_of_para(v, pi) > page) {
			break;
		}
		size_t ns;
		const WpSpan *spans = wp_editor_search_matches(v->ed, pi, &ns);
		for (size_t i = 0; i < ns; i++) {
			if (a.para == pi && b.para == pi && spans[i].start == a.offset &&
					spans[i].end == b.offset) {
				continue;
			}
			wp_layout_selection_rects(v->engine, (WpPos){ pi, spans[i].start },
				(WpPos){ pi, spans[i].end }, page, box_rect, cr);
		}
	}
	cairo_restore(cr);
}

/* Zigzags under the misspelled words. A word being typed (the last thing
 * was an edit and the caret's at its end) is left alone, so "hel" isn't
 * marked on its way to "hello". */
static void draw_misspellings(WpPageView *v, size_t page, cairo_t *cr) {
	if (wp_editor_spell(v->ed) == NULL) {
		return;
	}
	size_t n = wp_document_para_count(v->doc);
	WpPos caret = wp_editor_caret(v->ed);
	cairo_save(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1 / v->zoom);
	for (size_t pi = first_para_on_page(v, page); pi < n; pi++) {
		if (page_of_para(v, pi) > page) {
			break;
		}
		size_t ns;
		const WpSpan *spans = wp_editor_misspellings(v->ed, pi, &ns);
		for (size_t i = 0; i < ns; i++) {
			if (v->typing && v->focused && caret.para == pi &&
					caret.offset == spans[i].end) {
				continue;
			}
			wp_layout_selection_rects(v->engine, (WpPos){ pi, spans[i].start },
				(WpPos){ pi, spans[i].end }, page, zigzag, cr);
		}
	}
	cairo_restore(cr);
}

/* GEM's grey: a pixel checkerboard, in whole pixels whatever the scale. */
static cairo_pattern_t *grey_pattern(void) {
	static cairo_pattern_t *grey;
	if (grey == NULL) {
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2, 2);
		uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s) / 4;
		px[0] = 0x000000;
		px[1] = 0xffffff;
		px[stride] = 0xffffff;
		px[stride + 1] = 0x000000;
		cairo_surface_mark_dirty(s);
		grey = cairo_pattern_create_for_surface(s);
		cairo_surface_destroy(s);
		cairo_pattern_set_extend(grey, CAIRO_EXTEND_REPEAT);
		cairo_pattern_set_filter(grey, CAIRO_FILTER_NEAREST);
	}
	return grey;
}

static void snapshot(GtkWidget *w, GtkSnapshot *snap) {
	WpPageView *v = WP_PAGE_VIEW(w);
	double W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);

	/* Only what the viewport shows. */
	double vis0 = 0, vis1 = H;
	GtkWidget *vp = gtk_widget_get_ancestor(w, GTK_TYPE_VIEWPORT);
	if (vp != NULL) {
		GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vp));
		vis0 = gtk_adjustment_get_value(adj);
		vis1 = fmin(H, vis0 + gtk_adjustment_get_page_size(adj));
	}
	cairo_t *cr = gtk_snapshot_append_cairo(snap,
		&GRAPHENE_RECT_INIT(0, (float)vis0, (float)W, (float)(vis1 - vis0)));
	cairo_set_source(cr, grey_pattern());
	cairo_paint(cr);

	size_t n = wp_layout_page_count(v->engine);
	double pw = round(v->doc->page.width * v->zoom);
	double ph = round(v->doc->page.height * v->zoom);
	gboolean has_sel = wp_editor_has_selection(v->ed);
	WpPos a, b;
	wp_editor_get_selection(v->ed, &a, &b);
	size_t caret_page = 0;
	WpRect caret;
	gboolean have_caret = wp_layout_caret_rect(v->engine, wp_editor_caret(v->ed),
		&caret_page, &caret);

	for (size_t i = 0; i < n; i++) {
		double x0, y0;
		page_origin(v, i, &x0, &y0);
		if (y0 + ph + 3 < vis0 || y0 - 1 > vis1) {
			continue;
		}
		/* A black frame and a shadow below and to the right. */
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_rectangle(cr, x0 - 1, y0 - 1, pw + 2, ph + 2);
		cairo_rectangle(cr, x0 + 2, y0 + ph + 1, pw + 1, 2);
		cairo_rectangle(cr, x0 + pw + 1, y0 + 2, 2, ph + 1);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, x0, y0, pw, ph);
		cairo_fill(cr);

		cairo_save(cr);
		cairo_rectangle(cr, x0, y0, pw, ph);
		cairo_clip(cr);
		cairo_translate(cr, x0, y0);
		cairo_scale(cr, v->zoom, v->zoom);
		cairo_set_source_rgb(cr, 0, 0, 0);
		wp_layout_render_page(v->engine, i, cr);
		draw_matches(v, i, cr);
		draw_misspellings(v, i, cr);
		if (has_sel) {
			/* Inverted, as GEM selects. */
			cairo_save(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
			cairo_set_source_rgb(cr, 1, 1, 1);
			wp_layout_selection_rects(v->engine, a, b, i, fill_rect, cr);
			cairo_restore(cr);
		}
		if (have_caret && caret_page == i && v->focused && v->caret_on &&
				!has_sel) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, caret.x, caret.y, 2 / v->zoom, caret.h);
			cairo_fill(cr);
		}
		cairo_restore(cr);
	}
	cairo_destroy(cr);
}

/* ---- The widget ------------------------------------------------------------ */

static void dispose(GObject *obj) {
	WpPageView *v = WP_PAGE_VIEW(obj);
	g_clear_handle_id(&v->blink_id, g_source_remove);
	g_clear_handle_id(&v->comment_id, g_source_remove);
	if (v->scroll_tick != 0) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(v), v->scroll_tick);
		v->scroll_tick = 0;
	}
	g_clear_object(&v->im);
	g_clear_pointer(&v->comment_pop, gtk_widget_unparent);
	if (v->ed != NULL) {
		wp_editor_set_listener(v->ed, NULL);
		v->ed = NULL;
	}
	G_OBJECT_CLASS(wp_page_view_parent_class)->dispose(obj);
}

static void wp_page_view_class_init(WpPageViewClass *klass) {
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
	G_OBJECT_CLASS(klass)->dispose = dispose;
	wc->measure = measure;
	wc->get_request_mode = get_request_mode;
	wc->size_allocate = size_allocate;
	wc->snapshot = snapshot;
	gtk_widget_class_set_css_name(wc, "pageview");
	signals[SIG_STATE_CHANGED] = g_signal_new("state-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
	signals[SIG_DOCUMENT_CHANGED] = g_signal_new("document-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void wp_page_view_init(WpPageView *v) {
	GtkWidget *w = GTK_WIDGET(v);
	v->zoom = ZOOM_100;
	v->fit_width = TRUE;
	gtk_widget_set_focusable(w, TRUE);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_widget_set_cursor_from_name(w, "text");

	v->im = gtk_im_multicontext_new();
	gtk_im_context_set_client_widget(v->im, w);
	g_signal_connect(v->im, "commit", G_CALLBACK(on_commit), v);

	GtkEventController *key = gtk_event_controller_key_new();
	gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key), v->im);
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), v);
	gtk_widget_add_controller(w, key);

	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), v);
	g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), v);
	gtk_widget_add_controller(w, focus);

	GtkGesture *drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
	gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));
	v->drag = drag;

	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "pressed", G_CALLBACK(on_click), v);
	gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(click));

	GtkGesture *secondary = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary),
		GDK_BUTTON_SECONDARY);
	g_signal_connect(secondary, "pressed", G_CALLBACK(secondary_pressed), v);
	gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(secondary));

	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), v);
	gtk_widget_add_controller(w, scroll);

	/* The floating Comment button: see update_comment_button. */
	GtkWidget *button = gem_pixel_area_new(gem_button_width("Comment"),
		GEM_BUTTON_H, paint_comment_button, NULL);
	gtk_widget_set_cursor_from_name(button, "default");
	GtkGesture *press = gtk_gesture_click_new();
	g_signal_connect(press, "released", G_CALLBACK(comment_pressed), v);
	gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(press));
	v->comment_pop = gtk_popover_new();
	gtk_widget_add_css_class(v->comment_pop, "gem-popup");
	gtk_popover_set_child(GTK_POPOVER(v->comment_pop), button);
	gtk_popover_set_autohide(GTK_POPOVER(v->comment_pop), FALSE);
	gtk_popover_set_has_arrow(GTK_POPOVER(v->comment_pop), FALSE);
	gtk_popover_set_position(GTK_POPOVER(v->comment_pop), GTK_POS_RIGHT);
	gtk_widget_set_can_focus(v->comment_pop, FALSE);
	gtk_widget_set_parent(v->comment_pop, w);
}

/* ---- Public ---------------------------------------------------------------- */

WpPageView *wp_page_view_new(WpEditor *editor) {
	WpPageView *v = g_object_new(WP_TYPE_PAGE_VIEW, NULL);
	v->ed = editor;
	v->doc = wp_editor_document(editor);
	v->engine = wp_editor_engine(editor);
	WpEditorListener l = { on_editor_state, on_editor_document, v };
	wp_editor_set_listener(editor, &l);
	return v;
}

WpEditor *wp_page_view_get_editor(WpPageView *v) {
	return v->ed;
}

void wp_page_view_set_handlers(WpPageView *v, wp_page_menu_fn menu,
		wp_page_comment_fn comment, void *data) {
	v->menu_fn = menu;
	v->comment_fn = comment;
	v->handler_data = data;
}

void wp_page_view_set_menu_up(WpPageView *v, gboolean up) {
	v->menu_up = up;
	schedule_comment_button(v);
}

void wp_page_view_set_zoom(WpPageView *v, double zoom) {
	v->fit_width = FALSE;
	v->zoom = fmin(fmax(zoom, 0.25), 6.0);
	v->scroll_pending = TRUE;
	gtk_widget_queue_resize(GTK_WIDGET(v));
	g_signal_emit(v, signals[SIG_STATE_CHANGED], 0);
}

void wp_page_view_set_fit_width(WpPageView *v) {
	v->fit_width = TRUE;
	v->scroll_pending = TRUE;
	gtk_widget_queue_resize(GTK_WIDGET(v));
	g_signal_emit(v, signals[SIG_STATE_CHANGED], 0);
}

gboolean wp_page_view_get_fit_width(WpPageView *v) {
	return v->fit_width;
}

double wp_page_view_get_zoom(WpPageView *v) {
	return v->zoom;
}

double wp_page_view_get_page_x(WpPageView *v) {
	double x, y;
	page_origin(v, 0, &x, &y);
	return x;
}
