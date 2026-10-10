/*
 * gemwm-lock: GemWM's screen lock (ext-session-lock-v1, through
 * gtk4-layer-shell's session lock).
 *
 * The desktop shows (GemWM draws it; every window is hidden), with a menu
 * bar of its own along the top of each screen, saying it's locked and the
 * time, and on the first screen a GEM dialog: you, by name and icon, and
 * your password, checked by PAM (the "gemwm-lock" service, else "login")
 * in a thread of its own, so nothing waits on it. Anything else PAM says
 * (a fingerprint reader's "place your finger") is shown in the dialog; a
 * wrong password, in an alert.
 *
 *   gemwm-lock            lock now (Super+L, Desk > Lock Screen, idle)
 *   gemwm-lock --watch    stay running, and lock before the computer
 *                         sleeps (a logind delay inhibitor) or when asked
 *                         to (loginctl lock-session); GemWM starts this
 */
#define _GNU_SOURCE /* explicit_bzero */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-session-lock.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-ui.h"

#define BAR_H 20         /* as GemWM's menu bar: 19, and a line */
#define ICON_W 32        /* a person, as the login screen draws them */
#define ICON_H 30
#define DIALOG_W 380
#define LABEL_W 96
#define FIELD_W (DIALOG_W - 2 * 16 - LABEL_W)

enum { HIT_UNLOCK };

/* A screen's window: the bar, and on the first, the dialog. */
struct pane {
	GtkWidget *window, *area;
	GtkOverlay *host;
	bool main;
};

static struct {
	GtkApplication *app;
	GtkSessionLockInstance *lock;
	GPtrArray *panes;
	struct pane *main;
	GtkWidget *field;
	GArray *hits;
	char *user, *full;
	bool twelve_hour;
	char *note;          /* what PAM said, or why it can't */
	bool checking;
	int dialog_x, dialog_y, dialog_h;
} g;

/* ---- Checking the password (PAM, in a thread) ----------------------------- */

struct check {
	char *user, *password;
	char *said; /* PAM's last message */
	bool ok;
};

static int conversation(int n, const struct pam_message **msg,
		struct pam_response **resp, void *data) {
	struct check *c = data;
	struct pam_response *r = calloc(n, sizeof(*r));
	if (r == NULL) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < n; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			r[i].resp = strdup(c->password);
			break;
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(c->user);
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			g_free(c->said);
			c->said = g_strdup(msg[i]->msg);
			break;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static const char *pam_service(void) {
	return access("/etc/pam.d/gemwm-lock", R_OK) == 0 ? "gemwm-lock" : "login";
}

static void check_thread(GTask *task, gpointer source, gpointer data,
		GCancellable *cancel) {
	struct check *c = data;
	struct pam_conv conv = { conversation, c };
	pam_handle_t *pam = NULL;
	int r = pam_start(pam_service(), c->user, &conv, &pam);
	if (r == PAM_SUCCESS) {
		r = pam_authenticate(pam, 0);
	}
	c->ok = r == PAM_SUCCESS;
	if (pam != NULL) {
		pam_end(pam, r);
	}
	explicit_bzero(c->password, strlen(c->password));
	g_task_return_boolean(task, c->ok);
}

static void check_free(void *data) {
	struct check *c = data;
	g_free(c->user);
	g_free(c->password);
	g_free(c->said);
	g_free(c);
}

static void redraw(void) {
	for (guint i = 0; i < g.panes->len; i++) {
		gtk_widget_queue_draw(((struct pane *)g.panes->pdata[i])->area);
	}
}

static void set_note(const char *note) {
	g_free(g.note);
	g.note = g_strdup(note);
	redraw();
}

static void wrong_done(int button, void *data) {
	gtk_widget_grab_focus(g.field);
}

static void checked(GObject *source, GAsyncResult *res, gpointer data) {
	struct check *c = g_task_get_task_data(G_TASK(res));
	g.checking = false;
	gtk_widget_set_sensitive(g.field, TRUE);
	if (c->ok) {
		gtk_session_lock_instance_unlock(g.lock);
		return;
	}
	gtk_editable_set_text(GTK_EDITABLE(g.field), "");
	set_note(NULL);
	static const char *const ok[] = { "OK", NULL };
	char *text = c->said != NULL && strstr(c->said, "ocked") != NULL ?
		g_strdup(c->said) : g_strdup("That isn't your password.");
	gem_alert(g.main->host, GEM_ALERT_STOP, text, ok, 0, 0, wrong_done, NULL);
	g_free(text);
}

static void unlock(void) {
	if (g.checking || gem_alert_up(g.main->host)) {
		return;
	}
	const char *password = gtk_editable_get_text(GTK_EDITABLE(g.field));
	if (password[0] == '\0') {
		gtk_widget_grab_focus(g.field);
		return;
	}
	struct check *c = g_new0(struct check, 1);
	c->user = g_strdup(g.user);
	c->password = g_strdup(password);
	g.checking = true;
	gtk_widget_set_sensitive(g.field, FALSE);
	set_note("Checking...");
	GTask *task = g_task_new(NULL, NULL, checked, NULL);
	g_task_set_task_data(task, c, check_free);
	g_task_run_in_thread(task, check_thread);
	g_object_unref(task);
}

/* ---- Drawing ----------------------------------------------------------- */

/* A person, head and shoulders, as on the login screen. */
static void paint_person(cairo_t *cr, int x, int y) {
	cairo_save(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_set_line_width(cr, 1);
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_translate(cr, x + ICON_W / 2.0, y + ICON_H);
	cairo_scale(cr, 14, 12);
	cairo_arc(cr, 0, 0, 1, G_PI, 2 * G_PI);
	cairo_restore(cr);
	cairo_close_path(cr);
	gem_white(cr);
	cairo_fill_preserve(cr);
	gem_black(cr);
	cairo_stroke(cr);
	cairo_arc(cr, x + ICON_W / 2.0, y + 9.5, 8, 0, 2 * G_PI);
	gem_white(cr);
	cairo_fill_preserve(cr);
	gem_black(cr);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* A padlock, 9 x 11, before "Locked" in the bar. */
static void paint_padlock(cairo_t *cr, int x, int y) {
	static const char *const rows[] = {
		"..###..",
		".#...#.",
		".#...#.",
		"#######",
		"#######",
		"###.###",
		"###.###",
		"#######",
	};
	gem_bitmap(cr, rows, 8, x, y);
}

static char *clock_text(void) {
	GDateTime *now = g_date_time_new_now_local();
	char *t = g_date_time_format(now, g.twelve_hour ? "%l:%M %p" : "%H:%M");
	g_date_time_unref(now);
	return g_strstrip(t);
}

static void paint_bar(cairo_t *cr, int w) {
	gem_white(cr);
	gem_fill(cr, 0, 0, w, BAR_H - 1);
	gem_black(cr);
	gem_fill(cr, 0, BAR_H - 1, w, 1);
	paint_padlock(cr, 12, 5);
	gem_text(cr, "Locked", 26, 0, BAR_H - 1);
	char *t = clock_text();
	gem_text(cr, t, w - 12 - gem_text_width(cr, t), 0, BAR_H - 1);
	g_free(t);
}

static int dialog_height(void) {
	return 2 * GEM_PAD + ICON_H + 2 * GEM_PAD + GEM_FIELD_H + GEM_PAD +
		GEM_ROW_H + GEM_PAD + GEM_BUTTON_H + 2 * GEM_PAD;
}

static int field_y(void) {
	return g.dialog_y + 2 * GEM_PAD + ICON_H + 2 * GEM_PAD;
}

static void paint_dialog(cairo_t *cr) {
	int dx = g.dialog_x, dy = g.dialog_y, dw = DIALOG_W, dh = g.dialog_h;
	cairo_save(cr);
	cairo_translate(cr, dx, dy);
	gem_dialog_frame(cr, dw, dh);
	int y = 2 * GEM_PAD;
	paint_person(cr, 16, y);
	const char *name = g.full != NULL && g.full[0] != '\0' ? g.full : g.user;
	gem_text_clipped(cr, name, 16 + ICON_W + 16, y, dw - ICON_W - 48, GEM_ROW_H);
	gem_text_clipped(cr, name, 16 + ICON_W + 17, y, dw - ICON_W - 48, GEM_ROW_H);
	gem_text(cr, "This screen is locked.", 16 + ICON_W + 16, y + GEM_ROW_H,
		GEM_ROW_H);
	y += ICON_H + 2 * GEM_PAD;
	gem_text(cr, "Password:", 16, y + (GEM_FIELD_H - GEM_ROW_H) / 2, GEM_ROW_H);
	y += GEM_FIELD_H + GEM_PAD;
	/* What's going on: PAM's word, or Caps Lock. */
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
	GdkDevice *kb = seat != NULL ? gdk_seat_get_keyboard(seat) : NULL;
	const char *note = g.note != NULL ? g.note :
		kb != NULL && gdk_device_get_caps_lock_state(kb) ? "Caps Lock is on." : NULL;
	if (note != NULL) {
		gem_text_clipped(cr, note, 16, y, dw - 32, GEM_ROW_H);
	}
	y += GEM_ROW_H + GEM_PAD;
	cairo_restore(cr);
	int bw = gem_button_width("Unlock");
	gem_button(cr, g.hits, dx + dw - 16 - bw, dy + y, bw, "Unlock", HIT_UNLOCK,
		true);
}

static void paint(cairo_t *cr, int w, int h, struct pane *p) {
	if (p->main) {
		g_array_set_size(g.hits, 0);
		paint_dialog(cr);
	}
	paint_bar(cr, w);
}

/* Drawn in whole pixels over nothing: the desktop shows through. */
static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *c = cairo_create(img);
	gem_set_font(c);
	paint(c, w, h, data);
	cairo_destroy(c);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(img);
}

/* The dialog in the middle, and its field in it: on a resize. */
static void place(GtkDrawingArea *area, int w, int h, gpointer data) {
	g.dialog_h = dialog_height();
	g.dialog_x = (w - DIALOG_W) / 2;
	g.dialog_y = MAX(BAR_H + 16, (h - g.dialog_h) / 2);
	gtk_widget_set_margin_start(g.field, g.dialog_x + 16 + LABEL_W);
	gtk_widget_set_margin_top(g.field, field_y());
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		gpointer data) {
	if (gem_alert_up(g.main->host)) {
		return;
	}
	const struct gem_hit *hit = gem_hit_at(g.hits, x, y);
	if (hit != NULL && hit->id == HIT_UNLOCK) {
		unlock();
	} else {
		gtk_widget_grab_focus(g.field);
	}
}

static void activated(GtkEntry *e, gpointer data) {
	unlock();
}

/* Typing clears what PAM last said; Caps Lock shows as it changes. */
static void typed(GtkEditable *e, gpointer data) {
	if (g.note != NULL && !g.checking) {
		set_note(NULL);
	}
}

static gboolean tick(gpointer data) {
	redraw();
	return G_SOURCE_CONTINUE;
}

/* ---- Locking ------------------------------------------------------------- */

static void new_monitor(GtkSessionLockInstance *lock, GdkMonitor *monitor,
		gpointer data) {
	struct pane *p = g_new0(struct pane, 1);
	p->main = g.main == NULL;
	p->window = gtk_application_window_new(g.app);
	gtk_widget_add_css_class(p->window, "gemwm-lock");
	GtkWidget *overlay = gtk_overlay_new();
	p->host = GTK_OVERLAY(overlay);
	p->area = gtk_drawing_area_new();
	gtk_widget_set_hexpand(p->area, TRUE);
	gtk_widget_set_vexpand(p->area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->area), draw, p, NULL);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), p->area);
	if (p->main) {
		g.main = p;
		g_signal_connect(p->area, "resize", G_CALLBACK(place), NULL);
		GtkGesture *click = gtk_gesture_click_new();
		g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
		gtk_widget_add_controller(p->area, GTK_EVENT_CONTROLLER(click));
		g.field = gem_field_new(0, 0, FIELD_W);
		gtk_entry_set_visibility(GTK_ENTRY(g.field), FALSE);
		gtk_entry_set_input_purpose(GTK_ENTRY(g.field), GTK_INPUT_PURPOSE_PASSWORD);
		g_signal_connect(g.field, "activate", G_CALLBACK(activated), NULL);
		g_signal_connect(g.field, "changed", G_CALLBACK(typed), NULL);
		gtk_overlay_add_overlay(GTK_OVERLAY(overlay), g.field);
		gtk_window_set_focus(GTK_WINDOW(p->window), g.field);
	}
	gtk_window_set_child(GTK_WINDOW(p->window), overlay);
	g_ptr_array_add(g.panes, p);
	gtk_session_lock_instance_assign_window_to_monitor(lock,
		GTK_WINDOW(p->window), monitor);
}

static void unlocked(GtkSessionLockInstance *lock, gpointer data) {
	g_application_quit(G_APPLICATION(g.app));
}

static void failed(GtkSessionLockInstance *lock, gpointer data) {
	/* Locked already (another gemwm-lock), or no locking here. */
	g_printerr("gemwm-lock: couldn't lock the screen\n");
	g_application_quit(G_APPLICATION(g.app));
}

/* [clock] mode = 12h in GemWM's config, as the menu bar's clock reads it. */
static bool config_twelve_hour(void) {
	char *path = g_build_filename(g_get_user_config_dir(), "gemwm", "config",
		NULL);
	char *text = NULL;
	bool twelve = false;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		bool in_clock = false;
		char **lines = g_strsplit(text, "\n", -1);
		for (char **l = lines; *l != NULL; l++) {
			char *s = g_strstrip(*l);
			if (s[0] == '[') {
				in_clock = g_str_has_prefix(s, "[clock]");
			} else if (in_clock && g_str_has_prefix(s, "mode")) {
				char *eq = strchr(s, '=');
				twelve = eq != NULL && g_str_has_prefix(g_strchug(eq + 1), "12h");
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);
	return twelve;
}

static void activate(GtkApplication *app, gpointer data) {
	if (!gtk_session_lock_is_supported()) {
		g_printerr("gemwm-lock: this compositor can't lock the screen\n");
		return;
	}
	gem_ui_load_css();
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_string(css,
		"window.gemwm-lock, window.gemwm-lock > * { background: transparent; }");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(css);

	g_application_hold(G_APPLICATION(app));
	g.lock = gtk_session_lock_instance_new();
	g_signal_connect(g.lock, "monitor", G_CALLBACK(new_monitor), NULL);
	g_signal_connect(g.lock, "unlocked", G_CALLBACK(unlocked), NULL);
	g_signal_connect(g.lock, "failed", G_CALLBACK(failed), NULL);
	gtk_session_lock_instance_lock(g.lock);
	g_timeout_add_seconds(1, tick, NULL);
}

/* ---- --watch: locking before sleep --------------------------------------- */

static struct {
	GDBusConnection *bus;
	int inhibitor;     /* logind's delay lock, while held */
	guint waiting;     /* checking the lock shows, before sleep */
	int tries;
} w = { .inhibitor = -1 };

/* A delay inhibitor: logind waits (a few seconds at most) for us before
 * the computer sleeps. */
static void take_inhibitor(void) {
	if (w.inhibitor >= 0) {
		return;
	}
	GUnixFDList *fds = NULL;
	GError *error = NULL;
	GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(w.bus,
		"org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit",
		g_variant_new("(ssss)", "sleep", "GemWM",
			"Lock the screen before sleeping", "delay"),
		G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL,
		&error);
	if (r == NULL) {
		g_printerr("gemwm-lock: no sleep inhibitor: %s\n", error->message);
		g_error_free(error);
		return;
	}
	gint32 index;
	g_variant_get(r, "(h)", &index);
	w.inhibitor = g_unix_fd_list_get(fds, index, NULL);
	g_variant_unref(r);
	g_object_unref(fds);
}

static void let_sleep(void) {
	if (w.inhibitor >= 0) {
		close(w.inhibitor);
		w.inhibitor = -1;
	}
}

static void lock_now(void) {
	g_spawn_command_line_async("gemwm msg lock", NULL);
}

/* Until the screens show only the lock (or three seconds), then sleep. */
static gboolean check_shown(gpointer data) {
	char *out = NULL;
	g_spawn_command_line_sync("gemwm msg lock-status", &out, NULL, NULL, NULL);
	bool shown = out != NULL && strstr(out, "\"shown\":true") != NULL;
	g_free(out);
	if (shown || ++w.tries > 30) {
		w.waiting = 0;
		let_sleep();
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void prepare_for_sleep(GDBusConnection *c, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	gboolean sleeping;
	g_variant_get(params, "(b)", &sleeping);
	if (sleeping) {
		lock_now();
		w.tries = 0;
		if (w.waiting == 0) {
			w.waiting = g_timeout_add(100, check_shown, NULL);
		}
	} else {
		take_inhibitor(); /* for next time */
	}
}

/* loginctl lock-session (and anything else asking logind). */
static void session_lock(GDBusConnection *c, const char *sender,
		const char *path, const char *iface, const char *signal,
		GVariant *params, gpointer data) {
	lock_now();
}

static gboolean compositor_gone(gint fd, GIOCondition cond, gpointer loop) {
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static int watch(void) {
	GError *error = NULL;
	w.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (w.bus == NULL) {
		g_printerr("gemwm-lock: no system bus: %s\n", error->message);
		return 1;
	}
	/* Ours to watch while GemWM runs: when its display goes, we go. */
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		g_printerr("gemwm-lock: no Wayland display\n");
		return 1;
	}
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_unix_fd_add(wl_display_get_fd(display), G_IO_HUP | G_IO_ERR,
		compositor_gone, loop);

	g_dbus_connection_signal_subscribe(w.bus, "org.freedesktop.login1",
		"org.freedesktop.login1.Manager", "PrepareForSleep",
		"/org/freedesktop/login1", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		prepare_for_sleep, NULL, NULL);
	GVariant *r = g_dbus_connection_call_sync(w.bus, "org.freedesktop.login1",
		"/org/freedesktop/login1", "org.freedesktop.login1.Manager",
		"GetSessionByPID", g_variant_new("(u)", (guint32)getpid()),
		G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (r != NULL) {
		const char *session;
		g_variant_get(r, "(&o)", &session);
		g_dbus_connection_signal_subscribe(w.bus, "org.freedesktop.login1",
			"org.freedesktop.login1.Session", "Lock", session, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, session_lock, NULL, NULL);
		g_variant_unref(r);
	}
	take_inhibitor();
	g_main_loop_run(loop);
	let_sleep();
	wl_display_disconnect(display);
	return 0;
}

/* ---- Main ---------------------------------------------------------------- */

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "--watch") == 0) {
		return watch();
	}
	struct passwd *pw = getpwuid(getuid());
	if (pw == NULL) {
		g_printerr("gemwm-lock: who are you?\n");
		return 1;
	}
	g.user = g_strdup(pw->pw_name);
	/* The GECOS field's first part is the full name. */
	char **gecos = g_strsplit(pw->pw_gecos != NULL ? pw->pw_gecos : "", ",", 2);
	g.full = g_strdup(gecos[0] != NULL ? gecos[0] : "");
	g_strfreev(gecos);
	g.twelve_hour = config_twelve_hour();
	g.panes = g_ptr_array_new();
	g.hits = gem_hits_new();
	g.app = gtk_application_new("org.gemwm.Lock", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(g.app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(g.app), 1, argv);
	g_object_unref(g.app);
	return status;
}
