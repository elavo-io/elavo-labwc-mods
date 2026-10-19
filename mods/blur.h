/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_BLUR_H
#define ELAVO_BLUR_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>

struct output;
struct wlr_buffer;
struct wlr_scene_node;

/* True for a scene node we inserted as a blur backdrop. */
bool elavo_blur_is_ours(struct wlr_scene_node *node);

/*
 * Hash of what lies under an area: node, position, size and buffer. Moving
 * a window changes it without any client committing.
 */
uint64_t elavo_blur_signature(struct wlr_scene_node *stop,
	int layout_x, int layout_y, int logical_w, int logical_h);

/*
 * Capture what is below stop, blur it, and return a buffer the size of the
 * logical box at (layout_x, layout_y). shape is in that buffer's pixel space
 * and clips the result. Caller owns the buffer.
 */
struct wlr_buffer *elavo_blur_compose(struct output *output,
	struct wlr_scene_node *stop, int layout_x, int layout_y,
	int logical_w, int logical_h, const pixman_region32_t *shape);

#endif
