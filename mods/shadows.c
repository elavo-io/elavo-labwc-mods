/* SPDX-License-Identifier: GPL-2.0-only */
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

/*
 * Drop shadows on maximized and tiled windows. Every shadow is built from
 * the buffers labwc renders from the theme's window.*.shadow keys, so the
 * size and color live only in themerc.
 *
 * Windows labwc decorates keep labwc's own shadow; elavo_mods_ssd_shadows()
 * decides when it shows. Windows that draw their own decorations have no
 * labwc shadow, so one is added here while they are maximized. Tiled ones
 * are left alone: GTK and libadwaita still draw their own shadow there.
 *
 * With SceneFX (window.fx.shadows, on by default) the same rules pick the
 * windows, and each gets one SceneFX shadow node instead of eight buffer
 * pieces: a gaussian box shadow with the window's corner radius, cut out
 * under the window so a translucent window does not show it. Size and color
 * still come from window.*.shadow.
 */

struct csd_shadow {
	struct view *view;
	struct wlr_scene_tree *tree;
	struct ssd_shadow_subtree subtrees[2]; /* indexed by enum ssd_active_state */
	struct wl_listener activated;
	struct wl_listener destroy;
	struct wl_list link;
};

/* Initialized here because the theme can load before elavo_mods_init(). */
static struct wl_list shadows = { &shadows, &shadows };
static bool rebuild_pending;

static bool
ssd_shadow_rule(struct view *view)
{
	if (!rc.shadows_enabled || view->fullscreen) {
		return false;
	}
	if (view->maximized == VIEW_AXIS_BOTH) {
		return elavo_mods_shadows_on_maximized();
	}
	/*
	 * labwc also requires a gap as wide as the shadow, so tiled shadows
	 * never overlap a neighbor. Elavo has no gap and wants them anyway.
	 */
	if (view_is_tiled(view)) {
		return rc.shadows_on_tiled;
	}
	return true;
}

bool
elavo_mods_ssd_shadows(struct ssd *ssd)
{
	/* The SceneFX node replaces labwc's shadow pieces. */
	if (elavo_mods_fx_shadows()) {
		return false;
	}
	return ssd_shadow_rule(ssd->view);
}

/*
 * Redraw one corner piece as the shadow of a corner with the theme's
 * radius. The piece is drawn for the bottom-right corner, starting inset
 * pixels inside the window on both axes, as labwc draws it. labwc's falloff
 * is kept but measured from the curve, so at a radius equal to the inset
 * this matches labwc's gradient and still meets the edge pieces. Inside the
 * curve is erased, so the shadow does not show through a window's corner.
 */
static void
draw_rounded_corner(struct lab_data_buffer *buffer, int visible_size,
		const float color[4], int radius)
{
	if (!buffer) {
		return;
	}
	int total = buffer->logical_height;
	int inset = total - visible_size;
	double variance = 0.3 * 0.3;
	double centre = inset - radius;

	uint8_t *pixels = buffer->data;
	for (int y = 0; y < total; y++) {
		uint8_t *row = &pixels[y * buffer->stride];
		for (int x = 0; x < total; x++) {
			double dx = x - centre;
			double dy = y - centre;
			/* Distance outside the window's rounded corner */
			double d = dx > 0 && dy > 0
				? hypot(dx, dy) - radius
				: MAX(x - inset, y - inset);
			double coverage = fmin(fmax(d + 1.0, 0.0), 1.0);
			double n = (d + inset) / total;
			double alpha = exp(-(n * n) / variance) * coverage;

			/* Pre-multiplied, in the byte order labwc uses */
			row[4 * x] = color[2] * alpha * 255;
			row[4 * x + 1] = color[1] * alpha * 255;
			row[4 * x + 2] = color[0] * alpha * 255;
			row[4 * x + 3] = color[3] * alpha * 255;
		}
	}
}

void
elavo_mods_shadows_drawn(struct theme *theme)
{
	enum ssd_active_state state;
	FOR_EACH_ACTIVE_STATE(state) {
		int size = theme->window[state].shadow_size;
		if (size == 0) {
			continue;
		}
		draw_rounded_corner(theme->window[state].shadow_corner_top,
			size, theme->window[state].shadow_color, rc.corner_radius);
		draw_rounded_corner(theme->window[state].shadow_corner_bottom,
			size, theme->window[state].shadow_color, rc.corner_radius);
	}
}

static bool
never_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

/* Kept in step with corner_scale_crop() in src/ssd/ssd-shadow.c. */
static void
corner_scale_crop(struct wlr_scene_buffer *buffer, int horizontal_overlap,
		int vertical_overlap, int corner_size)
{
	int width = MAX(corner_size - horizontal_overlap, 0);
	int height = MAX(corner_size - vertical_overlap, 0);
	struct wlr_fbox src_box = {
		.x = horizontal_overlap,
		.y = vertical_overlap,
		.width = width,
		.height = height,
	};
	wlr_scene_buffer_set_source_box(buffer, &src_box);
	wlr_scene_buffer_set_dest_size(buffer, width, height);
}

/*
 * Kept in step with set_shadow_parts_geometry() in src/ssd/ssd-shadow.c,
 * with no title bar: the client's own title bar is inside its geometry.
 */
static void
place_parts(struct ssd_shadow_subtree *subtree, int width, int height,
		int corner_size, int inset, int visible_shadow_width)
{
	bool show_topbottom = width > inset * 2;
	bool show_sides = height > inset * 2;
	int horizontal_overlap = MAX(0, inset - width / 2);
	int vertical_overlap = MAX(0, inset - height / 2);

	int horizontal_overlap_downsized = horizontal_overlap;
	if (horizontal_overlap > 0) {
		horizontal_overlap_downsized -= width % 2;
	}
	int vertical_overlap_downsized = vertical_overlap;
	if (vertical_overlap > 0) {
		vertical_overlap_downsized -= height % 2;
	}

	wlr_scene_node_set_position(&subtree->bottom_right->node,
		width - inset + horizontal_overlap_downsized,
		height - inset + vertical_overlap_downsized);
	corner_scale_crop(subtree->bottom_right, horizontal_overlap_downsized,
		vertical_overlap_downsized, corner_size);

	wlr_scene_node_set_position(&subtree->bottom_left->node,
		-visible_shadow_width, height - inset + vertical_overlap);
	corner_scale_crop(subtree->bottom_left, horizontal_overlap,
		vertical_overlap, corner_size);

	wlr_scene_node_set_position(&subtree->top_left->node,
		-visible_shadow_width, -visible_shadow_width);
	corner_scale_crop(subtree->top_left, horizontal_overlap_downsized,
		vertical_overlap_downsized, corner_size);

	wlr_scene_node_set_position(&subtree->top_right->node,
		width - inset + horizontal_overlap, -visible_shadow_width);
	corner_scale_crop(subtree->top_right, horizontal_overlap,
		vertical_overlap, corner_size);

	wlr_scene_node_set_position(&subtree->right->node, width, inset);
	wlr_scene_buffer_set_dest_size(subtree->right,
		visible_shadow_width, MAX(height - 2 * inset, 0));
	wlr_scene_node_set_enabled(&subtree->right->node, show_sides);

	wlr_scene_node_set_position(&subtree->bottom->node, inset, height);
	wlr_scene_buffer_set_dest_size(subtree->bottom,
		MAX(width - 2 * inset, 0), visible_shadow_width);
	wlr_scene_node_set_enabled(&subtree->bottom->node, show_topbottom);

	wlr_scene_node_set_position(&subtree->left->node,
		-visible_shadow_width, inset);
	wlr_scene_buffer_set_dest_size(subtree->left,
		visible_shadow_width, MAX(height - 2 * inset, 0));
	wlr_scene_node_set_enabled(&subtree->left->node, show_sides);

	wlr_scene_node_set_position(&subtree->top->node,
		inset, -visible_shadow_width);
	wlr_scene_buffer_set_dest_size(subtree->top,
		MAX(width - 2 * inset, 0), visible_shadow_width);
	wlr_scene_node_set_enabled(&subtree->top->node, show_topbottom);
}

static struct wlr_scene_buffer *
make_part(struct wlr_scene_tree *parent, struct wlr_buffer *buf,
		enum wl_output_transform tx)
{
	struct wlr_scene_buffer *part = lab_wlr_scene_buffer_create(parent, buf);
	wlr_scene_buffer_set_transform(part, tx);
	part->point_accepts_input = never_accepts_input;
	/* labwc does the same: pixman blurs one-pixel buffers when scaling. */
	part->filter_mode = WLR_SCALE_FILTER_NEAREST;
	return part;
}

static void
set_active(struct csd_shadow *shadow, bool active)
{
	if (!shadow->tree) {
		return;
	}
	enum ssd_active_state state;
	FOR_EACH_ACTIVE_STATE(state) {
		if (shadow->subtrees[state].tree) {
			wlr_scene_node_set_enabled(
				&shadow->subtrees[state].tree->node,
				active == state);
		}
	}
}

static void
build(struct csd_shadow *shadow)
{
	struct theme *theme = rc.theme;
	shadow->tree = lab_wlr_scene_tree_create(shadow->view->scene_tree);
	wlr_scene_node_lower_to_bottom(&shadow->tree->node);

	enum ssd_active_state state;
	FOR_EACH_ACTIVE_STATE(state) {
		struct ssd_shadow_subtree *subtree = &shadow->subtrees[state];
		*subtree = (struct ssd_shadow_subtree){0};
		if (theme->window[state].shadow_size == 0) {
			continue;
		}
		subtree->tree = lab_wlr_scene_tree_create(shadow->tree);
		struct wlr_scene_tree *parent = subtree->tree;
		struct wlr_buffer *corner_top =
			&theme->window[state].shadow_corner_top->base;
		struct wlr_buffer *corner_bottom =
			&theme->window[state].shadow_corner_bottom->base;
		struct wlr_buffer *edge = &theme->window[state].shadow_edge->base;

		subtree->bottom_right = make_part(parent, corner_bottom,
			WL_OUTPUT_TRANSFORM_NORMAL);
		subtree->bottom_left = make_part(parent, corner_bottom,
			WL_OUTPUT_TRANSFORM_FLIPPED);
		subtree->top_left = make_part(parent, corner_top,
			WL_OUTPUT_TRANSFORM_180);
		subtree->top_right = make_part(parent, corner_top,
			WL_OUTPUT_TRANSFORM_FLIPPED_180);
		subtree->right = make_part(parent, edge, WL_OUTPUT_TRANSFORM_NORMAL);
		subtree->bottom = make_part(parent, edge, WL_OUTPUT_TRANSFORM_90);
		subtree->left = make_part(parent, edge, WL_OUTPUT_TRANSFORM_180);
		subtree->top = make_part(parent, edge, WL_OUTPUT_TRANSFORM_270);
	}
	set_active(shadow, shadow->view == server.active_view);
}

static void
drop_tree(struct csd_shadow *shadow)
{
	if (shadow->tree) {
		wlr_scene_node_destroy(&shadow->tree->node);
		shadow->tree = NULL;
	}
}

static bool
wants_shadow(struct view *view)
{
	if (!rc.shadows_enabled || view->ssd || view->fullscreen) {
		return false;
	}
	bool maximized = view->maximized == VIEW_AXIS_BOTH;
	/* clip-corners.c cut the client's own shadow off; draw labwc's. */
	if (elavo_mods_clip_csd(view)) {
		return !maximized || elavo_mods_shadows_on_maximized();
	}
	return elavo_mods_shadows_on_maximized() && maximized;
}

static void
sync(struct csd_shadow *shadow)
{
	struct view *view = shadow->view;
	bool show = wants_shadow(view);
	if (show && !shadow->tree) {
		build(shadow);
	}
	if (!shadow->tree) {
		return;
	}
	wlr_scene_node_set_enabled(&shadow->tree->node, show);
	if (!show) {
		return;
	}

	struct theme *theme = rc.theme;
	enum ssd_active_state state;
	FOR_EACH_ACTIVE_STATE(state) {
		struct ssd_shadow_subtree *subtree = &shadow->subtrees[state];
		if (!subtree->tree) {
			continue;
		}
		int visible = theme->window[state].shadow_size;
		int inset = SSD_SHADOW_INSET * (double)visible;
		int corner_size =
			theme->window[state].shadow_corner_top->logical_height;
		place_parts(subtree, view->current.width, view->current.height,
			corner_size, inset, visible);
	}
}

static void
destroy_shadow(struct csd_shadow *shadow)
{
	wl_list_remove(&shadow->activated.link);
	wl_list_remove(&shadow->destroy.link);
	wl_list_remove(&shadow->link);
	drop_tree(shadow);
	free(shadow);
}

static void
handle_activated(struct wl_listener *listener, void *data)
{
	struct csd_shadow *shadow = wl_container_of(listener, shadow, activated);
	bool *activated = data;
	set_active(shadow, *activated);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct csd_shadow *shadow = wl_container_of(listener, shadow, destroy);
	destroy_shadow(shadow);
}

static struct csd_shadow *
find_shadow(struct view *view)
{
	struct csd_shadow *shadow;
	wl_list_for_each(shadow, &shadows, link) {
		if (shadow->view == view) {
			return shadow;
		}
	}
	return NULL;
}

void
elavo_mods_view_moved(struct view *view)
{
	struct csd_shadow *shadow = find_shadow(view);
	if (elavo_mods_fx_shadows()) {
		if (shadow) {
			destroy_shadow(shadow);
		}
		return;
	}
	if (!shadow) {
		if (!wants_shadow(view)) {
			return;
		}
		shadow = znew(*shadow);
		shadow->view = view;
		shadow->activated.notify = handle_activated;
		wl_signal_add(&view->events.activated, &shadow->activated);
		shadow->destroy.notify = handle_destroy;
		wl_signal_add(&view->events.destroy, &shadow->destroy);
		wl_list_insert(&shadows, &shadow->link);
	}
	sync(shadow);
}

#ifdef HAVE_SCENEFX
struct fx_shadow {
	struct view *view;
	struct wlr_scene_shadow *node;
	struct wl_listener node_destroy;
	struct wl_listener view_destroy;
	struct wl_list link;
};

static struct wl_list fx_shadows = { &fx_shadows, &fx_shadows };

static void
destroy_fx_shadow(struct fx_shadow *shadow)
{
	wl_list_remove(&shadow->view_destroy.link);
	wl_list_remove(&shadow->link);
	if (shadow->node) {
		wl_list_remove(&shadow->node_destroy.link);
		wlr_scene_node_destroy(&shadow->node->node);
	}
	free(shadow);
}

static void
handle_fx_view_destroy(struct wl_listener *listener, void *data)
{
	struct fx_shadow *shadow = wl_container_of(listener, shadow, view_destroy);
	destroy_fx_shadow(shadow);
}

static void
handle_fx_node_destroy(struct wl_listener *listener, void *data)
{
	struct fx_shadow *shadow = wl_container_of(listener, shadow, node_destroy);
	wl_list_remove(&shadow->node_destroy.link);
	shadow->node = NULL;
}

/* The box the shadow is cast by, relative to view->scene_tree. */
static bool
fx_shadow_frame(struct view *view, struct wlr_box *frame)
{
	if (!view->mapped || !view->scene_tree || view->fullscreen) {
		return false;
	}
	int w = view->current.width;
	int h = view_effective_height(view, /* use_pending */ false);
	struct ssd *ssd = view->ssd;
	if (ssd && ssd->tree && ssd->tree->node.enabled) {
		if (!ssd_shadow_rule(view)) {
			return false;
		}
		struct border t = ssd_thickness(view);
		*frame = (struct wlr_box){ -t.left, -t.top,
			w + t.left + t.right, h + t.top + t.bottom };
	} else {
		if (!wants_shadow(view)) {
			return false;
		}
		*frame = (struct wlr_box){ 0, 0, w, h };
	}
	return frame->width > 0 && frame->height > 0;
}

static struct fx_shadow *
find_fx_shadow(struct view *view)
{
	struct fx_shadow *shadow;
	wl_list_for_each(shadow, &fx_shadows, link) {
		if (shadow->view == view) {
			return shadow;
		}
	}
	return NULL;
}

static void
sync_fx_shadow(struct view *view)
{
	struct fx_shadow *shadow = find_fx_shadow(view);
	struct wlr_box frame;
	bool show = elavo_mods_fx_shadows() && fx_shadow_frame(view, &frame);
	enum ssd_active_state state = view == server.active_view
		? SSD_ACTIVE : SSD_INACTIVE;
	int size = show ? rc.theme->window[state].shadow_size : 0;
	if (size <= 0) {
		if (shadow && shadow->node) {
			wlr_scene_node_set_enabled(&shadow->node->node, false);
		}
		return;
	}
	if (!shadow) {
		shadow = znew(*shadow);
		shadow->view = view;
		shadow->view_destroy.notify = handle_fx_view_destroy;
		wl_signal_add(&view->events.destroy, &shadow->view_destroy);
		wl_list_insert(&fx_shadows, &shadow->link);
	}
	const float *color = rc.theme->window[state].shadow_color;
	int r = rc.corner_radius;
	if (!shadow->node) {
		shadow->node = wlr_scene_shadow_create(view->scene_tree,
			1, 1, r, size, color);
		if (!shadow->node) {
			return;
		}
		shadow->node_destroy.notify = handle_fx_node_destroy;
		wl_signal_add(&shadow->node->node.events.destroy,
			&shadow->node_destroy);
		wlr_scene_node_lower_to_bottom(&shadow->node->node);
	}
	/*
	 * A window labwc decorates has round bottom corners only while
	 * clip-corners.c rounds them. One that decorates itself rounds its
	 * own, or is clipped round.
	 */
	int bottom = !view->ssd || elavo_mods_clip_active(view) ? r : 0;
	struct fx_corner_radii corners = corner_radii_new(r, r, bottom, bottom);
	wlr_scene_shadow_set_size(shadow->node,
		frame.width + 2 * size, frame.height + 2 * size);
	wlr_scene_node_set_position(&shadow->node->node,
		frame.x - size, frame.y - size);
	wlr_scene_shadow_set_blur_sigma(shadow->node, size);
	wlr_scene_shadow_set_corner_radius(shadow->node, r);
	wlr_scene_shadow_set_color(shadow->node, color);
	wlr_scene_shadow_set_clipped_region(shadow->node, (struct clipped_region){
		.area = { size, size, frame.width, frame.height },
		.corners = corners,
	});
	wlr_scene_node_set_enabled(&shadow->node->node, true);
}

static void
fx_shadows_frame(void)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		sync_fx_shadow(view);
	}
}

static void
fx_shadows_finish(void)
{
	struct fx_shadow *shadow, *tmp;
	wl_list_for_each_safe(shadow, tmp, &fx_shadows, link) {
		destroy_fx_shadow(shadow);
	}
}
#endif

void
elavo_mods_shadows_theme_changed(void)
{
	/*
	 * Called before labwc renders the new theme's shadow buffers, so the
	 * old pieces are dropped now and rebuilt on the next frame.
	 */
	struct csd_shadow *shadow;
	wl_list_for_each(shadow, &shadows, link) {
		drop_tree(shadow);
	}
	rebuild_pending = true;
}

void
elavo_mods_shadows_frame(void)
{
#ifdef HAVE_SCENEFX
	fx_shadows_frame();
#endif
	if (!rebuild_pending) {
		return;
	}
	rebuild_pending = false;
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		elavo_mods_view_moved(view);
	}
}

void
elavo_mods_shadows_finish(void)
{
	struct csd_shadow *shadow, *tmp;
	wl_list_for_each_safe(shadow, tmp, &shadows, link) {
		destroy_shadow(shadow);
	}
#ifdef HAVE_SCENEFX
	fx_shadows_finish();
#endif
}
