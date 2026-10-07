/*
 * gemwm-portal: GemWM's backend for xdg-desktop-portal's file chooser, so
 * programs that ask the portal for an Open or Save dialog (Firefox,
 * Chromium, Flatpak apps...) get GEM's item selector (lib/gem-file.h), as
 * GemWM's own programs do.
 *
 * xdg-desktop-portal finds it by gemwm.portal, and uses it on GemWM by
 * gemwm-portals.conf (the file chooser from here, the rest from GTK's
 * portal); D-Bus starts it when it's first asked. Each request is a small
 * window holding only the selector, put over the program's own window
 * (xdg-foreign: the parent_window handle), which GemWM centres on it.
 *
 * Not everything the portal can ask for is GEM's: one file is chosen even
 * when several may be (an item selector never chose more), and "choices"
 * (extra check boxes some programs add) aren't shown.
 */
#include <gdk/wayland/gdkwayland.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <string.h>
#include "gem-file.h"
#include "gem-ui.h"

#define BUS_NAME "org.freedesktop.impl.portal.desktop.gemwm"
#define PATH "/org/freedesktop/portal/desktop"
#define CHOOSER "org.freedesktop.impl.portal.FileChooser"
#define REQUEST "org.freedesktop.impl.portal.Request"

/* As the portal numbers them. */
enum { RESPONSE_OK = 0, RESPONSE_CANCELLED = 1, RESPONSE_OTHER = 2 };

static const char introspection[] =
	"<node>"
	" <interface name='" CHOOSER "'>"
	"  <method name='OpenFile'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='s' name='title' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <method name='SaveFile'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='s' name='title' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	"  <method name='SaveFiles'>"
	"   <arg type='o' name='handle' direction='in'/>"
	"   <arg type='s' name='app_id' direction='in'/>"
	"   <arg type='s' name='parent_window' direction='in'/>"
	"   <arg type='s' name='title' direction='in'/>"
	"   <arg type='a{sv}' name='options' direction='in'/>"
	"   <arg type='u' name='response' direction='out'/>"
	"   <arg type='a{sv}' name='results' direction='out'/>"
	"  </method>"
	" </interface>"
	" <interface name='" REQUEST "'>"
	"  <method name='Close'/>"
	" </interface>"
	"</node>";

static struct {
	GtkApplication *app;
	GDBusConnection *bus;
	GDBusNodeInfo *info;
} p;

/* One Open or Save, from its call to its answer. */
struct request {
	GDBusMethodInvocation *call;
	char *handle;            /* its Request object, to Close it by */
	guint registration;
	char *parent;            /* "wayland:<handle>", or "" */
	GtkWidget *window;
	char **files;            /* SaveFiles: the names to save in the folder */
	GVariant *filter;        /* the one shown, for current_filter */
	bool answered;
};

static void request_free(struct request *r) {
	if (r->registration != 0) {
		g_dbus_connection_unregister_object(p.bus, r->registration);
	}
	g_free(r->handle);
	g_free(r->parent);
	g_strfreev(r->files);
	if (r->filter != NULL) {
		g_variant_unref(r->filter);
	}
	g_free(r);
}

/* The answer, once; the window goes, and so does the request. */
static void answer(struct request *r, guint response, GVariant *results) {
	if (r->answered) {
		return;
	}
	r->answered = true;
	if (results == NULL) {
		GVariantBuilder empty;
		g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
		results = g_variant_builder_end(&empty);
	}
	g_dbus_method_invocation_return_value(r->call,
		g_variant_new("(u@a{sv})", response, results));
	if (r->window != NULL) {
		GtkWidget *w = r->window;
		r->window = NULL;
		gtk_window_destroy(GTK_WINDOW(w));
	}
	request_free(r);
	g_application_release(G_APPLICATION(p.app));
}

static void chosen(const char *path, void *data) {
	struct request *r = data;
	if (path == NULL) {
		answer(r, RESPONSE_CANCELLED, NULL);
		return;
	}
	GVariantBuilder uris;
	g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
	if (r->files != NULL) {
		/* SaveFiles: a folder, and each file's name in it. */
		for (int i = 0; r->files[i] != NULL; i++) {
			char *base = g_path_get_basename(r->files[i]);
			char *file = g_build_filename(path, base, NULL);
			char *uri = g_filename_to_uri(file, NULL, NULL);
			if (uri != NULL) {
				g_variant_builder_add(&uris, "s", uri);
			}
			g_free(uri);
			g_free(file);
			g_free(base);
		}
	} else {
		char *uri = g_filename_to_uri(path, NULL, NULL);
		if (uri != NULL) {
			g_variant_builder_add(&uris, "s", uri);
		}
		g_free(uri);
	}
	GVariantBuilder results;
	g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
	if (r->filter != NULL) {
		g_variant_builder_add(&results, "{sv}", "current_filter", r->filter);
	}
	answer(r, RESPONSE_OK, g_variant_builder_end(&results));
}

/* The window's close box: Cancel. */
static gboolean window_closed(GtkWindow *w, struct request *r) {
	r->window = NULL; /* it's going anyway */
	answer(r, RESPONSE_CANCELLED, NULL);
	return FALSE;
}

/* Over the program's window: GemWM puts a dialog in the middle of its
 * parent. */
static void realized(GtkWidget *window, struct request *r) {
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (g_str_has_prefix(r->parent, "wayland:") && GDK_IS_WAYLAND_TOPLEVEL(surface)) {
		gdk_wayland_toplevel_set_transient_for_exported(
			GDK_WAYLAND_TOPLEVEL(surface), r->parent + strlen("wayland:"));
	}
}

/* ---- The options ----------------------------------------------------------- */

/* A byte string option (current_folder, current_file), or NULL. */
static char *bytes_option(GVariant *options, const char *key) {
	GVariant *v = g_variant_lookup_value(options, key,
		G_VARIANT_TYPE_BYTESTRING);
	char *s = v != NULL ? g_strdup(g_variant_get_bytestring(v)) : NULL;
	if (v != NULL) {
		g_variant_unref(v);
	}
	if (s != NULL && s[0] == '\0') {
		g_clear_pointer(&s, g_free);
	}
	return s;
}

/* A filter, (name, [(0 glob | 1 MIME type, what)]), as the selector's
 * pattern: globs, and the MIME types' globs. */
static char *pattern_of(GVariant *filter) {
	GVariantIter *rules;
	guint32 kind;
	const char *what;
	GString *pattern = g_string_new(NULL);
	GPtrArray *types = g_ptr_array_new();
	g_variant_get(filter, "(&sa(us))", NULL, &rules);
	while (g_variant_iter_next(rules, "(u&s)", &kind, &what)) {
		if (kind == 0) {
			g_string_append_printf(pattern, "%s%s", pattern->len ? "," : "", what);
		} else {
			g_ptr_array_add(types, (gpointer)what);
		}
	}
	g_ptr_array_add(types, NULL);
	char *globs = types->len > 1 ?
		gem_file_pattern_for_types((const char *const *)types->pdata) : NULL;
	if (globs != NULL) {
		g_string_append_printf(pattern, "%s%s", pattern->len ? "," : "", globs);
		g_free(globs);
	}
	g_ptr_array_free(types, TRUE);
	g_variant_iter_free(rules);
	return pattern->len > 0 ? g_string_free(pattern, FALSE) :
		(g_string_free(pattern, TRUE), NULL);
}

/* The filter to show: the program's current one, else its first. */
static GVariant *chosen_filter(GVariant *options) {
	GVariant *current = g_variant_lookup_value(options, "current_filter",
		G_VARIANT_TYPE("(sa(us))"));
	if (current != NULL) {
		return current;
	}
	GVariant *filters = g_variant_lookup_value(options, "filters",
		G_VARIANT_TYPE("a(sa(us))"));
	GVariant *first = filters != NULL && g_variant_n_children(filters) > 0 ?
		g_variant_get_child_value(filters, 0) : NULL;
	if (filters != NULL) {
		g_variant_unref(filters);
	}
	return first;
}

/* ---- Requests --------------------------------------------------------------- */

static void request_call(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *call, gpointer data) {
	struct request *r = data;
	/* Close: the program's given up on it. */
	g_dbus_method_invocation_return_value(call, NULL);
	answer(r, RESPONSE_OTHER, NULL);
}

static void start(GDBusMethodInvocation *call, const char *method,
		GVariant *params) {
	const char *handle, *app_id, *parent, *title;
	GVariant *options;
	g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title,
		&options);
	struct request *r = g_new0(struct request, 1);
	r->call = call;
	r->handle = g_strdup(handle);
	r->parent = g_strdup(parent);
	g_application_hold(G_APPLICATION(p.app));
	static const GDBusInterfaceVTable vtable = { .method_call = request_call };
	r->registration = g_dbus_connection_register_object(p.bus, handle,
		g_dbus_node_info_lookup_interface(p.info, REQUEST), &vtable, r, NULL,
		NULL);

	bool save = strcmp(method, "SaveFile") == 0;
	bool many = strcmp(method, "SaveFiles") == 0;
	gboolean directory = FALSE;
	g_variant_lookup(options, "directory", "b", &directory);
	enum gem_file_mode mode = many || directory ? GEM_FILE_FOLDER :
		save ? GEM_FILE_SAVE : GEM_FILE_OPEN;
	const char *ok = NULL, *name = NULL;
	g_variant_lookup(options, "accept_label", "&s", &ok);
	g_variant_lookup(options, "current_name", "&s", &name);
	char *folder = bytes_option(options, "current_folder");
	char *file = bytes_option(options, "current_file");
	if (file != NULL) {
		/* Saving over a file that's there: its folder and name. */
		g_free(folder);
		folder = g_path_get_dirname(file);
		name = NULL;
	}
	if (many) {
		GVariant *files = g_variant_lookup_value(options, "files",
			G_VARIANT_TYPE_BYTESTRING_ARRAY);
		r->files = files != NULL ? g_variant_dup_bytestring_array(files, NULL) :
			g_new0(char *, 1);
		if (files != NULL) {
			g_variant_unref(files);
		}
	}
	r->filter = mode == GEM_FILE_FOLDER ? NULL : chosen_filter(options);
	char *pattern = r->filter != NULL ? pattern_of(r->filter) : NULL;

	/* A window that's only the selector. */
	int w, h;
	gem_file_size(&w, &h);
	r->window = gtk_window_new();
	gtk_window_set_application(GTK_WINDOW(r->window), p.app);
	/* GemWM frames it; its title is the selector's, as GEM's was, and what
	 * the program asked for is the heading inside. */
	gtk_window_set_title(GTK_WINDOW(r->window), "Item Selector");
	gtk_window_set_resizable(GTK_WINDOW(r->window), FALSE);
	gtk_widget_add_css_class(r->window, "gem");
	GtkWidget *host = gtk_overlay_new();
	GtkWidget *room = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(room, w, h);
	gtk_overlay_set_child(GTK_OVERLAY(host), room);
	gtk_window_set_child(GTK_WINDOW(r->window), host);
	g_signal_connect_after(r->window, "realize", G_CALLBACK(realized), r);
	g_signal_connect(r->window, "close-request", G_CALLBACK(window_closed), r);
	char *heading = g_strdup(title[0] != '\0' ? title :
		mode == GEM_FILE_FOLDER ? "Choose a Folder" :
		mode == GEM_FILE_SAVE ? "Save" : "Open");
	gem_file_choose(GTK_OVERLAY(host), heading, mode, folder,
		file != NULL ? strrchr(file, '/') + 1 : name, pattern, ok, chosen, r);
	gtk_window_present(GTK_WINDOW(r->window));
	g_free(heading);
	g_free(pattern);
	g_free(folder);
	g_free(file);
	g_variant_unref(options);
}

static void chooser_call(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *call, gpointer data) {
	start(call, method, params);
}

static void bus_acquired(GDBusConnection *bus, const char *name, gpointer data) {
	p.bus = bus;
	static const GDBusInterfaceVTable vtable = { .method_call = chooser_call };
	g_dbus_connection_register_object(bus, PATH,
		g_dbus_node_info_lookup_interface(p.info, CHOOSER), &vtable, NULL, NULL,
		NULL);
}

static void name_lost(GDBusConnection *bus, const char *name, gpointer data) {
	g_printerr("gemwm-portal: can't have %s (is one running?)\n", BUS_NAME);
	g_application_quit(G_APPLICATION(p.app));
}

static void activate(GtkApplication *app, gpointer data) {
	static bool started;
	if (started) {
		return;
	}
	started = true;
	gem_ui_load_css();
	p.info = g_dbus_node_info_new_for_xml(introspection, NULL);
	/* Held while a request's open; between them it stays a while, so
	 * one after another doesn't start it each time. */
	g_application_set_inactivity_timeout(G_APPLICATION(app), 60 * 1000);
	g_application_hold(G_APPLICATION(app));
	g_application_release(G_APPLICATION(app));
	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		bus_acquired, NULL, name_lost, NULL, NULL);
}

int main(int argc, char *argv[]) {
	/* A portal mustn't use the portals: GTK asks the Settings portal as it
	 * starts, and xdg-desktop-portal is waiting on us to start. Nor GVfs,
	 * which may not be up yet either. */
	const char *debug = g_getenv("GDK_DEBUG");
	char *nope = debug != NULL && debug[0] != '\0' ?
		g_strconcat(debug, ",no-portals", NULL) : g_strdup("no-portals");
	g_setenv("GDK_DEBUG", nope, TRUE);
	g_free(nope);
	g_setenv("GIO_USE_VFS", "local", TRUE);
	/* Its windows' app ID, which the menu bar names it by
	 * (org.gemwm.ItemSelector.desktop: "Item Selector"). */
	g_set_prgname("org.gemwm.ItemSelector");
	p.app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
	g_signal_connect(p.app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(p.app), 1, argv);
	g_object_unref(p.app);
	return status;
}
