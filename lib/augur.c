#include <gio/gio.h>
#include <string.h>
#include "augur.h"

#define BUS_NAME "io.github.matthewp.Augur"
#define OBJECT_PATH "/io/github/matthewp/Augur"
#define IFACE "io.github.matthewp.Augur1"
#define ASK_TIMEOUT (5 * 60 * 1000)

static struct {
	GDBusConnection *bus;
	char *app_id;
	bool enabled;
	bool known;
	void (*changed)(bool enabled, void *data);
	void *data;
} augur;

static void set_enabled(bool enabled) {
	bool was_known = augur.known;
	augur.known = true;
	if (enabled == augur.enabled && was_known) {
		return;
	}
	augur.enabled = enabled;
	if (augur.changed != NULL) {
		augur.changed(enabled, augur.data);
	}
}

/* Asking starts Augur if it isn't running: D-Bus does that. No Augur
 * installed is an error, and off. */
static void got_enabled(GObject *src, GAsyncResult *res, gpointer data) {
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res,
		NULL);
	bool on = false;
	if (r != NULL) {
		GVariant *v;
		g_variant_get(r, "(v)", &v);
		on = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
			g_variant_get_boolean(v);
		g_variant_unref(v);
		g_variant_unref(r);
	}
	set_enabled(on);
}

static void ask_enabled(void) {
	g_dbus_connection_call(augur.bus, BUS_NAME, OBJECT_PATH,
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", IFACE, "Enabled"), G_VARIANT_TYPE("(v)"),
		G_DBUS_CALL_FLAGS_NONE, 10000, NULL, got_enabled, NULL);
}

static void properties_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	const char *changed_iface;
	GVariant *changed;
	g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed, NULL);
	gboolean on;
	if (strcmp(changed_iface, IFACE) == 0 &&
			g_variant_lookup(changed, "Enabled", "b", &on)) {
		set_enabled(on);
	}
	g_variant_unref(changed);
}

/* Augur came (from somewhere: installed, started) or went (idle): ask
 * again when it comes; while it's away, it's whatever it last said. */
static void owner_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	const char *name, *old, *new;
	g_variant_get(params, "(&s&s&s)", &name, &old, &new);
	if (strcmp(name, BUS_NAME) == 0 && new[0] != '\0') {
		ask_enabled();
	}
}

void augur_watch(const char *app_id, void (*changed)(bool enabled, void *data),
		void *data) {
	augur.app_id = g_strdup(app_id);
	augur.changed = changed;
	augur.data = data;
	augur.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (augur.bus == NULL) {
		set_enabled(false);
		return;
	}
	g_dbus_connection_signal_subscribe(augur.bus, BUS_NAME,
		"org.freedesktop.DBus.Properties", "PropertiesChanged", OBJECT_PATH,
		NULL, G_DBUS_SIGNAL_FLAGS_NONE, properties_changed, NULL, NULL);
	g_dbus_connection_signal_subscribe(augur.bus, "org.freedesktop.DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus",
		BUS_NAME, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, NULL, NULL);
	ask_enabled();
}

bool augur_enabled(void) {
	return augur.enabled;
}

/* ---- Asking ---------------------------------------------------------------- */

struct asking {
	augur_answer_fn done;
	void *data;
};

static void answered(GObject *src, GAsyncResult *res, gpointer data) {
	struct asking *a = data;
	GError *error = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res,
		&error);
	if (r == NULL) {
		/* Augur's message, without D-Bus's wrapping. */
		g_dbus_error_strip_remote_error(error);
		a->done(NULL, error->message, a->data);
		g_error_free(error);
	} else {
		const char *text;
		g_variant_get(r, "(&sa{sv})", &text, NULL);
		a->done(text, NULL, a->data);
		g_variant_unref(r);
	}
	g_free(a);
}

static void add_message(GVariantBuilder *b, const char *role,
		const char *content) {
	GVariantBuilder m;
	g_variant_builder_init(&m, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&m, "{sv}", "role", g_variant_new_string(role));
	g_variant_builder_add(&m, "{sv}", "content", g_variant_new_string(content));
	g_variant_builder_add_value(b, g_variant_builder_end(&m));
}

void augur_ask(const char *system, const char *user, const char *schema,
		const char *model, const char *tier, augur_answer_fn done, void *data) {
	if (augur.bus == NULL) {
		done(NULL, "no session bus", data);
		return;
	}
	GVariantBuilder messages;
	g_variant_builder_init(&messages, G_VARIANT_TYPE("aa{sv}"));
	if (system != NULL) {
		add_message(&messages, "system", system);
	}
	add_message(&messages, "user", user);
	GVariantBuilder req;
	g_variant_builder_init(&req, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&req, "{sv}", "app-id",
		g_variant_new_string(augur.app_id != NULL ? augur.app_id : g_get_prgname()));
	g_variant_builder_add(&req, "{sv}", "messages",
		g_variant_builder_end(&messages));
	if (schema != NULL) {
		g_variant_builder_add(&req, "{sv}", "schema", g_variant_new_string(schema));
	}
	if (model != NULL && model[0] != '\0') {
		g_variant_builder_add(&req, "{sv}", "model", g_variant_new_string(model));
	}
	if (tier != NULL && tier[0] != '\0') {
		g_variant_builder_add(&req, "{sv}", "tier", g_variant_new_string(tier));
	}
	struct asking *a = g_new0(struct asking, 1);
	a->done = done;
	a->data = data;
	g_dbus_connection_call(augur.bus, BUS_NAME, OBJECT_PATH, IFACE, "Ask",
		g_variant_new("(a{sv})", &req), G_VARIANT_TYPE("(sa{sv})"),
		G_DBUS_CALL_FLAGS_NONE, ASK_TIMEOUT, NULL, answered, a);
}
