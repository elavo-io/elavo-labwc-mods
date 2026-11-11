/* SPDX-License-Identifier: GPL-2.0-only */
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include <cairo.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include "buffer.h"
#include "common/box.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "img/img.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "ssd.h"
#include "theme.h"
#include "view.h"

/*
 * A finger tap on the window buttons opens a larger pill of the hover
 * icons over the cluster and does not activate a button. The next tap
 * on a large button runs that action. A pointer click never comes
 * through here.
 *
 * The pill is centered on the cluster, then shifted so the whole pill
 * stays inside the output. A window flush with the left edge gets the
 * pill moved right rather than clipped. Its fill is the title bar
 * color, so a dark scheme gets a dark pill. The sizes are three
 * quarters of the first pill (36px icons in a 168 by 64 pill).
 *
 * The pill grows from half size and fades in, and shrinks and fades out
 * the same way when it is dismissed or times out. Taps pass through as
 * soon as it starts to close.
 *
 * Enlarged buttons are on unless ~/.config/elavo/touch.conf says
 * otherwise. Settings writes that file. A missing file leaves them on.
 */

#define PILL_BUTTON 27
#define PILL_GAP 12
#define PILL_PAD 11
#define PILL_W (PILL_PAD * 2 + PILL_BUTTON * 3 + PILL_GAP * 2)
#define PILL_H (PILL_PAD * 2 + PILL_BUTTON)
#define PILL_SHADOW 5
#define PILL_MARGIN 2
#define PILL_MS 3000
#define PILL_ANIM_MS 180
#define PILL_ANIM_TICK_MS 8

/* Client-side cluster. Covers a 30px title bar and a 46px header bar. */
#define CSD_HIT_W 96
#define CSD_HIT_H 46

static const enum lab_node_type pill_types[3] = {
	LAB_NODE_BUTTON_CLOSE,
	LAB_NODE_BUTTON_ICONIFY,
	LAB_NODE_BUTTON_MAXIMIZE,
};

static struct wlr_scene_tree *pill_tree;
static struct wlr_scene_buffer *pill_buffer;
static struct wl_event_source *pill_timer;
static struct view *pill_view;
static struct wl_listener pill_view_destroy;
static bool pill_watching;
static struct wlr_box pill_box;
static bool pill_armed;
static int pill_armed_slot;
static int32_t pill_armed_id;
static int32_t pill_tracked_id;
static double pill_tracked_x;
static double pill_tracked_y;
static struct wl_event_source *anim_timer;
static double anim_t; /* 0 hidden, 1 fully shown */
static int anim_dir; /* 1 opening, -1 closing, 0 still */
static int64_t anim_last_ms;
static int pill_full_w;
static int pill_full_h;
static double pill_cx;
static double pill_cy;

static bool never_accepts_input(struct wlr_scene_buffer *buffer, double *sx,
		double *sy)
{
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static bool
window_button(enum lab_node_type type)
{
	return type == LAB_NODE_BUTTON_CLOSE
		|| type == LAB_NODE_BUTTON_ICONIFY
		|| type == LAB_NODE_BUTTON_MAXIMIZE;
}

static bool
server_buttons(struct view *view)
{
	return view->ssd_mode == LAB_SSD_MODE_FULL
		&& view_titlebar_visible(view) && !view->fullscreen;
}

static bool
near_box(const struct wlr_box *box, double lx, double ly, int pad)
{
	if (box->width <= 0 || box->height <= 0) {
		return false;
	}
	struct wlr_box hit = {
		.x = box->x - pad,
		.y = box->y - pad,
		.width = box->width + pad * 2,
		.height = box->height + pad * 2,
	};
	return wlr_box_contains_point(&hit, lx, ly);
}

static void
ssd_clusters(struct view *view, struct wlr_box *left, struct wlr_box *right)
{
	struct theme *theme = rc.theme;
	int bw = theme->window_button_width;
	int bh = theme->window_button_height;
	int sp = theme->window_button_spacing;
	int y = view->current.y - theme->titlebar_height
		+ (theme->titlebar_height - bh) / 2;
	*left = (struct wlr_box){0};
	*right = (struct wlr_box){0};

	int x = view->current.x + theme->window_titlebar_padding_width;
	int first = 0;
	int last = 0;
	int n = 0;
	for (int b = 0; b < rc.nr_title_buttons_left; b++) {
		if (window_button(rc.title_buttons_left[b])) {
			if (n == 0) {
				first = x;
			}
			last = x;
			n++;
		}
		x += bw + sp;
	}
	if (n > 0) {
		left->x = first;
		left->y = y;
		left->width = last + bw - first;
		left->height = bh;
	}

	int local = view->current.width - theme->window_titlebar_padding_width
		+ theme->window_button_spacing;
	first = 0;
	last = 0;
	n = 0;
	for (int b = rc.nr_title_buttons_right - 1; b >= 0; b--) {
		local -= bw + sp;
		if (window_button(rc.title_buttons_right[b])) {
			if (n == 0) {
				last = local;
			}
			first = local;
			n++;
		}
	}
	if (n > 0) {
		right->x = view->current.x + first;
		right->y = y;
		right->width = last + bw - first;
		right->height = bh;
	}
}

static bool
pick_ssd(struct view *view, double lx, double ly, struct wlr_box *visual)
{
	if (!server_buttons(view) || !rc.theme) {
		return false;
	}
	struct wlr_box left, right;
	ssd_clusters(view, &left, &right);
	if (near_box(&left, lx, ly, 8)) {
		*visual = left;
		return true;
	}
	if (near_box(&right, lx, ly, 8)) {
		*visual = right;
		return true;
	}
	return false;
}

static bool
pick_csd(struct view *view, double lx, double ly, struct wlr_box *visual)
{
	if (server_buttons(view) || view->fullscreen || view->minimized
			|| !view->mapped || view->current.width <= 0) {
		return false;
	}
	/*
	 * A dialog (fixed size, or with a parent window) usually draws no
	 * window buttons of its own, so a tap in its corner is not a tap on
	 * a cluster. Let it through to the dialog.
	 */
	if (view->impl->contains_window_type
			&& view->impl->contains_window_type(view, LAB_WINDOW_TYPE_DIALOG)) {
		return false;
	}
	double vx = lx - view->current.x;
	double vy = ly - view->current.y;
	if (vy < 0 || vy >= CSD_HIT_H || vx < 0 || vx >= view->current.width) {
		return false;
	}
	/*
	 * Elavo puts the buttons on the left, and the apps it ships honor that
	 * layout. Only the left corner counts. The right corner of a header bar
	 * holds an app's own controls, like Brave's new tab button or the Files
	 * menu, and a tap there must reach the app.
	 */
	if (vx >= CSD_HIT_W) {
		return false;
	}
	int inset_x;
	int inset_y;
	int span;
	if (vy < 32) {
		/* 30px title-only bar: 8px from the side, 7px from the top. */
		inset_x = 8;
		inset_y = 7;
		span = 16 * 3 + 8 * 2;
	} else {
		/* 46px header bar: 19px from the side, 15px from the top. */
		inset_x = 19;
		inset_y = 15;
		span = 16 * 3 + 9 * 2;
	}
	int x = view->current.x + inset_x;
	*visual = (struct wlr_box){
		.x = x,
		.y = view->current.y + inset_y,
		.width = span,
		.height = 16,
	};
	return true;
}

static bool
cluster_at(double lx, double ly, struct view **view_out, struct wlr_box *visual)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(&server.scene->tree.node,
		lx, ly, &sx, &sy);
	while (node) {
		struct node_descriptor *desc = node->data;
		if (desc && desc->view && desc->view->mapped && !desc->view->minimized) {
			if (window_button(desc->type) || desc->type == LAB_NODE_TITLEBAR
					|| desc->type == LAB_NODE_TITLE
					|| desc->type == LAB_NODE_SSD_ROOT) {
				if (pick_ssd(desc->view, lx, ly, visual)) {
					*view_out = desc->view;
					return true;
				}
				return false;
			}
			if (desc->type == LAB_NODE_VIEW) {
				if (pick_csd(desc->view, lx, ly, visual)) {
					*view_out = desc->view;
					return true;
				}
				return false;
			}
		}
		if (!node->parent) {
			break;
		}
		node = &node->parent->node;
	}
	return false;
}

static void
unwatch_view(void)
{
	if (!pill_watching) {
		return;
	}
	wl_list_remove(&pill_view_destroy.link);
	pill_watching = false;
	pill_view = NULL;
}

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Size from 50% to 100% and opacity from 0 to 1, eased out on the way in. */
static void
apply_anim(void)
{
	if (!pill_buffer) {
		return;
	}
	double inv = 1.0 - anim_t;
	double e = 1.0 - inv * inv * inv;
	double size = 0.5 + 0.5 * e;
	int w = (int)(pill_full_w * size + 0.5);
	int h = (int)(pill_full_h * size + 0.5);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	wlr_scene_buffer_set_dest_size(pill_buffer, w, h);
	wlr_scene_node_set_position(&pill_buffer->node,
		(int)(pill_cx - w / 2.0 + 0.5), (int)(pill_cy - h / 2.0 + 0.5));
	wlr_scene_buffer_set_opacity(pill_buffer, (float)e);
}

static int
on_anim_tick(void *data)
{
	(void)data;
	if (!pill_buffer || anim_dir == 0) {
		return 0;
	}
	int64_t now = now_ms();
	anim_t += anim_dir * (double)(now - anim_last_ms) / PILL_ANIM_MS;
	anim_last_ms = now;
	bool done = false;
	if (anim_t >= 1.0) {
		anim_t = 1.0;
		done = true;
	} else if (anim_t <= 0.0) {
		anim_t = 0.0;
		done = true;
	}
	apply_anim();
	if (done) {
		if (anim_dir < 0) {
			wlr_scene_node_set_enabled(&pill_buffer->node, false);
		}
		anim_dir = 0;
		return 0;
	}
	wl_event_source_timer_update(anim_timer, PILL_ANIM_TICK_MS);
	return 0;
}

/* Run from wherever the pill is now, so a reversal does not jump. */
static void
start_anim(int dir)
{
	if (!pill_buffer) {
		return;
	}
	anim_dir = dir;
	anim_last_ms = now_ms();
	if (!anim_timer) {
		anim_t = dir > 0 ? 1.0 : 0.0;
		apply_anim();
		anim_dir = 0;
		if (dir < 0) {
			wlr_scene_node_set_enabled(&pill_buffer->node, false);
		}
		return;
	}
	apply_anim();
	wl_event_source_timer_update(anim_timer, PILL_ANIM_TICK_MS);
}

static void
hide_pill(void)
{
	unwatch_view();
	pill_armed = false;
	pill_box = (struct wlr_box){0};
	if (pill_buffer && pill_buffer->node.enabled && anim_dir >= 0) {
		start_anim(-1);
	}
	if (pill_timer) {
		wl_event_source_timer_update(pill_timer, 0);
	}
}

static void
on_view_destroy(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	hide_pill();
}

static int
on_pill_timeout(void *data)
{
	(void)data;
	hide_pill();
	return 0;
}

static void
rounded_rect(cairo_t *cairo, double x, double y, double w, double h, double r)
{
	const double pi = 3.141592653589793;
	cairo_new_sub_path(cairo);
	cairo_arc(cairo, x + w - r, y + r, r, -pi / 2.0, 0);
	cairo_arc(cairo, x + w - r, y + h - r, r, 0, pi / 2.0);
	cairo_arc(cairo, x + r, y + h - r, r, pi / 2.0, pi);
	cairo_arc(cairo, x + r, y + r, r, pi, 3.0 * pi / 2.0);
	cairo_close_path(cairo);
}

static struct lab_img *
hover_image(enum lab_node_type type, bool toggled)
{
	if (!rc.theme) {
		return NULL;
	}
	struct lab_img **imgs = rc.theme->window[SSD_ACTIVE].button_imgs[type];
	if (toggled && imgs[LAB_BS_HOVERED | LAB_BS_TOGGLED]) {
		return imgs[LAB_BS_HOVERED | LAB_BS_TOGGLED];
	}
	return imgs[LAB_BS_HOVERED];
}

static void
paint_icon(cairo_t *cairo, struct lab_img *img, double x, double y, double scale)
{
	if (!img) {
		return;
	}
	struct lab_data_buffer *icon = lab_img_render(img, PILL_BUTTON, PILL_BUTTON, scale);
	if (!icon) {
		return;
	}
	/*
	 * Both surfaces carry the output scale, and cairo treats a source
	 * surface's device scale as its logical size. The icon then occupies
	 * PILL_BUTTON logical pixels.
	 */
	cairo_set_source_surface(cairo, icon->surface, x, y);
	cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
	cairo_paint(cairo);
	wlr_buffer_drop(&icon->base);
}

/* Theme colors are premultiplied. Cairo wants straight alpha. */
static void
set_theme_color(cairo_t *cairo, const float rgba[4])
{
	float a = rgba[3] > 0.001f ? rgba[3] : 1.0f;
	cairo_set_source_rgba(cairo, rgba[0] / a, rgba[1] / a, rgba[2] / a, a);
}

/*
 * Missing, or anything other than false, leaves the enlarged buttons on.
 * Read on each tap so a change in Settings applies without reloading labwc.
 */
static bool
enlarged_buttons_on(void)
{
	const char *base = getenv("XDG_CONFIG_HOME");
	char path[PATH_MAX];
	if (base && base[0] == '/') {
		snprintf(path, sizeof(path), "%s/elavo/touch.conf", base);
	} else {
		const char *home = getenv("HOME");
		if (!home || home[0] != '/') {
			return true;
		}
		snprintf(path, sizeof(path), "%s/.config/elavo/touch.conf", home);
	}
	FILE *file = fopen(path, "r");
	if (!file) {
		return true;
	}
	char line[256];
	bool on = true;
	while (fgets(line, sizeof(line), file)) {
		char *key = strstr(line, "EnlargedWindowButtons");
		if (!key) {
			continue;
		}
		char *eq = strchr(key, '=');
		if (!eq) {
			continue;
		}
		eq++;
		while (*eq == ' ' || *eq == '\t') {
			eq++;
		}
		on = strncmp(eq, "false", 5) != 0 && strncmp(eq, "no", 2) != 0
			&& *eq != '0';
	}
	fclose(file);
	return on;
}

static struct lab_data_buffer *
draw_pill(struct view *view, double scale)
{
	int buf_w = PILL_W + PILL_MARGIN + PILL_SHADOW;
	int buf_h = PILL_H + PILL_MARGIN + PILL_SHADOW;
	struct lab_data_buffer *buffer = buffer_create_cairo(buf_w, buf_h, scale);
	if (!buffer) {
		return NULL;
	}
	cairo_t *cairo = cairo_create(buffer->surface);
	/* Room for the hairline on the top and left, and the shadow on the other sides. */
	cairo_translate(cairo, PILL_MARGIN, PILL_MARGIN);
	double radius = PILL_H / 2.0;
	bool maximized = view->maximized == VIEW_AXIS_BOTH;

	cairo_set_source_rgba(cairo, 0, 0, 0, 0.16);
	rounded_rect(cairo, 2, 2, PILL_W, PILL_H, radius);
	cairo_fill(cairo);

	if (rc.theme) {
		set_theme_color(cairo, rc.theme->window[SSD_ACTIVE].title_bg.color);
	} else {
		cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	}
	rounded_rect(cairo, 0, 0, PILL_W, PILL_H, radius);
	cairo_fill_preserve(cairo);
	if (rc.theme) {
		set_theme_color(cairo, rc.theme->window[SSD_ACTIVE].border_color);
	} else {
		cairo_set_source_rgba(cairo, 0, 0, 0, 0.12);
	}
	cairo_set_line_width(cairo, 1);
	cairo_stroke(cairo);

	double x = PILL_PAD;
	for (int i = 0; i < 3; i++) {
		bool toggled = maximized && pill_types[i] == LAB_NODE_BUTTON_MAXIMIZE;
		paint_icon(cairo, hover_image(pill_types[i], toggled), x, PILL_PAD, scale);
		x += PILL_BUTTON + PILL_GAP;
	}

	cairo_destroy(cairo);
	return buffer;
}

static bool
show_pill(struct view *view, const struct wlr_box *cluster)
{
	if (!pill_tree || !rc.theme) {
		return false;
	}
	int cx = cluster->x + cluster->width / 2;
	int cy = cluster->y + cluster->height / 2;
	struct output *output = output_nearest_to(cx, cy);
	if (!output || !output->wlr_output) {
		return false;
	}
	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &output_box);
	if (output_box.width <= 0 || output_box.height <= 0) {
		return false;
	}

	int x = 0;
	int y = 0;
	box_center(PILL_W, PILL_H, cluster, &output_box, &x, &y);

	double scale = output->wlr_output->scale;
	if (scale < 1) {
		scale = 1;
	}
	struct lab_data_buffer *buffer = draw_pill(view, scale);
	if (!buffer) {
		return false;
	}

	if (!pill_buffer) {
		pill_buffer = lab_wlr_scene_buffer_create(pill_tree, NULL);
		pill_buffer->point_accepts_input = never_accepts_input;
	}
	if (!pill_buffer->node.enabled) {
		anim_t = 0.0;
	}
	wlr_scene_buffer_set_buffer(pill_buffer, &buffer->base);
	pill_full_w = buffer->logical_width;
	pill_full_h = buffer->logical_height;
	pill_cx = x - PILL_MARGIN + pill_full_w / 2.0;
	pill_cy = y - PILL_MARGIN + pill_full_h / 2.0;
	wlr_buffer_drop(&buffer->base);
	wlr_scene_node_set_enabled(&pill_buffer->node, true);
	start_anim(1);
	wlr_scene_node_place_above(&pill_tree->node, &server.menu_tree->node);

	pill_box = (struct wlr_box){ .x = x, .y = y, .width = PILL_W, .height = PILL_H };
	pill_armed = false;

	unwatch_view();
	pill_view = view;
	pill_view_destroy.notify = on_view_destroy;
	wl_signal_add(&view->events.destroy, &pill_view_destroy);
	pill_watching = true;

	if (pill_timer) {
		wl_event_source_timer_update(pill_timer, PILL_MS);
	}
	return true;
}

static int
slot_at(double lx, double ly)
{
	if (!wlr_box_contains_point(&pill_box, lx, ly) || pill_box.width <= 0) {
		return -1;
	}
	int slot = (int)((lx - pill_box.x) / (pill_box.width / 3.0));
	if (slot < 0) {
		slot = 0;
	}
	if (slot > 2) {
		slot = 2;
	}
	return slot;
}

static void
run_slot(struct view *view, int slot)
{
	switch (pill_types[slot]) {
	case LAB_NODE_BUTTON_CLOSE:
		view_close(view);
		break;
	case LAB_NODE_BUTTON_ICONIFY:
		view_minimize(view, true);
		break;
	case LAB_NODE_BUTTON_MAXIMIZE:
		view_toggle_maximize(view, VIEW_AXIS_BOTH);
		break;
	default:
		break;
	}
}

bool
elavo_mods_touch_down(double lx, double ly, int32_t touch_id)
{
	pill_tracked_id = touch_id;
	pill_tracked_x = lx;
	pill_tracked_y = ly;

	if (pill_view) {
		int slot = slot_at(lx, ly);
		if (slot >= 0) {
			pill_armed = true;
			pill_armed_slot = slot;
			pill_armed_id = touch_id;
			if (pill_timer) {
				wl_event_source_timer_update(pill_timer, PILL_MS);
			}
			return true;
		}
		hide_pill();
		return true;
	}

	if (!enlarged_buttons_on()) {
		return false;
	}

	if (server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		return false;
	}
	struct view *view = NULL;
	struct wlr_box cluster = {0};
	if (!cluster_at(lx, ly, &view, &cluster)) {
		return false;
	}
	return show_pill(view, &cluster);
}

void
elavo_mods_touch_motion(double lx, double ly, int32_t touch_id)
{
	if (touch_id != pill_tracked_id) {
		return;
	}
	pill_tracked_x = lx;
	pill_tracked_y = ly;
	if (pill_armed && touch_id == pill_armed_id
			&& slot_at(lx, ly) != pill_armed_slot) {
		pill_armed = false;
	}
}

void
elavo_mods_touch_up(int32_t touch_id, bool released)
{
	if (!pill_armed || touch_id != pill_armed_id) {
		return;
	}
	int slot = pill_armed_slot;
	struct view *view = pill_view;
	bool inside = released && view
		&& slot_at(pill_tracked_x, pill_tracked_y) == slot;
	pill_armed = false;
	if (!inside) {
		return;
	}
	hide_pill();
	run_slot(view, slot);
}

void
elavo_mods_window_buttons_init(void)
{
	pill_tree = wlr_scene_tree_create(&server.scene->tree);
	die_if_null(pill_tree);
	pill_timer = wl_event_loop_add_timer(server.wl_event_loop, on_pill_timeout, NULL);
	anim_timer = wl_event_loop_add_timer(server.wl_event_loop, on_anim_tick, NULL);
	pill_view_destroy.notify = on_view_destroy;
}

void
elavo_mods_window_buttons_finish(void)
{
	hide_pill();
	anim_dir = 0;
	if (anim_timer) {
		wl_event_source_remove(anim_timer);
		anim_timer = NULL;
	}
	if (pill_timer) {
		wl_event_source_remove(pill_timer);
		pill_timer = NULL;
	}
	if (pill_tree) {
		wlr_scene_node_destroy(&pill_tree->node);
		pill_tree = NULL;
		pill_buffer = NULL;
	}
}
