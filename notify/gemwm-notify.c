/*
 * gemwm-notify: notifications, as GEM would have shown them. GEM had no
 * notifications, but it had form_alert, the box every program used to
 * tell you something; a notification is a small one that doesn't wait for
 * you, under the menu bar at the right: a dialog's frame, a close box, the
 * alert's note icon (stop, for critical ones), and the actions as buttons.
 *
 * Three programs in one:
 *
 *   gemwm-notify             the daemon: org.freedesktop.Notifications on
 *                            the session bus (started by D-Bus when first
 *                            asked), the pop-ups, and the history
 *   gemwm-notify --menu-app  the bar's item (a menu app, see menu.c): only
 *                            there while something's unseen
 *   gemwm-notify --history   the history window (Desk > Tools >
 *                            Notifications), with Do Not Disturb
 *
 * The history is kept in ~/.local/state/gemwm/notifications. A
 * notification's seen once you've closed it or clicked it, or opened the
 * history; one that just timed out isn't. With Do Not Disturb on, only
 * critical ones pop up; the rest go quietly into the history.
 *
 * Besides the standard interface the daemon has its own,
 * org.gemwm.Notifications1, for the other two: GetState -> (unseen, dnd),
 * History -> a JSON array, MarkSeen, Clear, SetDoNotDisturb; and Changed
 * (unseen, dnd) when any of that does.
 */
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "app-menu.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define BUS_NAME "org.freedesktop.Notifications"
#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"
#define GEM_IFACE "org.gemwm.Notifications1"

#define POPUP_W 340
#define MAX_SHOWN 3            /* pop-ups at once; the rest wait */
#define DEFAULT_TIMEOUT 5000   /* ms, when the sender leaves it to us */
#define KEEP 200               /* notifications in the history */
#define SUMMARY_LINES 2
#define BODY_LINES 5

/* Why a notification closed, as the spec numbers them. */
enum { EXPIRED = 1, DISMISSED = 2, CLOSED_BY_CALL = 3 };

struct note {
	guint32 id;
	char *app, *summary, *body;
	gint64 time;             /* when it came, unix seconds */
	bool critical, seen;
	char **actions;          /* key, label, key, label... */
	int timeout;             /* ms; 0: until closed */
	char *sender;            /* who to tell about actions and closing */
	char *entry;             /* the app's desktop-entry hint, if it gave one */
	/* While it's showing: */
	GtkWidget *popup;
	GArray *hits;
	guint timer;
	int height;
};

enum { HIT_CLOSE, HIT_BODY, HIT_ACTION };

/* ---- Text ---------------------------------------------------------------- */

/* Body markup (<b>, <i>, <a href>...) as plain text: GEM's font has one
 * face, and nothing here follows links. */
static char *plain(const char *markup) {
	GString *s = g_string_new(NULL);
	for (const char *p = markup != NULL ? markup : ""; *p != '\0'; p++) {
		if (*p == '<') {
			const char *end = strchr(p, '>');
			if (end != NULL) {
				p = end;
				continue;
			}
		}
		if (*p == '&') {
			static const struct { const char *entity, *text; } entities[] = {
				{ "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
				{ "&quot;", "\"" }, { "&apos;", "'" }, { "&#39;", "'" },
			};
			bool done = false;
			for (guint i = 0; i < G_N_ELEMENTS(entities) && !done; i++) {
				size_t n = strlen(entities[i].entity);
				if (strncmp(p, entities[i].entity, n) == 0) {
					g_string_append(s, entities[i].text);
					p += n - 1;
					done = true;
				}
			}
			if (done) {
				continue;
			}
		}
		g_string_append_c(s, *p);
	}
	return g_strstrip(g_string_free(s, FALSE));
}

/* Wrapped to width, at most max lines (the last ending in "..." if cut). */
static char **lines(const char *text, int width, int max) {
	char **all = gem_wrap(text, width);
	int n = g_strv_length(all);
	if (n <= max) {
		return all;
	}
	char **out = g_new0(char *, max + 1);
	for (int i = 0; i < max; i++) {
		out[i] = i < max - 1 ? g_strdup(all[i]) :
			g_strdup_printf("%s...", all[i]);
	}
	g_strfreev(all);
	return out;
}

static char *when(gint64 unix) {
	GDateTime *t = g_date_time_new_from_unix_local(unix);
	GDateTime *now = g_date_time_new_now_local();
	bool today = g_date_time_get_year(t) == g_date_time_get_year(now) &&
		g_date_time_get_day_of_year(t) == g_date_time_get_day_of_year(now);
	char *s = g_date_time_format(t, today ? "%H:%M" : "%e %b %H:%M");
	g_date_time_unref(t);
	g_date_time_unref(now);
	return g_strstrip(s);
}

static void note_free(gpointer p) {
	struct note *n = p;
	g_free(n->app);
	g_free(n->summary);
	g_free(n->body);
	g_strfreev(n->actions);
	g_free(n->sender);
	g_free(n->entry);
	if (n->hits != NULL) {
		g_array_unref(n->hits);
	}
	g_free(n);
}

/* ---- The history, on disk ------------------------------------------------ */

static char *history_path(void) {
	return g_build_filename(g_get_user_state_dir(), "gemwm", "notifications",
		NULL);
}

static JsonNode *note_json(struct note *n) {
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "app");
	json_builder_add_string_value(b, n->app);
	json_builder_set_member_name(b, "summary");
	json_builder_add_string_value(b, n->summary);
	json_builder_set_member_name(b, "body");
	json_builder_add_string_value(b, n->body);
	json_builder_set_member_name(b, "time");
	json_builder_add_int_value(b, n->time);
	json_builder_set_member_name(b, "critical");
	json_builder_add_boolean_value(b, n->critical);
	json_builder_set_member_name(b, "seen");
	json_builder_add_boolean_value(b, n->seen);
	json_builder_end_object(b);
	JsonNode *node = json_builder_get_root(b);
	g_object_unref(b);
	return node;
}

static struct note *note_from_json(JsonObject *o) {
	struct note *n = g_new0(struct note, 1);
	n->app = g_strdup(json_object_get_string_member_with_default(o, "app", ""));
	n->summary = g_strdup(json_object_get_string_member_with_default(o,
		"summary", ""));
	n->body = g_strdup(json_object_get_string_member_with_default(o, "body", ""));
	n->time = json_object_get_int_member_with_default(o, "time", 0);
	n->critical = json_object_get_boolean_member_with_default(o, "critical",
		FALSE);
	n->seen = json_object_get_boolean_member_with_default(o, "seen", TRUE);
	return n;
}

/* Newest first (struct note). */
static GPtrArray *notes_from_json(const char *json) {
	GPtrArray *notes = g_ptr_array_new_with_free_func(note_free);
	JsonNode *root = json != NULL ? json_from_string(json, NULL) : NULL;
	JsonArray *a = root != NULL && JSON_NODE_HOLDS_ARRAY(root) ?
		json_node_get_array(root) : NULL;
	for (guint i = 0; a != NULL && i < json_array_get_length(a); i++) {
		JsonNode *e = json_array_get_element(a, i);
		if (JSON_NODE_HOLDS_OBJECT(e)) {
			g_ptr_array_add(notes, note_from_json(json_node_get_object(e)));
		}
	}
	if (root != NULL) {
		json_node_unref(root);
	}
	return notes;
}

static char *notes_to_json(GPtrArray *notes) {
	JsonArray *a = json_array_new();
	for (guint i = 0; i < notes->len; i++) {
		json_array_add_element(a, note_json(notes->pdata[i]));
	}
	JsonNode *root = json_node_init_array(json_node_alloc(), a);
	json_array_unref(a);
	char *s = json_to_string(root, FALSE);
	json_node_unref(root);
	return s;
}

/* ---- The daemon ---------------------------------------------------------- */

static const char introspection[] =
	"<node>"
	" <interface name='" IFACE "'>"
	"  <method name='GetCapabilities'><arg type='as' direction='out'/></method>"
	"  <method name='Notify'>"
	"   <arg type='s' name='app_name' direction='in'/>"
	"   <arg type='u' name='replaces_id' direction='in'/>"
	"   <arg type='s' name='app_icon' direction='in'/>"
	"   <arg type='s' name='summary' direction='in'/>"
	"   <arg type='s' name='body' direction='in'/>"
	"   <arg type='as' name='actions' direction='in'/>"
	"   <arg type='a{sv}' name='hints' direction='in'/>"
	"   <arg type='i' name='expire_timeout' direction='in'/>"
	"   <arg type='u' name='id' direction='out'/>"
	"  </method>"
	"  <method name='CloseNotification'><arg type='u' name='id' direction='in'/></method>"
	"  <method name='GetServerInformation'>"
	"   <arg type='s' name='name' direction='out'/>"
	"   <arg type='s' name='vendor' direction='out'/>"
	"   <arg type='s' name='version' direction='out'/>"
	"   <arg type='s' name='spec_version' direction='out'/>"
	"  </method>"
	"  <signal name='NotificationClosed'>"
	"   <arg type='u' name='id'/><arg type='u' name='reason'/>"
	"  </signal>"
	"  <signal name='ActionInvoked'>"
	"   <arg type='u' name='id'/><arg type='s' name='action_key'/>"
	"  </signal>"
	" </interface>"
	" <interface name='" GEM_IFACE "'>"
	"  <method name='GetState'>"
	"   <arg type='u' name='unseen' direction='out'/>"
	"   <arg type='b' name='do_not_disturb' direction='out'/>"
	"  </method>"
	"  <method name='History'><arg type='s' name='json' direction='out'/></method>"
	"  <method name='MarkSeen'/>"
	"  <method name='Clear'/>"
	"  <method name='SetDoNotDisturb'><arg type='b' name='on' direction='in'/></method>"
	"  <signal name='Changed'>"
	"   <arg type='u' name='unseen'/><arg type='b' name='do_not_disturb'/>"
	"  </signal>"
	" </interface>"
	"</node>";

static struct {
	GDBusConnection *bus;
	GPtrArray *history;      /* struct note, newest first */
	GList *shown;            /* struct note: showing, top first */
	GQueue waiting;          /* struct note: to show when there's room */
	guint32 next_id;
	bool dnd;
} d = { .next_id = 1 };

static void save(void) {
	while (d.history->len > KEEP) {
		g_ptr_array_remove_index(d.history, d.history->len - 1);
	}
	char *path = history_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	/* The history, and Do Not Disturb, which is a setting. */
	char *notes = notes_to_json(d.history);
	char *json = g_strdup_printf("{\"do_not_disturb\":%s,\"notes\":%s}",
		d.dnd ? "true" : "false", notes);
	g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT,
		0600, NULL);
	g_free(json);
	g_free(notes);
	g_free(dir);
	g_free(path);
}

static void load(void) {
	char *path = history_path(), *text = NULL;
	JsonNode *root = g_file_get_contents(path, &text, NULL, NULL) ?
		json_from_string(text, NULL) : NULL;
	d.history = g_ptr_array_new_with_free_func(note_free);
	if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
		JsonObject *o = json_node_get_object(root);
		d.dnd = json_object_get_boolean_member_with_default(o, "do_not_disturb",
			FALSE);
		JsonNode *notes = json_object_get_member(o, "notes");
		if (notes != NULL) {
			char *s = json_to_string(notes, FALSE);
			g_ptr_array_unref(d.history);
			d.history = notes_from_json(s);
			g_free(s);
		}
	}
	if (root != NULL) {
		json_node_unref(root);
	}
	g_free(text);
	g_free(path);
}

static guint unseen(void) {
	guint n = 0;
	for (guint i = 0; i < d.history->len; i++) {
		n += !((struct note *)d.history->pdata[i])->seen;
	}
	return n;
}

static void changed(void) {
	save();
	g_dbus_connection_emit_signal(d.bus, NULL, PATH, GEM_IFACE, "Changed",
		g_variant_new("(ub)", unseen(), d.dnd), NULL);
}

static void popup_show(struct note *n);
static void restack(void);

/* A pop-up gone, for why; the next waiting one, if any, takes its place. */
static void popup_close(struct note *n, guint reason) {
	if (n->timer != 0) {
		g_source_remove(n->timer);
		n->timer = 0;
	}
	if (n->popup != NULL) {
		gtk_window_destroy(GTK_WINDOW(n->popup));
		n->popup = NULL;
	}
	d.shown = g_list_remove(d.shown, n);
	g_queue_remove(&d.waiting, n);
	g_dbus_connection_emit_signal(d.bus, n->sender, PATH, IFACE,
		"NotificationClosed", g_variant_new("(uu)", n->id, reason), NULL);
	if (reason != EXPIRED && reason != CLOSED_BY_CALL && !n->seen) {
		n->seen = true; /* you closed or clicked it: you saw it */
		changed();
	}
	while (g_list_length(d.shown) < MAX_SHOWN && !g_queue_is_empty(&d.waiting)) {
		popup_show(g_queue_pop_head(&d.waiting));
	}
	restack();
}

static gboolean expired(gpointer data) {
	struct note *n = data;
	n->timer = 0;
	popup_close(n, EXPIRED);
	return G_SOURCE_REMOVE;
}

/* Laid out: how tall it is, and the text it shows. */
struct layout {
	char **summary, **body;
	int text_x, text_w, actions_y, height;
};

static void lay_out(struct note *n, struct layout *l) {
	l->text_x = GEM_BORDER + GEM_PAD + GEM_ALERT_ICON + GEM_PAD;
	l->text_w = POPUP_W - l->text_x - GEM_BORDER - GEM_PAD;
	l->summary = lines(n->summary, l->text_w, SUMMARY_LINES);
	l->body = lines(n->body, l->text_w, BODY_LINES);
	int text_h = (g_strv_length(l->summary) + g_strv_length(l->body)) * GEM_ROW_H;
	int y = GEM_BORDER + GEM_ROW_H + 2 + GEM_PAD; /* under the title row */
	y += MAX(text_h, GEM_ALERT_ICON) + GEM_PAD;
	bool actions = false;
	for (int i = 0; n->actions != NULL && n->actions[i] != NULL &&
			n->actions[i + 1] != NULL; i += 2) {
		actions |= strcmp(n->actions[i], "default") != 0;
	}
	l->actions_y = actions ? y : -1;
	if (actions) {
		y += GEM_BUTTON_H + GEM_PAD;
	}
	l->height = y + GEM_BORDER;
}

/* GEM's close box: a box with a cross, at a window's top left. */
static void close_box(cairo_t *cr, int x, int y, int size) {
	gem_black(cr);
	gem_frame(cr, x, y, size, size, 1);
	for (int i = 3; i < size - 3; i++) {
		gem_fill(cr, x + i, y + i, 1, 1);
		gem_fill(cr, x + size - 1 - i, y + i, 1, 1);
	}
}

static void paint_popup(cairo_t *cr, int w, int h, void *data) {
	struct note *n = data;
	struct layout l;
	lay_out(n, &l);
	g_array_set_size(n->hits, 0);
	gem_dialog_frame(cr, w, h);
	/* The title row: the close box and who it's from, ruled off. */
	int x = GEM_BORDER + GEM_PAD / 2, y = GEM_BORDER + 1;
	int box = GEM_ROW_H - 4;
	close_box(cr, x, y + 1, box);
	gem_hit_add(n->hits, x - 2, y - 1, box + 4, box + 4, HIT_CLOSE, 0);
	gem_text_clipped(cr, n->app[0] != '\0' ? n->app : "Notification",
		x + box + GEM_PAD, y, w - (x + box + GEM_PAD) - GEM_BORDER - GEM_PAD,
		GEM_ROW_H);
	char *t = when(n->time);
	int tw = (int)gem_text_width(cr, t);
	gem_white(cr);
	gem_fill(cr, w - GEM_BORDER - GEM_PAD - tw - 4, y, tw + 4, GEM_ROW_H);
	gem_black(cr);
	gem_text(cr, t, w - GEM_BORDER - GEM_PAD - tw, y, GEM_ROW_H);
	g_free(t);
	y += GEM_ROW_H + 1;
	gem_fill(cr, GEM_BORDER, y, w - 2 * GEM_BORDER, 1);
	y += 1 + GEM_PAD;
	/* The icon, and what it says: the summary (bold, as GEM drew it, a
	 * pixel over) and the body. */
	int body_top = y;
	gem_alert_paint_icon(cr, n->critical ? GEM_ALERT_STOP : GEM_ALERT_NOTE,
		GEM_BORDER + GEM_PAD, y);
	for (int i = 0; l.summary[i] != NULL; i++, y += GEM_ROW_H) {
		gem_text(cr, l.summary[i], l.text_x, y, GEM_ROW_H);
		gem_text(cr, l.summary[i], l.text_x + 1, y, GEM_ROW_H);
	}
	for (int i = 0; l.body[i] != NULL; i++, y += GEM_ROW_H) {
		gem_text(cr, l.body[i], l.text_x, y, GEM_ROW_H);
	}
	gem_hit_add(n->hits, GEM_BORDER, body_top, w - 2 * GEM_BORDER,
		MAX(y - body_top, GEM_ALERT_ICON), HIT_BODY, 0);
	/* The actions, as buttons from the right, the first the default. */
	if (l.actions_y >= 0) {
		int bx = w - GEM_BORDER - GEM_PAD;
		int count = 0;
		for (int i = 0; n->actions[i] != NULL && n->actions[i + 1] != NULL; i += 2) {
			count += strcmp(n->actions[i], "default") != 0;
		}
		int k = count;
		for (int i = g_strv_length(n->actions) / 2 * 2 - 2; i >= 0; i -= 2) {
			if (strcmp(n->actions[i], "default") == 0) {
				continue;
			}
			k--;
			int bw = gem_button_width(n->actions[i + 1]);
			if (bx - bw < l.text_x) {
				break; /* no room for more */
			}
			bx -= bw;
			gem_button(cr, n->hits, bx, l.actions_y, bw, n->actions[i + 1],
				HIT_ACTION, k == 0);
			/* gem_button's hit has index 0: point it at this action. */
			g_array_index(n->hits, struct gem_hit, n->hits->len - 1).index = i;
			bx -= GEM_PAD;
		}
	}
	g_strfreev(l.summary);
	g_strfreev(l.body);
}

/* Whether a window's app ID is the notification's app: its desktop entry
 * (with or without .desktop; or its last part, for reverse-DNS IDs), or
 * the name it gave, whatever the case. */
static bool same_app(const char *app_id, const char *name) {
	if (app_id == NULL || name == NULL || name[0] == '\0') {
		return false;
	}
	char *want = g_str_has_suffix(name, ".desktop") ?
		g_strndup(name, strlen(name) - strlen(".desktop")) : g_strdup(name);
	const char *dot = strrchr(app_id, '.');
	bool same = g_ascii_strcasecmp(app_id, want) == 0 ||
		(dot != NULL && g_ascii_strcasecmp(dot + 1, want) == 0);
	g_free(want);
	return same;
}

/* No default action to ask the app for: its window brought forward
 * instead, the one used last, by asking GemWM (gemwm msg). */
static void focus_app(struct note *n) {
	char *out = NULL;
	const char *argv[] = { "gemwm", "msg", "windows", NULL };
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
			G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL)) {
		return;
	}
	JsonNode *root = json_from_string(out, NULL);
	g_free(out);
	JsonObject *o = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ?
		json_node_get_object(root) : NULL;
	JsonArray *windows = o != NULL && json_object_has_member(o, "windows") ?
		json_object_get_array_member(o, "windows") : NULL;
	gint64 found = -1;
	/* The list's most recently used first: the entry first, then the name. */
	const char *names[] = { n->entry, n->app };
	for (guint k = 0; k < G_N_ELEMENTS(names) && found < 0; k++) {
		for (guint i = 0; windows != NULL && i < json_array_get_length(windows) &&
				found < 0; i++) {
			JsonObject *w = json_array_get_object_element(windows, i);
			if (same_app(json_object_get_string_member_with_default(w, "app_id",
					NULL), names[k])) {
				found = json_object_get_int_member_with_default(w, "id", -1);
			}
		}
	}
	if (root != NULL) {
		json_node_unref(root);
	}
	if (found >= 0) {
		char *id = g_strdup_printf("%" G_GINT64_FORMAT, found);
		const char *focus[] = { "gemwm", "msg", "focus-window", id, NULL };
		g_spawn_async(NULL, (char **)focus, NULL, G_SPAWN_SEARCH_PATH |
			G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
			NULL, NULL);
		g_free(id);
	}
}

static void popup_pressed(GtkGestureClick *g, int n_press, double x, double y,
		gpointer data) {
	struct note *n = data;
	const struct gem_hit *hit = gem_hit_at(n->hits, x, y);
	if (hit == NULL) {
		return;
	}
	const char *key = NULL;
	if (hit->id == HIT_ACTION) {
		key = n->actions[hit->index];
	} else if (hit->id == HIT_BODY) {
		/* The body's the default action, if it has one. */
		for (int i = 0; n->actions != NULL && n->actions[i] != NULL &&
				n->actions[i + 1] != NULL; i += 2) {
			if (strcmp(n->actions[i], "default") == 0) {
				key = "default";
			}
		}
	}
	if (key != NULL) {
		g_dbus_connection_emit_signal(d.bus, n->sender, PATH, IFACE,
			"ActionInvoked", g_variant_new("(us)", n->id, key), NULL);
	} else if (hit->id == HIT_BODY) {
		focus_app(n);
	}
	popup_close(n, DISMISSED);
}

/* Over the pointer, it waits: you're reading it. */
static void popup_entered(GtkEventControllerMotion *m, double x, double y,
		gpointer data) {
	struct note *n = data;
	if (n->timer != 0) {
		g_source_remove(n->timer);
		n->timer = 0;
	}
}

static void popup_left(GtkEventControllerMotion *m, gpointer data) {
	struct note *n = data;
	if (n->timer == 0 && n->timeout > 0) {
		n->timer = g_timeout_add(n->timeout, expired, n);
	}
}

static void draw_popup(GtkDrawingArea *a, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_popup, data);
}

/* Down the right, under the menu bar, newest at the top. */
static void restack(void) {
	int y = GEM_PAD;
	for (GList *l = d.shown; l != NULL; l = l->next) {
		struct note *n = l->data;
		gtk_layer_set_margin(GTK_WINDOW(n->popup), GTK_LAYER_SHELL_EDGE_TOP, y);
		y += n->height + GEM_PAD;
	}
}

static void popup_show(struct note *n) {
	struct layout l;
	if (n->hits == NULL) {
		n->hits = gem_hits_new();
	}
	lay_out(n, &l);
	g_strfreev(l.summary);
	g_strfreev(l.body);
	n->height = l.height;
	GtkWidget *w = gtk_window_new();
	gtk_layer_init_for_window(GTK_WINDOW(w));
	gtk_layer_set_namespace(GTK_WINDOW(w), "notification");
	gtk_layer_set_layer(GTK_WINDOW(w), GTK_LAYER_SHELL_LAYER_OVERLAY);
	gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
	gtk_layer_set_margin(GTK_WINDOW(w), GTK_LAYER_SHELL_EDGE_RIGHT, GEM_PAD);
	gtk_layer_set_keyboard_mode(GTK_WINDOW(w), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, POPUP_W, l.height);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_popup, n, NULL);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(popup_pressed), n);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "enter", G_CALLBACK(popup_entered), n);
	g_signal_connect(motion, "leave", G_CALLBACK(popup_left), n);
	gtk_widget_add_controller(area, motion);
	gtk_window_set_child(GTK_WINDOW(w), area);
	n->popup = w;
	d.shown = g_list_prepend(d.shown, n);
	restack();
	gtk_window_present(GTK_WINDOW(w));
	if (n->timeout > 0) {
		n->timer = g_timeout_add(n->timeout, expired, n);
	}
}

static struct note *find(guint32 id) {
	for (guint i = 0; i < d.history->len; i++) {
		struct note *n = d.history->pdata[i];
		if (n->id == id && id != 0) {
			return n;
		}
	}
	return NULL;
}

static guint32 notify(GVariant *params, const char *sender) {
	const char *app, *icon, *summary, *body;
	guint32 replaces;
	GVariant *actions, *hints;
	gint32 timeout;
	g_variant_get(params, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon,
		&summary, &body, &actions, &hints, &timeout);
	guchar urgency = 1;
	g_variant_lookup(hints, "urgency", "y", &urgency);
	struct note *n = find(replaces);
	if (n != NULL) {
		/* Replaced in place: its pop-up goes, a new one comes. */
		if (n->popup != NULL) {
			if (n->timer != 0) {
				g_source_remove(n->timer);
				n->timer = 0;
			}
			gtk_window_destroy(GTK_WINDOW(n->popup));
			n->popup = NULL;
			d.shown = g_list_remove(d.shown, n);
		}
		g_queue_remove(&d.waiting, n);
		g_ptr_array_remove(d.history, n); /* freed: made again below */
	}
	n = g_new0(struct note, 1);
	n->id = replaces != 0 ? replaces : d.next_id++;
	n->app = g_strdup(app);
	n->summary = plain(summary);
	n->body = plain(body);
	n->time = g_get_real_time() / G_USEC_PER_SEC;
	n->critical = urgency >= 2;
	n->actions = g_variant_dup_strv(actions, NULL);
	n->sender = g_strdup(sender);
	const char *entry = NULL;
	if (g_variant_lookup(hints, "desktop-entry", "&s", &entry) && entry[0] != '\0') {
		n->entry = g_strdup(entry);
	}
	/* Critical ones stay until closed, as the spec says. */
	n->timeout = n->critical ? 0 : timeout < 0 ? DEFAULT_TIMEOUT : timeout;
	g_variant_unref(actions);
	g_variant_unref(hints);
	g_ptr_array_insert(d.history, 0, n);
	if (d.dnd && !n->critical) {
		/* Quietly into the history; the bar's item says so. */
	} else if (g_list_length(d.shown) < MAX_SHOWN) {
		popup_show(n);
	} else {
		g_queue_push_tail(&d.waiting, n);
	}
	changed();
	return n->id;
}

static void method_call(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *call, gpointer data) {
	if (strcmp(method, "GetCapabilities") == 0) {
		const char *caps[] = { "actions", "body", "body-markup", "persistence",
			NULL };
		g_dbus_method_invocation_return_value(call,
			g_variant_new("(^as)", caps));
	} else if (strcmp(method, "GetServerInformation") == 0) {
		g_dbus_method_invocation_return_value(call, g_variant_new("(ssss)",
			"GemWM Notifications", "GemWM", "1.0", "1.2"));
	} else if (strcmp(method, "Notify") == 0) {
		g_dbus_method_invocation_return_value(call,
			g_variant_new("(u)", notify(params, sender)));
	} else if (strcmp(method, "CloseNotification") == 0) {
		guint32 id;
		g_variant_get(params, "(u)", &id);
		struct note *n = find(id);
		if (n != NULL && (n->popup != NULL ||
				g_queue_find(&d.waiting, n) != NULL)) {
			popup_close(n, CLOSED_BY_CALL);
		}
		g_dbus_method_invocation_return_value(call, NULL);
	} else if (strcmp(method, "GetState") == 0) {
		g_dbus_method_invocation_return_value(call,
			g_variant_new("(ub)", unseen(), d.dnd));
	} else if (strcmp(method, "History") == 0) {
		char *json = notes_to_json(d.history);
		g_dbus_method_invocation_return_value(call, g_variant_new("(s)", json));
		g_free(json);
	} else if (strcmp(method, "MarkSeen") == 0) {
		for (guint i = 0; i < d.history->len; i++) {
			((struct note *)d.history->pdata[i])->seen = true;
		}
		changed();
		g_dbus_method_invocation_return_value(call, NULL);
	} else if (strcmp(method, "Clear") == 0) {
		/* Not the ones showing: they're closed by themselves. */
		for (guint i = d.history->len; i-- > 0;) {
			struct note *n = d.history->pdata[i];
			if (n->popup == NULL && g_queue_find(&d.waiting, n) == NULL) {
				g_ptr_array_remove_index(d.history, i);
			}
		}
		changed();
		g_dbus_method_invocation_return_value(call, NULL);
	} else if (strcmp(method, "SetDoNotDisturb") == 0) {
		gboolean on;
		g_variant_get(params, "(b)", &on);
		d.dnd = on;
		changed();
		g_dbus_method_invocation_return_value(call, NULL);
	}
}

static void bus_acquired(GDBusConnection *bus, const char *name, gpointer data) {
	d.bus = bus;
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection, NULL);
	static const GDBusInterfaceVTable vtable = { .method_call = method_call };
	for (int i = 0; info->interfaces[i] != NULL; i++) {
		g_dbus_connection_register_object(bus, PATH, info->interfaces[i],
			&vtable, NULL, NULL, NULL);
	}
	g_dbus_node_info_unref(info);
}

static void name_lost(GDBusConnection *bus, const char *name, gpointer data) {
	g_printerr("gemwm-notify: %s is taken (another notification daemon, like "
		"mako, is running)\n", BUS_NAME);
	g_application_release(G_APPLICATION(data));
}

static void daemon_activate(GtkApplication *app, gpointer data) {
	static bool started;
	if (started) {
		return;
	}
	started = true;
	gem_ui_load_css();
	load();
	/* Nothing's showing yet, but it runs till the session ends. */
	g_application_hold(G_APPLICATION(app));
	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		bus_acquired, NULL, name_lost, app, NULL);
}

/* ---- The bar's item ------------------------------------------------------ */

/* A bell, 13x12, for the bar. */
static const char *const bell[] = {
	"......#......",
	"....#####....",
	"...#.....#...",
	"..#.......#..",
	"..#.......#..",
	"..#.......#..",
	"..#.......#..",
	".#.........#.",
	"#...........#",
	"#############",
	".....###.....",
	"......#......",
};

static char *bell_spec(void) {
	int w = strlen(bell[0]), h = G_N_ELEMENTS(bell);
	GString *s = g_string_new(NULL);
	g_string_append_printf(s, "bitmap:%dx%d:", w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x += 8) {
			unsigned byte = 0;
			for (int bit = 0; bit < 8 && x + bit < w; bit++) {
				if (bell[y][x + bit] == '#') {
					byte |= 0x80 >> bit;
				}
			}
			g_string_append_printf(s, "%02x", byte);
		}
	}
	return g_string_free(s, FALSE);
}

static struct {
	GMainLoop *loop;
	GDBusConnection *bus;
	char *bell;
} m;

/* Only there while something's unseen. */
static void show_item(guint unseen, gboolean dnd) {
	if (unseen == 0) {
		printf("\n");
	} else {
		printf("%s\t%u\t\t%u new notification%s%s\n", m.bell, unseen, unseen,
			unseen == 1 ? "" : "s", dnd ? " (Do Not Disturb is on)" : "");
	}
	fflush(stdout);
}

static void got_state(GObject *src, GAsyncResult *res, gpointer data) {
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res,
		NULL);
	if (r != NULL) {
		guint unseen;
		gboolean dnd;
		g_variant_get(r, "(ub)", &unseen, &dnd);
		show_item(unseen, dnd);
		g_variant_unref(r);
	}
}

static void ask_state(void) {
	/* Asking starts the daemon, if D-Bus hasn't already. */
	g_dbus_connection_call(m.bus, BUS_NAME, PATH, GEM_IFACE, "GetState", NULL,
		G_VARIANT_TYPE("(ub)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, got_state, NULL);
}

static void state_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	guint unseen;
	gboolean dnd;
	g_variant_get(params, "(ub)", &unseen, &dnd);
	show_item(unseen, dnd);
}

/* The daemon came back (it was restarted): what it says now. */
static void owner_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	const char *name, *old, *new;
	g_variant_get(params, "(&s&s&s)", &name, &old, &new);
	if (strcmp(name, BUS_NAME) == 0 && new[0] != '\0') {
		ask_state();
	}
}

static gboolean bar_said(GIOChannel *ch, GIOCondition cond, gpointer data) {
	char *line = NULL;
	GIOStatus st = g_io_channel_read_line(ch, &line, NULL, NULL, NULL);
	if (st == G_IO_STATUS_EOF || st == G_IO_STATUS_ERROR) {
		g_main_loop_quit(m.loop); /* the bar's gone */
		return G_SOURCE_REMOVE;
	}
	if (line != NULL && g_str_has_prefix(line, "click")) {
		g_spawn_command_line_async("gemwm-notify --history", NULL);
	}
	g_free(line);
	return G_SOURCE_CONTINUE;
}

static int menu_app(void) {
	m.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (m.bus == NULL) {
		return 1;
	}
	m.bell = bell_spec();
	m.loop = g_main_loop_new(NULL, FALSE);
	g_dbus_connection_signal_subscribe(m.bus, NULL, GEM_IFACE, "Changed", PATH,
		NULL, G_DBUS_SIGNAL_FLAGS_NONE, state_changed, NULL, NULL);
	g_dbus_connection_signal_subscribe(m.bus, "org.freedesktop.DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus",
		BUS_NAME, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, NULL, NULL);
	GIOChannel *in = g_io_channel_unix_new(0);
	g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, bar_said, NULL);
	show_item(0, FALSE);
	ask_state();
	g_main_loop_run(m.loop);
	return 0;
}

/* ---- The history window -------------------------------------------------- */

enum { ACT_CLEAR = 1, ACT_CLOSE, ACT_DND };

#define ENTRY_PAD 4

static struct {
	GtkApplication *app;
	GtkWidget *window, *area, *bar;
	GtkAdjustment *adj;
	struct app_menu *menu;
	GDBusConnection *bus;
	GPtrArray *notes;        /* newest first */
	bool dnd;
} h;

static int entry_height(struct note *n, int w) {
	char **body = lines(n->body, w - 2 * GEM_PAD, 3);
	int rows = 1 + (n->summary[0] != '\0') + g_strv_length(body);
	g_strfreev(body);
	return rows * GEM_ROW_H + 2 * ENTRY_PAD + 1;
}

static int content_height(int w) {
	int total = 0;
	for (guint i = 0; h.notes != NULL && i < h.notes->len; i++) {
		total += entry_height(h.notes->pdata[i], w);
	}
	return total;
}

static gboolean fit_bar(gpointer data) {
	int w = gtk_widget_get_width(h.area), ht = gtk_widget_get_height(h.area);
	int total = content_height(w);
	gtk_adjustment_configure(h.adj,
		CLAMP(gtk_adjustment_get_value(h.adj), 0, MAX(total - ht, 0)), 0,
		MAX(total, ht), GEM_ROW_H, MAX(ht - GEM_ROW_H, GEM_ROW_H), ht);
	return G_SOURCE_REMOVE;
}

static void paint_history(cairo_t *cr, int w, int ht, void *data) {
	if ((int)gtk_adjustment_get_page_size(h.adj) != ht ||
			(int)gtk_adjustment_get_upper(h.adj) != MAX(content_height(w), ht)) {
		g_idle_add(fit_bar, NULL);
	}
	gem_black(cr);
	if (h.notes == NULL || h.notes->len == 0) {
		const char *none = h.dnd ? "No notifications. Do Not Disturb is on." :
			"No notifications.";
		gem_text(cr, none, MAX(GEM_PAD, (w - (int)gem_text_width(cr, none)) / 2),
			ht / 3, GEM_ROW_H);
		return;
	}
	int y = -(int)gtk_adjustment_get_value(h.adj);
	for (guint i = 0; i < h.notes->len; i++) {
		struct note *n = h.notes->pdata[i];
		int eh = entry_height(n, w);
		if (y + eh >= 0 && y <= ht) {
			int ty = y + ENTRY_PAD;
			/* Who and when, the unseen marked with GEM's diamond. */
			char *t = when(n->time);
			int tw = (int)gem_text_width(cr, t);
			int ax = GEM_PAD;
			if (!n->seen) {
				int cy = ty + GEM_ROW_H / 2;
				cairo_move_to(cr, ax + 4, cy - 4);
				cairo_line_to(cr, ax + 8, cy);
				cairo_line_to(cr, ax + 4, cy + 4);
				cairo_line_to(cr, ax, cy);
				cairo_close_path(cr);
				cairo_fill(cr);
				ax += 14;
			}
			gem_text_clipped(cr, n->app[0] != '\0' ? n->app : "Notification", ax,
				ty, w - ax - tw - 2 * GEM_PAD, GEM_ROW_H);
			gem_text(cr, t, w - GEM_PAD - tw, ty, GEM_ROW_H);
			g_free(t);
			ty += GEM_ROW_H;
			if (n->summary[0] != '\0') {
				gem_text_clipped(cr, n->summary, GEM_PAD, ty, w - 2 * GEM_PAD,
					GEM_ROW_H);
				gem_text_clipped(cr, n->summary, GEM_PAD + 1, ty, w - 2 * GEM_PAD - 1,
					GEM_ROW_H);
				ty += GEM_ROW_H;
			}
			char **body = lines(n->body, w - 2 * GEM_PAD, 3);
			for (int k = 0; body[k] != NULL; k++, ty += GEM_ROW_H) {
				gem_text(cr, body[k], GEM_PAD, ty, GEM_ROW_H);
			}
			g_strfreev(body);
			/* A dotted rule between them. */
			for (int x = GEM_PAD; x < w - GEM_PAD; x += 2) {
				gem_fill(cr, x, y + eh - 1, 1, 1);
			}
		}
		y += eh;
	}
}

static void draw_history(GtkDrawingArea *a, cairo_t *cr, int w, int ht,
		gpointer data) {
	gem_draw_pixelated(cr, w, ht, paint_history, NULL);
}

static void got_history(GObject *src, GAsyncResult *res, gpointer data) {
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res,
		NULL);
	if (r == NULL) {
		return;
	}
	const char *json;
	g_variant_get(r, "(&s)", &json);
	g_clear_pointer(&h.notes, g_ptr_array_unref);
	h.notes = notes_from_json(json);
	g_variant_unref(r);
	g_idle_add(fit_bar, NULL);
	gtk_widget_queue_draw(h.area);
	char *title = g_strdup_printf("Notifications: %u", h.notes->len);
	gtk_window_set_title(GTK_WINDOW(h.window), title);
	g_free(title);
}

static void refresh(void) {
	g_dbus_connection_call(h.bus, BUS_NAME, PATH, GEM_IFACE, "History", NULL,
		G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, got_history,
		NULL);
}

static void call(const char *method, GVariant *args) {
	g_dbus_connection_call(h.bus, BUS_NAME, PATH, GEM_IFACE, method, args, NULL,
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Something new while it's open: shown, and seen, since you're looking. */
static void history_changed(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	guint unseen;
	gboolean dnd;
	g_variant_get(params, "(ub)", &unseen, &dnd);
	if (dnd != h.dnd) {
		h.dnd = dnd;
		app_menu_update(h.menu);
	}
	refresh();
	if (unseen > 0 && gtk_window_is_active(GTK_WINDOW(h.window))) {
		call("MarkSeen", NULL);
	}
}

static void got_dnd(GObject *src, GAsyncResult *res, gpointer data) {
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res,
		NULL);
	if (r != NULL) {
		guint unseen;
		gboolean dnd;
		g_variant_get(r, "(ub)", &unseen, &dnd);
		h.dnd = dnd;
		app_menu_update(h.menu);
		g_variant_unref(r);
	}
}

static void build_menus(struct app_menu *menu, void *data) {
	bool any = h.notes != NULL && h.notes->len > 0;
	app_menu_add_menu(menu, "File");
	app_menu_add_item(menu, ACT_CLEAR, "Clear All", NULL,
		any ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(menu);
	app_menu_add_item(menu, ACT_CLOSE, "Close", "^Q", 0);
	app_menu_add_menu(menu, "Options");
	app_menu_add_item(menu, ACT_DND, "Do Not Disturb", NULL,
		h.dnd ? APP_MENU_CHECKED : 0);
}

static void menu_activate(uint32_t id, void *data) {
	switch (id) {
	case ACT_CLEAR:
		call("Clear", NULL);
		break;
	case ACT_CLOSE:
		gtk_window_destroy(GTK_WINDOW(h.window));
		break;
	case ACT_DND:
		call("SetDoNotDisturb", g_variant_new("(b)", !h.dnd));
		break;
	}
}

static gboolean history_scrolled(GtkEventControllerScroll *c, double dx,
		double dy, gpointer data) {
	gtk_adjustment_set_value(h.adj, gtk_adjustment_get_value(h.adj) +
		dy * 3 * GEM_ROW_H);
	return TRUE;
}

static void history_activate(GtkApplication *app, gpointer data) {
	if (h.window != NULL) {
		gtk_window_present(GTK_WINDOW(h.window));
		call("MarkSeen", NULL);
		return;
	}
	gem_ui_load_css();
	h.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	h.window = gtk_application_window_new(app);
	gtk_widget_add_css_class(h.window, "gem");
	gtk_window_set_title(GTK_WINDOW(h.window), "Notifications");
	gtk_window_set_default_size(GTK_WINDOW(h.window), 420, 480);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	h.area = gtk_drawing_area_new();
	gtk_widget_set_hexpand(h.area, TRUE);
	gtk_widget_set_vexpand(h.area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(h.area), draw_history, NULL,
		NULL);
	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(history_scrolled), NULL);
	gtk_widget_add_controller(h.area, wheel);
	h.adj = gtk_adjustment_new(0, 0, 0, GEM_ROW_H, GEM_ROW_H, 0);
	g_signal_connect_swapped(h.adj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), h.area);
	h.bar = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, h.adj);
	gtk_box_append(GTK_BOX(box), h.area);
	gtk_box_append(GTK_BOX(box), h.bar);
	gtk_window_set_child(GTK_WINDOW(h.window), box);
	h.menu = app_menu_new(h.window, build_menus, menu_activate, NULL);
	GtkEventController *keys = gtk_shortcut_controller_new();
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>q"),
			gtk_named_action_new("window.close")));
	gtk_widget_add_controller(h.window, keys);
	if (h.bus != NULL) {
		g_dbus_connection_signal_subscribe(h.bus, NULL, GEM_IFACE, "Changed",
			PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, history_changed, NULL, NULL);
		g_dbus_connection_call(h.bus, BUS_NAME, PATH, GEM_IFACE, "GetState",
			NULL, G_VARIANT_TYPE("(ub)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			got_dnd, NULL);
		refresh();
		call("MarkSeen", NULL); /* you're looking: the bar's item goes */
	}
	gtk_window_present(GTK_WINDOW(h.window));
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "--menu-app") == 0) {
		return menu_app();
	}
	if (argc > 1 && strcmp(argv[1], "--history") == 0) {
		h.app = gtk_application_new("org.gemwm.Notifications",
			G_APPLICATION_DEFAULT_FLAGS);
		g_signal_connect(h.app, "activate", G_CALLBACK(history_activate), NULL);
		int status = g_application_run(G_APPLICATION(h.app), 1, argv);
		g_object_unref(h.app);
		return status;
	}
	GtkApplication *app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(daemon_activate), NULL);
	int status = g_application_run(G_APPLICATION(app), 1, argv);
	g_object_unref(app);
	return status;
}
