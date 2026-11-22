// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lays the rooms of the dungeons and raids on their maps from a game folder, read only on the game's side.
//   interiors-convert <game folder> <out folder> [map number]...
// Writes <out folder>/rooms/<file>.dxt, and for each map with rooms <out folder>/<number>.txt (the pieces as
// the mosaics read them) and <number>.csv (every value of each piece, to compare with Twinmap). No numbers:
// every dungeon and raid.

#include "casc.h"
#include "interiors.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "interiors-convert <game folder> <out folder> [map number]...\n");
		return 2;
	}
	try {
		auto started = std::chrono::steady_clock::now();
		GameFiles files(argv[1], sent_keys(argv[1]));
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
		std::optional<Bytes> maps = files.read(MAP_FILE, true), rooms = files.read(ROOM_PICTURES_FILE, true);
		if (!maps || !rooms) {
			std::fprintf(stderr, "the game has no table of its maps or of its room pictures\n");
			return 1;
		}
		fs::path out = argv[2];
		fs::create_directories(out / "rooms");
		Converter converter(read, room_pictures(Table(std::move(*rooms), opens)), out / "rooms");
		std::vector<uint32_t> wanted;
		for (int i = 3; i < argc; i++)
			wanted.push_back(uint32_t(std::stoul(argv[i])));
		size_t kept = 0, pieces = 0;
		for (const MapEntry &map : map_entries(Table(std::move(*maps), opens))) {
			if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), map.id) == wanted.end())
				continue;
			std::vector<Laid> laid = converter.convert(map);
			if (laid.empty())
				continue; // no world file, or no room with a picture: not a dungeon to follow
			kept++;
			pieces += laid.size();
			std::ofstream(out / (std::to_string(map.id) + ".txt")) << pieces_text(laid);
			std::ofstream csv(out / (std::to_string(map.id) + ".csv"));
			csv << "file,u,v,width,height,angle,bottom,top,building,level\n";
			char line[256];
			for (const Laid &l : laid) {
				std::snprintf(line, sizeof line, "%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%u,%d\n", l.piece.file,
					      l.piece.u, l.piece.v, l.piece.width, l.piece.height, l.piece.angle, l.bottom, l.top,
					      l.building, l.piece.level);
				csv << line;
			}
			std::printf("%u %s: %zu pieces\n", map.id, map.name.c_str(), laid.size());
		}
		double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		std::printf("%zu maps with rooms, %zu pieces, %zu pictures dropped, in %.1f s\n", kept, pieces, converter.dropped,
			    took);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
