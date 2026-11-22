// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds the mosaics of a dungeon from pieces and room pictures already taken out of the game, writes
// them as PGM, and compares them with reference PGMs when given (twinmap.h says what it reads).
//   mosaic-check <pieces.txt> <rooms folder> <out prefix> [reference prefix]
// Writes <out prefix>-<level>.pgm; exits 1 when a reference differs by more than 1 grey.

#include "twinmap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

void write_pgm(const std::string &path, const Mosaic &m)
{
	std::ofstream out(path, std::ios::binary);
	out << "P5\n# " << m.u0 << " " << m.v0 << "\n" << m.width << " " << m.height << "\n255\n";
	out.write(reinterpret_cast<const char *>(m.grey.data()), std::streamsize(m.grey.size()));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "mosaic-check <pieces.txt> <rooms folder> <out prefix> [reference prefix]\n");
		return 2;
	}
	size_t pieces = 0;
	std::vector<Mosaic> mosaics = twinmap_mosaics(argv[1], argv[2], &pieces);
	std::printf("%zu pieces, %zu levels\n", pieces, mosaics.size());
	int worst = 0;
	for (size_t level = 0; level < mosaics.size(); level++) {
		const Mosaic &m = mosaics[level];
		write_pgm(std::string(argv[3]) + "-" + std::to_string(level) + ".pgm", m);
		std::printf("level %zu: %dx%d at %d,%d", level, m.width, m.height, m.u0, m.v0);
		if (argc > 4) {
			Mosaic ref;
			if (!read_pgm(std::string(argv[4]) + "-" + std::to_string(level) + ".pgm", ref)) {
				std::printf(", no reference\n");
				worst = 256;
				continue;
			}
			if (ref.u0 != m.u0 || ref.v0 != m.v0 || ref.width != m.width || ref.height != m.height) {
				std::printf(", reference is %dx%d at %d,%d\n", ref.width, ref.height, ref.u0, ref.v0);
				worst = 256;
				continue;
			}
			size_t differ = 0;
			int most = 0;
			for (size_t i = 0; i < m.grey.size(); i++) {
				int d = std::abs(int(m.grey[i]) - int(ref.grey[i]));
				differ += d != 0;
				most = std::max(most, d);
			}
			worst = std::max(worst, most);
			std::printf(", %zu pixels differ from the reference, by %d at most", differ, most);
		}
		std::printf("\n");
	}
	return worst > 1 ? 1 : 0;
}
