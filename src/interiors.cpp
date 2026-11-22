// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later

#include "interiors.h"

namespace {

// Which column is what, found by reading the real tables.
constexpr size_t NAME = 1, KIND = 8, WORLD_FILE = 21;
constexpr int32_t DUNGEON = 1, RAID = 2;
constexpr uint32_t CONTINENTS[] = {0, 1}; // Eastern Kingdoms and Kalimdor: worlds, whatever their kind says

} // namespace

std::vector<MapEntry> map_entries(const Table &maps)
{
	std::vector<MapEntry> out;
	for (const Table::Row &row : maps.rows) {
		bool world = false;
		for (uint32_t continent : CONTINENTS)
			world = world || row.id == continent;
		int32_t kind = maps.integer(row, KIND);
		if (world || (kind != DUNGEON && kind != RAID))
			continue;
		out.push_back({row.id, maps.text(row, NAME), kind == RAID, uint32_t(maps.integer(row, WORLD_FILE))});
	}
	return out;
}
