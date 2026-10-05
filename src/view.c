#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include "cairo_buffer.h"
#include "frame.h"
#include "server.h"

/* Size of the frame around the client's window geometry. */
/* The window's gadgets: the scroll bars its client drives, and a sizer
 * unless the layout decides its size. */
void view_frame_style(struct view *view, struct frame_style *style) {
	style->v = view->scroll_v;
	style->h = view->scroll_h;
	style->sizer = !view_is_tiled(view);
}

/* Whether we draw a frame round it: not if the client does its own, nor
 * while it's fullscreen. */
static bool framed(struct view *view) {
	return view->ssd && !view->fullscreen;
}

void view_extents(struct view *view, int *w, int *h) {
	if (!framed(view)) {
		*w = *h = 0;
		return;
	}
	struct frame_style style;
	view_frame_style(view, &style);
	int right, bottom;
	frame_extents(&style, &right, &bottom);
	*w = FRAME_LEFT + right;
	*h = FRAME_TOP + bottom;
}

void view_frame_box(struct view *view, struct wlr_box *box) {
	struct wlr_box *geo = &view->xdg_toplevel->base->geometry;
	int ew, eh;
	view_extents(view, &ew, &eh);
	box->x = view->x;
	box->y = view->y;
	box->width = geo->width + ew;
	box->height = geo->height + eh;
}

void view_move(struct view *view, int x, int y) {
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
}

/* The focus highlight: a thick border outside the frame of the focused
 * window, shown while Super is held or windows are being cycled. */
/* The dither never takes clicks: they go to the window underneath. */
static bool dim_accepts_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	return false;
}

/* Greys out an unfocused window the way GEM shows disabled things: a
 * checkerboard of semi-transparent black and white dots over it, so it
 * reads as grey on dark and light windows alike. */
static void view_update_dim(struct view *view) {
	struct server *server = view->server;
	bool on = server->dim == DIM_ALWAYS ||
		(server->dim == DIM_TILING && server->mode != MODE_WINDOW);
	bool show = on && view->workspace != NULL &&
		view != server->focused_view && server->dim_opacity > 0;
	wlr_scene_node_set_enabled(&view->dim->node, show);
	if (!show) {
		return;
	}
	struct wlr_box box;
	view_frame_box(view, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	if (box.width != view->dim_w || box.height != view->dim_h ||
			server->dim_opacity != view->dim_drawn_opacity) {
		struct cairo_buffer *buffer = cairo_buffer_create(box.width, box.height);
		if (buffer == NULL) {
			return;
		}
		uint32_t *px = (uint32_t *)cairo_image_surface_get_data(buffer->surface);
		int stride = cairo_image_surface_get_stride(buffer->surface) / 4;
		uint32_t a = (uint32_t)(server->dim_opacity * 255 + 0.5);
		/* Premultiplied ARGB: black dots on one diagonal, white on the
		 * other, clear in between. */
		uint32_t dark = a << 24, light = (a << 24) | (a << 16) | (a << 8) | a;
		for (int y = 0; y < box.height; y++) {
			for (int x = 0; x < box.width; x++) {
				px[y * stride + x] = (x + y) % 2 ? 0 :
					(y % 2 ? light : dark);
			}
		}
		cairo_surface_mark_dirty(buffer->surface);
		cairo_buffer_submit(buffer, view->dim);
		view->dim_w = box.width;
		view->dim_h = box.height;
		view->dim_drawn_opacity = server->dim_opacity;
	}
	wlr_scene_node_raise_to_top(&view->dim->node);
}

static void view_update_highlight(struct view *view) {
	struct server *server = view->server;
	view_update_dim(view);
	bool show = view == server->focused_view && view->workspace != NULL &&
		(server->super_held || server->cycle_views != NULL ||
			(server->highlight_tiling && view_is_tiled(view))) &&
		server->highlight_width > 0;
	wlr_scene_node_set_enabled(&view->highlight->node, show);
	if (!show) {
		return;
	}
	struct wlr_box box;
	view_frame_box(view, &box);
	int t = server->highlight_width, w = box.width, h = box.height;
	struct wlr_box r[4] = {
		{ -t, -t, w + 2 * t, t }, /* top */
		{ -t, h, w + 2 * t, t },  /* bottom */
		{ -t, 0, t, h },          /* left */
		{ w, 0, t, h },           /* right */
	};
	for (int i = 0; i < 4; i++) {
		struct wlr_scene_rect *rect = view->highlight_rects[i];
		wlr_scene_rect_set_size(rect, r[i].width, r[i].height);
		wlr_scene_rect_set_color(rect, server->highlight_color);
		wlr_scene_node_set_position(&rect->node, r[i].x, r[i].y);
	}
	wlr_scene_node_raise_to_top(&view->highlight->node);
}

void highlight_update(struct server *server) {
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		view_update_highlight(view);
	}
}

void view_update_frame(struct view *view) {
	struct wlr_box *geo = &view->xdg_toplevel->base->geometry;
	int ox = framed(view) ? FRAME_LEFT : 0;
	int oy = framed(view) ? FRAME_TOP : 0;
	/* The xdg scene tree already shifts the surface by the geometry's
	 * offset, so its origin is the geometry's top-left. Popups are placed
	 * relative to that too. */
	wlr_scene_node_set_position(&view->content->node, ox, oy);
	wlr_scene_node_set_position(&view->popups->node, ox, oy);
	/* While it grows or shrinks (animate.c), it's drawn at the size it's
	 * got to, not the client's. */
	int cw = view->sizing ? view->anim_w : geo->width;
	int ch = view->sizing ? view->anim_h : geo->height;
	/* Inside our frame, show only the window geometry. Clients that draw
	 * their own decorations put shadows (or, like Chromium, parts of their
	 * frame) outside it, which would otherwise spill over ours. */
	struct wlr_box clip = { geo->x, geo->y, cw, ch };
	wlr_scene_subsurface_tree_set_clip(&view->content->node,
		framed(view) || view->sizing ? &clip : NULL);
	wlr_scene_node_set_enabled(&view->frame->node, framed(view));
	view_update_highlight(view);
	if (!framed(view) || cw <= 0 || ch <= 0) {
		return;
	}

	bool active = view->server->focused_view == view;
	const char *title = view->xdg_toplevel->title ?
		view->xdg_toplevel->title : "";
	struct frame_style style;
	view_frame_style(view, &style);
	if (view->drawn_w == cw && view->drawn_h == ch &&
			view->drawn_active == active && view->drawn_title != NULL &&
			strcmp(view->drawn_title, title) == 0 &&
			memcmp(&view->drawn_style, &style, sizeof(style)) == 0) {
		return;
	}
	frame_draw(view->frame, &style, cw, ch, title, active);
	view->drawn_style = style;
	view->drawn_w = cw;
	view->drawn_h = ch;
	view->drawn_active = active;
	free(view->drawn_title);
	view->drawn_title = strdup(title);
}

void focus_view(struct view *view) {
	if (view == NULL) {
		return;
	}
	struct server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = view->xdg_toplevel->base->surface;
	/* Focus going anywhere but the cycle's own step (a new window, a
	 * click) ends the cycle first, there, so letting go of Super later
	 * doesn't raise the cycle's pick over this. */
	if (server->cycle_views != NULL &&
			view != server->cycle_views[server->cycle_index]) {
		view_cycle_end(server);
	}
	struct view *prev = server->focused_view;
	if (prev == view && seat->keyboard_state.focused_surface == surface) {
		return;
	}
	if (view->workspace != NULL && view->workspace != server->active_workspace) {
		/* Switching focuses the top window there; this one follows. */
		workspace_switch(server, view->workspace);
		prev = server->focused_view;
	}
	/* Nor behind a fullscreen window: that one leaves fullscreen. */
	struct view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other != view && other->fullscreen &&
				other->workspace == view->workspace) {
			view_set_fullscreen(other, false);
			break;
		}
	}
	/* Focus can't land on a tile hidden behind a zoomed one. */
	if (view->workspace != NULL && view->workspace->zoomed != NULL &&
			view->workspace->zoomed != view && view_is_tiled(view)) {
		tile_unzoom(view->workspace);
	}

	server->focused_view = view;
	if (prev != NULL && prev != view) {
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		view_update_frame(prev);
	}

	wlr_scene_node_raise_to_top(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
	view_update_frame(view);

	/* An exclusive layer surface (an alert box) keeps the keyboard until
	 * it goes away; unmapping it gives the keyboard back to this window. */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL && layers_exclusive_focus(server) == NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	/* Scrolling mode brings the focused column into view. */
	if (server->mode == MODE_SCROLLING && view->column != NULL) {
		view->column->focus = view;
		tile_arrange(server);
	}
	highlight_update(server);
	ipc_notify_windows(server);
}

struct view *view_at(struct server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy, bool *on_frame) {
	*surface = NULL;
	*on_frame = false;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL) {
		return NULL;
	}
	/* Any client surface: a window, a popup or a layer surface. */
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (scene_surface != NULL) {
			*surface = scene_surface->surface;
		}
	}
	/* A view's root tree is the only one with data set. Layer surfaces and
	 * the desktop have none, so they return no view. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	struct view *view = tree->node.data;
	if (node == &view->frame->node) {
		*on_frame = true;
	}
	return view;
}

/* ---- Cycling ------------------------------------------------------------ */

/* Steps focus through the current workspace's windows in the order they
 * were last used, like Alt+Tab. While the key's modifier is held (`held`)
 * the order is frozen, so repeated presses walk the whole list; releasing
 * it ends the cycle (view_cycle_end, from the keyboard code). */
void view_cycle(struct server *server, int direction, bool held) {
	if (server->mode != MODE_WINDOW) {
		return; /* tiling and scrolling move focus with Super+Arrows */
	}
	if (server->cycle_views == NULL) {
		int n = 0;
		struct view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->workspace == server->active_workspace) {
				n++;
			}
		}
		if (n < 2) {
			return;
		}
		server->cycle_views = calloc(n, sizeof(*server->cycle_views));
		server->cycle_count = 0;
		wl_list_for_each(view, &server->views, link) {
			if (view->workspace == server->active_workspace) {
				server->cycle_views[server->cycle_count++] = view;
			}
		}
		server->cycle_index = 0;
	}
	int n = server->cycle_count;
	server->cycle_index = ((server->cycle_index + direction) % n + n) % n;
	focus_view(server->cycle_views[server->cycle_index]);
	if (!held) {
		view_cycle_end(server);
	}
}

void view_cycle_end(struct server *server) {
	if (server->cycle_views == NULL) {
		return;
	}
	/* Each step raised a window to show it. Put the ones passed over back
	 * in their old order, so only the chosen window moves to the front:
	 * the next Super+Tab then returns to the previous window. */
	struct view *chosen = server->cycle_views[server->cycle_index];
	for (int i = server->cycle_count - 1; i >= 0; i--) {
		struct view *view = server->cycle_views[i];
		if (view != chosen) {
			wlr_scene_node_raise_to_top(&view->tree->node);
			wl_list_remove(&view->link);
			wl_list_insert(&server->views, &view->link);
		}
	}
	wlr_scene_node_raise_to_top(&chosen->tree->node);
	wl_list_remove(&chosen->link);
	wl_list_insert(&server->views, &chosen->link);
	free(server->cycle_views);
	server->cycle_views = NULL;
	server->cycle_count = 0;
	highlight_update(server);
}

/* ---- Outline dragging --------------------------------------------------- */

static void outline_show(struct server *server, const struct wlr_box *b) {
	/* A black line with a white line inside, visible on any background. */
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
		struct wlr_scene_rect *rect = server->outline_rects[i];
		wlr_scene_rect_set_size(rect, r[i].width > 0 ? r[i].width : 0,
			r[i].height > 0 ? r[i].height : 0);
		wlr_scene_node_set_position(&rect->node, r[i].x, r[i].y);
	}
	wlr_scene_node_raise_to_top(&server->outline->node);
	wlr_scene_node_set_enabled(&server->outline->node, true);
}

static void outline_hide(struct server *server) {
	wlr_scene_node_set_enabled(&server->outline->node, false);
}

void view_begin_interactive(struct view *view, enum cursor_mode mode,
		uint32_t edges) {
	struct server *server = view->server;
	if (view_is_tiled(view)) {
		return; /* the layout decides where tiles go */
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;
	server->grab_x = server->cursor->x;
	server->grab_y = server->cursor->y;
	server->resize_edges = edges;
	view_frame_box(view, &server->grab_box);
	server->drag_box = server->grab_box;
	wlr_seat_pointer_clear_focus(server->seat);
	outline_show(server, &server->drag_box);
}

void process_interactive_motion(struct server *server) {
	struct view *view = server->grabbed_view;
	int dx = (int)(server->cursor->x - server->grab_x);
	int dy = (int)(server->cursor->y - server->grab_y);
	struct wlr_box b = server->grab_box;

	if (server->cursor_mode == CURSOR_SCROLL) {
		/* The slider follows the pointer; the client is told the matching
		 * position, and redraws the frame by reporting it back. */
		bool vertical = server->grab_vertical;
		struct frame_axis *a = vertical ? &view->scroll_v : &view->scroll_h;
		struct wlr_box *geo = &view->xdg_toplevel->base->geometry;
		struct frame_style style;
		view_frame_style(view, &style);
		int track, slider;
		frame_slider_size(&style, geo->width, geo->height, vertical,
			&track, &slider);
		if (track > slider) {
			double per_px = (double)(a->total - a->visible) / (track - slider);
			view_scroll_to(view, vertical,
				server->grab_position + (int)((vertical ? dy : dx) * per_px));
		}
		return;
	}

	if (server->cursor_mode == CURSOR_MOVE) {
		b.x += dx;
		b.y += dy;
		/* Like GEM, windows can't be dragged up under the menu bar. */
		struct wlr_box usable;
		if (output_usable_area_at(server, server->cursor->x,
				server->cursor->y, &usable) && b.y < usable.y) {
			b.y = usable.y;
		}
	} else if (server->cursor_mode == CURSOR_RESIZE) {
		int ew, eh;
		view_extents(view, &ew, &eh);
		int min_w = ew + (view->ssd ? FRAME_MIN_W : 1);
		int min_h = eh + (view->ssd ? FRAME_MIN_H : 1);
		uint32_t edges = server->resize_edges;
		if (edges & WLR_EDGE_RIGHT) {
			b.width += dx;
		} else if (edges & WLR_EDGE_LEFT) {
			b.x += dx;
			b.width -= dx;
		}
		if (edges & WLR_EDGE_BOTTOM) {
			b.height += dy;
		} else if (edges & WLR_EDGE_TOP) {
			b.y += dy;
			b.height -= dy;
		}
		if (b.width < min_w) {
			if (edges & WLR_EDGE_LEFT) {
				b.x = server->grab_box.x + server->grab_box.width - min_w;
			}
			b.width = min_w;
		}
		if (b.height < min_h) {
			if (edges & WLR_EDGE_TOP) {
				b.y = server->grab_box.y + server->grab_box.height - min_h;
			}
			b.height = min_h;
		}
	}
	server->drag_box = b;
	outline_show(server, &b);
}

void end_interactive(struct server *server) {
	struct view *view = server->grabbed_view;
	struct wlr_box b = server->drag_box;
	enum cursor_mode mode = server->cursor_mode;
	server->cursor_mode = CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	outline_hide(server);
	if (view == NULL || mode == CURSOR_SCROLL) {
		return;
	}

	if (mode == CURSOR_RESIZE) {
		int ew, eh;
		view_extents(view, &ew, &eh);
		if (view->maximized) {
			view->maximized = false;
			wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
		}
		wlr_xdg_toplevel_set_size(view->xdg_toplevel,
			b.width - ew, b.height - eh);
	}
	view_move(view, b.x, b.y);
}

/* ---- Gadgets ------------------------------------------------------------ */

void view_toggle_maximize(struct view *view) {
	struct server *server = view->server;
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	if (view_is_tiled(view)) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		return;
	}
	int ew, eh;
	view_extents(view, &ew, &eh);

	if (view->maximized) {
		struct wlr_box *s = &view->saved_box;
		view->maximized = false;
		wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
		wlr_xdg_toplevel_set_size(view->xdg_toplevel,
			s->width - ew, s->height - eh);
		view_move(view, s->x, s->y);
		ipc_notify_windows(server);
		return;
	}

	struct wlr_box cur, area;
	view_frame_box(view, &cur);
	if (!output_usable_area_at(server, cur.x + cur.width / 2.0,
				cur.y + cur.height / 2.0, &area) &&
			!output_usable_area_at(server, server->cursor->x,
				server->cursor->y, &area)) {
		return;
	}

	view->saved_box = cur;
	view->maximized = true;
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
	wlr_xdg_toplevel_set_size(view->xdg_toplevel,
		area.width - ew, area.height - eh);
	view_move(view, area.x, area.y);
	ipc_notify_windows(server);
}

/* An arrow moves a tenth of the view; the track pages by most of one. */
static void view_scroll_by(struct view *view, bool vertical, double views) {
	struct frame_axis *a = vertical ? &view->scroll_v : &view->scroll_h;
	int step = (int)(a->visible * (views < 0 ? -views : views));
	step = step < 1 ? 1 : step;
	view_scroll_to(view, vertical, a->position + (views < 0 ? -step : step));
}

static void view_begin_slider_drag(struct view *view, bool vertical) {
	struct server *server = view->server;
	server->grabbed_view = view;
	server->cursor_mode = CURSOR_SCROLL;
	server->grab_x = server->cursor->x;
	server->grab_y = server->cursor->y;
	server->grab_vertical = vertical;
	server->grab_position = vertical ? view->scroll_v.position :
		view->scroll_h.position;
	wlr_seat_pointer_clear_focus(server->seat);
}

void view_frame_click(struct view *view, double fx, double fy, uint32_t time) {
	struct wlr_box *geo = &view->xdg_toplevel->base->geometry;
	struct frame_style style;
	view_frame_style(view, &style);
	switch (frame_part_at(&style, geo->width, geo->height, (int)fx, (int)fy)) {
	case FRAME_PART_CLOSER:
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
		break;
	case FRAME_PART_FULLER:
		view_toggle_maximize_or_zoom(view);
		break;
	case FRAME_PART_TITLE:
		view_begin_interactive(view, CURSOR_MOVE, 0);
		break;
	case FRAME_PART_SIZER:
		view_begin_interactive(view, CURSOR_RESIZE,
			WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
		break;
	case FRAME_PART_UP:
		view_scroll_by(view, true, -0.1);
		break;
	case FRAME_PART_DOWN:
		view_scroll_by(view, true, 0.1);
		break;
	case FRAME_PART_LEFT:
		view_scroll_by(view, false, -0.1);
		break;
	case FRAME_PART_RIGHT:
		view_scroll_by(view, false, 0.1);
		break;
	case FRAME_PART_PAGE_UP:
		view_scroll_by(view, true, -0.9);
		break;
	case FRAME_PART_PAGE_DOWN:
		view_scroll_by(view, true, 0.9);
		break;
	case FRAME_PART_PAGE_LEFT:
		view_scroll_by(view, false, -0.9);
		break;
	case FRAME_PART_PAGE_RIGHT:
		view_scroll_by(view, false, 0.9);
		break;
	case FRAME_PART_VSLIDER:
		view_begin_slider_drag(view, true);
		break;
	case FRAME_PART_HSLIDER:
		view_begin_slider_drag(view, false);
		break;
	default:
		break;
	}
}

/* ---- xdg-shell toplevels ------------------------------------------------ */

/* Tiling puts a new window where you're working, as i3 and sway do:
 * while only one column is in use it starts the other; after that it
 * splits the focused window's column, just below it (see tile.c).
 * Otherwise (other modes, dialogs, nothing focused) it goes last and the
 * layout finds it a column. */
static void tile_insert(struct view *view) {
	struct server *server = view->server;
	struct view *focused = server->focused_view;
	view->tile_column = -1;
	if (server->mode != MODE_TILING || !view_is_tiled(view) || focused == NULL ||
			focused->workspace != view->workspace || !view_is_tiled(focused) ||
			focused->tile_column < 0) {
		wl_list_insert(server->tiles.prev, &view->tile_link);
		return;
	}
	int count[2] = { 0, 0 };
	struct view *v;
	wl_list_for_each(v, &server->tiles, tile_link) {
		if (v->workspace == focused->workspace && view_is_tiled(v) &&
				v->tile_column >= 0 && v->tile_column <= 1) {
			count[v->tile_column]++;
		}
	}
	if (count[0] == 0 || count[1] == 0) {
		view->tile_column = count[0] > 0 ? 1 : 0; /* the other column */
		wl_list_insert(server->tiles.prev, &view->tile_link);
	} else {
		view->tile_column = focused->tile_column;
		wl_list_insert(&focused->tile_link, &view->tile_link);
	}
}

/* A dialog's window, if it's showing here. */
static struct view *view_parent(struct view *view) {
	struct wlr_xdg_toplevel *parent = view->xdg_toplevel->parent;
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (parent != NULL && v->xdg_toplevel == parent &&
				v->workspace == view->server->active_workspace) {
			return v;
		}
	}
	return NULL;
}

/* lo wins over hi: a window too big for the screen keeps its top-left. */
static int clamp(int v, int lo, int hi) {
	return v > hi ? (hi > lo ? hi : lo) : v < lo ? lo : v;
}

static void view_map(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, map);
	struct server *server = view->server;

	/* Cascade new windows from the top-left of the usable area under the
	 * pointer, like GEM's desktop opening drive windows. A dialog goes in
	 * the middle of its window instead, as GEM centred its forms, kept on
	 * screen. */
	struct wlr_box area = {0};
	output_usable_area_at(server, server->cursor->x, server->cursor->y, &area);
	struct view *parent = view_parent(view);
	if (parent != NULL) {
		struct wlr_box pb, b;
		view_frame_box(parent, &pb);
		view_frame_box(view, &b);
		int x = pb.x + (pb.width - b.width) / 2;
		int y = pb.y + (pb.height - b.height) / 2;
		x = clamp(x, area.x, area.x + area.width - b.width);
		y = clamp(y, area.y, area.y + area.height - b.height);
		view_move(view, x, y);
	} else {
		int step = server->cascade++ % 8;
		view_move(view, area.x + 32 + step * GEM_GADGET,
			area.y + 16 + step * GEM_GADGET);
	}

	view->workspace = server->active_workspace;
	wlr_scene_node_reparent(&view->tree->node, view->workspace->tree);
	wlr_scene_node_set_enabled(&view->tree->node, true);
	wl_list_insert(&server->views, &view->link);
	/* Opened in window mode, it's somewhere already: turning to tiling
	 * slides it from there. Otherwise it fades in in its place. */
	view->placed = server->mode == MODE_WINDOW;
	tile_insert(view);
	if (server->mode == MODE_SCROLLING) {
		scroll_add_view(view); /* a new column, right of the focused one */
	}
	if (view_is_tiled(view)) {
		tile_unzoom(view->workspace);
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_TOP |
			WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	}
	view_update_frame(view);
	tile_arrange(server);
	focus_view(view);
	/* Games and players may ask to start fullscreen. */
	if (view->xdg_toplevel->requested.fullscreen) {
		view_set_fullscreen(view, true);
	}
	cursor_rebase(server);
	ipc_notify_workspaces(server);
}

static void view_unmap(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, unmap);
	struct server *server = view->server;

	if (server->grabbed_view == view) {
		server->grabbed_view = NULL;
		end_interactive(server);
	}
	view_cycle_end(server);
	animate_view_gone(view);
	view->placed = false;
	view->tile_box_valid = false;
	view->fullscreen = false; /* its node goes back among the others below */
	if (view->workspace != NULL && view->workspace->zoomed == view) {
		tile_unzoom(view->workspace); /* re-shows the other tiles */
	}
	wlr_scene_node_set_enabled(&view->tree->node, false);
	/* Out of its workspace, so that workspace can be removed while this
	 * window is hidden. It rejoins the active one if it maps again. */
	wlr_scene_node_reparent(&view->tree->node, server->layer_views);
	view->workspace = NULL;
	wl_list_remove(&view->link);
	wl_list_init(&view->link);

	if (server->focused_view == view) {
		server->focused_view = NULL;
		workspace_focus_top(server);
	}
	wl_list_remove(&view->tile_link);
	wl_list_init(&view->tile_link);
	scroll_remove_view(view);
	workspace_prune(server);
	tile_arrange(server);
	cursor_rebase(server);
	ipc_notify_workspaces(server);
	ipc_notify_windows(server);
}

static void view_commit(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;

	if (toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
		if (view->xdg_decoration != NULL) {
			wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
		/* 0x0 lets the client pick its own size, but bounds tell it how
		 * much room there is: the usable area under the pointer, less our
		 * frame and a margin for the cascade. */
		struct wlr_box area;
		if (output_usable_area_at(view->server, view->server->cursor->x,
				view->server->cursor->y, &area)) {
			int ew, eh;
			view_extents(view, &ew, &eh);
			int w = area.width - ew - 2 * 32, h = area.height - eh - 2 * 16;
			if (w > 0 && h > 0) {
				wlr_xdg_toplevel_set_bounds(toplevel, w, h);
			}
		}
		wlr_xdg_toplevel_set_size(toplevel, 0, 0);
		return;
	}
	if (toplevel->base->surface->mapped) {
		view_update_frame(view);
	}
}

static void view_set_title(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, set_title);
	if (view->xdg_toplevel->base->surface->mapped) {
		view_update_frame(view);
		ipc_notify_windows(view->server);
	}
}

static void view_request_move(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_move);
	view_begin_interactive(view, CURSOR_MOVE, 0);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_resize);
	view_begin_interactive(view, CURSOR_RESIZE, event->edges);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_maximize);
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;
	if (!toplevel->base->initialized) {
		return;
	}
	if (toplevel->requested.maximized != view->maximized) {
		view_toggle_maximize(view);
	} else {
		wlr_xdg_surface_schedule_configure(toplevel->base);
	}
}

/* Shows only the fullscreen windows of the active workspace: they live
 * in their own layer, above the menu bar, not in their workspace's. */
void fullscreen_update(struct server *server) {
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->fullscreen) {
			wlr_scene_node_set_enabled(&view->tree->node,
				view->workspace == server->active_workspace &&
				!view->anim_hidden);
		}
	}
}

/* A window asks to fill its screen (a video, F11, a game), or to stop:
 * it loses its frame and covers the output it's on, menu bar and all; on
 * the way back it gets its frame and its place again. */
void view_set_fullscreen(struct view *view, bool fullscreen) {
	struct server *server = view->server;
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;
	if (!toplevel->base->initialized) {
		return;
	}
	if (fullscreen == view->fullscreen || view->workspace == NULL) {
		wlr_xdg_surface_schedule_configure(toplevel->base);
		return;
	}
	if (fullscreen) {
		struct wlr_box frame;
		view_frame_box(view, &frame);
		struct wlr_output *output = wlr_output_layout_output_at(
			server->output_layout, frame.x + frame.width / 2.0,
			frame.y + frame.height / 2.0);
		if (output == NULL) {
			output = wlr_output_layout_output_at(server->output_layout,
				server->cursor->x, server->cursor->y);
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output, &box);
		if (wlr_box_empty(&box)) {
			wlr_xdg_surface_schedule_configure(toplevel->base);
			return;
		}
		view->fullscreen_box = frame;
		view->fullscreen = true;
		wlr_scene_node_reparent(&view->tree->node, server->layer_fullscreen);
		view_move(view, box.x, box.y);
		wlr_xdg_toplevel_set_fullscreen(toplevel, true);
		wlr_xdg_toplevel_set_size(toplevel, box.width, box.height);
	} else {
		view->fullscreen = false;
		wlr_scene_node_reparent(&view->tree->node, view->workspace->tree);
		wlr_scene_node_raise_to_top(&view->tree->node);
		wlr_xdg_toplevel_set_fullscreen(toplevel, false);
		if (view_is_tiled(view)) {
			tile_arrange(server); /* back into its place in the layout */
		} else {
			struct wlr_box *b = &view->fullscreen_box;
			int ew, eh;
			view_extents(view, &ew, &eh);
			view_move(view, b->x, b->y);
			wlr_xdg_toplevel_set_size(toplevel, b->width - ew, b->height - eh);
		}
	}
	fullscreen_update(server);
	view_update_frame(view);
	cursor_rebase(server);
	ipc_notify_windows(server);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
}

static void view_destroy(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, destroy);
	struct server *server = view->server;

	if (server->grabbed_view == view) {
		server->grabbed_view = NULL;
		end_interactive(server);
	}
	if (server->focused_view == view) {
		server->focused_view = NULL;
	}
	view->xdg_toplevel->base->surface->data = NULL;
	scrollbars_view_destroyed(view);
	app_menus_view_destroyed(view);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->link);
	wl_list_remove(&view->tile_link);

	wlr_scene_node_destroy(&view->tree->node);
	free(view->drawn_title);
	free(view);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->id = ++server->next_view_id;
	view->xdg_toplevel = xdg_toplevel;
	wl_list_init(&view->link);
	wl_list_init(&view->tile_link);
	wl_list_init(&view->column_link);

	view->tree = wlr_scene_tree_create(server->layer_views);
	view->tree->node.data = view;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	view->frame = wlr_scene_buffer_create(view->tree, NULL);
	view->content = wlr_scene_xdg_surface_create(view->tree, xdg_toplevel->base);
	/* Popups get their own tree, positioned like the content, so clipping
	 * the window to its geometry doesn't cut its menus off. */
	view->popups = wlr_scene_tree_create(view->tree);
	view->highlight = wlr_scene_tree_create(view->tree);
	for (int i = 0; i < 4; i++) {
		view->highlight_rects[i] = wlr_scene_rect_create(view->highlight,
			0, 0, server->highlight_color);
	}
	wlr_scene_node_set_enabled(&view->highlight->node, false);
	view->dim = wlr_scene_buffer_create(view->tree, NULL);
	view->dim->point_accepts_input = dim_accepts_input;
	wlr_scene_node_set_enabled(&view->dim->node, false);
	/* Popups look up their parent's scene tree through xdg_surface.data;
	 * decorations look up the view through wlr_surface.data. */
	xdg_toplevel->base->data = view->popups;
	xdg_toplevel->base->surface->data = view;
	/* Every window gets a GEM frame unless it explicitly asks to draw its
	 * own (KDE decoration mode "client"). */
	view->ssd = true;

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize,
		&view->request_maximize);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&view->request_fullscreen);
	view->set_title.notify = view_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
}

/* ---- xdg-shell popups (menus, tooltips) --------------------------------- */

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void popup_commit(struct wl_listener *listener, void *data) {
	struct popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* Popups of layer surfaces get their parent later, and are added to the
	 * scene by layer.c. */
	struct wlr_xdg_surface *parent = xdg_popup->parent == NULL ? NULL :
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent != NULL) {
		struct wlr_scene_tree *parent_tree = parent->data;
		xdg_popup->base->data =
			wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	}

	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

void view_init_shell(struct server *server) {
	wl_list_init(&server->views);
	wl_list_init(&server->tiles);
	wl_list_init(&server->animations);
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 5);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	server->outline = wlr_scene_tree_create(server->layer_drag);
	const float black[4] = { 0, 0, 0, 1 };
	const float white[4] = { 1, 1, 1, 1 };
	for (int i = 0; i < 8; i++) {
		server->outline_rects[i] = wlr_scene_rect_create(server->outline,
			0, 0, i < 4 ? black : white);
	}
	outline_hide(server);
}

/* ---- Decorations -------------------------------------------------------- */

/* xdg-decoration: we always ask for server-side, so GTK/Qt/foot etc. drop
 * their own title bars and get a GEM frame instead. */
struct xdg_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static void xdg_decoration_request_mode(struct wl_listener *listener,
		void *data) {
	struct xdg_decoration *deco =
		wl_container_of(listener, deco, request_mode);
	if (deco->wlr->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void xdg_decoration_destroy(struct wl_listener *listener, void *data) {
	struct xdg_decoration *deco = wl_container_of(listener, deco, destroy);
	struct view *view = deco->wlr->toplevel->base->surface->data;
	if (view != NULL) {
		view->xdg_decoration = NULL;
	}
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_xdg_decoration(struct wl_listener *listener,
		void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *wlr = data;
	struct xdg_decoration *deco = calloc(1, sizeof(*deco));
	deco->wlr = wlr;
	deco->request_mode.notify = xdg_decoration_request_mode;
	wl_signal_add(&wlr->events.request_mode, &deco->request_mode);
	deco->destroy.notify = xdg_decoration_destroy;
	wl_signal_add(&wlr->events.destroy, &deco->destroy);

	struct view *view = wlr->toplevel->base->surface->data;
	if (view != NULL) {
		view->xdg_decoration = wlr;
	}
	/* Before the initial commit, view_commit sets the mode instead. */
	if (wlr->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

/* org_kde_kwin_server_decoration: the older protocol GTK3 uses. */
struct kde_decoration {
	struct wlr_server_decoration *wlr;
	struct wl_listener mode;
	struct wl_listener destroy;
};

static void kde_decoration_mode(struct wl_listener *listener, void *data) {
	struct kde_decoration *deco = wl_container_of(listener, deco, mode);
	struct view *view = deco->wlr->surface->data;
	if (view == NULL) {
		return;
	}
	view->ssd = deco->wlr->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
	if (view->xdg_toplevel->base->surface->mapped) {
		view_update_frame(view);
	}
}

static void kde_decoration_destroy(struct wl_listener *listener, void *data) {
	struct kde_decoration *deco = wl_container_of(listener, deco, destroy);
	wl_list_remove(&deco->mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_kde_decoration(struct wl_listener *listener,
		void *data) {
	struct kde_decoration *deco = calloc(1, sizeof(*deco));
	deco->wlr = data;
	deco->mode.notify = kde_decoration_mode;
	wl_signal_add(&deco->wlr->events.mode, &deco->mode);
	deco->destroy.notify = kde_decoration_destroy;
	wl_signal_add(&deco->wlr->events.destroy, &deco->destroy);
	kde_decoration_mode(&deco->mode, NULL);
}

/* xdg-activation: an app asks for one of its windows to come forward (a
 * GTK app handed a file or a link while running, say). GemWM grants it:
 * the window is focused, on its own workspace. */
static void server_request_activate(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
	if (toplevel == NULL || !toplevel->base->surface->mapped) {
		return;
	}
	focus_view(toplevel->base->surface->data);
}

void view_init_activation(struct server *server) {
	struct wlr_xdg_activation_v1 *activation =
		wlr_xdg_activation_v1_create(server->wl_display);
	server->request_activate.notify = server_request_activate;
	wl_signal_add(&activation->events.request_activate,
		&server->request_activate);
}

void view_init_decorations(struct server *server) {
	struct wlr_xdg_decoration_manager_v1 *xdg =
		wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_xdg_decoration.notify = server_new_xdg_decoration;
	wl_signal_add(&xdg->events.new_toplevel_decoration,
		&server->new_xdg_decoration);

	struct wlr_server_decoration_manager *kde =
		wlr_server_decoration_manager_create(server->wl_display);
	wlr_server_decoration_manager_set_default_mode(kde,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server->new_kde_decoration.notify = server_new_kde_decoration;
	wl_signal_add(&kde->events.new_decoration, &server->new_kde_decoration);
}
