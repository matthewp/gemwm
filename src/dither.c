/*
 * The desktop picture as an ST would show it: in big pixels, in the ST's
 * colours, with dithering standing in for the shades in between.
 *
 * The picture is shrunk to one sample per big pixel, reduced to a palette,
 * and blown back up with hard edges. The palettes:
 *
 *   st        any of the ST's 512 colours (three bits a channel)
 *   st16      16 of them, chosen for this picture (median cut), as low-res
 *             ST pictures were
 *   atari16   a fixed, bold 16 (as MPOS, the web desktop, uses)
 *
 * and the dithering: "diffuse" passes each pixel's rounding error on to
 * its neighbours (Floyd-Steinberg), a fine speckle; "ordered" nudges each
 * pixel by a fixed 8x8 pattern (Bayer), the regular crosshatch of 80s
 * computer art. It's done once per desktop change, never per frame.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"

#define ST_LEVELS 8   /* per channel */
#define MAX_COLORS 16 /* st16, atari16 */

static const float atari16[MAX_COLORS][3] = {
	{ 0, 0, 0 }, { 255, 255, 255 }, { 255, 0, 0 }, { 0, 255, 0 },
	{ 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 },
	{ 128, 128, 128 }, { 192, 192, 192 }, { 128, 0, 0 }, { 0, 128, 0 },
	{ 0, 0, 128 }, { 128, 128, 0 }, { 0, 128, 128 }, { 128, 0, 128 },
};

static const float st_step = 255.0f / (ST_LEVELS - 1);

/* The nearest ST level to a channel value. */
static float st_level(float v) {
	float level = roundf(v / st_step);
	level = level < 0 ? 0 : level > ST_LEVELS - 1 ? ST_LEVELS - 1 : level;
	return level * st_step;
}

/* Bayer's 8x8 pattern, 0..63. */
static const unsigned char bayer[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 },
};

struct box {
	float (*px)[3]; /* its pixels, in the sample array */
	int n;
};

static int sort_channel;

static int by_channel(const void *a, const void *b) {
	float d = ((const float *)a)[sort_channel] - ((const float *)b)[sort_channel];
	return (d > 0) - (d < 0);
}

/* The longest side of a box of colours, and which channel it's on. */
static float box_range(const struct box *b, int *channel) {
	float lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
	for (int i = 0; i < b->n; i++) {
		for (int c = 0; c < 3; c++) {
			lo[c] = fminf(lo[c], b->px[i][c]);
			hi[c] = fmaxf(hi[c], b->px[i][c]);
		}
	}
	*channel = 0;
	for (int c = 1; c < 3; c++) {
		if (hi[c] - lo[c] > hi[*channel] - lo[*channel]) {
			*channel = c;
		}
	}
	return hi[*channel] - lo[*channel];
}

/* Median cut: split the widest box of colours in half until there are
 * 16, then take each box's average, as an ST colour. Returns how many
 * different colours that made. */
static int choose_palette(const float (*samples)[3], int n,
		float palette[MAX_COLORS][3]) {
	float (*px)[3] = malloc(sizeof(*px) * n);
	memcpy(px, samples, sizeof(*px) * n);
	struct box boxes[MAX_COLORS] = { { px, n } };
	int n_boxes = 1;
	while (n_boxes < MAX_COLORS) {
		int widest = -1, channel = 0;
		float widest_range = 0;
		for (int i = 0; i < n_boxes; i++) {
			int c;
			float range = boxes[i].n > 1 ? box_range(&boxes[i], &c) : 0;
			if (range > widest_range) {
				widest = i;
				widest_range = range;
				channel = c;
			}
		}
		if (widest < 0) {
			break; /* fewer colours than that in the picture */
		}
		struct box *b = &boxes[widest];
		sort_channel = channel;
		qsort(b->px, b->n, sizeof(*b->px), by_channel);
		int half = b->n / 2;
		boxes[n_boxes++] = (struct box){ b->px + half, b->n - half };
		b->n = half;
	}

	int n_colors = 0;
	for (int i = 0; i < n_boxes; i++) {
		double sum[3] = { 0 };
		for (int j = 0; j < boxes[i].n; j++) {
			for (int c = 0; c < 3; c++) {
				sum[c] += boxes[i].px[j][c];
			}
		}
		float color[3];
		for (int c = 0; c < 3; c++) {
			color[c] = st_level((float)(sum[c] / boxes[i].n));
		}
		bool seen = false;
		for (int k = 0; k < n_colors && !seen; k++) {
			seen = memcmp(palette[k], color, sizeof(color)) == 0;
		}
		if (!seen) {
			memcpy(palette[n_colors++], color, sizeof(color));
		}
	}
	free(px);
	return n_colors;
}

/* The closest palette colour: weighted by how the eye sees each channel,
 * or (weighted false) plainly, which favours the bold colours of a fixed
 * palette. */
static void nearest(const float palette[][3], int n, const float v[3],
		bool weighted, float out[3]) {
	float best = INFINITY;
	float wr = weighted ? 0.30f : 1, wg = weighted ? 0.59f : 1,
		wb = weighted ? 0.11f : 1;
	for (int i = 0; i < n; i++) {
		float dr = v[0] - palette[i][0], dg = v[1] - palette[i][1],
			db = v[2] - palette[i][2];
		float d = wr * dr * dr + wg * dg * dg + wb * db * db;
		if (d < best) {
			best = d;
			memcpy(out, palette[i], sizeof(float) * 3);
		}
	}
}

/* block: a big pixel's size in device pixels; it needn't be whole. */
void dither_surface(cairo_surface_t *surface, double block,
		enum dither_palette palette_kind, enum dither_style style) {
	int w = cairo_image_surface_get_width(surface);
	int h = cairo_image_surface_get_height(surface);
	block = block < 1 ? 1 : block;
	int sw = (int)ceil(w / block), sh = (int)ceil(h / block);

	/* One sample per big pixel: the average of what's under it. */
	cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *cr = cairo_create(small);
	cairo_scale(cr, 1.0 / block, 1.0 / block);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	/* A big pixel partly off the edge takes its colour from what's on. */
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(small);

	uint32_t *spx = (uint32_t *)cairo_image_surface_get_data(small);
	int sstride = cairo_image_surface_get_stride(small) / 4;
	float (*samples)[3] = malloc(sizeof(*samples) * sw * sh);
	for (int y = 0; y < sh; y++) {
		for (int x = 0; x < sw; x++) {
			uint32_t p = spx[y * sstride + x];
			float *s = samples[y * sw + x];
			s[0] = (p >> 16) & 0xff;
			s[1] = (p >> 8) & 0xff;
			s[2] = p & 0xff;
		}
	}

	float palette[MAX_COLORS][3];
	int n_colors = 0;
	if (palette_kind == DITHER_ST16) {
		n_colors = choose_palette((const float (*)[3])samples, sw * sh, palette);
	} else if (palette_kind == DITHER_ATARI16) {
		memcpy(palette, atari16, sizeof(atari16));
		n_colors = MAX_COLORS;
	}
	/* How far the ordered pattern may push a pixel: about the gap between
	 * neighbouring colours. */
	float spread = n_colors > 0 ? 64.0f : st_step;

	for (int y = 0; y < sh; y++) {
		for (int x = 0; x < sw; x++) {
			float *s = samples[y * sw + x];
			float v[3], q[3];
			for (int c = 0; c < 3; c++) {
				v[c] = s[c];
				if (style == DITHER_ORDERED) {
					v[c] += ((bayer[y & 7][x & 7] + 0.5f) / 64 - 0.5f) * spread;
				}
				v[c] = v[c] < 0 ? 0 : v[c] > 255 ? 255 : v[c];
			}
			if (n_colors > 0) {
				nearest((const float (*)[3])palette, n_colors, v,
					palette_kind == DITHER_ST16, q);
			} else {
				for (int c = 0; c < 3; c++) {
					q[c] = st_level(v[c]);
				}
			}
			if (style == DITHER_DIFFUSE) {
				/* Floyd-Steinberg: 7/16 right, 3/16, 5/16, 1/16 below. */
				for (int c = 0; c < 3; c++) {
					float e = v[c] - q[c];
					if (x + 1 < sw) {
						samples[y * sw + x + 1][c] += e * 7 / 16;
					}
					if (y + 1 < sh) {
						if (x > 0) {
							samples[(y + 1) * sw + x - 1][c] += e * 3 / 16;
						}
						samples[(y + 1) * sw + x][c] += e * 5 / 16;
						if (x + 1 < sw) {
							samples[(y + 1) * sw + x + 1][c] += e * 1 / 16;
						}
					}
				}
			}
			spx[y * sstride + x] = 0xff000000u | (uint32_t)q[0] << 16 |
				(uint32_t)q[1] << 8 | (uint32_t)q[2];
		}
	}
	free(samples);
	cairo_surface_mark_dirty(small);

	/* Back to full size, with hard edges. */
	cr = cairo_create(surface);
	cairo_scale(cr, block, block);
	cairo_set_source_surface(cr, small, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(small);
}
