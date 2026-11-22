// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The insides of buildings (dungeons and raids), laid on the map from the game's own files. Ported from
// Twinmap Forever (app/src/main/interiors.ts).

#pragma once

#include "wdc5.h"

#include <cstdint>
#include <string>
#include <vector>

/** The file numbers of the game's tables in its archive. They have parts the game keeps encrypted. */
constexpr uint32_t MAP_FILE = 1349477;
constexpr uint32_t ROOM_PICTURES_FILE = 1323241;

/** A dungeon or a raid of the game's table of maps. */
struct MapEntry {
	uint32_t id;
	std::string name;
	bool raid;
	uint32_t world_file; // its world file (.wdt)
};

/** The dungeons and raids of the game's table of maps, in its order. */
// ponytail: the names of the bosses (the table of encounters) are left out: the map does not show them.
std::vector<MapEntry> map_entries(const Table &maps);
