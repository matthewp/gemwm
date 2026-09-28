/*
 * Workspaces: each is a scene tree holding its windows, and only the active
 * one is enabled. They are dynamic, GNOME style: new ones are added at the
 * end, and an empty workspace goes away once you leave it.
 */
#include <stdlib.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"

#define MAX_WORKSPACES 32

int workspace_count(struct server *server) {
	return wl_list_length(&server->workspaces);
}

int workspace_number(struct workspace *ws) {
	int n = 1;
	struct workspace *it;
	wl_list_for_each(it, &ws->server->workspaces, link) {
		if (it == ws) {
			return n;
		}
		n++;
	}
	return 0;
}

struct workspace *workspace_nth(struct server *server, int n) {
	struct workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (--n == 0) {
			return ws;
		}
	}
	return NULL;
}

struct workspace *workspace_create(struct server *server) {
	if (workspace_count(server) >= MAX_WORKSPACES) {
		return NULL;
	}
	struct workspace *ws = calloc(1, sizeof(*ws));
	ws->server = server;
	ws->tree = wlr_scene_tree_create(server->layer_views);
	wl_list_init(&ws->columns);
	wlr_scene_node_set_enabled(&ws->tree->node, false);
	wl_list_insert(server->workspaces.prev, &ws->link);
	return ws;
}

/* Workspace n, or a new one when n is one past the last. */
struct workspace *workspace_for_target(struct server *server, int n) {
	if (n == workspace_count(server) + 1) {
		return workspace_create(server);
	}
	return workspace_nth(server, n);
}

static bool workspace_empty(struct workspace *ws) {
	struct view *view;
	wl_list_for_each(view, &ws->server->views, link) {
		if (view->workspace == ws) {
			return false;
		}
	}
	return true;
}

/* Removes empty workspaces other than the active one. */
void workspace_prune(struct server *server) {
	struct workspace *ws, *tmp;
	wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
		if (ws != server->active_workspace && workspace_empty(ws)) {
			wl_list_remove(&ws->link);
			wlr_scene_node_destroy(&ws->tree->node);
			free(ws);
		}
	}
}

/* Gives focus to the top window of the active workspace, or to nothing. */
void workspace_focus_top(struct server *server) {
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace == server->active_workspace) {
			focus_view(view);
			return;
		}
	}
	struct view *prev = server->focused_view;
	server->focused_view = NULL;
	if (prev != NULL) {
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		view_update_frame(prev);
	}
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	ipc_notify_windows(server);
}

void workspace_switch(struct server *server, struct workspace *ws) {
	if (ws == NULL || ws == server->active_workspace) {
		return;
	}
	if (server->grabbed_view != NULL) {
		/* Drop a drag in progress rather than move a window we hide. */
		server->drag_box = server->grab_box;
		end_interactive(server);
	}
	wlr_scene_node_set_enabled(&server->active_workspace->tree->node, false);
	wlr_scene_node_set_enabled(&ws->tree->node, true);
	server->active_workspace = ws;
	workspace_focus_top(server);
	cursor_rebase(server);
	workspace_prune(server);
	ipc_notify_workspaces(server);
}

/* Moves a mapped window to another workspace, without following it. */
void view_set_workspace(struct view *view, struct workspace *ws) {
	struct server *server = view->server;
	if (ws == NULL || view->workspace == ws) {
		return;
	}
	/* Zoom doesn't travel: the old workspace gets its layout back, and a
	 * window arriving on a zoomed one shows the layout there too. */
	if (view->workspace != NULL && view->workspace->zoomed == view) {
		tile_unzoom(view->workspace);
	}
	tile_unzoom(ws);
	scroll_remove_view(view);
	view->workspace = ws;
	view->tile_column = -1; /* the layout there finds it a column */
	wlr_scene_node_reparent(&view->tree->node, ws->tree);
	if (server->mode == MODE_SCROLLING) {
		scroll_add_view(view); /* joins the end of the strip there */
	}
	if (server->focused_view == view && ws != server->active_workspace) {
		workspace_focus_top(server);
	}
	workspace_prune(server);
	tile_arrange(server);
	ipc_notify_workspaces(server);
	ipc_notify_windows(server);
}

void workspaces_init(struct server *server) {
	wl_list_init(&server->workspaces);
	server->active_workspace = workspace_create(server);
	wlr_scene_node_set_enabled(&server->active_workspace->tree->node, true);
}
