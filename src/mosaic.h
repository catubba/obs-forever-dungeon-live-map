// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rooms of a dungeon laid on one grey picture, a pixel a yard: what the minimap on the screen is
// looked for in. Ported from Twinmap Forever (app/src/main/minimap.ts, mosaicOfPieces), to the same
// pixel. No file is read here.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/** A picture of a room, laid on the map. Places in the map's yards: u east, v south. */
struct Piece {
	uint32_t file;        // which picture
	double u, v;          // its middle
	double width, height; // its size in yards; the picture has twice as many pixels
	double angle;         // its turn in radians, anticlockwise
	int level;            // how many rooms lie under it: 0 for a room with nothing below
};

/** Grey, a byte a yard, 0 where there is nothing. u0, v0: the yards of its top left corner. */
struct Mosaic {
	int u0 = 0, v0 = 0, width = 0, height = 0;
	std::vector<uint8_t> grey;
	int level = -1;    // the level it shows the rooms up to, in a dungeon with rooms over rooms
	bool open = false; // the open ground: what the minimap shows under no roof
};

/** The DXT1 blocks of a picture, as the game keeps them; empty when there is none. */
using PictureOf = std::function<const std::vector<uint8_t> &(uint32_t file)>;

/** The pieces laid on one picture, later pieces over earlier ones. Empty mosaic for no pieces. */
Mosaic mosaic_of_pieces(const std::vector<Piece> &pieces, const PictureOf &picture_of);

/** One mosaic a level, the rooms up to that level laid on it. */
std::vector<Mosaic> mosaics_by_level(const std::vector<Piece> &pieces, const PictureOf &picture_of);
