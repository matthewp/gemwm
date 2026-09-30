#include <math.h>
#include "gem-scrollbar.h"
#include "gem-ui.h"

struct scrollbar {
	GtkWidget *area;
	GtkAdjustment *adj;
	GtkOrientation orientation;
	int pressed;          /* the part held: PART_* */
	double grab;          /* where in the slider it was grabbed */
	guint repeat;
};

enum { PART_NONE, PART_BACK, PART_FORWARD, PART_PAGE_BACK, PART_PAGE_FORWARD,
	PART_SLIDER };

/* Along the bar: its length, and the slider's start and length. */
static int length(struct scrollbar *s) {
	return s->orientation == GTK_ORIENTATION_VERTICAL ?
		gtk_widget_get_height(s->area) : gtk_widget_get_width(s->area);
}

static void slider(struct scrollbar *s, int len, int *start, int *size) {
	double lower = gtk_adjustment_get_lower(s->adj);
	double upper = gtk_adjustment_get_upper(s->adj);
	double page = gtk_adjustment_get_page_size(s->adj);
	double value = gtk_adjustment_get_value(s->adj);
	int track = MAX(len - 2 * GEM_GADGET, 0);
	double total = upper - lower;
	*size = total <= page || total <= 0 ? track :
		MAX(GEM_GADGET, (int)(track * page / total));
	*size = MIN(*size, track);
	double range = total - page;
	*start = GEM_GADGET + (range > 0 ?
		(int)lround((track - *size) * (value - lower) / range) : 0);
}

static void paint_arrow(cairo_t *cr, int at, bool back) {
	double cx = GEM_GADGET / 2.0 + 0.5, cy = at + GEM_GADGET / 2.0;
	cairo_move_to(cr, cx, back ? cy - 4 : cy + 4);
	cairo_line_to(cr, cx + 5, back ? cy + 3 : cy - 3);
	cairo_line_to(cr, cx - 5, back ? cy + 3 : cy - 3);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* Drawn as a vertical bar; a horizontal one is the same drawing turned on
 * its side (x and y swapped). */
static void paint(cairo_t *cr, int w, int h, void *data) {
	struct scrollbar *s = data;
	if (s->orientation == GTK_ORIENTATION_HORIZONTAL) {
		cairo_matrix_t m;
		cairo_matrix_init(&m, 0, 1, 1, 0, 0, 0);
		cairo_transform(cr, &m);
		int t = w;
		w = h;
		h = t;
	}
	gem_black(cr);
	gem_fill(cr, 0, 0, 1, h);
	gem_dither(cr, 1, GEM_GADGET, w - 1, h - 2 * GEM_GADGET);
	gem_fill(cr, 0, GEM_GADGET - 1, w, 1);
	gem_fill(cr, 0, h - GEM_GADGET, w, 1);
	paint_arrow(cr, 0, true);
	paint_arrow(cr, h - GEM_GADGET, false);
	if (s->pressed == PART_BACK || s->pressed == PART_FORWARD) {
		/* The arrow held, inverted. */
		int at = s->pressed == PART_BACK ? 0 : h - GEM_GADGET + 1;
		cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
		gem_white(cr);
		gem_fill(cr, 1, at, w - 1, GEM_GADGET - 1);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	int start, size;
	slider(s, h, &start, &size);
	gem_white(cr);
	gem_fill(cr, 1, start, w - 1, size);
	gem_black(cr);
	gem_frame(cr, 0, start, w, size, 1);
}

static void step(struct scrollbar *s, int part) {
	double v = gtk_adjustment_get_value(s->adj);
	double stepping = gtk_adjustment_get_step_increment(s->adj);
	double page = gtk_adjustment_get_page_increment(s->adj);
	if (stepping <= 0) {
		stepping = 20;
	}
	if (page <= 0) {
		page = gtk_adjustment_get_page_size(s->adj);
	}
	switch (part) {
	case PART_BACK: v -= stepping; break;
	case PART_FORWARD: v += stepping; break;
	case PART_PAGE_BACK: v -= page; break;
	case PART_PAGE_FORWARD: v += page; break;
	}
	gtk_adjustment_set_value(s->adj, v);
}

static gboolean repeat_tick(gpointer data) {
	struct scrollbar *s = data;
	step(s, s->pressed);
	return G_SOURCE_CONTINUE;
}

/* After the first step, a pause, then steps while it's held. */
static gboolean repeat_start(gpointer data) {
	struct scrollbar *s = data;
	s->repeat = g_timeout_add(40, repeat_tick, s);
	return G_SOURCE_REMOVE;
}

static void stop_repeat(struct scrollbar *s) {
	if (s->repeat != 0) {
		g_source_remove(s->repeat);
		s->repeat = 0;
	}
}

static double along(struct scrollbar *s, double x, double y) {
	return s->orientation == GTK_ORIENTATION_VERTICAL ? y : x;
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	struct scrollbar *s = data;
	int len = length(s), start, size;
	slider(s, len, &start, &size);
	double at = along(s, x, y);
	s->pressed = at < GEM_GADGET ? PART_BACK : at >= len - GEM_GADGET ?
		PART_FORWARD : at < start ? PART_PAGE_BACK : at >= start + size ?
		PART_PAGE_FORWARD : PART_SLIDER;
	if (s->pressed == PART_SLIDER) {
		s->grab = at - start;
	} else {
		step(s, s->pressed);
		s->repeat = g_timeout_add(300, repeat_start, s);
	}
	gtk_widget_queue_draw(s->area);
}

static void drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct scrollbar *s = data;
	if (s->pressed != PART_SLIDER) {
		return;
	}
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	int len = length(s), start, size;
	slider(s, len, &start, &size);
	int track = len - 2 * GEM_GADGET - size;
	double pos = along(s, sx + dx, sy + dy) - s->grab - GEM_GADGET;
	double lower = gtk_adjustment_get_lower(s->adj);
	double range = gtk_adjustment_get_upper(s->adj) - lower -
		gtk_adjustment_get_page_size(s->adj);
	if (track > 0 && range > 0) {
		gtk_adjustment_set_value(s->adj, lower + pos * range / track);
	}
}

static void drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct scrollbar *s = data;
	stop_repeat(s);
	s->pressed = PART_NONE;
	gtk_widget_queue_draw(s->area);
}

static void adjustment_changed(GtkAdjustment *adj, gpointer data) {
	struct scrollbar *s = data;
	gtk_widget_queue_draw(s->area);
}

static void destroyed(GtkWidget *w, gpointer data) {
	struct scrollbar *s = data;
	stop_repeat(s);
	g_signal_handlers_disconnect_by_data(s->adj, s);
	g_object_unref(s->adj);
	g_free(s);
}

GtkWidget *gem_scrollbar_new(GtkOrientation orientation, GtkAdjustment *adj) {
	struct scrollbar *s = g_new0(struct scrollbar, 1);
	s->orientation = orientation;
	s->adj = g_object_ref(adj);
	bool vertical = orientation == GTK_ORIENTATION_VERTICAL;
	s->area = gem_pixel_area_new(vertical ? GEM_GADGET + 1 : 0,
		vertical ? 0 : GEM_GADGET + 1, paint, s);
	gtk_widget_set_hexpand(s->area, !vertical);
	gtk_widget_set_vexpand(s->area, vertical);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), s);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), s);
	g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), s);
	gtk_widget_add_controller(s->area, GTK_EVENT_CONTROLLER(drag));
	g_signal_connect(adj, "changed", G_CALLBACK(adjustment_changed), s);
	g_signal_connect(adj, "value-changed", G_CALLBACK(adjustment_changed), s);
	g_signal_connect(s->area, "destroy", G_CALLBACK(destroyed), s);
	return s->area;
}
