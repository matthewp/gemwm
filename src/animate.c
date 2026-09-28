/*
 * Animations ([animation] in the config), in one of two styles.
 *
 * slide (the default): when the tiling or scrolling layout gives a window
 * a new place, it glides there, easing out, instead of jumping; in
 * scrolling mode that's the whole strip sliding along. A new window fades
 * in, rising a little into its place. Sizes don't animate: a resized
 * window slides to its new place and takes its new size when its client
 * draws it.
 *
 * outline: the way GEM did it, which didn't slide windows but drew
 * outlines. Opening a window drew a box growing to its size (graf_growbox),
 * moving one drew a box going from where it was to where it would be
 * (graf_movebox), in a few steps, and then the window appeared in its
 * place. So in tiling mode a window the layout moves or resizes is hidden
 * while its outline steps from its old box to its new one, and a new
 * window grows from a small box in the middle of its place. Scrolling mode
 * still slides its strip: there the point is to see where you went.
 *
 * A window's position (view.x, view.y) is always where it's going; only
 * what's drawn is on the way. Nothing runs unless something is moving:
 * while it is, each output frame advances it and asks for the next.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "server.h"

#define RISE 16       /* slide: a new window rises this far as it fades in */
#define STEPS 6       /* outline: its positions between the two boxes */
#define GROW_FROM 16  /* outline: the box a new window grows from */

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

/* Quick to start, gentle to stop. */
static double ease_out(double t) {
	return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ---- Slide -------------------------------------------------------------- */

/* The node's position now, between where it started and view.x/y. */
static void slide_position(struct view *view, double t, int *x, int *y) {
	double e = ease_out(t);
	*x = view->slide_from_x + (int)lround((view->x - view->slide_from_x) * e);
	*y = view->slide_from_y + (int)lround((view->y - view->slide_from_y) * e);
}

struct fade {
	struct view *view;
	float alpha;
};

/* Every buffer of the window: its surfaces and its frame, but not the
 * unfocused dither, which has an opacity of its own. */
static void set_alpha(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	struct fade *fade = data;
	if (buffer != fade->view->dim) {
		wlr_scene_buffer_set_opacity(buffer, fade->alpha);
	}
}

static void fade_to(struct view *view, float alpha) {
	struct fade fade = { view, alpha };
	wlr_scene_node_for_each_buffer(&view->tree->node, set_alpha, &fade);
}

/* The window's place changed (view.x/y already hold the new one): it
 * glides there from wherever it's drawn now. */
void animate_move(struct view *view) {
	struct server *server = view->server;
	struct wlr_scene_node *node = &view->tree->node;
	if (server->anim_mode == ANIM_OFF || !node->enabled ||
			view->workspace != server->active_workspace) {
		wlr_scene_node_set_position(node, view->x, view->y);
		view->slide_start = 0;
		return;
	}
	view->slide_from_x = node->x;
	view->slide_from_y = node->y;
	view->slide_start = now_ms();
	schedule_frames(server);
}

/* A new window: it fades in, rising into its place. */
void animate_appear(struct view *view) {
	struct server *server = view->server;
	if (server->anim_mode == ANIM_OFF) {
		return;
	}
	view->slide_from_x = view->x;
	view->slide_from_y = view->y + RISE;
	view->slide_start = view->fade_start = now_ms();
	wlr_scene_node_set_position(&view->tree->node, view->x, view->y + RISE);
	fade_to(view, 0);
	schedule_frames(server);
}

/* ---- Outline ------------------------------------------------------------ */

struct box_anim {
	struct wl_list link; /* server.animations */
	struct view *view;
	struct wlr_box from, to;
	int64_t start;       /* ms */
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *rects[8];
};

/* The same outline as dragging a window: black, with white inside, so it
 * shows on any background. */
static void outline_place(struct box_anim *a, const struct wlr_box *b) {
	struct wlr_box r[8] = {
		{ b->x, b->y, b->width, 1 },
		{ b->x, b->y + b->height - 1, b->width, 1 },
		{ b->x, b->y, 1, b->height },
		{ b->x + b->width - 1, b->y, 1, b->height },
		{ b->x + 1, b->y + 1, b->width - 2, 1 },
		{ b->x + 1, b->y + b->height - 2, b->width - 2, 1 },
		{ b->x + 1, b->y + 1, 1, b->height - 2 },
		{ b->x + b->width - 2, b->y + 1, 1, b->height - 2 },
	};
	for (int i = 0; i < 8; i++) {
		wlr_scene_rect_set_size(a->rects[i], r[i].width > 0 ? r[i].width : 0,
			r[i].height > 0 ? r[i].height : 0);
		wlr_scene_node_set_position(&a->rects[i]->node, r[i].x, r[i].y);
	}
}

/* Where an outline is: the next of its steps, so it jumps like GEM's. */
static struct wlr_box step_box(const struct box_anim *a, int64_t now, int ms) {
	int step = (int)((now - a->start) * STEPS / (ms > 0 ? ms : 1));
	double f = (double)(step < STEPS ? step + 1 : STEPS) / STEPS;
	return (struct wlr_box){
		a->from.x + (int)((a->to.x - a->from.x) * f),
		a->from.y + (int)((a->to.y - a->from.y) * f),
		a->from.width + (int)((a->to.width - a->from.width) * f),
		a->from.height + (int)((a->to.height - a->from.height) * f),
	};
}

static struct box_anim *find(struct server *server, struct view *view) {
	struct box_anim *a;
	wl_list_for_each(a, &server->animations, link) {
		if (a->view == view) {
			return a;
		}
	}
	return NULL;
}

static void finish(struct box_anim *a, bool show) {
	struct view *view = a->view;
	view->anim_hidden = false;
	if (show) {
		/* The layout (zoom, a scrolled-away column) may hide it again. */
		wlr_scene_node_set_enabled(&view->tree->node, true);
	}
	wlr_scene_node_destroy(&a->tree->node);
	wl_list_remove(&a->link);
	free(a);
}

/* The window goes from box from to box to: hidden meanwhile, its outline
 * stepping between them. Called again mid-way, it carries on from where
 * the outline has got to. */
void animate_box(struct view *view, const struct wlr_box *from,
		const struct wlr_box *to) {
	struct server *server = view->server;
	if (server->anim_mode == ANIM_OFF) {
		return;
	}
	int64_t now = now_ms();
	struct box_anim *a = find(server, view);
	struct wlr_box start = *from;
	if (a != NULL) {
		start = step_box(a, now, server->anim_ms);
	} else {
		a = calloc(1, sizeof(*a));
		a->view = view;
		a->tree = wlr_scene_tree_create(server->layer_drag);
		const float black[4] = { 0, 0, 0, 1 };
		const float white[4] = { 1, 1, 1, 1 };
		for (int i = 0; i < 8; i++) {
			a->rects[i] = wlr_scene_rect_create(a->tree, 0, 0,
				i < 4 ? black : white);
		}
		wl_list_insert(&server->animations, &a->link);
	}
	a->from = start;
	a->to = *to;
	a->start = now;
	outline_place(a, &start);
	view->anim_hidden = true;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	schedule_frames(server);
}

/* A new window: its outline grows from the middle of its place. */
void animate_grow(struct view *view, const struct wlr_box *to) {
	struct wlr_box from = {
		to->x + to->width / 2 - GROW_FROM / 2,
		to->y + to->height / 2 - GROW_FROM / 2,
		GROW_FROM, GROW_FROM,
	};
	animate_box(view, &from, to);
}

/* ---- Both --------------------------------------------------------------- */

/* The window is going: stop moving it, and drop its outline. */
void animate_view_gone(struct view *view) {
	view->slide_start = view->fade_start = 0;
	struct box_anim *a = find(view->server, view);
	if (a != NULL) {
		finish(a, false);
	}
}

/* Each output frame: moves everything on. Returns whether anything is
 * still moving (and so wants another frame). */
bool animate_tick(struct server *server) {
	int64_t now = now_ms();
	int ms = server->anim_ms > 0 ? server->anim_ms : 1;
	bool moving = false, relayout = false;

	struct box_anim *a, *tmp;
	wl_list_for_each_safe(a, tmp, &server->animations, link) {
		if (now - a->start >= ms) {
			finish(a, true);
			relayout = true; /* it's shown now, unless the layout says not */
		} else {
			struct wlr_box b = step_box(a, now, ms);
			outline_place(a, &b);
			moving = true;
		}
	}

	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->slide_start != 0) {
			double t = (double)(now - view->slide_start) / ms;
			if (t >= 1) {
				view->slide_start = 0;
				wlr_scene_node_set_position(&view->tree->node,
					view->x, view->y);
				/* Scrolling hides columns off screen; one that slid off
				 * can go now. */
				relayout |= server->mode == MODE_SCROLLING;
			} else {
				int x, y;
				slide_position(view, t, &x, &y);
				wlr_scene_node_set_position(&view->tree->node, x, y);
				moving = true;
			}
		}
		if (view->fade_start != 0) {
			double t = (double)(now - view->fade_start) / ms;
			if (t >= 1) {
				view->fade_start = 0;
				fade_to(view, 1);
			} else {
				fade_to(view, (float)ease_out(t));
				moving = true;
			}
		}
	}
	if (relayout) {
		tile_arrange(server);
		fullscreen_update(server);
	}
	return moving;
}
