/* SPDX-License-Identifier: GPL-2.0-only */
#include "elavo-mods.h"
#include "elavo-mods-internal.h"

/*
 * Maximized windows keep the top-corner pieces when the theme asks.
 * labwc still drops the border on maximize (ssd thickness goes to the
 * title bar only). The corner pieces are part of the title bar, so they
 * stay; whether they meet a missing border cleanly is checked on a
 * maximized window, not assumed here. Bottom corners are not rounded:
 * labwc only builds top corner pieces, and clipping the client's own
 * pixels waits on a shader.
 */
bool
elavo_mods_squared(struct ssd *ssd, bool maximized, bool squared)
{
	(void)ssd;
	if (maximized && elavo_mods_round_maximized()) {
		return squared;
	}
	return maximized || squared;
}
