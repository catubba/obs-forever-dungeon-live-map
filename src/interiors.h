// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The insides of buildings (dungeons and raids), laid on the map from the game's own files. Ported from
// Twinmap Forever (app/src/main/interiors.ts).
//
// What the game has:
// - The table of its maps says, for a dungeon or a raid, which world file (.wdt) it is.
// - A world file lists the files of each block of 533.33 yards; the second of them ("obj0") holds a "MODF"
//   chunk: the buildings standing on the block, each with its file, position and turn. A map that is one
//   building and no ground has a single MODF in the world file itself.
// - A building (.wmo) says its number ("MOHD") and the box of each of its rooms ("MOGI").
// - The table of room pictures gives, for a building's number, the minimap pictures of its rooms: room,
//   piece across, piece up, file. Only rooms under a roof have one.
// - A picture is 2 pixels a yard, x of the building to the right and y up, starting from the lowest corner of
//   the room's box; a room over 128 yards is cut in pieces of 256 pixels. Every picture is DXT1 blocks with
//   one bit of transparency.

#pragma once

#include "mosaic.h"
#include "wdc5.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
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

struct RoomPicture {
	uint32_t room, across, up, file;
};
/** The pictures of the rooms of every building: building number -> its pictures. */
std::map<uint32_t, std::vector<RoomPicture>> room_pictures(const Table &table);

/** A piece as laid, with what the mosaics do not need but the comparison with Twinmap does. */
struct Laid {
	Piece piece;
	double bottom, top; // how high the room's floor and ceiling are, in yards
	uint32_t building;  // which of the buildings standing in the map it is a room of, from 0
};

/** A game file by number; none when the build has no such file or it is still locked. */
using ReadFile = std::function<std::optional<std::vector<uint8_t>>(uint32_t)>;

/**
 * Lays the rooms of dungeons on their maps. The pictures of the rooms are kept in `rooms`, as
 * <file>.dxt (DXT1 blocks, deflated); one already there is kept, so an interrupted run resumes.
 */
class Converter {
public:
	Converter(ReadFile read, std::map<uint32_t, std::vector<RoomPicture>> pictures, std::filesystem::path rooms);

	/** The pieces of a map, lowest floor first; none for a map whose world file is not there. */
	std::vector<Laid> convert(const MapEntry &map);
	/** Pictures the table lists that could not be laid, so far: no such room, outside the room, no picture. */
	size_t dropped = 0;

private:
	struct Building {
		uint32_t number;
		std::vector<std::array<float, 6>> rooms; // min x, y, z, max x, y, z
	};
	ReadFile read;
	std::map<uint32_t, std::vector<RoomPicture>> pictures;
	std::filesystem::path rooms;
	std::map<uint32_t, std::optional<Building>> buildings;
	struct Stored {
		uint32_t width, height;
		std::vector<uint8_t> solid; // which blocks of 4 x 4 pixels are mostly solid
	};
	std::map<uint32_t, std::optional<Stored>> stored;

	const std::optional<Building> &building(uint32_t file);
	const std::optional<Stored> &picture(uint32_t file);
};

/** The pieces as text, one a line: "file u v width height angle level", as mosaics_from_rooms reads them. */
std::string pieces_text(const std::vector<Laid> &pieces);
