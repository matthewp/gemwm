/*
 * What GemWM's own menu apps (gemwm-clock, gemwm-battery) share: the loop
 * that talks to the menu bar, and their settings. The protocol is described
 * at the top of menu/menu.c.
 */
#ifndef GEMWM_MENU_APP_H
#define GEMWM_MENU_APP_H

#include <stdbool.h>
#include <stddef.h>

enum app_event {
	APP_TIMER, /* timer_fd fired (it has not been read) */
	APP_CLICK, /* the item was clicked; *button is 1, 2 or 3 */
	APP_EXIT,  /* the bar has gone */
};

/* Waits for the bar to send a click, or for timer_fd (-1 for none). */
enum app_event app_wait(int timer_fd, int *button);

/* Sets the item's line, if it changed. "" hides the item. */
void app_show(const char *line);

/* A setting from ~/.config/gemwm/config, e.g. "mode" under [clock], into
 * out; false if it isn't set. */
bool app_config(const char *section, const char *key, char *out, size_t n);

#endif
