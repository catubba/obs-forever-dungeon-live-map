// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The filter that goes on the source capturing the game. It lets the picture through untouched and,
// twice a second, cuts the square of the minimap out of it: the square is drawn alone on a small
// texture, copied to memory, and read at the next frame, so the graphics card is never waited for.
// The square goes to a thread of its own, which looks for it among the rooms of the dungeon.

#include "minimap.h"
#include "twinmap.h"

#include <obs-module.h>
#include <util/platform.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-forever-dungeon-live-map", "en-US")

#define LOG(level, format, ...) blog(level, "[obs-forever-dungeon-live-map] " format, ##__VA_ARGS__)

namespace {

constexpr int SMALLEST = 32, LARGEST = 1024; // pixels of the side of the minimap's square
constexpr uint64_t EVERY_NS = 500000000;      // between two cuts: the follower looks twice a second

struct Filter {
	obs_source_t *source;
	std::atomic<int> x{0}, y{0}, size{238};
	std::atomic<bool> save_wanted{false};

	// Graphics thread only.
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stage = nullptr;
	int staged = 0;          // the side of the square copied at the last frame, still to be read; 0 for none
	uint64_t staged_at = 0;  // when it was cut, in nanoseconds
	bool saving = false;     // the square still to be read is to be saved as well

	// Shared with the matching thread, under `lock`.
	std::mutex lock;
	std::condition_variable wake;
	std::optional<Box> box; // the latest square not looked at yet; a newer one takes its place
	double box_ms = 0;
	int dungeon = 0, zoom = 0;
	bool stop = false;
	std::thread worker;
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
			LOG(LOG_INFO, "minimap saved to %s", path);
		else
			LOG(LOG_WARNING, "could not write %s", path);
	}
	bfree(folder);
	bfree(path);
}

// The square copied at the last frame, as a box; none when the copy cannot be read.
std::optional<Box> read_staged(Filter *f)
{
	int size = f->staged;
	f->staged = 0;
	uint8_t *data;
	uint32_t linesize;
	if (!gs_stagesurface_map(f->stage, &data, &linesize))
		return std::nullopt;
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
	return box;
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
	f->staged_at = os_gettime_ns();
}

// The rooms of a dungeon from what Twinmap Forever took out of the game: its interiors.json and rooms
// folder, in the first build folder under %LOCALAPPDATA%\Twinmap\tiles.
// ponytail: Twinmap's data until M3 reads the game's files; the open ground is left out (not in v1).
std::vector<Mosaic> twinmap_dungeon(int id)
{
	namespace fs = std::filesystem;
	const char *local = std::getenv("LOCALAPPDATA");
	std::error_code error;
	fs::path folder;
	if (local)
		for (const fs::directory_entry &entry : fs::directory_iterator(fs::path(local) / "Twinmap" / "tiles", error))
			if (entry.is_directory()) {
				folder = entry.path();
				break;
			}
	if (folder.empty()) {
		LOG(LOG_WARNING, "no Twinmap data under %%LOCALAPPDATA%%\\Twinmap\\tiles");
		return {};
	}
	std::vector<Piece> pieces;
	obs_data_t *all = obs_data_create_from_json_file((folder / "interiors.json").u8string().c_str());
	obs_data_array_t *maps = all ? obs_data_get_array(all, "maps") : nullptr;
	for (size_t i = 0; maps && i < obs_data_array_count(maps); i++) {
		obs_data_t *map = obs_data_array_item(maps, i);
		if (obs_data_get_int(map, "id") == id && strcmp(obs_data_get_string(map, "kind"), "world") != 0) {
			obs_data_array_t *list = obs_data_get_array(map, "pieces");
			for (size_t k = 0; k < obs_data_array_count(list); k++) {
				obs_data_t *p = obs_data_array_item(list, k);
				pieces.push_back({uint32_t(obs_data_get_int(p, "file")), obs_data_get_double(p, "u"),
						  obs_data_get_double(p, "v"), obs_data_get_double(p, "width"),
						  obs_data_get_double(p, "height"), obs_data_get_double(p, "angle"),
						  int(obs_data_get_int(p, "level"))});
				obs_data_release(p);
			}
			obs_data_array_release(list);
		}
		obs_data_release(map);
	}
	obs_data_array_release(maps);
	obs_data_release(all);
	return mosaics_from_rooms(pieces, (folder / "rooms").u8string());
}

// The matching thread: builds the mosaics of the dungeon chosen, and looks for each square it is handed.
void work(Filter *f)
{
	int dungeon = -1, zoom = -1;
	std::unique_ptr<Follower> follower;
	std::unique_lock<std::mutex> held(f->lock);
	for (;;) {
		f->wake.wait(held, [&] { return f->stop || f->box || f->dungeon != dungeon || f->zoom != zoom; });
		if (f->stop)
			return;
		if (f->dungeon != dungeon || f->zoom != zoom) {
			dungeon = f->dungeon;
			zoom = f->zoom;
			held.unlock();
			uint64_t started = os_gettime_ns();
			std::vector<Mosaic> mosaics = dungeon ? twinmap_dungeon(dungeon) : std::vector<Mosaic>{};
			follower = std::make_unique<Follower>(mosaics, widths_of(zoom));
			LOG(LOG_INFO, "dungeon %d, zoom %d: %zu mosaics in %.0f ms", dungeon, zoom, mosaics.size(),
			    (os_gettime_ns() - started) / 1e6);
			held.lock();
			continue;
		}
		Box box = std::move(*f->box);
		f->box.reset();
		double ms = f->box_ms;
		if (!dungeon)
			continue; // no dungeon chosen: nothing to look for
		held.unlock();
		uint64_t started = os_gettime_ns();
		std::optional<Inside> got = follower->look(box, ms);
		double took = (os_gettime_ns() - started) / 1e6;
		// ponytail: a line a look, to follow the trial in the log; M4 shows it on the map instead.
		if (got)
			LOG(LOG_INFO, "at %.1f %.1f, level %d, likeness %.3f, facing %s, %.1f ms", got->u, got->v, got->level,
			    got->score, got->facing ? std::to_string(int(std::lround(*got->facing))).c_str() : "-", took);
		else
			LOG(LOG_INFO, "no place, %.1f ms", took);
		held.lock();
	}
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
	std::lock_guard<std::mutex> held(f->lock);
	f->dungeon = int(obs_data_get_int(settings, "dungeon"));
	f->zoom = int(std::clamp<long long>(obs_data_get_int(settings, "zoom"), 0, 5));
	f->wake.notify_one();
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new Filter{source};
	filter_update(f, settings);
	f->worker = std::thread(work, f);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<Filter *>(data);
	{
		std::lock_guard<std::mutex> held(f->lock);
		f->stop = true;
		f->wake.notify_one();
	}
	f->worker.join();
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
	obs_properties_add_int(props, "dungeon", obs_module_text("Dungeon"), 0, 100000, 1);
	obs_properties_add_int(props, "zoom", obs_module_text("Zoom"), 0, 5, 1);
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
	if (f->staged) {
		uint64_t at = f->staged_at;
		std::optional<Box> box = read_staged(f);
		if (box && f->saving)
			save(*box);
		f->saving = false;
		if (box) {
			std::lock_guard<std::mutex> held(f->lock);
			f->box = std::move(box);
			f->box_ms = at / 1e6;
			f->wake.notify_one();
		}
	}
	uint64_t now = os_gettime_ns();
	bool save_now = f->save_wanted.exchange(false);
	if (save_now || now - f->staged_at >= EVERY_NS) {
		cut(f);
		f->saving = save_now && f->staged;
	}
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

	LOG(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}
