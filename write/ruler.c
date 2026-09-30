#include <langinfo.h>
#include <math.h>
#include "gem-ui.h"
#include "ruler.h"

#define RULER_H 30
#define HIT_PX 6       /* how near the pointer must be to take a marker */
#define DROP_PX 24     /* a tab dragged this far below the ruler is removed */
#define BASE 22        /* the scale's line */

enum grab { GRAB_NONE, GRAB_LEFT, GRAB_FIRST, GRAB_RIGHT, GRAB_TAB };

struct ruler {
	GtkWidget *area;
	WpPageView *view;
	WpEditor *ed;
	GtkAdjustment *hadj;  /* the page's sideways scrolling; weak */
	bool metric;

	/* A drag: what was taken, where it started (points from the page's
	 * left edge), where it is, and whether it's gone far enough to be a
	 * drag rather than a click. */
	enum grab grab;
	double grab_pt, drag_pt, start_x, start_y;
	int grab_tab;
	bool dragging, dropped;
};

/* ---- Where things are ------------------------------------------------------ */

/* The ruler's x of the page's left edge, and pixels per point. */
static void page_scale(struct ruler *r, double *x0, double *zoom) {
	*zoom = wp_page_view_get_zoom(r->view);
	double vx = wp_page_view_get_page_x(r->view);
	graphene_point_t in = GRAPHENE_POINT_INIT((float)vx, 0), out;
	if (!gtk_widget_compute_point(GTK_WIDGET(r->view), r->area, &in, &out)) {
		out.x = (float)vx;
	}
	*x0 = round(out.x);
}

static double x_to_pt(struct ruler *r, double x) {
	double x0, z;
	page_scale(r, &x0, &z);
	return (x - x0) / z;
}

static int pt_to_x(struct ruler *r, double pt) {
	double x0, z;
	page_scale(r, &x0, &z);
	return (int)floor(x0 + pt * z);
}

/* Drags snap to a sixteenth of an inch, or a millimetre. */
static double snap(struct ruler *r, double pt) {
	double step = r->metric ? 72 / 25.4 : 4.5;
	return round(pt / step) * step;
}

static const WpParaStyle *caret_style(struct ruler *r) {
	WpDocument *doc = wp_editor_document(r->ed);
	return &wp_document_para(doc, wp_editor_caret(r->ed).para)->style;
}

/* The caret paragraph's markers, in points from the page's left edge. */
static void markers(struct ruler *r, double *left, double *first,
		double *right) {
	const WpPageSetup *pg = &wp_editor_document(r->ed)->page;
	const WpParaStyle *st = caret_style(r);
	*left = pg->margin_left + st->indent_left_pt;
	*first = *left + (st->list_kind != WP_LIST_NONE ? -WP_LIST_HANG_PT :
		st->indent_first_pt);
	*right = pg->width - pg->margin_right - st->indent_right_pt;
}

/* ---- Drawing --------------------------------------------------------------- */

/* A marker: a black triangle, its point on the scale. */
static void triangle(cairo_t *cr, int x, int y, bool up, bool grey) {
	for (int k = 0; k < 6; k++) {
		int row = up ? y + k : y - k;
		gem_fill(cr, x - k, row, 2 * k + 1, 1);
	}
	if (grey) {
		gem_grey_out(cr, x - 5, up ? y : y - 5, 11, 6);
	}
}

static void tab_mark(cairo_t *cr, int x, int y, WpTabKind kind) {
	gem_fill(cr, x, y - 6, 2, 7);
	switch (kind) {
	case WP_TAB_LEFT: gem_fill(cr, x, y - 1, 6, 2); break;
	case WP_TAB_RIGHT: gem_fill(cr, x - 4, y - 1, 6, 2); break;
	case WP_TAB_CENTER: gem_fill(cr, x - 3, y - 1, 8, 2); break;
	case WP_TAB_DECIMAL:
		gem_fill(cr, x - 3, y - 1, 8, 2);
		gem_fill(cr, x + 4, y - 5, 2, 2);
		break;
	}
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct ruler *r = data;
	const WpPageSetup *pg = &wp_editor_document(r->ed)->page;
	int page_l = pt_to_x(r, 0), page_r = pt_to_x(r, pg->width);
	int text_l = pt_to_x(r, pg->margin_left);
	int text_r = pt_to_x(r, pg->width - pg->margin_right);

	/* Grey off the page and in its margins, white where the text goes. */
	gem_black(cr);
	gem_dither(cr, 0, 0, w, h - 1);
	gem_white(cr);
	gem_fill(cr, text_l, 0, text_r - text_l, h - 1);
	gem_black(cr);
	gem_fill(cr, page_l, 0, 1, h - 1);
	gem_fill(cr, page_r, 0, 1, h - 1);
	gem_fill(cr, 0, h - 1, w, 1);

	/* The scale, numbered from the left margin: inches in eighths, or
	 * centimetres in halves. */
	double unit = r->metric ? 72 / 2.54 : 72;
	int per = r->metric ? 2 : 8;
	double origin = pg->margin_left;
	int lo = (int)floor(-origin / unit * per);
	int hi = (int)ceil((pg->width - origin) / unit * per);
	for (int i = lo; i <= hi; i++) {
		double pt = origin + (double)i / per * unit;
		if (pt < pg->margin_left || pt > pg->width - pg->margin_right) {
			continue;
		}
		int x = pt_to_x(r, pt);
		if (i % per == 0) {
			gem_fill(cr, x, BASE - 5, 1, 6);
			if (i != 0) {
				char n[8];
				g_snprintf(n, sizeof n, "%d", abs(i / per));
				gem_text(cr, n, x - (int)gem_text_width(cr, n) / 2, 3, 14);
			}
		} else {
			int len = !r->metric && i % 4 == 0 ? 3 : 2;
			gem_fill(cr, x, BASE - len + 1, 1, len);
		}
	}
	gem_fill(cr, text_l, BASE, text_r - text_l, 1);

	/* The markers; the one being dragged follows the pointer. */
	double left, first, right;
	markers(r, &left, &first, &right);
	if (r->dragging) {
		if (r->grab == GRAB_LEFT) {
			first += r->drag_pt - left;
			left = r->drag_pt;
		} else if (r->grab == GRAB_FIRST) {
			first = r->drag_pt;
		} else if (r->grab == GRAB_RIGHT) {
			right = r->drag_pt;
		}
	}
	const WpParaStyle *st = caret_style(r);
	for (int i = 0; i < st->ntabs; i++) {
		double pt = left + st->tabs[i].pos_pt;
		if (r->dragging && r->grab == GRAB_TAB && i == r->grab_tab) {
			if (r->dropped) {
				continue;
			}
			pt = r->drag_pt;
		}
		tab_mark(cr, pt_to_x(r, pt), BASE - 1, st->tabs[i].kind);
	}
	triangle(cr, pt_to_x(r, left), BASE + 1, true, false);
	triangle(cr, pt_to_x(r, right), BASE + 1, true, false);
	/* A list item's first line hangs by itself: its marker's grey. */
	triangle(cr, pt_to_x(r, first), 5, false, st->list_kind != WP_LIST_NONE);
}

/* ---- Input ----------------------------------------------------------------- */

static enum grab hit(struct ruler *r, double x, double y, int *tab) {
	double left, first, right;
	markers(r, &left, &first, &right);
	const WpParaStyle *st = caret_style(r);
	if (y < RULER_H / 2 && st->list_kind == WP_LIST_NONE &&
			fabs(x - pt_to_x(r, first)) <= HIT_PX) {
		return GRAB_FIRST;
	}
	if (y >= RULER_H / 2) {
		if (fabs(x - pt_to_x(r, left)) <= HIT_PX) {
			return GRAB_LEFT;
		}
		if (fabs(x - pt_to_x(r, right)) <= HIT_PX) {
			return GRAB_RIGHT;
		}
	}
	for (int i = 0; i < st->ntabs; i++) {
		if (fabs(x - pt_to_x(r, left + st->tabs[i].pos_pt)) <= HIT_PX) {
			*tab = i;
			return GRAB_TAB;
		}
	}
	return GRAB_NONE;
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	struct ruler *r = data;
	r->grab_tab = -1;
	r->grab = hit(r, x, y, &r->grab_tab);
	r->start_x = x;
	r->start_y = y;
	r->dragging = r->dropped = false;
	double left, first, right;
	markers(r, &left, &first, &right);
	switch (r->grab) {
	case GRAB_LEFT: r->grab_pt = left; break;
	case GRAB_FIRST: r->grab_pt = first; break;
	case GRAB_RIGHT: r->grab_pt = right; break;
	case GRAB_TAB: r->grab_pt = left + caret_style(r)->tabs[r->grab_tab].pos_pt; break;
	case GRAB_NONE: r->grab_pt = x_to_pt(r, x); break;
	}
	r->drag_pt = r->grab_pt;
}

static void drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct ruler *r = data;
	if (r->grab == GRAB_NONE || (!r->dragging && fabs(dx) < 3 && fabs(dy) < 3)) {
		return;
	}
	r->dragging = true;
	const WpPageSetup *pg = &wp_editor_document(r->ed)->page;
	r->drag_pt = CLAMP(snap(r, x_to_pt(r, r->start_x + dx)), 0, pg->width);
	r->dropped = r->grab == GRAB_TAB && r->start_y + dy > RULER_H + DROP_PX;
	gtk_widget_queue_draw(r->area);
}

static void drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct ruler *r = data;
	const WpPageSetup *pg = &wp_editor_document(r->ed)->page;
	double left, first, right;
	markers(r, &left, &first, &right);
	if (!r->dragging) {
		/* A click on the scale between the indents: a left tab there. */
		double pt = snap(r, x_to_pt(r, r->start_x));
		if (r->grab == GRAB_NONE && pt > left && pt < right) {
			wp_editor_add_tab(r->ed, (float)(pt - left), WP_TAB_LEFT);
		}
	} else {
		switch (r->grab) {
		case GRAB_LEFT:
			wp_editor_set_indent(r->ed, WP_INDENT_LEFT,
				(float)(r->drag_pt - pg->margin_left));
			break;
		case GRAB_FIRST:
			wp_editor_set_indent(r->ed, WP_INDENT_FIRST, (float)(r->drag_pt - left));
			break;
		case GRAB_RIGHT:
			wp_editor_set_indent(r->ed, WP_INDENT_RIGHT,
				(float)(pg->width - pg->margin_right - r->drag_pt));
			break;
		case GRAB_TAB: {
			float from = caret_style(r)->tabs[r->grab_tab].pos_pt;
			if (r->dropped) {
				wp_editor_remove_tab(r->ed, from);
			} else {
				wp_editor_move_tab(r->ed, from, (float)(r->drag_pt - left));
			}
			break;
		}
		case GRAB_NONE:
			break;
		}
	}
	r->grab = GRAB_NONE;
	r->dragging = false;
	gtk_widget_queue_draw(r->area);
	gtk_widget_grab_focus(GTK_WIDGET(r->view));
}

static void motion(GtkEventControllerMotion *c, double x, double y,
		gpointer data) {
	struct ruler *r = data;
	int tab;
	if (r->grab == GRAB_NONE) {
		gtk_widget_set_cursor_from_name(r->area,
			hit(r, x, y, &tab) == GRAB_NONE ? NULL : "col-resize");
	}
}

/* ---- The widget ------------------------------------------------------------ */

static void hadj_changed(GtkAdjustment *adj, gpointer data) {
	struct ruler *r = data;
	gtk_widget_queue_draw(r->area);
}

/* The page scrolls sideways when zoomed past the window: follow it. */
static void follow_scrolling(struct ruler *r) {
	GtkWidget *vp = gtk_widget_get_ancestor(GTK_WIDGET(r->view),
		GTK_TYPE_VIEWPORT);
	GtkAdjustment *adj = vp != NULL ?
		gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(vp)) : NULL;
	if (adj == r->hadj) {
		return;
	}
	if (r->hadj != NULL) {
		g_signal_handlers_disconnect_by_func(r->hadj, hadj_changed, r);
	}
	r->hadj = adj;
	if (adj != NULL) {
		g_signal_connect(adj, "value-changed", G_CALLBACK(hadj_changed), r);
	}
}

static void view_changed(WpPageView *view, gpointer data) {
	struct ruler *r = data;
	follow_scrolling(r);
	gtk_widget_queue_draw(r->area);
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct ruler *r = data;
	g_signal_handlers_disconnect_by_data(r->view, r);
	if (r->hadj != NULL) {
		g_signal_handlers_disconnect_by_func(r->hadj, hadj_changed, r);
	}
	g_free(r);
}

GtkWidget *ruler_new(WpPageView *view) {
	struct ruler *r = g_new0(struct ruler, 1);
	r->view = view;
	r->ed = wp_page_view_get_editor(view);
	const char *m = nl_langinfo(_NL_MEASUREMENT_MEASUREMENT);
	r->metric = !(m != NULL && m[0] == 2); /* 2 is the US system */
	r->area = gem_pixel_area_new(0, RULER_H, paint, r);
	gtk_widget_set_hexpand(r->area, TRUE);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), r);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), r);
	g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), r);
	gtk_widget_add_controller(r->area, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *move = gtk_event_controller_motion_new();
	g_signal_connect(move, "motion", G_CALLBACK(motion), r);
	gtk_widget_add_controller(r->area, move);
	g_signal_connect(view, "state-changed", G_CALLBACK(view_changed), r);
	g_signal_connect(view, "document-changed", G_CALLBACK(view_changed), r);
	g_signal_connect(r->area, "destroy", G_CALLBACK(destroyed), r);
	return r->area;
}
