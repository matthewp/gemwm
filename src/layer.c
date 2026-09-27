/*
 * wlr-layer-shell: surfaces anchored to screen edges, like the GEM menu bar.
 * Surfaces with an exclusive zone shrink the output's usable area, which is
 * where windows get placed and maximized.
 */
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"

void layers_arrange(struct output *output) {
	struct server *server = output->server;
	struct wlr_box full;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
	struct wlr_box usable = full;

	/* Surfaces that reserve space go first, so the rest are placed inside
	 * what is left. Within a pass, higher layers claim space first. */
	for (int pass = 0; pass < 2; pass++) {
		for (int layer = 3; layer >= 0; layer--) {
			struct layer_surface *ls;
			wl_list_for_each(ls, &server->layer_surfaces, link) {
				struct wlr_layer_surface_v1 *wlr = ls->wlr;
				if (wlr->output != output->wlr_output || !wlr->initialized ||
						(int)wlr->current.layer != layer) {
					continue;
				}
				bool exclusive = wlr->current.exclusive_zone > 0;
				if (exclusive != (pass == 0)) {
					continue;
				}
				wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
			}
		}
	}
	output->usable_area = usable;
	tile_arrange(server); /* the room for tiles may have changed */
}

bool output_usable_area_at(struct server *server, double lx, double ly,
		struct wlr_box *box) {
	struct wlr_output *wlr_output =
		wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (wlr_output == NULL || wlr_output->data == NULL) {
		return false;
	}
	struct output *output = wlr_output->data;
	*box = output->usable_area;
	return true;
}

static void layer_map(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, map);
	struct wlr_layer_surface_v1 *wlr = ls->wlr;
	ls->mapped = true;
	if (wlr->output != NULL && wlr->output->data != NULL) {
		layers_arrange(wlr->output->data);
	}
	cursor_rebase(ls->server);
	if (wlr->current.keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(ls->server->seat);
		if (kb != NULL) {
			wlr_seat_keyboard_notify_enter(ls->server->seat, wlr->surface,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
		}
	}
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, unmap);
	struct server *server = ls->server;
	ls->mapped = false;
	if (ls->wlr->output != NULL && ls->wlr->output->data != NULL) {
		layers_arrange(ls->wlr->output->data);
	}
	cursor_rebase(server);
	/* Give the keyboard back to the top window. */
	if (server->seat->keyboard_state.focused_surface == ls->wlr->surface) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		focus_view(server->focused_view);
	}
}

static void layer_commit(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *wlr = ls->wlr;
	if (!wlr->initialized || wlr->output == NULL || wlr->output->data == NULL) {
		return;
	}
	uint32_t committed = wlr->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wlr_scene_node_reparent(&ls->scene->tree->node,
			ls->server->layers[wlr->current.layer]);
	}
	/* Only rearrange when the layout-relevant state changed; plain redraws
	 * of the surface don't need it. */
	if (wlr->initial_commit || committed != 0) {
		layers_arrange(wlr->output->data);
	}
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	/* Configure is handled by the generic xdg popup code in view.c; nested
	 * popups find their parent tree through xdg_surface.data. */
	popup->base->data =
		wlr_scene_xdg_surface_create(ls->scene->tree, popup->base);
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->link);
	free(ls);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *wlr = data;

	/* Clients may leave the output to us: use the one under the pointer. */
	if (wlr->output == NULL) {
		wlr->output = wlr_output_layout_output_at(server->output_layout,
			server->cursor->x, server->cursor->y);
	}
	if (wlr->output == NULL) {
		wlr_layer_surface_v1_destroy(wlr);
		return;
	}

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	ls->server = server;
	ls->wlr = wlr;
	ls->scene = wlr_scene_layer_surface_v1_create(
		server->layers[wlr->pending.layer], wlr);
	wlr->data = ls;

	ls->map.notify = layer_map;
	wl_signal_add(&wlr->surface->events.map, &ls->map);
	ls->unmap.notify = layer_unmap;
	wl_signal_add(&wlr->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_commit;
	wl_signal_add(&wlr->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_destroy;
	wl_signal_add(&wlr->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_new_popup;
	wl_signal_add(&wlr->events.new_popup, &ls->new_popup);
	wl_list_insert(&server->layer_surfaces, &ls->link);
}

/* A mapped layer surface that wants the keyboard to itself (an alert, a
 * lock screen), or NULL. Higher layers win. */
struct wlr_surface *layers_exclusive_focus(struct server *server) {
	for (int layer = 3; layer >= 0; layer--) {
		struct layer_surface *ls;
		wl_list_for_each(ls, &server->layer_surfaces, link) {
			if (ls->mapped && (int)ls->wlr->current.layer == layer &&
					ls->wlr->current.keyboard_interactive ==
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
				return ls->wlr->surface;
			}
		}
	}
	return NULL;
}

void layers_output_destroyed(struct output *output) {
	struct layer_surface *ls, *tmp;
	wl_list_for_each_safe(ls, tmp, &output->server->layer_surfaces, link) {
		if (ls->wlr->output == output->wlr_output) {
			ls->wlr->output = NULL;
			wlr_layer_surface_v1_destroy(ls->wlr);
		}
	}
}

void layers_init(struct server *server) {
	wl_list_init(&server->layer_surfaces);
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);
}
