/*
 * Scrolling mode, after niri: each workspace is an endless horizontal strip
 * of columns, and the screen is a window onto part of it.
 *
 *        screen
 *   +-----------------+
 *   | +------+------+ | +------+ +------+
 *   | |      | row1 | | |      | |      |
 *   | |  A   +------+ | |  D   | |  E   |   ...
 *   | |      | row2 | | |      | |      |
 *   | +------+------+ | +------+ +------+
 *   +-----------------+
 *
 * New windows open as a new column right of the focused one. A column can
 * hold several windows stacked vertically ("consume"), but no deeper
 * nesting, as in niri. Columns are a fraction of the screen wide (half by
 * default, so two fit), and the view scrolls just enough to keep the
 * focused column fully on screen.
 */
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"

#define MIN_COLUMN_W 80

static struct column *column_new(struct workspace *ws, struct column *after,
		bool before) {
	struct column *col = calloc(1, sizeof(*col));
	col->width = ws->server->column_width;
	wl_list_init(&col->views);
	if (after == NULL) {
		wl_list_insert(ws->columns.prev, &col->link);
	} else if (before) {
		wl_list_insert(after->link.prev, &col->link);
	} else {
		wl_list_insert(&after->link, &col->link);
	}
	return col;
}

static void column_add(struct column *col, struct view *view) {
	wl_list_insert(col->views.prev, &view->column_link);
	view->column = col;
	col->focus = view;
}

/* Takes a window out of its column; an emptied column goes away. */
void scroll_remove_view(struct view *view) {
	struct column *col = view->column;
	if (col == NULL) {
		return;
	}
	wl_list_remove(&view->column_link);
	wl_list_init(&view->column_link);
	view->column = NULL;
	if (col->focus == view) {
		col->focus = wl_list_empty(&col->views) ? NULL :
			wl_container_of(col->views.next, col->focus, column_link);
	}
	if (wl_list_empty(&col->views)) {
		wl_list_remove(&col->link);
		free(col);
	}
}

/* The focused column of a workspace: the focused window's, if it's here. */
static struct column *focused_column(struct workspace *ws) {
	struct view *focused = ws->server->focused_view;
	if (focused != NULL && focused->workspace == ws && focused->column) {
		return focused->column;
	}
	return NULL;
}

/* A newly shown window gets a column of its own, right of the focused one
 * (or at the end). */
void scroll_add_view(struct view *view) {
	struct workspace *ws = view->workspace;
	if (view->column != NULL || ws == NULL || !view_is_tiled(view)) {
		return;
	}
	column_add(column_new(ws, focused_column(ws), false), view);
}

/* Entering scrolling mode: every window becomes a column, in the order
 * they were opened. */
void scroll_build(struct server *server) {
	struct view *view;
	wl_list_for_each(view, &server->tiles, tile_link) {
		scroll_add_view(view);
	}
}

/* Leaving it: forget the columns and show everything again. */
void scroll_clear(struct server *server) {
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		scroll_remove_view(view);
		wlr_scene_node_set_enabled(&view->tree->node, true);
	}
}

static int column_px(struct column *col, int screen_w, int gap) {
	int w = (int)((screen_w - gap) * col->width) - gap;
	return w < MIN_COLUMN_W ? MIN_COLUMN_W : w;
}

void scroll_arrange_workspace(struct workspace *ws, struct wlr_box area) {
	int g = ws->server->gap;
	int W = area.width;
	if (wl_list_empty(&ws->columns)) {
		ws->scroll_x = 0;
		return;
	}

	/* Where each column sits on the strip. */
	int x = g;
	struct column *col;
	wl_list_for_each(col, &ws->columns, link) {
		col->x = x;
		col->w = column_px(col, W, g);
		x += col->w + g;
	}
	int strip_w = x;

	/* Scroll just enough to show all of the focused column. */
	struct column *focus = focused_column(ws);
	if (focus != NULL) {
		if (focus->x - g < ws->scroll_x) {
			ws->scroll_x = focus->x - g;
		}
		if (focus->x + focus->w + g > ws->scroll_x + W) {
			ws->scroll_x = focus->x + focus->w + g - W;
		}
	}
	int max = strip_w - W > 0 ? strip_w - W : 0;
	ws->scroll_x = ws->scroll_x < 0 ? 0 : ws->scroll_x > max ? max : ws->scroll_x;

	wl_list_for_each(col, &ws->columns, link) {
		int n = wl_list_length(&col->views);
		int sx = area.x + col->x - ws->scroll_x;
		/* Columns wholly off screen aren't drawn (nor seen on another
		 * monitor to the side). */
		bool visible = sx + col->w > area.x && sx < area.x + W;
		int avail = area.height - (n + 1) * g;
		int y = area.y + g, i = 0;
		struct view *view;
		wl_list_for_each(view, &col->views, column_link) {
			int h = i == n - 1 ? area.y + area.height - g - y : avail / n;
			tile_place(view, sx, y, col->w, h);
			wlr_scene_node_set_enabled(&view->tree->node, visible);
			y += h + g;
			i++;
		}
	}
}

/* ---- Commands ----------------------------------------------------------- */

static struct column *neighbour(struct workspace *ws, struct column *col,
		bool left) {
	struct wl_list *next = left ? col->link.prev : col->link.next;
	if (next == &ws->columns) {
		return NULL;
	}
	return wl_container_of(next, col, link);
}

static struct view *column_focus(struct column *col) {
	if (col->focus != NULL) {
		return col->focus;
	}
	return wl_container_of(col->views.next, col->focus, column_link);
}

/* Super+Arrows: left/right between columns, up/down within one. */
bool scroll_focus_direction(struct server *server, const char *dir) {
	struct view *view = server->focused_view;
	if (view == NULL || view->column == NULL) {
		struct workspace *ws = server->active_workspace;
		if (!wl_list_empty(&ws->columns)) {
			struct column *first = wl_container_of(ws->columns.next, first, link);
			focus_view(column_focus(first));
			return true;
		}
		return false;
	}
	struct column *col = view->column;
	if (strcmp(dir, "left") == 0 || strcmp(dir, "right") == 0) {
		struct column *next = neighbour(view->workspace, col, dir[0] == 'l');
		if (next == NULL) {
			return false;
		}
		focus_view(column_focus(next));
		return true;
	}
	struct wl_list *link = dir[0] == 'u' ? view->column_link.prev :
		view->column_link.next;
	if (link == &col->views) {
		return false;
	}
	struct view *other = wl_container_of(link, other, column_link);
	focus_view(other);
	return true;
}

/* Super+Ctrl+Arrows: move the column left/right, or the window up/down
 * inside its column. */
bool scroll_move(struct server *server, const char *dir) {
	struct view *view = server->focused_view;
	if (view == NULL || view->column == NULL) {
		return false;
	}
	struct column *col = view->column;
	if (strcmp(dir, "left") == 0 || strcmp(dir, "right") == 0) {
		bool left = dir[0] == 'l';
		struct column *next = neighbour(view->workspace, col, left);
		if (next == NULL) {
			return false;
		}
		wl_list_remove(&col->link);
		if (left) {
			wl_list_insert(next->link.prev, &col->link);
		} else {
			wl_list_insert(&next->link, &col->link);
		}
	} else {
		bool up = dir[0] == 'u';
		struct wl_list *link = up ? view->column_link.prev :
			view->column_link.next;
		if (link == &col->views) {
			return false;
		}
		wl_list_remove(&view->column_link);
		if (up) {
			wl_list_insert(link->prev, &view->column_link);
		} else {
			wl_list_insert(link, &view->column_link);
		}
	}
	tile_arrange(server);
	return true;
}

/* Super+[ and Super+]: a window sharing a column is pushed out into its
 * own column on that side; a window alone is pulled into the neighbouring
 * column, at the bottom. */
bool scroll_consume_or_expel(struct server *server, const char *dir) {
	struct view *view = server->focused_view;
	if (view == NULL || view->column == NULL) {
		return false;
	}
	struct workspace *ws = view->workspace;
	struct column *col = view->column;
	bool left = strcmp(dir, "left") == 0;
	if (wl_list_length(&col->views) > 1) {
		scroll_remove_view(view);
		column_add(column_new(ws, col, left), view);
	} else {
		struct column *next = neighbour(ws, col, left);
		if (next == NULL) {
			return false;
		}
		scroll_remove_view(view);
		column_add(next, view);
	}
	tile_arrange(server);
	return true;
}

/* Super+R: cycle the focused column through a third, a half and two
 * thirds of the screen. A number sets the fraction directly. */
bool scroll_column_width(struct server *server, const char *arg) {
	static const double presets[] = { 1.0 / 3, 1.0 / 2, 2.0 / 3 };
	struct view *view = server->focused_view;
	if (view == NULL || view->column == NULL) {
		return false;
	}
	struct column *col = view->column;
	col->saved_width = 0;
	if (arg == NULL || strcmp(arg, "cycle") == 0) {
		double next = presets[0];
		for (size_t i = 0; i < sizeof(presets) / sizeof(*presets); i++) {
			if (presets[i] > col->width + 0.01) {
				next = presets[i];
				break;
			}
		}
		col->width = next;
	} else {
		double w = strtod(arg, NULL);
		if (w <= 0.05 || w > 1) {
			return false;
		}
		col->width = w;
	}
	tile_arrange(server);
	return true;
}

/* Super+Z in scrolling mode: the column fills the screen, then back. */
void scroll_toggle_full(struct view *view) {
	struct column *col = view->column;
	if (col == NULL) {
		return;
	}
	if (col->saved_width > 0) {
		col->width = col->saved_width;
		col->saved_width = 0;
	} else {
		col->saved_width = col->width;
		col->width = 1.0;
	}
	tile_arrange(view->server);
	ipc_notify_windows(view->server);
}
