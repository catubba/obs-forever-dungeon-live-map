// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A recorded run through the follower, as Twinmap Forever's design/minimap/replay.ts does it, to compare
// the places with Twinmap's own (track-app.csv).
//   replay <pieces.txt> <rooms folder> <boxes.raw> <boxes.txt> <out.csv> [zoom, default 0] [open ground .pgm]
// boxes.raw: the minimap squares one after the other, three bytes a pixel; boxes.txt: the millisecond of
// each, one a line. Writes "ms,u,v,facing,took" a line, as Twinmap does.

#include "minimap.h"
#include "twinmap.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

std::string number(double x)
{
	char text[32];
	auto end = std::to_chars(text, text + sizeof text, x).ptr; // the shortest that reads back, as JavaScript writes it
	return std::string(text, end);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::fprintf(stderr, "replay <pieces.txt> <rooms folder> <boxes.raw> <boxes.txt> <out.csv> [zoom]\n");
		return 2;
	}
	std::vector<Mosaic> mosaics = twinmap_mosaics(argv[1], argv[2]);
	// ponytail: the open ground is not in v1; a PGM of it, as Twinmap builds it, only to compare with Twinmap.
	if (argc > 7) {
		Mosaic open;
		if (!read_pgm(argv[7], open)) {
			std::fprintf(stderr, "cannot read %s\n", argv[7]);
			return 1;
		}
		open.open = true;
		mosaics.push_back(std::move(open));
	}
	if (mosaics.empty()) {
		std::fprintf(stderr, "no rooms in %s\n", argv[1]);
		return 1;
	}
	std::vector<double> times;
	std::ifstream list(argv[4]);
	for (double ms; list >> ms;)
		times.push_back(ms);
	std::ifstream raw(argv[3], std::ios::binary | std::ios::ate);
	auto bytes = size_t(raw.tellg());
	raw.seekg(0);
	if (times.empty() || bytes % (times.size() * 3)) {
		std::fprintf(stderr, "%s does not hold %zu squares\n", argv[3], times.size());
		return 1;
	}
	Box box;
	box.size = int(std::lround(std::sqrt(double(bytes / times.size() / 3))));
	box.rgb.resize(size_t(box.size) * box.size * 3);
	if (box.rgb.size() * times.size() != bytes) {
		std::fprintf(stderr, "the squares of %s are not square\n", argv[3]);
		return 1;
	}

	Follower follower(mosaics, widths_of(argc > 6 ? std::atoi(argv[6]) : 0));
	std::ofstream csv(argv[5]);
	csv << "ms,u,v,facing,took";
	size_t found = 0;
	double total = 0;
	for (double ms : times) {
		raw.read(reinterpret_cast<char *>(box.rgb.data()), std::streamsize(box.rgb.size()));
		auto started = std::chrono::steady_clock::now();
		std::optional<Inside> got = follower.look(box, ms);
		double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		total += took;
		char facing[16] = "", spent[16];
		if (got && got->facing)
			std::snprintf(facing, sizeof facing, "%.0f", *got->facing);
		std::snprintf(spent, sizeof spent, "%.1f", took);
		csv << "\n" << number(ms) << "," << (got ? number(got->u) : "") << "," << (got ? number(got->v) : "") << ","
		    << facing << "," << spent;
		found += bool(got);
	}
	std::printf("%zu squares of %d pixels, %zu mosaics: a place for %zu (%.0f%%), %.1f ms a look on average\n",
		    times.size(), box.size, mosaics.size(), found, 100.0 * found / times.size(), total / times.size());
	return 0;
}
