// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the character is inside a dungeon: the picture of the minimap on the screen is looked for
// among the rooms of the dungeon (mosaic.h). Ported from Twinmap Forever (app/src/shared/minimap.ts
// and the follower of app/src/main/minimap.ts), to give the same places.

#pragma once

#include "mosaic.h"

#include <cstdint>
#include <optional>
#include <vector>

/** A square cut from the screen: the minimap and nothing around it. Three bytes a pixel, rows from the top. */
struct Box {
	int size = 0;
	std::vector<uint8_t> rgb;
};

/** How many yards the minimap shows across at each of its zooms (0 the widest), under a roof and in the open. */
// ponytail: only the first indoor figure is measured (300, in The Hall of Thanes); the rest are the figures
// known for the game. M4-F3 measures them.
extern const double INDOOR_YARDS[6];
extern const double OUTDOOR_YARDS[6];

/** A likeness under this is not a place: the point goes, it does not guess. */
constexpr double SURE = 0.75;

/** A mosaic at each of the steps it is looked through at, made once for a dungeon. */
struct Levels {
	struct At {
		int step, width, height;
		std::vector<float> grey;
	};
	int u0, v0, level;
	bool open;
	std::vector<At> at;
};
Levels levels_of(const Mosaic &mosaic);

/** A place in the yards of the pieces, and the likeness there, 0 to 1. */
struct Found {
	double u, v, score;
};

/**
 * Where the middle of the minimap is among the rooms, the minimap showing `across` yards. close: the
 * place found a moment ago, to look only around it; without it the whole mosaic is looked through.
 * None when the minimap shows next to nothing.
 */
std::optional<Found> locate(const Levels &levels, const Box &box, double across, const std::optional<Found> &close);

/** Which way the character faces, degrees clockwise from north, from the arrow in the middle; none when no arrow is seen. */
std::optional<double> facing_of(const Box &box);

/** Where the character is, and which way it faces when known. */
struct Inside {
	double u, v;
	std::optional<double> facing;
	int level; // -1 in a dungeon with a single level, and in the open
};

/**
 * Follows the character through one dungeon, a look at a time. widths: the yards the minimap may
 * show across, the likeliest first (widths_of). now: milliseconds, growing.
 */
class Follower {
public:
	Follower(const std::vector<Mosaic> &mosaics, std::vector<double> widths);
	std::optional<Inside> look(const Box &box, double now);

private:
	struct Last {
		Inside inside;
		double at;
		size_t picture;
		double across;
	};
	std::optional<Inside> fresh(double now) const;

	std::vector<Levels> floors;
	std::vector<double> widths;
	std::optional<Last> last;
	double looked_at = 0, searched_at = 0;
	size_t lost_tries = 0;
};

/** The widths the minimap may show at a zoom: under a roof, then in the open. */
std::vector<double> widths_of(int zoom);
