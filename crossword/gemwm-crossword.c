/*
 * gemwm-crossword: crossword puzzles for GemWM, after MPOS's.
 *
 *   gemwm-crossword [FILE.puz]
 *   gemwm-crossword --pdf OUT.pdf [--blank] FILE.puz
 *
 * Two screens in one GEM window. The library lists your puzzles by date,
 * with how far along each is, and the week's Wall Street Journal and
 * Universal puzzles to download (Across Lite .puz files, from the archive
 * at herbach.dnsalias.com). The puzzle: Across clues, the grid, Down clues;
 * the current square in blue, the rest of its word in light blue, and the
 * grid's border green or red once checked. Progress is saved as you type.
 * File > Print (print.c) puts it on paper, newspaper-style; --pdf makes
 * the same pages into a PDF, with your answers unless --blank.
 *
 * Puzzles and saves live in ~/.local/share/gemwm/crossword/.
 */
#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <stdbool.h>
#include <string.h>
#include "app-menu.h"
#include "gem-draw.h"
#include "gem-popup.h"
#include "gem-ui.h"
#include "gem-print.h"
#include "print.h"
#include "puz.h"

#define PAD 8
#define BAR_H 30       /* the library's top row */
#define ROW_H 24
#define BUTTON_H 18
#define PANEL_W 256    /* each clue panel */
#define MAX_CELL 30
#define MIN_CELL 12
#define HEADER_H 64    /* above the grid: Library, title, author */
#define FOOTER_H 72    /* below it: direction and buttons */
#define DAYS 7         /* how far back the download list goes */
#define ARCHIVE "https://herbach.dnsalias.com"

/* GEM on a colour ST: MPOS's blues, and its greys. */
#define BLUE 0.0, 0.0, 1.0
#define LIGHT_BLUE 0.90, 0.95, 1.0
#define GREY 0.75, 0.75, 0.75
#define DARK_GREY 0.5, 0.5, 0.5
#define GREEN 0.0, 0.8, 0.0
#define RED 1.0, 0.0, 0.0

/* ---- Files -------------------------------------------------------------- */

static char *data_dir(void) {
	return g_build_filename(g_get_user_data_dir(), "gemwm", "crossword", NULL);
}

static char *save_path(const char *id) {
	char *dir = data_dir();
	char *name = g_strconcat(id, ".save", NULL);
	char *path = g_build_filename(dir, "saves", name, NULL);
	g_free(dir);
	g_free(name);
	return path;
}

/* The archive's puzzles are named for their source and date: wsj260926. */
static const struct source {
	const char *prefix, *name;
	bool sundays;
} sources[] = {
	{ "wsj", "Wall Street Journal", false },
	{ "uc", "Universal Crossword", true },
};

static const struct source *source_of(const char *id, GDateTime **date) {
	for (size_t i = 0; i < G_N_ELEMENTS(sources); i++) {
		size_t n = strlen(sources[i].prefix);
		int yy, mm, dd;
		if (strncmp(id, sources[i].prefix, n) == 0 && strlen(id) == n + 6 &&
				sscanf(id + n, "%2d%2d%2d", &yy, &mm, &dd) == 3) {
			*date = g_date_time_new_local(2000 + yy, mm, dd, 0, 0, 0);
			return *date != NULL ? &sources[i] : NULL;
		}
	}
	return NULL;
}

/* ---- Progress ----------------------------------------------------------- */

struct progress {
	char *grid;     /* as struct puzzle's */
	int row, col;
	bool across, solved;
};

/* Keyfile saves; the grid with '-' for empty squares, as .puz does. */
static bool progress_load(const char *id, int cells, struct progress *out) {
	char *path = save_path(id);
	GKeyFile *kf = g_key_file_new();
	bool ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
	char *grid = ok ? g_key_file_get_string(kf, "progress", "grid", NULL) : NULL;
	if (grid != NULL && (int)strlen(grid) == cells) {
		g_strdelimit(grid, "-", ' ');
		out->grid = grid;
		out->row = g_key_file_get_integer(kf, "progress", "row", NULL);
		out->col = g_key_file_get_integer(kf, "progress", "col", NULL);
		out->across = !g_key_file_has_key(kf, "progress", "across", NULL) ||
			g_key_file_get_boolean(kf, "progress", "across", NULL);
		out->solved = g_key_file_get_boolean(kf, "progress", "solved", NULL);
	} else {
		g_free(grid);
		ok = false;
	}
	g_key_file_free(kf);
	g_free(path);
	return ok;
}

static void progress_save(const char *id, const struct progress *p) {
	char *path = save_path(id);
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	GKeyFile *kf = g_key_file_new();
	char *grid = g_strdelimit(g_strdup(p->grid), " ", '-');
	g_key_file_set_string(kf, "progress", "grid", grid);
	g_key_file_set_integer(kf, "progress", "row", p->row);
	g_key_file_set_integer(kf, "progress", "col", p->col);
	g_key_file_set_boolean(kf, "progress", "across", p->across);
	g_key_file_set_boolean(kf, "progress", "solved", p->solved);
	g_key_file_save_to_file(kf, path, NULL);
	g_key_file_free(kf);
	g_free(grid);
	g_free(dir);
	g_free(path);
}

static int percent_filled(const char *grid) {
	int cells = 0, filled = 0;
	for (const char *c = grid; *c != '\0'; c++) {
		if (*c != '.') {
			cells++;
			filled += *c != ' ';
		}
	}
	return cells > 0 ? filled * 100 / cells : 0;
}

/* ---- The library -------------------------------------------------------- */

struct entry {
	char *id, *path, *title, *author;
	GDateTime *date;
	int percent;
	bool solved, started;
};

static void entry_free(void *data) {
	struct entry *e = data;
	g_free(e->id);
	g_free(e->path);
	g_free(e->title);
	g_free(e->author);
	g_clear_pointer(&e->date, g_date_time_unref);
	g_free(e);
}

static int newest_first(const void *pa, const void *pb) {
	const struct entry *a = *(struct entry **)pa, *b = *(struct entry **)pb;
	int by_date = g_date_time_compare(b->date, a->date);
	return by_date != 0 ? by_date : g_utf8_collate(a->title, b->title);
}

/* Every .puz in the folder, with how far along it is. */
static GPtrArray *library_scan(void) {
	GPtrArray *list = g_ptr_array_new_with_free_func(entry_free);
	char *dir = data_dir();
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		if (!g_str_has_suffix(name, ".puz")) {
			continue;
		}
		char *path = g_build_filename(dir, name, NULL);
		struct puzzle *p = puz_load(path, NULL);
		if (p == NULL) {
			g_free(path);
			continue;
		}
		struct entry *e = g_new0(struct entry, 1);
		e->id = g_strndup(name, strlen(name) - 4);
		e->path = path;
		e->title = g_strdup(p->title);
		e->author = g_strdup(p->author);
		if (source_of(e->id, &e->date) == NULL) {
			GStatBuf st;
			e->date = g_stat(path, &st) == 0 ?
				g_date_time_new_from_unix_local(st.st_mtime) :
				g_date_time_new_now_local();
		}
		struct progress pr = { 0 };
		if (progress_load(e->id, p->width * p->height, &pr)) {
			e->started = true;
			e->percent = percent_filled(pr.grid);
			e->solved = pr.solved;
			g_free(pr.grid);
		}
		g_ptr_array_add(list, e);
		puz_free(p);
	}
	if (d != NULL) {
		g_dir_close(d);
	}
	g_free(dir);
	g_ptr_array_sort(list, newest_first);
	return list;
}

/* ---- State -------------------------------------------------------------- */

enum action {
	ACT_NONE,
	ACT_TAB_MINE,
	ACT_TAB_DOWNLOAD,
	ACT_OPEN,        /* hit.id: a library entry's id */
	ACT_DOWNLOAD,    /* hit.id: the puzzle's id */
	ACT_LIBRARY,
	ACT_CELL,        /* hit.row, hit.col */
	ACT_CLUE,        /* hit.row: the number; hit.col: 1 for across */
	ACT_CHECK,
	ACT_REVEAL_LETTER,
	ACT_REVEAL_WORD,
	ACT_CLEAR,
	ACT_ALERT_YES,
	ACT_ALERT_NO,
	ACT_CLOSE,
	ACT_SHOW_DOWNLOADS,
	ACT_PRINT,
	ACT_DELETE,      /* hit.id: a library entry's id; none, the open one */
	ACT_ROW_MENU,    /* hit.id: a library entry's id, for its menu */
	ACT_ROW,         /* hit.id: the library row, for a right-click */
};

/* A library row's menu (gem-popup ids). */
enum { ROW_OPEN = 1, ROW_DELETE };

struct hit {
	int x, y, w, h;
	enum action action;
	char *id;
	int row, col;
};

enum region { REGION_NONE, REGION_LIST, REGION_ACROSS, REGION_DOWN };

static struct {
	GtkWidget *window, *area;
	struct app_menu *menu;
	GArray *hits;
	enum action pressed;
	char *pressed_id;
	double pointer_x, pointer_y;
	char *message;

	bool playing;          /* the puzzle screen, else the library */
	bool downloads;        /* the library's Download tab */
	GPtrArray *library;
	GHashTable *fetching;  /* ids being downloaded */
	SoupSession *soup;
	int list_scroll, list_h, list_content;
	int list_top;

	struct puzzle *puz;
	char *id;
	int row, col;
	bool across;
	enum { UNCHECKED, CORRECT, INCORRECT } status;
	int clue_scroll[2];    /* across, down */
	bool center_clue;      /* bring the current clue into view */
	int panel_top, panel_h, panel_content[2];
	int panel_w;

	char *path;            /* the open puzzle's file */

	/* The alert that's up, if any, and for Delete, which puzzle. */
	enum { ASK_NONE, ASK_CLEAR, ASK_DELETE } asking;
	char *asking_id, *asking_title;

	struct gem_popup *popup; /* a library row's menu */
	char *popup_id;          /* whose */
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

static void changed(void) {
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

/* ---- The puzzle --------------------------------------------------------- */

static struct clue *current_clue(void) {
	return puz_clue_at(ui.puz, ui.row, ui.col, ui.across);
}

static void save(void) {
	if (ui.puz == NULL) {
		return;
	}
	struct progress p = { ui.puz->grid, ui.row, ui.col, ui.across,
		ui.status == CORRECT };
	progress_save(ui.id, &p);
}

static bool all_filled(void) {
	return strchr(ui.puz->grid, ' ') == NULL;
}

/* After any change to the letters: filled in right is solved, straight
 * away; anything else waits for Check. */
static void letters_changed(void) {
	ui.status = !ui.puz->scrambled && all_filled() &&
		strcmp(ui.puz->grid, ui.puz->solution) == 0 ? CORRECT : UNCHECKED;
	if (ui.status == CORRECT) {
		set_message("Solved!");
	}
	save();
	changed();
}

static void select_cell(int row, int col) {
	if (row < 0 || col < 0 || row >= ui.puz->height || col >= ui.puz->width ||
			puz_black(ui.puz, row, col)) {
		return;
	}
	ui.row = row;
	ui.col = col;
	/* A square in only one word goes that way. */
	if (puz_clue_at(ui.puz, row, col, ui.across) == NULL) {
		ui.across = !ui.across;
	}
	ui.center_clue = true;
	save();
	changed();
}

static void select_clue(struct clue *c, bool across) {
	ui.across = across;
	/* The first empty square, or the first. */
	int r = c->row, col = c->col;
	for (int i = 0; i < c->length; i++) {
		int cr = across ? c->row : c->row + i, cc = across ? c->col + i : c->col;
		if (ui.puz->grid[cr * ui.puz->width + cc] == ' ') {
			r = cr;
			col = cc;
			break;
		}
	}
	ui.row = r;
	ui.col = col;
	ui.center_clue = true;
	save();
	changed();
}

/* The next (or previous) clue in the current direction, wrapping round. */
static void next_clue(int step) {
	GPtrArray *list = ui.across ? ui.puz->across : ui.puz->down;
	struct clue *c = current_clue();
	guint i = 0;
	while (i < list->len && g_ptr_array_index(list, i) != c) {
		i++;
	}
	i = (i + list->len + step) % list->len;
	select_clue(g_ptr_array_index(list, i), ui.across);
}

/* Along the word; off its end, on to the next clue. */
static void advance(int step) {
	int dr = ui.across ? 0 : step, dc = ui.across ? step : 0;
	int r = ui.row + dr, c = ui.col + dc;
	if (r >= 0 && c >= 0 && r < ui.puz->height && c < ui.puz->width &&
			!puz_black(ui.puz, r, c)) {
		ui.row = r;
		ui.col = c;
	} else if (step > 0) {
		next_clue(1);
		return;
	} else {
		/* Back into the previous clue, at its last square. */
		GPtrArray *list = ui.across ? ui.puz->across : ui.puz->down;
		struct clue *cur = current_clue();
		guint i = 0;
		while (i < list->len && g_ptr_array_index(list, i) != cur) {
			i++;
		}
		struct clue *prev = g_ptr_array_index(list, (i + list->len - 1) % list->len);
		ui.row = ui.across ? prev->row : prev->row + prev->length - 1;
		ui.col = ui.across ? prev->col + prev->length - 1 : prev->col;
		ui.center_clue = true;
	}
	changed();
}

static void type_letter(char letter) {
	ui.puz->grid[ui.row * ui.puz->width + ui.col] = letter;
	advance(1);
	letters_changed();
}

static void erase(bool back) {
	char *cell = &ui.puz->grid[ui.row * ui.puz->width + ui.col];
	if (back && *cell == ' ') {
		advance(-1); /* an empty square: backspace clears the one before */
		cell = &ui.puz->grid[ui.row * ui.puz->width + ui.col];
	}
	*cell = ' ';
	letters_changed();
}

/* Arrows move to the next open square that way, and turn to face it. */
static void arrow(int dr, int dc) {
	bool across = dc != 0;
	if (across != ui.across) {
		ui.across = across;
		changed();
		return;
	}
	for (int r = ui.row + dr, c = ui.col + dc; r >= 0 && c >= 0 &&
			r < ui.puz->height && c < ui.puz->width; r += dr, c += dc) {
		if (!puz_black(ui.puz, r, c)) {
			select_cell(r, c);
			return;
		}
	}
}

static void check(void) {
	if (ui.puz->scrambled) {
		set_message("This puzzle's answers are locked; it can't be checked.");
		return;
	}
	int wrong = 0, empty = 0;
	for (int i = 0; ui.puz->grid[i] != '\0'; i++) {
		empty += ui.puz->grid[i] == ' ';
		wrong += ui.puz->grid[i] != ' ' && ui.puz->grid[i] != ui.puz->solution[i];
	}
	ui.status = wrong == 0 && empty == 0 ? CORRECT : INCORRECT;
	if (ui.status == CORRECT) {
		set_message("Solved!");
	} else if (wrong > 0) {
		set_message("%d wrong letter%s, %d square%s to go.", wrong,
			wrong == 1 ? "" : "s", empty, empty == 1 ? "" : "s");
	} else {
		set_message("No mistakes so far; %d square%s to go.", empty,
			empty == 1 ? "" : "s");
	}
	save();
	changed();
}

static void reveal(bool word) {
	if (ui.puz->scrambled) {
		set_message("This puzzle's answers are locked.");
		return;
	}
	struct clue *c = current_clue();
	int n = word && c != NULL ? c->length : 1;
	for (int i = 0; i < n; i++) {
		int r = word ? (ui.across ? c->row : c->row + i) : ui.row;
		int col = word ? (ui.across ? c->col + i : c->col) : ui.col;
		ui.puz->grid[r * ui.puz->width + col] =
			ui.puz->solution[r * ui.puz->width + col];
	}
	letters_changed();
}

static void clear(void) {
	for (int i = 0; ui.puz->grid[i] != '\0'; i++) {
		if (ui.puz->grid[i] != '.') {
			ui.puz->grid[i] = ' ';
		}
	}
	struct clue *first = g_ptr_array_index(ui.puz->across, 0);
	ui.row = first->row;
	ui.col = first->col;
	ui.across = true;
	g_free(ui.message);
	ui.message = NULL;
	letters_changed();
}

static void open_puzzle(const char *path) {
	GError *error = NULL;
	struct puzzle *p = puz_load(path, &error);
	if (p == NULL) {
		set_message("%s", error->message);
		g_error_free(error);
		return;
	}
	if (p->across->len == 0 || p->down->len == 0) {
		set_message("That puzzle has no clues.");
		puz_free(p);
		return;
	}
	puz_free(ui.puz);
	g_free(ui.id);
	g_free(ui.path);
	ui.puz = p;
	ui.path = g_strdup(path);
	char *base = g_path_get_basename(path);
	ui.id = g_str_has_suffix(base, ".puz") ?
		g_strndup(base, strlen(base) - 4) : g_strdup(base);
	g_free(base);

	struct clue *first = g_ptr_array_index(p->across, 0);
	ui.row = first->row;
	ui.col = first->col;
	ui.across = true;
	ui.status = UNCHECKED;
	struct progress pr = { 0 };
	if (progress_load(ui.id, p->width * p->height, &pr)) {
		memcpy(p->grid, pr.grid, p->width * p->height);
		if (pr.row >= 0 && pr.col >= 0 && pr.row < p->height &&
				pr.col < p->width && !puz_black(p, pr.row, pr.col)) {
			ui.row = pr.row;
			ui.col = pr.col;
			ui.across = pr.across;
		}
		ui.status = pr.solved ? CORRECT : UNCHECKED;
		g_free(pr.grid);
	}
	if (puz_clue_at(p, ui.row, ui.col, ui.across) == NULL) {
		ui.across = !ui.across;
	}
	ui.playing = true;
	ui.clue_scroll[0] = ui.clue_scroll[1] = 0;
	ui.center_clue = true;
	g_free(ui.message);
	ui.message = NULL;
	gtk_window_set_title(GTK_WINDOW(ui.window), p->title);
	changed();
}

static void show_library(void) {
	if (ui.playing) {
		save();
	}
	ui.playing = false;
	ui.asking = ASK_NONE;
	g_clear_pointer(&ui.library, g_ptr_array_unref);
	ui.library = library_scan();
	gtk_window_set_title(GTK_WINDOW(ui.window), "Crossword Puzzle");
	changed();
}

/* Only the library's own puzzles can be deleted: one opened from
 * elsewhere (gemwm-crossword FILE.puz) is the user's file, not ours. */
static char *library_path(const char *id) {
	char *dir = data_dir();
	char *name = g_strconcat(id, ".puz", NULL);
	char *path = g_build_filename(dir, name, NULL);
	g_free(dir);
	g_free(name);
	return path;
}

static bool deletable(void) {
	if (!ui.playing || ui.path == NULL) {
		return false;
	}
	char *ours = library_path(ui.id);
	bool yes = strcmp(ours, ui.path) == 0;
	g_free(ours);
	return yes;
}

/* Asks before deleting a puzzle: id, or the open one for NULL. */
static void ask_delete(const char *id) {
	const char *title = NULL;
	if (id == NULL) {
		if (!deletable()) {
			return;
		}
		id = ui.id;
		title = ui.puz->title;
	}
	for (guint i = 0; title == NULL && ui.library != NULL &&
			i < ui.library->len; i++) {
		struct entry *e = ui.library->pdata[i];
		if (strcmp(e->id, id) == 0) {
			title = e->title;
		}
	}
	g_free(ui.asking_id);
	g_free(ui.asking_title);
	ui.asking_id = g_strdup(id);
	ui.asking_title = g_strdup(title != NULL && title[0] ? title : id);
	ui.asking = ASK_DELETE;
}

/* The puzzle and its saved letters, gone; back to the library. A recent
 * one can be downloaded again. */
static void delete_puzzle(const char *id) {
	char *path = library_path(id), *saved = save_path(id);
	bool open = ui.playing && g_strcmp0(ui.id, id) == 0;
	if (g_unlink(path) != 0 && errno != ENOENT) {
		set_message("Can't delete %s: %s", path, g_strerror(errno));
	} else {
		g_unlink(saved);
		if (open) {
			/* Not saved again on the way out. */
			g_clear_pointer(&ui.puz, puz_free);
			ui.playing = false;
		}
		set_message("Deleted %s.", ui.asking_title ? ui.asking_title : id);
	}
	g_free(path);
	g_free(saved);
	show_library();
}

/* ---- Downloads ---------------------------------------------------------- */

struct fetch {
	char *id, *name;
};

static void downloaded(GObject *source, GAsyncResult *result, void *data) {
	struct fetch *f = data;
	GError *error = NULL;
	GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(source),
		result, &error);
	SoupMessage *msg = soup_session_get_async_result_message(
		SOUP_SESSION(source), result);
	guint status = msg != NULL ? soup_message_get_status(msg) : 0;
	gsize len = 0;
	const char *bytes = body ? g_bytes_get_data(body, &len) : NULL;
	g_hash_table_remove(ui.fetching, f->id);

	if (body == NULL) {
		set_message("Couldn't download %s: %s", f->name, error->message);
		g_error_free(error);
	} else if (status != 200 || len < 0x34 ||
			memcmp(bytes + 2, "ACROSS&DOWN", 12) != 0) {
		set_message("%s isn't there (yet).", f->name);
	} else {
		char *dir = data_dir();
		g_mkdir_with_parents(dir, 0755);
		char *name = g_strconcat(f->id, ".puz", NULL);
		char *path = g_build_filename(dir, name, NULL);
		if (g_file_set_contents(path, bytes, len, &error)) {
			set_message("Downloaded %s.", f->name);
		} else {
			set_message("Couldn't save it: %s", error->message);
			g_error_free(error);
		}
		g_free(path);
		g_free(name);
		g_free(dir);
		g_clear_pointer(&ui.library, g_ptr_array_unref);
		ui.library = library_scan();
	}
	if (body != NULL) {
		g_bytes_unref(body);
	}
	g_free(f->id);
	g_free(f->name);
	g_free(f);
	changed();
}

static void download(const char *id) {
	GDateTime *date = NULL;
	const struct source *src = source_of(id, &date);
	if (src == NULL || g_hash_table_contains(ui.fetching, id)) {
		g_clear_pointer(&date, g_date_time_unref);
		return;
	}
	char *url = g_strdup_printf(ARCHIVE "/%s/%s.puz", src->prefix, id);
	SoupMessage *msg = soup_message_new("GET", url);
	struct fetch *f = g_new0(struct fetch, 1);
	f->id = g_strdup(id);
	char *day = g_date_time_format(date, "%A's");
	f->name = g_strdup_printf("%s %s", day, src->name);
	g_free(day);
	g_hash_table_add(ui.fetching, g_strdup(id));
	soup_session_send_and_read_async(ui.soup, msg, G_PRIORITY_DEFAULT, NULL,
		downloaded, f);
	g_object_unref(msg);
	g_free(url);
	g_date_time_unref(date);
	set_message("Downloading %s...", f->name);
}

static bool in_library(const char *id) {
	for (guint i = 0; ui.library != NULL && i < ui.library->len; i++) {
		if (strcmp(((struct entry *)ui.library->pdata[i])->id, id) == 0) {
			return true;
		}
	}
	return false;
}

/* ---- Drawing ------------------------------------------------------------ */

static void add_hit(int x, int y, int w, int h, enum action action,
		const char *id, int row, int col) {
	struct hit hit = { x, y, w, h, action, g_strdup(id), row, col };
	g_array_append_val(ui.hits, hit);
}

static int button_width(cairo_t *cr, const char *label) {
	return (int)gem_text_width(cr, label) + 2 * PAD;
}

static void button(cairo_t *cr, int x, int y, const char *label,
		enum action action, const char *id, bool enabled) {
	int w = button_width(cr, label);
	bool pressed = enabled && ui.pressed == action &&
		g_strcmp0(ui.pressed_id, id) == 0;
	gem_black(cr);
	if (pressed) {
		gem_fill(cr, x, y, w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_white(cr);
		gem_fill(cr, x + 1, y + 1, w - 2, BUTTON_H - 2);
		gem_black(cr);
		gem_frame(cr, x, y, w, BUTTON_H, 1);
	}
	gem_text(cr, label, x + PAD, y, BUTTON_H);
	if (enabled) {
		add_hit(x, y, w, BUTTON_H, action, id, 0, 0);
	} else {
		gem_grey_out(cr, x, y, w, BUTTON_H);
	}
}

static void heading(cairo_t *cr, const char *label, int x, int y, int w) {
	gem_black(cr);
	gem_text(cr, label, x, y, ROW_H);
	for (int dx = x; dx < x + w; dx += 2) {
		gem_fill(cr, dx, y + ROW_H - 3, 1, 1);
	}
}

/* Text cut off at w. */
static void clipped(cairo_t *cr, const char *s, int x, int y, int w, int h) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	gem_text(cr, s, x, y, h);
	cairo_restore(cr);
}

/* A row's menu: a small box with GEM's down arrow, inverted while held. */
static void menu_gadget(cairo_t *cr, int x, int y, const char *id) {
	int s = BUTTON_H;
	bool held = ui.pressed == ACT_ROW_MENU && g_strcmp0(ui.pressed_id, id) == 0;
	gem_black(cr);
	if (held) {
		gem_fill(cr, x, y, s, s);
		gem_white(cr);
	} else {
		gem_frame(cr, x, y, s, s, 1);
	}
	/* The arrow, row by row: 7 wide narrowing to 1. */
	for (int i = 0; i < 4; i++) {
		gem_fill(cr, x + s / 2 - 3 + i, y + s / 2 - 2 + i, 7 - 2 * i, 1);
	}
	add_hit(x, y, s, s, ACT_ROW_MENU, id, 0, 0);
}

static int row_line(cairo_t *cr, const char *title, const char *detail,
		const char *status, const char *label, enum action action,
		const char *id, bool enabled, int y, int w) {
	int bw = button_width(cr, label);
	int bx = w - PAD - bw;
	/* A library puzzle has a menu (Open, Delete...) at the left. */
	int tx = 2 * PAD;
	if (action == ACT_OPEN) {
		menu_gadget(cr, PAD, y + (ROW_H - BUTTON_H) / 2, id);
		tx = 2 * PAD + BUTTON_H;
	}
	int sw = status ? (int)gem_text_width(cr, status) : 0;
	gem_black(cr);
	if (status != NULL) {
		gem_text(cr, status, bx - PAD - sw, y, ROW_H);
	}
	int text_w = bx - tx - sw - 2 * PAD;
	char *line = detail && detail[0] ? g_strdup_printf("%s  (%s)", title, detail) :
		g_strdup(title);
	clipped(cr, line, tx, y, text_w, ROW_H);
	g_free(line);
	button(cr, bx, y + (ROW_H - BUTTON_H) / 2, label, action, id, enabled);
	if (action == ACT_OPEN) {
		/* After the buttons, so they're found first. */
		add_hit(0, y, w, ROW_H, ACT_ROW, id, 0, 0);
	}
	return y + ROW_H;
}

static int note(cairo_t *cr, const char *s, int y, int w) {
	gem_black(cr);
	gem_text(cr, s, 2 * PAD, y, ROW_H);
	gem_grey_out(cr, 0, y, w, ROW_H);
	return y + ROW_H;
}

static int paint_mine(cairo_t *cr, int y, int w) {
	if (ui.library == NULL || ui.library->len == 0) {
		return note(cr, "No puzzles yet: Download has this week's.", y, w);
	}
	char *last = NULL;
	for (guint i = 0; i < ui.library->len; i++) {
		struct entry *e = ui.library->pdata[i];
		char *day = g_date_time_format(e->date, "%A %-d %B %Y");
		if (g_strcmp0(day, last) != 0) {
			heading(cr, day, PAD, y, w - 2 * PAD);
			y += ROW_H;
			g_free(last);
			last = g_strdup(day);
		}
		g_free(day);
		char pct[16];
		snprintf(pct, sizeof(pct), "%d%%", e->percent);
		y = row_line(cr, e->title, e->author, e->solved ? "Solved" :
			e->started ? pct : "New", "Open", ACT_OPEN, e->id, true, y, w);
	}
	g_free(last);
	return y;
}

static int paint_downloads(cairo_t *cr, int y, int w) {
	GDateTime *now = g_date_time_new_now_local();
	bool any = false;
	for (int d = 0; d < DAYS; d++) {
		GDateTime *day = g_date_time_add_days(now, -d);
		char *ymd = g_date_time_format(day, "%y%m%d");
		bool sunday = g_date_time_get_day_of_week(day) == 7;
		bool headed = false;
		for (size_t s = 0; s < G_N_ELEMENTS(sources); s++) {
			char *id = g_strconcat(sources[s].prefix, ymd, NULL);
			if ((sunday && !sources[s].sundays) || in_library(id)) {
				g_free(id);
				continue;
			}
			if (!headed) {
				char *label = d == 0 ? g_strdup("Today") : d == 1 ?
					g_strdup("Yesterday") :
					g_date_time_format(day, "%A %-d %B");
				heading(cr, label, PAD, y, w - 2 * PAD);
				g_free(label);
				y += ROW_H;
				headed = true;
			}
			bool fetching = g_hash_table_contains(ui.fetching, id);
			y = row_line(cr, sources[s].name, NULL, fetching ?
				"Downloading..." : NULL, "Download", ACT_DOWNLOAD, id,
				!fetching, y, w);
			any = true;
			g_free(id);
		}
		g_free(ymd);
		g_date_time_unref(day);
	}
	g_date_time_unref(now);
	if (!any) {
		y = note(cr, "You have all of this week's puzzles.", y, w);
	}
	return y;
}

static void paint_library(cairo_t *cr, int w, int h) {
	/* Top: the name, and the two tabs as GEM radio buttons. */
	gem_black(cr);
	gem_text(cr, "Crossword Puzzles", PAD, 0, BAR_H);
	static const char *const tabs[] = { "My Puzzles", "Download" };
	int tw = MAX(button_width(cr, tabs[0]), button_width(cr, tabs[1]));
	int by = (BAR_H - BUTTON_H) / 2;
	for (int i = 0; i < 2; i++) {
		/* Side by side, sharing a border. */
		int x = w - PAD - (2 - i) * tw + (i == 0 ? 1 : 0);
		bool selected = (i == 1) == ui.downloads;
		gem_black(cr);
		if (selected) {
			gem_fill(cr, x, by, tw, BUTTON_H);
			gem_white(cr);
		} else {
			gem_frame(cr, x, by, tw, BUTTON_H, 1);
		}
		gem_text(cr, tabs[i], x + (tw - gem_text_width(cr, tabs[i])) / 2, by,
			BUTTON_H);
		if (!selected) {
			add_hit(x, by, tw, BUTTON_H, i == 0 ? ACT_TAB_MINE :
				ACT_TAB_DOWNLOAD, NULL, 0, 0);
		}
	}
	gem_black(cr);
	gem_fill(cr, 0, BAR_H - 1, w, 1);

	/* The list, scrolled, between the bars. */
	ui.list_top = BAR_H;
	ui.list_h = h - 2 * BAR_H;
	ui.list_scroll = CLAMP(ui.list_scroll, 0, MAX(0, ui.list_content - ui.list_h));
	cairo_save(cr);
	cairo_rectangle(cr, 0, BAR_H, w, ui.list_h);
	cairo_clip(cr);
	guint first = ui.hits->len;
	int top = BAR_H + PAD / 2 - ui.list_scroll;
	int end = ui.downloads ? paint_downloads(cr, top, w) : paint_mine(cr, top, w);
	ui.list_content = end - top + PAD;
	cairo_restore(cr);
	for (guint i = first; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (hit->y < BAR_H || hit->y + hit->h > BAR_H + ui.list_h) {
			hit->action = ACT_NONE; /* scrolled out of sight */
		}
	}

	/* Bottom: what's happening. */
	gem_black(cr);
	gem_fill(cr, 0, h - BAR_H, w, 1);
	clipped(cr, ui.message ? ui.message : "", PAD, h - BAR_H, w - 2 * PAD, BAR_H);
}

/* A clue's text wrapped to width, as lines. */
static char **wrap(cairo_t *cr, const char *text, int width) {
	GPtrArray *lines = g_ptr_array_new();
	char **words = g_strsplit(text, " ", -1);
	GString *line = g_string_new(NULL);
	for (char **w = words; *w != NULL; w++) {
		if ((*w)[0] == '\0') {
			continue;
		}
		size_t before = line->len;
		if (line->len > 0) {
			g_string_append_c(line, ' ');
		}
		g_string_append(line, *w);
		if (gem_text_width(cr, line->str) > width && before > 0) {
			g_string_truncate(line, before);
			g_ptr_array_add(lines, g_strdup(line->str));
			g_string_assign(line, *w);
		}
	}
	g_ptr_array_add(lines, g_string_free(line, FALSE));
	g_ptr_array_add(lines, NULL);
	g_strfreev(words);
	return (char **)g_ptr_array_free(lines, FALSE);
}

static void paint_clues(cairo_t *cr, bool across, int x, int w, int h) {
	int i = across ? 0 : 1;
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	int line_h = (int)(fe.ascent + fe.descent) + 1;

	gem_white(cr);
	gem_fill(cr, x, 0, w, h);
	heading(cr, across ? "ACROSS" : "DOWN", x + PAD, PAD / 2, w - 2 * PAD);
	int top = ROW_H + PAD;
	ui.panel_top = top;
	ui.panel_h = h - top;

	/* Lay the clues out, to know where the current one is. */
	GPtrArray *list = across ? ui.puz->across : ui.puz->down;
	struct clue *current = puz_clue_at(ui.puz, ui.row, ui.col, across);
	int y = 0, cur_y = 0, cur_h = 0;
	int text_w = w - 2 * PAD - 4;
	char ***wrapped = g_new0(char **, list->len);
	for (guint k = 0; k < list->len; k++) {
		struct clue *c = list->pdata[k];
		char *s = g_strdup_printf("%d. %s", c->number, c->text);
		wrapped[k] = wrap(cr, s, text_w);
		g_free(s);
		int n = g_strv_length(wrapped[k]);
		if (c == current && across == ui.across) {
			cur_y = y;
			cur_h = n * line_h + 4;
		}
		y += n * line_h + 4;
	}
	ui.panel_content[i] = y;
	if (ui.center_clue && cur_h > 0) {
		ui.clue_scroll[i] = cur_y - (ui.panel_h - cur_h) / 2;
	}
	ui.clue_scroll[i] = CLAMP(ui.clue_scroll[i], 0, MAX(0, y - ui.panel_h));

	cairo_save(cr);
	cairo_rectangle(cr, x, top, w, ui.panel_h);
	cairo_clip(cr);
	y = top - ui.clue_scroll[i];
	for (guint k = 0; k < list->len; k++) {
		struct clue *c = list->pdata[k];
		int n = g_strv_length(wrapped[k]);
		int item_h = n * line_h + 4;
		bool highlight = c == current && across == ui.across;
		if (highlight) {
			cairo_set_source_rgb(cr, BLUE);
			gem_fill(cr, x + PAD - 2, y, w - 2 * PAD + 4, item_h);
			gem_white(cr);
		} else if (c == current) {
			/* The crossing clue, lightly. */
			cairo_set_source_rgb(cr, LIGHT_BLUE);
			gem_fill(cr, x + PAD - 2, y, w - 2 * PAD + 4, item_h);
			gem_black(cr);
		} else {
			gem_black(cr);
		}
		for (int l = 0; l < n; l++) {
			gem_text(cr, wrapped[k][l], x + PAD, y + 2 + l * line_h, line_h);
		}
		if (y + item_h > top && y < top + ui.panel_h) {
			add_hit(x, MAX(y, top), w, MIN(y + item_h, top + ui.panel_h) -
				MAX(y, top), ACT_CLUE, NULL, c->number, across);
		}
		y += item_h;
		g_strfreev(wrapped[k]);
	}
	g_free(wrapped);
	cairo_restore(cr);
}

static void paint_game(cairo_t *cr, int w, int h) {
	struct puzzle *p = ui.puz;
	ui.panel_w = MIN(PANEL_W, MAX(120, (w - 300) / 2));
	int pw = ui.panel_w;
	paint_clues(cr, true, 0, pw, h);
	paint_clues(cr, false, w - pw, pw, h);
	ui.center_clue = false;
	cairo_set_source_rgb(cr, DARK_GREY);
	gem_fill(cr, pw, 0, 2, h);
	gem_fill(cr, w - pw - 2, 0, 2, h);

	/* The middle, on grey. */
	int cx = pw + 2, cw = w - 2 * pw - 4;
	cairo_set_source_rgb(cr, GREY);
	gem_fill(cr, cx, 0, cw, h);

	/* Header: back to the library, the title and author. */
	int lb = button_width(cr, "< Library");
	char *by = p->author[0] ? g_strdup_printf("by %s", p->author) : g_strdup("");
	int title_w = (int)gem_text_width(cr, p->title);
	int head_w = lb + 2 * PAD + title_w;
	int hx = cx + MAX(PAD, (cw - head_w) / 2);
	button(cr, hx, PAD + 3, "< Library", ACT_LIBRARY, NULL, true);
	gem_black(cr);
	clipped(cr, p->title, hx + lb + 2 * PAD, PAD, cx + cw - (hx + lb + 2 * PAD) -
		PAD, ROW_H);
	cairo_set_source_rgb(cr, DARK_GREY);
	int by_w = (int)gem_text_width(cr, by);
	clipped(cr, by, cx + MAX(PAD, (cw - by_w) / 2), PAD + ROW_H, cw - 2 * PAD,
		ROW_H);
	g_free(by);

	/* The buttons go in a row under the grid, or stacked if the middle is
	 * too narrow for one: then the grid leaves them room. */
	static const char *const labels[] = { "Check Puzzle", "Reveal Letter",
		"Clear Puzzle" };
	static const enum action actions[] = { ACT_CHECK, ACT_REVEAL_LETTER,
		ACT_CLEAR };
	int total = 0;
	for (int i = 0; i < 3; i++) {
		total += button_width(cr, labels[i]) + (i > 0 ? PAD : 0);
	}
	bool stack = total > cw - 2 * PAD;
	int footer_h = FOOTER_H + (stack ? 2 * (BUTTON_H + PAD / 2) : 0);

	/* The grid, as big as fits. */
	int avail_w = cw - 2 * PAD - 8, avail_h = h - HEADER_H - footer_h - 8;
	int cell = MIN(MAX_CELL, MIN(avail_w / p->width, avail_h / p->height));
	cell = MAX(cell, MIN_CELL);
	int gw = cell * p->width + 1, gh = cell * p->height + 1;
	int gx = cx + (cw - gw) / 2, gy = HEADER_H + MAX(0, (avail_h - gh) / 2) + 4;
	/* The border says how the last check went. */
	if (ui.status == CORRECT) {
		cairo_set_source_rgb(cr, GREEN);
	} else if (ui.status == INCORRECT) {
		cairo_set_source_rgb(cr, RED);
	} else {
		cairo_set_source_rgb(cr, DARK_GREY);
	}
	gem_frame(cr, gx - 4, gy - 4, gw + 8, gh + 8, 4);

	struct clue *word = current_clue();
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			int x = gx + c * cell, y = gy + r * cell;
			bool black = puz_black(p, r, c);
			bool here = r == ui.row && c == ui.col;
			bool in_word = word != NULL && (ui.across ?
				r == word->row && c >= word->col && c < word->col + word->length :
				c == word->col && r >= word->row && r < word->row + word->length);
			if (black) {
				gem_black(cr);
			} else if (here) {
				cairo_set_source_rgb(cr, BLUE);
			} else if (in_word) {
				cairo_set_source_rgb(cr, LIGHT_BLUE);
			} else {
				gem_white(cr);
			}
			gem_fill(cr, x, y, cell, cell);
			if (!black) {
				add_hit(x, y, cell, cell, ACT_CELL, NULL, r, c);
			}
		}
	}
	/* The lines between squares. */
	gem_black(cr);
	for (int r = 0; r <= p->height; r++) {
		gem_fill(cr, gx, gy + r * cell, gw, 1);
	}
	for (int c = 0; c <= p->width; c++) {
		gem_fill(cr, gx + c * cell, gy, 1, gh);
	}
	/* Numbers, small, in the corner; letters, big, in the middle. */
	cairo_save(cr);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, MAX(7, cell * 3 / 10));
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			int n = p->numbers[r * p->width + c];
			if (n > 0) {
				char num[12];
				snprintf(num, sizeof(num), "%d", n);
				if (r == ui.row && c == ui.col) {
					gem_white(cr);
				} else {
					gem_black(cr);
				}
				cairo_font_extents_t fe;
				cairo_font_extents(cr, &fe);
				cairo_move_to(cr, gx + c * cell + 2, gy + r * cell + 1 + fe.ascent);
				cairo_show_text(cr, num);
			}
		}
	}
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, cell * 11 / 20);
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			char letter[2] = { p->grid[r * p->width + c], '\0' };
			if (letter[0] == ' ' || letter[0] == '.') {
				continue;
			}
			if (r == ui.row && c == ui.col) {
				gem_white(cr);
			} else {
				gem_black(cr);
			}
			cairo_text_extents_t te;
			cairo_text_extents(cr, letter, &te);
			cairo_move_to(cr, gx + c * cell + (cell - te.x_advance) / 2 + 1,
				gy + r * cell + cell * 3 / 4 + 1);
			cairo_show_text(cr, letter);
		}
	}
	cairo_restore(cr);

	/* Below: the direction, what happened, and the buttons. */
	int fy = gy + gh + 4 + PAD;
	char dir[64];
	snprintf(dir, sizeof(dir), "Direction: %s", ui.across ? "ACROSS" : "DOWN");
	gem_black(cr);
	const char *line = ui.message ? ui.message : dir;
	gem_text(cr, line, cx + (cw - gem_text_width(cr, line)) / 2, fy, 18);
	fy += 18 + PAD / 2;
	int bx = cx + (cw - total) / 2;
	for (int i = 0; i < 3; i++) {
		if (stack) {
			/* Centred on one another, like a GEM dialog's. */
			bx = cx + (cw - button_width(cr, labels[i])) / 2;
		}
		button(cr, bx, fy, labels[i], actions[i], NULL, !p->scrambled || i == 2);
		if (stack) {
			fy += BUTTON_H + PAD / 2;
		} else {
			bx += button_width(cr, labels[i]) + PAD;
		}
	}
}

static void paint_alert(cairo_t *cr, int w, int h) {
	char *question = ui.asking == ASK_DELETE ?
		g_strdup_printf("Delete %s?", ui.asking_title) :
		g_strdup("Clear the whole puzzle?");
	const char *lines[] = { question, ui.asking == ASK_DELETE ?
		"It and your letters will be gone." : "Your letters will be lost." };
	const char *yes = ui.asking == ASK_DELETE ? "Delete" : "Clear";
	int tw = MAX((int)gem_text_width(cr, lines[0]),
		(int)gem_text_width(cr, lines[1]));
	tw = MIN(tw, w - 12 * PAD);
	int bw = MAX(button_width(cr, yes), button_width(cr, "Cancel"));
	int aw = MAX(tw, 2 * bw + PAD) + 4 * PAD, ah = 2 * 18 + BUTTON_H + 5 * PAD;
	int ax = (w - aw) / 2, ay = (h - ah) / 2;
	gem_white(cr);
	gem_fill(cr, ax - 3, ay - 3, aw + 6, ah + 6);
	gem_black(cr);
	gem_frame(cr, ax - 3, ay - 3, aw + 6, ah + 6, 1);
	gem_frame(cr, ax, ay, aw, ah, 2);
	for (int i = 0; i < 2; i++) {
		clipped(cr, lines[i], ax + 2 * PAD, ay + 2 * PAD + i * 18, tw, 18);
	}
	g_free(question);
	int by = ay + ah - 2 * PAD - BUTTON_H;
	int bx = ax + aw - 2 * PAD - 2 * bw - PAD;
	button(cr, bx, by, yes, ACT_ALERT_YES, NULL, true);
	/* The safe answer is the default: a thicker border. */
	button(cr, bx + bw + PAD, by, "Cancel", ACT_ALERT_NO, NULL, true);
	gem_black(cr);
	gem_frame(cr, bx + bw + PAD, by, button_width(cr, "Cancel"), BUTTON_H, 2);
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	for (guint i = 0; i < ui.hits->len; i++) {
		g_free(g_array_index(ui.hits, struct hit, i).id);
	}
	g_array_set_size(ui.hits, 0);
	if (ui.playing) {
		paint_game(cr, w, h);
	} else {
		paint_library(cr, w, h);
	}
	if (ui.asking != ASK_NONE) {
		for (guint i = 0; i < ui.hits->len; i++) {
			g_free(g_array_index(ui.hits, struct hit, i).id);
		}
		g_array_set_size(ui.hits, 0);
		paint_alert(cr, w, h);
	}
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	gem_draw_pixelated(cr, w, h, paint, NULL);
}

/* ---- Input -------------------------------------------------------------- */

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

/* The line under the title on paper: author, then source and date. */
static char *subtitle(const char *id, const struct puzzle *p) {
	GDateTime *date = NULL;
	const struct source *src = source_of(id, &date);
	GString *s = g_string_new(NULL);
	if (p->author[0] != '\0') {
		g_string_append_printf(s, "by %s", p->author);
	}
	if (src != NULL) {
		char *day = g_date_time_format(date, "%A %-d %B %Y");
		g_string_append_printf(s, "%s%s, %s", s->len ? "  \u00b7  " : "",
			src->name, day);
		g_free(day);
		g_date_time_unref(date);
	}
	return g_string_free(s, FALSE);
}

static void printed(const char *message, void *data) {
	set_message("%s", message);
}

static void act(enum action action, const struct hit *hit);

static void row_chosen(int id, void *data) {
	if (ui.popup_id == NULL) {
		return;
	}
	struct hit hit = { .id = ui.popup_id };
	if (id == ROW_OPEN) {
		act(ACT_OPEN, &hit);
	} else if (id == ROW_DELETE) {
		act(ACT_DELETE, &hit);
	}
}

/* A library row's menu, at x, y: Open, and Delete... */
static void row_menu(const char *id, double x, double y) {
	if (ui.popup == NULL) {
		ui.popup = gem_popup_new(ui.area, row_chosen, NULL);
	}
	g_free(ui.popup_id);
	ui.popup_id = g_strdup(id);
	gem_popup_clear(ui.popup);
	gem_popup_add(ui.popup, ROW_OPEN, "Open", NULL, 0);
	gem_popup_add_separator(ui.popup);
	gem_popup_add(ui.popup, ROW_DELETE, "Delete...", NULL, 0);
	gem_popup_show(ui.popup, x, y);
}

static void act(enum action action, const struct hit *hit) {
	switch (action) {
	case ACT_TAB_MINE:
	case ACT_TAB_DOWNLOAD:
	case ACT_SHOW_DOWNLOADS:
		if (ui.playing) {
			show_library();
		}
		ui.downloads = action != ACT_TAB_MINE;
		ui.list_scroll = 0;
		break;
	case ACT_OPEN:
		for (guint i = 0; ui.library != NULL && i < ui.library->len; i++) {
			struct entry *e = ui.library->pdata[i];
			if (strcmp(e->id, hit->id) == 0) {
				open_puzzle(e->path);
				break;
			}
		}
		break;
	case ACT_DOWNLOAD:
		download(hit->id);
		break;
	case ACT_LIBRARY:
		show_library();
		break;
	case ACT_CELL:
		if (hit->row == ui.row && hit->col == ui.col) {
			/* The current square again: the other way. */
			if (puz_clue_at(ui.puz, ui.row, ui.col, !ui.across) != NULL) {
				ui.across = !ui.across;
				ui.center_clue = true;
			}
			changed();
		} else {
			select_cell(hit->row, hit->col);
		}
		break;
	case ACT_CLUE: {
		GPtrArray *list = hit->col ? ui.puz->across : ui.puz->down;
		for (guint i = 0; i < list->len; i++) {
			struct clue *c = list->pdata[i];
			if (c->number == hit->row) {
				select_clue(c, hit->col);
				break;
			}
		}
		break;
	}
	case ACT_CHECK:
		check();
		break;
	case ACT_REVEAL_LETTER:
	case ACT_REVEAL_WORD:
		reveal(action == ACT_REVEAL_WORD);
		break;
	case ACT_CLEAR:
		ui.asking = ASK_CLEAR;
		break;
	case ACT_DELETE:
		ask_delete(hit != NULL ? hit->id : NULL);
		break;
	case ACT_ROW_MENU:
		row_menu(hit->id, hit->x, hit->y + hit->h);
		break;
	case ACT_ALERT_YES:
		if (ui.asking == ASK_DELETE) {
			ui.asking = ASK_NONE;
			delete_puzzle(ui.asking_id);
		} else {
			ui.asking = ASK_NONE;
			clear();
		}
		break;
	case ACT_ALERT_NO:
		ui.asking = ASK_NONE;
		break;
	case ACT_PRINT:
		if (ui.playing) {
			save();
			char *sub = subtitle(ui.id, ui.puz);
			print_puzzle(GTK_WINDOW(ui.window), ui.puz, sub, printed, NULL);
			g_free(sub);
		}
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(ui.window));
		return;
	default:
		break;
	}
	changed();
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	/* Squares and clues act at once; buttons when let go over. */
	if (hit != NULL && (hit->action == ACT_CELL || hit->action == ACT_CLUE)) {
		struct hit copy = *hit;
		act(copy.action, &copy);
		return;
	}
	ui.pressed = hit != NULL ? hit->action : ACT_NONE;
	g_free(ui.pressed_id);
	ui.pressed_id = hit != NULL ? g_strdup(hit->id) : NULL;
	gtk_widget_queue_draw(ui.area);
}

static void released(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	enum action action = ui.pressed;
	char *id = ui.pressed_id;
	ui.pressed = ACT_NONE;
	ui.pressed_id = NULL;
	if (hit != NULL && action != ACT_NONE && hit->action == action &&
			g_strcmp0(hit->id, id) == 0) {
		struct hit copy = *hit;
		copy.id = id;
		act(action, &copy);
	}
	g_free(id);
	gtk_widget_queue_draw(ui.area);
}

/* A right-click on a library row: its menu, at the pointer. */
static void right_pressed(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	if (ui.playing || ui.asking != ASK_NONE) {
		return;
	}
	for (guint i = 0; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (hit->action == ACT_ROW && x >= hit->x && x < hit->x + hit->w &&
				y >= hit->y && y < hit->y + hit->h) {
			row_menu(hit->id, x, y);
			return;
		}
	}
}

static void motion(GtkEventControllerMotion *controller, double x, double y,
		void *data) {
	ui.pointer_x = x;
	ui.pointer_y = y;
}

/* The wheel scrolls whatever list is under the pointer. */
static gboolean scrolled(GtkEventControllerScroll *controller, double dx,
		double dy, void *data) {
	int step = (int)(dy * ROW_H * 2);
	if (!ui.playing) {
		ui.list_scroll += step;
	} else if (ui.pointer_x < ui.panel_w) {
		ui.clue_scroll[0] += step;
	} else if (ui.pointer_x > gtk_widget_get_width(ui.area) - ui.panel_w) {
		ui.clue_scroll[1] += step;
	}
	gtk_widget_queue_draw(ui.area);
	return TRUE;
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
		guint code, GdkModifierType mods, void *data) {
	bool ctrl = mods & GDK_CONTROL_MASK;
	if (ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) {
		act(ACT_CLOSE, NULL);
		return TRUE;
	}
	if (ctrl && (keyval == GDK_KEY_p || keyval == GDK_KEY_P)) {
		act(ACT_PRINT, NULL);
		return TRUE;
	}
	if (ctrl && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
		act(ACT_LIBRARY, NULL);
		return TRUE;
	}
	if (ui.asking != ASK_NONE) {
		if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Return) {
			act(ACT_ALERT_NO, NULL);
		}
		return TRUE;
	}
	if (!ui.playing || ctrl || (mods & GDK_ALT_MASK)) {
		return FALSE;
	}
	gunichar c = gdk_keyval_to_unicode(keyval);
	if (c < 128 && g_ascii_isalpha(c)) {
		type_letter(g_ascii_toupper(c));
		return TRUE;
	}
	switch (keyval) {
	case GDK_KEY_BackSpace:
		erase(true);
		return TRUE;
	case GDK_KEY_Delete:
		erase(false);
		return TRUE;
	case GDK_KEY_Left:
		arrow(0, -1);
		return TRUE;
	case GDK_KEY_Right:
		arrow(0, 1);
		return TRUE;
	case GDK_KEY_Up:
		arrow(-1, 0);
		return TRUE;
	case GDK_KEY_Down:
		arrow(1, 0);
		return TRUE;
	case GDK_KEY_Tab:
		next_clue(1);
		return TRUE;
	case GDK_KEY_ISO_Left_Tab:
		next_clue(-1);
		return TRUE;
	case GDK_KEY_space:
		if (puz_clue_at(ui.puz, ui.row, ui.col, !ui.across) != NULL) {
			ui.across = !ui.across;
			ui.center_clue = true;
			changed();
		}
		return TRUE;
	case GDK_KEY_Escape:
		act(ACT_LIBRARY, NULL);
		return TRUE;
	}
	return FALSE;
}

/* ---- Menus and setup ---------------------------------------------------- */

static void build_menus(struct app_menu *m, void *data) {
	uint32_t game = ui.playing ? 0 : APP_MENU_DISABLED;
	uint32_t checkable = ui.playing && !ui.puz->scrambled ? 0 : APP_MENU_DISABLED;
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_LIBRARY, "Library", "^L", game);
	app_menu_add_item(m, ACT_SHOW_DOWNLOADS, "Download Puzzles...", "", 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CHECK, "Check Puzzle", "", checkable);
	app_menu_add_item(m, ACT_REVEAL_LETTER, "Reveal Letter", "", checkable);
	app_menu_add_item(m, ACT_REVEAL_WORD, "Reveal Word", "", checkable);
	app_menu_add_item(m, ACT_CLEAR, "Clear Puzzle...", "", game);
	app_menu_add_item(m, ACT_DELETE, "Delete Puzzle...", "",
		deletable() ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_PRINT, "Print...", "^P", game);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
}

static void menu_activate(uint32_t id, void *data) {
	if (ui.asking == ASK_NONE) {
		act((enum action)id, NULL);
	}
}

static void window_destroyed(GtkWidget *window, void *data) {
	save();
}

static void setup(GtkApplication *app) {
	gem_ui_load_css(); /* the GEM look, for the rows' pop-up menus */
	ui.hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	ui.fetching = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ui.soup = soup_session_new_with_options("user-agent",
		"GemWM-Crossword/1.0", NULL);
	ui.window = gtk_application_window_new(app);
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 1040, 660);
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(ui.window), ui.area);

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(click));
	GtkGesture *right = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
	g_signal_connect(right, "pressed", G_CALLBACK(right_pressed), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(right));
	GtkEventController *move = gtk_event_controller_motion_new();
	g_signal_connect(move, "motion", G_CALLBACK(motion), NULL);
	gtk_widget_add_controller(ui.area, move);
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scrolled), NULL);
	gtk_widget_add_controller(ui.area, scroll);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	g_signal_connect(ui.window, "destroy", G_CALLBACK(window_destroyed), NULL);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);
	show_library();
}

static void activate(GtkApplication *app, void *data) {
	if (ui.window == NULL) {
		setup(app);
	}
	gtk_window_present(GTK_WINDOW(ui.window));
}

/* gemwm-crossword FILE.puz: straight to that puzzle. */
static void open_files(GApplication *app, GFile **files, int n, const char *hint,
		void *data) {
	if (ui.window == NULL) {
		setup(GTK_APPLICATION(app));
	}
	char *path = n > 0 ? g_file_get_path(files[0]) : NULL;
	if (path != NULL) {
		open_puzzle(path);
		g_free(path);
	}
	gtk_window_present(GTK_WINDOW(ui.window));
}

/* --pdf OUT [--blank] FILE: the printed pages as a PDF, no window. */
static int export_pdf(int argc, char *argv[]) {
	const char *out = NULL, *in = NULL;
	bool blank = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pdf") == 0 && i + 1 < argc) {
			out = argv[++i];
		} else if (strcmp(argv[i], "--blank") == 0) {
			blank = true;
		} else {
			in = argv[i];
		}
	}
	if (out == NULL || in == NULL) {
		fprintf(stderr, "usage: gemwm-crossword --pdf OUT.pdf [--blank] "
			"FILE.puz\n");
		return 2;
	}
	gtk_init();
	GError *error = NULL;
	struct puzzle *p = puz_load(in, &error);
	if (p == NULL) {
		fprintf(stderr, "gemwm-crossword: %s\n", error->message);
		return 1;
	}
	/* With the answers saved for it, if any. */
	char *base = g_path_get_basename(in);
	char *id = g_str_has_suffix(base, ".puz") ?
		g_strndup(base, strlen(base) - 4) : g_strdup(base);
	struct progress pr = { 0 };
	if (progress_load(id, p->width * p->height, &pr)) {
		memcpy(p->grid, pr.grid, p->width * p->height);
		g_free(pr.grid);
	}
	char *sub = subtitle(id, p);
	bool ok = print_puzzle_pdf(p, sub, !blank, out, &error);
	if (!ok) {
		fprintf(stderr, "gemwm-crossword: %s\n", error->message);
	}
	g_free(sub);
	g_free(id);
	g_free(base);
	puz_free(p);
	return ok ? 0 : 1;
}

int main(int argc, char *argv[]) {
	gem_print_setup();
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pdf") == 0) {
			return export_pdf(argc, argv);
		}
	}
	GtkApplication *app = gtk_application_new("org.gemwm.Crossword",
		G_APPLICATION_HANDLES_OPEN);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
