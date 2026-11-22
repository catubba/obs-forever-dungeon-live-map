// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the filter that follows the character hands to the source that draws the map.

#pragma once

#include "minimap.h"

#include <obs-module.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#define LOG(level, format, ...) blog(level, "[obs-forever-dungeon-live-map] " format, ##__VA_ARGS__)

/** The id of the filter, as OBS knows it. */
constexpr const char *FILTER_ID = "forever_dungeon_live_map_filter";

struct Shown {
	std::mutex lock;
	/** The mosaics of the dungeon chosen, a level each, the one with every room last; none without a dungeon. */
	std::shared_ptr<const std::vector<Mosaic>> mosaics;
	/** Where the character is, while the filter finds it. */
	std::optional<Inside> place;
};

/** What a filter of this plugin shows; null for any other source. */
Shown *shown_of(obs_source_t *filter);

void register_map_source();
