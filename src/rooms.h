// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rooms of a dungeon as they are kept once taken out of the game (interiors.h): <number>.txt, one piece
// a line ("file u v width height angle level"), and rooms/<file>.dxt, DXT1 blocks deflated.

#pragma once

#include "mosaic.h"

#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

inline std::vector<uint8_t> inflated(const std::filesystem::path &path)
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

/** The mosaics of a dungeon, a level each, the pictures of its pieces read from the rooms folder. */
inline std::vector<Mosaic> mosaics_from_rooms(const std::vector<Piece> &pieces, const std::filesystem::path &rooms)
{
	std::map<uint32_t, std::vector<uint8_t>> pictures;
	return mosaics_by_level(pieces, [&](uint32_t file) -> const std::vector<uint8_t> & {
		auto at = pictures.find(file);
		if (at == pictures.end())
			at = pictures.emplace(file, inflated(rooms / (std::to_string(file) + ".dxt"))).first;
		return at->second;
	});
}

/** The same from the pieces of a <number>.txt. */
inline std::vector<Mosaic> mosaics_from_files(const std::filesystem::path &pieces_txt, const std::filesystem::path &rooms, size_t *pieces_read = nullptr)
{
	std::vector<Piece> pieces;
	std::ifstream list(pieces_txt);
	for (Piece p; list >> p.file >> p.u >> p.v >> p.width >> p.height >> p.angle >> p.level;)
		pieces.push_back(p);
	if (pieces_read)
		*pieces_read = pieces.size();
	return mosaics_from_rooms(pieces, rooms);
}

/** A mosaic as a PGM whose comment line holds u0 v0. */
inline bool read_pgm(const std::string &path, Mosaic &m)
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
