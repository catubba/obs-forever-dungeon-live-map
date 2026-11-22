// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The filter that goes on the source capturing the game. For now it lets the picture through untouched.

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-forever-dungeon-live-map", "en-US")

namespace {

struct Filter {
	obs_source_t *source;
};

const char *filter_name(void *)
{
	return obs_module_text("MapFilter");
}

void *filter_create(obs_data_t *, obs_source_t *source)
{
	return new Filter{source};
}

void filter_destroy(void *data)
{
	delete static_cast<Filter *>(data);
}

void filter_render(void *data, gs_effect_t *)
{
	obs_source_skip_video_filter(static_cast<Filter *>(data)->source);
}

} // namespace

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = "forever_dungeon_live_map_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.video_render = filter_render;
	obs_register_source(&info);

	blog(LOG_INFO, "[obs-forever-dungeon-live-map] loaded (version %s)", PLUGIN_VERSION);
	return true;
}
