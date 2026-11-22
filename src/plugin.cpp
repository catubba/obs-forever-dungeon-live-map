// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The filter that goes on the source capturing the game. It lets the picture through untouched and,
// twice a second, cuts the square of the minimap out of it: the square is drawn alone on a small
// texture, copied to memory, and read at the next frame, so the graphics card is never waited for.
// The square goes to a thread of its own, which looks for it among the rooms of the dungeon.

#include "casc.h"
#include "interiors.h"
#include "minimap.h"
#include "rooms.h"
#include "shown.h"

#include <obs-module.h>
#include <util/platform.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-forever-dungeon-live-map", "en-US")

namespace fs = std::filesystem;

namespace {

constexpr int SMALLEST = 32, LARGEST = 1024; // pixels of the side of the minimap's square
constexpr uint64_t EVERY_NS = 500000000;      // between two cuts: the follower looks twice a second

struct Filter {
	obs_source_t *source;
	std::atomic<int> x{0}, y{0}, size{238};
	std::atomic<bool> save_wanted{false};
	std::atomic<bool> show_square{false}; // the square drawn over the game, to set it by eye

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
	std::string game; // the game folder chosen; empty for the one the installer says
	bool stop = false;
	std::thread worker;

	// For the source that draws the map.
	Shown shown;
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

// Writes a file under another name first: a file is either whole or not there.
bool write_whole(const fs::path &file, const std::string &text)
{
	fs::path part = file;
	part += ".part";
	{
		std::ofstream out(part, std::ios::binary);
		if (!(out << text))
			return false;
	}
	std::error_code error;
	fs::rename(part, file, error);
	return !error;
}

// The folder of the rooms of the installed build, made from the game's files the first time the build is
// met, read only on the game's side: <plugin config>/builds/<build key>/, holding rooms/, <map number>.txt
// for each dungeon and raid with rooms (rooms.h), and maps.txt, "number|name|kind" a line, written last.
// None without a game. chosen: the game folder set in the properties; empty, the one the installer says.
// ponytail: the folders of older builds are kept; remove them when a build is seen to replace them.
struct Installed {
	fs::path game, folder; // the game folder, and the folder of the rooms of its build
	Build build;
};
std::optional<Installed> installed_game(const std::string &chosen)
{
	std::vector<fs::path> where;
	if (!chosen.empty())
		where.push_back(fs::u8path(chosen));
	if (std::optional<fs::path> installed = registry_install_path())
		where.push_back(*installed);
	std::optional<fs::path> game = game_folder_from(where);
	if (!game)
		return std::nullopt;
	Build build = *installed_build(*game);
	char *config = obs_module_config_path("builds");
	fs::path folder = fs::u8path(config ? config : "builds") / build.build_key;
	bfree(config);
	return Installed{*game, folder, build};
}

std::optional<fs::path> rooms_of_build(const std::string &chosen)
{
	static std::mutex one_at_a_time; // two filters must not build the same folder together
	std::lock_guard<std::mutex> held(one_at_a_time);
	std::optional<Installed> found = installed_game(chosen);
	if (!found) {
		LOG(LOG_WARNING, "World of Warcraft: Forever not found: choose its folder in the filter's properties");
		return std::nullopt;
	}
	const fs::path &folder = found->folder, *game = &found->game;
	const Build &build = found->build;
	std::error_code error;
	if (fs::exists(folder / "maps.txt", error))
		return folder;

	LOG(LOG_INFO, "laying the rooms of build %s from %s", build.version.c_str(), game->u8string().c_str());
	uint64_t started = os_gettime_ns();
	try {
		GameFiles files(*game, sent_keys(*game));
		// A file still locked (the server has sent no key for it) is a file the map does without.
		ReadFile read = [&](uint32_t id) -> std::optional<Bytes> {
			try {
				return files.read(id);
			} catch (const EncryptedFile &) {
				return std::nullopt;
			}
		};
		auto opens = [&](const std::string &key) { return files.opens(key); };
		// These tables have parts the game keeps encrypted: read around the ones still locked.
		std::optional<Bytes> maps = files.read(MAP_FILE, true), pictures = files.read(ROOM_PICTURES_FILE, true);
		if (!maps || !pictures)
			throw std::runtime_error("the game has no table of its maps or of its room pictures");
		fs::create_directories(folder / "rooms");
		Converter converter(read, room_pictures(Table(std::move(*pictures), opens)), folder / "rooms");
		std::vector<MapEntry> list = map_entries(Table(std::move(*maps), opens));
		std::string index;
		for (size_t i = 0; i < list.size(); i++) {
			const MapEntry &map = list[i];
			std::vector<Laid> laid = converter.convert(map);
			if (!laid.empty()) { // no world file, or no room with a picture: not a dungeon to follow
				if (!write_whole(folder / (std::to_string(map.id) + ".txt"), pieces_text(laid)))
					throw std::runtime_error("cannot write in " + folder.u8string());
				index += std::to_string(map.id) + "|" + map.name + "|" + (map.raid ? "raid" : "dungeon") + "\n";
			}
			if ((i + 1) % 10 == 0 || i + 1 == list.size())
				LOG(LOG_INFO, "rooms laid: %zu of %zu maps", i + 1, list.size());
		}
		if (!write_whole(folder / "maps.txt", index))
			throw std::runtime_error("cannot write in " + folder.u8string());
		LOG(LOG_INFO, "rooms of build %s ready in %.1f s", build.version.c_str(), (os_gettime_ns() - started) / 1e9);
		return folder;
	} catch (const std::exception &e) {
		LOG(LOG_WARNING, "the rooms could not be laid: %s", e.what());
		return std::nullopt;
	}
}

// A new file for the places found in a dungeon, in the plugin's config folder: track-<dungeon>-<time>.csv,
// "ms,u,v,facing,likeness,took" a line, as the replay of a recorded run writes them.
// ponytail: for the trials of M2; M4 decides whether a track is kept at all.
FILE *open_track(int dungeon)
{
	char when[32];
	time_t now = time(nullptr);
	strftime(when, sizeof when, "%Y%m%d-%H%M%S", localtime(&now));
	std::string file = "track-" + std::to_string(dungeon) + "-" + when + ".csv";
	char *folder = obs_module_config_path("");
	char *path = obs_module_config_path(file.c_str());
	FILE *track = nullptr;
	if (folder && path) {
		os_mkdirs(folder);
		track = os_fopen(path, "w");
		if (track) {
			fputs("ms,u,v,facing,likeness,took\n", track);
			LOG(LOG_INFO, "places written to %s", path);
		}
	}
	bfree(folder);
	bfree(path);
	return track;
}

// The matching thread: builds the mosaics of the dungeon chosen, and looks for each square it is handed.
void work(Filter *f)
{
	int dungeon = -1, zoom = -1;
	std::string game = "\x01"; // none yet
	std::unique_ptr<Follower> follower;
	FILE *track = nullptr;
	std::unique_lock<std::mutex> held(f->lock);
	for (;;) {
		f->wake.wait(held, [&] {
			return f->stop || f->box || f->dungeon != dungeon || f->zoom != zoom || f->game != game;
		});
		if (f->stop) {
			if (track)
				fclose(track);
			return;
		}
		if (f->dungeon != dungeon || f->zoom != zoom || f->game != game) {
			dungeon = f->dungeon;
			zoom = f->zoom;
			game = f->game;
			held.unlock();
			// The rooms of the build are laid first, once, here: the drawing of OBS never waits for them.
			std::optional<fs::path> rooms = rooms_of_build(game);
			uint64_t started = os_gettime_ns();
			std::vector<Mosaic> mosaics;
			if (rooms && dungeon)
				mosaics = mosaics_from_files(*rooms / (std::to_string(dungeon) + ".txt"), *rooms / "rooms");
			follower = std::make_unique<Follower>(mosaics, widths_of(zoom));
			LOG(LOG_INFO, "dungeon %d, zoom %d: %zu mosaics in %.0f ms", dungeon, zoom, mosaics.size(),
			    (os_gettime_ns() - started) / 1e6);
			{
				std::lock_guard<std::mutex> showing(f->shown.lock);
				f->shown.mosaics = mosaics.empty() ? nullptr : std::make_shared<const std::vector<Mosaic>>(std::move(mosaics));
				f->shown.place.reset();
			}
			if (track)
				fclose(track);
			track = dungeon ? open_track(dungeon) : nullptr;
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
		{
			std::lock_guard<std::mutex> showing(f->shown.lock);
			f->shown.place = got;
		}
		double took = (os_gettime_ns() - started) / 1e6;
		// ponytail: a line a look, to follow the trial in the log; M4 shows it on the map instead.
		if (got)
			LOG(LOG_INFO, "at %.1f %.1f, level %d, likeness %.3f, facing %s, %.1f ms", got->u, got->v, got->level,
			    got->score, got->facing ? std::to_string(int(std::lround(*got->facing))).c_str() : "-", took);
		else
			LOG(LOG_INFO, "no place, %.1f ms", took);
		if (track) {
			if (got)
				fprintf(track, "%.0f,%.2f,%.2f,%s,%.3f,%.1f\n", ms, got->u, got->v,
					got->facing ? std::to_string(int(std::lround(*got->facing))).c_str() : "", got->score, took);
			else
				fprintf(track, "%.0f,,,,,%.1f\n", ms, took);
			fflush(track);
		}
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
	f->show_square = obs_data_get_bool(settings, "show");
	std::lock_guard<std::mutex> held(f->lock);
	f->dungeon = int(obs_data_get_int(settings, "dungeon"));
	f->zoom = int(std::clamp<long long>(obs_data_get_int(settings, "zoom"), 0, 5));
	f->game = obs_data_get_string(settings, "game");
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

// The dungeons and raids with rooms in the build already laid, by name; none before it is laid.
std::vector<std::pair<std::string, uint32_t>> known_maps(const std::string &chosen)
{
	std::vector<std::pair<std::string, uint32_t>> list;
	std::optional<Installed> found = installed_game(chosen);
	if (!found)
		return list;
	std::ifstream in(found->folder / "maps.txt");
	std::map<std::string, int> seen;
	std::vector<std::tuple<std::string, uint32_t, bool>> rows;
	for (std::string line; std::getline(in, line);) {
		size_t a = line.find('|'), b = line.rfind('|');
		if (a == std::string::npos || b <= a)
			continue;
		std::string name = line.substr(a + 1, b - a - 1);
		rows.emplace_back(name, uint32_t(std::stoul(line.substr(0, a))), line.substr(b + 1) == "raid");
		seen[name]++;
	}
	for (const auto &[name, id, raid] : rows) {
		std::string label = name + (raid ? " (" + std::string(obs_module_text("Raid")) + ")" : "");
		if (seen[name] > 1) // the same name twice, as Naxxramas: told apart by the map's number
			label += " #" + std::to_string(id);
		list.emplace_back(label, id);
	}
	std::sort(list.begin(), list.end());
	return list;
}

obs_properties_t *filter_properties(void *data)
{
	auto *f = static_cast<Filter *>(data);
	obs_properties_t *props = obs_properties_create();

	std::string game;
	{
		std::lock_guard<std::mutex> held(f->lock);
		game = f->game;
	}
	obs_property_t *dungeons = obs_properties_add_list(props, "dungeon", obs_module_text("Dungeon"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_INT);
	std::vector<std::pair<std::string, uint32_t>> maps = known_maps(game);
	obs_property_list_add_int(dungeons, obs_module_text(maps.empty() ? "DungeonsNotYet" : "DungeonNone"), 0);
	for (const auto &[label, id] : maps)
		obs_property_list_add_int(dungeons, label.c_str(), id);

	obs_property_t *zooms = obs_properties_add_list(props, "zoom", obs_module_text("Zoom"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int zoom = 0; zoom < 6; zoom++) {
		char label[128];
		std::snprintf(label, sizeof label, obs_module_text("ZoomItem"), zoom + 1, int(INDOOR_YARDS[zoom]));
		obs_property_list_add_int(zooms, label, zoom);
	}

	// The square of the minimap, moved with sliders while it is drawn over the game.
	obs_source_t *parent = obs_filter_get_parent(f->source);
	int width = parent ? int(obs_source_get_width(parent)) : 0, height = parent ? int(obs_source_get_height(parent)) : 0;
	obs_properties_add_bool(props, "show", obs_module_text("ShowSquare"));
	obs_properties_add_int_slider(props, "x", obs_module_text("MinimapX"), 0, width > 0 ? width - SMALLEST : 16384, 1);
	obs_properties_add_int_slider(props, "y", obs_module_text("MinimapY"), 0, height > 0 ? height - SMALLEST : 16384, 1);
	obs_properties_add_int_slider(props, "size", obs_module_text("MinimapSize"), SMALLEST, LARGEST, 1);
	obs_properties_add_path(props, "game", obs_module_text("GameFolder"), OBS_PATH_DIRECTORY, nullptr, nullptr);
	obs_properties_add_button2(
		props, "save", obs_module_text("SaveMinimap"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			static_cast<Filter *>(data)->save_wanted = true;
			return false;
		},
		data);
	return props;
}

// The square of the minimap over the game, and the ring the matching looks inside (KEEP of minimap.cpp): the
// square is right when its sides touch the round minimap.
void draw_square(int x, int y, int size)
{
	constexpr double PI = 3.14159265358979323846, KEEP = 0.8;
	constexpr uint32_t COLOR = 0xFF00FF40; // green, seen on the game
	double t = std::max(2.0, size / 80.0), half = size / 2.0, cx = x + half, cy = y + half;
	std::vector<std::pair<double, double>> points;
	auto quad = [&](double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
		points.insert(points.end(), {{x0, y0}, {x1, y1}, {x2, y2}, {x0, y0}, {x2, y2}, {x3, y3}});
	};
	quad(x, y, x + size, y, x + size, y + t, x, y + t);                         // top
	quad(x, y + size - t, x + size, y + size - t, x + size, y + size, x, y + size); // bottom
	quad(x, y, x + t, y, x + t, y + size, x, y + size);                         // left
	quad(x + size - t, y, x + size, y, x + size, y + size, x + size - t, y + size); // right
	for (int i = 0; i < 64; i++) {
		double a = 2 * PI * i / 64, b = 2 * PI * (i + 1) / 64, r = half * KEEP, s = r - t;
		quad(cx + r * std::cos(a), cy + r * std::sin(a), cx + r * std::cos(b), cy + r * std::sin(b), cx + s * std::cos(b),
		     cy + s * std::sin(b), cx + s * std::cos(a), cy + s * std::sin(a));
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), COLOR);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (const auto &[px, py] : points)
			gs_vertex2f(float(px), float(py));
		gs_render_stop(GS_TRIS);
	}
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
	if (f->show_square)
		draw_square(f->x, f->y, f->size);
}

} // namespace

Shown *shown_of(obs_source_t *filter)
{
	if (!filter || strcmp(obs_source_get_unversioned_id(filter), FILTER_ID) != 0)
		return nullptr;
	auto *f = static_cast<Filter *>(obs_obj_get_data(filter));
	return f ? &f->shown : nullptr;
}

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = FILTER_ID;
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
	register_map_source();

	LOG(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}
