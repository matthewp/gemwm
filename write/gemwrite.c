/*
 * GemWrite: a word processor, drawn like GEM.
 *
 *   ruler     | 1   2   3   4   5   6 |
 *   page      |  ------------------   |#|
 *             | |Meeting notes     |  |#|  comments
 *             | |                  |  | |
 *   info line: 124 words                          Fit 85%
 *
 * The document model, layout, file formats and every command come from
 * the core (core/: forked from Ream), which knows nothing of GTK; this is
 * the window around it. Its menus are in GemWM's menu bar, most of their
 * items rows of the core's command table (core/edit/commands.c), which
 * gemwrite-cli runs from the shell too.
 */
#include <math.h>
#include <string.h>
#include "app-menu.h"
#include "comments.h"
#include "edit/commands.h"
#include "find.h"
#include "fonts.h"
#include "gem-alert.h"
#include "gem-file.h"
#include "gem-popup.h"
#include "gem-print.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "layout/layout_pango.h"
#include "page.h"
#include "print.h"
#include "ruler.h"
#include "settings.h"

#define INFO_H 20            /* the info line, with its rule */
#define AUTOSAVE_DELAY 750   /* ms after the last change */
#define MESSAGE_TIME 4       /* seconds a message stays on the info line */
#define MAX_SUGGESTIONS 6
#define FILE_PATTERN "*.odt,*.txt"

/* Everything the menus, keys and right-click menu do. */
enum action {
	ACT_NONE,
	ACT_NEW, ACT_OPEN, ACT_SAVE, ACT_SAVE_AS, ACT_RELOAD, ACT_EXPORT,
	ACT_EXPORT_PDF,
	ACT_PRINT, ACT_CLOSE, ACT_QUIT,
	ACT_UNDO, ACT_REDO, ACT_CUT, ACT_COPY, ACT_PASTE, ACT_SELECT_ALL,
	ACT_FIND, ACT_FIND_NEXT, ACT_FIND_PREVIOUS, ACT_REPLACE, ACT_COMMENT,
	ACT_BOLD, ACT_ITALIC, ACT_UNDERLINE, ACT_FONT, ACT_BIGGER, ACT_SMALLER,
	ACT_BODY, ACT_TITLE, ACT_H1, ACT_H2, ACT_H3, ACT_QUOTE, ACT_NOTE,
	ACT_LEFT, ACT_CENTER, ACT_RIGHT, ACT_JUSTIFY,
	ACT_SINGLE, ACT_115, ACT_ONE_HALF, ACT_DOUBLE,
	ACT_INDENT, ACT_OUTDENT, ACT_BULLETS, ACT_NUMBERS, ACT_CLEAR_TABS,
	ACT_RULER, ACT_COMMENTS, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_FIT,
	ACT_SPELLING, ACT_AUTOSAVE, ACT_SYNC,
	ACT_SPELL_ADD, ACT_SPELL_IGNORE,
	ACT_SUGGESTION, /* the first spelling suggestion; the rest follow */
};

/* An action's keys, and the command-table command it runs, if it's one.
 * NULL keys take the command's own. */
static const struct binding {
	enum action action;
	const char *command;
	const char *arg;       /* its argument, for commands that take one */
	const char *accel, *alt;
} bindings[] = {
	{ ACT_NEW, NULL, NULL, "<Control>n" },
	{ ACT_OPEN, NULL, NULL, "<Control>o" },
	{ ACT_SAVE, NULL, NULL, "<Control>s" },
	{ ACT_SAVE_AS, NULL, NULL, "<Control><Shift>s" },
	{ ACT_PRINT, NULL, NULL, "<Control>p" },
	{ ACT_CLOSE, NULL, NULL, "<Control>w" },
	{ ACT_QUIT, NULL, NULL, "<Control>q" },
	{ ACT_UNDO, "undo" },
	{ ACT_REDO, "redo" },
	{ ACT_CUT, NULL, NULL, "<Control>x", "<Shift>Delete" },
	{ ACT_COPY, NULL, NULL, "<Control>c", "<Control>Insert" },
	{ ACT_PASTE, NULL, NULL, "<Control>v", "<Shift>Insert" },
	{ ACT_SELECT_ALL, "select-all" },
	{ ACT_FIND, NULL, NULL, "<Control>f" },
	{ ACT_FIND_NEXT, NULL, NULL, "<Control>g" },
	{ ACT_FIND_PREVIOUS, NULL, NULL, "<Control><Shift>g" },
	{ ACT_REPLACE, NULL, NULL, "<Control>h" },
	{ ACT_COMMENT, NULL, NULL, "<Control><Alt>m" },
	{ ACT_BOLD, "bold" },
	{ ACT_ITALIC, "italic" },
	{ ACT_UNDERLINE, "underline" },
	{ ACT_BIGGER, "font-bigger" },
	{ ACT_SMALLER, "font-smaller" },
	{ ACT_BODY, "style-body" },
	{ ACT_TITLE, "style-title" },
	{ ACT_H1, "style-h1" },
	{ ACT_H2, "style-h2" },
	{ ACT_H3, "style-h3" },
	{ ACT_QUOTE, "style-quote" },
	{ ACT_NOTE, "style-note" },
	{ ACT_LEFT, "align", "left" },
	{ ACT_CENTER, "align", "center" },
	{ ACT_RIGHT, "align", "right" },
	{ ACT_JUSTIFY, "align", "justify" },
	{ ACT_SINGLE, "spacing-single" },
	{ ACT_115, "line-spacing", "1.15" },
	{ ACT_ONE_HALF, "spacing-1.5" },
	{ ACT_DOUBLE, "spacing-double" },
	{ ACT_INDENT, "indent" },
	{ ACT_OUTDENT, "outdent" },
	{ ACT_BULLETS, "list-bullet" },
	{ ACT_NUMBERS, "list-number" },
	{ ACT_CLEAR_TABS, "tab-clear" },
	{ ACT_ZOOM_IN, NULL, NULL, "<Control>plus", "<Control>equal" },
	{ ACT_ZOOM_OUT, NULL, NULL, "<Control>minus" },
	{ ACT_ZOOM_FIT, NULL, NULL, "<Control>0" },
	{ ACT_RELOAD, "reload" },
	{ ACT_SPELL_ADD, "spell-add" },
	{ ACT_SPELL_IGNORE, "spell-ignore" },
};

struct win {
	GtkWidget *window;
	GtkOverlay *host;          /* alerts and the item selector go over it */
	WpEditor *ed;
	WpDocument *doc;
	WpPageView *view;
	GtkWidget *ruler, *scroller, *hbar, *info;
	struct comments *comments;
	struct find *find;
	struct gem_popup *popup;
	struct app_menu *menu;
	char **suggestions;        /* on the right-click menu */

	GArray *info_hits;
	char *message;             /* on the info line for a while */
	guint message_id;

	guint autosave_id;
	/* The file, watched so another program's writes are noticed: with Sync
	 * on and nothing unsaved it's read again, else the info line says so. */
	GFileMonitor *monitor;
	char *monitor_path;
	guint disk_check_id;
	int reload_tries;
	bool changed_on_disk;
	bool closing;              /* asked, and closing for real */
	bool close_after_save;
};

static struct {
	GtkApplication *app;
	GList *windows;            /* struct win */
	bool autosave, sync, spelling, ruler;
} app;

static struct win *win_new(void);
static void run(struct win *w, enum action action);
static void update(struct win *w);

/* ---- Small things ---------------------------------------------------------- */

static const struct binding *binding_of(enum action action) {
	for (size_t i = 0; i < G_N_ELEMENTS(bindings); i++) {
		if (bindings[i].action == action) {
			return &bindings[i];
		}
	}
	return NULL;
}

/* The binding's keys, the command's own if it has none. */
static const char *accel_of(const struct binding *b, bool alt) {
	if (b->accel != NULL || b->command == NULL) {
		return alt ? b->alt : b->accel;
	}
	const WpCommand *cmd = wp_command_find(b->command);
	/* A command with an argument has keys of its own only as another row. */
	if (cmd == NULL || b->arg != NULL) {
		return NULL;
	}
	return alt ? cmd->alt_accel : cmd->accel;
}

/* GTK's "<Control><Shift>z" as GEM's menus show it: "^Shift+Z". */
static char *shortcut_label(enum action action) {
	const struct binding *b = binding_of(action);
	const char *accel = b != NULL ? accel_of(b, false) : NULL;
	guint key;
	GdkModifierType mods;
	if (accel == NULL || !gtk_accelerator_parse(accel, &key, &mods)) {
		return NULL;
	}
	GString *s = g_string_new(NULL);
	if (mods & GDK_CONTROL_MASK) {
		g_string_append_c(s, '^');
	}
	if (mods & GDK_ALT_MASK) {
		g_string_append(s, "Alt+");
	}
	if (mods & GDK_SHIFT_MASK) {
		g_string_append(s, "Shift+");
	}
	gunichar c = gdk_keyval_to_unicode(key);
	if (c > ' ') {
		g_string_append_unichar(s, g_unichar_toupper(c));
	} else {
		g_string_append(s, gdk_keyval_name(key));
	}
	return g_string_free(s, FALSE);
}

static char *display_name(struct win *w) {
	const char *path = wp_editor_path(w->ed);
	return path != NULL ? g_path_get_basename(path) : g_strdup("Untitled");
}

static bool untouched(struct win *w) {
	return wp_editor_path(w->ed) == NULL && !wp_editor_modified(w->ed) &&
		!wp_editor_can_undo(w->ed);
}

static bool held(struct win *w) {
	return gem_alert_up(w->host);
}

static void focus_page(struct win *w) {
	gtk_widget_grab_focus(GTK_WIDGET(w->view));
}

/* ---- The info line --------------------------------------------------------- */

enum { INFO_RELOAD };

static gboolean message_expired(gpointer data) {
	struct win *w = data;
	w->message_id = 0;
	g_clear_pointer(&w->message, g_free);
	gtk_widget_queue_draw(w->info);
	return G_SOURCE_REMOVE;
}

static void show_message(struct win *w, const char *message) {
	g_free(w->message);
	w->message = g_strdup(message);
	if (w->message_id != 0) {
		g_source_remove(w->message_id);
	}
	w->message_id = g_timeout_add_seconds(MESSAGE_TIME, message_expired, w);
	gtk_widget_queue_draw(w->info);
}

static void paint_info(cairo_t *cr, int w, int h, void *data) {
	struct win *win = data;
	g_array_set_size(win->info_hits, 0);
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	int right = w - GEM_PAD;

	/* The zoom, at the right. */
	int pct = (int)(wp_page_view_get_zoom(win->view) * 72.0 / 96.0 * 100 + 0.5);
	char *zoom = wp_page_view_get_fit_width(win->view) ?
		g_strdup_printf("Fit %d%%", pct) : g_strdup_printf("%d%%", pct);
	right -= (int)gem_text_width(cr, zoom);
	gem_text(cr, zoom, right, 1, h - 1);
	g_free(zoom);

	/* Someone else wrote the file: say so, with the way to take theirs. */
	if (win->changed_on_disk) {
		const char *label = "Reload";
		int bw = (int)gem_text_width(cr, label) + GEM_PAD;
		right -= 2 * GEM_PAD + bw;
		gem_frame(cr, right, 2, bw, h - 3, 1);
		gem_text(cr, label, right + GEM_PAD / 2, 1, h - 1);
		gem_hit_add(win->info_hits, right, 1, bw, h - 1, INFO_RELOAD, 0);
		const char *note = " Changed on disk ";
		int nw = (int)gem_text_width(cr, note);
		right -= GEM_PAD + nw;
		gem_fill(cr, right, 2, nw, h - 3);
		gem_white(cr);
		gem_text(cr, note, right, 1, h - 1);
		gem_black(cr);
	}

	/* A message, or the words. */
	char *text;
	if (win->message != NULL) {
		text = g_strdup(win->message);
	} else {
		size_t total = wp_editor_word_count(win->ed, (WpPos){ 0, 0 },
			wp_document_end(win->doc));
		if (wp_editor_has_selection(win->ed)) {
			WpPos a, b;
			wp_editor_get_selection(win->ed, &a, &b);
			text = g_strdup_printf("%zu of %zu words",
				wp_editor_word_count(win->ed, a, b), total);
		} else {
			text = g_strdup_printf(total == 1 ? "%zu word" : "%zu words", total);
		}
	}
	gem_text_clipped(cr, text, GEM_PAD, 1, right - 2 * GEM_PAD, h - 1);
	g_free(text);
}

static bool reload_from_disk(struct win *w, char **err);
static void show_error(struct win *w, const char *what, const char *msg);

static void info_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct win *w = data;
	const struct gem_hit *hit = gem_hit_at(w->info_hits, x, y);
	if (hit != NULL && hit->id == INFO_RELOAD) {
		char *err = NULL;
		if (!reload_from_disk(w, &err)) {
			show_error(w, "Couldn't read the document again.", err);
			free(err);
		}
		focus_page(w);
	}
}

/* ---- Alerts ---------------------------------------------------------------- */

static void show_error(struct win *w, const char *what, const char *msg) {
	char *text = g_strdup_printf("%s\n%s", what, msg != NULL ? msg : "");
	static const char *const ok[] = { "OK", NULL };
	gem_alert(w->host, GEM_ALERT_STOP, g_strstrip(text), ok, 0, 0, NULL, NULL);
	g_free(text);
}

/* A command from the table, by name; a failure in an alert. */
static void run_command(struct win *w, const char *name, GVariant *arg) {
	if (arg != NULL) {
		g_variant_ref_sink(arg);
	}
	char *err = NULL;
	GVariant *out = NULL;
	if (!wp_command_run(w->ed, name, arg, &out, &err)) {
		char *what = g_strdup_printf("%s didn't work.",
			wp_command_find(name)->summary);
		show_error(w, what, err);
		g_free(what);
		free(err);
	}
	if (out != NULL) {
		g_variant_unref(g_variant_ref_sink(out));
	}
	if (arg != NULL) {
		g_variant_unref(arg);
	}
}

static void comments_run(const char *command, GVariant *arg, void *data) {
	run_command(data, command, arg);
}

/* ---- Spelling -------------------------------------------------------------- */

/* A checker for the editor, or none, as Options says. A missing dictionary
 * is an alert when it's just been turned on, else only logged. */
static void apply_spelling(struct win *w, bool report) {
	if (!app.spelling) {
		wp_editor_set_spell(w->ed, NULL);
		return;
	}
	char *err = NULL;
	if (wp_editor_ensure_spell(w->ed, &err)) {
		return;
	}
	if (report) {
		show_error(w, "Spelling can't be checked.", err);
	} else {
		g_message("Spelling isn't checked: %s", err);
	}
	free(err);
}

/* ---- Saving and the file on disk -------------------------------------------- */

/* A file named without an extension is an .odt. */
static char *with_extension(const char *path, const char *ext) {
	char *base = g_path_get_basename(path);
	bool has = strrchr(base, '.') != NULL && strrchr(base, '.') != base;
	g_free(base);
	return has ? g_strdup(path) : g_strconcat(path, ext, NULL);
}

static bool save_to(struct win *w, const char *path) {
	char *msg = NULL;
	if (!wp_editor_save(w->ed, path, &msg)) {
		show_error(w, "Couldn't save the document.", msg);
		free(msg);
		return false;
	}
	w->changed_on_disk = false; /* ours is the one on disk now */
	update(w);
	return true;
}

static void finish_close(struct win *w) {
	if (w->close_after_save) {
		w->closing = true;
		gtk_window_close(GTK_WINDOW(w->window));
	}
}

static void saved_as(const char *path, void *data) {
	struct win *w = data;
	if (path == NULL) {
		w->close_after_save = false;
		return;
	}
	char *full = with_extension(path, ".odt");
	if (save_to(w, full)) {
		finish_close(w);
	} else {
		w->close_after_save = false;
	}
	g_free(full);
}

static void save_as(struct win *w) {
	const char *path = wp_editor_path(w->ed);
	char *folder = path != NULL ? g_path_get_dirname(path) : NULL;
	char *name = path != NULL ? g_path_get_basename(path) : g_strdup("Untitled.odt");
	gem_file_select(w->host, "Save As", true, folder, name, FILE_PATTERN,
		saved_as, w);
	g_free(folder);
	g_free(name);
}

static void save(struct win *w) {
	if (wp_editor_path(w->ed) == NULL) {
		save_as(w);
	} else if (save_to(w, NULL)) {
		finish_close(w);
	}
}

/* Written shortly after the last change, if Autosave is on and there's
 * somewhere to write it. A failure turns it off, so it's reported once. */
static gboolean autosave_fire(gpointer data) {
	struct win *w = data;
	w->autosave_id = 0;
	if (!wp_editor_modified(w->ed) || wp_editor_path(w->ed) == NULL) {
		return G_SOURCE_REMOVE;
	}
	char *msg = NULL;
	if (wp_editor_save(w->ed, NULL, &msg)) {
		w->changed_on_disk = false;
		update(w);
	} else {
		app.autosave = app.sync = false;
		wp_settings_set_bool("autosave", false);
		wp_settings_set_bool("sync", false);
		char *detail = g_strdup_printf("%s\nAutosave is off now.",
			msg != NULL ? msg : "");
		show_error(w, "Couldn't save the document.", detail);
		g_free(detail);
		free(msg);
	}
	return G_SOURCE_REMOVE;
}

static void schedule_autosave(struct win *w) {
	if (!app.autosave || wp_editor_path(w->ed) == NULL ||
			!wp_editor_modified(w->ed)) {
		return;
	}
	if (w->autosave_id != 0) {
		g_source_remove(w->autosave_id);
	}
	w->autosave_id = g_timeout_add(AUTOSAVE_DELAY, autosave_fire, w);
}

static bool reload_from_disk(struct win *w, char **err) {
	if (!wp_editor_reload(w->ed, err)) {
		return false;
	}
	w->changed_on_disk = false;
	update(w);
	return true;
}

/* Something wrote the file. Our own saves are recognised (the editor
 * knows what it wrote); a file still being written gets a few tries. */
static gboolean check_disk(gpointer data) {
	struct win *w = data;
	w->disk_check_id = 0;
	if (!wp_editor_file_changed_on_disk(w->ed)) {
		return G_SOURCE_REMOVE;
	}
	if (app.sync && !wp_editor_modified(w->ed)) {
		char *err = NULL;
		if (reload_from_disk(w, &err)) {
			w->reload_tries = 0;
			return G_SOURCE_REMOVE;
		}
		free(err);
		if (w->reload_tries++ < 3) {
			w->disk_check_id = g_timeout_add(400, check_disk, w);
			return G_SOURCE_REMOVE;
		}
		w->reload_tries = 0;
	}
	w->changed_on_disk = true;
	gtk_widget_queue_draw(w->info);
	return G_SOURCE_REMOVE;
}

static void schedule_disk_check(struct win *w) {
	if (w->disk_check_id != 0) {
		g_source_remove(w->disk_check_id);
	}
	w->disk_check_id = g_timeout_add(250, check_disk, w);
}

static void file_changed(GFileMonitor *m, GFile *file, GFile *other,
		GFileMonitorEvent event, gpointer data) {
	if (event != G_FILE_MONITOR_EVENT_DELETED &&
			event != G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
		schedule_disk_check(data);
	}
}

/* The monitor on the editor's file, if that's changed (open, Save As). */
static void watch_file(struct win *w) {
	const char *path = wp_editor_path(w->ed);
	if (g_strcmp0(path, w->monitor_path) == 0) {
		return;
	}
	g_clear_object(&w->monitor);
	g_clear_pointer(&w->monitor_path, g_free);
	w->changed_on_disk = false;
	if (path == NULL) {
		return;
	}
	GFile *f = g_file_new_for_path(path);
	w->monitor = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
	g_object_unref(f);
	if (w->monitor != NULL) {
		g_signal_connect(w->monitor, "changed", G_CALLBACK(file_changed), w);
	}
	w->monitor_path = g_strdup(path);
}

/* ---- Opening ---------------------------------------------------------------- */

static bool load(struct win *w, const char *path) {
	char *msg = NULL;
	if (!wp_editor_load(w->ed, path, &msg)) {
		show_error(w, "Couldn't open the document.", msg);
		free(msg);
		return false;
	}
	focus_page(w);
	return true;
}

/* In this window if it's still empty, else in a window of its own. */
static void open_path(struct win *w, const char *path) {
	if (w == NULL || !untouched(w)) {
		w = win_new();
	}
	load(w, path);
}

static void opened(const char *path, void *data) {
	if (path != NULL) {
		open_path(data, path);
	}
}

/* Export writes a copy in another format and leaves the document on its
 * own file, so it's the command's, not the editor's save. */
struct export {
	struct win *w;
	const char *command, *ext;
};

static void exported(const char *path, void *data) {
	struct export *x = data;
	struct win *w = x->w;
	if (path != NULL) {
		char *full = with_extension(path, x->ext);
		char *msg = NULL;
		GVariant *arg = g_variant_ref_sink(g_variant_new_string(full));
		if (wp_command_run(w->ed, x->command, arg, NULL, &msg)) {
			char *base = g_path_get_basename(full);
			char *done = g_strdup_printf("Exported %s", base);
			show_message(w, done);
			g_free(done);
			g_free(base);
		} else {
			show_error(w, "Couldn't export the document.", msg);
			free(msg);
		}
		g_variant_unref(arg);
		g_free(full);
	}
	g_free(x);
}

/* Beside the document, named after it. */
static void export_as(struct win *w, const char *title, const char *command,
		const char *ext) {
	const char *path = wp_editor_path(w->ed);
	char *folder = path != NULL ? g_path_get_dirname(path) : NULL;
	char *base = path != NULL ? g_path_get_basename(path) : g_strdup("Untitled");
	char *dot = strrchr(base, '.');
	if (dot != NULL && dot != base) {
		*dot = '\0';
	}
	char *name = g_strconcat(base, ext, NULL);
	char *pattern = g_strconcat("*", ext, NULL);
	struct export *x = g_new(struct export, 1);
	x->w = w;
	x->command = command;
	x->ext = ext;
	gem_file_select(w->host, title, true, folder, name, pattern, exported, x);
	g_free(pattern);
	g_free(name);
	g_free(base);
	g_free(folder);
}

static void printed(const char *message, void *data) {
	struct win *w = data;
	if (message != NULL) {
		show_message(w, message);
	}
}

/* ---- The clipboard ---------------------------------------------------------- */

/* The editor's clipboard has the formatted text, the system's the same as
 * plain text for other programs. A paste compares them: while the
 * system's is still what we copied, the formatted text goes in. */
static void publish_clipboard(struct win *w) {
	const char *text = wp_clipboard_text();
	if (text != NULL) {
		gdk_clipboard_set_text(gtk_widget_get_clipboard(w->window), text);
	}
}

static void paste_ready(GObject *src, GAsyncResult *res, gpointer data) {
	GtkWidget *window = data;
	struct win *w = g_object_get_data(G_OBJECT(window), "gw-win");
	char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
	if (text != NULL && w != NULL) {
		const char *ours = wp_clipboard_text();
		if (ours == NULL || strcmp(ours, text) != 0) {
			wp_clipboard_set_text(text, -1);
		}
		wp_editor_paste(w->ed);
	}
	g_free(text);
	g_object_unref(window);
}

static bool can_paste(struct win *w) {
	GdkContentFormats *formats = gdk_clipboard_get_formats(
		gtk_widget_get_clipboard(w->window));
	return gdk_content_formats_contain_gtype(formats, G_TYPE_STRING);
}

/* ---- Options ------------------------------------------------------------------ */

static void apply_ruler(struct win *w) {
	gtk_widget_set_visible(w->ruler, app.ruler);
}

/* Sync is Autosave and reading the file again when something else writes
 * it, so it needs Autosave: one on turns both on, Autosave off both off. */
static void set_option(enum action action) {
	switch (action) {
	case ACT_RULER:
		app.ruler = !app.ruler;
		wp_settings_set_bool("ruler", app.ruler);
		break;
	case ACT_SPELLING:
		app.spelling = !app.spelling;
		wp_settings_set_bool("spellcheck", app.spelling);
		break;
	case ACT_AUTOSAVE:
		app.autosave = !app.autosave;
		if (!app.autosave) {
			app.sync = false;
		}
		break;
	case ACT_SYNC:
		app.sync = !app.sync;
		if (app.sync) {
			app.autosave = true;
		}
		break;
	default:
		return;
	}
	wp_settings_set_bool("autosave", app.autosave);
	wp_settings_set_bool("sync", app.sync);
	for (GList *l = app.windows; l != NULL; l = l->next) {
		struct win *w = l->data;
		apply_ruler(w);
		if (action == ACT_SPELLING) {
			apply_spelling(w, true);
		}
		if (action == ACT_AUTOSAVE || action == ACT_SYNC) {
			schedule_autosave(w); /* turned on with changes: saved now */
		}
		if (action == ACT_SYNC && app.sync) {
			schedule_disk_check(w); /* takes what the info line offered */
		}
		gtk_widget_queue_draw(GTK_WIDGET(w->view));
		update(w);
	}
}

/* ---- Menus ------------------------------------------------------------------- */

static void item(struct app_menu *m, enum action action, const char *label,
		bool enabled, bool checked) {
	char *keys = shortcut_label(action);
	app_menu_add_item(m, action, label, keys,
		(enabled ? 0 : APP_MENU_DISABLED) | (checked ? APP_MENU_CHECKED : 0));
	g_free(keys);
}

static bool style_is(struct win *w, const char *name) {
	return g_strcmp0(wp_editor_current_style_name(w->ed), name) == 0;
}

static void build_menus(struct app_menu *m, void *data) {
	struct win *w = data;
	WpEditor *ed = w->ed;
	bool sel = wp_editor_has_selection(ed);
	bool path = wp_editor_path(ed) != NULL;
	WpTextAttrs attrs = wp_editor_current_attrs(ed);
	WpAlign align = wp_editor_current_align(ed);
	double spacing = round(wp_editor_current_line_spacing(ed) * 100) / 100;
	WpListKind list = wp_editor_current_list_kind(ed);

	app_menu_add_menu(m, "File");
	item(m, ACT_NEW, "New", true, false);
	item(m, ACT_OPEN, "Open...", true, false);
	app_menu_add_separator(m);
	item(m, ACT_SAVE, "Save", true, false);
	item(m, ACT_SAVE_AS, "Save As...", true, false);
	item(m, ACT_RELOAD, "Revert to Saved", path, false);
	item(m, ACT_EXPORT, "Export as Markdown...", true, false);
	item(m, ACT_EXPORT_PDF, "Export as PDF...", true, false);
	app_menu_add_separator(m);
	item(m, ACT_PRINT, "Print...", true, false);
	app_menu_add_separator(m);
	item(m, ACT_CLOSE, "Close", true, false);
	item(m, ACT_QUIT, "Quit", true, false);

	app_menu_add_menu(m, "Edit");
	item(m, ACT_UNDO, "Undo", wp_editor_can_undo(ed), false);
	item(m, ACT_REDO, "Redo", wp_editor_can_redo(ed), false);
	app_menu_add_separator(m);
	item(m, ACT_CUT, "Cut", sel, false);
	item(m, ACT_COPY, "Copy", sel, false);
	item(m, ACT_PASTE, "Paste", can_paste(w), false);
	item(m, ACT_SELECT_ALL, "Select All", true, false);
	app_menu_add_separator(m);
	item(m, ACT_FIND, "Find...", true, false);
	item(m, ACT_FIND_NEXT, "Find Next", true, false);
	item(m, ACT_FIND_PREVIOUS, "Find Previous", true, false);
	item(m, ACT_REPLACE, "Replace...", true, false);
	app_menu_add_separator(m);
	item(m, ACT_COMMENT, "Add Comment", sel, false);

	app_menu_add_menu(m, "Style");
	item(m, ACT_BOLD, "Bold", true, attrs.flags & WP_ATTR_BOLD);
	item(m, ACT_ITALIC, "Italic", true, attrs.flags & WP_ATTR_ITALIC);
	item(m, ACT_UNDERLINE, "Underline", true, attrs.flags & WP_ATTR_UNDERLINE);
	app_menu_add_separator(m);
	item(m, ACT_FONT, "Font...", true, false);
	item(m, ACT_BIGGER, "Larger", true, false);
	item(m, ACT_SMALLER, "Smaller", true, false);
	app_menu_add_separator(m);
	item(m, ACT_BODY, "Body Text", true, style_is(w, WP_STYLE_STANDARD));
	item(m, ACT_TITLE, "Title", true, style_is(w, "Title"));
	item(m, ACT_H1, "Heading 1", true, style_is(w, "Heading 1"));
	item(m, ACT_H2, "Heading 2", true, style_is(w, "Heading 2"));
	item(m, ACT_H3, "Heading 3", true, style_is(w, "Heading 3"));
	item(m, ACT_QUOTE, "Quote", true, style_is(w, "Quote"));
	item(m, ACT_NOTE, "Note", true, style_is(w, "Note"));

	app_menu_add_menu(m, "Format");
	item(m, ACT_LEFT, "Align Left", true, align == WP_ALIGN_LEFT);
	item(m, ACT_CENTER, "Center", true, align == WP_ALIGN_CENTER);
	item(m, ACT_RIGHT, "Align Right", true, align == WP_ALIGN_RIGHT);
	item(m, ACT_JUSTIFY, "Justify", true, align == WP_ALIGN_JUSTIFY);
	app_menu_add_separator(m);
	item(m, ACT_SINGLE, "Single Spacing", true, spacing == 1.0);
	item(m, ACT_115, "1.15 Spacing", true, spacing == 1.15);
	item(m, ACT_ONE_HALF, "1.5 Spacing", true, spacing == 1.5);
	item(m, ACT_DOUBLE, "Double Spacing", true, spacing == 2.0);
	app_menu_add_separator(m);
	item(m, ACT_INDENT, "Indent More", true, false);
	item(m, ACT_OUTDENT, "Indent Less", true, false);
	item(m, ACT_CLEAR_TABS, "Clear Tab Stops", true, false);
	app_menu_add_separator(m);
	item(m, ACT_BULLETS, "Bulleted List", true, list == WP_LIST_BULLET);
	item(m, ACT_NUMBERS, "Numbered List", true, list == WP_LIST_NUMBER);

	app_menu_add_menu(m, "View");
	item(m, ACT_RULER, "Ruler", true, app.ruler);
	item(m, ACT_COMMENTS, "Comments", true, comments_shown(w->comments));
	app_menu_add_separator(m);
	item(m, ACT_ZOOM_IN, "Zoom In", true, false);
	item(m, ACT_ZOOM_OUT, "Zoom Out", true, false);
	item(m, ACT_ZOOM_FIT, "Fit to Window", true,
		wp_page_view_get_fit_width(w->view));

	/* Merged into GemWM's Options, above its own. */
	app_menu_add_menu(m, "Options");
	item(m, ACT_SPELLING, "Check Spelling", true, app.spelling);
	item(m, ACT_AUTOSAVE, "Autosave", true, app.autosave);
	item(m, ACT_SYNC, "Sync with File", true, app.sync);
}

static void menu_activate(uint32_t id, void *data) {
	struct win *w = data;
	if (!held(w)) {
		run(w, id);
	}
}

/* ---- The right-click menu ----------------------------------------------------- */

static void popup_chosen(int id, void *data) {
	struct win *w = data;
	wp_page_view_set_menu_up(w->view, FALSE);
	if (id >= ACT_SUGGESTION && w->suggestions != NULL &&
			id - ACT_SUGGESTION < (int)g_strv_length(w->suggestions)) {
		/* The misspelled word is selected: the correction replaces it. */
		wp_editor_insert_text(w->ed, w->suggestions[id - ACT_SUGGESTION], -1);
	} else if (id > ACT_NONE) {
		run(w, id);
	}
	g_clear_pointer(&w->suggestions, g_strfreev);
}

static void popup_item(struct win *w, enum action action, const char *label,
		bool enabled) {
	char *keys = shortcut_label(action);
	gem_popup_add(w->popup, action, label, keys, enabled ? 0 : GEM_POPUP_DISABLED);
	g_free(keys);
}

/* Corrections first when it's on a misspelled word (the page view has
 * selected it), then the clipboard and comments. */
static void page_menu(WpPageView *v, double x, double y, void *data) {
	struct win *w = data;
	gem_popup_clear(w->popup);
	g_clear_pointer(&w->suggestions, g_strfreev);
	WpPos a, b, ma, mb;
	wp_editor_get_selection(w->ed, &a, &b);
	if (wp_editor_has_selection(w->ed) && wp_editor_misspelled_at(w->ed, a, &ma, &mb) &&
			wp_pos_eq(a, ma) && wp_pos_eq(b, mb)) {
		const WpParagraph *p = wp_document_para(w->doc, a.para);
		char *word = g_strndup(p->text + a.offset, b.offset - a.offset);
		size_t n;
		w->suggestions = wp_spell_suggest(wp_editor_spell(w->ed), word,
			strlen(word), &n);
		for (size_t i = 0; i < n && i < MAX_SUGGESTIONS; i++) {
			gem_popup_add(w->popup, ACT_SUGGESTION + i, w->suggestions[i], NULL, 0);
		}
		if (n == 0) {
			gem_popup_add(w->popup, ACT_NONE, "No Suggestions", NULL,
				GEM_POPUP_DISABLED);
		}
		gem_popup_add_separator(w->popup);
		popup_item(w, ACT_SPELL_ADD, "Add to Dictionary", true);
		popup_item(w, ACT_SPELL_IGNORE, "Ignore Word", true);
		gem_popup_add_separator(w->popup);
		g_free(word);
	}
	bool sel = wp_editor_has_selection(w->ed);
	popup_item(w, ACT_CUT, "Cut", sel);
	popup_item(w, ACT_COPY, "Copy", sel);
	popup_item(w, ACT_PASTE, "Paste", can_paste(w));
	gem_popup_add_separator(w->popup);
	popup_item(w, ACT_SELECT_ALL, "Select All", true);
	gem_popup_add_separator(w->popup);
	popup_item(w, ACT_COMMENT, "Add Comment", sel);
	wp_page_view_set_menu_up(w->view, TRUE);
	gem_popup_show(w->popup, x, y);
}

static void page_comment(WpPageView *v, void *data) {
	run(data, ACT_COMMENT);
}

/* ---- Doing things -------------------------------------------------------------- */

static void zoom_by(struct win *w, double factor) {
	wp_page_view_set_zoom(w->view, wp_page_view_get_zoom(w->view) * factor);
}

static void run(struct win *w, enum action action) {
	const struct binding *b = binding_of(action);
	switch (action) {
	case ACT_NEW: win_new(); return;
	case ACT_OPEN:
		gem_file_select(w->host, "Open", false, NULL, NULL, FILE_PATTERN, opened, w);
		return;
	case ACT_SAVE: save(w); return;
	case ACT_SAVE_AS: save_as(w); return;
	case ACT_EXPORT:
		export_as(w, "Export as Markdown", "export-markdown", ".md");
		return;
	case ACT_EXPORT_PDF:
		export_as(w, "Export as PDF", "export-pdf", ".pdf");
		return;
	case ACT_PRINT: {
		char *name = display_name(w);
		print_document(GTK_WINDOW(w->window), w->ed, name, printed, w);
		g_free(name);
		return;
	}
	case ACT_CLOSE: gtk_window_close(GTK_WINDOW(w->window)); return;
	case ACT_QUIT: {
		GList *all = g_list_copy(app.windows);
		for (GList *l = all; l != NULL; l = l->next) {
			gtk_window_close(GTK_WINDOW(((struct win *)l->data)->window));
		}
		g_list_free(all);
		return;
	}
	case ACT_CUT:
		if (wp_editor_cut(w->ed)) {
			publish_clipboard(w);
		}
		return;
	case ACT_COPY:
		if (wp_editor_copy(w->ed)) {
			publish_clipboard(w);
		}
		return;
	case ACT_PASTE:
		gdk_clipboard_read_text_async(gtk_widget_get_clipboard(w->window), NULL,
			paste_ready, g_object_ref(w->window));
		return;
	case ACT_FIND: find_show(w->find, false); return;
	case ACT_REPLACE: find_show(w->find, true); return;
	case ACT_FIND_NEXT: find_step(w->find, 1); return;
	case ACT_FIND_PREVIOUS: find_step(w->find, -1); return;
	case ACT_COMMENT: comments_add(w->comments); return;
	case ACT_FONT: font_dialog(w->host, w->ed); return;
	case ACT_COMMENTS:
		comments_show(w->comments, !comments_shown(w->comments));
		return;
	case ACT_ZOOM_IN: zoom_by(w, 1.25); return;
	case ACT_ZOOM_OUT: zoom_by(w, 1 / 1.25); return;
	case ACT_ZOOM_FIT: wp_page_view_set_fit_width(w->view); return;
	case ACT_RULER:
	case ACT_SPELLING:
	case ACT_AUTOSAVE:
	case ACT_SYNC:
		set_option(action);
		return;
	default:
		break;
	}
	if (b != NULL && b->command != NULL) {
		const WpCommand *cmd = wp_command_find(b->command);
		GVariant *arg = NULL;
		if (b->arg != NULL) {
			char *err = NULL;
			char *argv[] = { (char *)b->arg, NULL };
			if (!wp_command_parse_args(cmd, argv, &arg, &err)) {
				g_warning("%s: %s", b->command, err);
				free(err);
				return;
			}
		}
		run_command(w, b->command, arg);
	}
}

/* ---- Keys ----------------------------------------------------------------------- */

static gboolean shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
	struct win *w = g_object_get_data(G_OBJECT(widget), "gw-win");
	if (w == NULL || held(w)) {
		return FALSE;
	}
	run(w, GPOINTER_TO_INT(data));
	return TRUE;
}

static void add_shortcuts(GtkWidget *window) {
	GtkEventController *c = gtk_shortcut_controller_new();
	for (size_t i = 0; i < G_N_ELEMENTS(bindings); i++) {
		for (int alt = 0; alt < 2; alt++) {
			const char *accel = accel_of(&bindings[i], alt);
			GtkShortcutTrigger *trigger = accel != NULL ?
				gtk_shortcut_trigger_parse_string(accel) : NULL;
			if (trigger == NULL) {
				continue;
			}
			gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(c),
				gtk_shortcut_new(trigger, gtk_callback_action_new(shortcut,
					GINT_TO_POINTER(bindings[i].action), NULL)));
		}
	}
	gtk_widget_add_controller(window, c);
}

/* ---- Keeping up ---------------------------------------------------------------- */

static void update(struct win *w) {
	char *name = display_name(w);
	char *title = wp_editor_modified(w->ed) ?
		g_strdup_printf("%s *", name) : g_strdup(name);
	gtk_window_set_title(GTK_WINDOW(w->window), title);
	g_free(title);
	g_free(name);
	gtk_widget_queue_draw(w->info);
	app_menu_update(w->menu);
}

static void view_state_changed(WpPageView *view, gpointer data) {
	struct win *w = data;
	watch_file(w);
	comments_caret_moved(w->comments);
	find_update(w->find);
	update(w);
}

static void view_document_changed(WpPageView *view, gpointer data) {
	struct win *w = data;
	schedule_autosave(w);
	comments_refresh(w->comments);
	update(w);
}

static void clipboard_changed(GdkClipboard *cb, gpointer window) {
	struct win *w = g_object_get_data(G_OBJECT(window), "gw-win");
	if (w != NULL) {
		app_menu_update(w->menu);
	}
}

static void comments_toggled(void *data) {
	app_menu_update(((struct win *)data)->menu);
}

/* The sideways bar only while the page is wider than the window. */
static void hadj_changed(GtkAdjustment *adj, gpointer data) {
	struct win *w = data;
	gtk_widget_set_visible(w->hbar, gtk_adjustment_get_upper(adj) -
		gtk_adjustment_get_lower(adj) > gtk_adjustment_get_page_size(adj) + 0.5);
}

/* ---- Closing -------------------------------------------------------------------- */

static void close_answered(int button, void *data) {
	struct win *w = data;
	switch (button) {
	case 0: /* Don't Save */
		w->closing = true;
		gtk_window_close(GTK_WINDOW(w->window));
		break;
	case 2: /* Save */
		w->close_after_save = true;
		save(w);
		break;
	}
}

static gboolean close_request(GtkWindow *window, gpointer data) {
	struct win *w = data;
	if (w->closing || !wp_editor_modified(w->ed)) {
		return FALSE;
	}
	if (held(w)) {
		return TRUE;
	}
	/* With Autosave on it's as good as saved. */
	if (app.autosave && wp_editor_path(w->ed) != NULL &&
			wp_editor_save(w->ed, NULL, NULL)) {
		return FALSE;
	}
	char *name = display_name(w);
	char *text = g_strdup_printf("Save the changes to \"%s\"\nbefore closing?",
		name);
	static const char *const buttons[] = { "Don't Save", "Cancel", "Save", NULL };
	gem_alert(w->host, GEM_ALERT_QUESTION, text, buttons, 2, 1, close_answered, w);
	g_free(text);
	g_free(name);
	return TRUE;
}

/* Once the window and everything in it has gone. */
static void win_free(gpointer data, GObject *where_the_window_was) {
	struct win *w = data;
	app.windows = g_list_remove(app.windows, w);
	if (w->autosave_id != 0) {
		g_source_remove(w->autosave_id);
		if (wp_editor_modified(w->ed) && wp_editor_path(w->ed) != NULL) {
			wp_editor_save(w->ed, NULL, NULL);
		}
	}
	g_clear_handle_id(&w->disk_check_id, g_source_remove);
	g_clear_handle_id(&w->message_id, g_source_remove);
	g_clear_object(&w->monitor);
	g_free(w->monitor_path);
	g_free(w->message);
	g_strfreev(w->suggestions);
	g_array_unref(w->info_hits);
	wp_editor_free(w->ed);
	g_free(w);
}

/* ---- The window ------------------------------------------------------------------ */

static struct win *win_new(void) {
	struct win *w = g_new0(struct win, 1);
	w->info_hits = gem_hits_new();
	w->ed = wp_editor_new(wp_layout_pango_new());
	w->doc = wp_editor_document(w->ed);
	w->view = wp_page_view_new(w->ed);
	wp_page_view_set_handlers(w->view, page_menu, page_comment, w);

	w->window = gtk_application_window_new(app.app);
	gtk_window_set_default_size(GTK_WINDOW(w->window), 900, 1000);
	gtk_widget_add_css_class(w->window, "gem");
	g_object_set_data(G_OBJECT(w->window), "gw-win", w);

	/* The page in its scroller, GEM's bars beside and below it; the find
	 * box goes over its corner. */
	w->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w->scroller),
		GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w->scroller),
		GTK_WIDGET(w->view));
	gtk_widget_set_hexpand(w->scroller, TRUE);
	gtk_widget_set_vexpand(w->scroller, TRUE);
	GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(w->scroller));
	GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(w->scroller));
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_attach(GTK_GRID(grid), w->scroller, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid),
		gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadj), 1, 0, 1, 1);
	w->hbar = gem_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, hadj);
	gtk_widget_set_visible(w->hbar, FALSE);
	gtk_grid_attach(GTK_GRID(grid), w->hbar, 0, 1, 1, 1);
	g_signal_connect(hadj, "changed", G_CALLBACK(hadj_changed), w);
	GtkWidget *page_area = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(page_area), grid);
	w->find = find_new(GTK_OVERLAY(page_area), w->view);

	/* The ruler over the page, not the comments. */
	w->ruler = ruler_new(w->view);
	GtkWidget *page_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(page_column), w->ruler);
	gtk_box_append(GTK_BOX(page_column), page_area);
	gtk_widget_set_vexpand(page_area, TRUE);

	w->comments = comments_new(w->view, comments_run, comments_toggled, w);
	GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(middle, TRUE);
	gtk_box_append(GTK_BOX(middle), page_column);
	gtk_box_append(GTK_BOX(middle), comments_widget(w->comments));

	w->info = gem_pixel_area_new(0, INFO_H, paint_info, w);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(info_pressed), w);
	gtk_widget_add_controller(w->info, GTK_EVENT_CONTROLLER(click));

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(outer), middle);
	gtk_box_append(GTK_BOX(outer), w->info);
	w->host = GTK_OVERLAY(gtk_overlay_new());
	gtk_overlay_set_child(w->host, outer);
	gtk_window_set_child(GTK_WINDOW(w->window), GTK_WIDGET(w->host));

	w->popup = gem_popup_new(GTK_WIDGET(w->view), popup_chosen, w);
	g_signal_connect(w->view, "state-changed", G_CALLBACK(view_state_changed), w);
	g_signal_connect(w->view, "document-changed",
		G_CALLBACK(view_document_changed), w);
	g_signal_connect_object(gtk_widget_get_clipboard(w->window), "changed",
		G_CALLBACK(clipboard_changed), w->window, 0);
	g_signal_connect(w->window, "close-request", G_CALLBACK(close_request), w);
	g_object_weak_ref(G_OBJECT(w->window), win_free, w);
	add_shortcuts(w->window);
	w->menu = app_menu_new(w->window, build_menus, menu_activate, w);
	app.windows = g_list_append(app.windows, w);

	apply_ruler(w);
	apply_spelling(w, false);
	update(w);
	gtk_window_present(GTK_WINDOW(w->window));
	focus_page(w);
	return w;
}

/* ---- The application -------------------------------------------------------------- */

static void load_css(void) {
	gem_ui_load_css();
	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider,
		".gw-comments { background: #fff; }"
		".gw-card { background: #fff; border: 1px solid #000; padding: 4px 6px; }"
		".gw-card.gw-active { border-width: 2px; padding: 3px 5px; }"
		"textview.gw-edit { border: 1px solid #000; }");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
	g_object_unref(provider);
}

static void startup(GApplication *application, gpointer data) {
	load_css();
	app.sync = wp_settings_get_bool("sync", false);
	app.autosave = app.sync || wp_settings_get_bool("autosave", false);
	app.spelling = wp_settings_get_bool("spellcheck", true);
	app.ruler = wp_settings_get_bool("ruler", true);
}

static void activate(GApplication *application, gpointer data) {
	GtkWindow *active = gtk_application_get_active_window(app.app);
	if (active != NULL) {
		gtk_window_present(active);
	} else {
		win_new();
	}
}

static void open_files(GApplication *application, GFile **files, int n,
		const char *hint, gpointer data) {
	for (int i = 0; i < n; i++) {
		char *path = g_file_get_path(files[i]);
		if (path != NULL) {
			GtkWindow *active = gtk_application_get_active_window(app.app);
			struct win *w = active != NULL ?
				g_object_get_data(G_OBJECT(active), "gw-win") : NULL;
			open_path(w, path);
			g_free(path);
		}
	}
}

int main(int argc, char *argv[]) {
	gem_print_setup();
	g_set_application_name("GemWrite");
	app.app = gtk_application_new("org.gemwm.GemWrite", G_APPLICATION_HANDLES_OPEN);
	g_application_set_option_context_parameter_string(G_APPLICATION(app.app),
		"[FILE...]");
	g_application_set_option_context_summary(G_APPLICATION(app.app),
		"Open each FILE (.odt or .txt), or start with an empty document.");
	g_application_set_option_context_description(G_APPLICATION(app.app),
		"To edit documents from a script or another program, use gemwrite-cli.");
	g_signal_connect(app.app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app.app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app.app, "open", G_CALLBACK(open_files), NULL);
	int status = g_application_run(G_APPLICATION(app.app), argc, argv);
	g_object_unref(app.app);
	return status;
}
