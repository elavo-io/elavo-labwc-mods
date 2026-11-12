/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_MODS_INTERNAL_H
#define ELAVO_MODS_INTERNAL_H

#include <stdbool.h>

/* Theme wins over rc.xml only for keys that were actually present. */
bool elavo_mods_round_maximized(void);
bool elavo_mods_shadows_on_maximized(void);
bool elavo_mods_blur_enabled(void);
int elavo_mods_blur_levels(void);
bool elavo_mods_clip_corners(void);
/* SceneFX paths; always false in a build without SceneFX. */
bool elavo_mods_fx_corners(void);
bool elavo_mods_fx_shadows(void);
struct view;
/* The window's corners are rounded by clip-corners.c this frame. */
bool elavo_mods_clip_active(struct view *view);
bool elavo_mods_clip_app(const char *app_id);
bool elavo_mods_clip_csd(struct view *view);

void elavo_mods_clip_frame(void);
void elavo_mods_clip_theme_changed(void);
void elavo_mods_clip_finish(void);

void elavo_mods_shadows_theme_changed(void);
void elavo_mods_shadows_frame(void);
void elavo_mods_shadows_finish(void);

/* Apply window.blur.strength to SceneFX's blur passes. */
void elavo_mods_fx_blur_params(void);

void elavo_mods_window_buttons_init(void);
void elavo_mods_window_buttons_finish(void);

#endif
