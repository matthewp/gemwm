#include <string.h>
#include "gem-alert.h"
#include "gem-file.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"

#define BOX_W 540
#define LIST_W 300       /* the list's frame, with its scroll bar */
#define ROWS 12
#define INNER (GEM_BORDER + 2 * GEM_PAD)

enum { HIT_ROW, HIT_CLOSE, HIT_OK, HIT_CANCEL };

struct entry {
	char *name;
	bool folder;
};

struct selector {
	GtkOverlay *host;
	GtkWidget *box, *area, *dir_field, *name_field, *scroll;
	GtkAdjustment *adj;  /* the first row shown */
	char *title, *folder, *pattern;
	bool save;
	GArray *entries;     /* struct entry: folders, then files */
	int selected;        /* in entries, or -1 */
	GArray *hits;
	gem_file_fn done;
	void *data;
};

static char *last_folder;

/* ---- Where things are ---------------------------------------------------- */

static int dir_label_y(void) { return INNER + GEM_ROW_H + GEM_PAD; }
static int dir_field_y(void) { return dir_label_y() + GEM_ROW_H; }
static int list_y(void) { return dir_field_y() + GEM_FIELD_H + 2 * GEM_PAD; }
static int rows_y(void) { return list_y() + 1 + GEM_ROW_H + 1; }
static int list_h(void) { return rows_y() - list_y() + ROWS * GEM_ROW_H + 1; }
static int right_x(void) { return INNER + LIST_W + 2 * GEM_PAD; }
static int right_w(void) { return BOX_W - INNER - right_x(); }
static int box_h(void) { return list_y() + list_h() + INNER; }

/* ---- The folder ---------------------------------------------------------- */

static void entry_clear(gpointer p) {
	g_free(((struct entry *)p)->name);
}

static int entry_cmp(gconstpointer a, gconstpointer b) {
	const struct entry *x = a, *y = b;
	if (x->folder != y->folder) {
		return x->folder ? -1 : 1;
	}
	char *cx = g_utf8_casefold(x->name, -1), *cy = g_utf8_casefold(y->name, -1);
	int r = g_utf8_collate(cx, cy);
	g_free(cx);
	g_free(cy);
	return r;
}

static bool matches(const char *pattern, const char *name) {
	char **globs = g_strsplit(pattern, ",", -1);
	bool any = false;
	for (int i = 0; globs[i] != NULL && !any; i++) {
		char *glob = g_strstrip(globs[i]);
		char *lower = g_ascii_strdown(name, -1);
		char *lglob = g_ascii_strdown(glob, -1);
		any = glob[0] != '\0' && g_pattern_match_simple(lglob, lower);
		g_free(lower);
		g_free(lglob);
	}
	g_strfreev(globs);
	return any;
}

/* The folder as the directory line shows it: home as ~. */
static char *shown_path(const char *path) {
	const char *home = g_get_home_dir();
	size_t n = strlen(home);
	if (strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0')) {
		return g_strconcat("~", path + n, NULL);
	}
	return g_strdup(path);
}

static void show_directory_line(struct selector *s) {
	char *shown = shown_path(s->folder);
	char *line = g_build_filename(shown, s->pattern, NULL);
	gtk_editable_set_text(GTK_EDITABLE(s->dir_field), line);
	g_free(line);
	g_free(shown);
}

static void read_folder(struct selector *s) {
	g_array_set_size(s->entries, 0);
	GDir *dir = g_dir_open(s->folder, 0, NULL);
	const char *name;
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		if (name[0] == '.') {
			continue;
		}
		char *path = g_build_filename(s->folder, name, NULL);
		bool folder = g_file_test(path, G_FILE_TEST_IS_DIR);
		g_free(path);
		if (folder || matches(s->pattern, name)) {
			struct entry e = { g_strdup(name), folder };
			g_array_append_val(s->entries, e);
		}
	}
	if (dir != NULL) {
		g_dir_close(dir);
	}
	g_array_sort(s->entries, entry_cmp);
	s->selected = -1;
	/* At least a page: GTK refuses a page bigger than the range. */
	gtk_adjustment_configure(s->adj, 0, 0, MAX(s->entries->len, ROWS), 1,
		ROWS - 1, ROWS);
	show_directory_line(s);
	gtk_widget_queue_draw(s->area);
}

static void go_to(struct selector *s, const char *folder) {
	char *canon = g_canonicalize_filename(folder, NULL);
	g_free(s->folder);
	s->folder = canon;
	read_folder(s);
}

static void go_up(struct selector *s) {
	char *parent = g_path_get_dirname(s->folder);
	char *was = g_path_get_basename(s->folder);
	go_to(s, parent);
	/* The folder just left, selected. */
	for (guint i = 0; i < s->entries->len; i++) {
		if (strcmp(g_array_index(s->entries, struct entry, i).name, was) == 0) {
			s->selected = i;
			gtk_adjustment_set_value(s->adj, MAX(0, (int)i - ROWS / 2));
		}
	}
	g_free(parent);
	g_free(was);
}

/* The directory line typed: a folder, and maybe a pattern after it. */
static void directory_typed(struct selector *s) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(s->dir_field));
	char *path = text[0] == '~' ?
		g_strconcat(g_get_home_dir(), text + 1, NULL) : g_strdup(text);
	char *base = g_path_get_basename(path);
	if (strpbrk(base, "*?") != NULL) {
		g_free(s->pattern);
		s->pattern = g_strdup(base);
		char *dir = g_path_get_dirname(path);
		g_free(path);
		path = dir;
	}
	g_free(base);
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		go_to(s, path);
	} else {
		gtk_widget_error_bell(s->area);
		read_folder(s);
	}
	g_free(path);
}

/* ---- Drawing ------------------------------------------------------------- */

/* A folder's mark: GEM's diamond. */
static void diamond(cairo_t *cr, int x, int y) {
	for (int r = 0; r < 7; r++) {
		int half = r < 4 ? r : 6 - r;
		gem_fill(cr, x + 3 - half, y + r, 2 * half + 1, 1);
	}
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct selector *s = data;
	g_array_set_size(s->hits, 0);
	gem_dialog_frame(cr, w, h);
	gem_text(cr, s->title, (w - (int)gem_text_width(cr, s->title)) / 2, INNER,
		GEM_ROW_H);
	gem_text(cr, "Directory:", INNER, dir_label_y(), GEM_ROW_H);
	gem_text(cr, "Selection:", right_x(), list_y(), GEM_ROW_H);

	/* The list: a window of its own, with a close box and the folder's
	 * name in its title bar. */
	int lx = INNER, ly = list_y(), lw = LIST_W, lh = list_h();
	gem_frame(cr, lx, ly, lw, lh, 1);
	gem_fill(cr, lx, ly + GEM_ROW_H + 1, lw, 1);
	gem_frame(cr, lx, ly, GEM_ROW_H + 1, GEM_ROW_H + 2, 1);
	for (int i = 4; i < GEM_ROW_H - 3; i++) { /* the close box's cross */
		gem_fill(cr, lx + i, ly + i, 1, 1);
		gem_fill(cr, lx + GEM_ROW_H - i, ly + i, 1, 1);
	}
	gem_hit_add(s->hits, lx, ly, GEM_ROW_H + 1, GEM_ROW_H + 2, HIT_CLOSE, 0);
	char *base = strcmp(s->folder, "/") == 0 ? g_strdup("/") :
		g_path_get_basename(s->folder);
	int tx = lx + GEM_ROW_H + 1 + GEM_PAD;
	int tw = (int)gem_text_width(cr, base);
	int avail = lw - GEM_ROW_H - 1 - 2 * GEM_PAD;
	gem_text_clipped(cr, base, tx + MAX(0, (avail - tw) / 2), ly + 1,
		avail, GEM_ROW_H);
	g_free(base);

	int rows_w = lw - GEM_GADGET - 1;
	int top = (int)gtk_adjustment_get_value(s->adj);
	for (int r = 0; r < ROWS; r++) {
		int i = top + r;
		if (i >= (int)s->entries->len) {
			break;
		}
		struct entry *e = &g_array_index(s->entries, struct entry, i);
		int y = rows_y() + r * GEM_ROW_H;
		gem_black(cr);
		if (i == s->selected) {
			gem_fill(cr, lx + 1, y, rows_w - 1, GEM_ROW_H);
			gem_white(cr);
		}
		if (e->folder) {
			diamond(cr, lx + GEM_PAD, y + (GEM_ROW_H - 7) / 2);
		}
		gem_text_clipped(cr, e->name, lx + GEM_PAD + 12, y,
			rows_w - GEM_PAD - 14, GEM_ROW_H);
		gem_hit_add(s->hits, lx + 1, y, rows_w - 1, GEM_ROW_H, HIT_ROW, i);
	}
	gem_black(cr);
	if (s->entries->len == 0) {
		gem_text(cr, "(nothing here)", lx + GEM_PAD + 12, rows_y(), GEM_ROW_H);
		gem_grey_out(cr, lx + 1, rows_y(), rows_w - 1, GEM_ROW_H);
	}

	int bw = MAX(gem_button_width("Cancel"), gem_button_width("OK")) + GEM_PAD;
	int bx = right_x() + (right_w() - bw) / 2;
	int by = ly + lh - 2 * GEM_BUTTON_H - GEM_PAD;
	gem_button(cr, s->hits, bx, by, bw, "OK", HIT_OK, true);
	gem_button(cr, s->hits, bx, by + GEM_BUTTON_H + GEM_PAD, bw, "Cancel",
		HIT_CANCEL, false);
}

/* ---- Choosing ------------------------------------------------------------ */

static void finish(struct selector *s, const char *path) {
	if (path != NULL) {
		g_free(last_folder);
		last_folder = g_strdup(s->folder);
	}
	gem_release(s->host, s->box);
	if (s->done != NULL) {
		s->done(path, s->data);
	}
	g_array_unref(s->entries);
	g_array_unref(s->hits);
	g_object_unref(s->adj);
	g_free(s->title);
	g_free(s->folder);
	g_free(s->pattern);
	g_free(s);
}

struct replacing {
	struct selector *s;
	char *path;
};

static void replace_answered(int button, void *data) {
	struct replacing *r = data;
	if (button == 0) {
		finish(r->s, r->path);
	} else {
		gtk_widget_grab_focus(r->s->name_field);
	}
	g_free(r->path);
	g_free(r);
}

static void ok(struct selector *s) {
	const char *name = gtk_editable_get_text(GTK_EDITABLE(s->name_field));
	if (name[0] == '\0') {
		if (s->selected >= 0) {
			struct entry *e = &g_array_index(s->entries, struct entry, s->selected);
			if (e->folder) {
				char *path = g_build_filename(s->folder, e->name, NULL);
				go_to(s, path);
				g_free(path);
				return;
			}
		}
		gtk_widget_error_bell(s->area);
		return;
	}
	char *path = name[0] == '/' ? g_strdup(name) : name[0] == '~' ?
		g_strconcat(g_get_home_dir(), name + 1, NULL) :
		g_build_filename(s->folder, name, NULL);
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		gtk_editable_set_text(GTK_EDITABLE(s->name_field), "");
		go_to(s, path);
		g_free(path);
		return;
	}
	bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
	if (!s->save && !exists) {
		char *msg = g_strdup_printf("There's no file called \"%s\" here.", name);
		static const char *const buttons[] = { "OK", NULL };
		gem_alert(s->host, GEM_ALERT_NOTE, msg, buttons, 0, 0, NULL, NULL);
		g_free(msg);
		g_free(path);
		return;
	}
	if (s->save && exists) {
		char *base = g_path_get_basename(path);
		char *msg = g_strdup_printf("\"%s\" is already there.\nReplace it?", base);
		struct replacing *r = g_new(struct replacing, 1);
		r->s = s;
		r->path = path;
		static const char *const buttons[] = { "Replace", "Cancel", NULL };
		gem_alert(s->host, GEM_ALERT_QUESTION, msg, buttons, 1, 1,
			replace_answered, r);
		g_free(msg);
		g_free(base);
		return;
	}
	finish(s, path);
	g_free(path);
}

static void select_entry(struct selector *s, int i) {
	if (i < 0 || i >= (int)s->entries->len) {
		return;
	}
	s->selected = i;
	struct entry *e = &g_array_index(s->entries, struct entry, i);
	if (!e->folder) {
		gtk_editable_set_text(GTK_EDITABLE(s->name_field), e->name);
	}
	int top = (int)gtk_adjustment_get_value(s->adj);
	if (i < top) {
		gtk_adjustment_set_value(s->adj, i);
	} else if (i >= top + ROWS) {
		gtk_adjustment_set_value(s->adj, i - ROWS + 1);
	}
	gtk_widget_queue_draw(s->area);
}

static void pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct selector *s = data;
	const struct gem_hit *hit = gem_hit_at(s->hits, x, y);
	if (hit == NULL) {
		return;
	}
	switch (hit->id) {
	case HIT_ROW: {
		int i = hit->index;
		select_entry(s, i);
		struct entry *e = &g_array_index(s->entries, struct entry, i);
		if (n == 2 && e->folder) {
			char *path = g_build_filename(s->folder, e->name, NULL);
			go_to(s, path);
			g_free(path);
		} else if (n == 2) {
			ok(s);
		}
		break;
	}
	case HIT_CLOSE:
		go_up(s);
		break;
	case HIT_OK:
		ok(s);
		break;
	case HIT_CANCEL:
		finish(s, NULL);
		break;
	}
}

static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	struct selector *s = data;
	gtk_adjustment_set_value(s->adj, gtk_adjustment_get_value(s->adj) + dy * 3);
	return TRUE;
}

static void scrolled(GtkAdjustment *adj, gpointer data) {
	struct selector *s = data;
	gtk_widget_queue_draw(s->area);
}

static gboolean key(GtkEventControllerKey *c, guint keyval, guint keycode,
		GdkModifierType state, gpointer data) {
	struct selector *s = data;
	int n = s->entries->len;
	switch (keyval) {
	case GDK_KEY_Escape:
		finish(s, NULL);
		return TRUE;
	case GDK_KEY_Up:
		select_entry(s, s->selected < 0 ? n - 1 : MAX(s->selected - 1, 0));
		return TRUE;
	case GDK_KEY_Down:
		select_entry(s, MIN(s->selected + 1, n - 1));
		return TRUE;
	case GDK_KEY_BackSpace:
		/* In an empty selection, up a folder, like the close box. */
		if (gtk_editable_get_text(GTK_EDITABLE(s->name_field))[0] == '\0' &&
				gtk_widget_has_focus(GTK_WIDGET(gtk_editable_get_delegate(
					GTK_EDITABLE(s->name_field))))) {
			go_up(s);
			return TRUE;
		}
		return FALSE;
	}
	return FALSE;
}

static void name_activated(GtkEntry *e, gpointer data) {
	struct selector *s = data;
	const char *name = gtk_editable_get_text(GTK_EDITABLE(s->name_field));
	if (name[0] == '\0' && s->selected >= 0) {
		struct entry *en = &g_array_index(s->entries, struct entry, s->selected);
		if (en->folder) {
			char *path = g_build_filename(s->folder, en->name, NULL);
			go_to(s, path);
			g_free(path);
			return;
		}
	}
	ok(s);
}

static void dir_activated(GtkEntry *e, gpointer data) {
	struct selector *s = data;
	directory_typed(s);
	gtk_widget_grab_focus(s->name_field);
}

void gem_file_select(GtkOverlay *host, const char *title, bool save,
		const char *folder, const char *name, const char *pattern,
		gem_file_fn done, void *data) {
	struct selector *s = g_new0(struct selector, 1);
	s->host = host;
	s->title = g_strdup(title);
	s->save = save;
	s->pattern = g_strdup(pattern != NULL ? pattern : "*");
	s->done = done;
	s->data = data;
	s->selected = -1;
	s->hits = gem_hits_new();
	s->entries = g_array_new(FALSE, TRUE, sizeof(struct entry));
	g_array_set_clear_func(s->entries, entry_clear);
	s->adj = g_object_ref_sink(gtk_adjustment_new(0, 0, ROWS, 1, ROWS - 1, ROWS));
	g_signal_connect(s->adj, "value-changed", G_CALLBACK(scrolled), s);

	s->box = gtk_overlay_new();
	s->area = gem_pixel_area_new(BOX_W, box_h(), paint, s);
	gtk_overlay_set_child(GTK_OVERLAY(s->box), s->area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), s);
	gtk_widget_add_controller(s->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(wheel), s);
	gtk_widget_add_controller(s->area, scroll);

	s->dir_field = gem_field_new(INNER, dir_field_y(), BOX_W - 2 * INNER);
	g_signal_connect(s->dir_field, "activate", G_CALLBACK(dir_activated), s);
	gtk_overlay_add_overlay(GTK_OVERLAY(s->box), s->dir_field);
	s->name_field = gem_field_new(right_x(), list_y() + GEM_ROW_H, right_w());
	g_signal_connect(s->name_field, "activate", G_CALLBACK(name_activated), s);
	gtk_overlay_add_overlay(GTK_OVERLAY(s->box), s->name_field);

	s->scroll = gem_scrollbar_new(GTK_ORIENTATION_VERTICAL, s->adj);
	gtk_widget_set_halign(s->scroll, GTK_ALIGN_START);
	gtk_widget_set_valign(s->scroll, GTK_ALIGN_START);
	gtk_widget_set_vexpand(s->scroll, FALSE);
	gtk_widget_set_margin_start(s->scroll, INNER + LIST_W - GEM_GADGET - 1);
	gtk_widget_set_margin_top(s->scroll, list_y() + GEM_ROW_H + 1);
	gtk_widget_set_size_request(s->scroll, GEM_GADGET + 1,
		list_h() - GEM_ROW_H - 1);
	gtk_overlay_add_overlay(GTK_OVERLAY(s->box), s->scroll);

	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), s);
	gtk_widget_add_controller(s->box, keys);

	const char *start = folder != NULL ? folder : last_folder;
	char *docs = NULL;
	if (start == NULL || !g_file_test(start, G_FILE_TEST_IS_DIR)) {
		const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
		docs = g_strdup(d != NULL && g_file_test(d, G_FILE_TEST_IS_DIR) ?
			d : g_get_home_dir());
		start = docs;
	}
	s->folder = g_canonicalize_filename(start, NULL);
	g_free(docs);
	read_folder(s);
	if (name != NULL) {
		gtk_editable_set_text(GTK_EDITABLE(s->name_field), name);
	}
	gem_hold(host, s->box);
	gtk_widget_grab_focus(s->name_field);
	/* The name without its extension selected, to type over. */
	const char *dot = name != NULL ? strrchr(name, '.') : NULL;
	gtk_editable_select_region(GTK_EDITABLE(s->name_field), 0,
		dot != NULL && dot != name ? g_utf8_pointer_to_offset(name, dot) : -1);
}
