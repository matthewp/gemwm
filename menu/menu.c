/*
 * gemwm-menu: the GEM menu bar, as a wlr-layer-shell client.
 *
 * The bar is a layer surface along the top of the screen that reserves its
 * height. Like GEM, hovering a title drops its menu down. While a menu is
 * open we put a transparent, screen-sized overlay surface on top of
 * everything, with the menu as a subsurface of it. That way we see every
 * click: one outside the menu closes it and never reaches the window below,
 * as on the ST.
 *
 * The middle of the bar shows the workspaces as "1 | 2 | +". We learn
 * about them over GemWM's control socket ($GEMWM_SOCKET, see src/ipc.c)
 * and switch by sending commands on it; without it the bar has no
 * workspace buttons.
 *
 * While a window has focus, the bar shows Desk, a menu for that window
 * (named after its application, with what GemWM can do to any window), the
 * window's own menus if it has any (gemwm-app-menu-v1), and Options, with
 * the window's own Options items merged in above GemWM's. With no window
 * focused, it shows the desktop's own menus. The control socket tells us
 * which window has focus, and its menus.
 *
 * Menus are built in (default_config below) and can be changed from
 * ~/.config/gemwm/menu, which uses the same format. A section there
 * replaces the built-in menu of the same name; other menus stay as they
 * are. Submenus, which GEM gained with AES 3.30 on the Falcon, open to the
 * side of their item.
 *
 * Everything at the right of the bar (Bluetooth, the battery, the clock)
 * is a menu app, listed under [Menu Apps]. Each is a program that runs as
 * long as the bar does, with a socket for its stdin and stdout. Every line
 * it prints replaces its item:
 *
 *   text
 *   icon<TAB>text
 *   icon<TAB>text<TAB>inverse     the text in white on black
 *   icon<TAB>text<TAB>flags<TAB>tooltip
 *
 * where icon is a PNG file, or "bitmap:WxH:hex", a 1-bit picture given as
 * rows of bits in hex, the first pixel in the top bit and 1 for black. An
 * empty line hides the item. A click on it sends the app "click 1" (2
 * middle, 3 right), and each notch of the scroll wheel over it "scroll up"
 * or "scroll down". The tooltip (flags is "inverse" or empty) shows in a
 * box below the item while the pointer rests on it. When stdin closes, the bar has gone and the app should
 * exit. The bar's own directory is put first on their PATH, so GemWM's
 * apps are found even when it runs from a build directory.
 */
#define _GNU_SOURCE /* memfd_create */
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BAR_H 20       /* 19px of bar plus a 1px line below it */
#define ITEM_H 18      /* one menu row */
#define TITLE_PAD 8    /* space either side of a bar title */
#define ITEM_PAD 16    /* GEM indents items by two characters */
#define FONT_SIZE 14
#define MAX_MENUS 16
#define MAX_ITEMS 32
#define ARROW_W 12     /* room for a submenu's arrow */
#define MAX_DEPTH 6    /* menu plus nested submenus open at once */
#define WS_PAD 6       /* space either side of a workspace number */
#define WS_GAP 3       /* space either side of the | between numbers */
#define MAX_WS 32      /* matches the compositor's limit */
#define MAX_APPS 8     /* menu apps */
#define APP_PAD 8      /* space either side of a menu app */
#define APP_MAX_W 200  /* longer menu-app text is cut off */
#define APP_RESTART 10 /* seconds an app must have run to be restarted */
#define SCROLL_STEP 15 /* scroll distance of one wheel notch */
#define TOOLTIP_DELAY 600 /* ms the pointer rests on an item first */
#define TOOLTIP_PAD 6
#define APPS_SECTION "Menu Apps"
#define DESK_MENU "Desk"       /* the desktop's menus kept while a window */
#define OPTIONS_MENU "Options" /* has focus */
#define APP_NAME_MAX 24        /* longer application names are cut */
#define SHORTCUT_GAP 16        /* between an item's label and its shortcut */
#define APP_MENU_DISABLED 1    /* gemwm-app-menu-v1's item flags */
#define APP_MENU_CHECKED 2

static const char default_config[] =
	"# gemwm-menu configuration\n"
	"#\n"
	"# [Title]           starts a menu in the bar\n"
	"# [Title > Sub]     starts a submenu, added to Title as \"Sub\"\n"
	"# Label = command   runs the command through /bin/sh\n"
	"# Label = [Sure?] command   asks first, in a GEM alert box\n"
	"# Label             a disabled (grey) item\n"
	"# Label >           places the submenu \"Label\" at this point\n"
	"# -                 a separator\n"
	"\n"
	"[Desk]\n"
	"Desktop Info...\n"
	"-\n"
	"Internet >\n"
	"Tools >\n"
	"Control Panel\n"
	"-\n"
	"Logout... = [End this GemWM session?] gemwm msg quit\n"
	"Restart... = [Restart the computer?] systemctl reboot\n"
	"Shutdown... = [Shut down the computer?] systemctl poweroff\n"
	"\n"
	"[Desk > Internet]\n"
	"Web Browser = if command -v gemweb >/dev/null; then exec gemweb;"
	" else exec gtk-launch \"$(xdg-settings get default-web-browser)\"; fi\n"
	"\n"
	"[Desk > Tools]\n"
	"Terminal = ${TERMINAL:-gemwm-terminal}\n"
	"\n"
	"[File]\n"
	"Open\n"
	"Show Info...\n"
	"-\n"
	"New Folder\n"
	"Close\n"
	"Close Window\n"
	"-\n"
	"Format...\n"
	"\n"
	"[View]\n"
	"Show as Icons\n"
	"Show as Text\n"
	"-\n"
	"Sort by Name\n"
	"Sort by Date\n"
	"Sort by Size\n"
	"Sort by Type\n"
	"\n"
	"[Options]\n"
	"Mode >\n"
	"Terminal Theme >\n"
	"-\n"
	"Install Disk Drive...\n"
	"Install Application...\n"
	"Set Preferences...\n"
	"Save Desktop\n"
	"Print Screen = gemwm-screenshot screen\n"
	"\n"
	"[Options > Mode]\n"
	"Window = gemwm msg mode window\n"
	"Tiling = gemwm msg mode tiling\n"
	"Scrolling = gemwm msg mode scrolling\n"
	"\n"
	"[Options > Terminal Theme]\n"
	"Light = gemwm-terminal theme light\n"
	"Dark = gemwm-terminal theme dark\n"
	"\n"
	"# Programs at the right of the bar, left to right; see the top of\n"
	"# menu.c.\n"
	"[" APPS_SECTION "]\n"
	"Bluetooth = exec gemwm-bluetooth --menu-app\n"
	"Volume = exec gemwm-volume --menu-app\n"
	"Battery = exec gemwm-battery\n"
	"Clock = exec gemwm-clock\n";

struct menu;

struct item {
	char *label;
	char *command;        /* NULL: disabled (unless it has a submenu) */
	char *confirm;        /* if set, an alert box asks this first */
	struct menu *submenu; /* opens to the side when hovered */
	bool separator;
	bool ipc;             /* command goes to GemWM's control socket */
	bool checked;
	char *shortcut;       /* shown at the right, e.g. "^T" */
};

struct menu {
	char *title;
	struct item items[MAX_ITEMS];
	int n_items;
	int x, w;          /* title position in the bar (top-level menus) */
	int drop_w, drop_h;
};

/* A program with an item at the right of the bar. */
struct menu_app {
	char *name, *command;
	pid_t pid;
	int fd;                /* its stdin and stdout; -1 when not running */
	char in[1024];         /* output not yet a whole line */
	size_t len;
	char *text, *icon_spec; /* icon_spec: a PNG's path or "bitmap:..." */
	cairo_surface_t *icon;
	bool inverse;
	char *tooltip;
	int x, w;              /* where it was drawn, for clicks; w 0: hidden */
	time_t started;
	time_t restart_at;     /* 0: not waiting to restart */
};

/* One open drop-down: the menu itself, or a submenu beside it. */
struct level {
	struct menu *menu;
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	int x, y;  /* position on screen */
	int hover; /* highlighted item, or -1 */
};

struct buffer {
	struct wl_buffer *wl_buffer;
	void *data;
	size_t size;
	cairo_surface_t *surface;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;

	struct wl_surface *bar;
	struct zwlr_layer_surface_v1 *bar_layer;
	int bar_w;

	/* Present only while a menu is open. The drop-downs are subsurfaces of
	 * the overlay. */
	struct wl_surface *overlay;
	struct zwlr_layer_surface_v1 *overlay_layer;
	struct wp_viewport *overlay_viewport;
	bool overlay_configured;
	int overlay_w, overlay_h;
	struct level levels[MAX_DEPTH];
	int depth; /* number of open levels */

	struct menu menus[MAX_MENUS]; /* the desktop's, from the config */
	int n_menus;
	/* What the bar shows: the desktop's menus, or while a window has
	 * focus, Desk, its window menu and Options. */
	struct menu *bar_menus[MAX_MENUS];
	int n_bar_menus;
	int open; /* index of the open menu in bar_menus, or -1 */

	/* The focused window, from the control socket; 0 when none. */
	unsigned focus_id;
	int focus_ws;
	bool focus_maximized;
	char focus_app[256];
	char *focus_menus; /* the window's own menus, as GemWM sends them */
	struct menu window_menu;
	struct menu app_menus[MAX_MENUS];
	int n_app_menus;
	struct menu options;  /* the window's Options items, then the desktop's */
	bool rebuild_pending; /* menus changed while one was open */

	struct wl_surface *pointer_surface;
	double px, py;
	const char *font;
	int quiet_title; /* title just clicked shut: no hover-open until left */

	/* Workspaces, from the control socket. ws_count 0: no buttons. */
	/* A GEM alert box asking before a command runs. */
	struct {
		bool active;
		char *text, *command, *action;
		struct wl_surface *surface;
		struct wl_subsurface *subsurface;
		int x, y, w, h;
		int button_x[2], button_y, button_w[2], button_h; /* action, Cancel */
	} alert;

	struct menu_app apps[MAX_APPS];
	int n_apps;
	uint32_t button; /* the button being pressed, for menu apps */
	double scrolled; /* over a menu app, not yet a whole notch */

	/* The tooltip of the menu app under the pointer: due at tip_due (ms,
	 * monotonic; 0 if not), shown in a subsurface below the bar. */
	struct menu_app *tip_app;
	int64_t tip_due;
	bool tip_shown;
	struct wl_surface *tip_surface;
	struct wl_subsurface *tip_subsurface;

	int ipc_fd;
	char ipc_in[65536];
	size_t ipc_len;
	int ws_count, ws_active;
	char wm_mode[16]; /* "window" or "tiling", for the check mark */
	int ws_x[MAX_WS + 1], ws_w[MAX_WS + 1]; /* buttons, then "+" */
	bool running;
} st;

/* ---- Config ------------------------------------------------------------- */

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			end[-1] == '\n' || end[-1] == '\r')) {
		*--end = '\0';
	}
	return s;
}

static void clear_menu(struct menu *menu) {
	for (int i = 0; i < menu->n_items; i++) {
		struct item *item = &menu->items[i];
		if (item->submenu != NULL) {
			clear_menu(item->submenu);
			free(item->submenu->title);
			free(item->submenu);
		}
		free(item->label);
		free(item->command);
		free(item->confirm);
		free(item->shortcut);
	}
	memset(menu->items, 0, sizeof(menu->items));
	menu->n_items = 0;
}

static struct item *add_item(struct menu *menu) {
	if (menu->n_items == MAX_ITEMS) {
		return NULL;
	}
	return &menu->items[menu->n_items++];
}

static struct item *find_item(struct menu *menu, const char *label) {
	for (int i = 0; i < menu->n_items; i++) {
		struct item *item = &menu->items[i];
		if (item->label != NULL && strcmp(item->label, label) == 0) {
			return item;
		}
	}
	return NULL;
}

/* Turns an item into a (still empty) submenu entry. */
static void make_submenu(struct item *item) {
	if (item->submenu != NULL) {
		return;
	}
	free(item->command);
	item->command = NULL;
	item->submenu = calloc(1, sizeof(*item->submenu));
	item->submenu->title = strdup(item->label);
}

/* The menu a "[Desk > Internet]" header refers to, created as needed. With
 * replace (the user's file), its existing items are dropped. */
static struct menu *section(char *spec, bool replace) {
	char *parts[MAX_DEPTH];
	int n = 0;
	for (char *p = strtok(spec, ">"); p != NULL && n < MAX_DEPTH;
			p = strtok(NULL, ">")) {
		p = trim(p);
		if (p[0] != '\0') {
			parts[n++] = p;
		}
	}
	if (n == 0) {
		return NULL;
	}

	struct menu *menu = NULL;
	for (int i = 0; i < st.n_menus; i++) {
		if (strcmp(st.menus[i].title, parts[0]) == 0) {
			menu = &st.menus[i];
		}
	}
	if (menu == NULL) {
		if (st.n_menus == MAX_MENUS) {
			return NULL;
		}
		menu = &st.menus[st.n_menus++];
		menu->title = strdup(parts[0]);
	}

	for (int i = 1; i < n; i++) {
		struct item *item = find_item(menu, parts[i]);
		if (item == NULL) {
			if ((item = add_item(menu)) == NULL) {
				return NULL;
			}
			item->label = strdup(parts[i]);
		}
		make_submenu(item);
		menu = item->submenu;
	}
	if (replace) {
		clear_menu(menu);
	}
	return menu;
}

static void parse_config(FILE *f, bool replace) {
	char *line = NULL;
	size_t cap = 0;
	struct menu *menu = NULL;
	while (getline(&line, &cap, f) != -1) {
		char *s = trim(line);
		if (s[0] == '\0' || s[0] == '#') {
			continue;
		}
		if (s[0] == '[') {
			char *end = strchr(s, ']');
			if (end != NULL) {
				*end = '\0';
			}
			menu = section(s + 1, replace);
			continue;
		}
		if (menu == NULL) {
			continue;
		}
		if (strcmp(s, "-") == 0) {
			struct item *item = add_item(menu);
			if (item != NULL) {
				item->separator = true;
			}
			continue;
		}
		char *command = NULL;
		char *eq = strchr(s, '=');
		if (eq != NULL) {
			*eq = '\0';
			command = trim(eq + 1);
		}
		char *label = trim(s);
		size_t len = strlen(label);
		bool placeholder = eq == NULL && len > 0 && label[len - 1] == '>';
		if (placeholder) {
			label[len - 1] = '\0';
			label = trim(label);
		}

		/* "Label >" may name a submenu whose header came first. */
		struct item *item = placeholder ? find_item(menu, label) : NULL;
		if (item == NULL) {
			if ((item = add_item(menu)) == NULL) {
				continue;
			}
			item->label = strdup(label);
		}
		/* "[Question?] command" asks before running. "|" breaks lines,
		 * as in GEM's alert strings. */
		if (command != NULL && command[0] == '[') {
			char *end = strchr(command, ']');
			if (end != NULL) {
				*end = '\0';
				item->confirm = strdup(command + 1);
				command = trim(end + 1);
			}
		}
		if (placeholder) {
			make_submenu(item);
		} else if (command != NULL && command[0] != '\0') {
			item->command = strdup(command);
		}
	}
	free(line);
}

static void load_config(void) {
	FILE *f = fmemopen((void *)default_config, strlen(default_config), "r");
	parse_config(f, false);
	fclose(f);

	char path[4096];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg != NULL && xdg[0] != '\0') {
		snprintf(path, sizeof(path), "%s/gemwm/menu", xdg);
	} else {
		snprintf(path, sizeof(path), "%s/.config/gemwm/menu",
			home ? home : "");
	}
	if ((f = fopen(path, "r")) != NULL) {
		parse_config(f, true);
		fclose(f);
	}

	/* [Menu Apps] isn't a menu: its items are the programs to run. */
	for (int i = 0; i < st.n_menus; i++) {
		struct menu *m = &st.menus[i];
		if (strcmp(m->title, APPS_SECTION) != 0) {
			continue;
		}
		for (int j = 0; j < m->n_items && st.n_apps < MAX_APPS; j++) {
			struct item *item = &m->items[j];
			if (item->command == NULL) {
				continue;
			}
			struct menu_app *a = &st.apps[st.n_apps++];
			a->name = strdup(item->label);
			a->command = strdup(item->command);
			a->fd = -1;
		}
		clear_menu(m);
	}

	/* An emptied menu disappears from the bar. */
	int kept = 0;
	for (int i = 0; i < st.n_menus; i++) {
		if (st.menus[i].n_items > 0) {
			st.menus[kept++] = st.menus[i];
		} else {
			free(st.menus[i].title);
		}
	}
	st.n_menus = kept;
}

/* Items that switch GemWM's window mode show a check mark on the current
 * one; the compositor tells us the mode over the control socket. */
static bool item_checked(const struct item *item) {
	static const char prefix[] = "gemwm msg mode ";
	if (item->checked) {
		return true;
	}
	return item->command != NULL && st.wm_mode[0] != '\0' &&
		strncmp(item->command, prefix, sizeof(prefix) - 1) == 0 &&
		strcmp(item->command + sizeof(prefix) - 1, st.wm_mode) == 0;
}

static bool item_enabled(const struct item *item) {
	if (item->separator) {
		return false;
	}
	return item->command != NULL ||
		(item->submenu != NULL && item->submenu->n_items > 0);
}

/* ---- Drawing helpers ---------------------------------------------------- */

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
	struct buffer *buffer = data;
	wl_buffer_destroy(buffer->wl_buffer);
	cairo_surface_destroy(buffer->surface);
	munmap(buffer->data, buffer->size);
	free(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

/* A one-shot shm buffer; it frees itself when the compositor releases it. */
static struct buffer *buffer_create(int w, int h) {
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("gemwm-menu", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, size);
	struct buffer *buffer = calloc(1, sizeof(*buffer));
	buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	buffer->data = data;
	buffer->size = size;
	buffer->surface = cairo_image_surface_create_for_data(data,
		CAIRO_FORMAT_ARGB32, w, h, stride);
	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
	return buffer;
}

static void submit(struct wl_surface *surface, struct buffer *buffer,
		int w, int h) {
	cairo_surface_flush(buffer->surface);
	wl_surface_attach(surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	wl_surface_commit(surface);
}

static cairo_t *begin(cairo_surface_t *surface) {
	cairo_t *cr = cairo_create(surface);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_select_font_face(cr, st.font, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE);
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	cairo_set_font_options(cr, opts);
	cairo_font_options_destroy(opts);
	return cr;
}

/* Text vertically centred in a row, left edge at x. */
static void text(cairo_t *cr, const char *s, int x, int y, int row_h) {
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	int baseline = y + (int)((row_h - (fe.ascent + fe.descent)) / 2 + fe.ascent);
	cairo_move_to(cr, x, baseline);
	cairo_show_text(cr, s);
}

/* A mask letting through every other pixel, for GEM's greyed-out look. */
static cairo_pattern_t *checker_mask(void) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 2, 2);
	unsigned char *px = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	px[0] = 0xff;
	px[1] = 0x00;
	px[stride] = 0x00;
	px[stride + 1] = 0xff;
	cairo_surface_mark_dirty(s);
	cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
	cairo_surface_destroy(s);
	cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
	return p;
}

/* Works out a drop-down's size, and those of its submenus. */
static void layout_drop(cairo_t *cr, struct menu *m) {
	int widest = 0, widest_shortcut = 0;
	bool arrows = false;
	for (int i = 0; i < m->n_items; i++) {
		struct item *item = &m->items[i];
		cairo_text_extents_t te;
		if (item->label != NULL) {
			cairo_text_extents(cr, item->label, &te);
			if ((int)te.x_advance > widest) {
				widest = (int)te.x_advance;
			}
		}
		if (item->shortcut != NULL && item->shortcut[0] != '\0') {
			cairo_text_extents(cr, item->shortcut, &te);
			if ((int)te.x_advance > widest_shortcut) {
				widest_shortcut = (int)te.x_advance;
			}
		}
		if (item->submenu != NULL) {
			arrows = true;
			layout_drop(cr, item->submenu);
		}
	}
	m->drop_w = widest + 2 * ITEM_PAD + 2 + (arrows ? ARROW_W : 0) +
		(widest_shortcut > 0 ? SHORTCUT_GAP + widest_shortcut : 0);
	m->drop_h = m->n_items * ITEM_H + 2;
}

/* Works out title positions and menu sizes from the font. */
static void layout_menus(void) {
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = begin(scratch);
	int x = TITLE_PAD;
	for (int i = 0; i < st.n_bar_menus; i++) {
		struct menu *m = st.bar_menus[i];
		cairo_text_extents_t te;
		cairo_text_extents(cr, m->title, &te);
		m->x = x;
		m->w = (int)te.x_advance + 2 * TITLE_PAD;
		x += m->w;
		layout_drop(cr, m);
		if (m->drop_w < m->w + 2) {
			m->drop_w = m->w + 2;
		}
	}
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
}

/* ---- The bar ------------------------------------------------------------ */

static void draw_bar(void);

/* "1 | 2 | 3 | +", centred. The active workspace is inverted like an open
 * menu title. Remembers where each button is for clicks. */
static void draw_workspaces(cairo_t *cr) {
	int n = st.ws_count > MAX_WS ? MAX_WS : st.ws_count;
	if (n <= 0) {
		return;
	}
	int cells = n + 1;
	int total = 0;
	for (int i = 0; i < cells; i++) {
		char label[8];
		snprintf(label, sizeof(label), i < n ? "%d" : "+", i + 1);
		cairo_text_extents_t te;
		cairo_text_extents(cr, label, &te);
		st.ws_w[i] = (int)te.x_advance + 2 * WS_PAD;
		total += st.ws_w[i] + (i > 0 ? 2 * WS_GAP + 1 : 0);
	}
	int x = (st.bar_w - total) / 2;
	for (int i = 0; i < cells; i++) {
		if (i > 0) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, x + WS_GAP, 4, 1, BAR_H - 9);
			cairo_fill(cr);
			x += 2 * WS_GAP + 1;
		}
		st.ws_x[i] = x;
		char label[8];
		snprintf(label, sizeof(label), i < n ? "%d" : "+", i + 1);
		if (i + 1 == st.ws_active) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, x, 0, st.ws_w[i], BAR_H - 1);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
		} else {
			cairo_set_source_rgb(cr, 0, 0, 0);
		}
		text(cr, label, x + WS_PAD, 0, BAR_H - 1);
		x += st.ws_w[i];
	}
}

/* The workspace button at x: 0-based index, n for "+", or -1. */
static int workspace_at(double x) {
	int n = st.ws_count > MAX_WS ? MAX_WS : st.ws_count;
	for (int i = 0; n > 0 && i <= n; i++) {
		if (x >= st.ws_x[i] && x < st.ws_x[i] + st.ws_w[i]) {
			return i;
		}
	}
	return -1;
}

/* ---- Control socket ----------------------------------------------------- */

static void close_menu(void);

/* ---- The window menu ---------------------------------------------------- */

/* The Name= of the application's .desktop file, which is usually named
 * after its app ID ("firefox" -> Firefox); failing that, the ID's last
 * part ("org.gemwm.GemWeb" -> GemWeb). */
static void app_name(const char *app_id, char *out, size_t n) {
	if (app_id[0] == '\0') {
		snprintf(out, n, "Window");
		return;
	}
	char dirs[4096];
	const char *home = getenv("HOME");
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (data_home != NULL && data_home[0] != '\0') {
		snprintf(dirs, sizeof(dirs), "%s:%s", data_home,
			data_dirs && data_dirs[0] ? data_dirs : "/usr/local/share:/usr/share");
	} else {
		snprintf(dirs, sizeof(dirs), "%s/.local/share:%s", home ? home : "",
			data_dirs && data_dirs[0] ? data_dirs : "/usr/local/share:/usr/share");
	}
	char *save = NULL;
	for (char *dir = strtok_r(dirs, ":", &save); dir != NULL;
			dir = strtok_r(NULL, ":", &save)) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/applications/%s.desktop", dir, app_id);
		FILE *f = fopen(path, "r");
		if (f == NULL) {
			continue;
		}
		char *line = NULL;
		size_t cap = 0;
		bool found = false, in_entry = false;
		while (!found && getline(&line, &cap, f) != -1) {
			char *l = trim(line);
			if (l[0] == '[') {
				in_entry = strcmp(l, "[Desktop Entry]") == 0;
			} else if (in_entry && strncmp(l, "Name=", 5) == 0 && l[5] != '\0') {
				snprintf(out, n, "%s", l + 5);
				found = true;
			}
		}
		free(line);
		fclose(f);
		if (found) {
			return;
		}
	}
	const char *dot = strrchr(app_id, '.');
	snprintf(out, n, "%s", dot != NULL && dot[1] != '\0' ? dot + 1 : app_id);
	if (out[0] >= 'a' && out[0] <= 'z') {
		out[0] -= 'a' - 'A';
	}
}

static struct item *add_ipc_item(struct menu *m, const char *label,
		const char *format, ...) {
	struct item *item = add_item(m);
	if (item == NULL) {
		return NULL;
	}
	item->label = strdup(label);
	if (format != NULL) {
		va_list args;
		va_start(args, format);
		if (vasprintf(&item->command, format, args) < 0) {
			item->command = NULL;
		}
		va_end(args);
		item->ipc = true;
	}
	return item;
}

/* What GemWM can do to any window, for the focused one. */
static void build_window_menu(void) {
	struct menu *m = &st.window_menu;
	clear_menu(m);
	free(m->title);
	char name[256];
	app_name(st.focus_app, name, sizeof(name));
	if (strlen(name) > APP_NAME_MAX) {
		strcpy(name + APP_NAME_MAX - 3, "...");
	}
	m->title = strdup(name);

	unsigned id = st.focus_id;
	add_ipc_item(m, "Close Window", "close-window %u", id);
	struct item *full = add_ipc_item(m, "Full Size", "maximize %u", id);
	if (full != NULL) {
		full->checked = st.focus_maximized;
	}
	struct item *sep = add_item(m);
	if (sep != NULL) {
		sep->separator = true;
	}
	struct item *move = add_ipc_item(m, "Move to Workspace", NULL);
	if (move == NULL) {
		return;
	}
	make_submenu(move);
	for (int i = 1; i <= st.ws_count && i < MAX_ITEMS - 1; i++) {
		char label[32];
		snprintf(label, sizeof(label), "Workspace %d", i);
		/* Where it is now: checked, and nothing to do. */
		struct item *ws = add_ipc_item(move->submenu, label,
			i == st.focus_ws ? NULL : "move-window %u %d", id, i);
		if (ws != NULL) {
			ws->checked = i == st.focus_ws;
		}
	}
	add_ipc_item(move->submenu, "New Workspace", "move-window %u new", id);
}

/* The contents of a JSON string, from just after its opening quote, as
 * the control socket writes them: the only escapes are \" \\ and \u00XX. */
static char *json_unescape(const char *s) {
	char *out = malloc(strlen(s) + 1), *o = out;
	for (; *s != '\0' && *s != '"'; s++) {
		unsigned c;
		if (*s != '\\' || s[1] == '\0') {
			*o++ = *s;
		} else if (*++s == 'u' && sscanf(s + 1, "%4x", &c) == 1) {
			*o++ = (char)c;
			s += 4;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
	return out;
}

/* A string strcmp that allows NULL. */
static int strcmp_null(const char *a, const char *b) {
	return a == NULL || b == NULL ? (a != NULL) - (b != NULL) : strcmp(a, b);
}

/* The window's own menus, from the text GemWM sends (src/appmenu.c). */
static void build_app_menus(void) {
	for (int i = 0; i < st.n_app_menus; i++) {
		clear_menu(&st.app_menus[i]);
		free(st.app_menus[i].title);
	}
	memset(st.app_menus, 0, sizeof(st.app_menus));
	st.n_app_menus = 0;
	if (st.focus_menus == NULL) {
		return;
	}
	char *text = strdup(st.focus_menus), *save = NULL;
	struct menu *m = NULL;
	for (char *line = strtok_r(text, "\n", &save); line != NULL;
			line = strtok_r(NULL, "\n", &save)) {
		char *f[5] = { 0 };
		int n = 0;
		for (char *p = line; n < 5; n++) {
			f[n] = p;
			char *tab = strchr(p, '\t');
			if (tab == NULL) {
				n++;
				break;
			}
			*tab = '\0';
			p = tab + 1;
		}
		if (strcmp(f[0], "M") == 0 && n >= 2) {
			m = st.n_app_menus < MAX_MENUS - 3 ? /* room for Desk and co. */
				&st.app_menus[st.n_app_menus++] : NULL;
			if (m != NULL) {
				m->title = strdup(f[1]);
			}
		} else if (strcmp(f[0], "S") == 0 && m != NULL) {
			struct item *item = add_item(m);
			if (item != NULL) {
				item->separator = true;
			}
		} else if (strcmp(f[0], "I") == 0 && n >= 4 && m != NULL) {
			unsigned flags = (unsigned)strtoul(f[2], NULL, 10);
			struct item *item = add_ipc_item(m, f[3],
				flags & APP_MENU_DISABLED ? NULL : "menu-activate %u %s",
				st.focus_id, f[1]);
			if (item != NULL) {
				item->checked = flags & APP_MENU_CHECKED;
				item->shortcut = n >= 5 ? strdup(f[4]) : NULL;
			}
		}
	}
	free(text);
}

/* Options: the window's own Options items (if it has that menu), a
 * separator, then GemWM's. Items are shared with both, not copied. */
static struct menu *merge_options(struct menu *app, struct menu *desktop) {
	if (app == NULL || desktop == NULL) {
		return app != NULL ? app : desktop;
	}
	struct menu *m = &st.options;
	memset(m, 0, sizeof(*m));
	m->title = desktop->title;
	for (int i = 0; i < app->n_items && m->n_items < MAX_ITEMS; i++) {
		m->items[m->n_items++] = app->items[i];
	}
	if (m->n_items < MAX_ITEMS) {
		m->items[m->n_items++].separator = true;
	}
	for (int i = 0; i < desktop->n_items && m->n_items < MAX_ITEMS; i++) {
		m->items[m->n_items++] = desktop->items[i];
	}
	return m;
}

static struct menu *desktop_menu(const char *title) {
	for (int i = 0; i < st.n_menus; i++) {
		if (strcmp(st.menus[i].title, title) == 0) {
			return &st.menus[i];
		}
	}
	return NULL;
}

/* Works out what the bar shows, after the focus changed. */
static void rebuild_bar(void) {
	close_menu(); /* its menu may be gone */
	st.n_bar_menus = 0;
	if (st.focus_id == 0) {
		for (int i = 0; i < st.n_menus; i++) {
			st.bar_menus[st.n_bar_menus++] = &st.menus[i];
		}
	} else {
		build_window_menu();
		build_app_menus();
		struct menu *desk = desktop_menu(DESK_MENU);
		struct menu *app_options = NULL;
		if (desk != NULL) {
			st.bar_menus[st.n_bar_menus++] = desk;
		}
		st.bar_menus[st.n_bar_menus++] = &st.window_menu;
		for (int i = 0; i < st.n_app_menus; i++) {
			if (strcmp(st.app_menus[i].title, OPTIONS_MENU) == 0) {
				app_options = &st.app_menus[i];
			} else {
				st.bar_menus[st.n_bar_menus++] = &st.app_menus[i];
			}
		}
		struct menu *options = merge_options(app_options,
			desktop_menu(OPTIONS_MENU));
		if (options != NULL) {
			st.bar_menus[st.n_bar_menus++] = options;
		}
	}
	layout_menus();
	draw_bar();
}

static void ipc_send(const char *command) {
	if (st.ipc_fd < 0) {
		return;
	}
	char line[64];
	int n = snprintf(line, sizeof(line), "%s\n", command);
	if (write(st.ipc_fd, line, n) != n) {
		fprintf(stderr, "gemwm-menu: lost the control socket\n");
	}
}

static void ipc_connect(void) {
	st.ipc_fd = -1;
	const char *path = getenv("GEMWM_SOCKET");
	if (path == NULL || path[0] == '\0') {
		return;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "gemwm-menu: can't connect to %s\n", path);
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	st.ipc_fd = fd;
	ipc_send("subscribe");
}

/* Reads events; only "workspaces" matters to the bar. The compositor puts
 * "active" and "count" first in that event, so a prefix match is enough. */
static void ipc_read(void) {
	ssize_t n = read(st.ipc_fd, st.ipc_in + st.ipc_len,
		sizeof(st.ipc_in) - 1 - st.ipc_len);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			return;
		}
		close(st.ipc_fd);
		st.ipc_fd = -1;
		st.ws_count = 0;
		st.focus_id = 0;
		rebuild_bar();
		return;
	}
	st.ipc_len += n;
	st.ipc_in[st.ipc_len] = '\0';

	bool changed = false, menus_changed = false;
	char *start = st.ipc_in, *nl;
	while ((nl = strchr(start, '\n')) != NULL) {
		*nl = '\0';
		int active, count;
		if (sscanf(start, "{\"event\":\"workspaces\",\"active\":%d,\"count\":%d",
				&active, &count) == 2) {
			changed |= active != st.ws_active || count != st.ws_count;
			/* The window menu lists the workspaces. */
			menus_changed |= st.focus_id != 0 && count != st.ws_count;
			st.ws_active = active;
			st.ws_count = count;
		}
		unsigned id;
		int ws;
		char maximized[8], app[sizeof(st.focus_app)] = "";
		int n = sscanf(start, "{\"event\":\"focus\",\"window\":%u,"
			"\"workspace\":%d,\"maximized\":%7[a-z],\"app_id\":\"%255[^\"]",
			&id, &ws, maximized, app);
		if (n >= 1) {
			bool max = n >= 3 && strcmp(maximized, "true") == 0;
			ws = n >= 2 ? ws : 0;
			static const char key[] = ",\"menus\":\"";
			char *menus_at = strstr(start, key);
			char *menus = menus_at != NULL ?
				json_unescape(menus_at + sizeof(key) - 1) : NULL;
			if (id != st.focus_id || ws != st.focus_ws ||
					max != st.focus_maximized || strcmp(app, st.focus_app) != 0 ||
					strcmp_null(menus, st.focus_menus) != 0) {
				st.focus_id = id;
				st.focus_ws = ws;
				st.focus_maximized = max;
				snprintf(st.focus_app, sizeof(st.focus_app), "%s", app);
				free(st.focus_menus);
				st.focus_menus = menus;
				menus_changed = true;
			} else {
				free(menus);
			}
		}
		sscanf(start, "{\"event\":\"mode\",\"mode\":\"%15[a-z]\"", st.wm_mode);
		start = nl + 1;
	}
	st.ipc_len -= start - st.ipc_in;
	memmove(st.ipc_in, start, st.ipc_len);
	if (st.ipc_len == sizeof(st.ipc_in) - 1) {
		st.ipc_len = 0; /* a line too long to be ours; drop it */
	}
	if (menus_changed && st.open >= 0) {
		/* Not under the pointer: when the open menu closes. */
		st.rebuild_pending = true;
	} else if (menus_changed) {
		rebuild_bar(); /* draws it too */
	} else if (changed) {
		draw_bar();
	}
}

/* ---- Menu apps ---------------------------------------------------------- */

static struct menu_app *app_at(double x);

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tooltip_hide(void) {
	if (st.tip_shown) {
		wl_surface_attach(st.tip_surface, NULL, 0, 0);
		wl_surface_commit(st.tip_surface);
		st.tip_shown = false;
	}
	st.tip_app = NULL;
	st.tip_due = 0;
}

/* A box below the item, as GEM draws its menus: white, black outline. It
 * takes no input, so the pointer goes through to whatever is under it. */
static void tooltip_show(void) {
	struct menu_app *a = st.tip_app;
	st.tip_due = 0;
	if (a == NULL || a->tooltip == NULL || a->w == 0) {
		tooltip_hide();
		return;
	}
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = begin(scratch);
	cairo_text_extents_t te;
	cairo_text_extents(cr, a->tooltip, &te);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
	int w = (int)te.x_advance + 2 * TOOLTIP_PAD + 2, h = ITEM_H + 2;
	struct buffer *buffer = buffer_create(w, h);
	if (buffer == NULL) {
		return;
	}
	cr = begin(buffer->surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
	cairo_stroke(cr);
	text(cr, a->tooltip, TOOLTIP_PAD + 1, 1, ITEM_H);
	cairo_destroy(cr);

	if (st.tip_surface == NULL) {
		st.tip_surface = wl_compositor_create_surface(st.compositor);
		st.tip_subsurface = wl_subcompositor_get_subsurface(st.subcompositor,
			st.tip_surface, st.bar);
		wl_subsurface_set_desync(st.tip_subsurface);
		struct wl_region *none = wl_compositor_create_region(st.compositor);
		wl_surface_set_input_region(st.tip_surface, none);
		wl_region_destroy(none);
	}
	/* Under the item, kept on screen; the top edge overlaps the bar's line. */
	int x = a->x + w > st.bar_w ? st.bar_w - w : a->x;
	wl_subsurface_set_position(st.tip_subsurface, x < 0 ? 0 : x, BAR_H - 1);
	submit(st.tip_surface, buffer, w, h);
	wl_surface_commit(st.bar); /* the position applies with the bar's commit */
	st.tip_shown = true;
}

/* Follows the pointer over the bar: resting on a menu app with a tooltip
 * shows it after a moment. */
static void tooltip_track(double x, double y) {
	struct menu_app *a = st.pointer_surface == st.bar && st.open < 0 &&
		y < BAR_H ? app_at(x) : NULL;
	if (a == st.tip_app) {
		return;
	}
	tooltip_hide();
	if (a != NULL && a->tooltip != NULL) {
		st.tip_app = a;
		st.tip_due = now_ms() + TOOLTIP_DELAY;
	}
}

static void app_start(struct menu_app *a) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		char self[4096];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		char *slash = n > 0 ? memrchr(self, '/', n) : NULL;
		if (slash != NULL) {
			*slash = '\0';
			const char *path = getenv("PATH");
			char *new_path;
			if (asprintf(&new_path, "%s:%s", self,
					path ? path : "/usr/bin:/bin") > 0) {
				setenv("PATH", new_path, 1);
			}
		}
		execl("/bin/sh", "/bin/sh", "-c", a->command, (void *)NULL);
		_exit(127);
	}
	close(sv[1]);
	if (pid < 0) {
		close(sv[0]);
		return;
	}
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	a->pid = pid;
	a->fd = sv[0];
	a->len = 0;
	a->started = time(NULL);
	a->restart_at = 0;
}

/* "bitmap:WxH:hex" as an image: black where a bit is set, else clear. */
static cairo_surface_t *load_bitmap(const char *spec) {
	int w, h, offset;
	if (sscanf(spec, "bitmap:%dx%d:%n", &w, &h, &offset) != 2 ||
			w <= 0 || h <= 0 || w > 256 || h > BAR_H) {
		return NULL;
	}
	const char *hex = spec + offset;
	int row_bytes = (w + 7) / 8;
	if (strlen(hex) != (size_t)(row_bytes * h * 2)) {
		return NULL;
	}
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s) / 4;
	for (int y = 0; y < h; y++) {
		for (int b = 0; b < row_bytes; b++) {
			unsigned byte;
			if (sscanf(hex + (y * row_bytes + b) * 2, "%2x", &byte) != 1) {
				cairo_surface_destroy(s);
				return NULL;
			}
			for (int bit = 0; bit < 8 && b * 8 + bit < w; bit++) {
				px[y * stride + b * 8 + bit] =
					byte & (0x80 >> bit) ? 0xff000000 : 0;
			}
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

static cairo_surface_t *load_icon(const struct menu_app *a, const char *spec) {
	cairo_surface_t *s;
	if (strncmp(spec, "bitmap:", 7) == 0) {
		s = load_bitmap(spec);
	} else {
		s = cairo_image_surface_create_from_png(spec);
		if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(s);
			s = NULL;
		}
	}
	if (s == NULL) {
		fprintf(stderr, "gemwm-menu: %s: bad icon %s\n", a->name, spec);
	}
	return s;
}

/* One line from the app: "text", or "icon<TAB>text", optionally followed
 * by "<TAB>flags" and "<TAB>tooltip". */
static void app_set(struct menu_app *a, char *line) {
	char *fields[4] = { NULL, line, NULL, NULL };
	char *tab = strchr(line, '\t');
	if (tab != NULL) {
		*tab = '\0';
		fields[0] = line;
		fields[1] = tab + 1;
		for (int i = 2; i < 4 && (tab = strchr(fields[i - 1], '\t')) != NULL;
				i++) {
			*tab = '\0';
			fields[i] = tab + 1;
		}
	}
	const char *spec = fields[0];
	if (spec == NULL || a->icon_spec == NULL || strcmp(spec, a->icon_spec) != 0) {
		if (a->icon != NULL) {
			cairo_surface_destroy(a->icon);
			a->icon = NULL;
		}
		free(a->icon_spec);
		a->icon_spec = spec != NULL ? strdup(spec) : NULL;
		if (spec != NULL && spec[0] != '\0') {
			a->icon = load_icon(a, spec);
		}
	}
	free(a->text);
	a->text = strdup(fields[1]);
	a->inverse = fields[2] != NULL && strcmp(fields[2], "inverse") == 0;
	free(a->tooltip);
	a->tooltip = fields[3] != NULL && fields[3][0] != '\0' ?
		strdup(fields[3]) : NULL;
}

/* The app exited (or closed its end): its item goes, and it's restarted
 * unless it died right away, which would only repeat. */
static void app_stopped(struct menu_app *a) {
	close(a->fd);
	a->fd = -1;
	kill(-a->pid, SIGTERM);
	waitpid(a->pid, NULL, WNOHANG);
	free(a->text);
	a->text = NULL;
	if (st.tip_app == a) {
		tooltip_hide();
	}
	if (time(NULL) - a->started >= APP_RESTART) {
		a->restart_at = time(NULL) + 1;
	} else {
		fprintf(stderr, "gemwm-menu: menu app %s exited\n", a->name);
	}
	draw_bar();
}

static void app_read(struct menu_app *a) {
	for (;;) {
		ssize_t n = read(a->fd, a->in + a->len, sizeof(a->in) - 1 - a->len);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			break;
		}
		if (n <= 0) {
			app_stopped(a);
			return;
		}
		a->len += n;
		/* Only the latest whole line matters: the one ending at the last
		 * newline. It's copied out first, as app_set cuts it up. */
		char *end = memrchr(a->in, '\n', a->len);
		if (end != NULL) {
			char *start = memrchr(a->in, '\n', (size_t)(end - a->in));
			start = start != NULL ? start + 1 : a->in;
			char line[sizeof(a->in)];
			memcpy(line, start, (size_t)(end - start));
			line[end - start] = '\0';
			size_t used = (size_t)(end + 1 - a->in);
			memmove(a->in, end + 1, a->len - used);
			a->len -= used;
			app_set(a, line);
			draw_bar();
			if (st.tip_shown && st.tip_app == a) {
				tooltip_show(); /* its text may have changed */
			}
		} else if (a->len == sizeof(a->in) - 1) {
			a->len = 0; /* an absurdly long line: drop it */
		}
	}
}

static void app_click(struct menu_app *a, uint32_t button) {
	int n = button == BTN_RIGHT ? 3 : button == BTN_MIDDLE ? 2 : 1;
	char msg[16];
	int len = snprintf(msg, sizeof(msg), "click %d\n", n);
	if (a->fd >= 0) {
		send(a->fd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
	}
}

/* Draws the menu apps right to left, ending at right, in their listed
 * order; returns the left edge. */
static int draw_apps(cairo_t *cr, int right) {
	for (int i = st.n_apps - 1; i >= 0; i--) {
		struct menu_app *a = &st.apps[i];
		a->w = 0;
		bool has_text = a->text != NULL && a->text[0] != '\0';
		if (a->fd < 0 || (a->icon == NULL && !has_text)) {
			continue;
		}
		int icon_w = a->icon ? cairo_image_surface_get_width(a->icon) : 0;
		int text_w = 0;
		if (has_text) {
			cairo_text_extents_t te;
			cairo_text_extents(cr, a->text, &te);
			text_w = (int)te.x_advance < APP_MAX_W ? (int)te.x_advance : APP_MAX_W;
		}
		int gap = icon_w > 0 && text_w > 0 ? 4 : 0;
		a->w = APP_PAD + icon_w + gap + text_w + APP_PAD;
		a->x = right - a->w;
		right = a->x;
		if (a->icon != NULL) {
			int ih = cairo_image_surface_get_height(a->icon);
			cairo_set_source_surface(cr, a->icon, a->x + APP_PAD,
				(BAR_H - 1 - ih) / 2);
			cairo_paint(cr);
		}
		if (has_text) {
			int tx = a->x + APP_PAD + icon_w + gap;
			cairo_set_source_rgb(cr, 0, 0, 0);
			if (a->inverse) {
				/* As a selected item: to catch the eye (a low battery). */
				cairo_rectangle(cr, tx - 2, 0, text_w + 4, BAR_H - 1);
				cairo_fill(cr);
				cairo_set_source_rgb(cr, 1, 1, 1);
			}
			cairo_save(cr);
			cairo_rectangle(cr, tx, 0, text_w, BAR_H - 1);
			cairo_clip(cr);
			text(cr, a->text, tx, 0, BAR_H - 1);
			cairo_restore(cr);
		}
	}
	return right;
}

static struct menu_app *app_at(double x) {
	for (int i = 0; i < st.n_apps; i++) {
		struct menu_app *a = &st.apps[i];
		if (a->w > 0 && x >= a->x && x < a->x + a->w) {
			return a;
		}
	}
	return NULL;
}

static void draw_bar(void) {
	if (st.bar_w <= 0) {
		return;
	}
	struct buffer *buffer = buffer_create(st.bar_w, BAR_H);
	if (buffer == NULL) {
		return;
	}
	cairo_t *cr = begin(buffer->surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, BAR_H - 1, st.bar_w, 1);
	cairo_fill(cr);

	for (int i = 0; i < st.n_bar_menus; i++) {
		struct menu *m = st.bar_menus[i];
		if (i == st.open) {
			/* The open menu's title is shown inverted. */
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, m->x, 0, m->w, BAR_H - 1);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
		} else {
			cairo_set_source_rgb(cr, 0, 0, 0);
		}
		text(cr, m->title, m->x + TITLE_PAD, 0, BAR_H - 1);
	}

	draw_workspaces(cr);

	draw_apps(cr, st.bar_w);

	cairo_destroy(cr);
	submit(st.bar, buffer, st.bar_w, BAR_H);
}

static int title_at(double x) {
	for (int i = 0; i < st.n_bar_menus; i++) {
		struct menu *m = st.bar_menus[i];
		if (x >= m->x && x < m->x + m->w) {
			return i;
		}
	}
	return -1;
}

/* ---- Drop-downs -------------------------------------------------------- */

static void draw_level(int l) {
	struct level *lv = &st.levels[l];
	struct menu *m = lv->menu;
	struct buffer *buffer = buffer_create(m->drop_w, m->drop_h);
	if (buffer == NULL) {
		return;
	}
	cairo_t *cr = begin(buffer->surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, m->drop_w - 1, m->drop_h - 1);
	cairo_stroke(cr);

	cairo_pattern_t *grey = checker_mask();
	for (int i = 0; i < m->n_items; i++) {
		struct item *item = &m->items[i];
		int y = 1 + i * ITEM_H;
		if (item->separator) {
			/* GEM uses a disabled row of dashes; a dotted rule reads the
			 * same. */
			cairo_rectangle(cr, 1, y + ITEM_H / 2, m->drop_w - 2, 1);
			cairo_save(cr);
			cairo_clip(cr);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_mask(cr, grey);
			cairo_restore(cr);
			continue;
		}

		bool enabled = item_enabled(item);
		bool inverted = enabled && i == lv->hover;
		if (inverted) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, 1, y, m->drop_w - 2, ITEM_H);
			cairo_fill(cr);
		}
		/* Disabled items are drawn through the grey mask. */
		cairo_push_group(cr);
		if (inverted) {
			cairo_set_source_rgb(cr, 1, 1, 1);
		} else {
			cairo_set_source_rgb(cr, 0, 0, 0);
		}
		text(cr, item->label, ITEM_PAD, y, ITEM_H);
		if (item->shortcut != NULL && item->shortcut[0] != '\0') {
			cairo_text_extents_t te;
			cairo_text_extents(cr, item->shortcut, &te);
			text(cr, item->shortcut, m->drop_w - ITEM_PAD - (int)te.x_advance,
				y, ITEM_H);
		}
		if (item_checked(item)) {
			/* GEM's check mark, in the indent left of the label. */
			cairo_set_line_width(cr, 2);
			cairo_move_to(cr, 4, y + ITEM_H / 2.0);
			cairo_line_to(cr, 7, y + ITEM_H / 2.0 + 3);
			cairo_line_to(cr, 12, y + ITEM_H / 2.0 - 4);
			cairo_stroke(cr);
		}
		if (item->submenu != NULL) {
			double ax = m->drop_w - ITEM_PAD / 2 - 5, cy = y + ITEM_H / 2.0;
			cairo_move_to(cr, ax, cy - 4);
			cairo_line_to(cr, ax + 4, cy);
			cairo_line_to(cr, ax, cy + 4);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		cairo_pop_group_to_source(cr);
		if (enabled) {
			cairo_paint(cr);
		} else {
			cairo_mask(cr, grey);
		}
	}
	cairo_pattern_destroy(grey);
	cairo_destroy(cr);

	/* Subsurfaces are synchronized: the overlay commit applies the new
	 * contents and position together. */
	wl_subsurface_set_position(lv->subsurface, lv->x, lv->y);
	submit(lv->surface, buffer, m->drop_w, m->drop_h);
	wl_surface_commit(st.overlay);
}

/* Closes the drop-downs from level `from` down. */
static void close_levels(int from) {
	while (st.depth > from) {
		struct level *lv = &st.levels[--st.depth];
		if (st.pointer_surface == lv->surface) {
			st.pointer_surface = NULL;
		}
		wl_subsurface_destroy(lv->subsurface);
		wl_surface_destroy(lv->surface);
		memset(lv, 0, sizeof(*lv));
	}
	if (st.overlay_configured) {
		wl_surface_commit(st.overlay);
	}
}

/* Opens `menu` as level l at (x, y), nudged to stay on screen. */
static void open_level(int l, struct menu *menu, int x, int y) {
	close_levels(l);
	if (l >= MAX_DEPTH) {
		return;
	}
	if (st.overlay_w > 0 && x + menu->drop_w > st.overlay_w) {
		x = st.overlay_w - menu->drop_w;
	}
	if (st.overlay_h > 0 && y + menu->drop_h > st.overlay_h) {
		y = st.overlay_h - menu->drop_h;
	}
	if (y < BAR_H - 1) {
		y = BAR_H - 1;
	}
	struct level *lv = &st.levels[l];
	lv->menu = menu;
	lv->x = x;
	lv->y = y;
	lv->hover = -1;
	lv->surface = wl_compositor_create_surface(st.compositor);
	lv->subsurface = wl_subcompositor_get_subsurface(st.subcompositor,
		lv->surface, st.overlay);
	st.depth = l + 1;
	draw_level(l);
}

/* Opens item i's submenu beside level l, on the right unless that would
 * run off the screen. Its first row lines up with the item. */
static void open_submenu(int l, int i) {
	struct level *lv = &st.levels[l];
	struct menu *sub = lv->menu->items[i].submenu;
	int x = lv->x + lv->menu->drop_w - 1;
	if (st.overlay_w > 0 && x + sub->drop_w > st.overlay_w) {
		x = lv->x - sub->drop_w + 1;
	}
	open_level(l + 1, sub, x, lv->y + i * ITEM_H);
}

static void set_hover(int l, int item) {
	if (l < st.depth && st.levels[l].hover != item) {
		st.levels[l].hover = item;
		draw_level(l);
	}
}

/* The deepest open level containing (x, y), or -1. */
static int level_at(double x, double y) {
	for (int l = st.depth - 1; l >= 0; l--) {
		struct level *lv = &st.levels[l];
		if (x >= lv->x && x < lv->x + lv->menu->drop_w &&
				y >= lv->y && y < lv->y + lv->menu->drop_h) {
			return l;
		}
	}
	return -1;
}

/* The enabled item of level l at (x, y), or -1. */
static int item_at(int l, double x, double y) {
	struct level *lv = &st.levels[l];
	double ly = y - lv->y - 1;
	if (ly < 0) {
		return -1;
	}
	int i = (int)ly / ITEM_H;
	if (i >= lv->menu->n_items || !item_enabled(&lv->menu->items[i])) {
		return -1;
	}
	return i;
}

/* ---- Opening and closing ------------------------------------------------ */

static void show_alert(void);
static void overlay_create(bool keyboard);

static void overlay_configure(void *data, struct zwlr_layer_surface_v1 *layer,
		uint32_t serial, uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	st.overlay_w = (int)w;
	st.overlay_h = (int)h;

	/* Fully transparent. With viewporter a 1x1 buffer is stretched to the
	 * screen instead of allocating a screen-sized one. */
	int bw = st.viewporter ? 1 : (int)w;
	int bh = st.viewporter ? 1 : (int)h;
	struct buffer *buffer = buffer_create(bw, bh);
	if (buffer == NULL) {
		return;
	}
	memset(buffer->data, 0, buffer->size);
	if (st.overlay_viewport != NULL) {
		wp_viewport_set_destination(st.overlay_viewport, w, h);
	}
	wl_surface_attach(st.overlay, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(st.overlay, 0, 0, bw, bh);

	if (!st.overlay_configured) {
		st.overlay_configured = true;
		if (st.alert.active) {
			show_alert(); /* commits the overlay too */
		} else {
			struct menu *m = st.bar_menus[st.open];
			/* The menu's top edge overlaps the bar's bottom line. */
			open_level(0, m, m->x, BAR_H - 1);
		}
	} else {
		wl_surface_commit(st.overlay);
	}
}

static void close_menu(void);
static void dismiss_alert(void);

static void overlay_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	close_menu();
	dismiss_alert();
}

static const struct zwlr_layer_surface_v1_listener overlay_listener = {
	.configure = overlay_configure,
	.closed = overlay_closed,
};

/* The transparent screen-sized surface that catches every click while a
 * menu or alert is up. An alert also takes the keyboard (Escape). */
static void overlay_create(bool keyboard) {
	st.overlay = wl_compositor_create_surface(st.compositor);
	if (st.viewporter != NULL) {
		st.overlay_viewport =
			wp_viewporter_get_viewport(st.viewporter, st.overlay);
	}
	st.overlay_layer = zwlr_layer_shell_v1_get_layer_surface(
		st.layer_shell, st.overlay, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "gemwm-menu-dropdown");
	zwlr_layer_surface_v1_set_anchor(st.overlay_layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	/* -1: cover the whole screen, including the bar. */
	zwlr_layer_surface_v1_set_exclusive_zone(st.overlay_layer, -1);
	if (keyboard) {
		zwlr_layer_surface_v1_set_keyboard_interactivity(st.overlay_layer,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	}
	zwlr_layer_surface_v1_add_listener(st.overlay_layer,
		&overlay_listener, NULL);
	wl_surface_commit(st.overlay);
}

static void overlay_destroy(void) {
	if (st.overlay == NULL) {
		return;
	}
	if (st.overlay_viewport != NULL) {
		wp_viewport_destroy(st.overlay_viewport);
	}
	zwlr_layer_surface_v1_destroy(st.overlay_layer);
	wl_surface_destroy(st.overlay);
	if (st.pointer_surface == st.overlay) {
		st.pointer_surface = NULL;
	}
	st.overlay_viewport = NULL;
	st.overlay_layer = NULL;
	st.overlay = NULL;
	st.overlay_configured = false;
}

static void open_menu(int index) {
	if (index == st.open) {
		return;
	}
	tooltip_hide();
	st.open = index;
	draw_bar();

	if (st.overlay == NULL) {
		overlay_create(false);
	} else if (st.overlay_configured) {
		struct menu *m = st.bar_menus[index];
		open_level(0, m, m->x, BAR_H - 1);
	}
}

static void close_menu(void) {
	if (st.open < 0) {
		return;
	}
	close_levels(0);
	overlay_destroy();
	st.open = -1;
	if (st.rebuild_pending) {
		st.rebuild_pending = false;
		rebuild_bar();
	} else {
		draw_bar();
	}
}

static void run(const char *command) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", command, (void *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

/* ---- Alert box ---------------------------------------------------------- */

/* GEM's form_alert: an icon, the question, and buttons, the default one
 * (Cancel, the safe choice) with a thick border. */
static void show_alert(void) {
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = begin(scratch);
	char *question = strdup(st.alert.text);
	char *lines[8];
	int n = 0;
	for (char *save, *line = strtok_r(question, "|", &save); line && n < 8;
			line = strtok_r(NULL, "|", &save)) {
		lines[n++] = line;
	}
	const int pad = 16, icon = 32, line_h = ITEM_H, button_h = 22;
	int text_w = 0;
	for (int i = 0; i < n; i++) {
		cairo_text_extents_t te;
		cairo_text_extents(cr, lines[i], &te);
		text_w = (int)te.x_advance > text_w ? (int)te.x_advance : text_w;
	}
	const char *labels[2] = { st.alert.action, "Cancel" };
	int buttons_w = 0;
	for (int i = 0; i < 2; i++) {
		cairo_text_extents_t te;
		cairo_text_extents(cr, labels[i], &te);
		st.alert.button_w[i] = (int)te.x_advance + 2 * ITEM_PAD;
		buttons_w += st.alert.button_w[i] + (i ? 12 : 0);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);

	int body_w = icon + pad + text_w;
	int w = 2 * pad + (body_w > buttons_w ? body_w : buttons_w);
	int text_h = n * line_h > icon ? n * line_h : icon;
	int h = 2 * pad + text_h + pad + button_h;
	st.alert.w = w + 2; /* room for the drop shadow */
	st.alert.h = h + 2;
	st.alert.x = (st.overlay_w - w) / 2;
	st.alert.y = (st.overlay_h - h) / 3;
	st.alert.button_h = button_h;
	st.alert.button_y = h - pad - button_h;
	st.alert.button_x[1] = w - pad - st.alert.button_w[1];
	st.alert.button_x[0] = st.alert.button_x[1] - 12 - st.alert.button_w[0];

	struct buffer *buffer = buffer_create(st.alert.w, st.alert.h);
	if (buffer == NULL) {
		free(question);
		return;
	}
	memset(buffer->data, 0, buffer->size);
	cr = begin(buffer->surface);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 2, 2, w, h); /* shadow */
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
	cairo_rectangle(cr, 2.5, 2.5, w - 5, h - 5); /* GEM's double outline */
	cairo_stroke(cr);

	/* The "?" icon: a black box with the mark cut out of it. */
	cairo_rectangle(cr, pad, pad, icon, icon);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_font_size(cr, 26);
	cairo_text_extents_t qe;
	cairo_text_extents(cr, "?", &qe);
	cairo_move_to(cr, pad + (icon - qe.width) / 2 - qe.x_bearing,
		pad + (icon - qe.height) / 2 - qe.y_bearing);
	cairo_show_text(cr, "?");
	cairo_set_font_size(cr, FONT_SIZE);

	cairo_set_source_rgb(cr, 0, 0, 0);
	for (int i = 0; i < n; i++) {
		text(cr, lines[i], pad + icon + pad, pad + i * line_h, line_h);
	}
	for (int i = 0; i < 2; i++) {
		int bx = st.alert.button_x[i], by = st.alert.button_y;
		int bw = st.alert.button_w[i];
		cairo_rectangle(cr, bx + 0.5, by + 0.5, bw - 1, button_h - 1);
		cairo_stroke(cr);
		if (i == 1) { /* the default button's thick border */
			cairo_rectangle(cr, bx - 1.5, by - 1.5, bw + 3, button_h + 3);
			cairo_rectangle(cr, bx - 0.5, by - 0.5, bw + 1, button_h + 1);
			cairo_stroke(cr);
		}
		cairo_text_extents_t te;
		cairo_text_extents(cr, labels[i], &te);
		text(cr, labels[i], bx + (bw - (int)te.x_advance) / 2, by, button_h);
	}
	cairo_destroy(cr);
	free(question);

	st.alert.surface = wl_compositor_create_surface(st.compositor);
	st.alert.subsurface = wl_subcompositor_get_subsurface(st.subcompositor,
		st.alert.surface, st.overlay);
	wl_subsurface_set_position(st.alert.subsurface, st.alert.x, st.alert.y);
	submit(st.alert.surface, buffer, st.alert.w, st.alert.h);
	wl_surface_commit(st.overlay);
}

/* Asks the item's question; the command runs if the user agrees. */
static void ask(const struct item *item) {
	close_menu();
	st.alert.active = true;
	st.alert.text = strdup(item->confirm);
	st.alert.command = strdup(item->command);
	/* The action button reads like the item: "Shutdown..." -> "Shutdown". */
	st.alert.action = strdup(item->label);
	size_t len = strlen(st.alert.action);
	while (len > 0 && st.alert.action[len - 1] == '.') {
		st.alert.action[--len] = '\0';
	}
	overlay_create(true);
}

static void dismiss_alert(void) {
	if (!st.alert.active) {
		return;
	}
	if (st.alert.subsurface != NULL) {
		wl_subsurface_destroy(st.alert.subsurface);
		wl_surface_destroy(st.alert.surface);
	}
	if (st.pointer_surface == st.alert.surface) {
		st.pointer_surface = NULL;
	}
	free(st.alert.text);
	free(st.alert.command);
	free(st.alert.action);
	memset(&st.alert, 0, sizeof(st.alert));
	overlay_destroy();
}

/* A click while the alert is up: its buttons, or nothing (it's modal). */
static void alert_press(double x, double y) {
	double lx = x - st.alert.x, ly = y - st.alert.y;
	for (int i = 0; i < 2; i++) {
		if (lx >= st.alert.button_x[i] &&
				lx < st.alert.button_x[i] + st.alert.button_w[i] &&
				ly >= st.alert.button_y &&
				ly < st.alert.button_y + st.alert.button_h) {
			char *command = i == 0 ? strdup(st.alert.command) : NULL;
			dismiss_alert();
			if (command != NULL) {
				run(command);
				free(command);
			}
			return;
		}
	}
}

/* ---- Pointer ------------------------------------------------------------ */

/* Pointer position in screen coordinates. The bar and the overlay both sit
 * at the screen's top-left; drop-downs are offset within the overlay. */
static void pointer_pos(double *x, double *y) {
	*x = st.px;
	*y = st.py;
	if (st.alert.surface != NULL && st.pointer_surface == st.alert.surface) {
		*x += st.alert.x;
		*y += st.alert.y;
	}
	for (int l = 0; l < st.depth; l++) {
		if (st.pointer_surface == st.levels[l].surface) {
			*x += st.levels[l].x;
			*y += st.levels[l].y;
		}
	}
}

static void handle_motion(void) {
	if (st.pointer_surface == NULL || st.alert.active) {
		return; /* menus stay shut while an alert is up */
	}
	double x, y;
	pointer_pos(&x, &y);
	tooltip_track(x, y);
	int l = level_at(x, y);
	if (y < BAR_H && l < 0) {
		/* GEM menus drop down on hover, no click needed. */
		int t = title_at(x);
		if (t != st.quiet_title) {
			st.quiet_title = -1;
		}
		if (t >= 0 && t != st.open && t != st.quiet_title) {
			open_menu(t);
		} else if (st.depth > 0) {
			close_levels(1);
			set_hover(0, -1);
		}
		return;
	}
	if (l < 0) {
		/* Off the menus: drop the highlight in the innermost one, keeping
		 * the path to it. */
		if (st.depth > 0) {
			set_hover(st.depth - 1, -1);
		}
		return;
	}

	int i = item_at(l, x, y);
	struct item *item = i >= 0 ? &st.levels[l].menu->items[i] : NULL;
	if (item != NULL && item->submenu != NULL) {
		bool already_open = st.depth > l + 1 &&
			st.levels[l + 1].menu == item->submenu;
		if (already_open) {
			close_levels(l + 2);
		} else {
			set_hover(l, i);
			open_submenu(l, i);
		}
	} else {
		close_levels(l + 1);
	}
	set_hover(l, i);
}

static void handle_press(void) {
	tooltip_hide();
	double x, y;
	pointer_pos(&x, &y);
	if (st.alert.active) {
		alert_press(x, y);
		return;
	}
	int l = level_at(x, y);
	int ws = l < 0 && y < BAR_H ? workspace_at(x) : -1;
	if (ws >= 0) {
		/* Switching also closes any open menu. */
		close_menu();
		char command[32];
		if (ws == st.ws_count) {
			snprintf(command, sizeof(command), "workspace new");
		} else {
			snprintf(command, sizeof(command), "workspace %d", ws + 1);
		}
		ipc_send(command);
		return;
	}
	int title = l < 0 && y < BAR_H ? title_at(x) : -1;
	if (title >= 0) {
		/* A title click toggles its menu. Closed by a click, it stays
		 * closed while the pointer is still over it. */
		if (title == st.open) {
			close_menu();
			st.quiet_title = title;
		} else {
			st.quiet_title = -1;
			open_menu(title);
		}
		return;
	}
	struct menu_app *app = l < 0 && y < BAR_H ? app_at(x) : NULL;
	if (app != NULL) {
		close_menu();
		app_click(app, st.button);
		return;
	}
	if (st.open < 0) {
		return;
	}
	if (l >= 0) {
		int i = item_at(l, x, y);
		struct item *item = i >= 0 ? &st.levels[l].menu->items[i] : NULL;
		if (item == NULL || item->submenu != NULL) {
			return; /* disabled items, separators and submenus stay open */
		}
		if (item->confirm != NULL) {
			ask(item);
			return;
		}
		if (item->ipc) {
			ipc_send(item->command);
		} else {
			run(item->command);
		}
	}
	close_menu();
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	st.pointer_surface = surface;
	st.px = wl_fixed_to_double(x);
	st.py = wl_fixed_to_double(y);
	handle_motion();
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	if (st.pointer_surface == surface) {
		st.pointer_surface = NULL;
	}
	if (surface == st.bar) {
		tooltip_hide();
	}
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	st.px = wl_fixed_to_double(x);
	st.py = wl_fixed_to_double(y);
	handle_motion();
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		st.button = button;
		handle_press();
	}
}

/* Scrolling over a menu app tells it, a notch at a time (a touchpad's
 * smaller steps add up). */
static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || st.open >= 0 ||
			st.pointer_surface != st.bar) {
		return;
	}
	struct menu_app *app = app_at(st.px);
	if (app == NULL || app->fd < 0) {
		st.scrolled = 0;
		return;
	}
	st.scrolled += wl_fixed_to_double(value);
	while (st.scrolled <= -SCROLL_STEP || st.scrolled >= SCROLL_STEP) {
		bool up = st.scrolled < 0;
		st.scrolled += up ? SCROLL_STEP : -SCROLL_STEP;
		static const char up_msg[] = "scroll up\n", down_msg[] = "scroll down\n";
		send(app->fd, up ? up_msg : down_msg,
			(up ? sizeof(up_msg) : sizeof(down_msg)) - 1,
			MSG_NOSIGNAL | MSG_DONTWAIT);
	}
}
static void pointer_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_axis_source(void *data, struct wl_pointer *pointer,
		uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis) {
	st.scrolled = 0;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	close(fd); /* we only look at raw key codes */
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface) {}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
		int32_t rate, int32_t delay) {}

/* Escape, Return and Enter all pick Cancel, the alert's default. */
static void keyboard_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	if (st.alert.active && state == WL_KEYBOARD_KEY_STATE_PRESSED &&
			(key == KEY_ESC || key == KEY_ENTER || key == KEY_KPENTER)) {
		dismiss_alert();
	}
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (has_keyboard && st.keyboard == NULL) {
		st.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(st.keyboard, &keyboard_listener, NULL);
	} else if (!has_keyboard && st.keyboard != NULL) {
		wl_keyboard_release(st.keyboard);
		st.keyboard = NULL;
	}
	bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && st.pointer == NULL) {
		st.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(st.pointer, &pointer_listener, NULL);
	} else if (!has_pointer && st.pointer != NULL) {
		wl_pointer_release(st.pointer);
		st.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* wl_display_dispatch() plus the control socket and the menu apps. */
static int dispatch(void) {
	while (wl_display_prepare_read(st.display) != 0) {
		wl_display_dispatch_pending(st.display);
	}
	if (wl_display_flush(st.display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(st.display);
		return -1;
	}
	/* poll() skips negative fds, so absent ones just stay -1. */
	struct pollfd fds[2 + MAX_APPS] = {
		{ .fd = wl_display_get_fd(st.display), .events = POLLIN },
		{ .fd = st.ipc_fd, .events = POLLIN },
	};
	/* Wake up for menu apps waiting to be restarted. */
	time_t now = time(NULL), wake = 0;
	for (int i = 0; i < st.n_apps; i++) {
		fds[2 + i] = (struct pollfd){ .fd = st.apps[i].fd, .events = POLLIN };
		time_t at = st.apps[i].restart_at;
		if (at != 0 && (wake == 0 || at < wake)) {
			wake = at;
		}
	}
	int timeout = wake == 0 ? -1 : wake <= now ? 0 : (int)(wake - now) * 1000;
	if (st.tip_due != 0) {
		int64_t in = st.tip_due - now_ms();
		int tip = in < 0 ? 0 : (int)in;
		timeout = timeout < 0 || tip < timeout ? tip : timeout;
	}
	if (poll(fds, 2 + st.n_apps, timeout) < 0) {
		wl_display_cancel_read(st.display);
		return errno == EINTR ? 0 : -1;
	}
	if (fds[0].revents & POLLIN) {
		if (wl_display_read_events(st.display) < 0) {
			return -1;
		}
	} else {
		wl_display_cancel_read(st.display);
	}
	if (fds[0].revents & (POLLERR | POLLHUP)) {
		return -1;
	}
	if (st.ipc_fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
		ipc_read();
	}
	for (int i = 0; i < st.n_apps; i++) {
		struct menu_app *a = &st.apps[i];
		if (a->fd >= 0 && (fds[2 + i].revents & (POLLIN | POLLHUP | POLLERR))) {
			app_read(a);
		}
		if (a->restart_at != 0 && a->restart_at <= time(NULL)) {
			app_start(a);
		}
	}
	if (st.tip_due != 0 && now_ms() >= st.tip_due) {
		tooltip_show();
	}
	while (waitpid(-1, NULL, WNOHANG) > 0) {
		/* reap menu apps that have exited */
	}
	return wl_display_dispatch_pending(st.display);
}

/* ---- Setup -------------------------------------------------------------- */

static void bar_configure(void *data, struct zwlr_layer_surface_v1 *layer,
		uint32_t serial, uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	st.bar_w = (int)w;
	draw_bar();
}

static void bar_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	st.running = false;
}

static const struct zwlr_layer_surface_v1_listener bar_listener = {
	.configure = bar_configure,
	.closed = bar_closed,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		st.compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		st.subcompositor = wl_registry_bind(registry, name,
			&wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		st.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 &&
			st.seat == NULL) {
		st.seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(st.seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		st.layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		st.viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	st.open = -1;
	st.font = getenv("GEMWM_FONT") ? getenv("GEMWM_FONT") : "monospace";
	st.quiet_title = -1;
	load_config();

	st.display = wl_display_connect(NULL);
	if (st.display == NULL) {
		fprintf(stderr, "gemwm-menu: can't connect to a Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(st.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(st.display);
	if (st.compositor == NULL || st.subcompositor == NULL ||
			st.shm == NULL || st.layer_shell == NULL) {
		fprintf(stderr, "gemwm-menu: compositor lacks wlr-layer-shell\n");
		return 1;
	}
	rebuild_bar();

	st.bar = wl_compositor_create_surface(st.compositor);
	st.bar_layer = zwlr_layer_shell_v1_get_layer_surface(st.layer_shell,
		st.bar, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "gemwm-menu");
	zwlr_layer_surface_v1_set_anchor(st.bar_layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(st.bar_layer, 0, BAR_H);
	zwlr_layer_surface_v1_set_exclusive_zone(st.bar_layer, BAR_H);
	zwlr_layer_surface_v1_add_listener(st.bar_layer, &bar_listener, NULL);
	wl_surface_commit(st.bar);

	ipc_connect();

	for (int i = 0; i < st.n_apps; i++) {
		app_start(&st.apps[i]);
	}

	st.running = true;
	while (st.running && dispatch() != -1) {
	}
	for (int i = 0; i < st.n_apps; i++) {
		if (st.apps[i].fd >= 0) {
			kill(-st.apps[i].pid, SIGTERM);
		}
	}
	wl_display_disconnect(st.display);
	return 0;
}
