/*
 * gemwm-app-menu-v1 (protocols/gemwm-app-menu-v1.xml): windows put their
 * own menus in the menu bar. We keep each window's menus as the text the
 * menu bar reads, one line per entry,
 *
 *   M<TAB>title
 *   I<TAB>id<TAB>flags<TAB>label<TAB>shortcut
 *   S                                            (a separator)
 *
 * and send the focused window's with the control socket's "focus" event
 * (src/ipc.c). The bar asks for an item with "menu-activate".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include "gemwm-app-menu-v1-protocol.h"
#include "server.h"

#define MAX_MENUS_TEXT 8192 /* more is ignored; the bar's lines are limited */

struct app_menu {
	struct wl_resource *resource;
	struct view *view; /* NULL once the window is gone */
	char *pending;     /* built up until commit */
	size_t len;
	bool overflow;
};

/* Appends to the pending text; tabs and newlines in strings would break
 * its lines, so they become spaces. */
static void append(struct app_menu *m, const char *s, bool clean) {
	size_t n = strlen(s);
	if (m->overflow || m->len + n + 1 > MAX_MENUS_TEXT) {
		m->overflow = true;
		return;
	}
	char *grown = realloc(m->pending, m->len + n + 1);
	if (grown == NULL) {
		m->overflow = true;
		return;
	}
	m->pending = grown;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		m->pending[m->len++] = clean && (c == '\t' || c == '\n' || c == '\r') ?
			' ' : c;
	}
	m->pending[m->len] = '\0';
}

static void focus_changed(struct view *view) {
	if (view->server->focused_view == view) {
		ipc_notify_focus(view->server);
	}
}

static void handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void handle_menu(struct wl_client *client,
		struct wl_resource *resource, const char *title) {
	struct app_menu *m = wl_resource_get_user_data(resource);
	append(m, "M\t", false);
	append(m, title, true);
	append(m, "\n", false);
}

static void handle_item(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, const char *label,
		const char *shortcut, uint32_t flags) {
	struct app_menu *m = wl_resource_get_user_data(resource);
	char head[48];
	snprintf(head, sizeof(head), "I\t%u\t%u\t", id, flags);
	append(m, head, false);
	append(m, label, true);
	append(m, "\t", false);
	append(m, shortcut, true);
	append(m, "\n", false);
}

static void handle_separator(struct wl_client *client,
		struct wl_resource *resource) {
	append(wl_resource_get_user_data(resource), "S\n", false);
}

static void handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
	struct app_menu *m = wl_resource_get_user_data(resource);
	char *text = m->overflow ? NULL : m->pending;
	if (m->overflow) {
		free(m->pending);
	}
	m->pending = NULL;
	m->len = 0;
	m->overflow = false;
	if (m->view == NULL) {
		free(text);
		return;
	}
	free(m->view->app_menus);
	m->view->app_menus = text;
	focus_changed(m->view);
}

static const struct gemwm_app_menu_v1_interface app_menu_impl = {
	.destroy = handle_destroy,
	.menu = handle_menu,
	.item = handle_item,
	.separator = handle_separator,
	.commit = handle_commit,
};

static void detach(struct app_menu *m) {
	struct view *view = m->view;
	if (view == NULL) {
		return;
	}
	free(view->app_menus);
	view->app_menus = NULL;
	view->app_menu = NULL;
	m->view = NULL;
	focus_changed(view);
}

static void app_menu_resource_destroy(struct wl_resource *resource) {
	struct app_menu *m = wl_resource_get_user_data(resource);
	detach(m);
	free(m->pending);
	free(m);
}

static void handle_get_app_menu(struct wl_client *client,
		struct wl_resource *manager, uint32_t id,
		struct wl_resource *surface_resource) {
	struct app_menu *m = calloc(1, sizeof(*m));
	m->resource = wl_resource_create(client, &gemwm_app_menu_v1_interface,
		wl_resource_get_version(manager), id);
	if (m->resource == NULL) {
		free(m);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(m->resource, &app_menu_impl, m,
		app_menu_resource_destroy);

	/* Only a toplevel's surface has a view (wlr_surface.data); for any
	 * other surface the object simply does nothing. */
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct view *view = surface->data;
	if (view != NULL) {
		if (view->app_menu != NULL) {
			detach(view->app_menu);
		}
		view->app_menu = m;
		m->view = view;
	}
}

static void handle_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct gemwm_app_menu_manager_v1_interface manager_impl = {
	.destroy = handle_manager_destroy,
	.get_app_menu = handle_get_app_menu,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&gemwm_app_menu_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void app_menus_init(struct server *server) {
	wl_global_create(server->wl_display, &gemwm_app_menu_manager_v1_interface,
		1, server, manager_bind);
}

/* A window is going away: its menus object stays until the client
 * destroys it, but no longer points here. */
void app_menus_view_destroyed(struct view *view) {
	if (view->app_menu != NULL) {
		view->app_menu->view = NULL;
		view->app_menu = NULL;
	}
	free(view->app_menus);
	view->app_menus = NULL;
}

/* The menu bar picked an item of the window's. */
void view_menu_activate(struct view *view, uint32_t id) {
	if (view->app_menu != NULL) {
		gemwm_app_menu_v1_send_activate(view->app_menu->resource, id);
	}
}
