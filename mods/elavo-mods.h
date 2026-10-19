/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ELAVO_MODS_H
#define ELAVO_MODS_H

#include <stdbool.h>

struct ssd;
struct output;

void elavo_mods_init(void);
void elavo_mods_finish(void);

/* Return whether the title-bar corners should be square. */
bool elavo_mods_squared(struct ssd *ssd, bool maximized, bool squared);

/* Apply theme keys before labwc clamps the radius. */
void elavo_mods_theme_loaded(void);

/* Blur opted-in surfaces before the output commits this frame. */
void elavo_mods_output_frame(struct output *output);

#endif /* ELAVO_MODS_H */
