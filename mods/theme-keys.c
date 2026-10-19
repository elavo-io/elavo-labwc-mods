/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "elavo-mods.h"
#include "elavo-mods-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>
#include "common/dir.h"
#include "common/parse-bool.h"
#include "config/rcxml.h"
#include "labwc.h"

static bool round_maximized;
static bool shadows_on_maximized;
static bool blur_enabled = true;
static int blur_levels = 3;

bool
elavo_mods_round_maximized(void)
{
	return round_maximized;
}

bool
elavo_mods_shadows_on_maximized(void)
{
	return shadows_on_maximized;
}

bool
elavo_mods_blur_enabled(void)
{
	return blur_enabled;
}

int
elavo_mods_blur_levels(void)
{
	return blur_levels;
}

static void
trim(char *s)
{
	char *end;
	while (*s && isspace((unsigned char)*s)) {
		memmove(s, s + 1, strlen(s));
	}
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
}

static void
apply_line(char *line)
{
	char *hash = strchr(line, '#');
	if (hash) {
		*hash = '\0';
	}
	char *colon = strchr(line, ':');
	if (!colon) {
		return;
	}
	*colon = '\0';
	trim(line);
	trim(colon + 1);
	if (!strcmp(line, "window.cornerRadius")) {
		rc.corner_radius = atoi(colon + 1);
	} else if (!strcmp(line, "window.maximizedCorners")) {
		int v = parse_bool(colon + 1, -1);
		if (v >= 0) {
			round_maximized = v;
		}
	} else if (!strcmp(line, "window.dropShadows")) {
		set_bool(colon + 1, &rc.shadows_enabled);
	} else if (!strcmp(line, "window.dropShadowsOnTiled")) {
		set_bool(colon + 1, &rc.shadows_on_tiled);
	} else if (!strcmp(line, "window.dropShadowsOnMaximized")) {
		int v = parse_bool(colon + 1, -1);
		if (v >= 0) {
			shadows_on_maximized = v;
		}
	} else if (!strcmp(line, "window.blur.strength")) {
		int n = atoi(colon + 1);
		if (n < 1) {
			n = 1;
		}
		if (n > 5) {
			n = 5;
		}
		blur_levels = n;
	}
}

static void
read_paths(struct wl_list *paths)
{
	bool should_merge = rc.merge_config;
	struct wl_list *(*iter)(struct wl_list *) =
		should_merge ? paths_get_prev : paths_get_next;

	for (struct wl_list *elm = iter(paths); elm != paths; elm = iter(elm)) {
		struct path *path = wl_container_of(elm, path, link);
		FILE *stream = fopen(path->string, "r");
		if (!stream) {
			continue;
		}
		char *line = NULL;
		size_t len = 0;
		while (getline(&line, &len, stream) != -1) {
			apply_line(line);
		}
		free(line);
		fclose(stream);
		if (!should_merge) {
			break;
		}
	}
}

static bool
file_has_blur(const char *path, bool *enabled)
{
	FILE *stream = fopen(path, "r");
	if (!stream) {
		return false;
	}
	char *line = NULL;
	size_t len = 0;
	bool found = false;
	while (getline(&line, &len, stream) != -1) {
		if (strcasestr(line, "<blur>no</blur>")) {
			*enabled = false;
			found = true;
		} else if (strcasestr(line, "<blur>yes</blur>")) {
			*enabled = true;
			found = true;
		}
	}
	free(line);
	fclose(stream);
	return found;
}

static void
read_blur_switch(void)
{
	const char *home = getenv("HOME");
	const char *config_home = getenv("XDG_CONFIG_HOME");
	char user_path[512];
	bool enabled = true;
	bool found = false;

	user_path[0] = '\0';
	if (rc.config_dir && rc.config_dir[0]) {
		snprintf(user_path, sizeof(user_path), "%s/rc.xml", rc.config_dir);
		found = file_has_blur(user_path, &enabled);
	}
	if (!found && config_home && config_home[0]) {
		snprintf(user_path, sizeof(user_path), "%s/labwc/rc.xml", config_home);
		found = file_has_blur(user_path, &enabled);
	} else if (!found && home && home[0]) {
		snprintf(user_path, sizeof(user_path), "%s/.config/labwc/rc.xml", home);
		found = file_has_blur(user_path, &enabled);
	}
	if (!found) {
		const char *dirs = getenv("XDG_CONFIG_DIRS");
		if (!dirs || !dirs[0]) {
			dirs = "/etc/xdg";
		}
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s", dirs);
		char *save = NULL;
		for (char *dir = strtok_r(buf, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
			char path[512];
			snprintf(path, sizeof(path), "%s/labwc/rc.xml", dir);
			if (file_has_blur(path, &enabled)) {
				found = true;
				break;
			}
		}
	}
	if (found) {
		blur_enabled = enabled;
	}
}

void
elavo_mods_theme_loaded(void)
{
	round_maximized = false;
	shadows_on_maximized = false;
	blur_levels = 3;

	if (rc.theme_name) {
		struct wl_list paths;
		paths_theme_create(&paths, rc.theme_name, "themerc");
		read_paths(&paths);
		paths_destroy(&paths);
	}

	struct wl_list override;
	paths_config_create(&override, "themerc-override");
	read_paths(&override);
	paths_destroy(&override);

	read_blur_switch();
	elavo_mods_shadows_theme_changed();
}
