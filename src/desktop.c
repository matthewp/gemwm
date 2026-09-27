#include <cairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "cairo_buffer.h"
#include "server.h"

/* The colour ST desktop (low and medium resolution) is palette entry $070:
 * full-intensity green on the ST's 3-bit-per-channel DAC. */
#define DESKTOP_GREEN 0xff00ff00u

/* GEMWM_DESKTOP picks the desktop: unset for the colour ST's green,
 * "mono" for the high-resolution 50% dither, or an RRGGBB colour. Returns
 * false for the dither. */
static bool desktop_color(uint32_t *argb) {
	const char *env = getenv("GEMWM_DESKTOP");
	if (env == NULL || env[0] == '\0') {
		*argb = DESKTOP_GREEN;
		return true;
	}
	if (strcmp(env, "mono") == 0) {
		return false;
	}
	if (env[0] == '#') {
		env++;
	}
	char *end;
	unsigned long rgb = strtoul(env, &end, 16);
	*argb = (*end == '\0' && strlen(env) == 6) ?
		0xff000000u | (uint32_t)rgb : DESKTOP_GREEN;
	return true;
}

void desktop_update_output(struct output *output) {
	struct server *server = output->server;
	int w, h;
	wlr_output_effective_resolution(output->wlr_output, &w, &h);
	if (w <= 0 || h <= 0) {
		return;
	}

	struct cairo_buffer *buffer = cairo_buffer_create(w, h);
	if (buffer == NULL) {
		return;
	}
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(buffer->surface);
	int stride = cairo_image_surface_get_stride(buffer->surface) / 4;
	uint32_t color;
	bool solid = desktop_color(&color);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			px[y * stride + x] = solid ? color :
				((x + y) & 1) ? 0xffffffff : 0xff000000;
		}
	}
	cairo_surface_mark_dirty(buffer->surface);

	if (output->desktop == NULL) {
		output->desktop =
			wlr_scene_buffer_create(server->layer_desktop, NULL);
	}
	cairo_buffer_submit(buffer, output->desktop);

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	wlr_scene_node_set_position(&output->desktop->node, box.x, box.y);
}
