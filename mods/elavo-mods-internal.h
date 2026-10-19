/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_MODS_INTERNAL_H
#define ELAVO_MODS_INTERNAL_H

#include <stdbool.h>

/* Theme wins over rc.xml only for keys that were actually present. */
bool elavo_mods_round_maximized(void);
bool elavo_mods_shadows_on_maximized(void);
bool elavo_mods_blur_enabled(void);
int elavo_mods_blur_levels(void);

void elavo_mods_shadows_theme_changed(void);
void elavo_mods_shadows_frame(void);
void elavo_mods_shadows_finish(void);

#endif
