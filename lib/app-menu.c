#include <gdk/wayland/gdkwayland.h>
#include <string.h>
#include "app-menu.h"
#include "gemwm-app-menu-v1-client-protocol.h"

struct app_menu {
	GtkWidget *window;
	app_menu_build_fn build;
	app_menu_activate_fn activate;
	void *data;
	struct gemwm_app_menu_v1 *object;
	GString *building; /* the menus being described, one line per entry */
	char *sent;        /* the last menus sent, the same way */
};

static struct gemwm_app_menu_manager_v1 *manager;

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *iface, uint32_t version) {
	if (strcmp(iface, gemwm_app_menu_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name,
			&gemwm_app_menu_manager_v1_interface, MIN(version, 2));
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* GemWM or not: looked for once, on GTK's own Wayland connection. */
static bool find_manager(GdkDisplay *display) {
	static bool looked;
	if (!looked && GDK_IS_WAYLAND_DISPLAY(display)) {
		looked = true;
		struct wl_display *wl = gdk_wayland_display_get_wl_display(display);
		struct wl_registry *registry = wl_display_get_registry(wl);
		wl_registry_add_listener(registry, &registry_listener, NULL);
		wl_display_roundtrip(wl);
		wl_registry_destroy(registry);
	}
	return manager != NULL;
}

static void on_activate(void *data, struct gemwm_app_menu_v1 *object,
		uint32_t id) {
	struct app_menu *m = data;
	m->activate(id, m->data);
}

static const struct gemwm_app_menu_v1_listener listener = {
	.activate = on_activate,
};

/* Tabs and newlines would break the lines; GemWM would make them spaces
 * anyway. */
static void append_clean(GString *s, const char *text) {
	for (const char *p = text != NULL ? text : ""; *p != '\0'; p++) {
		g_string_append_c(s, *p == '\t' || *p == '\n' ? ' ' : *p);
	}
}

void app_menu_add_menu(struct app_menu *m, const char *title) {
	g_string_append(m->building, "M\t");
	append_clean(m->building, title);
	g_string_append_c(m->building, '\n');
}

void app_menu_add_item(struct app_menu *m, uint32_t id, const char *label,
		const char *shortcut, uint32_t flags) {
	g_string_append_printf(m->building, "I\t%u\t%u\t", id, flags);
	append_clean(m->building, label);
	g_string_append_c(m->building, '\t');
	append_clean(m->building, shortcut);
	g_string_append_c(m->building, '\n');
}

void app_menu_add_separator(struct app_menu *m) {
	g_string_append(m->building, "S\n");
}

void app_menu_add_submenu(struct app_menu *m, const char *label,
		uint32_t flags) {
	g_string_append_printf(m->building, "U\t%u\t", flags);
	append_clean(m->building, label);
	g_string_append_c(m->building, '\n');
}

void app_menu_end_submenu(struct app_menu *m) {
	g_string_append(m->building, "E\n");
}

/* Sends the menus described as lines, as the protocol's requests. A
 * GemWM from before submenus (version 1) gets their items in the menu
 * itself. */
static void send(struct app_menu *m, const char *text) {
	bool submenus = gemwm_app_menu_v1_get_version(m->object) >= 2;
	char **lines = g_strsplit(text, "\n", -1);
	for (char **line = lines; *line != NULL; line++) {
		char **f = g_strsplit(*line, "\t", 5);
		if (g_strcmp0(f[0], "M") == 0 && f[1] != NULL) {
			gemwm_app_menu_v1_menu(m->object, f[1]);
		} else if (g_strcmp0(f[0], "S") == 0) {
			gemwm_app_menu_v1_separator(m->object);
		} else if (g_strcmp0(f[0], "U") == 0 && g_strv_length(f) == 3) {
			if (submenus) {
				gemwm_app_menu_v1_submenu(m->object, f[2],
					(uint32_t)g_ascii_strtoull(f[1], NULL, 10));
			}
		} else if (g_strcmp0(f[0], "E") == 0) {
			if (submenus) {
				gemwm_app_menu_v1_end_submenu(m->object);
			}
		} else if (g_strcmp0(f[0], "I") == 0 && g_strv_length(f) == 5) {
			gemwm_app_menu_v1_item(m->object,
				(uint32_t)g_ascii_strtoull(f[1], NULL, 10), f[3], f[4],
				(uint32_t)g_ascii_strtoull(f[2], NULL, 10));
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	gemwm_app_menu_v1_commit(m->object);
}

void app_menu_update(struct app_menu *m) {
	if (m->object == NULL) {
		return;
	}
	g_string_truncate(m->building, 0);
	m->build(m, m->data);
	if (g_strcmp0(m->building->str, m->sent) == 0) {
		return;
	}
	g_free(m->sent);
	m->sent = g_strdup(m->building->str);
	send(m, m->sent);
}

/* The window has a Wayland surface once it's on screen. */
static void window_mapped(GtkWidget *window, struct app_menu *m) {
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (m->object != NULL || !GDK_IS_WAYLAND_SURFACE(surface) ||
			!find_manager(gtk_widget_get_display(window))) {
		return;
	}
	m->object = gemwm_app_menu_manager_v1_get_app_menu(manager,
		gdk_wayland_surface_get_wl_surface(surface));
	gemwm_app_menu_v1_add_listener(m->object, &listener, m);
	app_menu_update(m);
}

static void window_destroyed(GtkWidget *window, struct app_menu *m) {
	if (m->object != NULL) {
		gemwm_app_menu_v1_destroy(m->object);
	}
	g_string_free(m->building, TRUE);
	g_free(m->sent);
	g_free(m);
}

struct app_menu *app_menu_new(GtkWidget *window, app_menu_build_fn build,
		app_menu_activate_fn activate, void *data) {
	struct app_menu *m = g_new0(struct app_menu, 1);
	m->window = window;
	m->build = build;
	m->activate = activate;
	m->data = data;
	m->building = g_string_new(NULL);
	g_signal_connect(window, "map", G_CALLBACK(window_mapped), m);
	g_signal_connect(window, "destroy", G_CALLBACK(window_destroyed), m);
	return m;
}
