#ifndef GEMWM_FRAME_H
#define GEMWM_FRAME_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

/* Size of a GEM window gadget (closer, fuller, arrows, sizer), in logical
 * pixels. Matches the ST high-resolution (640x400 mono) look. */
#define GEM_GADGET 19

/* The title bar and left border never change; the right and bottom depend
 * on the window's scroll bars and sizer (frame_extents). */
#define FRAME_LEFT 1
#define FRAME_TOP (GEM_GADGET + 2)

/* Smallest content size that still fits all the gadgets. */
#define FRAME_MIN_W (GEM_GADGET * 3)
#define FRAME_MIN_H (GEM_GADGET * 3)

/* One scroll bar, as its client describes it (gemwm-scroll-v1). */
struct frame_axis {
	bool on;
	int position, visible, total;
};

/* Which gadgets a window has, besides title bar, closer and fuller. As in
 * GEM, a window only has scroll bars when its application drives them. */
struct frame_style {
	struct frame_axis v, h;
	bool sizer;
};

enum frame_part {
	FRAME_PART_NONE,
	FRAME_PART_BORDER,
	FRAME_PART_TITLE,
	FRAME_PART_CLOSER,
	FRAME_PART_FULLER,
	FRAME_PART_SIZER,
	FRAME_PART_UP,
	FRAME_PART_DOWN,
	FRAME_PART_LEFT,
	FRAME_PART_RIGHT,
	FRAME_PART_VSLIDER,
	FRAME_PART_HSLIDER,
	FRAME_PART_PAGE_UP,    /* the track above the slider */
	FRAME_PART_PAGE_DOWN,
	FRAME_PART_PAGE_LEFT,
	FRAME_PART_PAGE_RIGHT,
};

/* The frame's thickness on the right and bottom. */
void frame_extents(const struct frame_style *style, int *right, int *bottom);

/* Draws the frame for content of the given size into the scene buffer. */
void frame_draw(struct wlr_scene_buffer *node, const struct frame_style *style,
	int content_w, int content_h, const char *title, bool active);

/* Which gadget is at (x, y), in frame-local coordinates. */
enum frame_part frame_part_at(const struct frame_style *style,
	int content_w, int content_h, int x, int y);

/* The length of an axis's track and of its slider, in pixels, for turning
 * slider drags into positions. */
void frame_slider_size(const struct frame_style *style, int content_w,
	int content_h, bool vertical, int *track, int *slider);

#endif
