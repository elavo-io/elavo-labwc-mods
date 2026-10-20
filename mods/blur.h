/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_BLUR_H
#define ELAVO_BLUR_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>

struct output;
struct wlr_buffer;
struct wlr_scene_node;
struct wlr_surface;

/* True for a scene node we inserted as a blur backdrop. */
bool elavo_blur_is_ours(struct wlr_scene_node *node);

/* How many times surface has committed. */
uint64_t elavo_blur_surface_commits(struct wlr_surface *surface);

/*
 * Hash of what lies under an area: node, position, size, buffer, and the
 * commit count of the surface showing it. Moving a window changes it
 * without any client committing; a commit to a surface outside the area,
 * or above it, does not change it.
 */
uint64_t elavo_blur_signature(struct wlr_scene_node *stop,
	int layout_x, int layout_y, int logical_w, int logical_h);

#define ELAVO_BLUR_MAX_LEVELS 5

/*
 * The buffers one backdrop draws into, kept between frames. Allocating
 * them each time costs a GPU allocation, an import, and a framebuffer per
 * buffer. The result alternates between two, so the one the scene shows is
 * never drawn into.
 */
struct elavo_blur_buffers {
	struct wlr_buffer *capture;
	struct wlr_buffer *down[ELAVO_BLUR_MAX_LEVELS];
	struct wlr_buffer *up[ELAVO_BLUR_MAX_LEVELS];
	struct wlr_buffer *out[2];
	int next_out;
};

void elavo_blur_buffers_finish(struct elavo_blur_buffers *buffers);

/*
 * Capture what is below stop, blur it, and return a buffer the size of the
 * logical box at (layout_x, layout_y). shape is in that buffer's pixel space
 * and clips the result. The buffer belongs to buffers; it stays valid until
 * the next call or elavo_blur_buffers_finish.
 */
struct wlr_buffer *elavo_blur_compose(struct elavo_blur_buffers *buffers,
	struct output *output, struct wlr_scene_node *stop,
	int layout_x, int layout_y, int logical_w, int logical_h,
	const pixman_region32_t *shape);

#endif
