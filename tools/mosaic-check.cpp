// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds the mosaics of a dungeon from pieces and room pictures already taken out of the game, writes
// them as PGM, and compares them with reference PGMs when given.
//   mosaic-check <pieces.txt> <rooms folder> <out prefix> [reference prefix]
// pieces.txt: one piece a line, "file u v width height angle level". rooms/<file>.dxt: DXT1 blocks,
// deflated. Writes <out prefix>-<level>.pgm; exits 1 when a reference differs by more than 1 grey.

#include "mosaic.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

namespace {

std::vector<uint8_t> inflated(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> packed((std::istreambuf_iterator<char>(in)), {}), out;
	if (packed.empty())
		return out;
	z_stream z{};
	if (inflateInit(&z) != Z_OK)
		return out;
	z.next_in = packed.data();
	z.avail_in = uInt(packed.size());
	int got = Z_OK;
	while (got == Z_OK) {
		out.resize(out.size() + 65536);
		z.next_out = out.data() + z.total_out;
		z.avail_out = uInt(out.size() - z.total_out);
		got = inflate(&z, Z_NO_FLUSH);
	}
	out.resize(got == Z_STREAM_END ? z.total_out : 0);
	inflateEnd(&z);
	return out;
}

void write_pgm(const std::string &path, const Mosaic &m)
{
	std::ofstream out(path, std::ios::binary);
	out << "P5\n# " << m.u0 << " " << m.v0 << "\n" << m.width << " " << m.height << "\n255\n";
	out.write(reinterpret_cast<const char *>(m.grey.data()), std::streamsize(m.grey.size()));
}

bool read_pgm(const std::string &path, Mosaic &m)
{
	std::ifstream in(path, std::ios::binary);
	std::string magic, hash;
	if (!(in >> magic >> hash >> m.u0 >> m.v0 >> m.width >> m.height) || magic != "P5")
		return false;
	int most;
	in >> most;
	in.get();
	m.grey.resize(size_t(m.width) * m.height);
	return bool(in.read(reinterpret_cast<char *>(m.grey.data()), std::streamsize(m.grey.size())));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "mosaic-check <pieces.txt> <rooms folder> <out prefix> [reference prefix]\n");
		return 2;
	}
	std::vector<Piece> pieces;
	std::ifstream list(argv[1]);
	for (Piece p; list >> p.file >> p.u >> p.v >> p.width >> p.height >> p.angle >> p.level;)
		pieces.push_back(p);
	std::map<uint32_t, std::vector<uint8_t>> pictures;
	std::string rooms = argv[2];
	auto picture_of = [&](uint32_t file) -> const std::vector<uint8_t> & {
		auto at = pictures.find(file);
		if (at == pictures.end())
			at = pictures.emplace(file, inflated(rooms + "/" + std::to_string(file) + ".dxt")).first;
		return at->second;
	};

	std::vector<Mosaic> mosaics = mosaics_by_level(pieces, picture_of);
	std::printf("%zu pieces, %zu pictures, %zu levels\n", pieces.size(), pictures.size(), mosaics.size());
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
