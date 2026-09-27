#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include "cairo_buffer.h"

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	wlr_buffer_finish(wlr_buffer);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = cairo_image_surface_get_data(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buffer->surface);
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

struct cairo_buffer *cairo_buffer_create(int width, int height) {
	struct cairo_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	buffer->surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(buffer->surface);
		free(buffer);
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	return buffer;
}

void cairo_buffer_submit(struct cairo_buffer *buffer,
		struct wlr_scene_buffer *node) {
	cairo_surface_flush(buffer->surface);
	wlr_scene_buffer_set_filter_mode(node, WLR_SCALE_FILTER_NEAREST);
	wlr_scene_buffer_set_buffer(node, &buffer->base);
	wlr_buffer_drop(&buffer->base);
}
