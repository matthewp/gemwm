/*
 * Drawing GEM-style windows for GemWM's own GTK applications: black and
 * white, drawn at 1x and pixel-doubled on HiDPI screens like GemWM's frames.
 */
#ifndef GEMWM_GEM_DRAW_H
#define GEMWM_GEM_DRAW_H

#include <cairo.h>
#include <gtk/gtk.h>

void gem_black(cairo_t *cr);
void gem_white(cairo_t *cr);
void gem_fill(cairo_t *cr, double x, double y, double w, double h);
/* A rectangle's outline, t pixels thick, inside x, y, w, h. */
void gem_frame(cairo_t *cr, int x, int y, int w, int h, int t);
double gem_text_width(cairo_t *cr, const char *s);
/* Text vertically centred in a row starting at y. */
void gem_text(cairo_t *cr, const char *s, double x, double y, double row_h);
/* GEM's disabled look: every other pixel knocked out. */
void gem_grey_out(cairo_t *cr, int x, int y, int w, int h);
/* A picture given as rows of '#' (drawn in the current colour) and '.'. */
void gem_bitmap(cairo_t *cr, const char *const rows[], int n_rows, int x, int y);

/* The font GemWM's apps draw in: [font] in GemWM's config, which reaches
 * them as GEMWM_FONT and GEMWM_FONT_SIZE. gem_set_font selects it, drawn
 * without antialiasing, as gem_draw_pixelated does; gem_measure is the
 * width of text in it, for laying out before anything's drawn. */
const char *gem_font_family(void);
int gem_font_size(void);
void gem_set_font(cairo_t *cr);
double gem_measure(const char *s);

/* For a GtkDrawingArea's draw function: paint (called with a white 1x
 * canvas and the font set up) is pixel-doubled onto cr. */
void gem_draw_pixelated(cairo_t *cr, int w, int h,
	void (*paint)(cairo_t *cr, int w, int h, void *data), void *data);

#endif
