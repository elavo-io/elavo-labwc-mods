/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#ifndef HAVE_SCENEFX
#include "blur.h"
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "ext-background-effect-v1-protocol.h"
#include "common/macros.h"
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
	/* The blur region changed; the signature does not cover it. */
	bool dirty;
	uint64_t signature;
	struct wlr_scene_buffer *node;
	struct wl_listener node_destroy;
	/*
	 * surface->data is the scene tree while the popup or layer surface
	 * owns it, and a dangling pointer after that tree is freed. A freed
	 * node's parent can be a non-canonical heap cookie; walking it in
	 * wlr_scene_node_coords is a general protection fault, which takes
	 * the session down. Remember the tree, drop it when the tree dies,
	 * and never follow a parent that is not an address we can read.
	 */
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *dead;
	struct wl_listener tree_destroy;
	bool tree_listening;
#ifdef HAVE_SCENEFX
	/*
	 * SceneFX blur nodes, in one tree at the bottom of the tree that
	 * holds the surface's buffer. Rebuilt when the region changes.
	 */
	struct wlr_scene_tree *fx;
	struct wl_listener fx_destroy;
	pixman_region32_t fx_region;
#else
	struct elavo_blur_buffers buffers;
#endif
};

/*
 * Counts each surface's commits. A client can draw into the buffer it
 * already showed, which leaves the scene unchanged, so the signature mixes
 * in this count for every surface under a backdrop. Commits elsewhere, such
 * as Lens redrawing itself on hover, leave the backdrop alone.
 */
struct surface_watch {
	struct wl_list link;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	uint64_t commits;
	struct wl_listener commit;
	struct wl_listener destroy;
};

#ifdef HAVE_SCENEFX
#define blur_buffers_finish(blur) ((void)(blur))
#else
#define blur_buffers_finish(blur) elavo_blur_buffers_finish(&(blur)->buffers)
#endif

static struct wl_global *effect_global;
static struct wl_list blurs = { 0 };
static struct wl_list watches = { 0 };
static struct wl_listener new_surface_listener;
static int logged_blurs;
static bool pixman_blocked;
static const struct wlr_addon_interface watch_addon_impl;

#ifdef HAVE_SCENEFX
static void
fx_drop(struct elavo_blur *blur)
{
	if (blur->fx) {
		wl_list_remove(&blur->fx_destroy.link);
		wlr_scene_node_destroy(&blur->fx->node);
		blur->fx = NULL;
	}
	pixman_region32_clear(&blur->fx_region);
}

static void
fx_hide(struct elavo_blur *blur)
{
	if (blur->fx) {
		wlr_scene_node_set_enabled(&blur->fx->node, false);
	}
}
#else
#define fx_drop(blur) ((void)(blur))
#define fx_hide(blur) ((void)(blur))
#endif

#ifndef HAVE_SCENEFX
uint64_t
elavo_blur_surface_commits(struct wlr_surface *surface)
{
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, &watches,
		&watch_addon_impl);
	if (!addon) {
		return 0;
	}
	struct surface_watch *watch = wl_container_of(addon, watch, addon);
	return watch->commits;
}
#endif

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

/* Also frees the buffers; showing it again recomputes. */
static void
hide_node(struct elavo_blur *blur)
{
	if (blur->node) {
		wlr_scene_node_set_enabled(&blur->node->node, false);
		wlr_scene_buffer_set_buffer(blur->node, NULL);
	}
	fx_hide(blur);
	blur_buffers_finish(blur);
	blur->signature = 0;
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
	fx_drop(blur);
#ifdef HAVE_SCENEFX
	pixman_region32_fini(&blur->fx_region);
#endif
	pixman_region32_fini(&blur->pending);
	pixman_region32_fini(&blur->current);
	if (blur->tree_listening) {
		wl_list_remove(&blur->tree_destroy.link);
		blur->tree_listening = false;
	}
	if (blur->node) {
		wl_list_remove(&blur->node_destroy.link);
		wlr_scene_node_destroy(&blur->node->node);
	}
	blur_buffers_finish(blur);
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
		blur->dirty = true;
	}
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
	/* The protocol makes the object inert once its surface is gone. */
	if (!blur->surface) {
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
#ifdef HAVE_SCENEFX
	pixman_region32_init(&blur->fx_region);
#endif
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

#ifndef HAVE_SCENEFX
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
#endif

/* Userspace addresses are canonical. A freed scene node's parent is often not. */
static bool
ptr_ok(const void *ptr)
{
	uintptr_t value = (uintptr_t)ptr;
	uintptr_t high = value >> 47;
	if (high != 0 && high != 0x1ffffULL) {
		return false;
	}
	return value >= 4096;
}

/*
 * Same walk as wlr_scene_node_coords, but a bad parent stops the walk
 * instead of faulting. Depth is bounded so a cycle cannot hang the frame.
 */
static bool
coords_ok(struct wlr_scene_node *node, int *lx, int *ly)
{
	int x = 0;
	int y = 0;
	bool enabled = true;
	for (int i = 0; i < 64; i++) {
		if (!ptr_ok(node)) {
			return false;
		}
		x += node->x;
		y += node->y;
		enabled = enabled && node->enabled;
		if (!node->parent) {
			*lx = x;
			*ly = y;
			return enabled;
		}
		if (!ptr_ok(node->parent)) {
			return false;
		}
		node = &node->parent->node;
	}
	return false;
}

static bool
blur_on_output(struct wlr_scene_node *node, const pixman_region32_t *region,
		struct output *output, int *lx, int *ly)
{
	if (!coords_ok(node, lx, ly)) {
		return false;
	}
	/*
	 * Test the blurred area, not the surface origin. A GTK popup's
	 * surface starts at its shadow's outer edge, which can be off the
	 * output while the menu itself is on it.
	 */
	pixman_box32_t *ext = pixman_region32_extents((pixman_region32_t *)region);
	int x1 = *lx + ext->x1;
	int y1 = *ly + ext->y1;
	int x2 = *lx + (ext->x2 > ext->x1 ? ext->x2 : ext->x1 + 1);
	int y2 = *ly + (ext->y2 > ext->y1 ? ext->y2 : ext->y1 + 1);
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &box);
	return x1 < box.x + box.width && x2 > box.x
		&& y1 < box.y + box.height && y2 > box.y;
}

#ifndef HAVE_SCENEFX
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

#endif
#ifndef HAVE_SCENEFX
static bool
backdrop_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}
#endif

#ifndef HAVE_SCENEFX
static void
blur_handle_node_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct elavo_blur *blur = wl_container_of(listener, blur, node_destroy);
	wl_list_remove(&blur->node_destroy.link);
	blur->node = NULL;
	blur->signature = 0;
}
#endif

static void
blur_handle_tree_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct elavo_blur *blur = wl_container_of(listener, blur, tree_destroy);
	wl_list_remove(&blur->tree_destroy.link);
	blur->tree_listening = false;
	/*
	 * The tree destroy frees our backdrop node with the rest of its
	 * children. Drop the pointer; destroying it here would free it twice.
	 */
	if (blur->node) {
		wl_list_remove(&blur->node_destroy.link);
		blur->node = NULL;
	}
#ifdef HAVE_SCENEFX
	if (blur->fx) {
		wl_list_remove(&blur->fx_destroy.link);
		blur->fx = NULL;
	}
	pixman_region32_clear(&blur->fx_region);
#endif
	blur->dead = blur->tree;
	blur->tree = NULL;
	blur->signature = 0;
	blur_buffers_finish(blur);
}

static void
forget_tree(struct elavo_blur *blur)
{
	if (blur->tree_listening) {
		wl_list_remove(&blur->tree_destroy.link);
		blur->tree_listening = false;
	}
	blur->tree = NULL;
}

/*
 * A popup's surface->data is only set while labwc has a scene tree for
 * it, and labwc frees that tree after the surface unmaps. A menu that
 * closes and opens again keeps its wl_surface and its blur, so the
 * pointer is read only while the surface is mapped.
 */
static struct wlr_scene_tree *
blur_tree(struct elavo_blur *blur)
{
	if (!blur->surface || !blur->surface->mapped) {
		return NULL;
	}
	if (blur->tree) {
		if (!ptr_ok(blur->tree) || blur->tree->node.type != WLR_SCENE_NODE_TREE
				|| !ptr_ok(blur->tree->node.parent) || !blur->tree->node.enabled) {
			return NULL;
		}
		return blur->tree;
	}
	void *data = blur->surface ? blur->surface->data : NULL;
	if (!ptr_ok(data) || data == (void *)blur->dead) {
		return NULL;
	}
	struct wlr_scene_tree *tree = data;
	if (tree->node.type != WLR_SCENE_NODE_TREE || !ptr_ok(tree->node.parent)
			|| !tree->node.enabled) {
		return NULL;
	}
	blur->tree = tree;
	blur->dead = NULL;
	blur->tree_destroy.notify = blur_handle_tree_destroy;
	wl_signal_add(&tree->node.events.destroy, &blur->tree_destroy);
	blur->tree_listening = true;
	return tree;
}

/*
 * The scene buffer that shows `surface`, under `node`. An xdg popup's tree
 * is placed at its window geometry, and the surface sits at minus that
 * geometry inside it, so the blur region (surface-local) is placed relative
 * to this buffer, not to the tree.
 */
static struct wlr_scene_buffer *
surface_buffer(struct wlr_scene_node *node, struct wlr_surface *surface, int depth)
{
	if (depth > 16 || !ptr_ok(node)) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(buffer);
		if (scene_surface && scene_surface->surface == surface) {
			return buffer;
		}
		return NULL;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_buffer *found = surface_buffer(child, surface, depth + 1);
		if (found) {
			return found;
		}
	}
	return NULL;
}

#ifdef HAVE_SCENEFX
static void
handle_fx_destroy(struct wl_listener *listener, void *data)
{
	struct elavo_blur *blur = wl_container_of(listener, blur, fx_destroy);
	wl_list_remove(&blur->fx_destroy.link);
	blur->fx = NULL;
	pixman_region32_clear(&blur->fx_region);
}

/*
 * Clients send a rounded rectangle as one rect per corner row plus the
 * middle (elavo-common's add_round_rect does). Recognise that shape so it
 * becomes one SceneFX node with an antialiased radius, not a staircase of
 * thin nodes. The radius is the first row's inset, or the run of inset rows
 * when that is taller.
 */
static bool
region_round_rect(pixman_region32_t *region, int *radius)
{
	int n = 0;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &n);
	pixman_box32_t *ext = pixman_region32_extents(region);
	*radius = 0;
	if (n <= 1) {
		return n == 1;
	}
	int y = ext->y1;
	int top_rows = 0;
	bool in_top = true;
	for (int i = 0; i < n; i++) {
		if (rects[i].y1 != y) {
			return false; /* a gap, or two rects in one band */
		}
		y = rects[i].y2;
		int left = rects[i].x1 - ext->x1;
		int right = ext->x2 - rects[i].x2;
		if (abs(left - right) > 1) {
			return false;
		}
		if (in_top && left > 0) {
			top_rows += rects[i].y2 - rects[i].y1;
		} else {
			in_top = false;
		}
	}
	if (y != ext->y2) {
		return false;
	}
	int first = rects[0].x1 - ext->x1;
	*radius = MAX(first, top_rows);
	int max = MIN(ext->x2 - ext->x1, ext->y2 - ext->y1) / 2;
	*radius = MIN(*radius, max);
	return true;
}

static void
fx_add_node(struct elavo_blur *blur, int x, int y, int w, int h, int radius)
{
	struct wlr_scene_blur *node = wlr_scene_blur_create(blur->fx, w, h);
	if (!node) {
		return;
	}
	wlr_scene_node_set_position(&node->node, x, y);
	if (radius > 0) {
		wlr_scene_blur_set_corner_radius(node, radius);
	}
}

static void
fx_update(struct elavo_blur *blur, struct wlr_scene_tree *host,
		struct wlr_scene_buffer *shown)
{
	if (!blur->fx) {
		blur->fx = wlr_scene_tree_create(host);
		if (!blur->fx) {
			return;
		}
		blur->fx_destroy.notify = handle_fx_destroy;
		wl_signal_add(&blur->fx->node.events.destroy, &blur->fx_destroy);
	}
	if (blur->fx->node.parent != host) {
		wlr_scene_node_reparent(&blur->fx->node, host);
	}
	/* Below the surface: SceneFX blurs what is drawn before the node. */
	wlr_scene_node_lower_to_bottom(&blur->fx->node);
	/* The region is surface-local; the buffer sits at the surface origin. */
	wlr_scene_node_set_position(&blur->fx->node, shown->node.x, shown->node.y);

	/* Rebuild only when the shape changes: clients resend the same region
	 * on many commits. */
	if (!pixman_region32_equal(&blur->fx_region, &blur->current)
			|| wl_list_empty(&blur->fx->children)) {
		blur->dirty = false;
		pixman_region32_copy(&blur->fx_region, &blur->current);
		struct wlr_scene_node *child, *tmp;
		wl_list_for_each_safe(child, tmp, &blur->fx->children, link) {
			wlr_scene_node_destroy(child);
		}
		int radius = 0;
		pixman_box32_t *ext = pixman_region32_extents(&blur->current);
		if (region_round_rect(&blur->current, &radius)) {
			fx_add_node(blur, ext->x1, ext->y1, ext->x2 - ext->x1,
				ext->y2 - ext->y1, radius);
		} else {
			int n = 0;
			pixman_box32_t *rects =
				pixman_region32_rectangles(&blur->current, &n);
			for (int i = 0; i < n; i++) {
				fx_add_node(blur, rects[i].x1, rects[i].y1,
					rects[i].x2 - rects[i].x1,
					rects[i].y2 - rects[i].y1, 0);
			}
		}
		if (logged_blurs < 8) {
			wlr_log(WLR_INFO, "elavo blur: %d SceneFX node(s), "
				"%dx%d radius %d",
				wl_list_length(&blur->fx->children),
				ext->x2 - ext->x1, ext->y2 - ext->y1, radius);
			logged_blurs++;
		}
	}
	wlr_scene_node_set_enabled(&blur->fx->node, true);
}

/* window.blur.strength (1 to 5) is the number of SceneFX blur passes. */
void
elavo_mods_fx_blur_params(void)
{
	if (!server.scene) {
		return;
	}
	struct blur_data data = blur_data_get_default();
	wlr_scene_set_blur_data(server.scene, elavo_mods_blur_levels(),
		data.radius, data.noise, data.brightness, data.contrast,
		data.saturation);
}
#endif

static void
update_blur(struct elavo_blur *blur, struct output *output)
{
	struct wlr_scene_tree *tree = blur_tree(blur);
	if (!tree) {
		/*
		 * The tree destroy listener has not fired, so the tree and
		 * the backdrop in it are still alive. Hide the backdrop and
		 * let go of the tree; it is read again once the surface maps.
		 */
		if ((blur->node
#ifdef HAVE_SCENEFX
				|| blur->fx
#endif
				) && blur->tree_listening) {
			hide_node(blur);
		}
		if (blur->surface && !blur->surface->mapped) {
			if (blur->node) {
				wl_list_remove(&blur->node_destroy.link);
				wlr_scene_node_destroy(&blur->node->node);
				blur->node = NULL;
			}
			fx_drop(blur);
			forget_tree(blur);
		}
		blur->signature = 0;
		return;
	}
	struct wlr_scene_buffer *shown = surface_buffer(&tree->node, blur->surface, 0);
	if (!shown || !shown->node.parent) {
		return;
	}
	struct wlr_scene_tree *host = shown->node.parent;
	int lx = 0;
	int ly = 0;
	if (!blur_on_output(&shown->node, &blur->current, output, &lx, &ly)) {
		return;
	}
	if (!blur->active || !elavo_mods_blur_enabled() || pixman_blocked) {
		hide_node(blur);
		return;
	}

	pixman_box32_t *ext = pixman_region32_extents(&blur->current);
	int bw = ext->x2 - ext->x1;
	int bh = ext->y2 - ext->y1;
	if (bw < 1 || bh < 1) {
		hide_node(blur);
		return;
	}
#ifdef HAVE_SCENEFX
	fx_update(blur, host, shown);
	return;
#endif

#ifndef HAVE_SCENEFX
	uint64_t signature = elavo_blur_signature(&tree->node,
		lx + ext->x1, ly + ext->y1, bw, bh);
	if (!blur->dirty && blur->signature == signature) {
		return;
	}
	blur->dirty = false;
	blur->signature = signature;

	int scale = (int)lround(output->wlr_output->scale);
	if (scale < 1) {
		scale = 1;
	}
	pixman_region32_t shape;
	shape_in_buffer(&shape, &blur->current, ext->x1, ext->y1, scale);

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct wlr_buffer *buffer = elavo_blur_compose(&blur->buffers, output,
		&tree->node, lx + ext->x1, ly + ext->y1, bw, bh, &shape);
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
	 * The backdrop is the bottom child of the tree that holds the
	 * surface's buffer, inside the popup or layer surface's own tree,
	 * not a sibling of it. labwc walks each layer tree's children as
	 * layer surfaces, and a foreign sibling there aborts the next arrange.
	 */
	if (!blur->node) {
		blur->node = wlr_scene_buffer_create(host, NULL);
		if (!blur->node) {
			return;
		}
		blur->node->point_accepts_input = backdrop_accepts_input;
		blur->node_destroy.notify = blur_handle_node_destroy;
		wl_signal_add(&blur->node->node.events.destroy, &blur->node_destroy);
	}
	if (blur->node->node.parent != host) {
		wlr_scene_node_reparent(&blur->node->node, host);
	}
	wlr_scene_node_lower_to_bottom(&blur->node->node);
	wlr_scene_node_set_position(&blur->node->node,
		shown->node.x + ext->x1, shown->node.y + ext->y1);
	wlr_scene_buffer_set_dest_size(blur->node, bw, bh);
	wlr_scene_buffer_set_buffer(blur->node, buffer);
	wlr_scene_node_set_enabled(&blur->node->node, true);
#endif
}

static void
any_surface_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct surface_watch *watch = wl_container_of(listener, watch, commit);
	watch->commits++;
}

static void
free_watch(struct surface_watch *watch)
{
	wlr_addon_finish(&watch->addon);
	wl_list_remove(&watch->commit.link);
	wl_list_remove(&watch->destroy.link);
	wl_list_remove(&watch->link);
	free(watch);
}

static void
watch_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct surface_watch *watch = wl_container_of(listener, watch, destroy);
	free_watch(watch);
}

static void
watch_addon_destroy(struct wlr_addon *addon)
{
	struct surface_watch *watch = wl_container_of(addon, watch, addon);
	free_watch(watch);
}

static const struct wlr_addon_interface watch_addon_impl = {
	.name = "elavo-blur-watch",
	.destroy = watch_addon_destroy,
};

static void
handle_new_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_surface *surface = data;
	struct surface_watch *watch = znew(*watch);
	watch->surface = surface;
	wlr_addon_init(&watch->addon, &surface->addons, &watches, &watch_addon_impl);
	watch->commit.notify = any_surface_commit;
	wl_signal_add(&surface->events.commit, &watch->commit);
	watch->destroy.notify = watch_handle_destroy;
	wl_signal_add(&surface->events.destroy, &watch->destroy);
	wl_list_insert(&watches, &watch->link);
}

void
elavo_mods_output_frame(struct output *output)
{
	elavo_mods_shadows_frame();
	elavo_mods_clip_frame();
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
#ifdef HAVE_SCENEFX
	elavo_mods_fx_blur_params();
#else
	pixman_blocked = wlr_renderer_is_pixman(server.renderer)
		&& !getenv("ELAVO_BLUR_PIXMAN");
	if (pixman_blocked) {
		wlr_log(WLR_INFO, "elavo blur off on the pixman renderer");
	}
#endif
	effect_global = wl_global_create(server.wl_display,
		&ext_background_effect_manager_v1_interface, 1, NULL, manager_bind);
	if (!effect_global) {
		wlr_log(WLR_ERROR, "failed to advertise ext_background_effect_manager_v1");
	}
	elavo_mods_window_buttons_init();
}

void
elavo_mods_finish(void)
{
	elavo_mods_window_buttons_finish();
	elavo_mods_shadows_finish();
	elavo_mods_clip_finish();
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
		free_watch(watch);
	}
	wl_list_remove(&new_surface_listener.link);
	if (effect_global) {
		wl_global_destroy(effect_global);
		effect_global = NULL;
	}
}
