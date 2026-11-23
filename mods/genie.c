/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* getline, strsep */
#endif
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include <cairo.h>
#include <dirent.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "common/border.h"
#include "common/mem.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "layers.h"
#include "node.h"
#include "output.h"
#include "ssd.h"
#include "view.h"

/*
 * Minimize and restore, as on a Mac.
 *
 * When a window is minimized, its surfaces (and labwc's title bar, if it
 * draws one) are rendered into a buffer while they still show. The window
 * then pours into its place in the dock: on every output frame, the
 * snapshot is drawn into a buffer one physical pixel row at a time, each
 * row squeezed to the width of a funnel that bends toward the dock, with
 * the window sliding down it (the "genie"). Each row takes the slice of
 * the window that lands on it, so rows meet exactly and there are no seams
 * or steps at fractional scales. That buffer is one scene node aligned to the
 * physical pixel grid. Restoring plays the same thing backwards, with a
 * fresh render, and the window shows again in the frame it lands.
 *
 * The dock says where things go. It writes, for this compositor's
 * WAYLAND_DISPLAY,
 *
 *   $XDG_RUNTIME_DIR/elavo/<display>/dock-targets
 *
 * tab separated, coordinates local to its layer surface (namespace
 * elavo-dock) on the named output:
 *
 *   animate  1|0                       (0 when GTK animations are off)
 *   slot     OUTPUT X Y W H            (where a new minimized window lands)
 *   app      OUTPUT APP_ID X Y W H     (an application's icon)
 *   window   OUTPUT APP_ID TITLE X Y W H  (a minimized window's tile)
 *
 * A minimize goes to "slot", or to the application's icon when the dock
 * minimizes into applications (it writes no slot then). A restore comes
 * out of the window's tile, or the application's icon. No file, no dock,
 * no matching line, or animate 0: the window hides and shows at once, as
 * before.
 *
 * The dock also shows the window in its minimized tile. Each minimize
 * writes a small PNG of the window and a note naming it to
 *
 *   $XDG_RUNTIME_DIR/elavo/<display>/minimized/<n>.png, <n>.meta
 *
 * (meta: app_id, a newline, the title), before the dock is told the window
 * is minimized. Both go when the window is restored or closed.
 *
 * Only wlroots render passes and a scene buffer are used, so the build
 * without SceneFX animates the same way.
 */

/* Rows of the funnel, in physical pixels. */
#define SAMPLES 4096
#define MINIMIZE_MS 650
#define RESTORE_MS 550
#define THUMB_MAX 512
#define DOCK_NAMESPACE "elavo-dock"

enum genie_dir { DIR_DOWN, DIR_UP, DIR_RIGHT, DIR_LEFT };

struct mini {
	struct wl_list link;
	struct view *view;
	struct wl_listener destroy;
	char *png;
	char *meta;

	/* Animation, while tree is set. */
	struct wlr_buffer *buffer;      /* the window, at bscale */
	struct wlr_texture *texture;    /* of buffer */
	struct wlr_buffer *frames[2];   /* drawn into in turn */
	int current;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *node;
	struct wlr_box bbox;            /* layout, covers window and target */
	double bscale;                  /* physical pixels per layout pixel */
	int64_t drawn_ms;
	struct wlr_box from;
	struct wlr_box to;
	enum genie_dir dir;
	double p_from;
	double p_to;
	double p;
	int64_t t0;
	int ms;
	bool restoring;
};

static struct wl_list minis = { &minis, &minis };
static unsigned int counter;
static bool cleaned;

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double
clamp01(double v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static double
smooth(double t)
{
	t = clamp01(t);
	return t * t * (3 - 2 * t);
}

static double
lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

/* $XDG_RUNTIME_DIR/elavo/<display>[/sub], created. Caller frees. */
static char *
runtime_path(const char *sub)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime || !*runtime || !display || !*display || strchr(display, '/')) {
		return NULL;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/elavo", runtime);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/elavo/%s", runtime, display);
	mkdir(path, 0700);
	if (sub) {
		size_t len = strlen(path);
		snprintf(path + len, sizeof(path) - len, "/%s", sub);
		mkdir(path, 0700);
	}
	return xstrdup(path);
}

/* Snapshots left by an earlier run of this display. */
static void
clean_old_snapshots(void)
{
	if (cleaned) {
		return;
	}
	cleaned = true;
	char *dir = runtime_path("minimized");
	if (!dir) {
		return;
	}
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') {
				continue;
			}
			char path[4096];
			snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
			unlink(path);
		}
		closedir(d);
	}
	free(dir);
}

/* ---------------------------------------------------------------------- */
/* Rendering a window into a buffer */

struct render_ctx {
	struct wlr_render_pass *pass;
	double scale;
	struct wlr_texture *temp[64];
	int ntemp;
};

static void
render_node(struct render_ctx *ctx, struct wlr_scene_node *node, double x, double y)
{
	if (!node->enabled) {
		return;
	}
	x += node->x;
	y += node->y;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(ctx, child, x, y);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		if (!sb->buffer) {
			break;
		}
		struct wlr_texture *texture = NULL;
		struct wlr_client_buffer *client = wlr_client_buffer_get(sb->buffer);
		if (client) {
			texture = client->texture;
		} else if (ctx->ntemp < (int)(sizeof(ctx->temp) / sizeof(ctx->temp[0]))) {
			/* labwc's own buffers: title bar, buttons, borders. */
			texture = wlr_texture_from_buffer(server.renderer, sb->buffer);
			if (texture) {
				ctx->temp[ctx->ntemp++] = texture;
			}
		}
		if (!texture) {
			break;
		}
		int w = sb->dst_width > 0 ? sb->dst_width : sb->buffer->width;
		int h = sb->dst_height > 0 ? sb->dst_height : sb->buffer->height;
		wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = sb->src_box,
			.dst_box = {
				.x = (int)lround(x * ctx->scale),
				.y = (int)lround(y * ctx->scale),
				.width = (int)lround(w * ctx->scale),
				.height = (int)lround(h * ctx->scale),
			},
			.alpha = &sb->opacity,
			.transform = sb->transform,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		});
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		wlr_render_pass_add_rect(ctx->pass, &(struct wlr_render_rect_options){
			.box = {
				.x = (int)lround(x * ctx->scale),
				.y = (int)lround(y * ctx->scale),
				.width = (int)lround(rect->width * ctx->scale),
				.height = (int)lround(rect->height * ctx->scale),
			},
			.color = {
				.r = rect->color[0],
				.g = rect->color[1],
				.b = rect->color[2],
				.a = rect->color[3],
			},
		});
		break;
	}
	default:
		/* SceneFX shadows and blur are not part of the window. */
		break;
	}
}

/* The window's box in layout coordinates, title bar and border included. */
static struct wlr_box
window_box(struct view *view, int *ox, int *oy)
{
	struct border margin = ssd_get_margin(view->ssd);
	int lx = 0, ly = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
	*ox = margin.left;
	*oy = margin.top;
	return (struct wlr_box){
		.x = lx - margin.left,
		.y = ly - margin.top,
		.width = view->current.width + margin.left + margin.right,
		.height = view->current.height + margin.top + margin.bottom,
	};
}

/*
 * Outputs usually scan out XRGB, where a cleared pixel is black, and a
 * window's rounded corners need alpha: the output's format with alpha.
 */
static struct wlr_buffer *
alloc_alpha_buffer(struct view *view, int width, int height)
{
	struct output *output = view->output;
	if (!output || !output->wlr_output || !output->wlr_output->swapchain) {
		output = NULL;
		struct output *o;
		wl_list_for_each(o, &server.outputs, link) {
			if (o->wlr_output && o->wlr_output->swapchain) {
				output = o;
				break;
			}
		}
	}
	if (!output) {
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
		buffer = wlr_allocator_create_buffer(server.allocator, width, height, &format);
	}
	return buffer;
}

static struct view *
root_of(struct view *view)
{
	return view->impl->get_root ? view->impl->get_root(view) : view;
}

/* Render the window at `scale` buffer pixels per layout pixel. */
static struct wlr_buffer *
render_window(struct view *view, double scale, struct wlr_box *box_out)
{
	if (!view->scene_tree || !view->content_tree) {
		return NULL;
	}
	int ox, oy;
	struct wlr_box box = window_box(view, &ox, &oy);
	if (box.width <= 0 || box.height <= 0) {
		return NULL;
	}
	int bw = (int)lround(box.width * scale);
	int bh = (int)lround(box.height * scale);
	if (bw < 1 || bh < 1) {
		return NULL;
	}
	struct wlr_buffer *buffer = alloc_alpha_buffer(view, bw, bh);
	if (!buffer) {
		wlr_log(WLR_ERROR, "genie: no buffer for %dx%d", bw, bh);
		return NULL;
	}
	struct render_ctx ctx = { .scale = scale };
	ctx.pass = wlr_renderer_begin_buffer_pass(server.renderer, buffer, NULL);
	if (!ctx.pass) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	wlr_render_pass_add_rect(ctx.pass, &(struct wlr_render_rect_options){
		.box = { .width = bw, .height = bh },
		.color = { 0, 0, 0, 0 },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	struct wlr_scene_node *child;
	wl_list_for_each(child, &view->scene_tree->children, link) {
		render_node(&ctx, child, ox, oy);
	}
	bool ok = wlr_render_pass_submit(ctx.pass);
	for (int i = 0; i < ctx.ntemp; i++) {
		wlr_texture_destroy(ctx.temp[i]);
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "genie: render pass failed");
		wlr_buffer_drop(buffer);
		return NULL;
	}
	if (box_out) {
		*box_out = box;
	}
	return buffer;
}

static double
view_scale(struct view *view)
{
	if (view->output && view->output->wlr_output) {
		return view->output->wlr_output->scale;
	}
	return 1.0;
}

/* ---------------------------------------------------------------------- */
/* The dock's snapshot */

static void
remove_files(struct mini *m)
{
	if (m->meta) {
		unlink(m->meta);
		free(m->meta);
		m->meta = NULL;
	}
	if (m->png) {
		unlink(m->png);
		free(m->png);
		m->png = NULL;
	}
}

static void
clean_text(char *s)
{
	for (; s && *s; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\r') {
			*s = ' ';
		}
	}
}

static void
write_snapshot(struct mini *m)
{
	struct view *view = m->view;
	int ox, oy;
	struct wlr_box box = window_box(view, &ox, &oy);
	int longest = box.width > box.height ? box.width : box.height;
	if (longest <= 0) {
		return;
	}
	double scale = view_scale(view);
	if (longest * scale > THUMB_MAX) {
		scale = (double)THUMB_MAX / longest;
	}
	struct wlr_buffer *buffer = render_window(view, scale, NULL);
	if (!buffer) {
		return;
	}
	int w = buffer->width, h = buffer->height;
	struct wlr_texture *texture = wlr_texture_from_buffer(server.renderer, buffer);
	unsigned char *data = texture ? calloc((size_t)w * h, 4) : NULL;
	bool ok = data && wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options){
			.data = data,
			.format = DRM_FORMAT_ARGB8888,
			.stride = (uint32_t)w * 4,
			.src_box = { .width = w, .height = h },
		});
	if (texture) {
		wlr_texture_destroy(texture);
	}
	wlr_buffer_drop(buffer);
	char *dir = ok ? runtime_path("minimized") : NULL;
	if (!dir) {
		if (!ok) {
			wlr_log(WLR_ERROR, "genie: could not read the snapshot back");
		}
		free(data);
		return;
	}
	unsigned int n = ++counter;
	char png[4096], tmp[4200], meta[4096];
	snprintf(png, sizeof(png), "%s/%u.png", dir, n);
	snprintf(tmp, sizeof(tmp), "%s.part", png);
	snprintf(meta, sizeof(meta), "%s/%u.meta", dir, n);
	free(dir);

	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_status_t status = cairo_surface_write_to_png(surface, tmp);
	cairo_surface_destroy(surface);
	free(data);
	if (status != CAIRO_STATUS_SUCCESS || rename(tmp, png) != 0) {
		unlink(tmp);
		return;
	}
	char *app_id = xstrdup(view->app_id ? view->app_id : "");
	char *title = xstrdup(view->title ? view->title : "");
	clean_text(app_id);
	clean_text(title);
	snprintf(tmp, sizeof(tmp), "%s.part", meta);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", app_id, title);
		fclose(f);
		rename(tmp, meta);
	}
	free(app_id);
	free(title);
	remove_files(m);
	m->png = xstrdup(png);
	m->meta = xstrdup(meta);
}

/* ---------------------------------------------------------------------- */
/* Where the dock is */

struct dock_targets {
	bool animate;
	bool have_slot, have_app, have_window;
	struct wlr_box slot, app, window;
	char slot_output[64], app_output[64], window_output[64];
};

static bool
parse_box(char **fields, int n, struct wlr_box *box)
{
	if (n < 4) {
		return false;
	}
	box->x = atoi(fields[0]);
	box->y = atoi(fields[1]);
	box->width = atoi(fields[2]);
	box->height = atoi(fields[3]);
	return box->width > 0 && box->height > 0;
}

/*
 * Read the dock's file for `view`. A line on the view's output wins over
 * the same line on another output.
 */
static void
read_targets(struct view *view, struct dock_targets *t)
{
	*t = (struct dock_targets){ .animate = true };
	char *dir = runtime_path(NULL);
	if (!dir) {
		return;
	}
	char path[4200];
	snprintf(path, sizeof(path), "%s/dock-targets", dir);
	free(dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	const char *here = view->output && view->output->wlr_output
		? view->output->wlr_output->name : "";
	char *app_id = xstrdup(view->app_id ? view->app_id : "");
	char *title = xstrdup(view->title ? view->title : "");
	clean_text(app_id);
	clean_text(title);
	bool slot_here = false, app_here = false, window_here = false;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, f)) > 0) {
		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
		}
		char *fields[12];
		int n = 0;
		/* strsep keeps empty fields, such as an empty title. */
		char *rest = line;
		while (n < 12 && rest) {
			fields[n++] = strsep(&rest, "\t");
		}
		if (n < 1) {
			continue;
		}
		struct wlr_box box;
		if (!strcmp(fields[0], "animate") && n >= 2) {
			t->animate = atoi(fields[1]) != 0;
		} else if (!strcmp(fields[0], "slot") && n >= 6
				&& parse_box(fields + 2, n - 2, &box)) {
			bool on = !strcmp(fields[1], here);
			if (!t->have_slot || (on && !slot_here)) {
				t->slot = box;
				t->have_slot = true;
				slot_here = on;
				snprintf(t->slot_output, sizeof(t->slot_output), "%s", fields[1]);
			}
		} else if (!strcmp(fields[0], "app") && n >= 7
				&& !strcmp(fields[2], app_id)
				&& parse_box(fields + 3, n - 3, &box)) {
			bool on = !strcmp(fields[1], here);
			if (!t->have_app || (on && !app_here)) {
				t->app = box;
				t->have_app = true;
				app_here = on;
				snprintf(t->app_output, sizeof(t->app_output), "%s", fields[1]);
			}
		} else if (!strcmp(fields[0], "window") && n >= 8
				&& !strcmp(fields[2], app_id)
				&& !strcmp(fields[3], title)
				&& parse_box(fields + 4, n - 4, &box)) {
			bool on = !strcmp(fields[1], here);
			if (!t->have_window || (on && !window_here)) {
				t->window = box;
				t->have_window = true;
				window_here = on;
				snprintf(t->window_output, sizeof(t->window_output), "%s", fields[1]);
			}
		}
	}
	free(line);
	fclose(f);
	free(app_id);
	free(title);
}

/* The dock's layer surface on `output_name`. */
static struct lab_layer_surface *
find_dock(const char *output_name)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->wlr_output || strcmp(output->wlr_output->name, output_name)) {
			continue;
		}
		for (int i = 0; i < 4; i++) {
			if (!output->layer_tree[i]) {
				continue;
			}
			struct wlr_scene_node *node;
			wl_list_for_each(node, &output->layer_tree[i]->children, link) {
				struct node_descriptor *desc = node->data;
				if (!desc || desc->type != LAB_NODE_LAYER_SURFACE) {
					continue;
				}
				struct lab_layer_surface *surface = node_layer_surface_from_node(node);
				if (surface && surface->mapped && surface->layer_surface->namespace
						&& !strcmp(surface->layer_surface->namespace, DOCK_NAMESPACE)) {
					return surface;
				}
			}
		}
	}
	return NULL;
}

/* A box local to the dock on `output_name`, in layout coordinates. */
static bool
dock_box(const char *output_name, struct wlr_box local, struct wlr_box *out,
		enum genie_dir *dir)
{
	struct lab_layer_surface *dock = find_dock(output_name);
	if (!dock) {
		return false;
	}
	int lx, ly;
	if (!wlr_scene_node_coords(&dock->scene_layer_surface->tree->node, &lx, &ly)) {
		/* Hidden (autohide moves it, it does not disable it). */
	}
	*out = local;
	out->x += lx;
	out->y += ly;
	uint32_t anchor = dock->layer_surface->current.anchor;
	bool top = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	bool bottom = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	bool left = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	bool right = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (top && bottom && left && !right) {
		*dir = DIR_LEFT;
	} else if (top && bottom && right && !left) {
		*dir = DIR_RIGHT;
	} else if (top && !bottom) {
		*dir = DIR_UP;
	} else {
		*dir = DIR_DOWN;
	}
	return true;
}

/* ---------------------------------------------------------------------- */
/* The animation */

static void
stop_animation(struct mini *m)
{
	if (m->tree) {
		wlr_scene_node_destroy(&m->tree->node);
		m->tree = NULL;
		m->node = NULL;
	}
	if (m->texture) {
		wlr_texture_destroy(m->texture);
		m->texture = NULL;
	}
	for (int i = 0; i < 2; i++) {
		if (m->frames[i]) {
			wlr_buffer_drop(m->frames[i]);
			m->frames[i] = NULL;
		}
	}
	if (m->buffer) {
		wlr_buffer_drop(m->buffer);
		m->buffer = NULL;
	}
}

static void
mini_free(struct mini *m)
{
	stop_animation(m);
	remove_files(m);
	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->link);
	free(m);
}

static bool
any_animating(void)
{
	struct mini *m;
	wl_list_for_each(m, &minis, link) {
		if (m->tree) {
			return true;
		}
	}
	return false;
}

static void
schedule_frames(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output->wlr_output && output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

/*
 * Draw the window for progress p: 0 is the window where it was, 1 is the
 * window poured into the target.
 *
 * Work in a frame where the window travels toward +a, with c across, in
 * physical pixels of the frame buffer. The funnel's sides bend in over the
 * first 40%, from the window's sides to the target's, steeper toward the
 * target. The rows slide down it with the edge nearest the dock leading,
 * each squeezed to the funnel's width where it is.
 */
static void
draw_frame(struct mini *m, double p)
{
	struct wlr_buffer *target = m->frames[m->current];
	m->current ^= 1;
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, target, NULL);
	if (!pass) {
		return;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = target->width, .height = target->height },
		.color = { 0, 0, 0, 0 },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});

	bool vertical = m->dir == DIR_DOWN || m->dir == DIR_UP;
	double sign = (m->dir == DIR_DOWN || m->dir == DIR_RIGHT) ? 1 : -1;
	double k = m->bscale;
	/* Physical, relative to the frame buffer. */
	double fx = (m->from.x - m->bbox.x) * k, fy = (m->from.y - m->bbox.y) * k;
	double fw = m->from.width * k, fh = m->from.height * k;
	double tx = (m->to.x - m->bbox.x) * k, ty = (m->to.y - m->bbox.y) * k;
	double tw = m->to.width * k, th = m->to.height * k;
	double wa0, wa1, wc0, wc1, ta0, ta1, tc0, tc1;
	if (vertical) {
		wa0 = fy; wa1 = fy + fh; wc0 = fx; wc1 = fx + fw;
		ta0 = ty; ta1 = ty + th; tc0 = tx; tc1 = tx + tw;
	} else {
		wa0 = fx; wa1 = fx + fw; wc0 = fy; wc1 = fy + fh;
		ta0 = tx; ta1 = tx + tw; tc0 = ty; tc1 = ty + th;
	}
	if (sign < 0) {
		double t;
		t = wa0; wa0 = -wa1; wa1 = -t;
		t = ta0; ta0 = -ta1; ta1 = -t;
	}
	double wlen = wa1 - wa0, tlen = ta1 - ta0;
	double reach = ta0 - wa0;
	double bend = smooth(p / 0.4);
	double slide = clamp01((p - 0.15) / 0.85);
	double alpha = 1.0 - 0.55 * smooth((p - 0.72) / 0.28);
	float alpha_f = (float)alpha;
	int ba = vertical ? m->buffer->height : m->buffer->width;
	int bc = vertical ? m->buffer->width : m->buffer->height;

	/*
	 * Sample where each slice of the window is along the funnel, then
	 * walk it one physical pixel row at a time, so neighbors share an edge
	 * and the funnel's sides step by at most a pixel's worth of bend.
	 */
	static double pos[SAMPLES + 1];
	for (int j = 0; j <= SAMPLES; j++) {
		double v = (double)j / SAMPLES;
		double q = smooth(slide * 2.0 - (1.0 - v));
		double a = lerp(wa0 + v * wlen, ta0 + v * tlen, q);
		pos[j] = a;
		if (j > 0 && pos[j] < pos[j - 1]) {
			pos[j] = pos[j - 1];
		}
	}
	long first = (long)floor(pos[0]);
	long last = (long)ceil(pos[SAMPLES]);
	int j = 0;
	double v_prev = 0;
	for (long r = first; r < last; r++) {
		/* Slice of the window that lands at or before r + 1. */
		double edge = (double)(r + 1);
		while (j < SAMPLES && pos[j + 1] <= edge) {
			j++;
		}
		double v_next;
		if (j >= SAMPLES) {
			v_next = 1.0;
		} else {
			double span = pos[j + 1] - pos[j];
			double f = span > 1e-9 ? (edge - pos[j]) / span : 1.0;
			v_next = ((double)j + clamp01(f)) / SAMPLES;
		}
		if (v_next <= v_prev) {
			continue;
		}
		double s = reach > 1 ? smooth((r + 0.5 - wa0) / reach) : 1.0;
		s *= bend;
		long c0 = lround(lerp(wc0, tc0, s));
		long c1 = lround(lerp(wc1, tc1, s));
		if (c1 <= c0) {
			c1 = c0 + 1;
		}
		double sa0 = sign > 0 ? v_prev * ba : (1 - v_next) * ba;
		double sa1 = sign > 0 ? v_next * ba : (1 - v_prev) * ba;
		long row = sign > 0 ? r : -(r + 1);
		struct wlr_box dst;
		struct wlr_fbox src;
		if (vertical) {
			dst = (struct wlr_box){ (int)c0, (int)row, (int)(c1 - c0), 1 };
			src = (struct wlr_fbox){ 0, sa0, bc, sa1 - sa0 };
		} else {
			dst = (struct wlr_box){ (int)row, (int)c0, 1, (int)(c1 - c0) };
			src = (struct wlr_fbox){ sa0, 0, sa1 - sa0, bc };
		}
		wlr_render_pass_add_texture(pass,
			&(struct wlr_render_texture_options){
				.texture = m->texture,
				.src_box = src,
				.dst_box = dst,
				.alpha = &alpha_f,
				.filter_mode = WLR_SCALE_FILTER_BILINEAR,
			});
		v_prev = v_next;
	}
	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "genie: frame render failed");
		return;
	}
	wlr_scene_buffer_set_buffer(m->node, target);
	wlr_scene_buffer_set_dest_size(m->node, m->bbox.width, m->bbox.height);
}

static double
ease_in_out(double t)
{
	/* Close to a Mac: gentle start and landing. */
	t = clamp01(t);
	return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
}

static void
finish_animation(struct mini *m)
{
	bool restoring = m->restoring;
	stop_animation(m);
	if (!restoring) {
		return;
	}
	struct view *view = m->view;
	mini_free(m);
	if (view->mapped && !view->minimized) {
		view_update_visibility(view);
	}
}

/* Advance every animation, once per frame, before the output commits. */
void
elavo_mods_genie_frame(struct output *output)
{
	(void)output;
	if (!any_animating()) {
		return;
	}
	int64_t now = now_ms();
	struct mini *m, *tmp;
	wl_list_for_each_safe(m, tmp, &minis, link) {
		if (!m->tree) {
			continue;
		}
		double t = m->ms > 0 ? (double)(now - m->t0) / m->ms : 1.0;
		if (t >= 1.0) {
			finish_animation(m);
			continue;
		}
		/* Several outputs: draw once per moment. */
		if (now == m->drawn_ms) {
			continue;
		}
		m->drawn_ms = now;
		m->p = lerp(m->p_from, m->p_to, ease_in_out(t));
		draw_frame(m, m->p);
	}
	/* One more frame after the last one lands, to finish it. */
	schedule_frames();
}

/* x snapped down so that (x - origin) * scale is a whole pixel. */
static int
snap(int x, int origin, double scale)
{
	for (int i = 0; i < 16; i++) {
		double px = (x - i - origin) * scale;
		if (fabs(px - round(px)) < 1e-3) {
			return x - i;
		}
	}
	return x;
}

/* Clear a rounded corner the window's own buffers draw square. */
static void
clear_corners(struct wlr_buffer *buffer, double radius)
{
	if (radius < 1) {
		return;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, buffer, NULL);
	if (!pass) {
		return;
	}
	int r = (int)ceil(radius);
	int w = buffer->width, h = buffer->height;
	for (int y = 0; y < r && y < h / 2; y++) {
		double dy = radius - (y + 0.5);
		double inside = sqrt(fmax(0, radius * radius - dy * dy));
		int cut = (int)lround(radius - inside);
		if (cut <= 0) {
			continue;
		}
		struct wlr_box boxes[4] = {
			{ 0, y, cut, 1 }, { w - cut, y, cut, 1 },
			{ 0, h - 1 - y, cut, 1 }, { w - cut, h - 1 - y, cut, 1 },
		};
		for (int i = 0; i < 4; i++) {
			wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
				.box = boxes[i],
				.color = { 0, 0, 0, 0 },
				.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			});
		}
	}
	wlr_render_pass_submit(pass);
}

static struct wlr_scene_tree *
animation_parent(void)
{
	return &server.scene->tree;
}

/*
 * Start pouring `m` between its window box and target. A running animation
 * of the same window turns around from where it is.
 */
static bool
start_animation(struct mini *m, struct wlr_buffer *buffer, double p_to, int full_ms)
{
	double p_from = m->tree ? m->p : 1.0 - p_to;
	stop_animation(m);
	m->buffer = buffer;
	m->bscale = view_scale(m->view);
	if (m->view->ssd) {
		clear_corners(buffer, rc.corner_radius * m->bscale);
	}
	m->texture = wlr_texture_from_buffer(server.renderer, buffer);
	if (!m->texture) {
		stop_animation(m);
		return false;
	}

	/* Cover window and target, aligned to the output's pixel grid. */
	struct wlr_box *a = &m->from, *b = &m->to;
	int x0 = a->x < b->x ? a->x : b->x;
	int y0 = a->y < b->y ? a->y : b->y;
	int x1 = a->x + a->width > b->x + b->width ? a->x + a->width : b->x + b->width;
	int y1 = a->y + a->height > b->y + b->height ? a->y + a->height : b->y + b->height;
	int ox = 0, oy = 0;
	if (m->view->output && m->view->output->wlr_output) {
		struct wlr_box ob;
		wlr_output_layout_get_box(server.output_layout,
			m->view->output->wlr_output, &ob);
		ox = ob.x;
		oy = ob.y;
	}
	x0 = snap(x0 - 2, ox, m->bscale);
	y0 = snap(y0 - 2, oy, m->bscale);
	int w = x1 + 2 - x0, h = y1 + 2 - y0;
	/* Whole physical pixels across, so the buffer maps one to one. */
	for (int i = 0; i < 16 && fabs(w * m->bscale - round(w * m->bscale)) > 1e-3; i++) {
		w++;
	}
	for (int i = 0; i < 16 && fabs(h * m->bscale - round(h * m->bscale)) > 1e-3; i++) {
		h++;
	}
	m->bbox = (struct wlr_box){ x0, y0, w, h };
	int bw = (int)lround(w * m->bscale);
	int bh = (int)lround(h * m->bscale);
	for (int i = 0; i < 2; i++) {
		m->frames[i] = alloc_alpha_buffer(m->view, bw, bh);
		if (!m->frames[i]) {
			stop_animation(m);
			return false;
		}
	}
	m->current = 0;
	m->tree = wlr_scene_tree_create(animation_parent());
	m->node = m->tree ? wlr_scene_buffer_create(m->tree, NULL) : NULL;
	if (!m->node) {
		stop_animation(m);
		return false;
	}
	wlr_scene_node_raise_to_top(&m->tree->node);
	wlr_scene_node_set_position(&m->node->node, m->bbox.x, m->bbox.y);
	wlr_scene_buffer_set_filter_mode(m->node, WLR_SCALE_FILTER_BILINEAR);

	m->p_from = p_from;
	m->p_to = p_to;
	m->p = p_from;
	m->ms = (int)(full_ms * fabs(p_to - p_from));
	m->t0 = now_ms();
	m->drawn_ms = m->t0;
	/* The first frame shows with the window's hiding, not after it. */
	draw_frame(m, p_from);
	schedule_frames();
	return true;
}

static void
handle_view_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct mini *m = wl_container_of(listener, m, destroy);
	mini_free(m);
}

static struct mini *
find_mini(struct view *view)
{
	struct mini *m;
	wl_list_for_each(m, &minis, link) {
		if (m->view == view) {
			return m;
		}
	}
	return NULL;
}

static struct mini *
get_mini(struct view *view)
{
	struct mini *m = find_mini(view);
	if (m) {
		return m;
	}
	m = znew(*m);
	m->view = view;
	m->destroy.notify = handle_view_destroy;
	wl_signal_add(&view->events.destroy, &m->destroy);
	wl_list_insert(&minis, &m->link);
	return m;
}

static bool
genie_wanted(void)
{
	const char *env = getenv("ELAVO_GENIE");
	return !(env && !strcmp(env, "0"));
}

void
elavo_mods_view_minimizing(struct view *view)
{
	if (!view || !view->mapped || !view->content_tree || root_of(view) != view) {
		return;
	}
	clean_old_snapshots();
	struct mini *m = get_mini(view);
	bool turning = m->tree && m->restoring;
	m->restoring = false;
	write_snapshot(m);

	if (!genie_wanted()) {
		stop_animation(m);
		return;
	}
	struct dock_targets t;
	read_targets(view, &t);
	struct wlr_box target;
	enum genie_dir dir;
	bool have = t.animate && (
		(t.have_slot && dock_box(t.slot_output, t.slot, &target, &dir))
		|| (t.have_app && dock_box(t.app_output, t.app, &target, &dir)));
	if (!have) {
		stop_animation(m);
		return;
	}
	struct wlr_box box;
	struct wlr_buffer *buffer = render_window(view, view_scale(view), &box);
	if (!buffer) {
		stop_animation(m);
		return;
	}
	if (!turning) {
		m->from = box;
	}
	m->to = target;
	m->dir = dir;
	start_animation(m, buffer, 1.0, MINIMIZE_MS);
}

void
elavo_mods_view_restored(struct view *view)
{
	if (!view || root_of(view) != view) {
		return;
	}
	struct mini *m = find_mini(view);
	if (m) {
		remove_files(m);
	}
	if (!genie_wanted() || !view->mapped || !view->content_tree) {
		if (m) {
			mini_free(m);
		}
		return;
	}
	struct dock_targets t;
	read_targets(view, &t);
	struct wlr_box target;
	enum genie_dir dir;
	bool have = t.animate && (
		(t.have_window && dock_box(t.window_output, t.window, &target, &dir))
		|| (t.have_app && dock_box(t.app_output, t.app, &target, &dir)));
	if (!have) {
		if (m) {
			mini_free(m);
		}
		return;
	}
	struct wlr_box box;
	struct wlr_buffer *buffer = render_window(view, view_scale(view), &box);
	if (!buffer) {
		if (m) {
			mini_free(m);
		}
		return;
	}
	if (!m) {
		m = get_mini(view);
	}
	m->restoring = true;
	m->from = box;
	m->to = target;
	m->dir = dir;
	if (!start_animation(m, buffer, 0.0, RESTORE_MS)) {
		mini_free(m);
		return;
	}
	/* labwc just showed it; it shows again when it lands. */
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
}

void
elavo_mods_genie_finish(void)
{
	struct mini *m, *tmp;
	wl_list_for_each_safe(m, tmp, &minis, link) {
		mini_free(m);
	}
}
