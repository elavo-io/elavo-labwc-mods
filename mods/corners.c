/* SPDX-License-Identifier: GPL-2.0-only */
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include "ssd.h"
#include "ssd-internal.h"
#include "view.h"

/*
 * With window.maximizedCorners: yes, maximized and tiled windows keep the
 * title bar's rounded corners, the same as a floating window. labwc squares
 * them for both. The only square case kept is a window too narrow for two
 * corner pieces, where labwc can only draw a plain bar. labwc still drops
 * the border on maximize; clip-corners.c rounds the bottom corners and
 * redraws the border around them.
 */
bool
elavo_mods_squared(struct ssd *ssd, bool maximized, bool squared)
{
	if (!elavo_mods_round_maximized()) {
		return maximized || squared;
	}
	/*
	 * Width 0 is a window not sized yet: a window rule that tiles or
	 * maximizes at map time gets here before the first commit. Calling
	 * that narrow squared the title bar for good, since labwc reruns this
	 * only when the maximized or tiled state changes.
	 */
	int width = ssd->view->current.width;
	return width > 0 && width < ssd_get_corner_width() * 2;
}
