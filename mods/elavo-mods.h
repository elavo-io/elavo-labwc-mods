/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_MODS_H
#define ELAVO_MODS_H

#include <stdbool.h>
#include <stdint.h>

struct ssd;
struct output;
struct view;
struct theme;

void elavo_mods_init(void);
void elavo_mods_finish(void);

/* Return whether the title-bar corners should be square. */
bool elavo_mods_squared(struct ssd *ssd, bool maximized, bool squared);

/* Return whether labwc's shadow shows on a window it decorates. */
bool elavo_mods_ssd_shadows(struct ssd *ssd);

/* Follow a window's geometry and state for shadows labwc does not draw. */
void elavo_mods_view_moved(struct view *view);

/* Redraw labwc's shadow corner pieces to follow the corner radius. */
void elavo_mods_shadows_drawn(struct theme *theme);

/* Apply theme keys before labwc clamps the radius. */
void elavo_mods_theme_loaded(void);

/* Blur opted-in surfaces before the output commits this frame. */
void elavo_mods_output_frame(struct output *output);

/*
 * A finger on the window-button cluster, or on the enlarged pill.
 * touch_down returns true when labwc should not deliver the touch.
 */
bool elavo_mods_touch_down(double lx, double ly, int32_t touch_id);
void elavo_mods_touch_motion(double lx, double ly, int32_t touch_id);
void elavo_mods_touch_up(int32_t touch_id, bool released);

#endif /* ELAVO_MODS_H */
