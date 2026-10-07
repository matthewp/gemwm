/*
 * The overview (Super+W, window mode): every window on the workspace at
 * once, shrunk into a grid over the desktop, each in its GEM frame with
 * what it shows now, as the Mac's Mission Control does. The selected one
 * is drawn as the active window is. Super+Tab (Shift: back) and the arrow
 * keys move the selection, the pointer selects what it's over; Return or
 * a click brings that window forward, and Escape (or Super+W again, or a
 * click on the desktop) leaves things as they were.
 *
 * The windows themselves stay where they are, hidden while it's up; the
 * grid holds copies, drawn again each frame from the windows' surfaces
 * (and those are told to keep drawing, as they would be if they showed),
 * so a video plays on. Opening, they glide from their places into the
 * grid, and back again when it closes, as long as animations are on.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>
#include "server.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define MARGIN 32   /* round the grid, inside the usable area */
#define GAP 24      /* between thumbnails */

struct thumb {
	struct view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *frame;
	struct wlr_scene_tree *content;
	struct wlr_scene_tree *highlight;   /* Super+Tab's border, when selected */
	struct wlr_scene_rect *highlight_rects[4];
	struct wlr_box from, to;   /* its window's frame box, its place in the grid */
	struct wlr_box now;        /* where it's drawn */
	bool drawn_selected;
	int drawn_w, drawn_h;
};

static struct {
	struct wlr_scene_tree *tree;
	struct thumb *thumbs;
	int count, selected, columns;
	int64_t start;             /* the glide in or out, 0 when still */
	bool closing;
	struct view *chosen;       /* closing: the one to bring forward, or NULL */
	bool pressed;              /* a click of ours: its release is too */
} o;

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void schedule_frames(struct server *server) {
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

bool overview_active(struct server *server) {
	return o.tree != NULL;
}

/* ---- Laying out ----------------------------------------------------------- */

static void extents(int *w, int *h) {
	struct frame_style plain = { 0 };
	int right, bottom;
	frame_extents(&plain, &right, &bottom);
	*w = FRAME_LEFT + right;
	*h = FRAME_TOP + bottom;
}

/* A window's frame box shrunk (never grown) to fit cell w x h: its
 * contents scale, its frame's chrome doesn't. */
static void fit(const struct wlr_box *frame, int w, int h, struct wlr_box *out) {
	int ew, eh;
	extents(&ew, &eh);
	int cw = MAX(frame->width - ew, 1), ch = MAX(frame->height - eh, 1);
	double s = MIN(1.0, MIN((double)(w - ew) / cw, (double)(h - eh) / ch));
	s = MAX(s, 0.05);
	out->width = (int)(cw * s) + ew;
	out->height = (int)(ch * s) + eh;
}

/* The grid: as many columns as shows the windows biggest. */
static void lay_out(struct server *server) {
	struct wlr_box area;
	if (!output_usable_area_at(server, server->cursor->x, server->cursor->y,
			&area)) {
		return;
	}
	area.x += MARGIN;
	area.y += MARGIN;
	area.width -= 2 * MARGIN;
	area.height -= 2 * MARGIN;
	int best = 1;
	double best_area = -1;
	for (int cols = 1; cols <= o.count; cols++) {
		int rows = (o.count + cols - 1) / cols;
		int cw = (area.width - (cols - 1) * GAP) / cols;
		int ch = (area.height - (rows - 1) * GAP) / rows;
		if (cw < 40 || ch < 40) {
			break;
		}
		double total = 0;
		for (int i = 0; i < o.count; i++) {
			struct wlr_box b;
			fit(&o.thumbs[i].from, cw, ch, &b);
			total += (double)b.width * b.height;
		}
		if (total > best_area) {
			best_area = total;
			best = cols;
		}
	}
	o.columns = best;
	int rows = (o.count + best - 1) / best;
	int cw = (area.width - (best - 1) * GAP) / best;
	int ch = (area.height - (rows - 1) * GAP) / rows;
	for (int i = 0; i < o.count; i++) {
		struct thumb *t = &o.thumbs[i];
		int row = i / best, col = i % best;
		/* The last row, if it's short, centred. */
		int in_row = row == rows - 1 ? o.count - row * best : best;
		int row_w = in_row * cw + (in_row - 1) * GAP;
		int x0 = area.x + (area.width - row_w) / 2;
		int grid_h = rows * ch + (rows - 1) * GAP;
		int y0 = area.y + (area.height - grid_h) / 2;
		fit(&t->from, cw, ch, &t->to);
		t->to.x = x0 + col * (cw + GAP) + (cw - t->to.width) / 2;
		t->to.y = y0 + row * (ch + GAP) + (ch - t->to.height) / 2;
	}
}

/* ---- Drawing ------------------------------------------------------------ */

struct copying {
	struct wlr_scene_tree *content;
	double scale;
	int geo_x, geo_y;
	struct timespec now;
};

/* A surface of the window, scaled, in its place in the thumbnail. */
static void copy_surface(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	struct copying *c = data;
	wlr_surface_send_frame_done(surface, &c->now);
	if (surface->buffer == NULL || surface->current.width <= 0) {
		return;
	}
	struct wlr_scene_buffer *b = wlr_scene_buffer_create(c->content,
		&surface->buffer->base);
	if (b == NULL) {
		return;
	}
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	wlr_scene_buffer_set_source_box(b, &src);
	wlr_scene_buffer_set_transform(b, surface->current.transform);
	int w = MAX((int)lround(surface->current.width * c->scale), 1);
	int h = MAX((int)lround(surface->current.height * c->scale), 1);
	wlr_scene_buffer_set_dest_size(b, w, h);
	wlr_scene_node_set_position(&b->node,
		(int)lround((sx - c->geo_x) * c->scale),
		(int)lround((sy - c->geo_y) * c->scale));
}

/* The border Super+Tab draws round the window it's on ([highlight] in the
 * config), round the selected one. */
static void draw_highlight(struct server *server, struct thumb *t,
		bool selected) {
	bool show = selected && server->highlight_width > 0;
	wlr_scene_node_set_enabled(&t->highlight->node, show);
	if (!show) {
		return;
	}
	int b = server->highlight_width, w = t->now.width, h = t->now.height;
	struct wlr_box r[4] = {
		{ -b, -b, w + 2 * b, b }, /* top */
		{ -b, h, w + 2 * b, b },  /* bottom */
		{ -b, 0, b, h },          /* left */
		{ w, 0, b, h },           /* right */
	};
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_size(t->highlight_rects[i], r[i].width, r[i].height);
		wlr_scene_rect_set_color(t->highlight_rects[i], server->highlight_color);
		wlr_scene_node_set_position(&t->highlight_rects[i]->node, r[i].x, r[i].y);
	}
	wlr_scene_node_raise_to_top(&t->highlight->node);
}

static void draw_thumb(struct server *server, struct thumb *t, bool selected) {
	int ew, eh;
	extents(&ew, &eh);
	int cw = MAX(t->now.width - ew, 1), ch = MAX(t->now.height - eh, 1);
	wlr_scene_node_set_position(&t->tree->node, t->now.x, t->now.y);
	if (selected != t->drawn_selected || cw != t->drawn_w || ch != t->drawn_h) {
		struct frame_style plain = { 0 };
		frame_draw(t->frame, &plain, cw, ch, t->view->xdg_toplevel->title != NULL ?
			t->view->xdg_toplevel->title : "", selected);
		t->drawn_selected = selected;
		t->drawn_w = cw;
		t->drawn_h = ch;
	}
	/* What the window shows now, its contents scaled to fit. */
	if (t->content != NULL) {
		wlr_scene_node_destroy(&t->content->node);
	}
	t->content = wlr_scene_tree_create(t->tree);
	wlr_scene_node_set_position(&t->content->node, FRAME_LEFT, FRAME_TOP);
	struct wlr_box *geo = &t->view->xdg_toplevel->base->geometry;
	struct copying c = {
		.content = t->content,
		.scale = (double)cw / MAX(geo->width, 1),
		.geo_x = geo->x,
		.geo_y = geo->y,
	};
	clock_gettime(CLOCK_MONOTONIC, &c.now);
	wlr_xdg_surface_for_each_surface(t->view->xdg_toplevel->base, copy_surface,
		&c);
	draw_highlight(server, t, selected);
}

/* Quick to start, gentle to stop. */
static double ease_out(double t) {
	return 1 - (1 - t) * (1 - t) * (1 - t);
}

static void lerp(const struct wlr_box *a, const struct wlr_box *b, double t,
		struct wlr_box *out) {
	out->x = a->x + (int)lround((b->x - a->x) * t);
	out->y = a->y + (int)lround((b->y - a->y) * t);
	out->width = a->width + (int)lround((b->width - a->width) * t);
	out->height = a->height + (int)lround((b->height - a->height) * t);
}

static void finish_close(struct server *server);

/* Each output frame while it's up: the glide, and the windows' contents
 * drawn again. True while there's more to draw. */
bool overview_tick(struct server *server) {
	if (o.tree == NULL) {
		return false;
	}
	double t = 1;
	if (o.start != 0) {
		int ms = server->anim_ms;
		t = ms > 0 ? (double)(now_ms() - o.start) / ms : 1;
		if (t >= 1) {
			t = 1;
			o.start = 0;
		}
	}
	double e = ease_out(t);
	if (o.closing) {
		e = 1 - e;
	}
	for (int i = 0; i < o.count; i++) {
		struct thumb *t2 = &o.thumbs[i];
		lerp(&t2->from, &t2->to, e, &t2->now);
		draw_thumb(server, t2, i == o.selected && !o.closing);
	}
	if (o.closing && o.start == 0) {
		finish_close(server);
		return false;
	}
	return true; /* up: draw it again next frame, for what's playing */
}

/* ---- Opening and closing ------------------------------------------------- */

static void free_thumbs(void) {
	free(o.thumbs);
	o.thumbs = NULL;
	o.count = 0;
}

void overview_open(struct server *server) {
	if (o.tree != NULL || server->mode != MODE_WINDOW ||
			server->active_workspace == NULL) {
		return;
	}
	view_cycle_end(server);
	int n = 0;
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		n += view->workspace == server->active_workspace && !view->fullscreen;
	}
	if (n == 0) {
		return;
	}
	o.thumbs = calloc(n, sizeof(*o.thumbs));
	o.tree = wlr_scene_tree_create(&server->scene->tree);
	/* Over the windows, under the menu bar. */
	wlr_scene_node_place_above(&o.tree->node, &server->layer_views->node);
	/* The most recently used first, as Super+Tab goes. */
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace != server->active_workspace || view->fullscreen) {
			continue;
		}
		struct thumb *t = &o.thumbs[o.count++];
		t->view = view;
		view_frame_box(view, &t->from);
		t->now = t->from;
		t->tree = wlr_scene_tree_create(o.tree);
		t->frame = wlr_scene_buffer_create(t->tree, NULL);
		t->highlight = wlr_scene_tree_create(t->tree);
		for (int k = 0; k < 4; k++) {
			t->highlight_rects[k] = wlr_scene_rect_create(t->highlight, 0, 0,
				server->highlight_color);
		}
		t->drawn_selected = !false; /* drawn the first time */
		t->drawn_w = -1;
	}
	/* Later ones under earlier ones, as the windows are stacked. */
	for (int i = o.count - 1; i >= 0; i--) {
		wlr_scene_node_raise_to_top(&o.thumbs[i].tree->node);
	}
	lay_out(server);
	o.selected = 0;
	o.closing = false;
	o.chosen = NULL;
	o.start = server->anim_ms > 0 ? now_ms() : 0;
	wlr_scene_node_set_enabled(&server->active_workspace->tree->node, false);
	highlight_update(server);
	schedule_frames(server);
}

static void finish_close(struct server *server) {
	if (o.tree == NULL) {
		return;
	}
	wlr_scene_node_destroy(&o.tree->node);
	o.tree = NULL;
	struct view *chosen = o.chosen;
	free_thumbs();
	if (server->active_workspace != NULL) {
		wlr_scene_node_set_enabled(&server->active_workspace->tree->node, true);
	}
	if (chosen != NULL) {
		focus_view(chosen);
	}
	highlight_update(server);
	cursor_rebase(server);
	schedule_frames(server);
}

/* Closes it, gliding back, bringing chosen forward (NULL: as it was). */
void overview_close(struct server *server, struct view *chosen) {
	if (o.tree == NULL || o.closing) {
		return;
	}
	o.closing = true;
	o.chosen = chosen;
	/* The chosen one glides back on top. */
	for (int i = 0; i < o.count; i++) {
		if (o.thumbs[i].view == chosen) {
			wlr_scene_node_raise_to_top(&o.thumbs[i].tree->node);
		}
	}
	o.start = server->anim_ms > 0 ? now_ms() : 0;
	if (o.start == 0) {
		finish_close(server);
	} else {
		schedule_frames(server);
	}
}

/* Something changed under it (a window came or went, the workspace or
 * mode changed): it goes at once, as things were. */
void overview_cancel(struct server *server) {
	if (o.tree != NULL) {
		o.chosen = NULL;
		finish_close(server);
	}
}

void overview_toggle(struct server *server) {
	if (o.tree != NULL) {
		overview_close(server, NULL);
	} else {
		overview_open(server);
	}
}

/* ---- Choosing ------------------------------------------------------------- */

static void select_index(struct server *server, int i) {
	if (o.count == 0) {
		return;
	}
	o.selected = (i % o.count + o.count) % o.count;
	schedule_frames(server);
}

/* Super+Tab while it's up. */
void overview_step(struct server *server, int direction) {
	if (o.tree != NULL && !o.closing) {
		select_index(server, o.selected + direction);
	}
}

static void choose(struct server *server) {
	overview_close(server, o.selected >= 0 && o.selected < o.count ?
		o.thumbs[o.selected].view : NULL);
}

/* Keys while it's up: true if it took them (it takes all but bindings'). */
bool overview_key(struct server *server, xkb_keysym_t sym) {
	if (o.tree == NULL) {
		return false;
	}
	if (o.closing) {
		return true;
	}
	int row = o.selected / MAX(o.columns, 1);
	switch (sym) {
	case XKB_KEY_Escape:
		overview_close(server, NULL);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		choose(server);
		break;
	case XKB_KEY_Left:
		select_index(server, o.selected - 1);
		break;
	case XKB_KEY_Right:
		select_index(server, o.selected + 1);
		break;
	case XKB_KEY_Up:
		if (row > 0) {
			select_index(server, o.selected - o.columns);
		}
		break;
	case XKB_KEY_Down:
		if (o.selected + o.columns < o.count) {
			select_index(server, o.selected + o.columns);
		} else if (row < (o.count - 1) / o.columns) {
			select_index(server, o.count - 1); /* the short last row */
		}
		break;
	}
	return true;
}

static int thumb_at(double x, double y) {
	/* Topmost first: the last raised. */
	for (int i = 0; i < o.count; i++) {
		struct wlr_box *b = &o.thumbs[i].now;
		if (x >= b->x && x < b->x + b->width && y >= b->y && y < b->y + b->height) {
			return i;
		}
	}
	return -1;
}

/* The pointer over it: what's under it is selected. */
void overview_pointer(struct server *server, double x, double y) {
	if (o.tree == NULL || o.closing) {
		return;
	}
	int i = thumb_at(x, y);
	if (i >= 0 && i != o.selected) {
		select_index(server, i);
	}
}

/* A click: on a window, it; elsewhere, nothing changes. */
bool overview_button(struct server *server, double x, double y, bool pressed) {
	if (o.tree == NULL) {
		bool ours = o.pressed && !pressed;
		o.pressed = false;
		return ours; /* the release of the click that chose */
	}
	if (pressed) {
		o.pressed = true;
		if (!o.closing) {
			int i = thumb_at(x, y);
			if (i >= 0) {
				o.selected = i;
				choose(server);
			} else {
				overview_close(server, NULL);
			}
		}
	} else {
		o.pressed = false;
	}
	return true;
}

/* A window going: its thumbnail would point at nothing. */
void overview_view_gone(struct server *server, struct view *view) {
	for (int i = 0; i < o.count; i++) {
		if (o.thumbs[i].view == view) {
			overview_cancel(server);
			return;
		}
	}
}
