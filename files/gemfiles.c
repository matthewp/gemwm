/*
 * GemFiles: folders in windows, as GEM's desktop showed them. Each window
 * is one folder, its things as icons (a folder, a document, a picture, a
 * program) with their names under them, or as text, a line each; double-
 * click a folder and the window shows it, and the close box goes back up,
 * a folder at a time, as GEM's did, until the window's at the top of
 * where it is (home, a disk, /), where it closes.
 *
 * Things are picked by clicking (Shift for a run, Ctrl for one more) or by
 * dragging a box round them, and dragged to another window or folder to
 * move them there (Ctrl: copy; across disks, always a copy). Cut, Copy and
 * Paste do the same through the clipboard, which other programs share.
 * Move to Trash puts them in the freedesktop Trash, where Go > Trash shows
 * them, to put back or delete for good. Copies and moves happen in a
 * thread, their progress on the info line.
 *
 * Show as Icons or Text, the sort, and hidden files are one setting for
 * every window, kept in ~/.local/state/gemwm/files.
 */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include "app-menu.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-popup.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define CELL_W 100      /* an icon's place in the grid */
#define CELL_H 76
#define ICON 32
#define LABEL_H 16      /* a line of an icon's name */
#define INFO_H (GEM_ROW_H + 1)
#define TYPE_PAUSE 1000 /* ms between keys before typing starts afresh */

#define ATTRS "standard::name,standard::display-name,standard::type," \
	"standard::size,standard::is-hidden,standard::is-backup," \
	"standard::is-symlink,standard::fast-content-type,time::modified," \
	"access::can-execute"

enum view_mode { VIEW_ICONS, VIEW_TEXT };
enum sort_key { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE };
enum kind { KIND_FOLDER, KIND_DOCUMENT, KIND_PICTURE, KIND_PROGRAM };

struct item {
	char *name;          /* on disk */
	char *display;       /* as shown */
	char *key;           /* for sorting by name */
	char *type;          /* its content type */
	enum kind kind;
	bool dir, link, selected;
	goffset size;
	gint64 mtime;
};

struct win {
	GtkWidget *window, *area, *info;
	GtkOverlay *host;
	GtkAdjustment *vadj;
	struct app_menu *menu;
	struct gem_popup *popup;
	GFile *dir;
	GFileMonitor *monitor;
	guint reload_id;
	GArray *items;            /* struct item, sorted */
	int cursor, anchor;       /* the keyboard's place, and a run's start */
	int cols;                 /* icons across, as last laid out */
	char *message;            /* on the info line for a while */
	guint message_id;
	char **select_after;      /* names to pick once they're read */

	/* The pointer: a click on something already picked picks it alone
	 * when let go, unless it was dragged; a box dragged round things. */
	int pending_single;
	bool band;
	double bx0, by0, bx1, by1;   /* in the content, scroll and all */
	bool *band_base;             /* what was picked before, with Ctrl */
	int drop_hover;              /* the folder a drag is over, or -1 */
	GdkDragAction drop_action;
	char typed[64];
	gint64 typed_at;
};

/* A copy, move, trash or delete, run in a thread. */
enum op_kind { OP_COPY, OP_MOVE, OP_TRASH, OP_DELETE, OP_RESTORE, OP_EMPTY };

struct pair {
	GFile *src, *dst;
};

struct op {
	enum op_kind kind;
	GArray *pairs;            /* struct pair; dst NULL but for copy, move, put back */
	bool replace;
	GWeakRef window;          /* where it started, for its errors */
	GFile *dest_dir;          /* copy and move: picked there after */
	GMutex lock;
	char *status;             /* "Copying 2 of 5: name" */
	GPtrArray *errors;        /* "name: why" */
};

static struct {
	GtkApplication *app;
	GList *wins;              /* struct win */
	enum view_mode view;
	enum sort_key sort;
	bool hidden;
	GList *ops;               /* struct op, running */
	guint op_timer;
	bool cut;                 /* what we put on the clipboard is to move */
	struct win *dragging;     /* the window a drag of ours is from */
	GVolumeMonitor *volumes;
	char **places;            /* Go's places, kept up to date */
} app;

static struct win *win_new(GFile *dir);
static void fit_scroll(struct win *w);
static void scroll_to_item(struct win *w, int i);
static void run(struct win *w, int id);
static void start_op(struct win *w, struct op *op);

/* ---- Small things ---------------------------------------------------------- */

static void redraw(struct win *w) {
	gtk_widget_queue_draw(w->area);
	gtk_widget_queue_draw(w->info);
}

static void update_menus(struct win *w) {
	app_menu_update(w->menu);
}

static void each_win(void (*fn)(struct win *)) {
	for (GList *l = app.wins; l != NULL; l = l->next) {
		fn(l->data);
	}
}

static struct item *item_at_index(struct win *w, int i) {
	return &g_array_index(w->items, struct item, i);
}

static int n_items(struct win *w) {
	return (int)w->items->len;
}

static int n_selected(struct win *w) {
	int n = 0;
	for (int i = 0; i < n_items(w); i++) {
		n += item_at_index(w, i)->selected;
	}
	return n;
}

static void select_none(struct win *w) {
	for (int i = 0; i < n_items(w); i++) {
		item_at_index(w, i)->selected = false;
	}
}

static int first_selected(struct win *w) {
	for (int i = 0; i < n_items(w); i++) {
		if (item_at_index(w, i)->selected) {
			return i;
		}
	}
	return -1;
}

static GFile *child(struct win *w, int i) {
	return g_file_get_child(w->dir, item_at_index(w, i)->name);
}

/* The selected things, as GFiles. */
static GList *selected_files(struct win *w) {
	GList *files = NULL;
	for (int i = n_items(w) - 1; i >= 0; i--) {
		if (item_at_index(w, i)->selected) {
			files = g_list_prepend(files, child(w, i));
		}
	}
	return files;
}

/* A path as it's shown: home as ~. */
static char *shown_path(GFile *f) {
	char *path = g_file_get_path(f);
	if (path == NULL) {
		return g_file_get_uri(f);
	}
	const char *home = g_get_home_dir();
	size_t n = strlen(home);
	if (strncmp(path, home, n) == 0 && (path[n] == '\0' || path[n] == '/')) {
		char *s = g_strconcat("~", path + n, NULL);
		g_free(path);
		return s;
	}
	return path;
}

static char *trash_dir(const char *part) {
	return g_build_filename(g_get_user_data_dir(), "Trash", part, NULL);
}

static bool in_trash(struct win *w) {
	char *files = trash_dir("files");
	char *path = g_file_get_path(w->dir);
	bool yes = g_strcmp0(files, path) == 0;
	g_free(files);
	g_free(path);
	return yes;
}

static bool trash_empty(void) {
	char *files = trash_dir("files");
	GDir *d = g_dir_open(files, 0, NULL);
	bool empty = d == NULL || g_dir_read_name(d) == NULL;
	if (d != NULL) {
		g_dir_close(d);
	}
	g_free(files);
	return empty;
}

static gboolean message_gone(gpointer data) {
	struct win *w = data;
	w->message_id = 0;
	g_clear_pointer(&w->message, g_free);
	gtk_widget_queue_draw(w->info);
	return G_SOURCE_REMOVE;
}

/* On the info line for a few seconds. */
static void say(struct win *w, char *message) {
	g_free(w->message);
	w->message = message;
	if (w->message_id != 0) {
		g_source_remove(w->message_id);
	}
	w->message_id = g_timeout_add(4000, message_gone, w);
	gtk_widget_queue_draw(w->info);
}

static void alert(struct win *w, enum gem_alert_icon icon, const char *text) {
	static const char *const ok[] = { "OK", NULL };
	gem_alert(w->host, icon, text, ok, 0, 0, NULL, NULL);
}

/* ---- Settings ---------------------------------------------------------------- */

static char *settings_path(void) {
	return g_build_filename(g_get_user_state_dir(), "gemwm", "files", NULL);
}

static void settings_load(void) {
	GKeyFile *kf = g_key_file_new();
	char *path = settings_path();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		char *view = g_key_file_get_string(kf, "files", "view", NULL);
		char *sort = g_key_file_get_string(kf, "files", "sort", NULL);
		app.view = g_strcmp0(view, "text") == 0 ? VIEW_TEXT : VIEW_ICONS;
		app.sort = g_strcmp0(sort, "date") == 0 ? SORT_DATE :
			g_strcmp0(sort, "size") == 0 ? SORT_SIZE :
			g_strcmp0(sort, "type") == 0 ? SORT_TYPE : SORT_NAME;
		app.hidden = g_key_file_get_boolean(kf, "files", "hidden", NULL);
		g_free(view);
		g_free(sort);
	}
	g_free(path);
	g_key_file_unref(kf);
}

static void settings_save(void) {
	static const char *const sorts[] = { "name", "date", "size", "type" };
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_string(kf, "files", "view",
		app.view == VIEW_TEXT ? "text" : "icons");
	g_key_file_set_string(kf, "files", "sort", sorts[app.sort]);
	g_key_file_set_boolean(kf, "files", "hidden", app.hidden);
	char *path = settings_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_key_file_save_to_file(kf, path, NULL);
	g_free(dir);
	g_free(path);
	g_key_file_unref(kf);
}

/* ---- Reading a folder -------------------------------------------------------- */

static void item_clear(gpointer p) {
	struct item *it = p;
	g_free(it->name);
	g_free(it->display);
	g_free(it->key);
	g_free(it->type);
}

static enum kind kind_of(GFileInfo *info, const char *type) {
	if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
		return KIND_FOLDER;
	}
	if (type != NULL && g_str_has_prefix(type, "image/")) {
		return KIND_PICTURE;
	}
	/* Executable, and of a kind that can be: a script, or (the type's
	 * only guessed from the name) one with no name to go on, as programs
	 * mostly are. Not any text file, though: on a FAT stick everything's
	 * executable. */
	if (g_file_info_get_attribute_boolean(info,
			G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)) {
		if (type == NULL || g_content_type_is_unknown(type)) {
			return KIND_PROGRAM;
		}
		if (g_content_type_is_a(type, "text/plain")) {
			const char *scripts[] = { "shellscript", "python", "perl", "ruby",
				"lua", "tcl", NULL };
			for (int i = 0; scripts[i] != NULL; i++) {
				if (strstr(type, scripts[i]) != NULL) {
					return KIND_PROGRAM;
				}
			}
		} else if (g_content_type_can_be_executable(type)) {
			return KIND_PROGRAM;
		}
	}
	return KIND_DOCUMENT;
}

/* Folders first, then by the sort. */
static int compare(const void *a, const void *b) {
	const struct item *x = a, *y = b;
	if (x->dir != y->dir) {
		return x->dir ? -1 : 1;
	}
	int c = 0;
	switch (app.sort) {
	case SORT_DATE:
		c = x->mtime > y->mtime ? -1 : x->mtime < y->mtime; /* newest first */
		break;
	case SORT_SIZE:
		c = x->size > y->size ? -1 : x->size < y->size; /* biggest first */
		break;
	case SORT_TYPE:
		c = g_strcmp0(x->type, y->type);
		break;
	case SORT_NAME:
		break;
	}
	return c != 0 ? c : strcmp(x->key, y->key);
}

static void sort_items(struct win *w) {
	g_array_sort(w->items, compare);
}

static void reload_soon(struct win *w);

static void dir_changed(GFileMonitor *m, GFile *f, GFile *other,
		GFileMonitorEvent event, gpointer data) {
	reload_soon(data);
}

static void watch(struct win *w) {
	if (w->monitor != NULL) {
		g_signal_handlers_disconnect_by_data(w->monitor, w);
		g_file_monitor_cancel(w->monitor);
		g_clear_object(&w->monitor);
	}
	w->monitor = g_file_monitor_directory(w->dir, G_FILE_MONITOR_WATCH_MOVES,
		NULL, NULL);
	if (w->monitor != NULL) {
		g_file_monitor_set_rate_limit(w->monitor, 200);
		g_signal_connect(w->monitor, "changed", G_CALLBACK(dir_changed), w);
	}
}

static void set_title(struct win *w) {
	char *title;
	if (in_trash(w)) {
		title = g_strdup("Trash");
	} else {
		char *shown = shown_path(w->dir);
		title = g_str_has_suffix(shown, "/") ? g_strdup(shown) :
			g_strconcat(shown, "/", NULL);
		g_free(shown);
	}
	gtk_window_set_title(GTK_WINDOW(w->window), title);
	g_free(title);
}

/* The folder read into the window. keep: it's the same folder again, so
 * what was picked, where the keyboard was and the scroll stay. Fails, with
 * an alert, if it can't be read. */
static bool read_dir(struct win *w, GFile *dir, bool keep) {
	GError *error = NULL;
	GFileEnumerator *e = g_file_enumerate_children(dir, ATTRS,
		G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (e == NULL) {
		char *shown = shown_path(dir);
		char *text = g_strdup_printf("Can't open %s:\n%s", shown, error->message);
		alert(w, GEM_ALERT_STOP, text);
		g_free(text);
		g_free(shown);
		g_error_free(error);
		return false;
	}
	GHashTable *picked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		NULL);
	char *cursor_name = NULL;
	if (keep) {
		for (int i = 0; i < n_items(w); i++) {
			struct item *it = item_at_index(w, i);
			if (it->selected) {
				g_hash_table_add(picked, g_strdup(it->name));
			}
		}
		if (w->cursor >= 0 && w->cursor < n_items(w)) {
			cursor_name = g_strdup(item_at_index(w, w->cursor)->name);
		}
	}
	for (int i = 0; w->select_after != NULL && w->select_after[i] != NULL; i++) {
		g_hash_table_add(picked, g_strdup(w->select_after[i]));
	}
	bool picking_new = w->select_after != NULL;
	g_clear_pointer(&w->select_after, g_strfreev);

	g_array_set_size(w->items, 0);
	GFileInfo *info;
	while ((info = g_file_enumerator_next_file(e, NULL, NULL)) != NULL) {
		if (!app.hidden && (g_file_info_get_is_hidden(info) ||
				g_file_info_get_is_backup(info))) {
			g_object_unref(info);
			continue;
		}
		struct item it = { 0 };
		it.name = g_strdup(g_file_info_get_name(info));
		it.display = g_strdup(g_file_info_get_display_name(info));
		it.key = g_utf8_collate_key_for_filename(it.display, -1);
		const char *type = g_file_info_get_attribute_string(info,
			G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
		it.type = g_strdup(type);
		it.dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
		it.link = g_file_info_get_is_symlink(info);
		it.kind = kind_of(info, type);
		it.size = g_file_info_get_size(info);
		GDateTime *t = g_file_info_get_modification_date_time(info);
		it.mtime = t != NULL ? g_date_time_to_unix(t) : 0;
		if (t != NULL) {
			g_date_time_unref(t);
		}
		it.selected = g_hash_table_contains(picked, it.name);
		g_array_append_val(w->items, it);
		g_object_unref(info);
	}
	g_object_unref(e);
	g_hash_table_unref(picked);

	if (!keep || !g_file_equal(dir, w->dir)) {
		g_set_object(&w->dir, dir);
		watch(w);
		set_title(w);
	}
	sort_items(w);
	w->cursor = w->anchor = -1;
	for (int i = 0; i < n_items(w); i++) {
		struct item *it = item_at_index(w, i);
		if (cursor_name != NULL ? strcmp(it->name, cursor_name) == 0 :
				it->selected && w->cursor < 0) {
			w->cursor = w->anchor = i;
		}
	}
	g_free(cursor_name);
	if (!keep) {
		gtk_adjustment_set_value(w->vadj, 0);
	}
	fit_scroll(w);
	if (picking_new) {
		scroll_to_item(w, w->cursor);
	}
	update_menus(w);
	redraw(w);
	return true;
}

static gboolean reload_now(gpointer data) {
	struct win *w = data;
	w->reload_id = 0;
	if (g_file_query_exists(w->dir, NULL)) {
		read_dir(w, w->dir, true);
		return G_SOURCE_REMOVE;
	}
	/* Gone (deleted, or its disk taken out): the nearest that's still
	 * there. */
	GFile *up = g_file_get_parent(w->dir);
	while (up != NULL && !g_file_query_exists(up, NULL)) {
		GFile *next = g_file_get_parent(up);
		g_object_unref(up);
		up = next;
	}
	if (up == NULL) {
		up = g_file_new_for_path(g_get_home_dir());
	}
	read_dir(w, up, false);
	g_object_unref(up);
	return G_SOURCE_REMOVE;
}

static void reload_soon(struct win *w) {
	if (w->reload_id == 0) {
		w->reload_id = g_timeout_add(150, reload_now, w);
	}
}

/* The top of where a folder is, where the close box closes the window:
 * the disk it's on (if it's mounted), else home if it's in home, else /. */
static GFile *top_of(GFile *dir) {
	GMount *mount = g_file_find_enclosing_mount(dir, NULL, NULL);
	if (mount != NULL) {
		GFile *root = g_mount_get_root(mount);
		g_object_unref(mount);
		return root;
	}
	GFile *home = g_file_new_for_path(g_get_home_dir());
	if (g_file_equal(dir, home) || g_file_has_prefix(dir, home)) {
		return home;
	}
	g_object_unref(home);
	return g_file_new_for_path("/");
}

static bool at_top(struct win *w) {
	GFile *top = top_of(w->dir);
	bool yes = g_file_equal(top, w->dir) || in_trash(w);
	g_object_unref(top);
	return yes;
}

/* Up a folder, the one left picked. The close box stops at the top (of
 * home, a disk): false there. */
static bool go_up(struct win *w, bool stop_at_top) {
	GFile *parent = (stop_at_top && at_top(w)) || in_trash(w) ? NULL :
		g_file_get_parent(w->dir);
	if (parent == NULL) {
		return false;
	}
	char *name = g_file_get_basename(w->dir);
	char *names[] = { name, NULL };
	w->select_after = g_strdupv(names);
	g_free(name);
	read_dir(w, parent, false);
	g_object_unref(parent);
	return true;
}

/* ---- Laying out --------------------------------------------------------------- */

static int area_w(struct win *w) {
	return gtk_widget_get_width(w->area);
}

static void lay_out(struct win *w) {
	w->cols = MAX(1, (area_w(w) - GEM_PAD) / CELL_W);
}

static int content_h(struct win *w) {
	if (app.view == VIEW_TEXT) {
		return 4 + n_items(w) * GEM_ROW_H;
	}
	int rows = (n_items(w) + w->cols - 1) / w->cols;
	return GEM_PAD + rows * CELL_H;
}

struct box {
	int x, y, w, h;
};

static void cell_of(struct win *w, int i, struct box *b) {
	if (app.view == VIEW_TEXT) {
		b->x = 0;
		b->y = 2 + i * GEM_ROW_H;
		b->w = area_w(w);
		b->h = GEM_ROW_H;
	} else {
		b->x = GEM_PAD / 2 + (i % w->cols) * CELL_W;
		b->y = GEM_PAD + (i / w->cols) * CELL_H;
		b->w = CELL_W;
		b->h = CELL_H;
	}
}

/* The part of an item's place a click or a box picks it by. */
static void hit_box(struct win *w, int i, struct box *b) {
	cell_of(w, i, b);
	if (app.view == VIEW_ICONS) {
		b->x += 6;
		b->w -= 12;
		b->h -= 6;
	}
}

static double scroll_y(struct win *w) {
	return gtk_adjustment_get_value(w->vadj);
}

/* The item at x, y in the window, or -1. */
static int item_at(struct win *w, double x, double y) {
	lay_out(w);
	double cy = y + scroll_y(w);
	for (int i = 0; i < n_items(w); i++) {
		struct box b;
		hit_box(w, i, &b);
		if (x >= b.x && x < b.x + b.w && cy >= b.y && cy < b.y + b.h) {
			return i;
		}
	}
	return -1;
}

static void fit_scroll(struct win *w) {
	int h = gtk_widget_get_height(w->area);
	int total = MAX(content_h(w), h);
	int step = app.view == VIEW_TEXT ? GEM_ROW_H : CELL_H / 2;
	gtk_adjustment_configure(w->vadj,
		CLAMP(scroll_y(w), 0, MAX(total - h, 0)), 0, total, step,
		MAX(h - step, step), h);
}

/* The keyboard's place, shown. */
static void scroll_to_item(struct win *w, int i) {
	if (i < 0) {
		return;
	}
	lay_out(w);
	struct box b;
	cell_of(w, i, &b);
	int h = gtk_widget_get_height(w->area);
	double top = scroll_y(w);
	if (b.y < top) {
		gtk_adjustment_set_value(w->vadj, b.y - (app.view == VIEW_ICONS ? GEM_PAD : 2));
	} else if (b.y + b.h > top + h) {
		gtk_adjustment_set_value(w->vadj, b.y + b.h - h + 2);
	}
}

/* ---- Icons ---------------------------------------------------------------------- */

/* Drawn in fg on bg: black on white, or the other way round when picked. */
struct ink {
	double fg, bg;
};

static void ink(cairo_t *cr, double v) {
	cairo_set_source_rgb(cr, v, v, v);
}

static void dot(cairo_t *cr, int x, int y) {
	cairo_rectangle(cr, x, y, 1, 1);
	cairo_fill(cr);
}

/* A box, its inside bg and its edge fg. */
static void box(cairo_t *cr, struct ink k, int x, int y, int w, int h) {
	ink(cr, k.bg);
	gem_fill(cr, x, y, w, h);
	ink(cr, k.fg);
	gem_frame(cr, x, y, w, h, 1);
}

static void icon_folder(cairo_t *cr, struct ink k, int x, int y) {
	box(cr, k, x + 1, y + 5, 13, 5);         /* the tab */
	box(cr, k, x, y + 9, 32, 21);
	ink(cr, k.fg);
	gem_fill(cr, x + 1, y + 11, 30, 1);
}

/* A page with its corner turned down; lines is what's on it. */
static void page(cairo_t *cr, struct ink k, int x, int y, int w, int h) {
	const int c = 7;
	ink(cr, k.bg);
	for (int r = 0; r < h; r++) {
		int cut = r < c ? c - r : 0;
		gem_fill(cr, x, y + r, w - cut, 1);
	}
	ink(cr, k.fg);
	gem_fill(cr, x, y, w - c, 1);                 /* top */
	gem_fill(cr, x, y, 1, h);                     /* left */
	gem_fill(cr, x, y + h - 1, w, 1);             /* bottom */
	gem_fill(cr, x + w - 1, y + c, 1, h - c);     /* right */
	for (int i = 0; i <= c; i++) {
		dot(cr, x + w - c + i - 1, y + i);         /* the fold's edge */
	}
	gem_fill(cr, x + w - c - 1, y, 1, c + 1);      /* the fold */
	gem_fill(cr, x + w - c - 1, y + c, c + 1, 1);
}

static void icon_document(cairo_t *cr, struct ink k, int x, int y) {
	page(cr, k, x + 5, y + 1, 22, 30);
	ink(cr, k.fg);
	for (int r = 0; r < 5; r++) {
		gem_fill(cr, x + 9, y + 12 + r * 4, r == 4 ? 8 : 14, 1);
	}
}

static void icon_picture(cairo_t *cr, struct ink k, int x, int y) {
	page(cr, k, x + 5, y + 1, 22, 30);
	ink(cr, k.fg);
	gem_frame(cr, x + 8, y + 11, 16, 15, 1);
	gem_fill(cr, x + 19, y + 14, 3, 3);          /* the sun */
	for (int r = 0; r < 7; r++) {                /* the hill */
		gem_fill(cr, x + 14 - r, y + 18 + r, 2 * r + 1, 1);
	}
}

/* GEM drew a program as a little window. */
static void icon_program(cairo_t *cr, struct ink k, int x, int y) {
	box(cr, k, x + 1, y + 4, 30, 24);
	ink(cr, k.fg);
	gem_fill(cr, x + 1, y + 4, 30, 6);
	ink(cr, k.bg);
	gem_fill(cr, x + 3, y + 6, 3, 2);            /* its close box */
	ink(cr, k.fg);
	for (int r = 0; r < 3; r++) {
		gem_fill(cr, x + 5, y + 14 + r * 4, 16, 1);
	}
}

/* A link: a little arrow at the bottom left. */
static void link_mark(cairo_t *cr, struct ink k, int x, int y) {
	box(cr, k, x, y + 22, 10, 10);
	ink(cr, k.fg);
	gem_fill(cr, x + 3, y + 25, 4, 1);
	gem_fill(cr, x + 6, y + 25, 1, 4);
	for (int i = 0; i < 3; i++) {
		dot(cr, x + 5 - i, y + 27 - i);
	}
}

static void paint_icon(cairo_t *cr, const struct item *it, int x, int y,
		bool inverted) {
	struct ink k = inverted ? (struct ink){ 1, 0 } : (struct ink){ 0, 1 };
	switch (it->kind) {
	case KIND_FOLDER: icon_folder(cr, k, x, y); break;
	case KIND_PICTURE: icon_picture(cr, k, x, y); break;
	case KIND_PROGRAM: icon_program(cr, k, x, y); break;
	case KIND_DOCUMENT: icon_document(cr, k, x, y); break;
	}
	if (it->link) {
		link_mark(cr, k, x, y);
	}
}

/* A name in at most two lines of width: broken after a space, dash, dot
 * or underscore if there's one, and the second shortened with ".." if it
 * still doesn't fit. */
static void label_lines(const char *name, int width, char **one, char **two) {
	glong n = g_utf8_strlen(name, -1);
	glong fit = 0;
	for (glong i = 1; i <= n; i++) {
		char *head = g_utf8_substring(name, 0, i);
		bool ok = gem_measure(head) <= width;
		g_free(head);
		if (!ok) {
			break;
		}
		fit = i;
	}
	if (fit >= n) {
		*one = g_strdup(name);
		*two = NULL;
		return;
	}
	glong cut = fit;
	for (glong i = fit; i > 1; i--) {
		gunichar c = g_utf8_get_char(g_utf8_offset_to_pointer(name, i - 1));
		if (c == ' ' || c == '-' || c == '_' || c == '.') {
			cut = i;
			break;
		}
	}
	*one = g_utf8_substring(name, 0, cut);
	char *rest = g_utf8_substring(name, cut, n);
	if (gem_measure(rest) <= width) {
		*two = rest;
		return;
	}
	glong rn = g_utf8_strlen(rest, -1);
	for (glong i = rn; i > 0; i--) {
		char *head = g_utf8_substring(rest, 0, i);
		char *s = g_strconcat(head, "..", NULL);
		g_free(head);
		if (gem_measure(s) <= width) {
			*two = s;
			g_free(rest);
			return;
		}
		g_free(s);
	}
	*two = g_strdup("..");
	g_free(rest);
}

/* ---- Drawing ------------------------------------------------------------------ */

static void paint_item_icons(cairo_t *cr, struct win *w, int i, int y0) {
	struct item *it = item_at_index(w, i);
	struct box c;
	cell_of(w, i, &c);
	int y = c.y - y0;
	bool inv = it->selected || i == w->drop_hover;
	int ix = c.x + (CELL_W - ICON) / 2;
	paint_icon(cr, it, ix, y + 4, inv);
	char *one, *two;
	label_lines(it->display, CELL_W - 6, &one, &two);
	int ly = y + 4 + ICON + 3;
	for (int l = 0; l < 2; l++) {
		const char *s = l == 0 ? one : two;
		if (s == NULL) {
			break;
		}
		int tw = (int)gem_text_width(cr, s);
		int tx = c.x + (CELL_W - tw) / 2;
		if (inv) {
			gem_black(cr);
			gem_fill(cr, tx - 2, ly + l * LABEL_H, tw + 4, LABEL_H);
			gem_white(cr);
		} else {
			gem_black(cr);
		}
		gem_text(cr, s, tx, ly + l * LABEL_H, LABEL_H);
	}
	gem_black(cr);
	g_free(one);
	g_free(two);
}

/* A folder in text: GEM's diamond, as the item selector shows them. */
static void diamond(cairo_t *cr, int x, int y) {
	for (int r = 0; r < 7; r++) {
		int half = r < 4 ? r : 6 - r;
		gem_fill(cr, x + 3 - half, y + r, 2 * half + 1, 1);
	}
}

static char *when(gint64 t) {
	GDateTime *d = g_date_time_new_from_unix_local(t);
	char *s = d != NULL ? g_date_time_format(d, "%Y-%m-%d %H:%M") : g_strdup("");
	if (d != NULL) {
		g_date_time_unref(d);
	}
	return s;
}

static void paint_item_text(cairo_t *cr, struct win *w, int i, int y0, int width) {
	struct item *it = item_at_index(w, i);
	int y = 2 + i * GEM_ROW_H - y0;
	bool inv = it->selected || i == w->drop_hover;
	gem_black(cr);
	if (inv) {
		gem_fill(cr, 0, y, width, GEM_ROW_H);
		gem_white(cr);
	}
	if (it->dir) {
		diamond(cr, GEM_PAD, y + (GEM_ROW_H - 7) / 2);
	}
	int date_w = (int)gem_text_width(cr, "0000-00-00 00:00");
	int size_w = (int)gem_text_width(cr, "000.0 MB");
	int date_x = width - GEM_PAD - date_w;
	int size_x = date_x - 2 * GEM_PAD - size_w;
	int name_x = GEM_PAD + 14;
	char *name = it->link ? g_strconcat(it->display, " @", NULL) :
		g_strdup(it->display);
	gem_text_clipped(cr, name, name_x, y, MAX(size_x - name_x - GEM_PAD, 0),
		GEM_ROW_H);
	g_free(name);
	if (!it->dir) {
		char *size = g_format_size(it->size);
		int sw = (int)gem_text_width(cr, size);
		gem_text(cr, size, size_x + size_w - sw, y, GEM_ROW_H);
		g_free(size);
	}
	char *t = when(it->mtime);
	gem_text(cr, t, date_x, y, GEM_ROW_H);
	g_free(t);
	gem_black(cr);
}

/* GEM's dotted box, for dragging round things. */
static void dotted(cairo_t *cr, int x, int y, int w, int h) {
	gem_black(cr);
	for (int i = 0; i < w; i += 2) {
		dot(cr, x + i, y);
		dot(cr, x + i, y + h - 1);
	}
	for (int i = 0; i < h; i += 2) {
		dot(cr, x, y + i);
		dot(cr, x + w - 1, y + i);
	}
}

static void paint(cairo_t *cr, int width, int height, void *data) {
	struct win *w = data;
	lay_out(w);
	int y0 = (int)scroll_y(w);
	if (n_items(w) == 0) {
		const char *s = "This folder is empty.";
		gem_black(cr);
		gem_text(cr, s, (width - (int)gem_text_width(cr, s)) / 2, height / 3,
			GEM_ROW_H);
		gem_grey_out(cr, 0, height / 3, width, GEM_ROW_H);
		return;
	}
	for (int i = 0; i < n_items(w); i++) {
		struct box c;
		cell_of(w, i, &c);
		if (c.y + c.h < y0 || c.y > y0 + height) {
			continue;
		}
		if (app.view == VIEW_TEXT) {
			paint_item_text(cr, w, i, y0, width);
		} else {
			paint_item_icons(cr, w, i, y0);
		}
	}
	if (w->band) {
		int x = (int)MIN(w->bx0, w->bx1), y = (int)MIN(w->by0, w->by1) - y0;
		int bw = (int)fabs(w->bx1 - w->bx0) + 1, bh = (int)fabs(w->by1 - w->by0) + 1;
		dotted(cr, x, y, bw, bh);
	}
}

static void draw(GtkDrawingArea *a, cairo_t *cr, int width, int height,
		gpointer data) {
	struct win *w = data;
	gem_draw_pixelated(cr, width, height, paint, w);
}

static void paint_info(cairo_t *cr, int width, int height, void *data) {
	struct win *w = data;
	gem_black(cr);
	gem_fill(cr, 0, 0, width, 1);
	/* A copy or move under way, else a message, else what's here. */
	char *s = NULL;
	for (GList *l = app.ops; l != NULL && s == NULL; l = l->next) {
		struct op *op = l->data;
		g_mutex_lock(&op->lock);
		s = g_strdup(op->status);
		g_mutex_unlock(&op->lock);
	}
	if (s != NULL) {
		/* busy */
	} else if (w->message != NULL) {
		g_free(s);
		s = g_strdup(w->message);
	} else if (s == NULL) {
		int n = n_items(w), picked = n_selected(w);
		goffset bytes = 0;
		for (int i = 0; i < n; i++) {
			struct item *it = item_at_index(w, i);
			if (!it->dir && (picked == 0 || it->selected)) {
				bytes += it->size;
			}
		}
		char *size = g_format_size(bytes);
		bool files = false;
		for (int i = 0; i < n; i++) {
			struct item *it = item_at_index(w, i);
			files |= !it->dir && (picked == 0 || it->selected);
		}
		const char *items = n == 1 ? "item" : "items";
		if (picked > 0) {
			s = files ? g_strdup_printf("%d of %d %s picked, %s", picked, n,
				items, size) : g_strdup_printf("%d of %d %s picked", picked, n,
				items);
		} else {
			s = files ? g_strdup_printf("%s in %d %s", size, n, items) :
				g_strdup_printf("%d %s", n, items);
		}
		g_free(size);
	}
	gem_text_clipped(cr, s, GEM_PAD, 1, width - 2 * GEM_PAD, height - 1);
	g_free(s);
}

/* ---- Opening ------------------------------------------------------------------ */

static void launch(struct win *w, GFile *f, const char *display) {
	GError *error = NULL;
	char *uri = g_file_get_uri(f);
	GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(
		gtk_widget_get_display(w->window));
	if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx),
			&error)) {
		char *text = g_strdup_printf("Can't open \"%s\":\n%s", display,
			error->message);
		alert(w, GEM_ALERT_STOP, text);
		g_free(text);
		g_error_free(error);
	}
	g_object_unref(ctx);
	g_free(uri);
}

/* What's picked opened: one folder shows in this window (in its own with
 * new_window), more each in a window of their own; files in their apps. */
static void open_selected(struct win *w, bool new_window) {
	int folders = 0;
	for (int i = 0; i < n_items(w); i++) {
		struct item *it = item_at_index(w, i);
		folders += it->selected && it->dir;
	}
	GFile *into = NULL;
	for (int i = 0; i < n_items(w); i++) {
		struct item *it = item_at_index(w, i);
		if (!it->selected) {
			continue;
		}
		GFile *f = child(w, i);
		if (!it->dir) {
			launch(w, f, it->display);
		} else if (folders == 1 && !new_window) {
			into = g_object_ref(f);
		} else {
			win_new(f);
		}
		g_object_unref(f);
	}
	if (into != NULL) {
		read_dir(w, into, false);
		g_object_unref(into);
	}
}

/* ---- File operations, in a thread ---------------------------------------------- */

static void op_free(struct op *op) {
	for (guint i = 0; i < op->pairs->len; i++) {
		struct pair *p = &g_array_index(op->pairs, struct pair, i);
		g_clear_object(&p->src);
		g_clear_object(&p->dst);
	}
	g_array_unref(op->pairs);
	g_weak_ref_clear(&op->window);
	g_clear_object(&op->dest_dir);
	g_mutex_clear(&op->lock);
	g_free(op->status);
	g_ptr_array_unref(op->errors);
	g_free(op);
}

static struct op *op_new(enum op_kind kind) {
	struct op *op = g_new0(struct op, 1);
	op->kind = kind;
	op->pairs = g_array_new(FALSE, TRUE, sizeof(struct pair));
	g_mutex_init(&op->lock);
	op->errors = g_ptr_array_new_with_free_func(g_free);
	g_weak_ref_init(&op->window, NULL);
	return op;
}

static void op_add(struct op *op, GFile *src, GFile *dst) {
	struct pair p = { g_object_ref(src), dst != NULL ? g_object_ref(dst) : NULL };
	g_array_append_val(op->pairs, p);
}

static void op_status_set(struct op *op, char *status) {
	g_mutex_lock(&op->lock);
	g_free(op->status);
	op->status = status;
	g_mutex_unlock(&op->lock);
}

static void op_error(struct op *op, GFile *f, GError *error) {
	char *name = g_file_get_basename(f);
	g_mutex_lock(&op->lock);
	g_ptr_array_add(op->errors, g_strdup_printf("%s: %s", name, error->message));
	g_mutex_unlock(&op->lock);
	g_free(name);
}

static bool delete_tree(GFile *f, GError **error) {
	GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
	if (info == NULL) {
		return false;
	}
	bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
	g_object_unref(info);
	if (dir) {
		GFileEnumerator *e = g_file_enumerate_children(f,
			G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
			NULL, error);
		if (e == NULL) {
			return false;
		}
		GFileInfo *ci;
		bool ok = true;
		while (ok && (ci = g_file_enumerator_next_file(e, NULL, error)) != NULL) {
			GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
			ok = delete_tree(c, error);
			g_object_unref(c);
			g_object_unref(ci);
		}
		g_object_unref(e);
		if (!ok || (error != NULL && *error != NULL)) {
			return false;
		}
	}
	return g_file_delete(f, NULL, error);
}

static bool copy_tree(GFile *src, GFile *dst, bool replace, GError **error) {
	GFileInfo *info = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_TYPE,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
	if (info == NULL) {
		return false;
	}
	bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
	g_object_unref(info);
	GFileCopyFlags flags = G_FILE_COPY_NOFOLLOW_SYMLINKS |
		G_FILE_COPY_ALL_METADATA | (replace ? G_FILE_COPY_OVERWRITE : 0);
	if (!dir) {
		return g_file_copy(src, dst, flags, NULL, NULL, NULL, error);
	}
	GError *made = NULL;
	if (!g_file_make_directory(dst, NULL, &made)) {
		if (!replace || !g_error_matches(made, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
			g_propagate_error(error, made);
			return false;
		}
		g_error_free(made);
	}
	GFileEnumerator *e = g_file_enumerate_children(src,
		G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		NULL, error);
	if (e == NULL) {
		return false;
	}
	GFileInfo *ci;
	bool ok = true;
	while (ok && (ci = g_file_enumerator_next_file(e, NULL, error)) != NULL) {
		const char *name = g_file_info_get_name(ci);
		GFile *s = g_file_get_child(src, name), *d = g_file_get_child(dst, name);
		ok = copy_tree(s, d, replace, error);
		g_object_unref(s);
		g_object_unref(d);
		g_object_unref(ci);
	}
	g_object_unref(e);
	if (!ok || (error != NULL && *error != NULL)) {
		return false;
	}
	g_file_copy_attributes(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS |
		G_FILE_COPY_ALL_METADATA, NULL, NULL);
	return true;
}

/* Renamed if it can be (on the same disk), else copied and the original
 * deleted. */
static bool move(GFile *src, GFile *dst, bool replace, GError **error) {
	GError *e = NULL;
	if (g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS |
			G_FILE_COPY_ALL_METADATA | (replace ? G_FILE_COPY_OVERWRITE : 0),
			NULL, NULL, NULL, &e)) {
		return true;
	}
	if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) &&
			!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE) &&
			!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_propagate_error(error, e);
		return false;
	}
	g_error_free(e);
	return copy_tree(src, dst, replace, error) && delete_tree(src, error);
}

static GFile *trash_info_for(GFile *in_trash) {
	char *name = g_file_get_basename(in_trash);
	char *info_name = g_strconcat(name, ".trashinfo", NULL);
	char *dir = trash_dir("info");
	char *path = g_build_filename(dir, info_name, NULL);
	GFile *f = g_file_new_for_path(path);
	g_free(path);
	g_free(dir);
	g_free(info_name);
	g_free(name);
	return f;
}

static const char *verb(enum op_kind kind) {
	switch (kind) {
	case OP_COPY: return "Copying";
	case OP_MOVE: return "Moving";
	case OP_TRASH: return "Moving to the Trash";
	case OP_DELETE: return "Deleting";
	case OP_RESTORE: return "Putting back";
	case OP_EMPTY: return "Emptying the Trash";
	}
	return "";
}

static void op_thread(GTask *task, gpointer source, gpointer data,
		GCancellable *cancel) {
	struct op *op = data;
	if (op->kind == OP_EMPTY) {
		op_status_set(op, g_strdup(verb(op->kind)));
		const char *parts[] = { "files", "info" };
		for (int k = 0; k < 2; k++) {
			char *path = trash_dir(parts[k]);
			GFile *d = g_file_new_for_path(path);
			GFileEnumerator *e = g_file_enumerate_children(d,
				G_FILE_ATTRIBUTE_STANDARD_NAME,
				G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
			GFileInfo *ci;
			while (e != NULL &&
					(ci = g_file_enumerator_next_file(e, NULL, NULL)) != NULL) {
				GFile *c = g_file_get_child(d, g_file_info_get_name(ci));
				GError *error = NULL;
				if (!delete_tree(c, &error)) {
					op_error(op, c, error);
					g_error_free(error);
				}
				g_object_unref(c);
				g_object_unref(ci);
			}
			g_clear_object(&e);
			g_object_unref(d);
			g_free(path);
		}
		char *sizes = trash_dir("directorysizes");
		g_unlink(sizes);
		g_free(sizes);
		g_task_return_boolean(task, TRUE);
		return;
	}
	for (guint i = 0; i < op->pairs->len; i++) {
		struct pair *p = &g_array_index(op->pairs, struct pair, i);
		char *name = g_file_get_basename(p->src);
		op_status_set(op, op->pairs->len == 1 ?
			g_strdup_printf("%s %s", verb(op->kind), name) :
			g_strdup_printf("%s %u of %u: %s", verb(op->kind), i + 1,
				op->pairs->len, name));
		g_free(name);
		GError *error = NULL;
		bool ok = true;
		switch (op->kind) {
		case OP_COPY:
			ok = copy_tree(p->src, p->dst, op->replace, &error);
			break;
		case OP_MOVE:
			ok = move(p->src, p->dst, op->replace, &error);
			break;
		case OP_TRASH:
			ok = g_file_trash(p->src, NULL, &error);
			break;
		case OP_DELETE:
			ok = delete_tree(p->src, &error);
			if (ok) {
				GFile *info = trash_info_for(p->src);
				g_file_delete(info, NULL, NULL);
				g_object_unref(info);
			}
			break;
		case OP_RESTORE:
			ok = move(p->src, p->dst, false, &error);
			if (ok) {
				GFile *info = trash_info_for(p->src);
				g_file_delete(info, NULL, NULL);
				g_object_unref(info);
			}
			break;
		case OP_EMPTY:
			break;
		}
		if (!ok) {
			op_error(op, p->src, error);
			g_clear_error(&error);
		}
	}
	g_task_return_boolean(task, TRUE);
}

static void redraw_info(struct win *w) {
	gtk_widget_queue_draw(w->info);
}

static gboolean op_tick(gpointer data) {
	each_win(redraw_info);
	return G_SOURCE_CONTINUE;
}

static void op_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct op *op = data;
	app.ops = g_list_remove(app.ops, op);
	if (app.ops == NULL && app.op_timer != 0) {
		g_source_remove(app.op_timer);
		app.op_timer = 0;
	}
	/* What was copied or moved, picked where it went. */
	for (GList *l = app.wins; op->dest_dir != NULL && l != NULL; l = l->next) {
		struct win *w = l->data;
		if (!g_file_equal(w->dir, op->dest_dir)) {
			continue;
		}
		GPtrArray *names = g_ptr_array_new();
		for (guint i = 0; i < op->pairs->len; i++) {
			struct pair *p = &g_array_index(op->pairs, struct pair, i);
			g_ptr_array_add(names, g_file_get_basename(p->dst));
		}
		g_ptr_array_add(names, NULL);
		g_strfreev(w->select_after);
		w->select_after = (char **)g_ptr_array_free(names, FALSE);
		select_none(w);
		read_dir(w, w->dir, true);
	}
	each_win(redraw_info);
	each_win(update_menus);
	if (op->errors->len > 0) {
		GtkWidget *window = g_weak_ref_get(&op->window);
		struct win *w = window != NULL ?
			g_object_get_data(G_OBJECT(window), "gemfiles-win") : NULL;
		if (w == NULL && app.wins != NULL) {
			w = app.wins->data;
		}
		if (w != NULL) {
			GString *s = g_string_new(NULL);
			g_string_append_printf(s, "%s didn't all go:", verb(op->kind));
			for (guint i = 0; i < op->errors->len && i < 3; i++) {
				g_string_append_printf(s, "\n%s", (char *)op->errors->pdata[i]);
			}
			if (op->errors->len > 3) {
				g_string_append_printf(s, "\nand %u more.", op->errors->len - 3);
			}
			alert(w, GEM_ALERT_STOP, s->str);
			g_string_free(s, TRUE);
		}
		g_clear_object(&window);
	}
	op_free(op);
}

static void start_op(struct win *w, struct op *op) {
	g_weak_ref_set(&op->window, w != NULL ? w->window : NULL);
	app.ops = g_list_append(app.ops, op);
	if (app.op_timer == 0) {
		app.op_timer = g_timeout_add(250, op_tick, NULL);
	}
	GTask *task = g_task_new(NULL, NULL, op_done, op);
	g_task_set_task_data(task, op, NULL);
	g_task_run_in_thread(task, op_thread);
	g_object_unref(task);
}

/* ---- Copying and moving, asked first about what's already there -------------- */

/* "name copy.ext", or "name copy 2.ext", free in dir. */
static GFile *copy_name(GFile *dir, const char *name) {
	const char *dot = strrchr(name, '.');
	if (dot == name) {
		dot = NULL;
	}
	char *stem = dot != NULL ? g_strndup(name, dot - name) : g_strdup(name);
	const char *ext = dot != NULL ? dot : "";
	for (int n = 1;; n++) {
		char *try = n == 1 ? g_strdup_printf("%s copy%s", stem, ext) :
			g_strdup_printf("%s copy %d%s", stem, n, ext);
		GFile *f = g_file_get_child(dir, try);
		g_free(try);
		if (!g_file_query_exists(f, NULL)) {
			g_free(stem);
			return f;
		}
		g_object_unref(f);
	}
}

struct transfer {
	struct win *w;
	struct op *op;
};

static void transfer_answered(int button, void *data) {
	struct transfer *t = data;
	/* Cancel, Skip, Replace */
	if (button == 0) {
		op_free(t->op);
	} else {
		if (button == 1) {
			for (guint i = t->op->pairs->len; i-- > 0;) {
				struct pair *p = &g_array_index(t->op->pairs, struct pair, i);
				if (g_file_query_exists(p->dst, NULL)) {
					g_object_unref(p->src);
					g_object_unref(p->dst);
					g_array_remove_index(t->op->pairs, i);
				}
			}
		}
		t->op->replace = button == 2;
		if (t->op->pairs->len > 0) {
			start_op(t->w, t->op);
		} else {
			op_free(t->op);
		}
	}
	g_free(t);
}

/* files copied (or moved) into dir; asked first if some are there already. */
static void transfer(struct win *w, GList *files, GFile *dir, bool copy) {
	struct op *op = op_new(copy ? OP_COPY : OP_MOVE);
	op->dest_dir = g_object_ref(dir);
	int clashes = 0;
	char *clash = NULL;
	for (GList *l = files; l != NULL; l = l->next) {
		GFile *src = l->data;
		GFile *parent = g_file_get_parent(src);
		bool here = parent != NULL && g_file_equal(parent, dir);
		g_clear_object(&parent);
		if (g_file_equal(src, dir) || g_file_has_prefix(dir, src)) {
			char *name = g_file_get_basename(src);
			char *text = g_strdup_printf("\"%s\" can't go inside itself.", name);
			alert(w, GEM_ALERT_STOP, text);
			g_free(text);
			g_free(name);
			op_free(op);
			return;
		}
		if (here && !copy) {
			continue; /* moved to where it is */
		}
		char *name = g_file_get_basename(src);
		GFile *dst = here ? copy_name(dir, name) : g_file_get_child(dir, name);
		if (!here && g_file_query_exists(dst, NULL)) {
			clashes++;
			if (clash == NULL) {
				clash = g_strdup(name);
			}
		}
		op_add(op, src, dst);
		g_object_unref(dst);
		g_free(name);
	}
	if (op->pairs->len == 0) {
		op_free(op);
		g_free(clash);
		return;
	}
	if (clashes == 0) {
		start_op(w, op);
		return;
	}
	char *where = shown_path(dir);
	char *text = clashes == 1 ?
		g_strdup_printf("\"%s\" is already in %s.\nReplace it?", clash, where) :
		g_strdup_printf("%d of these are already in %s.\nReplace them?", clashes,
			where);
	static const char *const buttons[] = { "Cancel", "Skip", "Replace", NULL };
	struct transfer *t = g_new0(struct transfer, 1);
	t->w = w;
	t->op = op;
	gem_alert(w->host, GEM_ALERT_QUESTION, text, buttons, 2, 0,
		transfer_answered, t);
	g_free(text);
	g_free(where);
	g_free(clash);
}

static bool same_disk(GFile *a, GFile *b) {
	GFileInfo *x = g_file_query_info(a, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	GFileInfo *y = g_file_query_info(b, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	bool same = x != NULL && y != NULL && g_strcmp0(
		g_file_info_get_attribute_string(x, G_FILE_ATTRIBUTE_ID_FILESYSTEM),
		g_file_info_get_attribute_string(y, G_FILE_ATTRIBUTE_ID_FILESYSTEM)) == 0;
	g_clear_object(&x);
	g_clear_object(&y);
	return same;
}

/* ---- Trash ---------------------------------------------------------------------- */

static void trash_selected(struct win *w) {
	if (n_selected(w) == 0) {
		return;
	}
	struct op *op = op_new(OP_TRASH);
	GList *files = selected_files(w);
	for (GList *l = files; l != NULL; l = l->next) {
		op_add(op, l->data, NULL);
	}
	g_list_free_full(files, g_object_unref);
	start_op(w, op);
}

struct confirm {
	struct win *w;
	struct op *op;
};

static void confirm_answered(int button, void *data) {
	struct confirm *c = data;
	if (button == 1) {
		start_op(c->w, c->op);
	} else {
		op_free(c->op);
	}
	g_free(c);
}

static void confirm_op(struct win *w, struct op *op, const char *text,
		const char *yes) {
	const char *buttons[] = { "Cancel", yes, NULL };
	struct confirm *c = g_new0(struct confirm, 1);
	c->w = w;
	c->op = op;
	gem_alert(w->host, GEM_ALERT_QUESTION, text, buttons, 0, 0,
		confirm_answered, c);
}

/* In the Trash: gone for good, once asked. */
static void delete_selected(struct win *w) {
	int n = n_selected(w);
	if (n == 0) {
		return;
	}
	struct op *op = op_new(OP_DELETE);
	GList *files = selected_files(w);
	for (GList *l = files; l != NULL; l = l->next) {
		op_add(op, l->data, NULL);
	}
	g_list_free_full(files, g_object_unref);
	char *text = n == 1 ?
		g_strdup_printf("Delete \"%s\" for good?\nIt can't be put back.",
			item_at_index(w, first_selected(w))->display) :
		g_strdup_printf("Delete these %d items for good?\nThey can't be put back.",
			n);
	confirm_op(w, op, text, "Delete");
	g_free(text);
}

static void empty_trash(struct win *w) {
	confirm_op(w, op_new(OP_EMPTY),
		"Empty the Trash?\nWhat's in it is deleted for good.", "Empty");
}

/* Where a thing in the Trash came from, from its .trashinfo. */
static GFile *trash_origin(GFile *f) {
	GFile *info = trash_info_for(f);
	char *path = g_file_get_path(info);
	GKeyFile *kf = g_key_file_new();
	GFile *origin = NULL;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		char *escaped = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
		char *orig = escaped != NULL ? g_uri_unescape_string(escaped, NULL) : NULL;
		if (orig != NULL) {
			origin = g_file_new_for_path(orig);
		}
		g_free(orig);
		g_free(escaped);
	}
	g_key_file_unref(kf);
	g_free(path);
	g_object_unref(info);
	return origin;
}

static void put_back(struct win *w) {
	struct op *op = op_new(OP_RESTORE);
	GList *files = selected_files(w);
	for (GList *l = files; l != NULL; l = l->next) {
		GFile *origin = trash_origin(l->data);
		if (origin == NULL) {
			char *name = g_file_get_basename(l->data);
			g_ptr_array_add(op->errors,
				g_strdup_printf("%s: where it came from isn't known", name));
			g_free(name);
			continue;
		}
		op_add(op, l->data, origin);
		g_object_unref(origin);
	}
	g_list_free_full(files, g_object_unref);
	start_op(w, op);
}

/* ---- The clipboard ---------------------------------------------------------- */

static void clipboard_put(struct win *w, bool cut) {
	GList *files = selected_files(w);
	if (files == NULL) {
		return;
	}
	GSList *list = NULL;
	GString *uris = g_string_new(NULL);
	GString *gnome = g_string_new(cut ? "cut" : "copy");
	for (GList *l = g_list_last(files); l != NULL; l = l->prev) {
		list = g_slist_prepend(list, l->data);
	}
	for (GList *l = files; l != NULL; l = l->next) {
		char *uri = g_file_get_uri(l->data);
		g_string_append_printf(uris, "%s\r\n", uri);
		g_string_append_printf(gnome, "\n%s", uri);
		g_free(uri);
	}
	GdkFileList *fl = gdk_file_list_new_from_list(list);
	GBytes *ub = g_string_free_to_bytes(uris);
	GBytes *gb = g_string_free_to_bytes(gnome);
	GdkContentProvider *providers[] = {
		gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl),
		gdk_content_provider_new_for_bytes("text/uri-list", ub),
		gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gb),
	};
	GdkContentProvider *p = gdk_content_provider_new_union(providers, 3);
	gdk_clipboard_set_content(gtk_widget_get_clipboard(w->window), p);
	g_object_unref(p);
	g_bytes_unref(ub);
	g_bytes_unref(gb);
	g_boxed_free(GDK_TYPE_FILE_LIST, fl);
	g_slist_free(list);
	g_list_free_full(files, g_object_unref);
	app.cut = cut;
	char *s = g_strdup_printf("%s %d %s", cut ? "Cut" : "Copied", n_selected(w),
		n_selected(w) == 1 ? "item" : "items");
	say(w, s);
}

static bool clipboard_has_files(struct win *w) {
	GdkContentFormats *f = gdk_clipboard_get_formats(
		gtk_widget_get_clipboard(w->window));
	return gdk_content_formats_contain_gtype(f, GDK_TYPE_FILE_LIST) ||
		gdk_content_formats_contain_mime_type(f, "text/uri-list");
}

static void pasted(GObject *source, GAsyncResult *result, gpointer data) {
	GtkWidget *window = data;
	struct win *w = g_object_get_data(G_OBJECT(window), "gemfiles-win");
	const GValue *v = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source),
		result, NULL);
	if (w != NULL && v != NULL && G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
		GSList *files = gdk_file_list_get_files(g_value_get_boxed(v));
		GList *list = NULL;
		for (GSList *l = files; l != NULL; l = l->next) {
			list = g_list_append(list, l->data);
		}
		bool move = app.cut && gdk_clipboard_is_local(GDK_CLIPBOARD(source));
		transfer(w, list, w->dir, !move);
		if (move) {
			/* Moved once; what's on the clipboard is gone from there. */
			gdk_clipboard_set_content(GDK_CLIPBOARD(source), NULL);
			app.cut = false;
		}
		g_list_free(list);
		g_slist_free(files);
	}
	g_object_unref(window);
}

static void paste(struct win *w) {
	gdk_clipboard_read_value_async(gtk_widget_get_clipboard(w->window),
		GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, pasted,
		g_object_ref(w->window));
}

/* ---- Dialogs that ask for a name -------------------------------------------- */

#define ASK_W 400

struct ask {
	struct win *w;
	GtkWidget *box, *area, *field;
	GArray *hits;
	char *title, *label, *ok;
	char **rows;              /* "Label\tvalue" under the field */
	int field_y;
	void (*done)(struct win *w, const char *text, void *data);
	void *data;
};

enum { ASK_CANCEL = 1, ASK_OK };

static int ask_height(struct ask *a) {
	int rows = a->rows != NULL ? g_strv_length(a->rows) : 0;
	return 2 * GEM_PAD + GEM_ROW_H + GEM_PAD + GEM_FIELD_H + GEM_PAD +
		rows * GEM_ROW_H + GEM_PAD + GEM_ROW_H + 4 + GEM_PAD;
}

static int label_w(void) {
	return (int)gem_measure("Modified: ") + GEM_PAD;
}

static void ask_paint(cairo_t *cr, int w, int h, void *data) {
	struct ask *a = data;
	g_array_set_size(a->hits, 0);
	gem_dialog_frame(cr, w, h);
	int y = 2 * GEM_PAD;
	gem_black(cr);
	gem_text(cr, a->title, (w - (int)gem_text_width(cr, a->title)) / 2, y,
		GEM_ROW_H);
	y += GEM_ROW_H + GEM_PAD;
	gem_text(cr, a->label, 2 * GEM_PAD, y + (GEM_FIELD_H - GEM_ROW_H) / 2,
		GEM_ROW_H);
	y += GEM_FIELD_H + GEM_PAD;
	for (int i = 0; a->rows != NULL && a->rows[i] != NULL; i++) {
		char **kv = g_strsplit(a->rows[i], "\t", 2);
		gem_text(cr, kv[0], 2 * GEM_PAD, y, GEM_ROW_H);
		if (kv[1] != NULL) {
			gem_text_clipped(cr, kv[1], 2 * GEM_PAD + label_w(), y,
				w - 4 * GEM_PAD - label_w(), GEM_ROW_H);
		}
		g_strfreev(kv);
		y += GEM_ROW_H;
	}
	y += GEM_PAD;
	int bw = MAX(gem_button_width("Cancel"), gem_button_width(a->ok));
	int x = w - 2 * GEM_PAD - bw;
	gem_button(cr, a->hits, x, y, bw, a->ok, ASK_OK, true);
	gem_button(cr, a->hits, x - GEM_PAD - bw, y, bw, "Cancel", ASK_CANCEL,
		false);
}

static void ask_close(struct ask *a, bool ok) {
	char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->field)));
	gem_release(a->w->host, a->box);
	if (ok) {
		a->done(a->w, text, a->data);
	}
	g_free(text);
	g_array_unref(a->hits);
	g_free(a->title);
	g_free(a->label);
	g_free(a->ok);
	g_strfreev(a->rows);
	g_free(a);
}

static void ask_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct ask *a = data;
	const struct gem_hit *hit = gem_hit_at(a->hits, x, y);
	if (hit != NULL) {
		ask_close(a, hit->id == ASK_OK);
	}
}

static void ask_activate(GtkEntry *e, gpointer data) {
	ask_close(data, true);
}

static gboolean ask_key(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	if (key == GDK_KEY_Escape) {
		ask_close(data, false);
		return TRUE;
	}
	return FALSE;
}

/* A box over the window asking for text: title, then label and a field
 * holding value (select_to: how much of it is picked, -1 all), then rows
 * of "Label\tvalue", then Cancel and ok. done gets the text on ok. */
static void ask(struct win *w, const char *title, const char *label,
		const char *value, int select_to, char **rows, const char *ok,
		void (*done)(struct win *, const char *, void *), void *data) {
	struct ask *a = g_new0(struct ask, 1);
	a->w = w;
	a->title = g_strdup(title);
	a->label = g_strdup(label);
	a->ok = g_strdup(ok);
	a->rows = g_strdupv(rows);
	a->done = done;
	a->data = data;
	a->hits = gem_hits_new();
	int h = ask_height(a);
	a->box = gtk_overlay_new();
	a->area = gem_pixel_area_new(ASK_W, h, ask_paint, a);
	gtk_overlay_set_child(GTK_OVERLAY(a->box), a->area);
	int fx = 2 * GEM_PAD + label_w();
	a->field = gem_field_new(fx, 2 * GEM_PAD + GEM_ROW_H + GEM_PAD,
		ASK_W - fx - 2 * GEM_PAD);
	gtk_overlay_add_overlay(GTK_OVERLAY(a->box), a->field);
	gtk_editable_set_text(GTK_EDITABLE(a->field), value);
	g_signal_connect(a->field, "activate", G_CALLBACK(ask_activate), a);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(ask_pressed), a);
	gtk_widget_add_controller(a->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(ask_key), a);
	gtk_widget_add_controller(a->box, keys);
	gem_hold(w->host, a->box);
	gtk_widget_grab_focus(a->field);
	gtk_editable_select_region(GTK_EDITABLE(a->field), 0, select_to);
}

/* ---- New Folder, Show Info, Go to Folder ------------------------------------- */

static void made_folder(struct win *w, const char *name, void *data) {
	if (name[0] == '\0') {
		return;
	}
	GFile *f = g_file_get_child_for_display_name(w->dir, name, NULL);
	GError *error = NULL;
	if (f == NULL || !g_file_make_directory(f, NULL, &error)) {
		char *text = g_strdup_printf("Can't make the folder \"%s\":\n%s", name,
			error != NULL ? error->message : "that name won't do");
		alert(w, GEM_ALERT_STOP, text);
		g_free(text);
		g_clear_error(&error);
		g_clear_object(&f);
		return;
	}
	char *base = g_file_get_basename(f);
	char *names[] = { base, NULL };
	g_strfreev(w->select_after);
	w->select_after = g_strdupv(names);
	select_none(w);
	read_dir(w, w->dir, true);
	scroll_to_item(w, w->cursor);
	g_free(base);
	g_object_unref(f);
}

static void new_folder(struct win *w) {
	char *name = g_strdup("New Folder");
	for (int n = 2;; n++) {
		GFile *f = g_file_get_child(w->dir, name);
		bool there = g_file_query_exists(f, NULL);
		g_object_unref(f);
		if (!there) {
			break;
		}
		g_free(name);
		name = g_strdup_printf("New Folder %d", n);
	}
	ask(w, "New Folder", "Name:", name, -1, NULL, "Make", made_folder, NULL);
	g_free(name);
}

static void renamed(struct win *w, const char *name, void *data) {
	char *old = data;
	if (name[0] != '\0' && strcmp(name, old) != 0) {
		GFile *f = g_file_get_child(w->dir, old);
		GError *error = NULL;
		GFile *to = g_file_set_display_name(f, name, NULL, &error);
		if (to == NULL) {
			char *text = g_strdup_printf("Can't rename \"%s\":\n%s", old,
				error->message);
			alert(w, GEM_ALERT_STOP, text);
			g_free(text);
			g_error_free(error);
		} else {
			char *base = g_file_get_basename(to);
			char *names[] = { base, NULL };
			g_strfreev(w->select_after);
			w->select_after = g_strdupv(names);
			select_none(w);
			read_dir(w, w->dir, true);
			g_free(base);
			g_object_unref(to);
		}
		g_object_unref(f);
	}
	g_free(old);
}

/* GEM's Show Info: what it is, and its name, to change. */
static void show_info(struct win *w) {
	int i = w->cursor >= 0 && item_at_index(w, w->cursor)->selected ?
		w->cursor : first_selected(w);
	if (i < 0) {
		return;
	}
	struct item *it = item_at_index(w, i);
	char *kind = it->dir ? g_strdup("Folder") :
		it->type != NULL ? g_content_type_get_description(it->type) :
		g_strdup("File");
	char *size;
	if (it->dir) {
		int count = 0;
		GFile *f = child(w, i);
		GFileEnumerator *e = g_file_enumerate_children(f,
			G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, NULL);
		GFileInfo *ci;
		while (e != NULL && (ci = g_file_enumerator_next_file(e, NULL, NULL)) != NULL) {
			count++;
			g_object_unref(ci);
		}
		g_clear_object(&e);
		g_object_unref(f);
		size = g_strdup_printf("%d %s", count, count == 1 ? "item" : "items");
	} else {
		size = g_format_size_full(it->size, G_FORMAT_SIZE_LONG_FORMAT);
	}
	char *modified = when(it->mtime);
	char *where = shown_path(w->dir);
	char *link = NULL;
	if (it->link) {
		GFile *f = child(w, i);
		GFileInfo *li = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
		if (li != NULL) {
			link = g_strdup(g_file_info_get_symlink_target(li));
			g_object_unref(li);
		}
		g_object_unref(f);
	}
	GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(rows, g_strdup_printf("Kind:\t%s", kind));
	g_ptr_array_add(rows, g_strdup_printf("Size:\t%s", size));
	g_ptr_array_add(rows, g_strdup_printf("Modified:\t%s", modified));
	g_ptr_array_add(rows, g_strdup_printf("Where:\t%s", where));
	if (link != NULL) {
		g_ptr_array_add(rows, g_strdup_printf("Links to:\t%s", link));
	}
	g_ptr_array_add(rows, NULL);
	/* The name without its extension picked, to type over. */
	const char *dot = it->dir ? NULL : strrchr(it->display, '.');
	int select_to = dot != NULL && dot != it->display ?
		(int)g_utf8_pointer_to_offset(it->display, dot) : -1;
	ask(w, "Item Information", "Name:", it->display, select_to,
		(char **)rows->pdata, "OK", renamed, g_strdup(it->name));
	g_ptr_array_unref(rows);
	g_free(kind);
	g_free(size);
	g_free(modified);
	g_free(where);
	g_free(link);
}

static void went(struct win *w, const char *text, void *data) {
	if (text[0] == '\0') {
		return;
	}
	char *path = text[0] == '~' ?
		g_strconcat(g_get_home_dir(), text + 1, NULL) : g_strdup(text);
	GFile *f = g_file_new_for_commandline_arg_and_cwd(path,
		g_file_peek_path(w->dir));
	if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL) ==
			G_FILE_TYPE_DIRECTORY) {
		read_dir(w, f, false);
	} else {
		char *t = g_strdup_printf("There's no folder %s.", text);
		alert(w, GEM_ALERT_STOP, t);
		g_free(t);
	}
	g_object_unref(f);
	g_free(path);
}

static void go_to_folder(struct win *w) {
	char *shown = shown_path(w->dir);
	ask(w, "Go to Folder", "Folder:", shown, -1, NULL, "Go", went, NULL);
	g_free(shown);
}

/* ---- Menus ---------------------------------------------------------------------- */

enum {
	ACT_NEW_WINDOW = 1, ACT_OPEN, ACT_INFO, ACT_NEW_FOLDER, ACT_TRASH,
	ACT_PUT_BACK, ACT_DELETE, ACT_EMPTY_TRASH, ACT_CLOSE,
	ACT_CUT, ACT_COPY, ACT_PASTE, ACT_SELECT_ALL,
	ACT_ICONS, ACT_TEXT, ACT_SORT_NAME, ACT_SORT_DATE, ACT_SORT_SIZE,
	ACT_SORT_TYPE, ACT_HIDDEN,
	ACT_UP, ACT_GO_TO, ACT_TRASH_FOLDER,
	ACT_PLACE = 100, /* Go's places, in order */
};

/* Go's places: home and the usual folders in it, then what's mounted:
 * "path" or "path\tname", with "" where the Trash goes. */
static char **places(void) {
	GPtrArray *a = g_ptr_array_new();
	g_ptr_array_add(a, g_strdup(g_get_home_dir()));
	const GUserDirectory dirs[] = {
		G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS,
		G_USER_DIRECTORY_DOWNLOAD, G_USER_DIRECTORY_MUSIC,
		G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_VIDEOS,
	};
	for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
		const char *d = g_get_user_special_dir(dirs[i]);
		if (d != NULL && strcmp(d, g_get_home_dir()) != 0 &&
				g_file_test(d, G_FILE_TEST_IS_DIR)) {
			g_ptr_array_add(a, g_strdup(d));
		}
	}
	g_ptr_array_add(a, g_strdup("")); /* a separator */
	GList *mounts = g_volume_monitor_get_mounts(app.volumes);
	for (GList *l = mounts; l != NULL; l = l->next) {
		GFile *root = g_mount_get_root(l->data);
		char *path = g_file_get_path(root);
		if (path != NULL && !g_mount_is_shadowed(l->data)) {
			char *name = g_mount_get_name(l->data);
			g_ptr_array_add(a, g_strdup_printf("%s\t%s", path, name));
			g_free(name);
		}
		g_free(path);
		g_object_unref(root);
	}
	g_list_free_full(mounts, g_object_unref);
	g_ptr_array_add(a, g_strdup("/\tComputer"));
	g_ptr_array_add(a, NULL);
	return (char **)g_ptr_array_free(a, FALSE);
}

static void build_menus(struct app_menu *m, void *data) {
	struct win *w = data;
	int picked = n_selected(w);
	uint32_t some = picked > 0 ? 0 : APP_MENU_DISABLED;
	bool trash = in_trash(w);
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_NEW_WINDOW, "New Window", "^N", 0);
	app_menu_add_item(m, ACT_OPEN, "Open", "^O", some);
	app_menu_add_item(m, ACT_INFO, "Show Info...", "^I", some);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_NEW_FOLDER, "New Folder...", "^Shift+N",
		trash ? APP_MENU_DISABLED : 0);
	if (trash) {
		app_menu_add_item(m, ACT_PUT_BACK, "Put Back", NULL, some);
		app_menu_add_item(m, ACT_DELETE, "Delete...", "Del", some);
	} else {
		app_menu_add_item(m, ACT_TRASH, "Move to Trash", "Del", some);
	}
	app_menu_add_item(m, ACT_EMPTY_TRASH, "Empty Trash...", NULL,
		trash_empty() ? APP_MENU_DISABLED : 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close Window", "^W", 0);

	app_menu_add_menu(m, "Edit");
	app_menu_add_item(m, ACT_CUT, "Cut", "^X", trash ? APP_MENU_DISABLED : some);
	app_menu_add_item(m, ACT_COPY, "Copy", "^C", some);
	app_menu_add_item(m, ACT_PASTE, "Paste", "^V",
		!trash && clipboard_has_files(w) ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_SELECT_ALL, "Select All", "^A",
		n_items(w) > 0 ? 0 : APP_MENU_DISABLED);

	app_menu_add_menu(m, "View");
	app_menu_add_item(m, ACT_ICONS, "Show as Icons", NULL,
		app.view == VIEW_ICONS ? APP_MENU_CHECKED : 0);
	app_menu_add_item(m, ACT_TEXT, "Show as Text", NULL,
		app.view == VIEW_TEXT ? APP_MENU_CHECKED : 0);
	app_menu_add_separator(m);
	static const char *const sorts[] = { "Sort by Name", "Sort by Date",
		"Sort by Size", "Sort by Type" };
	for (int s = 0; s < 4; s++) {
		app_menu_add_item(m, ACT_SORT_NAME + s, sorts[s], NULL,
			(int)app.sort == s ? APP_MENU_CHECKED : 0);
	}
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_HIDDEN, "Show Hidden Files", "^H",
		app.hidden ? APP_MENU_CHECKED : 0);

	app_menu_add_menu(m, "Go");
	app_menu_add_item(m, ACT_UP, "Enclosing Folder", "^Up",
		trash || !g_file_has_parent(w->dir, NULL) ? APP_MENU_DISABLED : 0);
	app_menu_add_separator(m);
	for (int i = 0; app.places[i] != NULL; i++) {
		if (app.places[i][0] == '\0') {
			app_menu_add_item(m, ACT_TRASH_FOLDER, "Trash", NULL, 0);
			app_menu_add_separator(m);
			continue;
		}
		char **pn = g_strsplit(app.places[i], "\t", 2);
		char *label = pn[1] != NULL ? g_strdup(pn[1]) :
			i == 0 ? g_strdup("Home") : g_path_get_basename(pn[0]);
		app_menu_add_item(m, ACT_PLACE + i, label, NULL, 0);
		g_free(label);
		g_strfreev(pn);
	}
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_GO_TO, "Go to Folder...", "^L", 0);
}

static void apply_view(struct win *w) {
	lay_out(w);
	fit_scroll(w);
	scroll_to_item(w, w->cursor);
	update_menus(w);
	redraw(w);
}

static void reread(struct win *w) {
	read_dir(w, w->dir, true);
}

static void resort(struct win *w) {
	char *name = w->cursor >= 0 ? g_strdup(item_at_index(w, w->cursor)->name) : NULL;
	sort_items(w);
	w->cursor = w->anchor = -1;
	for (int i = 0; name != NULL && i < n_items(w); i++) {
		if (strcmp(item_at_index(w, i)->name, name) == 0) {
			w->cursor = w->anchor = i;
		}
	}
	g_free(name);
	apply_view(w);
}

static void close_window(struct win *w) {
	gtk_window_destroy(GTK_WINDOW(w->window));
}

static void run(struct win *w, int id) {
	if (gem_alert_up(w->host)) {
		return;
	}
	if (id >= ACT_PLACE && id - ACT_PLACE < (int)g_strv_length(app.places)) {
		char **pn = g_strsplit(app.places[id - ACT_PLACE], "\t", 2);
		GFile *f = g_file_new_for_path(pn[0]);
		read_dir(w, f, false);
		g_object_unref(f);
		g_strfreev(pn);
		return;
	}
	switch (id) {
	case ACT_NEW_WINDOW: {
		GFile *home = g_file_new_for_path(g_get_home_dir());
		win_new(home);
		g_object_unref(home);
		break;
	}
	case ACT_OPEN: open_selected(w, false); break;
	case ACT_INFO: show_info(w); break;
	case ACT_NEW_FOLDER: if (!in_trash(w)) new_folder(w); break;
	case ACT_TRASH: if (!in_trash(w)) trash_selected(w); break;
	case ACT_PUT_BACK: if (in_trash(w)) put_back(w); break;
	case ACT_DELETE: if (in_trash(w)) delete_selected(w); break;
	case ACT_EMPTY_TRASH: if (!trash_empty()) empty_trash(w); break;
	case ACT_CLOSE: close_window(w); break;
	case ACT_CUT: if (!in_trash(w)) clipboard_put(w, true); break;
	case ACT_COPY: clipboard_put(w, false); break;
	case ACT_PASTE: if (!in_trash(w)) paste(w); break;
	case ACT_SELECT_ALL:
		for (int i = 0; i < n_items(w); i++) {
			item_at_index(w, i)->selected = true;
		}
		update_menus(w);
		redraw(w);
		break;
	case ACT_ICONS:
	case ACT_TEXT:
		app.view = id == ACT_ICONS ? VIEW_ICONS : VIEW_TEXT;
		settings_save();
		each_win(apply_view);
		break;
	case ACT_SORT_NAME: case ACT_SORT_DATE: case ACT_SORT_SIZE:
	case ACT_SORT_TYPE:
		app.sort = id - ACT_SORT_NAME;
		settings_save();
		each_win(resort);
		break;
	case ACT_HIDDEN:
		app.hidden = !app.hidden;
		settings_save();
		each_win(reread);
		each_win(update_menus);
		break;
	case ACT_UP: go_up(w, false); break;
	case ACT_GO_TO: go_to_folder(w); break;
	case ACT_TRASH_FOLDER: {
		char *files = trash_dir("files");
		g_mkdir_with_parents(files, 0700);
		GFile *f = g_file_new_for_path(files);
		read_dir(w, f, false);
		g_object_unref(f);
		g_free(files);
		break;
	}
	}
}

static void menu_activate(uint32_t id, void *data) {
	run(data, (int)id);
}

/* The right-click menu: for what's under the pointer, or the folder. */
static void popup_chosen(int id, void *data) {
	if (id > 0) {
		run(data, id);
	}
}

static void show_popup(struct win *w, double x, double y, bool on_item) {
	bool trash = in_trash(w);
	gem_popup_clear(w->popup);
	if (on_item) {
		gem_popup_add(w->popup, ACT_OPEN, "Open", NULL, 0);
		gem_popup_add(w->popup, ACT_INFO, "Show Info...", NULL, 0);
		gem_popup_add_separator(w->popup);
		if (!trash) {
			gem_popup_add(w->popup, ACT_CUT, "Cut", NULL, 0);
		}
		gem_popup_add(w->popup, ACT_COPY, "Copy", NULL, 0);
		gem_popup_add_separator(w->popup);
		if (trash) {
			gem_popup_add(w->popup, ACT_PUT_BACK, "Put Back", NULL, 0);
			gem_popup_add(w->popup, ACT_DELETE, "Delete...", NULL, 0);
		} else {
			gem_popup_add(w->popup, ACT_TRASH, "Move to Trash", NULL, 0);
		}
	} else {
		gem_popup_add(w->popup, ACT_NEW_FOLDER, "New Folder...", NULL,
			trash ? GEM_POPUP_DISABLED : 0);
		gem_popup_add(w->popup, ACT_PASTE, "Paste", NULL,
			!trash && clipboard_has_files(w) ? 0 : GEM_POPUP_DISABLED);
		gem_popup_add_separator(w->popup);
		gem_popup_add(w->popup, ACT_ICONS, "Show as Icons", NULL,
			app.view == VIEW_ICONS ? GEM_POPUP_CHECKED : 0);
		gem_popup_add(w->popup, ACT_TEXT, "Show as Text", NULL,
			app.view == VIEW_TEXT ? GEM_POPUP_CHECKED : 0);
		if (trash) {
			gem_popup_add_separator(w->popup);
			gem_popup_add(w->popup, ACT_EMPTY_TRASH, "Empty Trash...", NULL,
				trash_empty() ? GEM_POPUP_DISABLED : 0);
		}
	}
	gem_popup_show(w->popup, x, y);
}

/* ---- The pointer -------------------------------------------------------------------- */

static void pick_only(struct win *w, int i) {
	select_none(w);
	if (i >= 0) {
		item_at_index(w, i)->selected = true;
	}
	w->cursor = w->anchor = i;
}

static void pick_run(struct win *w, int to) {
	int from = w->anchor >= 0 ? w->anchor : to;
	select_none(w);
	for (int i = MIN(from, to); i <= MAX(from, to); i++) {
		item_at_index(w, i)->selected = true;
	}
	w->cursor = to;
}

static void pressed(GtkGestureClick *g, int n_press, double x, double y,
		gpointer data) {
	struct win *w = data;
	gtk_widget_grab_focus(w->area);
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
	GdkModifierType mods = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(g));
	int i = item_at(w, x, y);
	w->pending_single = -1;
	if (button == GDK_BUTTON_SECONDARY) {
		if (i >= 0 && !item_at_index(w, i)->selected) {
			pick_only(w, i);
		} else if (i < 0) {
			select_none(w);
		}
		update_menus(w);
		redraw(w);
		show_popup(w, x, y, i >= 0);
		return;
	}
	if (n_press == 2 && i >= 0) {
		if (!item_at_index(w, i)->selected) {
			pick_only(w, i);
		}
		open_selected(w, mods & GDK_CONTROL_MASK);
		return;
	}
	if (mods & GDK_SHIFT_MASK && i >= 0) {
		pick_run(w, i);
	} else if (mods & GDK_CONTROL_MASK) {
		if (i >= 0) {
			struct item *it = item_at_index(w, i);
			it->selected = !it->selected;
			w->cursor = w->anchor = i;
		}
	} else if (i < 0) {
		select_none(w);
		w->cursor = w->anchor = -1;
	} else if (item_at_index(w, i)->selected) {
		w->pending_single = i; /* alone, if this isn't a drag */
		w->cursor = w->anchor = i;
	} else {
		pick_only(w, i);
	}
	update_menus(w);
	redraw(w);
}

static void released(GtkGestureClick *g, int n_press, double x, double y,
		gpointer data) {
	struct win *w = data;
	if (w->pending_single >= 0 && w->pending_single == item_at(w, x, y) &&
			n_press == 1) {
		pick_only(w, w->pending_single);
		update_menus(w);
		redraw(w);
	}
	w->pending_single = -1;
}

/* Dragging a box round things, from somewhere there's nothing. */
static void band_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	struct win *w = data;
	if (item_at(w, x, y) >= 0) {
		gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	GdkModifierType mods = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(g));
	w->band = true;
	w->bx0 = w->bx1 = x;
	w->by0 = w->by1 = y + scroll_y(w);
	g_free(w->band_base);
	w->band_base = g_new0(bool, MAX(n_items(w), 1));
	for (int i = 0; i < n_items(w); i++) {
		w->band_base[i] = (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
			item_at_index(w, i)->selected;
	}
}

static void band_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct win *w = data;
	if (!w->band) {
		return;
	}
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	w->bx1 = sx + dx;
	w->by1 = sy + dy + scroll_y(w);
	double x0 = MIN(w->bx0, w->bx1), x1 = MAX(w->bx0, w->bx1);
	double y0 = MIN(w->by0, w->by1), y1 = MAX(w->by0, w->by1);
	lay_out(w);
	for (int i = 0; i < n_items(w); i++) {
		struct box b;
		hit_box(w, i, &b);
		bool in = b.x < x1 && b.x + b.w > x0 && b.y < y1 && b.y + b.h > y0;
		item_at_index(w, i)->selected = w->band_base[i] || in;
	}
	/* Near an edge, it scrolls. */
	int h = gtk_widget_get_height(w->area);
	double py = sy + dy;
	if (py < 0) {
		gtk_adjustment_set_value(w->vadj, scroll_y(w) + py);
	} else if (py > h) {
		gtk_adjustment_set_value(w->vadj, scroll_y(w) + py - h);
	}
	update_menus(w);
	redraw(w);
}

static void band_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct win *w = data;
	if (w->band) {
		w->band = false;
		w->cursor = w->anchor = first_selected(w);
		redraw(w);
	}
}

static gboolean scrolled(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct win *w = data;
	double step = app.view == VIEW_TEXT ? 3 * GEM_ROW_H : CELL_H / 2.0;
	gtk_adjustment_set_value(w->vadj, scroll_y(w) + dy * step);
	return TRUE;
}

/* ---- Dragging things out, and dropping them in ------------------------------------ */

/* What's being dragged, drawn: its icon, and how many. */
static GdkPaintable *drag_icon(struct win *w, int i, int count) {
	int s = 1, wd = ICON + 24, ht = ICON + 4;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		wd * s, ht * s);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, s, s);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	gem_set_font(cr);
	paint_icon(cr, item_at_index(w, i), 0, 2, false);
	if (count > 1) {
		char *n = g_strdup_printf("%d", count);
		int tw = (int)gem_text_width(cr, n);
		gem_black(cr);
		gem_fill(cr, ICON - 4, 0, tw + 6, LABEL_H);
		gem_white(cr);
		gem_text(cr, n, ICON - 1, 0, LABEL_H);
		g_free(n);
	}
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	GBytes *bytes = g_bytes_new(cairo_image_surface_get_data(surface),
		cairo_image_surface_get_stride(surface) * ht * s);
	GdkTexture *t = gdk_memory_texture_new(wd * s, ht * s,
		GDK_MEMORY_DEFAULT, bytes, cairo_image_surface_get_stride(surface));
	g_bytes_unref(bytes);
	cairo_surface_destroy(surface);
	return GDK_PAINTABLE(t);
}

static GdkContentProvider *drag_prepare(GtkDragSource *src, double x, double y,
		gpointer data) {
	struct win *w = data;
	int i = item_at(w, x, y);
	if (i < 0 || gem_alert_up(w->host)) {
		return NULL;
	}
	if (!item_at_index(w, i)->selected) {
		pick_only(w, i);
	}
	w->pending_single = -1;
	GList *files = selected_files(w);
	GSList *list = NULL;
	for (GList *l = g_list_last(files); l != NULL; l = l->prev) {
		list = g_slist_prepend(list, l->data);
	}
	GdkFileList *fl = gdk_file_list_new_from_list(list);
	GdkContentProvider *p = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
	g_boxed_free(GDK_TYPE_FILE_LIST, fl);
	g_slist_free(list);
	g_list_free_full(files, g_object_unref);
	GdkPaintable *icon = drag_icon(w, i, n_selected(w));
	gtk_drag_source_set_icon(src, icon, ICON / 2, ICON / 2);
	g_object_unref(icon);
	app.dragging = w;
	redraw(w);
	return p;
}

static void drag_end(GtkDragSource *src, GdkDrag *drag, gboolean delete,
		gpointer data) {
	app.dragging = NULL;
}

/* Where a drop at x, y goes: a folder under it (not one being dragged,
 * if the drag's from here), else this window's folder. */
static GFile *drop_dir(struct win *w, double x, double y, int *hover) {
	int i = item_at(w, x, y);
	if (i >= 0 && item_at_index(w, i)->dir &&
			!(app.dragging == w && item_at_index(w, i)->selected)) {
		*hover = i;
		return child(w, i);
	}
	*hover = -1;
	return g_object_ref(w->dir);
}

static GdkDragAction drop_motion(GtkDropTarget *t, double x, double y,
		gpointer data) {
	struct win *w = data;
	int hover;
	GFile *dir = drop_dir(w, x, y, &hover);
	if (hover != w->drop_hover) {
		w->drop_hover = hover;
		redraw(w);
	}
	GdkDragAction action = 0;
	const GValue *v = gtk_drop_target_get_value(t);
	GSList *files = v != NULL && G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST) ?
		gdk_file_list_get_files(g_value_get_boxed(v)) : NULL;
	if (files != NULL && !in_trash(w)) {
		GFile *first = files->data;
		GFile *parent = g_file_get_parent(first);
		bool here = parent != NULL && g_file_equal(parent, dir);
		g_clear_object(&parent);
		GdkModifierType mods = gtk_event_controller_get_current_event_state(
			GTK_EVENT_CONTROLLER(t));
		if (mods & GDK_CONTROL_MASK) {
			action = GDK_ACTION_COPY;
		} else if (!here) {
			action = same_disk(first, dir) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
		}
	}
	g_slist_free(files);
	g_object_unref(dir);
	w->drop_action = action;
	return action;
}

static void drop_leave(GtkDropTarget *t, gpointer data) {
	struct win *w = data;
	w->drop_hover = -1;
	redraw(w);
}

static gboolean drop(GtkDropTarget *t, const GValue *value, double x, double y,
		gpointer data) {
	struct win *w = data;
	int hover;
	GFile *dir = drop_dir(w, x, y, &hover);
	w->drop_hover = -1;
	redraw(w);
	if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST) || w->drop_action == 0) {
		g_object_unref(dir);
		return FALSE;
	}
	GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
	GList *list = NULL;
	for (GSList *l = files; l != NULL; l = l->next) {
		list = g_list_append(list, l->data);
	}
	transfer(w, list, dir, w->drop_action == GDK_ACTION_COPY);
	g_list_free(list);
	g_slist_free(files);
	g_object_unref(dir);
	return TRUE;
}

/* ---- Keys ------------------------------------------------------------------------ */

static void move_cursor(struct win *w, int to, bool extend) {
	if (n_items(w) == 0) {
		return;
	}
	to = CLAMP(to, 0, n_items(w) - 1);
	if (extend) {
		pick_run(w, to);
	} else {
		pick_only(w, to);
	}
	scroll_to_item(w, to);
	update_menus(w);
	redraw(w);
}

/* Typing a name goes to the first thing starting with what's typed. */
static void type_select(struct win *w, guint32 c) {
	gint64 now = g_get_monotonic_time() / 1000;
	if (now - w->typed_at > TYPE_PAUSE) {
		w->typed[0] = '\0';
	}
	w->typed_at = now;
	char buf[8];
	int n = g_unichar_to_utf8(g_unichar_tolower(c), buf);
	buf[n] = '\0';
	if (strlen(w->typed) + n < sizeof(w->typed)) {
		strcat(w->typed, buf);
	}
	for (int i = 0; i < n_items(w); i++) {
		char *lower = g_utf8_strdown(item_at_index(w, i)->display, -1);
		bool match = g_str_has_prefix(lower, w->typed);
		g_free(lower);
		if (match) {
			move_cursor(w, i, false);
			return;
		}
	}
}

static gboolean key_pressed(GtkEventControllerKey *c, guint key, guint code,
		GdkModifierType mods, gpointer data) {
	struct win *w = data;
	if (gem_alert_up(w->host)) {
		return FALSE;
	}
	bool ctrl = mods & GDK_CONTROL_MASK, shift = mods & GDK_SHIFT_MASK;
	if (ctrl) {
		switch (gdk_keyval_to_lower(key)) {
		case GDK_KEY_n: run(w, shift ? ACT_NEW_FOLDER : ACT_NEW_WINDOW); return TRUE;
		case GDK_KEY_o: run(w, ACT_OPEN); return TRUE;
		case GDK_KEY_i: run(w, ACT_INFO); return TRUE;
		case GDK_KEY_w: run(w, ACT_CLOSE); return TRUE;
		case GDK_KEY_x: run(w, ACT_CUT); return TRUE;
		case GDK_KEY_c: run(w, ACT_COPY); return TRUE;
		case GDK_KEY_v: run(w, ACT_PASTE); return TRUE;
		case GDK_KEY_a: run(w, ACT_SELECT_ALL); return TRUE;
		case GDK_KEY_h: run(w, ACT_HIDDEN); return TRUE;
		case GDK_KEY_l: run(w, ACT_GO_TO); return TRUE;
		case GDK_KEY_Up: run(w, ACT_UP); return TRUE;
		case GDK_KEY_Down: run(w, ACT_OPEN); return TRUE;
		}
		return FALSE;
	}
	int cur = w->cursor >= 0 ? w->cursor : -1;
	int across = app.view == VIEW_ICONS ? w->cols : 1;
	int page = MAX(1, (int)(gtk_widget_get_height(w->area) /
		(app.view == VIEW_ICONS ? CELL_H : GEM_ROW_H))) * across;
	switch (key) {
	case GDK_KEY_Return: case GDK_KEY_KP_Enter:
		open_selected(w, false);
		return TRUE;
	case GDK_KEY_BackSpace:
		go_up(w, false);
		return TRUE;
	case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
		run(w, in_trash(w) ? ACT_DELETE : ACT_TRASH);
		return TRUE;
	case GDK_KEY_F2:
		show_info(w);
		return TRUE;
	case GDK_KEY_Escape:
		select_none(w);
		update_menus(w);
		redraw(w);
		return TRUE;
	case GDK_KEY_Left:
		if (app.view == VIEW_ICONS) {
			move_cursor(w, cur < 0 ? 0 : cur - 1, shift);
		}
		return TRUE;
	case GDK_KEY_Right:
		if (app.view == VIEW_ICONS) {
			move_cursor(w, cur < 0 ? 0 : cur + 1, shift);
		}
		return TRUE;
	case GDK_KEY_Up:
		move_cursor(w, cur < 0 ? 0 : cur - across >= 0 ? cur - across : cur, shift);
		return TRUE;
	case GDK_KEY_Down:
		move_cursor(w, cur < 0 ? 0 : cur + across < n_items(w) ? cur + across : cur,
			shift);
		return TRUE;
	case GDK_KEY_Home:
		move_cursor(w, 0, shift);
		return TRUE;
	case GDK_KEY_End:
		move_cursor(w, n_items(w) - 1, shift);
		return TRUE;
	case GDK_KEY_Page_Up:
		move_cursor(w, cur < 0 ? 0 : cur - page, shift);
		return TRUE;
	case GDK_KEY_Page_Down:
		move_cursor(w, cur < 0 ? 0 : cur + page, shift);
		return TRUE;
	}
	guint32 u = gdk_keyval_to_unicode(key);
	if (u >= 0x20 && !(mods & GDK_ALT_MASK)) {
		type_select(w, u);
		return TRUE;
	}
	return FALSE;
}

/* ---- Windows ---------------------------------------------------------------------- */

/* GEM's close box: up a folder, and closed only at the top. */
static gboolean close_request(GtkWindow *window, gpointer data) {
	struct win *w = data;
	if (gem_alert_up(w->host)) {
		return TRUE;
	}
	return go_up(w, true);
}

static void win_free(gpointer data, GObject *gone) {
	struct win *w = data;
	app.wins = g_list_remove(app.wins, w);
	if (app.dragging == w) {
		app.dragging = NULL;
	}
	if (w->monitor != NULL) {
		g_signal_handlers_disconnect_by_data(w->monitor, w);
		g_file_monitor_cancel(w->monitor);
		g_object_unref(w->monitor);
	}
	if (w->reload_id != 0) {
		g_source_remove(w->reload_id);
	}
	if (w->message_id != 0) {
		g_source_remove(w->message_id);
	}
	g_array_unref(w->items);
	g_clear_object(&w->dir);
	g_free(w->message);
	g_strfreev(w->select_after);
	g_free(w->band_base);
	g_free(w);
}

static void clipboard_changed(GdkClipboard *c, gpointer data) {
	each_win(update_menus);
}

static void resized(GtkDrawingArea *a, int width, int height, gpointer data) {
	struct win *w = data;
	lay_out(w);
	fit_scroll(w);
}

static struct win *win_new(GFile *dir) {
	struct win *w = g_new0(struct win, 1);
	w->items = g_array_new(FALSE, TRUE, sizeof(struct item));
	g_array_set_clear_func(w->items, item_clear);
	w->cursor = w->anchor = w->pending_single = w->drop_hover = -1;
	w->cols = 1;

	w->window = gtk_application_window_new(app.app);
	g_object_set_data(G_OBJECT(w->window), "gemfiles-win", w);
	gtk_widget_add_css_class(w->window, "gem");
	gtk_window_set_default_size(GTK_WINDOW(w->window), 560, 400);

	w->area = gtk_drawing_area_new();
	gtk_widget_set_focusable(w->area, TRUE);
	gtk_widget_set_hexpand(w->area, TRUE);
	gtk_widget_set_vexpand(w->area, TRUE);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->area), draw, w, NULL);
	g_signal_connect(w->area, "resize", G_CALLBACK(resized), w);

	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), w);
	g_signal_connect(click, "released", G_CALLBACK(released), w);
	gtk_widget_add_controller(w->area, GTK_EVENT_CONTROLLER(click));

	GtkGesture *band = gtk_gesture_drag_new();
	g_signal_connect(band, "drag-begin", G_CALLBACK(band_begin), w);
	g_signal_connect(band, "drag-update", G_CALLBACK(band_update), w);
	g_signal_connect(band, "drag-end", G_CALLBACK(band_end), w);
	gtk_widget_add_controller(w->area, GTK_EVENT_CONTROLLER(band));

	GtkDragSource *src = gtk_drag_source_new();
	gtk_drag_source_set_actions(src, GDK_ACTION_COPY | GDK_ACTION_MOVE);
	g_signal_connect(src, "prepare", G_CALLBACK(drag_prepare), w);
	g_signal_connect(src, "drag-end", G_CALLBACK(drag_end), w);
	gtk_widget_add_controller(w->area, GTK_EVENT_CONTROLLER(src));

	GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
		GDK_ACTION_COPY | GDK_ACTION_MOVE);
	gtk_drop_target_set_preload(dt, TRUE);
	g_signal_connect(dt, "enter", G_CALLBACK(drop_motion), w);
	g_signal_connect(dt, "motion", G_CALLBACK(drop_motion), w);
	g_signal_connect(dt, "leave", G_CALLBACK(drop_leave), w);
	g_signal_connect(dt, "drop", G_CALLBACK(drop), w);
	gtk_widget_add_controller(w->area, GTK_EVENT_CONTROLLER(dt));

	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(wheel, "scroll", G_CALLBACK(scrolled), w);
	gtk_widget_add_controller(w->area, wheel);

	w->vadj = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
	g_signal_connect_swapped(w->vadj, "value-changed",
		G_CALLBACK(gtk_widget_queue_draw), w->area);

	w->info = gem_pixel_area_new(0, INFO_H, paint_info, w);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(row), w->area);
	gtk_box_append(GTK_BOX(row),
		gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, w->vadj));
	gtk_widget_set_vexpand(row, TRUE);
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(outer), row);
	gtk_box_append(GTK_BOX(outer), w->info);
	w->host = GTK_OVERLAY(gtk_overlay_new());
	gtk_overlay_set_child(w->host, outer);
	gtk_window_set_child(GTK_WINDOW(w->window), GTK_WIDGET(w->host));

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), w);
	gtk_widget_add_controller(w->window, keys);
	g_signal_connect(w->window, "close-request", G_CALLBACK(close_request), w);
	g_object_weak_ref(G_OBJECT(w->window), win_free, w);

	w->popup = gem_popup_new(w->area, popup_chosen, w);
	app.wins = g_list_append(app.wins, w);
	w->menu = app_menu_new(w->window, build_menus, menu_activate, w);
	g_signal_connect_object(gtk_widget_get_clipboard(w->window), "changed",
		G_CALLBACK(clipboard_changed), w->window, 0);

	if (!read_dir(w, dir, false)) {
		GFile *home = g_file_new_for_path(g_get_home_dir());
		read_dir(w, home, false);
		g_object_unref(home);
	}
	gtk_window_present(GTK_WINDOW(w->window));
	gtk_widget_grab_focus(w->area);
	return w;
}

/* ---- The application ------------------------------------------------------------ */

static void activate(GtkApplication *a, gpointer data) {
	GFile *home = g_file_new_for_path(g_get_home_dir());
	win_new(home);
	g_object_unref(home);
}

/* A folder in a window; a file, its folder with it picked. */
static void open_files(GApplication *a, GFile **files, int n, const char *hint,
		gpointer data) {
	for (int i = 0; i < n; i++) {
		if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) ==
				G_FILE_TYPE_DIRECTORY) {
			win_new(files[i]);
			continue;
		}
		GFile *parent = g_file_get_parent(files[i]);
		if (parent == NULL) {
			continue;
		}
		struct win *w = win_new(parent);
		char *name = g_file_get_basename(files[i]);
		char *names[] = { name, NULL };
		w->select_after = g_strdupv(names);
		read_dir(w, parent, true);
		scroll_to_item(w, w->cursor);
		g_free(name);
		g_object_unref(parent);
	}
}

static void mounts_changed(GVolumeMonitor *vm, GMount *mount, gpointer data) {
	g_strfreev(app.places);
	app.places = places();
	each_win(update_menus);
}

static void startup(GApplication *a, gpointer data) {
	gem_ui_load_css();
	settings_load();
	app.volumes = g_volume_monitor_get();
	app.places = places();
	g_signal_connect(app.volumes, "mount-added", G_CALLBACK(mounts_changed), NULL);
	g_signal_connect(app.volumes, "mount-removed", G_CALLBACK(mounts_changed),
		NULL);
	g_signal_connect(app.volumes, "mount-changed", G_CALLBACK(mounts_changed),
		NULL);
}

int main(int argc, char *argv[]) {
	g_set_application_name("GemFiles");
	app.app = gtk_application_new("org.gemwm.Files", G_APPLICATION_HANDLES_OPEN);
	g_application_set_option_context_parameter_string(G_APPLICATION(app.app),
		"[FOLDER...]");
	g_signal_connect(app.app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app.app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app.app, "open", G_CALLBACK(open_files), NULL);
	int status = g_application_run(G_APPLICATION(app.app), argc, argv);
	g_object_unref(app.app);
	return status;
}
