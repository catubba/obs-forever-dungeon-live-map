// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The source that draws the map: the rooms of the level the character is on, a pixel a yard, and a dot
// with an arrow where the filter finds the character. The dot goes when the filter finds no place: it does
// not guess. The source is as large as the dungeon; the scene scales it.
//
// The filter works only while OBS draws the source it is on, and the game capture stops when nothing shows
// it. So the map keeps that source shown and draws it, out of sight, on a texture of one pixel: the scene
// can hide the game and show the map alone.

#include "shown.h"

#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace {

constexpr const char *SOURCE_ID = "forever_dungeon_live_map_source";
constexpr char BETWEEN = '\x1f';            // between the name of a source and of its filter, in the settings
constexpr uint64_t LOOK_AGAIN_NS = 1000000000; // between two looks for the filter while none is found
constexpr double PI = 3.14159265358979323846;
constexpr uint32_t DOT = 0xFFFF8C00, EDGE = 0xFF000000; // orange, with a black edge: seen on the rooms and off them

struct MapSource {
	obs_source_t *source;
	std::mutex lock;
	std::string chosen; // "<source><BETWEEN><filter>"; empty for the first filter of this plugin found
	std::atomic<bool> chosen_anew{true};
	std::atomic<uint32_t> width{0}, height{0};

	// Graphics thread only.
	obs_weak_source_t *filter = nullptr;
	obs_weak_source_t *driven = nullptr; // the source of the filter, kept shown by the map
	gs_texrender_t *out_of_sight = nullptr;
	uint64_t looked_at = 0;
	std::shared_ptr<const std::vector<Mosaic>> drawn; // the mosaics the textures are of
	std::vector<gs_texture_t *> textures;
	size_t level = 0; // the mosaic shown: the level the character was last found on
};

// Every filter of this plugin: the name of its source and its own.
std::vector<std::pair<std::string, std::string>> our_filters()
{
	std::vector<std::pair<std::string, std::string>> list;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			obs_source_enum_filters(
				source,
				[](obs_source_t *parent, obs_source_t *child, void *param) {
					if (strcmp(obs_source_get_unversioned_id(child), FILTER_ID) == 0)
						static_cast<std::vector<std::pair<std::string, std::string>> *>(param)->emplace_back(
							obs_source_get_name(parent), obs_source_get_name(child));
				},
				param);
			return true;
		},
		&list);
	return list;
}

// The filter chosen, with a reference the caller releases; null when it is not there.
obs_source_t *find_filter(const std::string &chosen)
{
	std::string source, filter;
	if (chosen.empty()) {
		std::vector<std::pair<std::string, std::string>> all = our_filters();
		if (all.empty())
			return nullptr;
		source = all[0].first;
		filter = all[0].second;
	} else {
		size_t at = chosen.find(BETWEEN);
		source = chosen.substr(0, at);
		filter = at == std::string::npos ? "" : chosen.substr(at + 1);
	}
	obs_source_t *parent = obs_get_source_by_name(source.c_str());
	obs_source_t *found = parent ? obs_source_get_filter_by_name(parent, filter.c_str()) : nullptr;
	obs_source_release(parent);
	return found;
}

// Keeps the source of the filter shown, and draws it out of sight so that its filter looks at the game.
void drive(MapSource *s, obs_source_t *filter)
{
	obs_source_t *parent = filter ? obs_filter_get_parent(filter) : nullptr;
	obs_source_t *was = s->driven ? obs_weak_source_get_source(s->driven) : nullptr;
	if (was != parent) {
		if (was)
			obs_source_dec_showing(was);
		obs_weak_source_release(s->driven);
		s->driven = parent ? obs_source_get_weak_source(parent) : nullptr;
		if (parent)
			obs_source_inc_showing(parent);
	}
	obs_source_release(was);
	if (!parent)
		return;
	if (!s->out_of_sight)
		s->out_of_sight = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_texrender_reset(s->out_of_sight);
	uint32_t width = obs_source_get_width(parent), height = obs_source_get_height(parent);
	if (width && height && gs_texrender_begin(s->out_of_sight, 1, 1)) {
		gs_ortho(0.0f, float(width), 0.0f, float(height), -100.0f, 100.0f);
		obs_source_video_render(parent);
		gs_texrender_end(s->out_of_sight);
	}
}

void forget_textures(MapSource *s)
{
	for (gs_texture_t *t : s->textures)
		gs_texture_destroy(t);
	s->textures.clear();
}

// A texture for each mosaic: its grey, see-through where there is nothing.
void make_textures(MapSource *s, const std::shared_ptr<const std::vector<Mosaic>> &mosaics)
{
	forget_textures(s);
	s->drawn = mosaics;
	s->level = mosaics ? mosaics->size() - 1 : 0;
	for (const Mosaic &m : mosaics ? *mosaics : std::vector<Mosaic>{}) {
		std::vector<uint8_t> bgra(m.grey.size() * 4);
		for (size_t i = 0; i < m.grey.size(); i++) {
			uint8_t g = m.grey[i];
			bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = g;
			bgra[i * 4 + 3] = g ? 255 : 0;
		}
		const uint8_t *data = bgra.data();
		s->textures.push_back(gs_texture_create(uint32_t(m.width), uint32_t(m.height), GS_BGRA, 1, &data, 0));
	}
	const Mosaic *top = mosaics && !mosaics->empty() ? &mosaics->back() : nullptr;
	s->width = top ? uint32_t(top->width) : 0;
	s->height = top ? uint32_t(top->height) : 0;
}

// Triangles in one color, from points around (x, y).
void draw_triangles(uint32_t color, double x, double y, const std::vector<std::pair<double, double>> &points)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (const auto &[px, py] : points)
			gs_vertex2f(float(x + px), float(y + py));
		gs_render_stop(GS_TRIS);
	}
}

std::vector<std::pair<double, double>> disc(double radius)
{
	std::vector<std::pair<double, double>> points;
	for (int i = 0; i < 24; i++) {
		double a = 2 * PI * i / 24, b = 2 * PI * (i + 1) / 24;
		points.insert(points.end(), {{0, 0}, {radius * std::cos(a), radius * std::sin(a)}, {radius * std::cos(b), radius * std::sin(b)}});
	}
	return points;
}

// The arrow ahead of the dot. facing: degrees clockwise from north; north is up, east right.
std::vector<std::pair<double, double>> arrow(double facing, double grow)
{
	double t = facing * PI / 180, ax = std::sin(t), ay = -std::cos(t); // ahead
	double sx = -ay, sy = ax;                                           // to the side
	double tip = 17 + grow, back = 6 - grow / 2, half = 6 + grow;
	return {{ax * tip, ay * tip}, {ax * back + sx * half, ay * back + sy * half}, {ax * back - sx * half, ay * back - sy * half}};
}

const char *map_name(void *)
{
	return obs_module_text("MapSource");
}

void map_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<MapSource *>(data);
	std::lock_guard<std::mutex> held(s->lock);
	s->chosen = obs_data_get_string(settings, "filter");
	s->chosen_anew = true;
}

void *map_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new MapSource{source};
	map_update(s, settings);
	return s;
}

void map_destroy(void *data)
{
	auto *s = static_cast<MapSource *>(data);
	if (obs_source_t *driven = s->driven ? obs_weak_source_get_source(s->driven) : nullptr) {
		obs_source_dec_showing(driven);
		obs_source_release(driven);
	}
	obs_weak_source_release(s->driven);
	obs_weak_source_release(s->filter);
	obs_enter_graphics();
	forget_textures(s);
	gs_texrender_destroy(s->out_of_sight);
	obs_leave_graphics();
	delete s;
}

obs_properties_t *map_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(props, "filter", obs_module_text("MapFilterChosen"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("MapFilterFirst"), "");
	for (const auto &[source, filter] : our_filters())
		obs_property_list_add_string(list, (source + ": " + filter).c_str(), (source + BETWEEN + filter).c_str());
	return props;
}

uint32_t map_width(void *data)
{
	return static_cast<MapSource *>(data)->width;
}

uint32_t map_height(void *data)
{
	return static_cast<MapSource *>(data)->height;
}

void map_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<MapSource *>(data);
	if (s->chosen_anew.exchange(false)) {
		obs_weak_source_release(s->filter);
		s->filter = nullptr;
		s->looked_at = 0;
	}
	obs_source_t *filter = s->filter ? obs_weak_source_get_source(s->filter) : nullptr;
	uint64_t now = os_gettime_ns();
	if (!filter && now - s->looked_at >= LOOK_AGAIN_NS) {
		s->looked_at = now;
		std::string chosen;
		{
			std::lock_guard<std::mutex> held(s->lock);
			chosen = s->chosen;
		}
		filter = find_filter(chosen);
		obs_weak_source_release(s->filter);
		s->filter = filter ? obs_source_get_weak_source(filter) : nullptr;
	}
	drive(s, filter);
	std::shared_ptr<const std::vector<Mosaic>> mosaics;
	std::optional<Inside> place;
	if (Shown *shown = shown_of(filter)) {
		std::lock_guard<std::mutex> held(shown->lock);
		mosaics = shown->mosaics;
		place = shown->place;
	}
	obs_source_release(filter);
	if (mosaics != s->drawn)
		make_textures(s, mosaics);
	if (s->textures.empty())
		return;

	// The level the character is on; in a dungeon with one level, or while it is not found, the one shown last.
	if (place && place->level >= 0)
		s->level = std::min(size_t(place->level), s->textures.size() - 1);
	const Mosaic &top = mosaics->back(), &m = (*mosaics)[s->level];
	gs_effect_t *plain = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(plain, "image"), s->textures[s->level]);
	gs_matrix_push();
	gs_matrix_translate3f(float(m.u0 - top.u0), float(m.v0 - top.v0), 0);
	while (gs_effect_loop(plain, "Draw"))
		gs_draw_sprite(s->textures[s->level], 0, uint32_t(m.width), uint32_t(m.height));
	gs_matrix_pop();

	if (!place)
		return; // not found: the dot goes, it does not guess
	double x = place->u - top.u0, y = place->v - top.v0;
	if (place->facing)
		draw_triangles(EDGE, x, y, arrow(*place->facing, 2));
	draw_triangles(EDGE, x, y, disc(7.5));
	if (place->facing)
		draw_triangles(DOT, x, y, arrow(*place->facing, 0));
	draw_triangles(DOT, x, y, disc(5.5));
}

} // namespace

void register_map_source()
{
	obs_source_info info = {};
	info.id = SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = map_name;
	info.create = map_create;
	info.destroy = map_destroy;
	info.update = map_update;
	info.get_properties = map_properties;
	info.get_width = map_width;
	info.get_height = map_height;
	info.video_render = map_render;
	obs_register_source(&info);
}
