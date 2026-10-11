/*
 * Pictures as an ST would show them (dither.c): for GemWM's desktop
 * picture, and GemPlay's album covers.
 */
#ifndef GEMWM_DITHER_H
#define GEMWM_DITHER_H

#include <cairo.h>

enum dither_palette { DITHER_OFF, DITHER_ST, DITHER_ST16, DITHER_ATARI16 };
enum dither_style { DITHER_DIFFUSE, DITHER_ORDERED };

/* The picture in surface (RGB24 or ARGB32), redrawn in big pixels of
 * block device pixels, in the palette's colours, dithered. */
void dither_surface(cairo_surface_t *surface, double block,
	enum dither_palette palette, enum dither_style style);

#endif
