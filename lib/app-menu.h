/*
 * A GTK window's own menus in GemWM's menu bar (gemwm-app-menu-v1), for
 * GemWM's own applications. Outside GemWM it does nothing.
 *
 * The application describes its menus in a build callback, with
 * app_menu_add_*; call app_menu_update whenever what they'd show changes
 * (an item enabled, a check mark), and they're sent again if they differ.
 * The object goes away with its window.
 */
#ifndef GEMWM_APP_MENU_H
#define GEMWM_APP_MENU_H

#include <gtk/gtk.h>
#include <stdint.h>

enum {
	APP_MENU_DISABLED = 1,
	APP_MENU_CHECKED = 2,
};

struct app_menu;

typedef void (*app_menu_build_fn)(struct app_menu *menu, void *data);
typedef void (*app_menu_activate_fn)(uint32_t id, void *data);

struct app_menu *app_menu_new(GtkWidget *window, app_menu_build_fn build,
	app_menu_activate_fn activate, void *data);
void app_menu_update(struct app_menu *menu);

/* For the build callback. */
void app_menu_add_menu(struct app_menu *menu, const char *title);
void app_menu_add_item(struct app_menu *menu, uint32_t id, const char *label,
	const char *shortcut, uint32_t flags);
void app_menu_add_separator(struct app_menu *menu);
/* A submenu, opening to the side: what's added after it goes in it, until
 * app_menu_end_submenu. They may nest. */
void app_menu_add_submenu(struct app_menu *menu, const char *label,
	uint32_t flags);
void app_menu_end_submenu(struct app_menu *menu);

#endif
