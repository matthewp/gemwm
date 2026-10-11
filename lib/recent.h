/*
 * The documents an application opened lately, for File > Open Recent: its
 * own list, newest first, in ~/.local/state/gemwm/<app>-recent, a path a
 * line.
 * Its own so Clear Menu clears only its own.
 */
#ifndef GEM_RECENT_H
#define GEM_RECENT_H

#include <stdint.h>
#include "app-menu.h"

#define RECENT_SHOWN 5 /* in the submenu */

/* path opened (or saved as): to the top. */
void recent_add(const char *app, const char *path);
/* Up to max of them, newest first, only those still there. Read once and
 * kept (menus are built often), so it's fresh as of the last add or
 * clear. */
char **recent_list(const char *app, int max);
void recent_clear(const char *app);

/* The Open Recent submenu, its documents numbered from first_id and Clear
 * Menu as clear_id; greyed out with none. Returns the paths it shows, in
 * order (first_id's first), for the caller to open the one chosen. */
char **recent_submenu(struct app_menu *m, const char *app, uint32_t first_id,
	uint32_t clear_id);

#endif
