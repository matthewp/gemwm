/*
 * Tiling mode: windows share the screen, master and stack, like dwm.
 *
 *   +--------------+-------------+
 *   |              |  stack 1    |
 *   |   master     +-------------+
 *   |  (first)     |  stack 2    |
 *   |              +-------------+
 *   |              |  stack 3    |
 *   +--------------+-------------+
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
#include "server.h"

bool view_is_tiled(struct view *view) {
	return view->server->tiling && view->xdg_toplevel->parent == NULL &&
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

static void place(struct view *view, int x, int y, int w, int h) {
	int ew, eh;
	view_extents(view, &ew, &eh);
	if (w - ew < 1 || h - eh < 1) {
		return;
	}
	view_move(view, x, y);
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, w - ew, h - eh);
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
		place(ws->zoomed, area.x + g, area.y + g,
			area.width - 2 * g, area.height - 2 * g);
		return;
	}
	if (n == 1) {
		place(tiles[0], area.x + g, area.y + g,
			area.width - 2 * g, area.height - 2 * g);
		return;
	}
	int master_w = (area.width - 3 * g) / 2;
	int stack_x = area.x + 2 * g + master_w;
	int stack_w = area.width - 3 * g - master_w;
	place(tiles[0], area.x + g, area.y + g, master_w, area.height - 2 * g);

	int k = n - 1;
	int avail = area.height - (k + 1) * g;
	int y = area.y + g;
	for (int i = 0; i < k; i++) {
		/* The last one takes whatever rounding left over. */
		int h = i == k - 1 ? area.y + area.height - g - y : avail / k;
		place(tiles[1 + i], stack_x, y, stack_w, h);
		y += h + g;
	}
}

void tile_arrange(struct server *server) {
	/* At startup the config is read before the cursor and outputs exist;
	 * the first output's arrival arranges everything. */
	if (!server->tiling || server->cursor == NULL) {
		return;
	}
	struct wlr_box area;
	if (!output_usable_area_at(server, server->cursor->x, server->cursor->y,
			&area)) {
		return;
	}
	struct workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		arrange_workspace(ws, area);
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
	if (!view_is_tiled(view) || ws == NULL) {
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
	if (view_is_tiled(view)) {
		tile_toggle_zoom(view);
	} else {
		view_toggle_maximize(view);
	}
}

void tile_set_mode(struct server *server, bool tiling) {
	if (server->tiling == tiling) {
		return;
	}
	if (server->grabbed_view != NULL) {
		server->drag_box = server->grab_box;
		end_interactive(server);
	}
	view_cycle_end(server);

	struct view *view;
	if (tiling) {
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
		server->tiling = true;
		tile_arrange(server);
	} else {
		struct workspace *ws;
		wl_list_for_each(ws, &server->workspaces, link) {
			tile_unzoom(ws);
		}
		server->tiling = false;
		wl_list_for_each(view, &server->views, link) {
			if (view->xdg_toplevel->parent != NULL) {
				continue;
			}
			struct wlr_box *b = &view->float_box;
			int ew, eh;
			view_extents(view, &ew, &eh);
			wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_NONE);
			/* Windows opened while tiling have no old place: cascade. */
			if (b->width > ew && b->height > eh) {
				view_move(view, b->x, b->y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					b->width - ew, b->height - eh);
			} else {
				view_move(view, view->x + 16, view->y + 16);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
			}
		}
	}
	highlight_update(server);
	cursor_rebase(server);
	ipc_notify_mode(server);
}

/* Moves focus to the nearest tile in a direction ("left", "right", "up",
 * "down"). Only windows wholly past that edge of the focused one count, so
 * Down from a full-height master goes nowhere, as in i3. */
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
	if (best != NULL) {
		focus_view(best);
	}
	return best != NULL;
}
