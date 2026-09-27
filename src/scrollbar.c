/*
 * gemwm-scroll-v1 (protocols/gemwm-scroll-v1.xml): clients report how their
 * window is scrolled, and the frame gets real GEM scroll bars. Working the
 * bars (arrows, paging on the track, dragging the slider) asks the client
 * to scroll. Windows that don't use the protocol have no scroll bars.
 */
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include "gemwm-scroll-v1-protocol.h"
#include "server.h"

struct scroll_client {
	struct wl_resource *resource;
	struct view *view; /* NULL once the window is gone */
};

/* The frame changes; when a bar appears or goes, so does the frame's size,
 * and arranged layouts have to make room. */
static void scroll_changed(struct view *view, bool bars_changed) {
	view_update_frame(view);
	if (bars_changed) {
		tile_arrange(view->server);
		cursor_rebase(view->server);
	}
}

static void handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void handle_set_axis(struct wl_client *client,
		struct wl_resource *resource, uint32_t axis, int32_t position,
		int32_t visible, int32_t total) {
	struct scroll_client *sc = wl_resource_get_user_data(resource);
	if (sc->view == NULL || axis > GEMWM_SCROLL_V1_AXIS_HORIZONTAL) {
		return;
	}
	struct frame_axis *a = axis == GEMWM_SCROLL_V1_AXIS_VERTICAL ?
		&sc->view->scroll_v : &sc->view->scroll_h;
	bool was_on = a->on;
	a->on = total > 0;
	a->position = position;
	a->visible = visible;
	a->total = total;
	scroll_changed(sc->view, was_on != a->on);
}

static const struct gemwm_scroll_v1_interface scroll_impl = {
	.destroy = handle_destroy,
	.set_axis = handle_set_axis,
};

static void detach(struct scroll_client *sc) {
	struct view *view = sc->view;
	if (view == NULL) {
		return;
	}
	bool had_bars = view->scroll_v.on || view->scroll_h.on;
	memset(&view->scroll_v, 0, sizeof(view->scroll_v));
	memset(&view->scroll_h, 0, sizeof(view->scroll_h));
	view->scroll = NULL;
	sc->view = NULL;
	scroll_changed(view, had_bars);
}

static void scroll_resource_destroy(struct wl_resource *resource) {
	struct scroll_client *sc = wl_resource_get_user_data(resource);
	detach(sc);
	free(sc);
}

static void handle_get_scroll(struct wl_client *client,
		struct wl_resource *manager, uint32_t id,
		struct wl_resource *surface_resource) {
	struct scroll_client *sc = calloc(1, sizeof(*sc));
	sc->resource = wl_resource_create(client, &gemwm_scroll_v1_interface,
		wl_resource_get_version(manager), id);
	if (sc->resource == NULL) {
		free(sc);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(sc->resource, &scroll_impl, sc,
		scroll_resource_destroy);

	/* Only a toplevel's surface has a view (wlr_surface.data); for any
	 * other surface the object simply does nothing. */
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct view *view = surface->data;
	if (view != NULL) {
		if (view->scroll != NULL) {
			detach(view->scroll);
		}
		view->scroll = sc;
		sc->view = view;
	}
}

static void handle_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct gemwm_scroll_manager_v1_interface manager_impl = {
	.destroy = handle_manager_destroy,
	.get_scroll = handle_get_scroll,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&gemwm_scroll_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void scrollbars_init(struct server *server) {
	wl_global_create(server->wl_display, &gemwm_scroll_manager_v1_interface,
		1, server, manager_bind);
}

/* A window is going away: its scroll object stays until the client
 * destroys it, but no longer points here. */
void scrollbars_view_destroyed(struct view *view) {
	if (view->scroll != NULL) {
		view->scroll->view = NULL;
		view->scroll = NULL;
	}
}

/* Asks the client to scroll an axis to position, kept within its range. */
void view_scroll_to(struct view *view, bool vertical, int position) {
	struct frame_axis *a = vertical ? &view->scroll_v : &view->scroll_h;
	if (view->scroll == NULL || !a->on) {
		return;
	}
	int max = a->total - a->visible > 0 ? a->total - a->visible : 0;
	position = position < 0 ? 0 : position > max ? max : position;
	gemwm_scroll_v1_send_scroll_to(view->scroll->resource,
		vertical ? GEMWM_SCROLL_V1_AXIS_VERTICAL :
		GEMWM_SCROLL_V1_AXIS_HORIZONTAL, position);
}
