/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include "blur.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "ext-background-effect-v1-protocol.h"
#include "common/mem.h"
#include "labwc.h"
#include "output.h"

struct elavo_blur {
	struct wl_list link;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
	pixman_region32_t pending;
	pixman_region32_t current;
	bool has_pending;
	bool active;
	uint64_t seen_dirty;
	uint64_t signature;
	struct wlr_scene_buffer *node;
	struct wl_listener node_destroy;
};

struct surface_watch {
	struct wl_list link;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct wl_global *effect_global;
static struct wl_list blurs = { 0 };
static struct wl_list watches = { 0 };
static struct wl_listener new_surface_listener;
static uint64_t dirty_serial = 1;
static int logged_blurs;
static bool pixman_blocked;

static void
mark_dirty(void)
{
	dirty_serial++;
}

static struct elavo_blur *
blur_from_surface(struct wlr_surface *surface)
{
	struct elavo_blur *blur;
	wl_list_for_each(blur, &blurs, link) {
		if (blur->surface == surface) {
			return blur;
		}
	}
	return NULL;
}

static void
hide_node(struct elavo_blur *blur)
{
	if (blur->node) {
		wlr_scene_node_set_enabled(&blur->node->node, false);
	}
}

static void
destroy_blur(struct elavo_blur *blur)
{
	if (!blur) {
		return;
	}
	wl_list_remove(&blur->commit.link);
	wl_list_remove(&blur->destroy.link);
	wl_list_remove(&blur->link);
	pixman_region32_fini(&blur->pending);
	pixman_region32_fini(&blur->current);
	if (blur->node) {
		wl_list_remove(&blur->node_destroy.link);
		wlr_scene_node_destroy(&blur->node->node);
	}
	free(blur);
}

static void
blur_handle_commit(struct wl_listener *listener, void *data)
{
	struct elavo_blur *blur = wl_container_of(listener, blur, commit);
	(void)data;
	if (blur->has_pending) {
		/* Clip at commit, when the surface size is current. A region
		 * set before the first commit would otherwise be emptied. */
		pixman_region32_intersect_rect(&blur->pending, &blur->pending,
			0, 0, blur->surface->current.width,
			blur->surface->current.height);
		pixman_region32_copy(&blur->current, &blur->pending);
		blur->active = pixman_region32_not_empty(&blur->current);
		blur->has_pending = false;
	}
	mark_dirty();
}

static void
blur_handle_destroy(struct wl_listener *listener, void *data)
{
	struct elavo_blur *blur = wl_container_of(listener, blur, destroy);
	(void)data;
	if (blur->resource) {
		wl_resource_set_user_data(blur->resource, NULL);
	}
	destroy_blur(blur);
}

static void
effect_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
effect_set_blur_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region_resource)
{
	(void)client;
	struct elavo_blur *blur = wl_resource_get_user_data(resource);
	if (!blur) {
		return;
	}
	if (!blur->surface) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"surface destroyed");
		return;
	}
	pixman_region32_clear(&blur->pending);
	if (region_resource) {
		const pixman_region32_t *region = wlr_region_from_resource(region_resource);
		if (region) {
			pixman_region32_copy(&blur->pending, region);
		}
	}
	blur->has_pending = true;
}

static void
effect_resource_destroy(struct wl_resource *resource)
{
	struct elavo_blur *blur = wl_resource_get_user_data(resource);
	if (!blur) {
		return;
	}
	blur->resource = NULL;
	destroy_blur(blur);
}

static const struct ext_background_effect_surface_v1_interface effect_impl = {
	.destroy = effect_destroy,
	.set_blur_region = effect_set_blur_region,
};

static void
manager_get_background_effect(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (blur_from_surface(surface)) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"surface already has a background effect");
		return;
	}

	struct elavo_blur *blur = znew(*blur);
	blur->surface = surface;
	pixman_region32_init(&blur->pending);
	pixman_region32_init(&blur->current);
	wl_list_insert(&blurs, &blur->link);

	blur->commit.notify = blur_handle_commit;
	wl_signal_add(&surface->events.commit, &blur->commit);
	blur->destroy.notify = blur_handle_destroy;
	wl_signal_add(&surface->events.destroy, &blur->destroy);

	blur->resource = wl_resource_create(client,
		&ext_background_effect_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (!blur->resource) {
		destroy_blur(blur);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(blur->resource, &effect_impl, blur,
		effect_resource_destroy);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
	.destroy = effect_destroy,
	.get_background_effect = manager_get_background_effect,
};

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
	ext_background_effect_manager_v1_send_capabilities(resource,
		EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

bool
elavo_blur_is_ours(struct wlr_scene_node *node)
{
	struct elavo_blur *blur;
	wl_list_for_each(blur, &blurs, link) {
		if (blur->node && &blur->node->node == node) {
			return true;
		}
	}
	return false;
}

static bool
surface_on_output(struct wlr_scene_node *node, struct output *output, int *lx, int *ly)
{
	if (!wlr_scene_node_coords(node, lx, ly)) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &box);
	return *lx < box.x + box.width && *lx + 1 > box.x
		&& *ly < box.y + box.height && *ly + 1 > box.y;
}

static void
shape_in_buffer(pixman_region32_t *dst, const pixman_region32_t *src,
		int origin_x, int origin_y, int scale)
{
	pixman_region32_init(dst);
	int n = 0;
	pixman_box32_t *boxes = pixman_region32_rectangles((pixman_region32_t *)src, &n);
	for (int i = 0; i < n; i++) {
		int x = (boxes[i].x1 - origin_x) * scale;
		int y = (boxes[i].y1 - origin_y) * scale;
		int w = (boxes[i].x2 - boxes[i].x1) * scale;
		int h = (boxes[i].y2 - boxes[i].y1) * scale;
		if (w > 0 && h > 0) {
			pixman_region32_union_rect(dst, dst, x, y, w, h);
		}
	}
}

static bool
backdrop_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static void
blur_handle_node_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct elavo_blur *blur = wl_container_of(listener, blur, node_destroy);
	wl_list_remove(&blur->node_destroy.link);
	blur->node = NULL;
}

static void
update_blur(struct elavo_blur *blur, struct output *output)
{
	struct wlr_scene_tree *tree = blur->surface->data;
	if (!tree || tree->node.type != WLR_SCENE_NODE_TREE || !tree->node.parent) {
		hide_node(blur);
		return;
	}
	int lx = 0;
	int ly = 0;
	if (!surface_on_output(&tree->node, output, &lx, &ly)) {
		return;
	}
	if (!blur->active || !elavo_mods_blur_enabled() || pixman_blocked) {
		blur->seen_dirty = dirty_serial;
		hide_node(blur);
		return;
	}

	pixman_box32_t *ext = pixman_region32_extents(&blur->current);
	int bw = ext->x2 - ext->x1;
	int bh = ext->y2 - ext->y1;
	if (bw < 1 || bh < 1) {
		blur->seen_dirty = dirty_serial;
		hide_node(blur);
		return;
	}

	uint64_t signature = elavo_blur_signature(&tree->node,
		lx + ext->x1, ly + ext->y1, bw, bh);
	if (blur->seen_dirty == dirty_serial && blur->signature == signature) {
		return;
	}
	blur->seen_dirty = dirty_serial;
	blur->signature = signature;

	int scale = (int)lround(output->wlr_output->scale);
	if (scale < 1) {
		scale = 1;
	}
	pixman_region32_t shape;
	shape_in_buffer(&shape, &blur->current, ext->x1, ext->y1, scale);

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct wlr_buffer *buffer = elavo_blur_compose(output, &tree->node,
		lx + ext->x1, ly + ext->y1, bw, bh, &shape);
	pixman_region32_fini(&shape);
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	if (logged_blurs < 8) {
		double ms = (end.tv_sec - start.tv_sec) * 1000.0
			+ (end.tv_nsec - start.tv_nsec) / 1.0e6;
		wlr_log(WLR_INFO, "elavo blur %dx%d in %.2f ms", bw, bh, ms);
		logged_blurs++;
	}
	if (!buffer) {
		hide_node(blur);
		return;
	}

	/*
	 * The backdrop is the bottom child of the surface's own tree, not a
	 * sibling of it. labwc walks each layer tree's children as layer
	 * surfaces, and a foreign sibling there aborts the next arrange.
	 */
	if (!blur->node) {
		blur->node = wlr_scene_buffer_create(tree, NULL);
		if (!blur->node) {
			wlr_buffer_drop(buffer);
			return;
		}
		blur->node->point_accepts_input = backdrop_accepts_input;
		blur->node_destroy.notify = blur_handle_node_destroy;
		wl_signal_add(&blur->node->node.events.destroy, &blur->node_destroy);
	}
	if (blur->node->node.parent != tree) {
		wlr_scene_node_reparent(&blur->node->node, tree);
	}
	wlr_scene_node_lower_to_bottom(&blur->node->node);
	wlr_scene_node_set_position(&blur->node->node, ext->x1, ext->y1);
	wlr_scene_buffer_set_dest_size(blur->node, bw, bh);
	wlr_scene_buffer_set_buffer(blur->node, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_node_set_enabled(&blur->node->node, true);
}

static void
any_surface_commit(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	/* A window under the dock commits without the dock committing. */
	mark_dirty();
}

static void
watch_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct surface_watch *watch = wl_container_of(listener, watch, destroy);
	wl_list_remove(&watch->commit.link);
	wl_list_remove(&watch->destroy.link);
	wl_list_remove(&watch->link);
	free(watch);
}

static void
handle_new_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_surface *surface = data;
	struct surface_watch *watch = znew(*watch);
	watch->commit.notify = any_surface_commit;
	wl_signal_add(&surface->events.commit, &watch->commit);
	watch->destroy.notify = watch_handle_destroy;
	wl_signal_add(&surface->events.destroy, &watch->destroy);
	wl_list_insert(&watches, &watch->link);
}

void
elavo_mods_output_frame(struct output *output)
{
	if (!output || wl_list_empty(&blurs)) {
		return;
	}
	struct elavo_blur *blur;
	wl_list_for_each(blur, &blurs, link) {
		update_blur(blur, output);
	}
}

void
elavo_mods_init(void)
{
	wl_list_init(&blurs);
	wl_list_init(&watches);
	new_surface_listener.notify = handle_new_surface;
	wl_signal_add(&server.compositor->events.new_surface, &new_surface_listener);
	pixman_blocked = wlr_renderer_is_pixman(server.renderer)
		&& !getenv("ELAVO_BLUR_PIXMAN");
	if (pixman_blocked) {
		wlr_log(WLR_INFO, "elavo blur off on the pixman renderer");
	}
	effect_global = wl_global_create(server.wl_display,
		&ext_background_effect_manager_v1_interface, 1, NULL, manager_bind);
	if (!effect_global) {
		wlr_log(WLR_ERROR, "failed to advertise ext_background_effect_manager_v1");
	}
}

void
elavo_mods_finish(void)
{
	struct elavo_blur *blur, *tmp;
	wl_list_for_each_safe(blur, tmp, &blurs, link) {
		if (blur->resource) {
			wl_resource_set_user_data(blur->resource, NULL);
			blur->resource = NULL;
		}
		destroy_blur(blur);
	}
	struct surface_watch *watch, *watch_tmp;
	wl_list_for_each_safe(watch, watch_tmp, &watches, link) {
		wl_list_remove(&watch->commit.link);
		wl_list_remove(&watch->destroy.link);
		wl_list_remove(&watch->link);
		free(watch);
	}
	wl_list_remove(&new_surface_listener.link);
	if (effect_global) {
		wl_global_destroy(effect_global);
		effect_global = NULL;
	}
}
