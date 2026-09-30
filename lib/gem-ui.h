/*
 * What GemWM's own GTK applications build their GEM windows from: the
 * sizes of things, pixel-drawn areas that know where their buttons are,
 * GEM buttons and dialog frames, and CSS that draws GTK's own text fields
 * and text views in black and white. The widgets in gem-alert.h,
 * gem-file.h, gem-popup.h and gem-scrollbar.h are made of these.
 */
#ifndef GEMWM_GEM_UI_H
#define GEMWM_GEM_UI_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include "gem-draw.h"

#define GEM_ROW_H 19     /* a line of text, a list's row */
#define GEM_PAD 8
#define GEM_BUTTON_H 22
#define GEM_FIELD_H 24
#define GEM_GADGET 19    /* a scroll bar's width, its arrow boxes */
#define GEM_BORDER 3     /* a dialog's frame: 1 pixel, a gap, then 2 */

/* Once, before any window: GEM's look for GTK's own widgets, by class.
 * "gem" on a window gives it a white background; "gem-field" is a GEM
 * editable text field (on a GtkEntry or GtkText); "gem-text" a text view
 * in the GEM font; "gem-popup" a popover with no decoration of its own,
 * for a pixel area that draws one. */
void gem_ui_load_css(void);

/* A GEM text field, placed at x, y in a GtkOverlay (by its margins) and w
 * wide, however narrow. */
GtkWidget *gem_field_new(int x, int y, int w);

/* Where something clickable was drawn, found again from a click. */
struct gem_hit {
	int x, y, w, h;
	int id;
	int index;
};
void gem_hit_add(GArray *hits, int x, int y, int w, int h, int id, int index);
const struct gem_hit *gem_hit_at(GArray *hits, double x, double y);
GArray *gem_hits_new(void);

/* A drawing area painted pixel by pixel through gem_draw_pixelated, with
 * paint(cr, w, h, data). w or h of 0 leaves that size to the layout. */
typedef void (*gem_paint_fn)(cairo_t *cr, int w, int h, void *data);
GtkWidget *gem_pixel_area_new(int w, int h, gem_paint_fn paint, void *data);

/* A button: its label in a box, the default's with a thicker border. w 0
 * sizes it to the label. Adds a hit for id; returns the width. */
int gem_button_width(const char *label);
int gem_button(cairo_t *cr, GArray *hits, int x, int y, int w,
	const char *label, int id, bool is_default);
/* A check box (square, crossed when on) with its label; adds a hit. */
int gem_checkbox(cairo_t *cr, GArray *hits, int x, int y, const char *label,
	bool on, int id);
/* A dialog's frame around all of w x h, on white. */
void gem_dialog_frame(cairo_t *cr, int w, int h);
/* Text clipped to w. */
void gem_text_clipped(cairo_t *cr, const char *s, int x, int y, int w, int h);
/* GEM's grey: every other pixel of the rectangle black. */
void gem_dither(cairo_t *cr, int x, int y, int w, int h);

/* text broken into lines no wider than width, at spaces (or inside a word
 * too long for a line); its own newlines kept. A NULL-terminated vector
 * for g_strfreev. */
char **gem_wrap(const char *text, int width);

#endif
