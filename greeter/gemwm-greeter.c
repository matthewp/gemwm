/*
 * gemwm-greeter: GemWM's login screen, for greetd. GEM never had one (TOS
 * started straight into the desktop), so it's the desktop before anyone's
 * on it: the menu bar with only Desk, and the people who can log in as
 * icons on the desktop, as the ST showed its disk drives. Open one (or
 * type a name) and a GEM dialog asks for the password.
 *
 * It runs inside GemWM's greeter mode (gemwm -G gemwm-greeter), which
 * gives its window the whole desktop, frameless, under the menu bar. The
 * window is transparent but for the icons and the dialog, so the desktop
 * shows through. greetd does the logging in (PAM, the session); we only
 * talk to it, over GREETD_SOCK (greetd-ipc(7)), and when it's started a
 * session we're done, and so is GemWM, and greetd starts it.
 *
 * Who logged in last, and with which session, is kept in
 * /var/cache/gemwm-greeter (made by gemwm-greeter.tmpfiles), if it's
 * there to keep it in.
 */
#define _DEFAULT_SOURCE /* getpwent */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <pwd.h>
#include <string.h>
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-popup.h"
#include "gem-ui.h"

#define STATE_DIR "/var/cache/gemwm-greeter"
#define ICON_W 32        /* a person, as GEM's 32x32 icons were */
#define ICON_H 30
#define SLOT_W 96        /* an icon and its label on the desktop */
#define SLOT_H 64
#define DIALOG_W 420
#define LABEL_W 96       /* the fields' labels, in the dialog */
#define FIELD_W (DIALOG_W - 2 * 16 - LABEL_W)

enum { HIT_USER, HIT_SESSION, HIT_OK, HIT_SHUTDOWN };

struct user {
	char *name, *full;
};

struct session {
	char *name, *exec, *desktop;
};

/* Where the conversation with greetd is. */
enum step {
	IDLE,        /* nothing asked */
	ASKING,      /* a request out: waiting on greetd (and PAM) */
	PROMPTING,   /* PAM asked something more than the password */
	STARTING,    /* start_session sent */
};

static struct {
	GtkApplication *app;
	GtkWidget *window, *area, *user_field, *password_field;
	GtkOverlay *host;
	struct gem_popup *sessions_popup;
	GArray *hits;
	GPtrArray *users;          /* struct user */
	GPtrArray *sessions;       /* struct session */
	int selected;              /* a user's icon, or -1 */
	int session;
	char *prompt;              /* the second field's label */
	char *note;                /* PAM's info, under the fields */
	enum step step;
	bool password_sent;        /* the first secret prompt's answered */
	bool cancelling;           /* the request out is a cancel_session */
	GSocketConnection *greetd;
	int dialog_x, dialog_y, dialog_h;
} g = { .selected = -1 };

/* ---- Who and what ------------------------------------------------------ */

/* UID_MIN and UID_MAX, from login.defs: who's a person, not a service. */
static void uid_range(uid_t *lo, uid_t *hi) {
	*lo = 1000;
	*hi = 60000;
	char *text = NULL;
	if (!g_file_get_contents("/etc/login.defs", &text, NULL, NULL)) {
		return;
	}
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i] != NULL; i++) {
		unsigned v;
		if (sscanf(lines[i], " UID_MIN %u", &v) == 1) {
			*lo = v;
		} else if (sscanf(lines[i], " UID_MAX %u", &v) == 1) {
			*hi = v;
		}
	}
	g_strfreev(lines);
	g_free(text);
}

static void load_users(void) {
	g.users = g_ptr_array_new();
	uid_t lo, hi;
	uid_range(&lo, &hi);
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent()) != NULL) {
		const char *shell = pw->pw_shell != NULL ? pw->pw_shell : "";
		if (pw->pw_uid < lo || pw->pw_uid > hi ||
				g_str_has_suffix(shell, "nologin") ||
				g_str_has_suffix(shell, "false")) {
			continue;
		}
		struct user *u = g_new0(struct user, 1);
		u->name = g_strdup(pw->pw_name);
		/* The GECOS field's first part is the full name. */
		char *full = g_strdup(pw->pw_gecos != NULL ? pw->pw_gecos : "");
		char *comma = strchr(full, ',');
		if (comma != NULL) {
			*comma = '\0';
		}
		u->full = g_strstrip(full);
		g_ptr_array_add(g.users, u);
	}
	endpwent();
}

/* The sessions installed, by their .desktop files. */
static void load_sessions(void) {
	g.sessions = g_ptr_array_new();
	const char *dirs[] = { "/usr/local/share/wayland-sessions",
		"/usr/share/wayland-sessions" };
	for (guint d = 0; d < G_N_ELEMENTS(dirs); d++) {
		GDir *dir = g_dir_open(dirs[d], 0, NULL);
		const char *file;
		while (dir != NULL && (file = g_dir_read_name(dir)) != NULL) {
			if (!g_str_has_suffix(file, ".desktop")) {
				continue;
			}
			char *path = g_build_filename(dirs[d], file, NULL);
			GKeyFile *kf = g_key_file_new();
			const char *grp = G_KEY_FILE_DESKTOP_GROUP;
			if (g_key_file_load_from_file(kf, path, 0, NULL) &&
					!g_key_file_get_boolean(kf, grp, "Hidden", NULL) &&
					!g_key_file_get_boolean(kf, grp, "NoDisplay", NULL)) {
				char *name = g_key_file_get_locale_string(kf, grp, "Name", NULL, NULL);
				char *exec = g_key_file_get_string(kf, grp, "Exec", NULL);
				bool seen = false;
				for (guint i = 0; name != NULL && i < g.sessions->len; i++) {
					seen |= strcmp(((struct session *)g.sessions->pdata[i])->name,
						name) == 0;
				}
				if (name != NULL && exec != NULL && !seen) {
					struct session *s = g_new0(struct session, 1);
					s->name = name;
					s->exec = exec;
					s->desktop = g_key_file_get_string(kf, grp, "DesktopNames", NULL);
					if (s->desktop != NULL) {
						g_strdelimit(s->desktop, ";", ':');
						if (g_str_has_suffix(s->desktop, ":")) {
							s->desktop[strlen(s->desktop) - 1] = '\0';
						}
					}
					g_ptr_array_add(g.sessions, s);
				} else {
					g_free(name);
					g_free(exec);
				}
			}
			g_key_file_free(kf);
			g_free(path);
		}
		if (dir != NULL) {
			g_dir_close(dir);
		}
	}
	/* GemWM first, of course. */
	for (guint i = 0; i < g.sessions->len; i++) {
		if (strcmp(((struct session *)g.sessions->pdata[i])->name, "GemWM") == 0) {
			gpointer s = g.sessions->pdata[i];
			g.sessions->pdata[i] = g.sessions->pdata[0];
			g.sessions->pdata[0] = s;
		}
	}
}

static const char *session_name(void) {
	return g.session < (int)g.sessions->len ?
		((struct session *)g.sessions->pdata[g.session])->name : "(none)";
}

/* ---- Remembering ----------------------------------------------------- */

static void load_last(void) {
	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, STATE_DIR "/state", 0, NULL)) {
		char *user = g_key_file_get_string(kf, "last", "user", NULL);
		char *session = g_key_file_get_string(kf, "last", "session", NULL);
		for (guint i = 0; user != NULL && i < g.users->len; i++) {
			if (strcmp(((struct user *)g.users->pdata[i])->name, user) == 0) {
				g.selected = i;
			}
		}
		for (guint i = 0; session != NULL && i < g.sessions->len; i++) {
			if (strcmp(((struct session *)g.sessions->pdata[i])->name, session) == 0) {
				g.session = i;
			}
		}
		g_free(user);
		g_free(session);
	}
	g_key_file_free(kf);
	if (g.selected < 0 && g.users->len == 1) {
		g.selected = 0; /* only you */
	}
}

static void save_last(const char *user) {
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_string(kf, "last", "user", user);
	g_key_file_set_string(kf, "last", "session", session_name());
	/* Not there (not installed with its tmpfiles): not remembered. */
	g_key_file_save_to_file(kf, STATE_DIR "/state", NULL);
	g_key_file_free(kf);
}

/* ---- Talking to greetd ------------------------------------------------- */

/* A request and its answer, one at a time, off the main thread: PAM can
 * take seconds (a wrong password's delay). */
static JsonNode *greetd_call(GSocketConnection *c, const char *json,
		GError **error) {
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c));
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(c));
	guint32 len = strlen(json);
	if (!g_output_stream_write_all(out, &len, sizeof len, NULL, NULL, error) ||
			!g_output_stream_write_all(out, json, len, NULL, NULL, error) ||
			!g_input_stream_read_all(in, &len, sizeof len, NULL, NULL, error)) {
		return NULL;
	}
	if (len > 1024 * 1024) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"greetd's answer is too big");
		return NULL;
	}
	char *text = g_malloc(len + 1);
	gsize got = 0;
	if (!g_input_stream_read_all(in, text, len, &got, NULL, error) || got != len) {
		g_free(text);
		if (error != NULL && *error == NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
				"greetd went away");
		}
		return NULL;
	}
	text[len] = '\0';
	JsonNode *node = json_from_string(text, error);
	g_free(text);
	return node;
}

static void call_thread(GTask *task, gpointer source, gpointer data,
		GCancellable *cancel) {
	GError *error = NULL;
	JsonNode *node = greetd_call(g.greetd, data, &error);
	if (node == NULL) {
		g_task_return_error(task, error);
	} else {
		g_task_return_pointer(task, node, (GDestroyNotify)json_node_unref);
	}
}

static void answered(GObject *source, GAsyncResult *res, gpointer data);

/* Sends a request built by b (an object, without its end). */
static void send(JsonBuilder *b) {
	json_builder_end_object(b);
	JsonNode *root = json_builder_get_root(b);
	char *json = json_to_string(root, FALSE);
	json_node_unref(root);
	g_object_unref(b);
	GTask *task = g_task_new(NULL, NULL, answered, NULL);
	g_task_set_task_data(task, json, g_free);
	g_task_run_in_thread(task, call_thread);
	g_object_unref(task);
}

static JsonBuilder *request(const char *type) {
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, type);
	return b;
}

/* ---- The conversation ------------------------------------------------ */

static void redraw(void) {
	gtk_widget_queue_draw(g.area);
}

/* Waiting (the busy bee, as GEM's mouse became one), or not. */
static void set_step(enum step step) {
	g.step = step;
	bool busy = step == ASKING || step == STARTING;
	gtk_widget_set_cursor_from_name(g.window, busy ? "wait" : NULL);
	gtk_widget_set_sensitive(g.user_field, step == IDLE);
	gtk_widget_set_sensitive(g.password_field, !busy);
	redraw();
}

static void reset_prompt(void) {
	g_free(g.prompt);
	g.prompt = g_strdup("Password:");
	g_clear_pointer(&g.note, g_free);
	gtk_entry_set_visibility(GTK_ENTRY(g.password_field), FALSE);
}

static void cancel(void) {
	if (g.greetd != NULL) {
		g.cancelling = true;
		set_step(ASKING);
		send(request("cancel_session"));
	}
}

static void alert_done(int button, void *data) {
	gtk_editable_set_text(GTK_EDITABLE(g.password_field), "");
	gtk_widget_grab_focus(g.step == IDLE && g.selected < 0 ?
		g.user_field : g.password_field);
}

static void alert(enum gem_alert_icon icon, const char *text) {
	static const char *const ok[] = { "Try Again", NULL };
	gem_alert(g.host, icon, text, ok, 0, 0, alert_done, NULL);
}

static void start_session(void) {
	struct session *s = g.session < (int)g.sessions->len ?
		g.sessions->pdata[g.session] : NULL;
	if (s == NULL) {
		alert(GEM_ALERT_STOP, "There's no session to start.");
		cancel();
		return;
	}
	/* Its Exec, without field codes (%f and the like: there are no files). */
	GString *exec = g_string_new(NULL);
	for (const char *p = s->exec; *p != '\0'; p++) {
		if (*p == '%' && p[1] != '\0') {
			p++;
			continue;
		}
		g_string_append_c(exec, *p);
	}
	JsonBuilder *b = request("start_session");
	json_builder_set_member_name(b, "cmd");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "/bin/sh");
	json_builder_add_string_value(b, "-c");
	json_builder_add_string_value(b, exec->str);
	json_builder_end_array(b);
	json_builder_set_member_name(b, "env");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "XDG_SESSION_TYPE=wayland");
	if (s->desktop != NULL) {
		char *v = g_strdup_printf("XDG_CURRENT_DESKTOP=%s", s->desktop);
		json_builder_add_string_value(b, v);
		g_free(v);
	}
	json_builder_end_array(b);
	g_string_free(exec, TRUE);
	set_step(STARTING);
	send(b);
}

static void respond(const char *text) {
	JsonBuilder *b = request("post_auth_message_response");
	if (text != NULL) {
		json_builder_set_member_name(b, "response");
		json_builder_add_string_value(b, text);
	}
	set_step(ASKING);
	send(b);
}

static void answered(GObject *source, GAsyncResult *res, gpointer data) {
	GError *error = NULL;
	JsonNode *node = g_task_propagate_pointer(G_TASK(res), &error);
	if (node == NULL) {
		set_step(IDLE);
		char *text = g_strdup_printf("Couldn't reach greetd:\n%s", error->message);
		alert(GEM_ALERT_STOP, text);
		g_free(text);
		g_error_free(error);
		return;
	}
	JsonObject *o = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
	const char *type = o != NULL ?
		json_object_get_string_member_with_default(o, "type", "") : "";
	bool cancelled = g.cancelling;
	g.cancelling = false;
	if (cancelled) {
		set_step(IDLE); /* whatever it says, there's no session now */
		reset_prompt();
	} else if (strcmp(type, "success") == 0) {
		if (g.step == STARTING) {
			/* greetd starts it once we're gone: so are we, and GemWM. */
			save_last(gtk_editable_get_text(GTK_EDITABLE(g.user_field)));
			gtk_window_destroy(GTK_WINDOW(g.window));
		} else {
			start_session(); /* logged in */
		}
	} else if (strcmp(type, "auth_message") == 0) {
		const char *kind = json_object_get_string_member_with_default(o,
			"auth_message_type", "");
		const char *text = json_object_get_string_member_with_default(o,
			"auth_message", "");
		bool secret = strcmp(kind, "secret") == 0;
		if (secret && !g.password_sent) {
			/* The password, typed already. */
			g.password_sent = true;
			char *password = g_strdup(gtk_editable_get_text(
				GTK_EDITABLE(g.password_field)));
			respond(password);
			memset(password, 0, strlen(password));
			g_free(password);
		} else if (secret || strcmp(kind, "visible") == 0) {
			/* Something more (a code from a key, say): asked in the
			 * second field, under PAM's own words. */
			g_free(g.prompt);
			g.prompt = g_strdup(text);
			gtk_entry_set_visibility(GTK_ENTRY(g.password_field), !secret);
			gtk_editable_set_text(GTK_EDITABLE(g.password_field), "");
			set_step(PROMPTING);
			gtk_widget_grab_focus(g.password_field);
		} else {
			/* info or error: shown, and nothing to answer. */
			g_free(g.note);
			g.note = g_strdup(text);
			respond(NULL);
		}
	} else if (strcmp(type, "error") == 0) {
		bool auth = g_strcmp0(json_object_get_string_member_with_default(o,
			"error_type", ""), "auth_error") == 0;
		const char *why = json_object_get_string_member_with_default(o,
			"description", "");
		set_step(IDLE);
		reset_prompt();
		g.password_sent = false;
		if (auth) {
			alert(GEM_ALERT_STOP, "That password isn't right.");
		} else {
			char *text = g_strdup_printf("Couldn't log in:\n%s", why);
			alert(GEM_ALERT_STOP, text);
			g_free(text);
		}
		cancel(); /* greetd wants a fresh start */
	}
	json_node_unref(node);
}

static void log_in(void) {
	if (g.step == PROMPTING) {
		respond(gtk_editable_get_text(GTK_EDITABLE(g.password_field)));
		return;
	}
	if (g.step != IDLE) {
		return;
	}
	const char *user = gtk_editable_get_text(GTK_EDITABLE(g.user_field));
	if (user[0] == '\0') {
		gtk_widget_grab_focus(g.user_field);
		return;
	}
	if (g.greetd == NULL) {
		const char *path = g_getenv("GREETD_SOCK");
		GError *error = NULL;
		GSocketClient *client = g_socket_client_new();
		GSocketAddress *addr = path != NULL ?
			g_unix_socket_address_new(path) : NULL;
		g.greetd = addr != NULL ? g_socket_client_connect(client,
			G_SOCKET_CONNECTABLE(addr), NULL, &error) : NULL;
		g_clear_object(&addr);
		g_object_unref(client);
		if (g.greetd == NULL) {
			char *text = g_strdup_printf("Couldn't reach greetd:\n%s",
				error != NULL ? error->message : "GREETD_SOCK isn't set");
			alert(GEM_ALERT_STOP, text);
			g_free(text);
			g_clear_error(&error);
			return;
		}
	}
	reset_prompt();
	g.password_sent = false;
	JsonBuilder *b = request("create_session");
	json_builder_set_member_name(b, "username");
	json_builder_add_string_value(b, user);
	set_step(ASKING);
	send(b);
}

/* ---- Drawing ----------------------------------------------------------- */

/* A person, head and shoulders: white with a black edge, so it shows on
 * any desktop; black when selected, as GEM inverted its icons. */
static void paint_person(cairo_t *cr, int x, int y, bool on) {
	cairo_save(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_set_line_width(cr, 1);
	/* Shoulders: half an ellipse. */
	cairo_save(cr);
	cairo_translate(cr, x + ICON_W / 2.0, y + ICON_H);
	cairo_scale(cr, 14, 12);
	cairo_arc(cr, 0, 0, 1, G_PI, 2 * G_PI);
	cairo_restore(cr);
	cairo_close_path(cr);
	on ? gem_black(cr) : gem_white(cr);
	cairo_fill_preserve(cr);
	gem_black(cr);
	cairo_stroke(cr);
	/* Head. */
	cairo_arc(cr, x + ICON_W / 2.0, y + 9.5, 8, 0, 2 * G_PI);
	on ? gem_black(cr) : gem_white(cr);
	cairo_fill_preserve(cr);
	on ? gem_white(cr) : gem_black(cr);
	cairo_stroke(cr);
	cairo_restore(cr);
}

static void paint_users(cairo_t *cr) {
	for (guint i = 0; i < g.users->len; i++) {
		struct user *u = g.users->pdata[i];
		bool on = (int)i == g.selected;
		int x = 16, y = 16 + i * (SLOT_H + 8);
		paint_person(cr, x + (SLOT_W - ICON_W) / 2, y, on);
		/* The label, on white (black when selected), as on GEM's desktop. */
		int lw = MIN((int)gem_text_width(cr, u->name) + 6, SLOT_W);
		int lx = x + (SLOT_W - lw) / 2, ly = y + ICON_H + 4;
		on ? gem_black(cr) : gem_white(cr);
		gem_fill(cr, lx, ly, lw, GEM_ROW_H);
		on ? gem_white(cr) : gem_black(cr);
		gem_text_clipped(cr, u->name, lx + 3, ly, lw - 6, GEM_ROW_H);
		gem_hit_add(g.hits, x, y, SLOT_W, SLOT_H, HIT_USER, i);
	}
}

/* A GEM pop-up button: a box with a drop shadow, as GEM drew them. */
static void popup_button(cairo_t *cr, int x, int y, int w, const char *label) {
	gem_white(cr);
	gem_fill(cr, x, y, w, GEM_FIELD_H);
	gem_black(cr);
	gem_frame(cr, x, y, w, GEM_FIELD_H, 1);
	gem_fill(cr, x + 2, y + GEM_FIELD_H, w, 2);
	gem_fill(cr, x + w, y + 2, 2, GEM_FIELD_H);
	gem_text_clipped(cr, label, x + GEM_PAD, y + (GEM_FIELD_H - GEM_ROW_H) / 2,
		w - 2 * GEM_PAD, GEM_ROW_H);
	gem_hit_add(g.hits, x, y, w + 2, GEM_FIELD_H + 2, HIT_SESSION, 0);
}

static int dialog_height(void) {
	return GEM_PAD * 2 + GEM_ROW_H * 2 + GEM_PAD + 3 * (GEM_FIELD_H + GEM_PAD) +
		GEM_ROW_H + GEM_PAD + GEM_BUTTON_H + GEM_PAD * 2;
}

/* Where the dialog goes, in the middle, and its fields in it: on a resize
 * (not while drawing, when GTK can't move widgets). */
static void place(GtkDrawingArea *area, int w, int h, gpointer data) {
	g.dialog_h = dialog_height();
	g.dialog_x = (w - DIALOG_W) / 2;
	g.dialog_y = MAX(16, (h - g.dialog_h) / 2);
	int fx = g.dialog_x + 16 + LABEL_W;
	int fy = g.dialog_y + GEM_PAD * 2 + GEM_ROW_H * 2 + GEM_PAD;
	gtk_widget_set_margin_start(g.user_field, fx);
	gtk_widget_set_margin_top(g.user_field, fy);
	gtk_widget_set_margin_start(g.password_field, fx);
	gtk_widget_set_margin_top(g.password_field, fy + GEM_FIELD_H + GEM_PAD);
}

static void paint_dialog(cairo_t *cr, int w, int h) {
	int dw = DIALOG_W, dh = g.dialog_h;
	int dx = g.dialog_x, dy = g.dialog_y;
	cairo_save(cr);
	cairo_translate(cr, dx, dy);
	gem_dialog_frame(cr, dw, dh);
	int y = GEM_PAD * 2;
	const char *title = "Welcome to GemWM";
	int tw = (int)gem_text_width(cr, title);
	gem_text(cr, title, (dw - tw) / 2, y, GEM_ROW_H);
	gem_text(cr, title, (dw - tw) / 2 + 1, y, GEM_ROW_H); /* bold */
	y += GEM_ROW_H;
	/* PAM's question, when it's asked more than the password and it
	 * doesn't fit as the field's label; else whose this is. */
	bool long_prompt = gem_text_width(cr, g.prompt) > LABEL_W - 4;
	const char *sub = long_prompt ? g.prompt : g.selected >= 0 &&
		((struct user *)g.users->pdata[g.selected])->full[0] != '\0' ?
		((struct user *)g.users->pdata[g.selected])->full : "Log in to begin.";
	int sw = MIN((int)gem_text_width(cr, sub), dw - 32);
	gem_text_clipped(cr, sub, (dw - sw) / 2, y, sw, GEM_ROW_H);
	y += GEM_ROW_H + GEM_PAD;
	int lx = 16, fx = 16 + LABEL_W;
	int row = (GEM_FIELD_H - GEM_ROW_H) / 2;
	gem_text(cr, "User:", lx, y + row, GEM_ROW_H);
	y += GEM_FIELD_H + GEM_PAD;
	gem_text_clipped(cr, long_prompt ? "Answer:" : g.prompt, lx, y + row,
		LABEL_W - 4, GEM_ROW_H);
	y += GEM_FIELD_H + GEM_PAD;
	gem_text(cr, "Session:", lx, y + row, GEM_ROW_H);
	int session_y = y;
	y += GEM_FIELD_H + GEM_PAD;
	if (g.note != NULL) {
		gem_text_clipped(cr, g.note, lx, y, dw - 32, GEM_ROW_H);
	} else if (g.step == ASKING || g.step == STARTING) {
		gem_text(cr, g.step == STARTING ? "Starting..." : "Checking...", lx, y,
			GEM_ROW_H);
	}
	y += GEM_ROW_H + GEM_PAD;
	cairo_restore(cr);
	/* The pop-up and buttons, in the window's space for their hits. */
	popup_button(cr, dx + fx, dy + session_y, FIELD_W - 2, session_name());
	int bw = gem_button_width("Shut Down");
	int okw = MAX(gem_button_width("OK"), bw);
	gem_button(cr, g.hits, dx + 16, dy + y, bw, "Shut Down", HIT_SHUTDOWN, false);
	gem_button(cr, g.hits, dx + dw - 16 - okw, dy + y, okw,
		g.step == PROMPTING ? "Continue" : "OK", HIT_OK, true);
}

static void paint(cairo_t *cr, int w, int h) {
	g_array_set_size(g.hits, 0);
	paint_users(cr);
	paint_dialog(cr, w, h);
}

/* Like gem_draw_pixelated, but on nothing: the desktop shows through. */
static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *c = cairo_create(img);
	gem_set_font(c);
	paint(c, w, h);
	cairo_destroy(c);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(img);
}

/* ---- Doing things -------------------------------------------------------- */

static void choose_user(int i) {
	if (g.step != IDLE || i < 0 || i >= (int)g.users->len) {
		return;
	}
	g.selected = i;
	gtk_editable_set_text(GTK_EDITABLE(g.user_field),
		((struct user *)g.users->pdata[i])->name);
	gtk_widget_grab_focus(g.password_field);
	redraw();
}

static void session_chosen(int id, void *data) {
	if (id >= 0 && id < (int)g.sessions->len) {
		g.session = id;
		redraw();
	}
}

static void shutdown_answered(int button, void *data) {
	if (button == 1) {
		g_spawn_command_line_async("systemctl poweroff", NULL);
	}
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(g.host)) {
		return;
	}
	const struct gem_hit *hit = gem_hit_at(g.hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_USER:
		choose_user(hit->index);
		break;
	case HIT_SESSION:
		if (g.step == IDLE) {
			gem_popup_clear(g.sessions_popup);
			for (guint i = 0; i < g.sessions->len; i++) {
				gem_popup_add(g.sessions_popup, i,
					((struct session *)g.sessions->pdata[i])->name, NULL,
					(int)i == g.session ? GEM_POPUP_CHECKED : 0);
			}
			gem_popup_show(g.sessions_popup, hit->x, hit->y + hit->h);
		}
		break;
	case HIT_OK:
		log_in();
		break;
	case HIT_SHUTDOWN: {
		static const char *const buttons[] = { "Cancel", "Shut Down", NULL };
		gem_alert(g.host, GEM_ALERT_QUESTION, "Shut down the computer?", buttons,
			0, 0, shutdown_answered, NULL);
		break;
	}
	}
}

/* Typing a name selects its icon, if it has one. */
static void user_typed(GtkEditable *e, gpointer data) {
	const char *text = gtk_editable_get_text(e);
	int found = -1;
	for (guint i = 0; i < g.users->len; i++) {
		if (strcmp(((struct user *)g.users->pdata[i])->name, text) == 0) {
			found = i;
		}
	}
	if (found != g.selected) {
		g.selected = found;
		redraw();
	}
}

static void user_activated(GtkEntry *e, gpointer data) {
	gtk_widget_grab_focus(g.password_field);
}

static void password_activated(GtkEntry *e, gpointer data) {
	log_in();
}

static void activate(GtkApplication *app, gpointer data) {
	gem_ui_load_css();
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_string(css,
		"window.greeter, window.greeter > * { background: transparent; }");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(css);

	g.window = gtk_application_window_new(app);
	gtk_widget_add_css_class(g.window, "greeter");
	gtk_window_set_decorated(GTK_WINDOW(g.window), FALSE);
	gtk_window_set_title(GTK_WINDOW(g.window), "GemWM");
	GtkWidget *overlay = gtk_overlay_new();
	g.host = GTK_OVERLAY(overlay);
	g.area = gtk_drawing_area_new();
	gtk_widget_set_hexpand(g.area, TRUE);
	gtk_widget_set_vexpand(g.area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g.area), draw, NULL, NULL);
	g_signal_connect(g.area, "resize", G_CALLBACK(place), NULL);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	gtk_widget_add_controller(g.area, GTK_EVENT_CONTROLLER(click));
	gtk_overlay_set_child(GTK_OVERLAY(overlay), g.area);

	g.user_field = gem_field_new(0, 0, FIELD_W);
	g.password_field = gem_field_new(0, 0, FIELD_W);
	gtk_entry_set_visibility(GTK_ENTRY(g.password_field), FALSE);
	gtk_entry_set_input_purpose(GTK_ENTRY(g.password_field),
		GTK_INPUT_PURPOSE_PASSWORD);
	g_signal_connect(g.user_field, "changed", G_CALLBACK(user_typed), NULL);
	g_signal_connect(g.user_field, "activate", G_CALLBACK(user_activated), NULL);
	g_signal_connect(g.password_field, "activate",
		G_CALLBACK(password_activated), NULL);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), g.user_field);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), g.password_field);
	gtk_window_set_child(GTK_WINDOW(g.window), overlay);
	g.sessions_popup = gem_popup_new(g.area, session_chosen, NULL);

	reset_prompt();
	if (g.selected >= 0) {
		gtk_editable_set_text(GTK_EDITABLE(g.user_field),
			((struct user *)g.users->pdata[g.selected])->name);
	}
	/* Someone chosen already (the last, or the only one): their password. */
	gtk_window_set_focus(GTK_WINDOW(g.window),
		g.selected >= 0 ? g.password_field : g.user_field);
	gtk_window_present(GTK_WINDOW(g.window));
}

int main(int argc, char *argv[]) {
	g.hits = gem_hits_new();
	load_users();
	load_sessions();
	load_last();
	g.app = gtk_application_new("org.gemwm.Greeter", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(g.app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(g.app), argc, argv);
	g_object_unref(g.app);
	return status;
}
