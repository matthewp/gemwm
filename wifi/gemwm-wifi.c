/*
 * gemwm-wifi: Wi-Fi for GemWM, GEM style.
 *
 *   gemwm-wifi              the window: turn Wi-Fi on and off, scan, and
 *                           connect to, disconnect from or forget networks
 *   gemwm-wifi --menu-app   the menu bar item (see menu/menu.c): signal
 *                           bars, greyed when Wi-Fi is off; the tooltip
 *                           says what it's connected to; a click opens the
 *                           window
 *
 * Both talk to iwd on the system bus. While the window is open it's iwd's
 * agent, so a network's password is asked for in a GEM alert box. It
 * handles open and password (PSK) networks; enterprise (802.1X) ones need
 * iwd's own configuration.
 */
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app-menu.h"
#include "gem-draw.h"

#define IWD "net.connman.iwd"
#define AGENT_PATH "/org/gemwm/wifi/agent"
#define PAD 8
#define BAR_H 30      /* the top row (power) and the bottom row (scan) */
#define ROW_H 24
#define BUTTON_H 18
#define MAX_SHOWN 12  /* networks listed, strongest first */
#define SIGNAL_POLL 15 /* seconds between signal readings */

static GDBusObjectManager *iwd;

/* ---- iwd ---------------------------------------------------------------- */

static GVariant *prop(GDBusProxy *proxy, const char *name) {
	return proxy != NULL ? g_dbus_proxy_get_cached_property(proxy, name) : NULL;
}

static bool prop_bool(GDBusProxy *proxy, const char *name) {
	GVariant *v = prop(proxy, name);
	bool b = v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
		g_variant_get_boolean(v);
	if (v != NULL) {
		g_variant_unref(v);
	}
	return b;
}

static char *prop_string(GDBusProxy *proxy, const char *name) {
	GVariant *v = prop(proxy, name);
	char *s = NULL;
	if (v != NULL && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
			g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))) {
		s = g_variant_dup_string(v, NULL);
	}
	if (v != NULL) {
		g_variant_unref(v);
	}
	return s;
}

static GDBusProxy *interface_at(const char *path, const char *name) {
	GDBusObject *object = g_dbus_object_manager_get_object(iwd, path);
	GDBusInterface *i = object ? g_dbus_object_get_interface(object, name) : NULL;
	g_clear_object(&object);
	return i != NULL ? G_DBUS_PROXY(i) : NULL;
}

/* The first wireless device (wlan0), or NULL. Unref it. Its Station
 * interface is only there while it's powered. */
static GDBusProxy *device(void) {
	GList *objects = g_dbus_object_manager_get_objects(iwd);
	GDBusProxy *found = NULL;
	for (GList *l = objects; l != NULL && found == NULL; l = l->next) {
		GDBusInterface *i = g_dbus_object_get_interface(l->data, IWD ".Device");
		found = i != NULL ? G_DBUS_PROXY(i) : NULL;
	}
	g_list_free_full(objects, g_object_unref);
	return found;
}

static GDBusProxy *station(void) {
	GDBusProxy *d = device();
	GDBusProxy *s = d ? interface_at(g_dbus_proxy_get_object_path(d),
		IWD ".Station") : NULL;
	g_clear_object(&d);
	return s;
}

struct network {
	char *path, *name, *type; /* type: open, psk, 8021x, wep */
	char *known;              /* its KnownNetwork, if it's saved */
	bool connected;
	int dbm;
};

static void network_free(void *data) {
	struct network *n = data;
	g_free(n->path);
	g_free(n->name);
	g_free(n->type);
	g_free(n->known);
	g_free(n);
}

/* Bars, as iwd's own tools count them. */
static int bars(int dbm) {
	return dbm >= -60 ? 4 : dbm >= -67 ? 3 : dbm >= -74 ? 2 : dbm >= -80 ? 1 : 0;
}

static struct {
	GPtrArray *networks; /* struct network, strongest first */
	bool asking;         /* GetOrderedNetworks in flight */
	bool again;
	void (*changed)(void);
	guint idle;
} model;

static void fetch(void);

static void on_networks(GObject *source, GAsyncResult *result, void *data) {
	model.asking = false;
	GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, NULL);
	GPtrArray *list = g_ptr_array_new_with_free_func(network_free);
	if (r != NULL) {
		GVariantIter *it;
		const char *path;
		gint16 signal;
		g_variant_get(r, "(a(on))", &it);
		while (g_variant_iter_next(it, "(&on)", &path, &signal)) {
			GDBusProxy *p = interface_at(path, IWD ".Network");
			if (p == NULL) {
				continue;
			}
			struct network *n = g_new0(struct network, 1);
			n->path = g_strdup(path);
			n->name = prop_string(p, "Name");
			n->type = prop_string(p, "Type");
			n->known = prop_string(p, "KnownNetwork");
			n->connected = prop_bool(p, "Connected");
			n->dbm = signal / 100;
			g_ptr_array_add(list, n);
			g_object_unref(p);
		}
		g_variant_iter_free(it);
		g_variant_unref(r);
	}
	g_clear_pointer(&model.networks, g_ptr_array_unref);
	model.networks = list;
	model.changed();
	if (model.again) {
		model.again = false;
		fetch();
	}
}

/* The station's networks, as iwd has them from its last scan. */
static void fetch(void) {
	if (model.asking) {
		model.again = true;
		return;
	}
	GDBusProxy *s = station();
	if (s == NULL) {
		g_clear_pointer(&model.networks, g_ptr_array_unref);
		model.changed();
		return;
	}
	model.asking = true;
	g_dbus_proxy_call(s, "GetOrderedNetworks", NULL, G_DBUS_CALL_FLAGS_NONE,
		-1, NULL, on_networks, NULL);
	g_object_unref(s);
}

static gboolean fetch_idle(void *data) {
	model.idle = 0;
	fetch();
	return G_SOURCE_REMOVE;
}

/* Anything in iwd changed; many changes come together, so once, idle. */
static void iwd_changed(void *data) {
	if (model.idle == 0) {
		model.idle = g_idle_add(fetch_idle, NULL);
	}
}

static gboolean signal_poll(void *data) {
	fetch(); /* signal strength changes without iwd saying so */
	return G_SOURCE_CONTINUE;
}

static bool iwd_connect(void (*changed)(void)) {
	GError *error = NULL;
	iwd = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, IWD, "/", NULL, NULL, NULL,
		NULL, &error);
	if (iwd == NULL) {
		fprintf(stderr, "gemwm-wifi: %s\n", error->message);
		g_error_free(error);
		return false;
	}
	model.changed = changed;
	static const char *signals[] = { "object-added", "object-removed",
		"interface-added", "interface-removed",
		"interface-proxy-properties-changed", "notify::name-owner" };
	for (size_t i = 0; i < G_N_ELEMENTS(signals); i++) {
		g_signal_connect_swapped(iwd, signals[i], G_CALLBACK(iwd_changed), NULL);
	}
	g_timeout_add_seconds(SIGNAL_POLL, signal_poll, NULL);
	fetch();
	return true;
}

static struct network *connected_network(void) {
	for (guint i = 0; model.networks != NULL && i < model.networks->len; i++) {
		struct network *n = g_ptr_array_index(model.networks, i);
		if (n->connected) {
			return n;
		}
	}
	return NULL;
}

/* ---- The icon ----------------------------------------------------------- */

/* Four bars, 2 pixels wide, rising left to right; unlit ones are just
 * their foot. Off: all of them, every other pixel. */
enum { ICON_W = 11, ICON_H = 10 };

static void icon_pixels(bool px[ICON_H][ICON_W], int lit, bool off) {
	memset(px, 0, sizeof(bool) * ICON_H * ICON_W);
	for (int b = 0; b < 4; b++) {
		int h = 3 + b * 2;
		for (int y = ICON_H - 1; y >= ICON_H - h; y--) {
			for (int x = b * 3; x < b * 3 + 2; x++) {
				bool on = off ? (x + y) % 2 == 0 :
					b < lit || y == ICON_H - 1;
				px[y][x] = on;
			}
		}
	}
}

/* ---- The menu bar item -------------------------------------------------- */

static char *self;

static void status_print(void) {
	static char *last;
	GDBusProxy *d = device();
	if (d != NULL && model.asking && model.networks == NULL) {
		g_object_unref(d);
		return; /* not "Not connected" before iwd has said */
	}
	GString *line = g_string_new(NULL);
	if (d != NULL) {
		bool powered = prop_bool(d, "Powered");
		struct network *n = powered ? connected_network() : NULL;
		bool px[ICON_H][ICON_W];
		icon_pixels(px, n != NULL ? bars(n->dbm) : 0, !powered);
		g_string_append_printf(line, "bitmap:%dx%d:", ICON_W, ICON_H);
		for (int y = 0; y < ICON_H; y++) {
			for (int x = 0; x < ICON_W; x += 8) {
				unsigned byte = 0;
				for (int b = 0; b < 8 && x + b < ICON_W; b++) {
					byte |= px[y][x + b] ? 0x80u >> b : 0;
				}
				g_string_append_printf(line, "%02x", byte);
			}
		}
		/* No text; the tooltip says what it's connected to. */
		char *name = n != NULL ? g_strdelimit(g_strdup(n->name), "\t\n", ' ') :
			NULL;
		g_string_append_printf(line, "\t\t\t%s%s", !powered ? "Wi-Fi off" :
			n != NULL ? "Connected to " : "Not connected", name ? name : "");
		g_free(name);
		g_object_unref(d);
	}
	if (g_strcmp0(line->str, last) != 0) {
		printf("%s\n", line->str);
		fflush(stdout);
		g_free(last);
		last = g_strdup(line->str);
	}
	g_string_free(line, TRUE);
}

static void read_bar(GObject *source, GAsyncResult *result, void *data) {
	GMainLoop *loop = data;
	char *line = g_data_input_stream_read_line_finish(
		G_DATA_INPUT_STREAM(source), result, NULL, NULL);
	if (line == NULL) {
		g_main_loop_quit(loop); /* the bar has gone */
		return;
	}
	if (strcmp(line, "click 1") == 0) {
		/* Single instance: a second click brings the window forward. */
		char *argv[] = { self, NULL };
		g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			NULL);
	}
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source),
		G_PRIORITY_DEFAULT, NULL, read_bar, loop);
}

static int menu_app(void) {
	if (!iwd_connect(status_print)) {
		return 1;
	}
	status_print();
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GInputStream *in = g_unix_input_stream_new(0, FALSE);
	GDataInputStream *lines = g_data_input_stream_new(in);
	g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL,
		read_bar, loop);
	g_main_loop_run(loop);
	return 0;
}

/* ---- The window: state -------------------------------------------------- */

enum action {
	ACT_NONE,
	ACT_POWER_ON,
	ACT_POWER_OFF,
	ACT_SCAN,
	ACT_CONNECT,
	ACT_DISCONNECT,
	ACT_FORGET,
	ACT_ALERT_0, /* the alert's buttons */
	ACT_ALERT_1,
	ACT_CLOSE,
};

struct hit {
	int x, y, w, h;
	enum action action;
	char *path; /* the network, for its buttons */
};

enum alert_kind {
	ALERT_PASSWORD, /* answers iwd's RequestPassphrase */
	ALERT_FORGET,   /* asks before forgetting */
};

static struct {
	GtkWidget *window, *area;
	struct app_menu *menu;
	GArray *hits;
	enum action pressed;
	char *pressed_path;
	char *message;
	GHashTable *busy; /* network paths with a call in flight */
	int height;

	struct {
		bool active;
		enum alert_kind kind;
		char *text;       /* lines split by \n */
		const char *buttons[2];
		GString *entry;   /* the password typed, for ALERT_PASSWORD */
		GDBusMethodInvocation *invocation;
		char *path;
	} alert;
} ui;

static void set_message(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void set_message(const char *format, ...) {
	g_free(ui.message);
	va_list args;
	va_start(args, format);
	ui.message = g_strdup_vprintf(format, args);
	va_end(args);
	gtk_widget_queue_draw(ui.area);
}

static char *network_name(const char *path) {
	GDBusProxy *p = interface_at(path, IWD ".Network");
	char *name = p ? prop_string(p, "Name") : NULL;
	g_clear_object(&p);
	return name != NULL ? name : g_strdup("the network");
}

/* ---- The window: alerts and the agent ----------------------------------- */

static void alert_close(void) {
	if (ui.alert.invocation != NULL) {
		g_dbus_method_invocation_return_dbus_error(ui.alert.invocation,
			IWD ".Agent.Error.Canceled", "Canceled");
	}
	g_free(ui.alert.text);
	g_free(ui.alert.path);
	if (ui.alert.entry != NULL) {
		/* Don't leave the password lying about in memory. */
		memset(ui.alert.entry->str, 0, ui.alert.entry->len);
		g_string_free(ui.alert.entry, TRUE);
	}
	memset(&ui.alert, 0, sizeof(ui.alert));
	gtk_widget_queue_draw(ui.area);
}

static void alert_show(enum alert_kind kind, char *text, const char *yes,
		const char *no, GDBusMethodInvocation *invocation, const char *path) {
	alert_close();
	ui.alert.active = true;
	ui.alert.kind = kind;
	ui.alert.text = text;
	ui.alert.buttons[0] = yes;
	ui.alert.buttons[1] = no;
	ui.alert.invocation = invocation;
	ui.alert.path = g_strdup(path);
	if (kind == ALERT_PASSWORD) {
		ui.alert.entry = g_string_new(NULL);
	}
	gtk_window_present(GTK_WINDOW(ui.window));
	gtk_widget_queue_draw(ui.area);
}

static void call(const char *path, const char *iface, const char *method,
	GVariant *params, enum action action, const char *name);

static void alert_answer(int button) {
	if (ui.alert.kind == ALERT_PASSWORD && button == 0 &&
			ui.alert.invocation != NULL) {
		g_dbus_method_invocation_return_value(ui.alert.invocation,
			g_variant_new("(s)", ui.alert.entry->str));
		ui.alert.invocation = NULL;
	} else if (ui.alert.kind == ALERT_FORGET && button == 0) {
		GDBusProxy *k = interface_at(ui.alert.path, IWD ".KnownNetwork");
		char *name = prop_string(k, "Name");
		call(ui.alert.path, IWD ".KnownNetwork", "Forget", NULL, ACT_FORGET,
			name);
		g_free(name);
		g_clear_object(&k);
	}
	alert_close(); /* anything unanswered is cancelled */
}

static const char agent_xml[] =
	"<node><interface name='net.connman.iwd.Agent'>"
	"<method name='Release'/>"
	"<method name='RequestPassphrase'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/></method>"
	"<method name='RequestPrivateKeyPassphrase'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/></method>"
	"<method name='RequestUserNameAndPassword'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
	"<method name='RequestUserPassword'><arg type='o' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='Cancel'><arg type='s' direction='in'/></method>"
	"</interface></node>";

static void agent_method(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *invocation, void *data) {
	if (strcmp(method, "RequestPassphrase") == 0) {
		const char *network;
		g_variant_get(params, "(&o)", &network);
		char *name = network_name(network);
		alert_show(ALERT_PASSWORD, g_strdup_printf("Password for %s:", name),
			"Connect", "Cancel", invocation, network);
		g_free(name);
	} else if (strcmp(method, "Cancel") == 0) {
		if (ui.alert.active && ui.alert.kind == ALERT_PASSWORD) {
			alert_close();
		}
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "Release") == 0) {
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else {
		/* Enterprise networks: usernames, certificates. */
		set_message("That kind of network needs setting up in iwd.");
		g_dbus_method_invocation_return_dbus_error(invocation,
			IWD ".Agent.Error.Canceled", "Not supported");
	}
}

static void agent_init(void) {
	GDBusConnection *bus = g_dbus_object_manager_client_get_connection(
		G_DBUS_OBJECT_MANAGER_CLIENT(iwd));
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
	static const GDBusInterfaceVTable vtable = { .method_call = agent_method };
	g_dbus_connection_register_object(bus, AGENT_PATH, info->interfaces[0],
		&vtable, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
	GVariant *r = g_dbus_connection_call_sync(bus, IWD, "/net/connman/iwd",
		IWD ".AgentManager", "RegisterAgent", g_variant_new("(o)", AGENT_PATH),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (r != NULL) {
		g_variant_unref(r);
	}
}

/* ---- The window: actions ------------------------------------------------ */

struct call {
	char *path, *name;
	enum action action;
};

static void call_done(GObject *source, GAsyncResult *result, void *data) {
	struct call *c = data;
	GError *error = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
		result, &error);
	if (c->path != NULL) {
		g_hash_table_remove(ui.busy, c->path);
	}
	if (error != NULL) {
		g_dbus_error_strip_remote_error(error);
		/* Cancelling the password isn't a failure worth reporting. */
		if (strstr(error->message, "Canceled") == NULL &&
				strstr(error->message, "Aborted") == NULL) {
			const char *what = c->action == ACT_CONNECT ? "Couldn't connect" :
				c->action == ACT_FORGET ? "Couldn't forget" :
				c->action == ACT_SCAN ? "Couldn't scan" : "Couldn't do that";
			set_message("%s: %s", what, error->message);
		} else {
			set_message("%s", "");
		}
		g_error_free(error);
	} else if (c->action == ACT_CONNECT) {
		set_message("Connected to %s.", c->name);
	} else if (c->action == ACT_FORGET) {
		set_message("Forgot %s.", c->name);
	} else {
		set_message("%s", "");
	}
	if (r != NULL) {
		g_variant_unref(r);
	}
	g_free(c->path);
	g_free(c->name);
	g_free(c);
	fetch();
}

/* A method call on iwd; name (the network's) is for the message when it's
 * done. */
static void call(const char *path, const char *iface, const char *method,
		GVariant *params, enum action action, const char *name) {
	struct call *c = g_new0(struct call, 1);
	c->action = action;
	if (action == ACT_CONNECT) {
		c->path = g_strdup(path);
		g_hash_table_add(ui.busy, g_strdup(path));
	}
	c->name = g_strdup(name != NULL ? name : "the network");
	GDBusConnection *bus = g_dbus_object_manager_client_get_connection(
		G_DBUS_OBJECT_MANAGER_CLIENT(iwd));
	/* Connecting waits for the password to be typed. */
	g_dbus_connection_call(bus, IWD, path, iface, method, params, NULL,
		G_DBUS_CALL_FLAGS_NONE, action == ACT_CONNECT ? G_MAXINT : 30000, NULL,
		call_done, c);
}

static void act(enum action action, const char *path) {
	GDBusProxy *d = device();
	GDBusProxy *s = station();
	switch (action) {
	case ACT_POWER_ON:
	case ACT_POWER_OFF:
		if (d != NULL) {
			call(g_dbus_proxy_get_object_path(d), "org.freedesktop.DBus.Properties",
				"Set", g_variant_new("(ssv)", IWD ".Device", "Powered",
					g_variant_new_boolean(action == ACT_POWER_ON)), action, NULL);
		}
		break;
	case ACT_SCAN:
		if (s != NULL) {
			call(g_dbus_proxy_get_object_path(s), IWD ".Station", "Scan", NULL,
				ACT_SCAN, NULL);
		}
		break;
	case ACT_CONNECT: {
		char *name = network_name(path);
		set_message("Connecting to %s...", name);
		call(path, IWD ".Network", "Connect", NULL, ACT_CONNECT, name);
		g_free(name);
		break;
	}
	case ACT_DISCONNECT:
		if (s != NULL) {
			call(g_dbus_proxy_get_object_path(s), IWD ".Station", "Disconnect",
				NULL, ACT_DISCONNECT, NULL);
		}
		break;
	case ACT_FORGET: {
		GDBusProxy *n = interface_at(path, IWD ".Network");
		char *known = prop_string(n, "KnownNetwork");
		char *name = network_name(path);
		if (known != NULL) {
			alert_show(ALERT_FORGET, g_strdup_printf("Forget %s?\nYou'll need "
				"its password again\nto connect.", name), "Forget", "Cancel",
				NULL, known);
		}
		g_free(name);
		g_free(known);
		g_clear_object(&n);
		break;
	}
	case ACT_ALERT_0:
	case ACT_ALERT_1:
		alert_answer(action == ACT_ALERT_0 ? 0 : 1);
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(ui.window));
		break;
	default:
		break;
	}
	g_clear_object(&d);
	g_clear_object(&s);
	gtk_widget_queue_draw(ui.area);
}

/* ---- The window: drawing ------------------------------------------------ */

static void add_hit(int x, int y, int w, int h, enum action action,
		const char *path) {
	struct hit hit = { x, y, w, h, action, g_strdup(path) };
	g_array_append_val(ui.hits, hit);
}

static int button_width(cairo_t *cr, const char *label) {
	return (int)gem_text_width(cr, label) + 2 * PAD;
}

static void button(cairo_t *cr, int x, int y, int w, const char *label,
		enum action action, const char *path, bool is_default, bool enabled) {
	bool pressed = enabled && ui.pressed == action &&
		g_strcmp0(ui.pressed_path, path) == 0;
	gem_black(cr);
	if (pressed) {
		gem_fill(cr, x, y, w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_frame(cr, x, y, w, BUTTON_H, is_default ? 2 : 1);
	}
	gem_text(cr, label, x + (w - gem_text_width(cr, label)) / 2, y, BUTTON_H);
	if (enabled) {
		add_hit(x, y, w, BUTTON_H, action, path);
	} else {
		gem_grey_out(cr, x, y, w, BUTTON_H);
	}
}

static void signal_icon(cairo_t *cr, int x, int y, int lit) {
	bool px[ICON_H][ICON_W];
	icon_pixels(px, lit, false);
	gem_black(cr);
	for (int r = 0; r < ICON_H; r++) {
		for (int c = 0; c < ICON_W; c++) {
			if (px[r][c]) {
				gem_fill(cr, x + c, y + r, 1, 1);
			}
		}
	}
}

static void lock_icon(cairo_t *cr, int x, int y) {
	static const char *const rows[] = {
		".###.", "#...#", "#...#", "#####", "##.##", "##.##", "#####",
	};
	gem_black(cr);
	gem_bitmap(cr, rows, 7, x, y);
}

static void check_mark(cairo_t *cr, int x, int y) {
	static const char *const rows[] = {
		".......#", "......##", "#....##.", "##..##..", ".####...", "..##....",
	};
	gem_black(cr);
	gem_bitmap(cr, rows, 6, x, y);
}

static void network_row(cairo_t *cr, struct network *n, int y, int w) {
	bool busy = g_hash_table_contains(ui.busy, n->path);
	int by = y + (ROW_H - BUTTON_H) / 2;
	int x = w - PAD;
	if (n->known != NULL) {
		int fw = button_width(cr, "Forget");
		x -= fw;
		button(cr, x, by, fw, "Forget", ACT_FORGET, n->path, false, !busy);
		x -= PAD;
	}
	int cw = button_width(cr, "Disconnect");
	x -= cw;
	button(cr, x, by, cw, n->connected ? "Disconnect" : "Connect",
		n->connected ? ACT_DISCONNECT : ACT_CONNECT, n->path, false, !busy);

	if (n->connected) {
		check_mark(cr, PAD, y + (ROW_H - 6) / 2);
	}
	signal_icon(cr, PAD + 12, y + (ROW_H - ICON_H) / 2, bars(n->dbm));
	int name_x = PAD + 12 + ICON_W + 6;
	bool locked = g_strcmp0(n->type, "open") != 0;
	int name_end = x - PAD - (locked ? 10 : 0);
	cairo_save(cr);
	cairo_rectangle(cr, name_x, y, name_end - name_x, ROW_H);
	cairo_clip(cr);
	gem_black(cr);
	gem_text(cr, n->name != NULL ? n->name : "?", name_x, y, ROW_H);
	double end = name_x + gem_text_width(cr, n->name ? n->name : "?");
	cairo_restore(cr);
	if (locked) {
		lock_icon(cr, (int)MIN(end + 6, name_end + 2), y + (ROW_H - 7) / 2);
	}
}

static int note(cairo_t *cr, const char *s, int y, int w) {
	gem_black(cr);
	gem_text(cr, s, PAD + 12, y, ROW_H);
	gem_grey_out(cr, 0, y, w, ROW_H);
	return y + ROW_H;
}

static void paint_alert(cairo_t *cr, int w, int h) {
	char **lines = g_strsplit(ui.alert.text, "\n", -1);
	int n = g_strv_length(lines);
	bool entry = ui.alert.kind == ALERT_PASSWORD;
	int tw = entry ? 240 : 0;
	for (int i = 0; i < n; i++) {
		tw = MAX(tw, (int)gem_text_width(cr, lines[i]));
	}
	int bw = MAX(button_width(cr, ui.alert.buttons[0]),
		button_width(cr, ui.alert.buttons[1]));
	int buttons_w = 2 * bw + PAD;
	int aw = MAX(tw, buttons_w) + 4 * PAD;
	int ah = n * 18 + (entry ? BUTTON_H + PAD : 0) + BUTTON_H + 5 * PAD;
	int ax = (w - aw) / 2, ay = (h - ah) / 2;
	gem_white(cr);
	gem_fill(cr, ax - 3, ay - 3, aw + 6, ah + 6);
	gem_black(cr);
	gem_frame(cr, ax - 3, ay - 3, aw + 6, ah + 6, 1);
	gem_frame(cr, ax, ay, aw, ah, 2);
	int y = ay + 2 * PAD;
	for (int i = 0; i < n; i++, y += 18) {
		gem_text(cr, lines[i], ax + 2 * PAD, y, 18);
	}
	if (entry) {
		/* A GEM text field: the password as stars, and a block cursor. */
		int ex = ax + 2 * PAD, ew = aw - 4 * PAD;
		gem_frame(cr, ex, y, ew, BUTTON_H, 1);
		glong len = g_utf8_strlen(ui.alert.entry->str, -1);
		char *stars = g_strnfill(len, '*');
		cairo_save(cr);
		cairo_rectangle(cr, ex + 1, y + 1, ew - 2, BUTTON_H - 2);
		cairo_clip(cr);
		gem_text(cr, stars, ex + 4, y, BUTTON_H);
		gem_fill(cr, ex + 4 + gem_text_width(cr, stars) + 1, y + 3, 7,
			BUTTON_H - 6);
		cairo_restore(cr);
		g_free(stars);
		y += BUTTON_H + PAD;
	}
	int bx = ax + aw - 2 * PAD - buttons_w;
	int by = ay + ah - 2 * PAD - BUTTON_H;
	/* The password's default is Connect; forgetting's is the safe Cancel. */
	int def = ui.alert.kind == ALERT_PASSWORD ? 0 : 1;
	for (int i = 0; i < 2; i++) {
		button(cr, bx + i * (bw + PAD), by, bw, ui.alert.buttons[i],
			i == 0 ? ACT_ALERT_0 : ACT_ALERT_1, NULL, i == def, true);
	}
	g_strfreev(lines);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	for (guint i = 0; i < ui.hits->len; i++) {
		g_free(g_array_index(ui.hits, struct hit, i).path);
	}
	g_array_set_size(ui.hits, 0);
	GDBusProxy *d = device();
	GDBusProxy *s = station();
	bool powered = d != NULL && prop_bool(d, "Powered");
	bool scanning = s != NULL && prop_bool(s, "Scanning");

	/* Top: the power switch, as a pair of GEM radio buttons. */
	gem_black(cr);
	gem_text(cr, "Wi-Fi", PAD, 0, BAR_H);
	int rw = button_width(cr, "Off");
	int by = (BAR_H - BUTTON_H) / 2;
	if (d != NULL) {
		for (int i = 0; i < 2; i++) {
			bool on = i == 0, selected = on == powered;
			int x = on ? w - PAD - 2 * rw + 1 : w - PAD - rw;
			gem_black(cr);
			if (selected) {
				gem_fill(cr, x, by, rw, BUTTON_H);
				gem_white(cr);
			} else {
				gem_frame(cr, x, by, rw, BUTTON_H, 1);
			}
			const char *label = on ? "On" : "Off";
			gem_text(cr, label, x + (rw - gem_text_width(cr, label)) / 2, by,
				BUTTON_H);
			if (!selected) {
				add_hit(x, by, rw, BUTTON_H, on ? ACT_POWER_ON : ACT_POWER_OFF,
					NULL);
			}
		}
	}
	gem_black(cr);
	gem_fill(cr, 0, BAR_H - 1, w, 1);

	int y = BAR_H + PAD / 2;
	if (d == NULL) {
		y = note(cr, "No Wi-Fi device.", y, w);
	} else if (!powered) {
		y = note(cr, "Wi-Fi is off.", y, w);
	} else if (model.networks == NULL || model.networks->len == 0) {
		y = note(cr, scanning ? "Looking..." : "No networks found.", y, w);
	} else {
		for (guint i = 0; i < model.networks->len && i < MAX_SHOWN; i++) {
			network_row(cr, g_ptr_array_index(model.networks, i), y, w);
			y += ROW_H;
		}
	}
	y += PAD / 2;

	/* Bottom: what's happening, and Scan. */
	int foot = MAX(y, h - BAR_H);
	gem_black(cr);
	gem_fill(cr, 0, foot, w, 1);
	int sw = button_width(cr, "Scan");
	cairo_save(cr);
	cairo_rectangle(cr, 0, foot, w - 2 * PAD - sw, BAR_H);
	cairo_clip(cr);
	gem_text(cr, scanning ? "Scanning..." : ui.message ? ui.message : "", PAD,
		foot, BAR_H);
	cairo_restore(cr);
	button(cr, w - PAD - sw, foot + by, sw, "Scan", ACT_SCAN, NULL, true,
		powered && !scanning);

	if (ui.alert.active) {
		/* Only the alert's buttons answer while it's up. */
		for (guint i = 0; i < ui.hits->len; i++) {
			g_free(g_array_index(ui.hits, struct hit, i).path);
		}
		g_array_set_size(ui.hits, 0);
		paint_alert(cr, w, h);
	}
	/* The window grows to fit the list. */
	int need = y + BAR_H;
	if (need != ui.height) {
		ui.height = need;
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), need);
	}
	g_clear_object(&d);
	g_clear_object(&s);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	gem_draw_pixelated(cr, w, h, paint, NULL);
}

/* ---- The window: input -------------------------------------------------- */

static struct hit *hit_at(double x, double y) {
	for (guint i = 0; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (x >= hit->x && x < hit->x + hit->w && y >= hit->y &&
				y < hit->y + hit->h) {
			return hit;
		}
	}
	return NULL;
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	ui.pressed = hit != NULL ? hit->action : ACT_NONE;
	g_free(ui.pressed_path);
	ui.pressed_path = hit != NULL ? g_strdup(hit->path) : NULL;
	gtk_widget_queue_draw(ui.area);
}

/* As in GEM, a button acts when released over it. */
static void released(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	enum action action = ui.pressed;
	char *path = ui.pressed_path;
	ui.pressed = ACT_NONE;
	ui.pressed_path = NULL;
	if (hit != NULL && hit->action == action &&
			g_strcmp0(hit->path, path) == 0) {
		act(action, path);
	}
	g_free(path);
	gtk_widget_queue_draw(ui.area);
}

static void pasted(GObject *source, GAsyncResult *result, void *data) {
	char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result,
		NULL);
	if (text != NULL && ui.alert.entry != NULL) {
		/* Only the first line: a password never has a newline. */
		text[strcspn(text, "\r\n")] = '\0';
		g_string_append(ui.alert.entry, text);
		gtk_widget_queue_draw(ui.area);
	}
	g_free(text);
}

/* Typing goes to the password field while it's up. */
static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
		guint code, GdkModifierType mods, void *data) {
	bool ctrl = mods & GDK_CONTROL_MASK;
	if (ui.alert.active) {
		if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
			act(ui.alert.kind == ALERT_PASSWORD ? ACT_ALERT_0 : ACT_ALERT_1,
				NULL);
		} else if (keyval == GDK_KEY_Escape) {
			act(ACT_ALERT_1, NULL);
		} else if (ui.alert.entry != NULL && keyval == GDK_KEY_BackSpace) {
			char *end = ui.alert.entry->str + ui.alert.entry->len;
			char *prev = g_utf8_find_prev_char(ui.alert.entry->str, end);
			if (prev != NULL) {
				g_string_truncate(ui.alert.entry, prev - ui.alert.entry->str);
			}
		} else if (ui.alert.entry != NULL && ctrl &&
				(keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
			gdk_clipboard_read_text_async(gtk_widget_get_clipboard(ui.area),
				NULL, pasted, NULL);
		} else if (ui.alert.entry != NULL && !ctrl) {
			gunichar c = gdk_keyval_to_unicode(keyval);
			if (c >= 0x20 && c != 0x7f) {
				g_string_append_unichar(ui.alert.entry, c);
			}
		}
		gtk_widget_queue_draw(ui.area);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape ||
			(ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_W))) {
		act(ACT_CLOSE, NULL);
		return TRUE;
	}
	return FALSE;
}

/* The menus in GemWM's menu bar. Options is merged into GemWM's own. */
static void build_menus(struct app_menu *m, void *data) {
	GDBusProxy *d = device();
	GDBusProxy *s = station();
	bool powered = d != NULL && prop_bool(d, "Powered");
	bool scanning = s != NULL && prop_bool(s, "Scanning");
	uint32_t none = d == NULL ? APP_MENU_DISABLED : 0;
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_SCAN, scanning ? "Scanning..." : "Scan", "",
		powered && !scanning ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
	app_menu_add_menu(m, "Options");
	app_menu_add_item(m, ACT_POWER_ON, "Wi-Fi On", "",
		none | (powered ? APP_MENU_CHECKED : 0));
	app_menu_add_item(m, ACT_POWER_OFF, "Wi-Fi Off", "",
		none | (d != NULL && !powered ? APP_MENU_CHECKED : 0));
	g_clear_object(&d);
	g_clear_object(&s);
}

static void menu_activate(uint32_t id, void *data) {
	if (!ui.alert.active) {
		act((enum action)id, NULL);
	}
}

static void window_changed(void) {
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

static void activate(GtkApplication *app, void *data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	ui.hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	ui.busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "Wi-Fi");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 440, -1);
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), 160);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(ui.window), ui.area);

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);

	if (!iwd_connect(window_changed)) {
		set_message("iwd isn't running.");
	} else {
		agent_init();
	}
	gtk_window_present(GTK_WINDOW(ui.window));
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "--menu-app") == 0) {
		self = strchr(argv[0], '/') != NULL ? g_strdup(argv[0]) :
			g_find_program_in_path(argv[0]);
		return menu_app();
	}
	GtkApplication *app = gtk_application_new("org.gemwm.WiFi",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
