/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "elavo-mods-internal.h"
#include "blur.h"
#include <drm_fourcc.h>
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "labwc.h"
#include "output.h"

struct drawn_texture {
	struct wlr_texture *texture;
	bool owned;
};

struct capture {
	struct wlr_render_pass *pass;
	struct wlr_scene_node *stop;
	struct output *output;
	int origin_x;
	int origin_y;
	int scale;
	pixman_region32_t *clip;
	struct drawn_texture *temps;
	size_t ntemps;
	size_t ctemps;
};

static void
keep_texture(struct capture *cap, struct wlr_texture *texture, bool owned)
{
	if (!texture) {
		return;
	}
	if (cap->ntemps == cap->ctemps) {
		size_t next = cap->ctemps ? cap->ctemps * 2 : 16;
		struct drawn_texture *grown = realloc(cap->temps, next * sizeof(*grown));
		if (!grown) {
			if (owned) {
				wlr_texture_destroy(texture);
			}
			return;
		}
		cap->temps = grown;
		cap->ctemps = next;
	}
	cap->temps[cap->ntemps].texture = texture;
	cap->temps[cap->ntemps].owned = owned;
	cap->ntemps++;
}

static bool
intersects(int x, int y, int w, int h, int ox, int oy, int ow, int oh)
{
	return x < ox + ow && x + w > ox && y < oy + oh && y + h > oy;
}

static bool
walk(struct wlr_scene_node *node, int ax, int ay, struct capture *cap, int cap_w, int cap_h)
{
	if (node == cap->stop) {
		return true;
	}
	/* node->data belongs to labwc. Overwriting it crashes cursor hit-testing. */
	if (!node->enabled || elavo_blur_is_ours(node)) {
		return false;
	}

	int x = ax + node->x;
	int y = ay + node->y;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			if (walk(child, x, y, cap, cap_w, cap_h)) {
				return true;
			}
		}
		return false;
	}

	if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (!intersects(x, y, rect->width, rect->height,
				cap->origin_x, cap->origin_y, cap_w, cap_h)) {
			return false;
		}
		wlr_render_pass_add_rect(cap->pass, &(struct wlr_render_rect_options){
			.box = {
				.x = (x - cap->origin_x) * cap->scale,
				.y = (y - cap->origin_y) * cap->scale,
				.width = rect->width * cap->scale,
				.height = rect->height * cap->scale,
			},
			.color = {
				.r = rect->color[0],
				.g = rect->color[1],
				.b = rect->color[2],
				.a = rect->color[3],
			},
			.clip = cap->clip,
		});
		return false;
	}

	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return false;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	if (!scene_buffer->buffer) {
		return false;
	}
	int dw = scene_buffer->dst_width > 0
		? scene_buffer->dst_width : scene_buffer->buffer->width;
	int dh = scene_buffer->dst_height > 0
		? scene_buffer->dst_height : scene_buffer->buffer->height;
	if (dw <= 0 || dh <= 0) {
		return false;
	}
	if (!intersects(x, y, dw, dh, cap->origin_x, cap->origin_y, cap_w, cap_h)) {
		return false;
	}

	struct wlr_texture *texture = NULL;
	bool owned = false;
	struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
	if (client_buffer && client_buffer->texture) {
		texture = client_buffer->texture;
	} else {
		texture = wlr_texture_from_buffer(server.renderer, scene_buffer->buffer);
		owned = texture != NULL;
	}
	if (!texture) {
		return false;
	}
	keep_texture(cap, texture, owned);

	float alpha = scene_buffer->opacity;
	struct wlr_fbox src = scene_buffer->src_box;
	if (src.x < 0 || src.y < 0) {
		src.x = 0;
		src.y = 0;
		src.width = 0;
		src.height = 0;
	}
	wlr_render_pass_add_texture(cap->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = src,
		.dst_box = {
			.x = (x - cap->origin_x) * cap->scale,
			.y = (y - cap->origin_y) * cap->scale,
			.width = dw * cap->scale,
			.height = dh * cap->scale,
		},
		.alpha = &alpha,
		.clip = cap->clip,
		.transform = scene_buffer->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
	return false;
}

struct signature {
	struct wlr_scene_node *stop;
	int x, y, w, h;
	uint64_t hash;
};

static void
mix(struct signature *sig, uint64_t value)
{
	sig->hash ^= value + 0x9e3779b97f4a7c15ull + (sig->hash << 6) + (sig->hash >> 2);
}

static bool
sig_walk(struct wlr_scene_node *node, int ax, int ay, struct signature *sig)
{
	if (node == sig->stop) {
		return true;
	}
	if (!node->enabled || elavo_blur_is_ours(node)) {
		return false;
	}
	int x = ax + node->x;
	int y = ay + node->y;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			if (sig_walk(child, x, y, sig)) {
				return true;
			}
		}
		return false;
	}

	int w = 0;
	int h = 0;
	uint64_t content = 0;
	if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		w = rect->width;
		h = rect->height;
		for (int i = 0; i < 4; i++) {
			content = content * 1000003u + (uint64_t)lroundf(rect->color[i] * 255.0f);
		}
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			return false;
		}
		w = scene_buffer->dst_width > 0
			? scene_buffer->dst_width : scene_buffer->buffer->width;
		h = scene_buffer->dst_height > 0
			? scene_buffer->dst_height : scene_buffer->buffer->height;
		content = (uintptr_t)scene_buffer->buffer
			^ ((uint64_t)lroundf(scene_buffer->opacity * 255.0f) << 56);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface) {
			content = content * 1000003u
				+ elavo_blur_surface_commits(scene_surface->surface);
		}
	} else {
		return false;
	}
	if (!intersects(x, y, w, h, sig->x, sig->y, sig->w, sig->h)) {
		return false;
	}
	mix(sig, (uintptr_t)node);
	mix(sig, ((uint64_t)(uint32_t)x << 32) | (uint32_t)y);
	mix(sig, ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
	mix(sig, content);
	return false;
}

uint64_t
elavo_blur_signature(struct wlr_scene_node *stop,
		int layout_x, int layout_y, int logical_w, int logical_h)
{
	int reach = elavo_mods_blur_levels() * 8;
	struct signature sig = {
		.stop = stop,
		.x = layout_x - reach,
		.y = layout_y - reach,
		.w = logical_w + reach * 2,
		.h = logical_h + reach * 2,
		.hash = 0xcbf29ce484222325ull,
	};
	mix(&sig, ((uint64_t)(uint32_t)layout_x << 32) | (uint32_t)layout_y);
	mix(&sig, ((uint64_t)(uint32_t)logical_w << 32) | (uint32_t)logical_h);
	sig_walk(&server.scene->tree.node, 0, 0, &sig);
	return sig.hash;
}

static struct wlr_buffer *
alloc_buffer(struct output *output, int width, int height)
{
	if (!output->wlr_output->swapchain || width < 1 || height < 1) {
		return NULL;
	}
	return wlr_allocator_create_buffer(server.allocator, width, height,
		&output->wlr_output->swapchain->format);
}

/*
 * The output usually scans out XRGB, where a cleared pixel is black. The
 * result is shown through a rounded shape, so it needs an alpha channel.
 * Same modifiers as the swapchain; only the format changes.
 */
static struct wlr_buffer *
alloc_alpha_buffer(struct output *output, int width, int height)
{
	if (!output->wlr_output->swapchain || width < 1 || height < 1) {
		return NULL;
	}
	struct wlr_drm_format format = output->wlr_output->swapchain->format;
	format.format = DRM_FORMAT_ARGB8888;
	struct wlr_buffer *buffer =
		wlr_allocator_create_buffer(server.allocator, width, height, &format);
	if (!buffer) {
		uint64_t linear = DRM_FORMAT_MOD_LINEAR;
		format.len = 1;
		format.capacity = 1;
		format.modifiers = &linear;
		buffer = wlr_allocator_create_buffer(server.allocator,
			width, height, &format);
	}
	return buffer;
}

/* The buffer in slot, replaced by a new one if it is missing or the wrong size. */
static struct wlr_buffer *
reuse(struct wlr_buffer **slot, struct output *output, int width, int height, bool alpha)
{
	if (*slot && (*slot)->width == width && (*slot)->height == height) {
		return *slot;
	}
	if (*slot) {
		wlr_buffer_drop(*slot);
	}
	*slot = alpha ? alloc_alpha_buffer(output, width, height)
		: alloc_buffer(output, width, height);
	return *slot;
}

static void
drop_slot(struct wlr_buffer **slot)
{
	if (*slot) {
		wlr_buffer_drop(*slot);
		*slot = NULL;
	}
}

void
elavo_blur_buffers_finish(struct elavo_blur_buffers *buffers)
{
	drop_slot(&buffers->capture);
	for (int i = 0; i < ELAVO_BLUR_MAX_LEVELS; i++) {
		drop_slot(&buffers->down[i]);
		drop_slot(&buffers->up[i]);
	}
	drop_slot(&buffers->out[0]);
	drop_slot(&buffers->out[1]);
	buffers->next_out = 0;
}

static void
clear_buffer(struct wlr_render_pass *pass, int width, int height)
{
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = width, .height = height },
		.color = { 0, 0, 0, 0 },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
}

static bool
draw_scaled(struct wlr_buffer *dst, struct wlr_texture *src, int sw, int sh)
{
	int dw = dst->width;
	int dh = dst->height;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, dst, NULL);
	if (!pass) {
		return false;
	}
	clear_buffer(pass, dw, dh);

	/* Stay inside the texture. wlroots rejects a source box with a
	 * negative origin, so the diagonal taps are 0 and 1 pixel. */
	static const float ox[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
	static const float oy[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const float weight[4] = { 1.0f, 0.5f, 1.0f / 3.0f, 0.25f };
	float src_w = sw > 1 ? (float)sw - 1.0f : (float)sw;
	float src_h = sh > 1 ? (float)sh - 1.0f : (float)sh;

	for (int i = 0; i < 4; i++) {
		float alpha = weight[i];
		float sx = sw > 1 ? ox[i] : 0.0f;
		float sy = sh > 1 ? oy[i] : 0.0f;
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = src,
			.src_box = {
				.x = sx,
				.y = sy,
				.width = src_w,
				.height = src_h,
			},
			.dst_box = { .x = 0, .y = 0, .width = dw, .height = dh },
			.alpha = i == 0 ? NULL : &alpha,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
			.blend_mode = i == 0
				? WLR_RENDER_BLEND_MODE_NONE
				: WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
	}
	return wlr_render_pass_submit(pass);
}

static bool
scale_into(struct wlr_buffer *dst, struct wlr_buffer *src)
{
	struct wlr_texture *tex = wlr_texture_from_buffer(server.renderer, src);
	if (!tex) {
		return false;
	}
	bool ok = draw_scaled(dst, tex, src->width, src->height);
	wlr_texture_destroy(tex);
	return ok;
}

/*
 * Halve source levels times into down[], then double back up through up[],
 * where up[i] has the size of down[i - 1] (up[0] that of source). Returns
 * up[0], or source when levels is 0, or NULL on failure.
 */
static struct wlr_buffer *
blur_levels(struct elavo_blur_buffers *buffers, struct output *output,
		struct wlr_buffer *source, int levels)
{
	if (levels > ELAVO_BLUR_MAX_LEVELS) {
		levels = ELAVO_BLUR_MAX_LEVELS;
	}
	struct wlr_buffer *current = source;
	int ndown = 0;
	for (int level = 0; level < levels; level++) {
		int nw = current->width > 1 ? current->width / 2 : 1;
		int nh = current->height > 1 ? current->height / 2 : 1;
		struct wlr_buffer *down = reuse(&buffers->down[level], output, nw, nh, false);
		if (!down || !scale_into(down, current)) {
			break;
		}
		current = down;
		ndown++;
	}

	for (int i = ndown - 1; i >= 0; i--) {
		struct wlr_buffer *larger = (i == 0) ? source : buffers->down[i - 1];
		struct wlr_buffer *up = reuse(&buffers->up[i], output,
			larger->width, larger->height, false);
		if (!up || !scale_into(up, current)) {
			return NULL;
		}
		current = up;
	}
	return current;
}

struct wlr_buffer *
elavo_blur_compose(struct elavo_blur_buffers *buffers, struct output *output,
		struct wlr_scene_node *stop, int layout_x, int layout_y,
		int logical_w, int logical_h, const pixman_region32_t *shape)
{
	if (!output || !output->wlr_output || !stop || logical_w < 1 || logical_h < 1) {
		return NULL;
	}
	int scale = (int)lround(output->wlr_output->scale);
	if (scale < 1) {
		scale = 1;
	}
	int levels = elavo_mods_blur_levels();
	int reach = levels * 8;
	int cap_w = logical_w + reach * 2;
	int cap_h = logical_h + reach * 2;
	int px_w = cap_w * scale;
	int px_h = cap_h * scale;
	if (px_w > 4096 || px_h > 4096) {
		return NULL;
	}

	struct wlr_buffer *captured = reuse(&buffers->capture, output, px_w, px_h, false);
	if (!captured) {
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, captured, NULL);
	if (!pass) {
		return NULL;
	}
	clear_buffer(pass, px_w, px_h);

	struct capture cap = {
		.pass = pass,
		.stop = stop,
		.output = output,
		.origin_x = layout_x - reach,
		.origin_y = layout_y - reach,
		.scale = scale,
	};
	walk(&server.scene->tree.node, 0, 0, &cap, cap_w, cap_h);
	bool ok = wlr_render_pass_submit(pass);
	for (size_t i = 0; i < cap.ntemps; i++) {
		if (cap.temps[i].owned) {
			wlr_texture_destroy(cap.temps[i].texture);
		}
	}
	free(cap.temps);
	if (!ok) {
		return NULL;
	}

	struct wlr_buffer *blurred = blur_levels(buffers, output, captured, levels);
	if (!blurred) {
		return NULL;
	}

	int out_w = logical_w * scale;
	int out_h = logical_h * scale;
	struct wlr_buffer *out = reuse(&buffers->out[buffers->next_out],
		output, out_w, out_h, true);
	if (!out) {
		return NULL;
	}
	struct wlr_texture *tex = wlr_texture_from_buffer(server.renderer, blurred);
	if (!tex) {
		return NULL;
	}
	pass = wlr_renderer_begin_buffer_pass(server.renderer, out, NULL);
	if (!pass) {
		wlr_texture_destroy(tex);
		return NULL;
	}
	clear_buffer(pass, out_w, out_h);
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = {
			.x = reach * scale,
			.y = reach * scale,
			.width = out_w,
			.height = out_h,
		},
		.dst_box = { .x = 0, .y = 0, .width = out_w, .height = out_h },
		.clip = shape,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
	ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(tex);
	if (!ok) {
		return NULL;
	}
	buffers->next_out ^= 1;
	return out;
}
