#ifndef GEMWM_FRAME_H
#define GEMWM_FRAME_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

/* Size of a GEM window gadget (closer, fuller, arrows, sizer), in logical
 * pixels. Matches the ST high-resolution (640x400 mono) look. */
#define GEM_GADGET 19

/* Frame extents around the client's content: a 1px border on the left, the
 * title bar on top, and a scroll bar column/row on the right and bottom. */
#define FRAME_LEFT 1
#define FRAME_TOP (GEM_GADGET + 2)
#define FRAME_RIGHT (GEM_GADGET + 2)
#define FRAME_BOTTOM (GEM_GADGET + 2)

/* Smallest content size that still fits all the gadgets. */
#define FRAME_MIN_W (GEM_GADGET * 3)
#define FRAME_MIN_H (GEM_GADGET * 3)

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
	FRAME_PART_VTRACK,
	FRAME_PART_HTRACK,
};

/* Draws the frame for content of the given size into the scene buffer. */
void frame_draw(struct wlr_scene_buffer *node, int content_w, int content_h,
	const char *title, bool active);

/* Which gadget is at (x, y), in frame-local coordinates. */
enum frame_part frame_part_at(int content_w, int content_h, int x, int y);

#endif
