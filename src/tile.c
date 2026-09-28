/*
 * Tiling mode: windows share the screen in two columns, each a stack of
 * windows top to bottom.
 *
 *   +--------------+-------------+
 *   |   left 1     |  right 1    |
 *   +--------------+-------------+
 *   |   left 2     |  right 2    |
 *   |              +-------------+
 *   |              |  right 3    |
 *   +--------------+-------------+
 *
 * A new window goes where you're working, as in i3 and sway: while only
 * one column is in use it starts the other (on the right); after that it
 * splits the focused window's column, just below it (view.c). A column
 * left empty gives the other the whole width. Windows arriving without a
 * column (from another mode or workspace) take the left if it's free,
 * else the right, so switching modes gives the familiar layout.
 *
 * One window fills the screen. `gap` pixels separate the tiles and the
 * screen edges. Windows keep their GEM frames; the gap is between frames.
 * Dialogs (windows with a parent) float on top instead. Switching back to
 * window mode puts every window back where it was.
 */
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include "frame.h"
#include "server.h"

bool view_is_tiled(struct view *view) {
	return view->server->mode != MODE_WINDOW &&
		view->xdg_toplevel->parent == NULL &&
		view->workspace != NULL;
}

/* The tiled windows of a workspace, in the order they were opened. */
static int workspace_tiles(struct workspace *ws, struct view **out, int max) {
	int n = 0;
	struct view *view;
	wl_list_for_each(view, &ws->server->tiles, tile_link) {
		if (view->workspace == ws && view_is_tiled(view) && n < max) {
			out[n++] = view;
		}
	}
	return n;
}

void tile_place(struct view *view, int x, int y, int w, int h) {
	int ew, eh;
	view_extents(view, &ew, &eh);
	if (w - ew < 1 || h - eh < 1) {
		return;
	}
	view_move(view, x, y);
	/* Arranging runs often (every focus change when scrolling): only ask
	 * the client to resize when the size actually changes. */
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;
	if (toplevel->scheduled.width != w - ew ||
			toplevel->scheduled.height != h - eh) {
		wlr_xdg_toplevel_set_size(toplevel, w - ew, h - eh);
	}
}

/* A column's windows, top to bottom, sharing its height. */
static void place_column(struct view **views, int k, int x, int w,
		struct wlr_box area, int g) {
	int avail = area.height - (k + 1) * g;
	int y = area.y + g;
	for (int i = 0; i < k; i++) {
		/* The last one takes whatever rounding left over. */
		int h = i == k - 1 ? area.y + area.height - g - y : avail / k;
		tile_place(views[i], x, y, w, h);
		y += h + g;
	}
}

static void arrange_workspace(struct workspace *ws, struct wlr_box area) {
	struct server *server = ws->server;
	struct view *tiles[256];
	int n = workspace_tiles(ws, tiles, 256);
	int g = server->gap;
	if (n == 0) {
		return;
	}
	/* A maximized ("zoomed") tile fills the screen; the others stay open
	 * but hidden until it's restored. */
	for (int i = 0; i < n; i++) {
		wlr_scene_node_set_enabled(&tiles[i]->tree->node,
			ws->zoomed == NULL || tiles[i] == ws->zoomed);
	}
	if (ws->zoomed != NULL) {
		tile_place(ws->zoomed, area.x + g, area.y + g,
			area.width - 2 * g, area.height - 2 * g);
		return;
	}
	struct view *columns[2][256];
	int count[2] = { 0, 0 };
	for (int i = 0; i < n; i++) {
		struct view *view = tiles[i];
		if (view->tile_column < 0 || view->tile_column > 1) {
			view->tile_column = count[0] == 0 ? 0 : 1;
		}
		columns[view->tile_column][count[view->tile_column]++] = view;
	}
	/* One column in use: it has the whole width. */
	if (count[0] == 0 || count[1] == 0) {
		int c = count[0] > 0 ? 0 : 1;
		place_column(columns[c], count[c], area.x + g, area.width - 2 * g,
			area, g);
		return;
	}
	int left_w = (area.width - 3 * g) / 2;
	place_column(columns[0], count[0], area.x + g, left_w, area, g);
	place_column(columns[1], count[1], area.x + 2 * g + left_w,
		area.width - 3 * g - left_w, area, g);
}

void tile_arrange(struct server *server) {
	/* At startup the config is read before the cursor and outputs exist;
	 * the first output's arrival arranges everything. */
	if (server->mode == MODE_WINDOW || server->cursor == NULL) {
		return;
	}
	struct wlr_box area;
	if (!output_usable_area_at(server, server->cursor->x, server->cursor->y,
			&area)) {
		return;
	}
	struct workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (server->mode == MODE_SCROLLING) {
			scroll_arrange_workspace(ws, area);
		} else {
			arrange_workspace(ws, area);
		}
	}
}

/* Puts a workspace's zoomed tile back into the layout. */
void tile_unzoom(struct workspace *ws) {
	if (ws == NULL || ws->zoomed == NULL) {
		return;
	}
	struct view *view = ws->zoomed;
	ws->zoomed = NULL;
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, false);
	struct view *tiles[256];
	int n = workspace_tiles(ws, tiles, 256);
	for (int i = 0; i < n; i++) {
		wlr_scene_node_set_enabled(&tiles[i]->tree->node, true);
	}
	tile_arrange(ws->server);
	cursor_rebase(ws->server);
	ipc_notify_windows(ws->server);
}

/* Super+Z on a tile: fill the screen with it, or put it back. */
void tile_toggle_zoom(struct view *view) {
	struct workspace *ws = view->workspace;
	if (!view_is_tiled(view) || ws == NULL ||
			view->server->mode != MODE_TILING) {
		return;
	}
	if (ws->zoomed == view) {
		tile_unzoom(ws);
		return;
	}
	tile_unzoom(ws);
	ws->zoomed = view;
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, true);
	tile_arrange(view->server);
	focus_view(view);
	cursor_rebase(view->server);
	ipc_notify_windows(view->server);
}

/* What the fuller gadget and Super+Z do: zoom a tile, or maximize a
 * window in window mode. */
void view_toggle_maximize_or_zoom(struct view *view) {
	if (view->server->mode == MODE_SCROLLING && view->column != NULL) {
		scroll_toggle_full(view);
	} else if (view_is_tiled(view)) {
		tile_toggle_zoom(view);
	} else {
		view_toggle_maximize(view);
	}
}

void tile_set_mode(struct server *server, enum layout_mode mode) {
	enum layout_mode old = server->mode;
	if (old == mode) {
		return;
	}
	if (server->grabbed_view != NULL) {
		server->drag_box = server->grab_box;
		end_interactive(server);
	}
	view_cycle_end(server);

	struct view *view;
	struct workspace *ws;
	if (old == MODE_WINDOW) {
		/* Remember where every window was, to put it back later. */
		wl_list_for_each(view, &server->views, link) {
			if (view->maximized) {
				view_toggle_maximize(view);
			}
			view_frame_box(view, &view->float_box);
			if (view->xdg_toplevel->parent == NULL) {
				wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_TOP |
					WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
			}
		}
	} else if (old == MODE_TILING) {
		wl_list_for_each(ws, &server->workspaces, link) {
			tile_unzoom(ws);
		}
	} else if (old == MODE_SCROLLING) {
		scroll_clear(server);
	}

	server->mode = mode;
	if (mode == MODE_SCROLLING) {
		scroll_build(server);
	}
	if (mode == MODE_WINDOW) {
		wl_list_for_each(view, &server->views, link) {
			if (view->xdg_toplevel->parent != NULL) {
				continue;
			}
			struct wlr_box *b = &view->float_box;
			int ew, eh;
			view_extents(view, &ew, &eh);
			wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_NONE);
			/* Windows opened while arranged have no old place: they
			 * cascade from the top-left, like newly opened windows. */
			if (b->width > ew && b->height > eh) {
				view_move(view, b->x, b->y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					b->width - ew, b->height - eh);
			} else {
				struct wlr_box area = {0};
				output_usable_area_at(server, server->cursor->x,
					server->cursor->y, &area);
				int step = server->cascade++ % 8;
				view_move(view, area.x + 32 + step * GEM_GADGET,
					area.y + 16 + step * GEM_GADGET);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
			}
		}
	} else {
		tile_arrange(server);
	}
	highlight_update(server);
	cursor_rebase(server);
	ipc_notify_mode(server);
}

/* The nearest tile to from in a direction ("left", "right", "up", "down"),
 * or NULL. Only windows wholly past that edge of it count, so Down from a
 * full-height tile finds nothing, as in i3. */
static struct view *neighbour(struct view *from, const char *dir) {
	struct view *tiles[256];
	int n = workspace_tiles(from->workspace, tiles, 256);
	struct wlr_box f;
	view_frame_box(from, &f);
	struct view *best = NULL;
	int best_score = 0;
	for (int i = 0; i < n; i++) {
		if (tiles[i] == from) {
			continue;
		}
		struct wlr_box b;
		view_frame_box(tiles[i], &b);
		int distance, offset;
		if (strcmp(dir, "left") == 0 && b.x + b.width <= f.x) {
			distance = f.x - (b.x + b.width);
		} else if (strcmp(dir, "right") == 0 && b.x >= f.x + f.width) {
			distance = b.x - (f.x + f.width);
		} else if (strcmp(dir, "up") == 0 && b.y + b.height <= f.y) {
			distance = f.y - (b.y + b.height);
		} else if (strcmp(dir, "down") == 0 && b.y >= f.y + f.height) {
			distance = b.y - (f.y + f.height);
		} else {
			continue; /* not (wholly) in that direction */
		}
		/* Among those, prefer the one most in line with this window. */
		bool horizontal = dir[0] == 'l' || dir[0] == 'r';
		offset = horizontal ?
			abs((b.y + b.height / 2) - (f.y + f.height / 2)) :
			abs((b.x + b.width / 2) - (f.x + f.width / 2));
		int score = distance + 2 * offset;
		if (best == NULL || score < best_score) {
			best = tiles[i];
			best_score = score;
		}
	}
	return best;
}

/* Moves focus to the nearest tile in a direction. */
bool tile_focus_direction(struct server *server, const char *dir) {
	struct view *tiles[256];
	int n = workspace_tiles(server->active_workspace, tiles, 256);
	struct view *from = server->focused_view;
	if (n == 0) {
		return false;
	}
	if (from == NULL || !view_is_tiled(from)) {
		focus_view(tiles[0]);
		return true;
	}
	if (server->active_workspace->zoomed != NULL) {
		tile_unzoom(server->active_workspace);
	}
	struct view *best = neighbour(from, dir);
	if (best != NULL) {
		focus_view(best);
	}
	return best != NULL;
}

/* The focused tile, if tiling and it's arranged: what swap and push move. */
static struct view *focused_tile(struct server *server) {
	struct view *view = server->focused_view;
	if (server->mode != MODE_TILING || view == NULL || !view_is_tiled(view) ||
			view->tile_column < 0) {
		return NULL;
	}
	tile_unzoom(view->workspace);
	return view;
}

static void moved(struct view *view) {
	tile_arrange(view->server);
	focus_view(view);
	cursor_rebase(view->server);
	ipc_notify_windows(view->server);
}

/* Super+Ctrl+Arrow: the focused tile trades places with its neighbour
 * that way, column and all. */
bool tile_swap(struct server *server, const char *dir) {
	struct view *view = focused_tile(server);
	struct view *other = view ? neighbour(view, dir) : NULL;
	if (other == NULL) {
		return false;
	}
	/* Swap their places in the tile order... */
	struct wl_list *a = &view->tile_link, *b = &other->tile_link;
	if (a->next == b) {
		wl_list_remove(a);
		wl_list_insert(b, a);
	} else if (b->next == a) {
		wl_list_remove(b);
		wl_list_insert(a, b);
	} else {
		struct wl_list *before_a = a->prev;
		wl_list_remove(a);
		wl_list_insert(b, a);
		wl_list_remove(b);
		wl_list_insert(before_a, b);
	}
	/* ...and their columns. */
	int column = view->tile_column;
	view->tile_column = other->tile_column;
	other->tile_column = column;
	moved(view);
	return true;
}

/* Super+Shift+Left/Right: the focused tile leaves its column for the other
 * one, below the window it lands beside. A window alone in its column
 * stays; a single full-width column gains a second, on that side. */
bool tile_push(struct server *server, const char *dir) {
	struct view *view = focused_tile(server);
	bool left = strcmp(dir, "left") == 0;
	if (view == NULL || (!left && strcmp(dir, "right") != 0)) {
		return false;
	}
	struct view *tiles[256];
	int n = workspace_tiles(view->workspace, tiles, 256);
	int count[2] = { 0, 0 };
	for (int i = 0; i < n; i++) {
		if (tiles[i]->tile_column == 0 || tiles[i]->tile_column == 1) {
			count[tiles[i]->tile_column]++;
		}
	}
	int from = view->tile_column, to = left ? 0 : 1;
	if (count[from] < 2) {
		return false; /* it would leave its column empty */
	}
	if (count[1 - from] == 0) {
		/* One column: the others go to the far side, this one to dir. */
		for (int i = 0; i < n; i++) {
			tiles[i]->tile_column = 1 - to;
		}
		view->tile_column = to;
		moved(view);
		return true;
	}
	if (from == to) {
		return false; /* already the column on that side */
	}
	struct view *beside = neighbour(view, dir);
	wl_list_remove(&view->tile_link);
	if (beside != NULL) {
		wl_list_insert(&beside->tile_link, &view->tile_link);
	} else {
		wl_list_insert(server->tiles.prev, &view->tile_link);
	}
	view->tile_column = to;
	moved(view);
	return true;
}
