#ifndef GEMWM_CAIRO_BUFFER_H
#define GEMWM_CAIRO_BUFFER_H

#include <cairo.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

/* A wlr_buffer backed by a cairo image surface, so we can draw with cairo
 * and hand the result to the scene graph. */
struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

struct cairo_buffer *cairo_buffer_create(int width, int height);

/* Hands the buffer to a scene node and drops our reference. Nearest
 * filtering keeps pixels crisp when the output is scaled up. */
void cairo_buffer_submit(struct cairo_buffer *buffer,
	struct wlr_scene_buffer *node);

#endif
