// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dungeons and raids of the game's table of maps, read from a game folder: "id|name|kind|world file"
// a line, kind "dungeon" or "raid". Read only on the game's side.
//   maps-list <game folder>

#include "casc.h"
#include "interiors.h"

#include <cstdio>

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "maps-list <game folder>\n");
		return 2;
	}
	try {
		GameFiles files(argv[1], sent_keys(argv[1]));
		// The table has parts the game keeps encrypted: read around the ones still locked.
		std::optional<Bytes> bytes = files.read(MAP_FILE, true);
		if (!bytes) {
			std::fprintf(stderr, "the game has no table of its maps\n");
			return 1;
		}
		Table maps(std::move(*bytes), [&](const std::string &key) { return files.opens(key); });
		std::vector<MapEntry> list = map_entries(maps);
		for (const MapEntry &m : list)
			std::printf("%u|%s|%s|%u\n", m.id, m.name.c_str(), m.raid ? "raid" : "dungeon", m.world_file);
		std::fprintf(stderr, "%zu rows read, %zu locked; %zu dungeons and raids\n", maps.rows.size(), maps.encrypted_rows,
			     list.size());
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
