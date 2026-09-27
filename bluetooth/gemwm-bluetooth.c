/*
 * gemwm-bluetooth: Bluetooth for GemWM, GEM style.
 *
 *   gemwm-bluetooth              the window: turn Bluetooth on and off,
 *                                scan, pair, connect and forget devices
 *   gemwm-bluetooth --menu-app   the menu bar item (see menu/menu.c): the
 *                                Bluetooth rune, greyed when it's off, and
 *                                what's connected; a click opens the window
 *
 * Both talk to BlueZ on the system bus. The window is drawn like GemWeb's
 * chrome, in black and white at 1x and pixel-doubled on HiDPI screens, and
 * under GemWM its device list scrolls with the frame's scroll bar
 * (gemwm-scroll-v1), and its File and Options menus are in the menu bar
 * (lib/app-menu.c). While it's open it is the BlueZ agent for the pairings
 * it starts, so passkeys are shown and confirmed in GEM alert boxes.
 */
#include <cairo.h>
#include <gdk/wayland/gdkwayland.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app-menu.h"
#include "gemwm-scroll-v1-client-protocol.h"

#define FONT_SIZE 14
#define PAD 8          /* margin, and space inside buttons */
#define BAR_H 30       /* the top row (power) and the bottom row (scan) */
#define ROW_H 24       /* one device */
#define BUTTON_H 18
#define SCAN_SECONDS 30
#define AGENT_PATH "/org/gemwm/bluetooth/agent"

static const char *font_family;
static GDBusObjectManager *bluez; /* org.bluez's objects, kept up to date */

/* ---- BlueZ -------------------------------------------------------------- */

static bool prop_bool(GDBusProxy *proxy, const char *name) {
	GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
	bool b = v != NULL && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
		g_variant_get_boolean(v);
	if (v != NULL) {
		g_variant_unref(v);
	}
	return b;
}

static char *prop_string(GDBusProxy *proxy, const char *name) {
	GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
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

static GDBusProxy *interface(GDBusObject *object, const char *name) {
	GDBusInterface *i = g_dbus_object_get_interface(object, name);
	return i != NULL ? G_DBUS_PROXY(i) : NULL;
}

/* The first adapter (hci0 on most machines), or NULL. Unref it. */
static GDBusProxy *adapter(void) {
	GList *objects = g_dbus_object_manager_get_objects(bluez);
	GDBusProxy *found = NULL;
	for (GList *l = objects; l != NULL; l = l->next) {
		GDBusProxy *a = interface(l->data, "org.bluez.Adapter1");
		if (a == NULL) {
			continue;
		}
		if (found == NULL || strcmp(g_dbus_proxy_get_object_path(a),
				g_dbus_proxy_get_object_path(found)) < 0) {
			g_clear_object(&found);
			found = a;
		} else {
			g_object_unref(a);
		}
	}
	g_list_free_full(objects, g_object_unref);
	return found;
}

struct device {
	char *path, *name;
	bool paired, connected;
	int rssi; /* INT_MIN when unknown */
};

static void device_free(void *data) {
	struct device *d = data;
	g_free(d->path);
	g_free(d->name);
	g_free(d);
}

/* Names go in single lines of the bar and window. */
static char *clean_name(char *name) {
	for (char *p = name; *p != '\0'; p++) {
		if (*p == '\t' || *p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
	return name;
}

static int device_order(const void *pa, const void *pb) {
	const struct device *a = *(struct device **)pa, *b = *(struct device **)pb;
	if (a->paired != b->paired) {
		return a->paired ? -1 : 1;
	}
	if (!a->paired && a->rssi != b->rssi) {
		return a->rssi > b->rssi ? -1 : 1; /* nearest first */
	}
	return g_utf8_collate(a->name, b->name);
}

/* The adapter's devices: paired ones by name, then the others nearest
 * first. Unpaired devices that don't give a name (most of the Bluetooth
 * LE chatter around) are left out. */
static GPtrArray *devices(const char *adapter_path) {
	GPtrArray *list = g_ptr_array_new_with_free_func(device_free);
	GList *objects = g_dbus_object_manager_get_objects(bluez);
	for (GList *l = objects; l != NULL; l = l->next) {
		GDBusProxy *p = interface(l->data, "org.bluez.Device1");
		if (p == NULL) {
			continue;
		}
		char *owner = prop_string(p, "Adapter");
		char *name = prop_string(p, "Name");
		bool paired = prop_bool(p, "Paired");
		if (g_strcmp0(owner, adapter_path) == 0 && (paired || name != NULL)) {
			struct device *d = g_new0(struct device, 1);
			d->path = g_strdup(g_dbus_proxy_get_object_path(p));
			d->name = prop_string(p, "Alias");
			if (d->name == NULL) {
				d->name = g_strdup(name != NULL ? name : "Unknown");
			}
			clean_name(d->name);
			d->paired = paired;
			d->connected = prop_bool(p, "Connected");
			GVariant *rssi = g_dbus_proxy_get_cached_property(p, "RSSI");
			d->rssi = rssi != NULL ? g_variant_get_int16(rssi) : INT_MIN;
			if (rssi != NULL) {
				g_variant_unref(rssi);
			}
			g_ptr_array_add(list, d);
		}
		g_free(owner);
		g_free(name);
		g_object_unref(p);
	}
	g_list_free_full(objects, g_object_unref);
	g_ptr_array_sort(list, device_order);
	return list;
}

static char *device_name(const char *path) {
	GDBusObject *object = g_dbus_object_manager_get_object(bluez, path);
	GDBusProxy *p = object ? interface(object, "org.bluez.Device1") : NULL;
	char *name = p ? prop_string(p, "Alias") : NULL;
	g_clear_object(&p);
	g_clear_object(&object);
	return clean_name(name != NULL ? name : g_strdup("the device"));
}

static GDBusConnection *system_bus(void) {
	return g_dbus_object_manager_client_get_connection(
		G_DBUS_OBJECT_MANAGER_CLIENT(bluez));
}

/* Anything in BlueZ changed: devices came or went, connected, were named.
 * Many changes arrive together, so they're handled once, when idle. */
static void (*on_change)(void);
static guint change_idle;

static gboolean change_run(void *data) {
	change_idle = 0;
	on_change();
	return G_SOURCE_REMOVE;
}

static void bluez_changed(void *data) {
	if (change_idle == 0) {
		change_idle = g_idle_add(change_run, NULL);
	}
}

static bool bluez_connect(void) {
	GError *error = NULL;
	bluez = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/",
		NULL, NULL, NULL, NULL, &error);
	if (bluez == NULL) {
		fprintf(stderr, "gemwm-bluetooth: %s\n", error->message);
		g_error_free(error);
		return false;
	}
	static const char *signals[] = { "object-added", "object-removed",
		"interface-added", "interface-removed",
		"interface-proxy-properties-changed" };
	for (size_t i = 0; i < G_N_ELEMENTS(signals); i++) {
		g_signal_connect_swapped(bluez, signals[i],
			G_CALLBACK(bluez_changed), NULL);
	}
	/* bluetoothd starting or stopping. */
	g_signal_connect_swapped(bluez, "notify::name-owner",
		G_CALLBACK(bluez_changed), NULL);
	return true;
}

/* ---- The menu bar item -------------------------------------------------- */

static char *self;        /* this program, to start the window */
static char *last_status;

static void status_print(void) {
	GDBusProxy *a = adapter();
	char *line;
	if (a == NULL) {
		line = g_strdup(""); /* no Bluetooth here: no item */
	} else if (!prop_bool(a, "Powered")) {
		line = g_strdup(ICON_DIR "/bluetooth-off.png\t");
	} else {
		GPtrArray *list = devices(g_dbus_proxy_get_object_path(a));
		int n = 0;
		const char *name = NULL;
		for (guint i = 0; i < list->len; i++) {
			struct device *d = g_ptr_array_index(list, i);
			if (d->connected) {
				n++;
				name = d->name;
			}
		}
		if (n > 1) {
			line = g_strdup_printf(ICON_DIR "/bluetooth.png\t%d devices", n);
		} else {
			line = g_strdup_printf(ICON_DIR "/bluetooth.png\t%s",
				n == 1 ? name : "");
		}
		g_ptr_array_unref(list);
	}
	g_clear_object(&a);
	if (g_strcmp0(line, last_status) != 0) {
		printf("%s\n", line);
		fflush(stdout);
		g_free(last_status);
		last_status = line;
	} else {
		g_free(line);
	}
}

static void read_click(GObject *source, GAsyncResult *result, void *data) {
	GMainLoop *loop = data;
	char *line = g_data_input_stream_read_line_finish(
		G_DATA_INPUT_STREAM(source), result, NULL, NULL);
	if (line == NULL) {
		g_main_loop_quit(loop); /* the bar has gone */
		return;
	}
	if (g_str_has_prefix(line, "click")) {
		/* Single instance: a second click brings the window forward. */
		char *argv[] = { self, NULL };
		GError *error = NULL;
		if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
				NULL, &error)) {
			fprintf(stderr, "gemwm-bluetooth: %s\n", error->message);
			g_error_free(error);
		}
	}
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source),
		G_PRIORITY_DEFAULT, NULL, read_click, loop);
}

static int menu_app(void) {
	if (!bluez_connect()) {
		return 1;
	}
	on_change = status_print;
	status_print();
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GInputStream *in = g_unix_input_stream_new(0, FALSE);
	GDataInputStream *lines = g_data_input_stream_new(in);
	g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL,
		read_click, loop);
	g_main_loop_run(loop);
	return 0;
}

/* ---- The window: state -------------------------------------------------- */

enum action {
	ACT_NONE,
	ACT_POWER_ON,
	ACT_POWER_OFF,
	ACT_SCAN,
	ACT_STOP_SCAN,
	ACT_CONNECT,
	ACT_DISCONNECT,
	ACT_PAIR,
	ACT_FORGET,
	ACT_TRUST,
	ACT_REMOVE,
	ACT_ALERT_0, /* the alert's buttons */
	ACT_ALERT_1,
	ACT_CLOSE,
};

/* Something clickable, as last drawn. */
struct hit {
	int x, y, w, h;
	enum action action;
	char *path; /* the device, for device buttons */
};

enum alert_kind {
	ALERT_NOTICE,  /* just OK */
	ALERT_CONFIRM, /* answers BlueZ's pending question */
	ALERT_FORGET,  /* asks before forgetting path */
};

static struct {
	GtkWidget *window, *area;
	GArray *hits;
	enum action pressed; /* button held down, drawn inverted */
	char *pressed_path;
	char *message;       /* the status line */
	GHashTable *busy;    /* device paths with a call in flight */
	guint scan_timer;
	bool agent;          /* our agent is registered with BlueZ */
	int scroll;          /* the device list's offset */
	int list_h, content_h;

	struct app_menu *menu;
	struct gemwm_scroll_manager_v1 *scroll_manager;
	struct gemwm_scroll_v1 *scroll_bar;
	int reported[3];

	struct {
		bool active;
		enum alert_kind kind;
		char *text;             /* lines split by \n */
		const char *buttons[2]; /* the second may be NULL */
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

/* ---- The window: alerts and the pairing agent --------------------------- */

static void alert_close(void) {
	if (ui.alert.invocation != NULL) {
		g_dbus_method_invocation_return_dbus_error(ui.alert.invocation,
			"org.bluez.Error.Rejected", "Rejected");
	}
	g_free(ui.alert.text);
	g_free(ui.alert.path);
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
	gtk_window_present(GTK_WINDOW(ui.window));
	gtk_widget_queue_draw(ui.area);
}

static void call(const char *path, const char *iface, const char *method,
	GVariant *params, enum action action);

static void alert_answer(int button) {
	if (ui.alert.kind == ALERT_CONFIRM && ui.alert.invocation != NULL &&
			button == 0) {
		g_dbus_method_invocation_return_value(ui.alert.invocation, NULL);
		ui.alert.invocation = NULL;
	} else if (ui.alert.kind == ALERT_FORGET && button == 0) {
		GDBusProxy *a = adapter();
		if (a != NULL) {
			call(g_dbus_proxy_get_object_path(a), "org.bluez.Adapter1",
				"RemoveDevice", g_variant_new("(o)", ui.alert.path),
				ACT_REMOVE);
			g_object_unref(a);
		}
	}
	alert_close(); /* anything unanswered is rejected */
}

static const char agent_xml[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/>"
	"<arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/>"
	"<arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/>"
	"<arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/>"
	"<arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/>"
	"<arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

/* BlueZ asking us about a pairing we started. As "DisplayYesNo" we can
 * show a code or ask whether two codes match, which covers headphones,
 * keyboards, mice and phones. */
static void agent_method(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *invocation, void *data) {
	const char *device = NULL;
	if (g_variant_n_children(params) > 0) {
		g_variant_get_child(params, 0, "&o", &device);
	}
	char *name = device != NULL ? device_name(device) : NULL;

	if (strcmp(method, "RequestConfirmation") == 0) {
		guint32 passkey;
		g_variant_get(params, "(&ou)", NULL, &passkey);
		alert_show(ALERT_CONFIRM, g_strdup_printf(
			"Does %s show\nthe code %06u?", name, passkey),
			"Yes", "No", invocation, device);
	} else if (strcmp(method, "DisplayPasskey") == 0) {
		guint32 passkey;
		guint16 entered;
		g_variant_get(params, "(&ouq)", NULL, &passkey, &entered);
		if (entered == 0) {
			alert_show(ALERT_NOTICE, g_strdup_printf(
				"Type %06u on %s,\nthen press Enter.", passkey, name),
				"OK", NULL, NULL, device);
		}
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "DisplayPinCode") == 0) {
		const char *pin;
		g_variant_get(params, "(&o&s)", NULL, &pin);
		alert_show(ALERT_NOTICE, g_strdup_printf(
			"Type %s on %s,\nthen press Enter.", pin, name),
			"OK", NULL, NULL, device);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "RequestPinCode") == 0) {
		/* Old devices with a fixed PIN nearly all use 0000. */
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(s)", "0000"));
	} else if (strcmp(method, "RequestAuthorization") == 0 ||
			strcmp(method, "AuthorizeService") == 0) {
		/* Only pairings started from this window reach us. */
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "Cancel") == 0) {
		if (ui.alert.active && ui.alert.kind != ALERT_FORGET) {
			alert_close();
		}
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method, "Release") == 0) {
		ui.agent = false;
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else {
		g_dbus_method_invocation_return_dbus_error(invocation,
			"org.bluez.Error.Rejected", "Not supported");
	}
	g_free(name);
}

static void agent_init(void) {
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
	static const GDBusInterfaceVTable vtable = { .method_call = agent_method };
	g_dbus_connection_register_object(system_bus(), AGENT_PATH,
		info->interfaces[0], &vtable, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

/* Registered just before pairing, in case bluetoothd restarted since. */
static void agent_register(void) {
	if (ui.agent) {
		return;
	}
	GVariant *r = g_dbus_connection_call_sync(system_bus(), "org.bluez",
		"/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
		g_variant_new("(os)", AGENT_PATH, "DisplayYesNo"), NULL,
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (r != NULL) {
		ui.agent = true;
		g_variant_unref(r);
	}
}

/* ---- The window: actions ------------------------------------------------ */

struct call {
	char *path;
	enum action action;
};

static const char *failure(enum action action) {
	switch (action) {
	case ACT_POWER_ON: return "Couldn't turn Bluetooth on";
	case ACT_POWER_OFF: return "Couldn't turn Bluetooth off";
	case ACT_SCAN: return "Couldn't scan";
	case ACT_CONNECT: return "Couldn't connect";
	case ACT_DISCONNECT: return "Couldn't disconnect";
	case ACT_PAIR: return "Couldn't pair";
	case ACT_REMOVE: return "Couldn't forget";
	default: return NULL;
	}
}

static void call_done(GObject *source, GAsyncResult *result, void *data) {
	struct call *c = data;
	GError *error = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
		result, &error);
	if (c->path != NULL) {
		g_hash_table_remove(ui.busy, c->path);
	}
	char *name = c->path != NULL ? device_name(c->path) : NULL;
	bool already = error != NULL && c->action == ACT_PAIR &&
		strstr(error->message, "AlreadyExists") != NULL;
	if (ui.alert.active && ui.alert.kind != ALERT_FORGET &&
			c->action == ACT_PAIR) {
		alert_close(); /* the code shown for this pairing */
	}

	if (error != NULL && !already) {
		g_dbus_error_strip_remote_error(error);
		if (failure(c->action) != NULL) {
			set_message("%s: %s", failure(c->action), error->message);
		}
		g_error_free(error);
	} else {
		g_clear_error(&error);
		switch (c->action) {
		case ACT_PAIR:
			/* Trusted devices may reconnect by themselves later. */
			call(c->path, "org.freedesktop.DBus.Properties", "Set",
				g_variant_new("(ssv)", "org.bluez.Device1", "Trusted",
					g_variant_new_boolean(TRUE)), ACT_TRUST);
			set_message("Connecting to %s...", name);
			call(c->path, "org.bluez.Device1", "Connect", NULL, ACT_CONNECT);
			break;
		case ACT_CONNECT:
			set_message("Connected to %s.", name);
			break;
		case ACT_REMOVE:
			set_message("Forgot %s.", name);
			break;
		case ACT_SCAN:
			set_message("Looking for devices...");
			break;
		case ACT_DISCONNECT:
		case ACT_POWER_ON:
		case ACT_POWER_OFF:
		case ACT_STOP_SCAN:
			set_message("%s", "");
			break;
		default:
			break;
		}
	}
	if (r != NULL) {
		g_variant_unref(r);
	}
	g_free(name);
	g_free(c->path);
	g_free(c);
	gtk_widget_queue_draw(ui.area);
}

static void call(const char *path, const char *iface, const char *method,
		GVariant *params, enum action action) {
	struct call *c = g_new0(struct call, 1);
	c->action = action;
	bool device = strcmp(iface, "org.bluez.Device1") == 0;
	if (device) {
		c->path = g_strdup(path);
		g_hash_table_add(ui.busy, g_strdup(path));
	}
	/* Pairing waits on the person at the other device. */
	int timeout = action == ACT_PAIR ? G_MAXINT : 60000;
	g_dbus_connection_call(system_bus(), "org.bluez", path, iface, method,
		params, NULL, G_DBUS_CALL_FLAGS_NONE, timeout, NULL, call_done, c);
}

static gboolean scan_timeout(void *data) {
	ui.scan_timer = 0;
	GDBusProxy *a = adapter();
	if (a != NULL) {
		call(g_dbus_proxy_get_object_path(a), "org.bluez.Adapter1",
			"StopDiscovery", NULL, ACT_STOP_SCAN);
		g_object_unref(a);
	}
	return G_SOURCE_REMOVE;
}

static void act(enum action action, const char *path) {
	GDBusProxy *a = adapter();
	const char *adapter_path = a ? g_dbus_proxy_get_object_path(a) : NULL;
	char *name = path != NULL ? device_name(path) : NULL;
	switch (action) {
	case ACT_POWER_ON:
	case ACT_POWER_OFF:
		if (adapter_path != NULL) {
			call(adapter_path, "org.freedesktop.DBus.Properties", "Set",
				g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered",
					g_variant_new_boolean(action == ACT_POWER_ON)),
				action);
		}
		break;
	case ACT_SCAN:
		if (adapter_path != NULL) {
			call(adapter_path, "org.bluez.Adapter1", "StartDiscovery", NULL,
				ACT_SCAN);
			if (ui.scan_timer != 0) {
				g_source_remove(ui.scan_timer);
			}
			ui.scan_timer = g_timeout_add_seconds(SCAN_SECONDS,
				scan_timeout, NULL);
		}
		break;
	case ACT_STOP_SCAN:
		if (ui.scan_timer != 0) {
			g_source_remove(ui.scan_timer);
		}
		scan_timeout(NULL);
		break;
	case ACT_CONNECT:
		set_message("Connecting to %s...", name);
		call(path, "org.bluez.Device1", "Connect", NULL, ACT_CONNECT);
		break;
	case ACT_DISCONNECT:
		set_message("Disconnecting %s...", name);
		call(path, "org.bluez.Device1", "Disconnect", NULL, ACT_DISCONNECT);
		break;
	case ACT_PAIR:
		agent_register();
		set_message("Pairing with %s...", name);
		call(path, "org.bluez.Device1", "Pair", NULL, ACT_PAIR);
		break;
	case ACT_FORGET:
		alert_show(ALERT_FORGET, g_strdup_printf("Forget %s?\nYou'll have "
			"to pair it again\nto use it.", name), "Forget", "Cancel",
			NULL, path);
		break;
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
	g_free(name);
	g_clear_object(&a);
}

/* ---- The window: drawing ------------------------------------------------ */

static void black(cairo_t *cr) { cairo_set_source_rgb(cr, 0, 0, 0); }
static void white(cairo_t *cr) { cairo_set_source_rgb(cr, 1, 1, 1); }

static void fill(cairo_t *cr, double x, double y, double w, double h) {
	if (w > 0 && h > 0) {
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
	}
}

static void frame(cairo_t *cr, int x, int y, int w, int h, int t) {
	fill(cr, x, y, w, t);
	fill(cr, x, y + h - t, w, t);
	fill(cr, x, y, t, h);
	fill(cr, x + w - t, y, t, h);
}

static double text_width(cairo_t *cr, const char *s) {
	cairo_text_extents_t te;
	cairo_text_extents(cr, s, &te);
	return te.x_advance;
}

/* Text vertically centred in a row starting at y. */
static void text(cairo_t *cr, const char *s, double x, double y, double row_h) {
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	cairo_move_to(cr, x,
		y + (int)((row_h - (fe.ascent + fe.descent)) / 2 + fe.ascent));
	cairo_show_text(cr, s);
}

/* GEM's disabled look: every other pixel knocked out. */
static void grey_out(cairo_t *cr, int x, int y, int w, int h) {
	static cairo_pattern_t *checker;
	if (checker == NULL) {
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 2, 2);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		px[0] = 255;
		px[stride + 1] = 255;
		cairo_surface_mark_dirty(s);
		checker = cairo_pattern_create_for_surface(s);
		cairo_surface_destroy(s);
		cairo_pattern_set_extend(checker, CAIRO_EXTEND_REPEAT);
		cairo_pattern_set_filter(checker, CAIRO_FILTER_NEAREST);
	}
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	white(cr);
	cairo_mask(cr, checker);
	cairo_restore(cr);
}

static void add_hit(int x, int y, int w, int h, enum action action,
		const char *path) {
	struct hit hit = { x, y, w, h, action, g_strdup(path) };
	g_array_append_val(ui.hits, hit);
}

static int button_width(cairo_t *cr, const char *label) {
	return (int)text_width(cr, label) + 2 * PAD;
}

/* A GEM button: outlined, thicker when it's the default, inverted while
 * pressed, grey when disabled. */
static void button(cairo_t *cr, int x, int y, int w, const char *label,
		enum action action, const char *path, bool is_default, bool enabled) {
	bool pressed = enabled && ui.pressed == action &&
		g_strcmp0(ui.pressed_path, path) == 0;
	black(cr);
	if (pressed) {
		fill(cr, x, y, w, BUTTON_H);
		white(cr);
	} else {
		frame(cr, x, y, w, BUTTON_H, is_default ? 2 : 1);
	}
	text(cr, label, x + (w - text_width(cr, label)) / 2, y, BUTTON_H);
	if (enabled) {
		add_hit(x, y, w, BUTTON_H, action, path);
	} else {
		grey_out(cr, x, y, w, BUTTON_H);
	}
}

/* GEM's menu check mark, for connected devices. */
static void check_mark(cairo_t *cr, int x, int y) {
	static const char *rows[] = {
		".......#",
		"......##",
		"#....##.",
		"##..##..",
		".####...",
		"..##....",
	};
	black(cr);
	for (int r = 0; r < 6; r++) {
		for (int c = 0; c < 8; c++) {
			if (rows[r][c] == '#') {
				fill(cr, x + c, y + r, 1, 1);
			}
		}
	}
}

/* A section heading in the list, underlined with a dotted rule. */
static void heading(cairo_t *cr, const char *label, int y, int w) {
	black(cr);
	text(cr, label, PAD, y, ROW_H);
	for (int x = PAD; x < w - PAD; x += 2) {
		fill(cr, x, y + ROW_H - 3, 1, 1);
	}
}

static void device_row(cairo_t *cr, struct device *d, int y, int w) {
	bool busy = g_hash_table_contains(ui.busy, d->path);
	int by = y + (ROW_H - BUTTON_H) / 2;
	int x = w - PAD;
	if (d->paired) {
		int fw = button_width(cr, "Forget");
		x -= fw;
		button(cr, x, by, fw, "Forget", ACT_FORGET, d->path, false, !busy);
		/* The wider label sizes both, so the column lines up. */
		int cw = button_width(cr, "Disconnect");
		x -= PAD + cw;
		button(cr, x, by, cw, d->connected ? "Disconnect" : "Connect",
			d->connected ? ACT_DISCONNECT : ACT_CONNECT, d->path, false,
			!busy);
	} else {
		int pw = button_width(cr, "Pair");
		x -= pw;
		button(cr, x, by, pw, "Pair", ACT_PAIR, d->path, false, !busy);
	}
	if (d->connected) {
		check_mark(cr, PAD, y + (ROW_H - 6) / 2);
	}
	cairo_save(cr);
	cairo_rectangle(cr, 0, y, x - PAD, ROW_H);
	cairo_clip(cr);
	black(cr);
	text(cr, d->name, PAD + 12, y, ROW_H);
	cairo_restore(cr);
}

static void list_message(cairo_t *cr, const char *s, int y, int w) {
	black(cr);
	text(cr, s, PAD + 12, y, ROW_H);
	grey_out(cr, 0, y, w, ROW_H);
}

/* The device list, from y (already scrolled); returns its height. */
static int paint_list(cairo_t *cr, GDBusProxy *a, bool scanning, int y, int w) {
	int top = y;
	if (a == NULL) {
		list_message(cr, "No Bluetooth adapter.", y, w);
		return ROW_H;
	}
	if (!prop_bool(a, "Powered")) {
		list_message(cr, "Bluetooth is off.", y, w);
		return ROW_H;
	}
	GPtrArray *list = devices(g_dbus_proxy_get_object_path(a));
	guint i = 0;
	heading(cr, "Paired Devices", y, w);
	y += ROW_H;
	for (; i < list->len && ((struct device *)list->pdata[i])->paired; i++) {
		device_row(cr, list->pdata[i], y, w);
		y += ROW_H;
	}
	if (i == 0) {
		list_message(cr, "None yet.", y, w);
		y += ROW_H;
	}
	y += ROW_H / 2;
	heading(cr, "Other Devices", y, w);
	y += ROW_H;
	if (i == list->len) {
		list_message(cr, scanning ? "Looking..." :
			"Scan to find devices.", y, w);
		y += ROW_H;
	}
	for (; i < list->len; i++) {
		device_row(cr, list->pdata[i], y, w);
		y += ROW_H;
	}
	g_ptr_array_unref(list);
	return y - top + PAD;
}

static void paint_alert(cairo_t *cr, int w, int h) {
	char **lines = g_strsplit(ui.alert.text, "\n", -1);
	int n = g_strv_length(lines);
	int tw = 0;
	for (int i = 0; i < n; i++) {
		int lw = (int)text_width(cr, lines[i]);
		tw = lw > tw ? lw : tw;
	}
	int bw = 0;
	for (int i = 0; i < 2; i++) {
		if (ui.alert.buttons[i] != NULL) {
			int b = button_width(cr, ui.alert.buttons[i]);
			bw = b > bw ? b : bw;
		}
	}
	int nb = ui.alert.buttons[1] != NULL ? 2 : 1;
	int buttons_w = nb * bw + (nb - 1) * PAD;
	int aw = (tw > buttons_w ? tw : buttons_w) + 4 * PAD;
	int ah = n * 18 + BUTTON_H + 5 * PAD;
	int ax = (w - aw) / 2, ay = (h - ah) / 2;
	/* GEM's alert box: outlined, with a gap, on a white field. */
	white(cr);
	fill(cr, ax - 3, ay - 3, aw + 6, ah + 6);
	black(cr);
	frame(cr, ax - 3, ay - 3, aw + 6, ah + 6, 1);
	frame(cr, ax, ay, aw, ah, 2);
	for (int i = 0; i < n; i++) {
		text(cr, lines[i], ax + 2 * PAD, ay + 2 * PAD + i * 18, 18);
	}
	int bx = ax + aw - 2 * PAD - buttons_w;
	int by = ay + ah - 2 * PAD - BUTTON_H;
	for (int i = 0; i < nb; i++) {
		/* The default is the safe answer: No/Cancel, or the only one. */
		button(cr, bx + i * (bw + PAD), by, bw, ui.alert.buttons[i],
			i == 0 ? ACT_ALERT_0 : ACT_ALERT_1, NULL, i == nb - 1, true);
	}
	g_strfreev(lines);
}

static void report_scroll(void) {
	int total = ui.content_h > ui.list_h ? ui.content_h : ui.list_h;
	int now[3] = { ui.scroll, ui.list_h, total };
	if (ui.scroll_bar == NULL || memcmp(now, ui.reported, sizeof(now)) == 0) {
		return;
	}
	memcpy(ui.reported, now, sizeof(now));
	gemwm_scroll_v1_set_axis(ui.scroll_bar, GEMWM_SCROLL_V1_AXIS_VERTICAL,
		ui.scroll, ui.list_h, total > 0 ? total : 1);
}

static void paint(cairo_t *c, int w, int h) {
	for (guint i = 0; i < ui.hits->len; i++) {
		g_free(g_array_index(ui.hits, struct hit, i).path);
	}
	g_array_set_size(ui.hits, 0);
	GDBusProxy *a = adapter();
	bool powered = a != NULL && prop_bool(a, "Powered");
	bool scanning = powered && prop_bool(a, "Discovering");

	/* Top: the power switch, as a pair of GEM radio buttons. */
	black(c);
	text(c, "Bluetooth", PAD, 0, BAR_H);
	int rw = button_width(c, "Off");
	int by = (BAR_H - BUTTON_H) / 2;
	int ox = w - PAD - rw, nx = ox - rw + 1;
	if (a != NULL) {
		for (int i = 0; i < 2; i++) {
			bool on = i == 0, selected = on == powered;
			int x = on ? nx : ox;
			black(c);
			if (selected) {
				fill(c, x, by, rw, BUTTON_H);
				white(c);
			} else {
				frame(c, x, by, rw, BUTTON_H, 1);
			}
			const char *label = on ? "On" : "Off";
			text(c, label, x + (rw - text_width(c, label)) / 2, by, BUTTON_H);
			if (!selected) {
				add_hit(x, by, rw, BUTTON_H, on ? ACT_POWER_ON : ACT_POWER_OFF,
					NULL);
			}
		}
	}
	black(c);
	fill(c, 0, BAR_H - 1, w, 1);

	/* The list, scrolled, between the bars. */
	ui.list_h = h - 2 * BAR_H;
	int max = ui.content_h - ui.list_h;
	ui.scroll = ui.scroll > max ? max : ui.scroll;
	ui.scroll = ui.scroll < 0 ? 0 : ui.scroll;
	cairo_save(c);
	cairo_rectangle(c, 0, BAR_H, w, ui.list_h);
	cairo_clip(c);
	guint first = ui.hits->len;
	ui.content_h = paint_list(c, a, scanning, BAR_H + PAD / 2 - ui.scroll, w);
	cairo_restore(c);
	/* Buttons scrolled out of sight can't be clicked. */
	for (guint i = first; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (hit->y < BAR_H || hit->y + hit->h > BAR_H + ui.list_h) {
			hit->action = ACT_NONE;
		}
	}

	/* Bottom: what's happening, and Scan. */
	black(c);
	fill(c, 0, h - BAR_H, w, 1);
	const char *scan = scanning ? "Stop" : "Scan";
	int sw = button_width(c, "Scan");
	cairo_save(c);
	cairo_rectangle(c, 0, h - BAR_H, w - 2 * PAD - sw, BAR_H);
	cairo_clip(c);
	text(c, ui.message != NULL ? ui.message : "", PAD, h - BAR_H, BAR_H);
	cairo_restore(c);
	button(c, w - PAD - sw, h - BAR_H + by, sw, scan,
		scanning ? ACT_STOP_SCAN : ACT_SCAN, NULL, true, powered);

	if (ui.alert.active) {
		/* Only the alert's buttons answer while it's up. */
		g_array_set_size(ui.hits, 0);
		paint_alert(c, w, h);
	}
	g_clear_object(&a);
	report_scroll();
}

/* Draws into a 1x image and pixel-doubles it onto the widget, so the
 * window has the same chunky pixels as GemWM's frames on HiDPI screens. */
static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *c = cairo_create(img);
	cairo_set_antialias(c, CAIRO_ANTIALIAS_NONE);
	cairo_select_font_face(c, font_family, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(c, FONT_SIZE);
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	cairo_set_font_options(c, opts);
	cairo_font_options_destroy(opts);
	white(c);
	cairo_paint(c);
	paint(c, w, h);
	cairo_destroy(c);

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(img);
}

/* ---- The window: input -------------------------------------------------- */

static struct hit *hit_at(double x, double y) {
	for (guint i = 0; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (hit->action != ACT_NONE && x >= hit->x && x < hit->x + hit->w &&
				y >= hit->y && y < hit->y + hit->h) {
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

static gboolean scrolled(GtkEventControllerScroll *controller, double dx,
		double dy, void *data) {
	ui.scroll += (int)(dy * ROW_H);
	gtk_widget_queue_draw(ui.area);
	return TRUE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint key,
		guint code, GdkModifierType mods, void *data) {
	if (ui.alert.active && (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter)) {
		act(ui.alert.buttons[1] != NULL ? ACT_ALERT_1 : ACT_ALERT_0, NULL);
		return TRUE;
	}
	if ((mods & GDK_CONTROL_MASK) && (key == GDK_KEY_w || key == GDK_KEY_W)) {
		act(ACT_CLOSE, NULL);
		return TRUE;
	}
	if (key == GDK_KEY_Escape) {
		if (ui.alert.active) {
			act(ui.alert.buttons[1] != NULL ? ACT_ALERT_1 : ACT_ALERT_0, NULL);
		} else {
			gtk_window_close(GTK_WINDOW(ui.window));
		}
		return TRUE;
	}
	return FALSE;
}

/* ---- The window: GemWM's scroll bar ------------------------------------- */

static void on_scroll_to(void *data, struct gemwm_scroll_v1 *bar,
		uint32_t axis, int32_t position) {
	if (axis == GEMWM_SCROLL_V1_AXIS_VERTICAL) {
		ui.scroll = position;
		gtk_widget_queue_draw(ui.area);
	}
}

static const struct gemwm_scroll_v1_listener scroll_listener = {
	.scroll_to = on_scroll_to,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *iface, uint32_t version) {
	if (strcmp(iface, gemwm_scroll_manager_v1_interface.name) == 0) {
		ui.scroll_manager = wl_registry_bind(registry, name,
			&gemwm_scroll_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void window_mapped(GtkWidget *window, void *data) {
	GdkDisplay *display = gtk_widget_get_display(window);
	if (!GDK_IS_WAYLAND_DISPLAY(display) || ui.scroll_bar != NULL) {
		return;
	}
	if (ui.scroll_manager == NULL) {
		struct wl_display *wl = gdk_wayland_display_get_wl_display(display);
		struct wl_registry *registry = wl_display_get_registry(wl);
		wl_registry_add_listener(registry, &registry_listener, NULL);
		wl_display_roundtrip(wl);
		wl_registry_destroy(registry);
	}
	GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
	if (ui.scroll_manager == NULL || !GDK_IS_WAYLAND_SURFACE(surface)) {
		return;
	}
	ui.scroll_bar = gemwm_scroll_manager_v1_get_scroll(ui.scroll_manager,
		gdk_wayland_surface_get_wl_surface(surface));
	gemwm_scroll_v1_add_listener(ui.scroll_bar, &scroll_listener, NULL);
	memset(ui.reported, -1, sizeof(ui.reported));
	report_scroll();
}

/* ---- The window: setup -------------------------------------------------- */

/* The menus in GemWM's menu bar. Options is merged into GemWM's own. */
static void build_menus(struct app_menu *m, void *data) {
	GDBusProxy *a = adapter();
	bool powered = a != NULL && prop_bool(a, "Powered");
	bool scanning = powered && prop_bool(a, "Discovering");
	bool present = a != NULL;
	uint32_t none = present ? 0 : APP_MENU_DISABLED;
	g_clear_object(&a);

	app_menu_add_menu(m, "File");
	if (scanning) {
		app_menu_add_item(m, ACT_STOP_SCAN, "Stop Scanning", "", 0);
	} else {
		app_menu_add_item(m, ACT_SCAN, "Scan", "",
			powered ? 0 : APP_MENU_DISABLED);
	}
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);

	app_menu_add_menu(m, "Options");
	app_menu_add_item(m, ACT_POWER_ON, "Bluetooth On", "",
		none | (powered ? APP_MENU_CHECKED : 0));
	app_menu_add_item(m, ACT_POWER_OFF, "Bluetooth Off", "",
		none | (present && !powered ? APP_MENU_CHECKED : 0));
}

static void menu_activate(uint32_t id, void *data) {
	if (!ui.alert.active) {
		act((enum action)id, NULL);
	}
}

static void redraw(void) {
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

static void activate(GtkApplication *app, void *data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	if (!bluez_connect()) {
		g_application_quit(G_APPLICATION(app));
		return;
	}
	on_change = redraw;
	ui.hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	ui.busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	agent_init();

	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "Bluetooth");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 400, 300);
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(ui.window), ui.area);

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scrolled), NULL);
	gtk_widget_add_controller(ui.area, scroll);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	g_signal_connect(ui.window, "map", G_CALLBACK(window_mapped), NULL);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);

	gtk_window_present(GTK_WINDOW(ui.window));
}

int main(int argc, char *argv[]) {
	font_family = g_getenv("GEMWM_FONT") ? g_getenv("GEMWM_FONT") : "monospace";
	if (argc > 1 && strcmp(argv[1], "--menu-app") == 0) {
		self = strchr(argv[0], '/') != NULL ? g_strdup(argv[0]) :
			g_find_program_in_path(argv[0]);
		return menu_app();
	}
	GtkApplication *app = gtk_application_new("org.gemwm.Bluetooth",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
