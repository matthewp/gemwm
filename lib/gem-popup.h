/*
 * A GEM pop-up menu at a point in a widget, drawn like the drop-down menus
 * of GemWM's menu bar: rows of items with their shortcuts, the one under
 * the pointer inverted, disabled ones grey, check marks, dotted
 * separators. For right-click menus and pop-up choices.
 *
 * Fill it (gem_popup_clear, then add items), then show it; chosen gets the
 * item's id once one is picked, or -1 if it's dismissed.
 */
#ifndef GEMWM_GEM_POPUP_H
#define GEMWM_GEM_POPUP_H

#include <gtk/gtk.h>
#include <stdint.h>

enum {
	GEM_POPUP_DISABLED = 1,
	GEM_POPUP_CHECKED = 2,
};

struct gem_popup;
typedef void (*gem_popup_fn)(int id, void *data);

/* Lives as long as parent. */
struct gem_popup *gem_popup_new(GtkWidget *parent, gem_popup_fn chosen,
	void *data);
void gem_popup_clear(struct gem_popup *p);
void gem_popup_add(struct gem_popup *p, int id, const char *label,
	const char *shortcut, uint32_t flags);
void gem_popup_add_separator(struct gem_popup *p);
/* At x, y in the parent's coordinates, below and to the right. */
void gem_popup_show(struct gem_popup *p, double x, double y);
bool gem_popup_shown(struct gem_popup *p);

#endif
