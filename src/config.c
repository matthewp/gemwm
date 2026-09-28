/*
 * ~/.config/gemwm/config: key bindings, the focus highlight, the layout,
 * and the desktop background.
 *
 * The built-in defaults below are loaded first; the user's file then
 * changes them one key at a time ("Key = none" removes a binding). A
 * binding's action is a command in the control socket's language (see
 * ipc.c), so a key can do anything `gemwm msg` can.
 */
#define _GNU_SOURCE /* asprintf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "server.h"

static const char default_config[] =
	"[keys]\n"
	"Super+Q = close-window focused\n"
	"Super+Z = maximize focused\n"
	"Super+Tab = cycle-windows next\n"
	"Super+Shift+Tab = cycle-windows prev\n"
	"Super+T = exec ${TERMINAL:-gemwm-terminal}\n"
	"Super+B = exec gtk-launch \"$(xdg-settings get default-web-browser)\"\n"
	"Print = exec gemwm-screenshot screen\n"
	"Shift+Print = exec gemwm-screenshot area\n"
	"Alt+Print = exec gemwm-screenshot window\n"
	"XF86AudioRaiseVolume = exec gemwm-volume up\n"
	"XF86AudioLowerVolume = exec gemwm-volume down\n"
	"XF86AudioMute = exec gemwm-volume mute\n"
	"XF86AudioMicMute = exec gemwm-volume mic-mute\n"
	"Super+1 = workspace 1\n"
	"Super+2 = workspace 2\n"
	"Super+3 = workspace 3\n"
	"Super+4 = workspace 4\n"
	"Super+5 = workspace 5\n"
	"Super+6 = workspace 6\n"
	"Super+7 = workspace 7\n"
	"Super+8 = workspace 8\n"
	"Super+9 = workspace 9\n"
	"Super+Shift+1 = move-window focused 1\n"
	"Super+Shift+2 = move-window focused 2\n"
	"Super+Shift+3 = move-window focused 3\n"
	"Super+Shift+4 = move-window focused 4\n"
	"Super+Shift+5 = move-window focused 5\n"
	"Super+Shift+6 = move-window focused 6\n"
	"Super+Shift+7 = move-window focused 7\n"
	"Super+Shift+8 = move-window focused 8\n"
	"Super+Shift+9 = move-window focused 9\n"
	"Alt+Return = exec ${TERMINAL:-gemwm-terminal}\n"
	"Alt+Tab = cycle-windows next\n"
	"Alt+Shift+Tab = cycle-windows prev\n"
	"Super+Left = focus-direction left\n"
	"Super+Right = focus-direction right\n"
	"Super+Up = focus-direction up\n"
	"Super+Down = focus-direction down\n"
	"Super+Ctrl+Left = move-direction left\n"
	"Super+Ctrl+Right = move-direction right\n"
	"Super+Ctrl+Up = move-direction up\n"
	"Super+Ctrl+Down = move-direction down\n"
	"Super+Shift+Left = push-direction left\n"
	"Super+Shift+Right = push-direction right\n"
	"Super+bracketleft = consume-or-expel left\n"
	"Super+bracketright = consume-or-expel right\n"
	"Super+R = column-width cycle\n"
	"Alt+Escape = quit\n"
	"Alt+1 = workspace 1\n"
	"Alt+2 = workspace 2\n"
	"Alt+3 = workspace 3\n"
	"Alt+4 = workspace 4\n"
	"Alt+5 = workspace 5\n"
	"Alt+6 = workspace 6\n"
	"Alt+7 = workspace 7\n"
	"Alt+8 = workspace 8\n"
	"Alt+9 = workspace 9\n"
	"Alt+Shift+1 = move-window focused 1\n"
	"Alt+Shift+2 = move-window focused 2\n"
	"Alt+Shift+3 = move-window focused 3\n"
	"Alt+Shift+4 = move-window focused 4\n"
	"Alt+Shift+5 = move-window focused 5\n"
	"Alt+Shift+6 = move-window focused 6\n"
	"Alt+Shift+7 = move-window focused 7\n"
	"Alt+Shift+8 = move-window focused 8\n"
	"Alt+Shift+9 = move-window focused 9\n"
	"\n"
	"[highlight]\n"
	"color = #ff3fa4\n"
	"width = 4\n"
	"tiling = always\n"
	"dim = tiling\n"
	"dim-opacity = 0.5\n"
	"\n"
	"[layout]\n"
	"mode = window\n"
	"gap = 8\n"
	"column-width = 0.5\n"
	"\n"
	"[desktop]\n"
	"color = green\n"
	"image =\n"
	"image-mode = fill\n"
	"dither = off\n"
	"dither-style = diffuse\n"
	"pixel-size = 2\n"
	"resolution = off\n"
	"pattern = none\n";

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

/* "Super+Shift+Tab" -> modifiers and a lower-case keysym. */
static bool parse_combo(char *combo, uint32_t *mods, xkb_keysym_t *sym) {
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;
	char *save = NULL;
	for (char *part = strtok_r(combo, "+", &save); part != NULL;
			part = strtok_r(NULL, "+", &save)) {
		part = trim(part);
		if (*sym != XKB_KEY_NoSymbol) {
			return false; /* the key has to come last */
		}
		if (strcasecmp(part, "Super") == 0 || strcasecmp(part, "Logo") == 0 ||
				strcasecmp(part, "Mod4") == 0) {
			*mods |= WLR_MODIFIER_LOGO;
		} else if (strcasecmp(part, "Alt") == 0 ||
				strcasecmp(part, "Mod1") == 0) {
			*mods |= WLR_MODIFIER_ALT;
		} else if (strcasecmp(part, "Ctrl") == 0 ||
				strcasecmp(part, "Control") == 0) {
			*mods |= WLR_MODIFIER_CTRL;
		} else if (strcasecmp(part, "Shift") == 0) {
			*mods |= WLR_MODIFIER_SHIFT;
		} else {
			*sym = xkb_keysym_to_lower(
				xkb_keysym_from_name(part, XKB_KEYSYM_CASE_INSENSITIVE));
			if (*sym == XKB_KEY_NoSymbol) {
				return false;
			}
		}
	}
	return *sym != XKB_KEY_NoSymbol;
}

static void set_binding(struct server *server, uint32_t mods,
		xkb_keysym_t sym, const char *action) {
	for (int i = 0; i < server->n_bindings; i++) {
		struct binding *b = &server->bindings[i];
		if (b->mods == mods && b->sym == sym) {
			free(b->action);
			if (strcmp(action, "none") == 0) {
				*b = server->bindings[--server->n_bindings];
			} else {
				b->action = strdup(action);
			}
			return;
		}
	}
	if (strcmp(action, "none") == 0) {
		return;
	}
	server->bindings = realloc(server->bindings,
		(server->n_bindings + 1) * sizeof(*server->bindings));
	server->bindings[server->n_bindings++] = (struct binding){
		.mods = mods, .sym = sym, .action = strdup(action),
	};
}

static bool parse_color(const char *s, float color[4]) {
	if (s[0] == '#') {
		s++;
	}
	char *end;
	unsigned long rgb = strtoul(s, &end, 16);
	if (*end != '\0' || strlen(s) != 6) {
		return false;
	}
	color[0] = ((rgb >> 16) & 0xff) / 255.0f;
	color[1] = ((rgb >> 8) & 0xff) / 255.0f;
	color[2] = (rgb & 0xff) / 255.0f;
	color[3] = 1.0f;
	return true;
}

static void parse(struct server *server, FILE *f, const char *name) {
	char *line = NULL;
	size_t cap = 0;
	char section[32] = "";
	int lineno = 0;
	while (getline(&line, &cap, f) != -1) {
		lineno++;
		char *s = trim(line);
		if (s[0] == '\0' || s[0] == '#') {
			continue;
		}
		if (s[0] == '[') {
			char *end = strchr(s, ']');
			if (end != NULL) {
				*end = '\0';
			}
			snprintf(section, sizeof(section), "%s", trim(s + 1));
			continue;
		}
		char *eq = strchr(s, '=');
		if (eq == NULL) {
			wlr_log(WLR_ERROR, "%s:%d: expected \"name = value\"", name, lineno);
			continue;
		}
		*eq = '\0';
		/* A # after whitespace starts a comment; a value may itself begin
		 * with # (a colour). */
		char *start = eq + 1;
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		for (char *c = start; *c != '\0'; c++) {
			if (*c == '#' && c > start && (c[-1] == ' ' || c[-1] == '\t')) {
				*c = '\0';
				break;
			}
		}
		char *key = trim(s), *value = trim(eq + 1);

		if (strcmp(section, "keys") == 0) {
			uint32_t mods;
			xkb_keysym_t sym;
			if (!parse_combo(key, &mods, &sym)) {
				wlr_log(WLR_ERROR, "%s:%d: unknown key", name, lineno);
				continue;
			}
			set_binding(server, mods, sym, value);
		} else if (strcmp(section, "highlight") == 0 &&
				strcmp(key, "color") == 0) {
			if (!parse_color(value, server->highlight_color)) {
				wlr_log(WLR_ERROR, "%s:%d: colours look like #ff3fa4",
					name, lineno);
			}
		} else if (strcmp(section, "highlight") == 0 &&
				strcmp(key, "width") == 0) {
			int width = atoi(value);
			server->highlight_width = width < 0 ? 0 : width > 64 ? 64 : width;
		} else if (strcmp(section, "highlight") == 0 &&
				strcmp(key, "tiling") == 0) {
			/* "always": the focused tile keeps its border; "super": only
			 * while Super is held, as in window mode. */
			server->highlight_tiling = strcmp(value, "super") != 0;
		} else if (strcmp(section, "highlight") == 0 &&
				strcmp(key, "dim") == 0) {
			server->dim = strcmp(value, "always") == 0 ? DIM_ALWAYS :
				strcmp(value, "off") == 0 ? DIM_OFF : DIM_TILING;
		} else if (strcmp(section, "highlight") == 0 &&
				strcmp(key, "dim-opacity") == 0) {
			float o = strtof(value, NULL);
			server->dim_opacity = o < 0 ? 0 : o > 1 ? 1 : o;
		} else if (strcmp(section, "layout") == 0 &&
				strcmp(key, "gap") == 0) {
			int gap = atoi(value);
			server->gap = gap < 0 ? 0 : gap > 200 ? 200 : gap;
		} else if (strcmp(section, "layout") == 0 &&
				strcmp(key, "mode") == 0) {
			/* Only the mode GemWM starts in: later reloads leave the
			 * one picked from the menu alone. */
			if (!server->mode_configured) {
				server->mode = strcmp(value, "tiling") == 0 ? MODE_TILING :
					strcmp(value, "scrolling") == 0 ? MODE_SCROLLING :
					MODE_WINDOW;
			}
		} else if (strcmp(section, "layout") == 0 &&
				strcmp(key, "column-width") == 0) {
			double w = strtod(value, NULL);
			server->column_width = w < 0.1 ? 0.1 : w > 1 ? 1 : w;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "color") == 0) {
			if (!desktop_parse_color(value, &server->desktop_color,
					&server->desktop_mono)) {
				wlr_log(WLR_ERROR, "%s:%d: colours are a name (green, blue, "
					"grey...), mono, or like #00ff00", name, lineno);
			}
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "image") == 0) {
			free(server->desktop_image);
			server->desktop_image = NULL;
			const char *home = getenv("HOME");
			if (value[0] == '~' && value[1] == '/' && home != NULL) {
				if (asprintf(&server->desktop_image, "%s%s", home, value + 1) < 0) {
					server->desktop_image = NULL;
				}
			} else if (value[0] != '\0') {
				server->desktop_image = strdup(value);
			}
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "image-mode") == 0) {
			server->desktop_image_mode =
				strcmp(value, "fit") == 0 ? IMAGE_FIT :
				strcmp(value, "center") == 0 ? IMAGE_CENTER :
				strcmp(value, "tile") == 0 ? IMAGE_TILE :
				strcmp(value, "stretch") == 0 ? IMAGE_STRETCH : IMAGE_FILL;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "dither") == 0) {
			server->desktop_palette = strcmp(value, "st") == 0 ? DITHER_ST :
				strcmp(value, "st16") == 0 ? DITHER_ST16 :
				strcmp(value, "atari16") == 0 ? DITHER_ATARI16 : DITHER_OFF;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "dither-style") == 0) {
			server->desktop_dither_style = strcmp(value, "ordered") == 0 ?
				DITHER_ORDERED : DITHER_DIFFUSE;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "pixel-size") == 0) {
			int size = atoi(value);
			server->desktop_pixel_size = size < 1 ? 1 : size > 16 ? 16 : size;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "resolution") == 0) {
			/* "640x400": big pixels sized for that many across and down. */
			int rw = 0, rh = 0;
			if (sscanf(value, "%dx%d", &rw, &rh) != 2 || rw < 16 || rh < 16) {
				rw = rh = 0;
			}
			server->desktop_res_w = rw;
			server->desktop_res_h = rh;
		} else if (strcmp(section, "desktop") == 0 &&
				strcmp(key, "pattern") == 0) {
			static const char *const names[] = { "none", "dots", "lines",
				"vertical-lines", "crosshatch", "diagonal", "checkerboard",
				"bricks" };
			server->desktop_pattern = PATTERN_NONE;
			for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
				if (strcmp(value, names[i]) == 0) {
					server->desktop_pattern = (enum desktop_pattern)i;
				}
			}
		} else if (strcmp(section, "font") == 0 && strcmp(key, "family") == 0) {
			free(server->font_family);
			server->font_family = value[0] != '\0' ? strdup(value) : NULL;
		} else if (strcmp(section, "font") == 0 && strcmp(key, "size") == 0) {
			int size = atoi(value);
			server->font_size = size < 6 ? 0 : size > 48 ? 48 : size;
		} else if (strcmp(section, "clock") != 0 &&
				strcmp(section, "battery") != 0) { /* the menu bar's */
			wlr_log(WLR_ERROR, "%s:%d: unknown setting", name, lineno);
		}
	}
	free(line);
}

static void font_configure(struct server *server);

void config_load(struct server *server) {
	config_finish(server);

	FILE *f = fmemopen((void *)default_config, strlen(default_config), "r");
	parse(server, f, "defaults");
	fclose(f);

	char path[4096];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg != NULL && xdg[0] != '\0') {
		snprintf(path, sizeof(path), "%s/gemwm/config", xdg);
	} else {
		snprintf(path, sizeof(path), "%s/.config/gemwm/config",
			home ? home : "");
	}
	if ((f = fopen(path, "r")) != NULL) {
		parse(server, f, path);
		fclose(f);
	}
	server->mode_configured = true;
	highlight_update(server);
	tile_arrange(server); /* the gap may have changed */
	desktop_configure(server);
	font_configure(server);
}

/* The ST's own system font, installed with GemWM (extras/fonts). It's
 * drawn in whole pixels at 16. */
#define ST_FONT "Atari ST 8x16"

/* [font] in the config, else GEMWM_FONT from the session's environment,
 * else the ST font; the size likewise, else 16 for the ST font and 14 for
 * others. It goes to everything we start through the environment, and
 * the window titles are drawn again. Running programs keep theirs. */
static void font_configure(struct server *server) {
	static char *env_family, *env_size;
	static bool saved;
	if (!saved) {
		saved = true;
		env_family = getenv("GEMWM_FONT") ? strdup(getenv("GEMWM_FONT")) : NULL;
		env_size = getenv("GEMWM_FONT_SIZE") ? strdup(getenv("GEMWM_FONT_SIZE")) :
			NULL;
	}
	const char *family = server->font_family ? server->font_family :
		env_family ? env_family : ST_FONT;
	int size = server->font_size ? server->font_size :
		env_size ? atoi(env_size) : strcmp(family, ST_FONT) == 0 ? 16 : 14;
	char size_text[16];
	snprintf(size_text, sizeof(size_text), "%d", size > 0 ? size : 14);
	setenv("GEMWM_FONT", family, 1);
	setenv("GEMWM_FONT_SIZE", size_text, 1);

	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		free(view->drawn_title); /* redraw, in the new font */
		view->drawn_title = NULL;
		view_update_frame(view);
	}
}

void config_finish(struct server *server) {
	for (int i = 0; i < server->n_bindings; i++) {
		free(server->bindings[i].action);
	}
	free(server->bindings);
	server->bindings = NULL;
	free(server->desktop_image);
	server->desktop_image = NULL;
	free(server->font_family);
	server->font_family = NULL;
	server->font_size = 0;
	server->n_bindings = 0;
}

/* Runs the binding for a key press, if there is one. Keys are matched by
 * their unshifted symbol, so Super+Shift+Tab is Tab (not ISO_Left_Tab) and
 * Alt+Shift+2 is 2 (not @); the shifted symbol is tried too, for bindings
 * like Super+question. */
bool bindings_handle(struct server *server, uint32_t mods,
		const xkb_keysym_t *base, int nbase,
		const xkb_keysym_t *syms, int nsyms) {
	mods &= WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL |
		WLR_MODIFIER_SHIFT;
	for (int i = 0; i < server->n_bindings; i++) {
		struct binding *b = &server->bindings[i];
		if (b->mods != mods) {
			continue;
		}
		bool match = false;
		for (int j = 0; j < nbase && !match; j++) {
			match = xkb_keysym_to_lower(base[j]) == b->sym;
		}
		for (int j = 0; j < nsyms && !match; j++) {
			match = xkb_keysym_to_lower(syms[j]) == b->sym;
		}
		if (match) {
			ipc_run_command(server, b->action);
			return true;
		}
	}
	return false;
}
