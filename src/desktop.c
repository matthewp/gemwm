/*
 * The desktop background, from [desktop] in the config: a colour (or the
 * high-resolution ST's 50% mono pattern), with a picture over it if one is
 * set, optionally shown in the ST's colours (dither.c).
 * It's drawn at the output's real pixel size, so pictures stay sharp on
 * HiDPI screens, while the dither keeps its chunky ST-sized pixels.
 */
#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include "cairo_buffer.h"
#include "server.h"

/* Named colours are ST palette entries: three bits a channel, $000-$777,
 * so each is one a real ST could show. */
static const struct {
	const char *name;
	uint32_t rgb;
} presets[] = {
	{ "green", 0x00ff00 },      /* $070, the colour desktop's own */
	{ "dark-green", 0x009200 }, /* $040 */
	{ "blue", 0x0000ff },       /* $007 */
	{ "navy", 0x000092 },       /* $004 */
	{ "cyan", 0x00ffff },       /* $077 */
	{ "teal", 0x009292 },       /* $044 */
	{ "amber", 0xffb600 },      /* $750 */
	{ "red", 0xb60000 },        /* $500 */
	{ "purple", 0x6d006d },     /* $303 */
	{ "grey", 0xb6b6b6 },       /* $555 */
	{ "dark-grey", 0x494949 },  /* $222 */
	{ "white", 0xffffff },      /* $777 */
	{ "black", 0x000000 },      /* $000 */
};

/* A [desktop] colour: a name above, "mono" for the dither, or #rrggbb. */
bool desktop_parse_color(const char *value, uint32_t *argb, bool *mono) {
	if (strcasecmp(value, "mono") == 0) {
		*mono = true;
		return true;
	}
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		if (strcasecmp(value, presets[i].name) == 0) {
			*argb = 0xff000000u | presets[i].rgb;
			*mono = false;
			return true;
		}
	}
	const char *hex = value[0] == '#' ? value + 1 : value;
	char *end;
	unsigned long rgb = strtoul(hex, &end, 16);
	if (strlen(hex) != 6 || *end != '\0') {
		return false;
	}
	*argb = 0xff000000u | (uint32_t)rgb;
	*mono = false;
	return true;
}

/* Any format gdk-pixbuf reads (PNG, JPEG, WebP...), as a cairo image,
 * turned the way its camera said. */
static cairo_surface_t *load_picture(const char *path) {
	GError *error = NULL;
	GdkPixbuf *loaded = gdk_pixbuf_new_from_file(path, &error);
	if (loaded == NULL) {
		wlr_log(WLR_ERROR, "desktop image %s: %s", path, error->message);
		g_error_free(error);
		return NULL;
	}
	GdkPixbuf *pb = gdk_pixbuf_apply_embedded_orientation(loaded);
	g_object_unref(loaded);

	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int channels = gdk_pixbuf_get_n_channels(pb);
	int rowstride = gdk_pixbuf_get_rowstride(pb);
	const guchar *pixels = gdk_pixbuf_read_pixels(pb);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	uint32_t *out = (uint32_t *)cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s) / 4;
	for (int y = 0; y < h; y++) {
		const guchar *p = pixels + (size_t)y * rowstride;
		for (int x = 0; x < w; x++, p += channels) {
			uint32_t a = channels == 4 ? p[3] : 255;
			/* cairo wants premultiplied alpha. */
			out[y * stride + x] = a << 24 | (p[0] * a / 255) << 16 |
				(p[1] * a / 255) << 8 | (p[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(s);
	g_object_unref(pb);
	return s;
}

/* The picture on a w x h desktop. Center and tile keep its pixels one
 * desktop pixel each (scale device pixels); the others size it to the
 * screen. Blown up 2x or more, it's pixel art: keep the pixels square. */
static void draw_picture(cairo_t *cr, cairo_surface_t *pic,
		enum image_mode mode, int w, int h, double scale) {
	double iw = cairo_image_surface_get_width(pic);
	double ih = cairo_image_surface_get_height(pic);
	double sx, sy;
	switch (mode) {
	case IMAGE_FILL:
		sx = sy = fmax(w / iw, h / ih);
		break;
	case IMAGE_FIT:
		sx = sy = fmin(w / iw, h / ih);
		break;
	case IMAGE_STRETCH:
		sx = w / iw;
		sy = h / ih;
		break;
	default: /* center, tile */
		sx = sy = scale;
		break;
	}
	cairo_save(cr);
	if (mode != IMAGE_TILE) {
		cairo_translate(cr, floor((w - iw * sx) / 2), floor((h - ih * sy) / 2));
	}
	cairo_scale(cr, sx, sy);
	cairo_set_source_surface(cr, pic, 0, 0);
	cairo_pattern_t *pattern = cairo_get_source(cr);
	cairo_pattern_set_filter(pattern, fmin(sx, sy) >= 2 ?
		CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
	if (mode == IMAGE_TILE) {
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	}
	cairo_paint(cr);
	cairo_restore(cr);
}

void desktop_update_output(struct output *output) {
	struct server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	int lw, lh, w, h;
	wlr_output_effective_resolution(wlr_output, &lw, &lh);
	wlr_output_transformed_resolution(wlr_output, &w, &h);
	if (lw <= 0 || lh <= 0 || w <= 0 || h <= 0) {
		return;
	}
	double scale = wlr_output->scale > 0 ? wlr_output->scale : 1;

	struct cairo_buffer *buffer = cairo_buffer_create(w, h);
	if (buffer == NULL) {
		return;
	}
	cairo_surface_t *surface = buffer->surface;
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	for (int y = 0; y < h; y++) {
		int dy = (int)(y / scale);
		for (int x = 0; x < w; x++) {
			/* The dither's pixels are desktop pixels, not device ones. */
			px[y * stride + x] = !server->desktop_mono ?
				server->desktop_color :
				(((int)(x / scale) + dy) & 1) ? 0xffffffff : 0xff000000;
		}
	}
	cairo_surface_mark_dirty(surface);
	if (server->desktop_picture != NULL) {
		cairo_t *cr = cairo_create(surface);
		draw_picture(cr, server->desktop_picture, server->desktop_image_mode,
			w, h, scale);
		cairo_destroy(cr);
	}
	if (server->desktop_palette != DITHER_OFF) {
		dither_surface(surface, (int)lround(server->desktop_pixel_size * scale),
			server->desktop_palette, server->desktop_dither_style);
	}
	cairo_surface_flush(surface);

	if (output->desktop == NULL) {
		output->desktop =
			wlr_scene_buffer_create(server->layer_desktop, NULL);
	}
	cairo_buffer_submit(buffer, output->desktop);
	wlr_scene_buffer_set_dest_size(output->desktop, lw, lh);

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
	wlr_scene_node_set_position(&output->desktop->node, box.x, box.y);
}

/* After the config is (re)read: the picture is loaded again, and every
 * desktop redrawn. GEMWM_DESKTOP, if set, overrides the
 * colour (as it did before there was a [desktop] section). */
void desktop_configure(struct server *server) {
	const char *env = getenv("GEMWM_DESKTOP");
	if (env != NULL && env[0] != '\0' &&
			!desktop_parse_color(env, &server->desktop_color,
				&server->desktop_mono)) {
		wlr_log(WLR_ERROR, "GEMWM_DESKTOP: not a colour: %s", env);
	}
	if (server->desktop_picture != NULL) {
		cairo_surface_destroy(server->desktop_picture);
		server->desktop_picture = NULL;
	}
	if (server->desktop_image != NULL) {
		server->desktop_picture = load_picture(server->desktop_image);
	}
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		desktop_update_output(output);
	}
}
