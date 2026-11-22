// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The filter that goes on the source capturing the game. It lets the picture through untouched and,
// when asked, cuts the square of the minimap out of it: the square is drawn alone on a small texture,
// copied to memory, and read at the next frame, so the graphics card is never waited for.

#include "minimap.h"

#include <obs-module.h>
#include <util/platform.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-forever-dungeon-live-map", "en-US")

namespace {

constexpr int SMALLEST = 32, LARGEST = 1024; // pixels of the side of the minimap's square

struct Filter {
	obs_source_t *source;
	std::atomic<int> x{0}, y{0}, size{238};
	std::atomic<bool> save_wanted{false};
	gs_texrender_t *texrender = nullptr; // graphics thread only, as the two below
	gs_stagesurf_t *stage = nullptr;
	int staged = 0; // the side of the square copied at the last frame, still to be read; 0 for none
};

// A PNG of a box: rows unfiltered, deflated with zlib.
bool write_png(const std::string &path, const Box &box)
{
	std::vector<uint8_t> raw;
	raw.reserve(size_t(box.size) * (box.size * 3 + 1));
	for (int y = 0; y < box.size; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), box.rgb.begin() + size_t(y) * box.size * 3, box.rgb.begin() + size_t(y + 1) * box.size * 3);
	}
	uLongf packed_size = compressBound(uLong(raw.size()));
	std::vector<uint8_t> packed(packed_size);
	if (compress2(packed.data(), &packed_size, raw.data(), uLong(raw.size()), 9) != Z_OK)
		return false;
	packed.resize(packed_size);

	FILE *out = os_fopen(path.c_str(), "wb");
	if (!out)
		return false;
	auto u32 = [](uint32_t n) { return std::vector<uint8_t>{uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)}; };
	auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
		std::vector<uint8_t> body(type, type + 4);
		body.insert(body.end(), data.begin(), data.end());
		std::vector<uint8_t> length = u32(uint32_t(data.size())), crc = u32(uint32_t(crc32(0, body.data(), uInt(body.size()))));
		fwrite(length.data(), 1, 4, out);
		fwrite(body.data(), 1, body.size(), out);
		fwrite(crc.data(), 1, 4, out);
	};
	static const uint8_t signature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
	fwrite(signature, 1, 8, out);
	std::vector<uint8_t> head = u32(box.size), height = u32(box.size);
	head.insert(head.end(), height.begin(), height.end());
	head.insert(head.end(), {8, 2, 0, 0, 0}); // 8 bits, red green blue, deflate, no filter, not interlaced
	chunk("IHDR", head);
	chunk("IDAT", packed);
	chunk("IEND", {});
	return fclose(out) == 0;
}

void save(const Box &box)
{
	char *folder = obs_module_config_path("");
	char *path = obs_module_config_path("minimap.png");
	if (folder && path) {
		os_mkdirs(folder);
		if (write_png(path, box))
			blog(LOG_INFO, "[obs-forever-dungeon-live-map] minimap saved to %s", path);
		else
			blog(LOG_WARNING, "[obs-forever-dungeon-live-map] could not write %s", path);
	}
	bfree(folder);
	bfree(path);
}

// The square copied at the last frame, as a box; nothing when the copy cannot be read.
void read_staged(Filter *f)
{
	int size = f->staged;
	f->staged = 0;
	uint8_t *data;
	uint32_t linesize;
	if (!gs_stagesurface_map(f->stage, &data, &linesize))
		return;
	Box box;
	box.size = size;
	box.rgb.resize(size_t(size) * size * 3);
	for (int y = 0; y < size; y++) {
		const uint8_t *row = data + size_t(y) * linesize;
		for (int x = 0; x < size; x++) {
			uint8_t *to = &box.rgb[(size_t(y) * size + x) * 3];
			to[0] = row[x * 4 + 2]; // blue, green, red, alpha on the card
			to[1] = row[x * 4 + 1];
			to[2] = row[x * 4];
		}
	}
	gs_stagesurface_unmap(f->stage);
	save(box);
}

// Draws the square of the minimap alone on a texture of its size, and copies it to memory.
void cut(Filter *f)
{
	obs_source_t *target = obs_filter_get_target(f->source), *parent = obs_filter_get_parent(f->source);
	if (!target || !parent)
		return;
	int size = f->size, x = f->x, y = f->y;
	if (!f->texrender)
		f->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_texrender_reset(f->texrender);
	if (!gs_texrender_begin(f->texrender, size, size))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(float(x), float(x + size), float(y), float(y + size), -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	uint32_t flags = obs_source_get_output_flags(parent);
	if (target == parent && !(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC)))
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_blend_state_pop();
	gs_texrender_end(f->texrender);

	if (f->stage && int(gs_stagesurface_get_width(f->stage)) != size) {
		gs_stagesurface_destroy(f->stage);
		f->stage = nullptr;
	}
	if (!f->stage)
		f->stage = gs_stagesurface_create(size, size, GS_BGRA);
	gs_stage_texture(f->stage, gs_texrender_get_texture(f->texrender));
	f->staged = size;
}

const char *filter_name(void *)
{
	return obs_module_text("MapFilter");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<Filter *>(data);
	f->x = int(obs_data_get_int(settings, "x"));
	f->y = int(obs_data_get_int(settings, "y"));
	f->size = int(std::clamp<long long>(obs_data_get_int(settings, "size"), SMALLEST, LARGEST));
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new Filter{source};
	filter_update(f, settings);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<Filter *>(data);
	obs_enter_graphics();
	gs_texrender_destroy(f->texrender);
	gs_stagesurface_destroy(f->stage);
	obs_leave_graphics();
	delete f;
}

void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "size", 238);
}

obs_properties_t *filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "x", obs_module_text("MinimapX"), 0, 16384, 1);
	obs_properties_add_int(props, "y", obs_module_text("MinimapY"), 0, 16384, 1);
	obs_properties_add_int(props, "size", obs_module_text("MinimapSize"), SMALLEST, LARGEST, 1);
	obs_properties_add_button2(
		props, "save", obs_module_text("SaveMinimap"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			static_cast<Filter *>(data)->save_wanted = true;
			return false;
		},
		data);
	return props;
}

void filter_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<Filter *>(data);
	if (f->staged)
		read_staged(f);
	if (f->save_wanted.exchange(false))
		cut(f);
	obs_source_skip_video_filter(f->source);
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
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.video_render = filter_render;
	obs_register_source(&info);

	blog(LOG_INFO, "[obs-forever-dungeon-live-map] loaded (version %s)", PLUGIN_VERSION);
	return true;
}
