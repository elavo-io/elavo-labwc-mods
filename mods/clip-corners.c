/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "buffer.h"
#include "common/graphic-helpers.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "theme.h"
#include "view.h"

/*
 * Round the bottom corners of windows labwc decorates, so the whole frame
 * follows window.cornerRadius and not only the title bar.
 *
 * With SceneFX (the default build), each buffer in the window's surface
 * tree that reaches a corner of the window gets that corner's radius, and
 * SceneFX's shader cuts it with an antialiased edge. Popups are separate
 * trees and are not touched. The bottom border becomes one rounded
 * rectangle with the client's area cut out of it, so the border follows the
 * curve. window.fx.corners: no switches back to the strips below.
 *
 * Without SceneFX, wlr_scene can only clip a surface tree to one rectangle.
 * So the client is drawn more than once: one copy clipped to the window
 * less its bottom radius rows, and one copy per run of rows in the corner
 * band, each clipped to the width the curve leaves at those rows. Every copy
 * points at the same client buffer, so this costs extra draw calls (about
 * radius / 2 per window), not memory. The edge of the curve is a one-pixel
 * staircase with no antialiasing; the border arc drawn over it hides most of
 * that.
 *
 * Maximized and tiled windows keep their corners too when the theme sets
 * window.maximizedCorners: yes. Fullscreen windows stay square: they have
 * no decorations, and their content is meant to fill the screen.
 *
 * Windows that draw their own decorations are left alone by default: they
 * round (or do not round) their own pixels, and their buffer usually holds
 * a shadow outside the window geometry that a clip would cut off. Apps named
 * in window.clipApps (app ids, space separated) are clipped anyway, on all
 * four corners of their window geometry, and get labwc's shadow in place of
 * the one the clip removes. That is for apps such as Telegram that draw a
 * square title bar of their own.
 *
 * Off unless the theme sets window.clipCorners: yes.
 */

#define MAX_STRIPS 64

struct strip {
	struct wlr_scene_tree *tree;
	struct wl_listener destroy;
};

struct piece {
	struct wlr_scene_buffer *buffer;
	struct wl_listener destroy;
};

struct clipped {
	struct view *view;
	struct wlr_scene_node *surface_tree;
	struct strip strips[MAX_STRIPS];
	int n_strips;
	int radius;
	struct piece pieces[2][2]; /* [enum ssd_active_state][left, right] */
	struct ssd *pieces_ssd;
	bool on;
	/* Corner radii and the border cut-out are set on SceneFX nodes. */
	bool fx_on;
	struct wl_listener view_destroy;
	struct wl_list link;
};

static struct wl_list clipped_views = { &clipped_views, &clipped_views };
static struct lab_data_buffer *corner_buffers[2];
static int corner_radius_drawn;

static void
drop_corner_buffers(void)
{
	for (int i = 0; i < 2; i++) {
		if (corner_buffers[i]) {
			wlr_buffer_drop(&corner_buffers[i]->base);
			corner_buffers[i] = NULL;
		}
	}
	corner_radius_drawn = 0;
}

/* The border arc for a bottom-right corner, its centre at (0, 0). */
static struct lab_data_buffer *
corner_buffer(enum ssd_active_state state, int radius)
{
	if (corner_radius_drawn != radius) {
		drop_corner_buffers();
		corner_radius_drawn = radius;
	}
	if (corner_buffers[state]) {
		return corner_buffers[state];
	}
	struct theme *theme = rc.theme;
	int bw = theme->border_width;
	struct lab_data_buffer *buffer = buffer_create_cairo(radius, radius, 1.0);
	if (!buffer) {
		return NULL;
	}
	cairo_t *cairo = cairo_create(buffer->surface);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
	if (bw > 0) {
		set_cairo_color(cairo, theme->window[state].border_color);
		cairo_set_line_width(cairo, bw);
		cairo_arc(cairo, 0, 0, radius - bw / 2.0, 0, M_PI / 2);
		cairo_stroke(cairo);
	}
	cairo_destroy(cairo);
	cairo_surface_flush(buffer->surface);
	corner_buffers[state] = buffer;
	return buffer;
}

static bool
never_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static void
handle_strip_destroy(struct wl_listener *listener, void *data)
{
	struct strip *strip = wl_container_of(listener, strip, destroy);
	wl_list_remove(&strip->destroy.link);
	strip->tree = NULL;
}

static void
handle_piece_destroy(struct wl_listener *listener, void *data)
{
	struct piece *piece = wl_container_of(listener, piece, destroy);
	wl_list_remove(&piece->destroy.link);
	piece->buffer = NULL;
}

static void
drop_strips(struct clipped *c)
{
	for (int i = 0; i < c->n_strips; i++) {
		struct strip *strip = &c->strips[i];
		if (strip->tree) {
			wl_list_remove(&strip->destroy.link);
			wlr_scene_node_destroy(&strip->tree->node);
			strip->tree = NULL;
		}
	}
	c->n_strips = 0;
	c->radius = 0;
}

static void
drop_pieces(struct clipped *c)
{
	for (int s = 0; s < 2; s++) {
		for (int side = 0; side < 2; side++) {
			struct piece *piece = &c->pieces[s][side];
			if (piece->buffer) {
				wl_list_remove(&piece->destroy.link);
				wlr_scene_node_destroy(&piece->buffer->node);
				piece->buffer = NULL;
			}
		}
	}
	c->pieces_ssd = NULL;
}

/* The tree wlr_scene clips: the subsurface tree of the root surface. */
static struct wlr_scene_node *
find_surface_tree(struct view *view)
{
	if (!view->content_tree) {
		return NULL;
	}
	if (view->type == LAB_XDG_SHELL_VIEW) {
		/* wlr_scene_xdg_surface_create() makes it the first child. */
		struct wl_list *children = &view->content_tree->children;
		if (wl_list_empty(children)) {
			return NULL;
		}
		struct wlr_scene_node *first =
			wl_container_of(children->next, first, link);
		return first;
	}
	return &view->content_tree->node;
}

static void
geometry_offset(struct view *view, int *x, int *y)
{
	*x = 0;
	*y = 0;
	if (view->type == LAB_XDG_SHELL_VIEW) {
		struct wlr_xdg_surface *xdg = xdg_surface_from_view(view);
		if (xdg) {
			*x = xdg->geometry.x;
			*y = xdg->geometry.y;
		}
	}
}

static bool
corners_allowed(struct view *view)
{
	int r = rc.corner_radius;
	return view->mapped && view->surface && view->content_tree
		&& !view->fullscreen && !view->shaded
		&& (elavo_mods_round_maximized()
			|| (view->maximized != VIEW_AXIS_BOTH
				&& !view_is_tiled(view)))
		&& view->current.width > 2 * r
		&& view->current.height > 2 * r;
}

/* An app that draws its own decorations, clipped because the theme names it. */
bool
elavo_mods_clip_csd(struct view *view)
{
	return elavo_mods_clip_corners() && !view->ssd
		&& view->type == LAB_XDG_SHELL_VIEW
		&& elavo_mods_clip_app(view->app_id)
		&& corners_allowed(view);
}

static bool
wants_clip(struct view *view)
{
	if (elavo_mods_clip_csd(view)) {
		return true;
	}
	struct ssd *ssd = view->ssd;
	int r = rc.corner_radius;
	return elavo_mods_clip_corners() && view->mapped && ssd
		&& ssd->tree && ssd->tree->node.enabled
		&& ssd->titlebar.height > 0 && ssd->border.tree
		&& view->surface && view->content_tree
		&& !view->fullscreen && !view->shaded
		&& (elavo_mods_round_maximized()
			|| (view->maximized != VIEW_AXIS_BOTH
				&& !view_is_tiled(view)
				&& !ssd->state.was_squared))
		&& r > rc.theme->border_width
		&& view->current.width > 2 * r
		&& view->current.height > 2 * r;
}

#ifdef HAVE_SCENEFX
struct radii_walk {
	struct wlr_box geo; /* window geometry, in the walk's coordinates */
	int radius;
	bool top; /* also round the top corners (a client-decorated window) */
};

static void
set_buffer_radii(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	struct radii_walk *walk = data;
	int w = buffer->dst_width;
	int h = buffer->dst_height;
	if ((w <= 0 || h <= 0) && buffer->buffer) {
		w = buffer->buffer->width;
		h = buffer->buffer->height;
	}
	struct fx_corner_radii radii = corner_radii_none();
	int r = walk->radius;
	if (r > 0 && w > 0 && h > 0) {
		/* A buffer rounds only the window corners it covers. */
		bool left = sx <= walk->geo.x && sx + w >= walk->geo.x + r;
		bool right = sx + w >= walk->geo.x + walk->geo.width
			&& sx <= walk->geo.x + walk->geo.width - r;
		bool top = walk->top && sy <= walk->geo.y
			&& sy + h >= walk->geo.y + r;
		bool bottom = sy + h >= walk->geo.y + walk->geo.height
			&& sy <= walk->geo.y + walk->geo.height - r;
		radii = corner_radii_new(top && left ? r : 0, top && right ? r : 0,
			bottom && right ? r : 0, bottom && left ? r : 0);
	}
	wlr_scene_buffer_set_corner_radii(buffer, radii);
}

static void
fx_reset_border(struct ssd *ssd)
{
	if (!ssd || !ssd->border.tree) {
		return;
	}
	enum ssd_active_state s;
	FOR_EACH_ACTIVE_STATE(s) {
		struct wlr_scene_rect *bottom = ssd->border.subtrees[s].bottom;
		if (bottom) {
			wlr_scene_rect_set_corner_radii(bottom, corner_radii_none());
			wlr_scene_rect_set_clipped_region(bottom,
				clipped_region_get_default());
		}
	}
}

static void
fx_off(struct clipped *c)
{
	if (!c->fx_on) {
		return;
	}
	c->fx_on = false;
	if (c->surface_tree) {
		struct radii_walk walk = { .radius = 0 };
		wlr_scene_node_for_each_buffer(c->surface_tree,
			set_buffer_radii, &walk);
	}
	fx_reset_border(c->view->ssd);
}

/*
 * The client keeps its geometry clip, and every buffer at a window corner
 * gets the radius. With the border showing, the client's radius is the
 * outer one less the border width, so the curves stay concentric; the
 * bottom border rect grows into a plate that runs around both corners with
 * the client's area cut out of it.
 */
static void
fx_sync(struct clipped *c, struct wlr_scene_node *surface_tree, bool csd,
		bool border_on, int r, int rc_, int gx, int gy, int w, int h)
{
	c->fx_on = true;
	struct wlr_box geometry = { gx, gy, w, h };
	wlr_scene_subsurface_tree_set_clip(surface_tree, &geometry);
	/* The walk counts offsets from surface_tree's parent. */
	struct radii_walk walk = {
		.geo = { surface_tree->x + gx, surface_tree->y + gy, w, h },
		.radius = rc_,
		.top = csd,
	};
	wlr_scene_node_for_each_buffer(surface_tree, set_buffer_radii, &walk);

	struct ssd *ssd = c->view->ssd;
	if (!border_on) {
		fx_reset_border(ssd);
		return;
	}
	struct theme *theme = rc.theme;
	int bw = theme->border_width;
	int full_width = w + 2 * bw;
	int corner_width = ssd_get_corner_width();
	enum ssd_active_state s;
	FOR_EACH_ACTIVE_STATE(s) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[s];
		wlr_scene_rect_set_size(subtree->left, bw, MAX(h - rc_, 0));
		wlr_scene_node_set_position(&subtree->left->node, 0, 0);
		wlr_scene_rect_set_size(subtree->right, bw, MAX(h - rc_, 0));
		wlr_scene_node_set_position(&subtree->right->node, bw + w, 0);
		wlr_scene_rect_set_size(subtree->bottom, full_width, rc_ + bw);
		wlr_scene_node_set_position(&subtree->bottom->node, 0, h - rc_);
		wlr_scene_rect_set_corner_radii(subtree->bottom,
			corner_radii_bottom(r));
		wlr_scene_rect_set_clipped_region(subtree->bottom,
			(struct clipped_region){
				.area = { bw, 0, w, rc_ },
				.corners = corner_radii_bottom(rc_),
			});
		/* A tiled window is laid out as a floating one; see below. */
		wlr_scene_rect_set_size(subtree->top,
			MAX(w - 2 * corner_width, 0), bw);
		wlr_scene_node_set_position(&subtree->top->node,
			bw + corner_width, -(ssd->titlebar.height + bw));
	}
}
#endif

static void
turn_off(struct clipped *c)
{
	if (!c->on) {
		return;
	}
	c->on = false;
#ifdef HAVE_SCENEFX
	fx_off(c);
#endif
	drop_strips(c);
	drop_pieces(c);
	if (c->surface_tree) {
		wlr_scene_subsurface_tree_set_clip(c->surface_tree, NULL);
		c->surface_tree = NULL;
	}
	if (c->view->ssd && c->view->ssd->border.tree) {
		ssd_border_update(c->view->ssd);
	}
}

static void
build_pieces(struct clipped *c, int r)
{
	struct ssd *ssd = c->view->ssd;
	enum ssd_active_state s;
	FOR_EACH_ACTIVE_STATE(s) {
		struct lab_data_buffer *buffer = corner_buffer(s, r);
		if (!buffer) {
			continue;
		}
		for (int side = 0; side < 2; side++) {
			struct piece *piece = &c->pieces[s][side];
			piece->buffer = lab_wlr_scene_buffer_create(
				ssd->border.subtrees[s].tree, &buffer->base);
			wlr_scene_buffer_set_transform(piece->buffer, side == 0
				? WL_OUTPUT_TRANSFORM_FLIPPED
				: WL_OUTPUT_TRANSFORM_NORMAL);
			piece->buffer->point_accepts_input = never_accepts_input;
			piece->destroy.notify = handle_piece_destroy;
			wl_signal_add(&piece->buffer->node.events.destroy,
				&piece->destroy);
		}
	}
	c->pieces_ssd = ssd;
}

static bool
place_strip(struct clipped *c, int n, struct wlr_scene_tree *parent,
		struct wlr_scene_node *surface_tree, struct wlr_box *box)
{
	struct strip *strip = &c->strips[n];
	if (!strip->tree) {
		strip->tree = wlr_scene_subsurface_tree_create(parent,
			c->view->surface);
		if (!strip->tree) {
			return false;
		}
		strip->destroy.notify = handle_strip_destroy;
		wl_signal_add(&strip->tree->node.events.destroy,
			&strip->destroy);
		/* Under the client's popups and over its own copy. */
		wlr_scene_node_place_above(&strip->tree->node, surface_tree);
	}
	wlr_scene_node_set_position(&strip->tree->node,
		surface_tree->x, surface_tree->y);
	wlr_scene_subsurface_tree_set_clip(&strip->tree->node, box);
	return true;
}

static void
sync_clip(struct clipped *c)
{
	struct view *view = c->view;
	if (!wants_clip(view)) {
		turn_off(c);
		return;
	}

	struct wlr_scene_node *surface_tree = find_surface_tree(view);
	if (!surface_tree) {
		turn_off(c);
		return;
	}
	struct theme *theme = rc.theme;
	struct ssd *ssd = view->ssd;
	int r = rc.corner_radius;
	int bw = theme->border_width;
	/*
	 * labwc hides the border on a maximized window; then the client's
	 * corner is the window's corner. Otherwise it sits inside the border.
	 */
	bool csd = !ssd;
	bool border_on = !csd && ssd->border.tree->node.enabled;
	int rc_ = border_on ? r - bw : r;
	int w = view->current.width;
	int h = view->current.height;
	int gx, gy;
	geometry_offset(view, &gx, &gy);

	c->on = true;
	c->surface_tree = surface_tree;

#ifdef HAVE_SCENEFX
	if (elavo_mods_fx_corners()) {
		drop_strips(c);
		drop_pieces(c);
		fx_sync(c, surface_tree, csd, border_on, r, rc_, gx, gy, w, h);
		return;
	}
	fx_off(c);
#endif

	struct wlr_box main = csd
		? (struct wlr_box){ gx, gy + rc_, w, h - 2 * rc_ }
		: (struct wlr_box){ gx, gy, w, h - rc_ };
	wlr_scene_subsurface_tree_set_clip(surface_tree, &main);

	/* One strip per run of rows that leave the same inset. */
	int key = rc_ * 2 + csd;
	if (c->radius != key) {
		drop_strips(c);
		c->radius = key;
	}
	struct wlr_scene_tree *parent = surface_tree->parent;
	int n = 0;
	int row = 0;
	while (row < rc_ && n < MAX_STRIPS) {
		double dy = row + 0.5;
		int inset = (int)lround(rc_ - sqrt((double)rc_ * rc_ - dy * dy));
		int end = row + 1;
		while (end < rc_) {
			double dy2 = end + 0.5;
			int inset2 = (int)lround(rc_
				- sqrt((double)rc_ * rc_ - dy2 * dy2));
			if (inset2 != inset) {
				break;
			}
			end++;
		}
		struct wlr_box box = {
			gx + inset, gy + h - rc_ + row,
			w - 2 * inset, end - row,
		};
		if (!place_strip(c, n, parent, surface_tree, &box)) {
			break;
		}
		n++;
		if (csd && n < MAX_STRIPS) {
			/* The same run, mirrored into the top band. */
			box.y = gy + rc_ - end;
			if (!place_strip(c, n, parent, surface_tree, &box)) {
				break;
			}
			n++;
		}
		row = end;
	}
	for (int i = n; i < c->n_strips; i++) {
		struct strip *strip = &c->strips[i];
		if (strip->tree) {
			wl_list_remove(&strip->destroy.link);
			wlr_scene_node_destroy(&strip->tree->node);
			strip->tree = NULL;
		}
	}
	c->n_strips = n;

	/*
	 * The border: stop the straight edges short of the curve, add arcs.
	 * A tiled window gets labwc's squared layout (sides running up past
	 * the title bar, a full-width top); it is laid out as a floating one
	 * here, since its title-bar corners stay round.
	 */
	if (!border_on) {
		return;
	}
	if (c->pieces_ssd != ssd || !c->pieces[0][0].buffer
			|| !c->pieces[1][0].buffer) {
		drop_pieces(c);
		build_pieces(c, r);
	}
	int full_width = w + 2 * bw;
	enum ssd_active_state s;
	FOR_EACH_ACTIVE_STATE(s) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[s];
		wlr_scene_rect_set_size(subtree->left, bw, MAX(h - rc_, 0));
		wlr_scene_node_set_position(&subtree->left->node, 0, 0);
		wlr_scene_rect_set_size(subtree->right, bw, MAX(h - rc_, 0));
		wlr_scene_node_set_position(&subtree->right->node, bw + w, 0);
		wlr_scene_rect_set_size(subtree->bottom,
			MAX(full_width - 2 * r, 0), bw);
		wlr_scene_node_set_position(&subtree->bottom->node, r, h);
		int corner_width = ssd_get_corner_width();
		wlr_scene_rect_set_size(subtree->top,
			MAX(w - 2 * corner_width, 0), bw);
		wlr_scene_node_set_position(&subtree->top->node,
			bw + corner_width, -(ssd->titlebar.height + bw));
		struct piece *left = &c->pieces[s][0];
		struct piece *right = &c->pieces[s][1];
		if (left->buffer) {
			wlr_scene_node_set_position(&left->buffer->node,
				0, h + bw - r);
		}
		if (right->buffer) {
			wlr_scene_node_set_position(&right->buffer->node,
				full_width - r, h + bw - r);
		}
	}
}

static void
destroy_clipped(struct clipped *c)
{
	drop_strips(c);
	drop_pieces(c);
	wl_list_remove(&c->view_destroy.link);
	wl_list_remove(&c->link);
	free(c);
}

static void
handle_view_destroy(struct wl_listener *listener, void *data)
{
	struct clipped *c = wl_container_of(listener, c, view_destroy);
	destroy_clipped(c);
}

static struct clipped *
find_clipped(struct view *view)
{
	struct clipped *c;
	wl_list_for_each(c, &clipped_views, link) {
		if (c->view == view) {
			return c;
		}
	}
	return NULL;
}

/*
 * Called every output frame. labwc rewrites the border rects on its own
 * schedule (geometry, reconfigure, tiling), so this puts them back each time.
 */
void
elavo_mods_clip_frame(void)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		struct clipped *c = find_clipped(view);
		if (!c) {
			if (!wants_clip(view)) {
				continue;
			}
			c = znew(*c);
			c->view = view;
			c->view_destroy.notify = handle_view_destroy;
			wl_signal_add(&view->events.destroy, &c->view_destroy);
			wl_list_insert(&clipped_views, &c->link);
		}
		sync_clip(c);
	}
}

bool
elavo_mods_clip_active(struct view *view)
{
	struct clipped *c = find_clipped(view);
	return c && c->on;
}

void
elavo_mods_clip_theme_changed(void)
{
	struct clipped *c;
	wl_list_for_each(c, &clipped_views, link) {
		drop_pieces(c);
	}
	drop_corner_buffers();
}

void
elavo_mods_clip_finish(void)
{
	struct clipped *c, *tmp;
	wl_list_for_each_safe(c, tmp, &clipped_views, link) {
		destroy_clipped(c);
	}
	drop_corner_buffers();
}
