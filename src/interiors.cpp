// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later

#include "interiors.h"

#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {

// Which column is what, found by reading the real tables.
constexpr size_t NAME = 1, KIND = 8, WORLD_FILE = 21;
constexpr size_t ROOM = 0, ACROSS = 1, UP = 2, PICTURE = 3;
constexpr int32_t DUNGEON = 1, RAID = 2;
constexpr uint32_t CONTINENTS[] = {0, 1}; // Eastern Kingdoms and Kalimdor: worlds, whatever their kind says

constexpr double PI = 3.14159265358979323846;
constexpr int BLOCKS = 64;                     // blocks a side of a map's square
constexpr double BLOCK_YARDS = 1600.0 / 3;     // yards a side of a block
constexpr double PIXELS_PER_YARD = 2;          // of a room's picture
constexpr double PIECE_YARDS = 128;            // what a piece of a large room covers
constexpr double CELL = 2;      // yards: the side of a DXT block of a picture, and of the squares rooms are compared on
constexpr double STEP_UP = 4;   // yards between two floors, at least
constexpr double SHARED = 0.15; // of the smaller room, for the two to count as one over the other

// The table numbers the rooms as a building had them when its pictures were made. One building has lost
// rooms since: its pictures would land on the wrong rooms. Seen in the files alone: of the 62 rooms of one
// picture, 34 have a picture of the size their box asks as numbered, 62 as renumbered.
std::optional<uint32_t> renumbered(uint32_t building, uint32_t room)
{
	if (building == 21107) // Stratholme: seven rooms gone
		return room >= 39 ? std::optional<uint32_t>(room - 7) : room >= 32 ? std::nullopt : std::optional<uint32_t>(room);
	return room;
}

uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}
uint16_t le16(const uint8_t *p)
{
	return uint16_t(p[0] | p[1] << 8);
}
float lef(const uint8_t *p)
{
	uint32_t bits = le32(p);
	float out;
	std::memcpy(&out, &bits, 4);
	return out;
}

// A game file is a run of chunks: a four letter tag written backwards, a size, the data.
struct Chunk {
	std::string tag;
	const uint8_t *data;
	size_t size;
};
std::vector<Chunk> chunks(const std::vector<uint8_t> &b)
{
	std::vector<Chunk> out;
	for (size_t at = 0; at + 8 <= b.size();) {
		size_t size = std::min<size_t>(le32(&b[at + 4]), b.size() - at - 8);
		out.push_back({std::string(reinterpret_cast<const char *>(&b[at]), 4), &b[at + 8], size});
		at += 8 + size;
	}
	return out;
}

// JavaScript's Number(n.toFixed(digits)): from the exact decimal value, to the nearest, a half to the larger number.
double to_fixed(double n, int digits)
{
	char exact[512];
	*std::to_chars(exact, exact + sizeof exact - 1, n, std::chars_format::fixed, 100).ptr = 0;
	std::string text(exact);
	size_t cut = text.find('.') + 1 + size_t(digits);
	std::string rest = text.substr(cut);
	bool negative = n < 0, more = rest.find_first_not_of('0', 1) != std::string::npos;
	bool away = rest[0] > '5' || (rest[0] == '5' && (more || !negative)); // a negative half goes towards zero
	double kept = std::strtod(text.substr(0, cut).c_str(), nullptr);
	double out = away ? kept + (negative ? -1 : 1) * std::pow(10.0, -digits) : kept;
	char again[64];
	std::snprintf(again, sizeof again, "%.*f", digits, out);
	return std::strtod(again, nullptr);
}

// Writes a file under another name first: a file is either whole or not there. One already there is kept.
void keep(const fs::path &file, const std::function<std::vector<uint8_t>()> &bytes)
{
	std::error_code error;
	if (fs::exists(file, error))
		return;
	fs::create_directories(file.parent_path(), error);
	std::vector<uint8_t> data = bytes();
	fs::path part = file;
	part += ".part";
	{
		std::ofstream out(part, std::ios::binary);
		out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
	}
	fs::rename(part, file, error);
}

// Which blocks of 4 x 4 pixels of a picture are mostly solid: one byte each, rows from the top.
std::vector<uint8_t> solid_blocks(const uint8_t *dxt, size_t size)
{
	std::vector<uint8_t> solid(size >> 3);
	for (size_t i = 0; i < solid.size(); i++) {
		// A block whose first color is not the greater has a see-through choice, the fourth.
		int see_through = 0;
		if (le16(dxt + i * 8) <= le16(dxt + i * 8 + 2)) {
			uint32_t choices = le32(dxt + i * 8 + 4);
			for (int k = 0; k < 16; k++)
				see_through += ((choices >> (k * 2)) & 3) == 3;
		}
		solid[i] = see_through <= 8;
	}
	return solid;
}

// How many rooms lie under each piece. Two rooms are one over the other when their pictures cover a good
// part of the same ground (not just a doorway) and one's floor is well above the other's.
std::vector<int> levels_of(const std::vector<Laid> &pieces, const std::function<const std::vector<uint8_t> *(uint32_t)> &solid_of)
{
	size_t n = pieces.size();
	std::map<int64_t, std::vector<size_t>> ground; // squares of two yards: which pieces cover each
	std::vector<size_t> covered(n);
	for (size_t i = 0; i < n; i++) {
		const Piece &p = pieces[i].piece;
		const std::vector<uint8_t> *solid = solid_of(p.file);
		if (!solid)
			continue;
		int across = int(std::floor(p.width * PIXELS_PER_YARD / 4 + 0.5));
		double cos = std::cos(p.angle), sin = std::sin(p.angle);
		std::set<int64_t> mine;
		for (size_t b = 0; b < solid->size(); b++) {
			if (!(*solid)[b])
				continue;
			// The middle of the block, from the middle of the picture, turned as the picture is.
			double x = (double(b % across) + 0.5) * CELL - p.width / 2, y = (std::floor(double(b) / across) + 0.5) * CELL - p.height / 2;
			mine.insert(int64_t(std::floor((p.u + x * cos + y * sin) / CELL)) * 32768 +
				    int64_t(std::floor((p.v - x * sin + y * cos) / CELL)));
		}
		covered[i] = mine.size();
		for (int64_t square : mine)
			ground[square].push_back(i);
	}
	std::map<size_t, size_t> shared; // pair of pieces -> squares they both cover
	for (const auto &[square, list] : ground)
		for (size_t a = 0; a < list.size(); a++)
			for (size_t b = a + 1; b < list.size(); b++)
				shared[list[a] * n + list[b]]++;
	std::vector<std::vector<size_t>> under(n);
	for (const auto &[pair, squares] : shared) {
		size_t a = pair / n, b = pair % n;
		if (squares < SHARED * std::min(covered[a], covered[b]))
			continue;
		double rise = pieces[a].bottom - pieces[b].bottom;
		if (rise >= STEP_UP)
			under[a].push_back(b);
		else if (rise <= -STEP_UP)
			under[b].push_back(a);
	}
	// From the lowest floor up: a room is one level over the highest room under it.
	std::vector<size_t> order(n);
	for (size_t i = 0; i < n; i++)
		order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pieces[a].bottom < pieces[b].bottom; });
	std::vector<int> levels(n);
	for (size_t i : order)
		for (size_t lower : under[i])
			levels[i] = std::max(levels[i], levels[lower] + 1);
	return levels;
}

struct Placed {
	uint32_t file;
	double u, v, height, turn, scale;
};

// The buildings standing in a map, from its world file, by the number the game gives each: one may lie on
// several blocks, and is kept where it was first listed.
std::vector<Placed> placed_in(const std::vector<uint8_t> &wdt, const ReadFile &read)
{
	std::vector<Placed> placed;
	std::map<uint32_t, size_t> where;
	auto buildings = [&](const uint8_t *d, size_t size, bool whole) {
		for (size_t at = 0; at + 64 <= size; at += 64) {
			if (!(le16(d + at + 56) & 8))
				continue; // named, not numbered: not seen in this game
			// A map that is one building has it at the middle of the square, not where its entry says (0, 0).
			double middle = whole ? (BLOCKS / 2) * BLOCK_YARDS : 0;
			uint16_t scale = le16(d + at + 62);
			Placed p{le32(d + at), lef(d + at + 8) + middle, lef(d + at + 16) + middle, lef(d + at + 12), lef(d + at + 24),
				 scale ? scale / 1024.0 : 1};
			uint32_t id = le32(d + at + 4);
			auto known = where.find(id);
			if (known != where.end()) {
				placed[known->second] = p;
			} else {
				where[id] = placed.size();
				placed.push_back(p);
			}
		}
	};
	for (const Chunk &c : chunks(wdt)) {
		if (c.tag == "FDOM")
			buildings(c.data, c.size, true);
		if (c.tag != "DIAM")
			continue;
		for (size_t block = 0; block < size_t(BLOCKS) * BLOCKS && block * 32 + 8 <= c.size; block++) {
			uint32_t objects = le32(c.data + block * 32 + 4);
			std::optional<std::vector<uint8_t>> file = objects ? read(objects) : std::nullopt;
			if (file)
				for (const Chunk &o : chunks(*file))
					if (o.tag == "FDOM")
						buildings(o.data, o.size, false);
		}
	}
	return placed;
}

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

std::map<uint32_t, std::vector<RoomPicture>> room_pictures(const Table &table)
{
	std::map<uint32_t, std::vector<RoomPicture>> all;
	for (const Table::Row &r : table.rows)
		if (r.parent) // the high byte of "up" is always 1
			all[*r.parent].push_back({uint32_t(table.integer(r, ROOM)), uint32_t(table.integer(r, ACROSS)),
						  uint32_t(table.integer(r, UP)) & 255, uint32_t(table.integer(r, PICTURE))});
	return all;
}

Converter::Converter(ReadFile read_, std::map<uint32_t, std::vector<RoomPicture>> pictures_, fs::path rooms_)
	: read(std::move(read_)), pictures(std::move(pictures_)), rooms(std::move(rooms_))
{
}

const std::optional<Converter::Building> &Converter::building(uint32_t file)
{
	auto known = buildings.find(file);
	if (known != buildings.end())
		return known->second;
	std::optional<Building> out;
	if (std::optional<std::vector<uint8_t>> b = read(file)) {
		const Chunk *head = nullptr, *boxes = nullptr;
		std::vector<Chunk> list = chunks(*b);
		for (const Chunk &c : list) {
			if (c.tag == "DHOM")
				head = &c;
			if (c.tag == "IGOM")
				boxes = &c;
		}
		if (head && boxes && head->size >= 36) {
			out = Building{le32(head->data + 32), {}};
			for (size_t i = 0; i < boxes->size / 32; i++) {
				std::array<float, 6> box;
				for (size_t k = 0; k < 6; k++)
					box[k] = lef(boxes->data + i * 32 + 4 + k * 4);
				out->rooms.push_back(box);
			}
		}
	}
	return buildings.emplace(file, std::move(out)).first->second;
}

const std::optional<Converter::Stored> &Converter::picture(uint32_t file)
{
	auto known = stored.find(file);
	if (known != stored.end())
		return known->second;
	std::optional<Stored> out;
	if (std::optional<std::vector<uint8_t>> f = read(file)) {
		const std::vector<uint8_t> &b = *f;
		// DXT1 blocks of a .blp picture: version 2, stored as DXT, one bit of transparency.
		// ponytail: a picture stored some other way is left out (none is, of 5,512 in Twinmap's conversion).
		if (b.size() >= 24 && std::memcmp(b.data(), "BLP2", 4) == 0 && b[8] == 2 && b[10] == 0) {
			uint32_t width = le32(&b[12]), height = le32(&b[16]), at = le32(&b[20]);
			size_t size = size_t(width) * height / 2;
			if (width % 4 == 0 && height % 4 == 0 && at + size <= b.size()) {
				const uint8_t *dxt = &b[at];
				keep(rooms / (std::to_string(file) + ".dxt"), [&] {
					uLongf packed_size = compressBound(uLong(size));
					std::vector<uint8_t> packed(packed_size);
					compress2(packed.data(), &packed_size, dxt, uLong(size), 9);
					packed.resize(packed_size);
					return packed;
				});
				out = Stored{width, height, solid_blocks(dxt, size)};
			}
		}
	}
	return stored.emplace(file, std::move(out)).first->second;
}

std::vector<Laid> Converter::convert(const MapEntry &map)
{
	std::vector<Laid> pieces;
	std::optional<std::vector<uint8_t>> wdt = read(map.world_file);
	if (!wdt)
		return pieces;
	std::vector<Placed> placed = placed_in(*wdt, read);
	for (size_t n = 0; n < placed.size(); n++) {
		const Placed &p = placed[n];
		const std::optional<Building> &b = building(p.file);
		if (!b)
			continue;
		auto list = pictures.find(b->number);
		if (list == pictures.end())
			continue;
		// The building's x and y turned into the map: x of the world is north, y is west.
		double turn = (p.turn - 180) * PI / 180, cos = std::cos(turn), sin = std::sin(turn);
		double angle = (p.turn - 90) * PI / 180;
		while (angle > PI)
			angle -= 2 * PI;
		for (const RoomPicture &pic : list->second) {
			std::optional<uint32_t> number = renumbered(b->number, pic.room);
			const std::array<float, 6> *room = number && *number < b->rooms.size() ? &b->rooms[*number] : nullptr;
			double x = room ? (*room)[0] + pic.across * PIECE_YARDS : 0, y = room ? (*room)[1] + pic.up * PIECE_YARDS : 0;
			// A piece that starts past its room's box is of some other room: the table is behind the building.
			const std::optional<Stored> *size = room && x < (*room)[3] && y < (*room)[4] ? &picture(pic.file) : nullptr;
			if (!size || !*size) {
				dropped++;
				continue;
			}
			double width = (*size)->width / PIXELS_PER_YARD, height = (*size)->height / PIXELS_PER_YARD;
			double mx = x + width / 2, my = y + height / 2; // the middle of the picture, in the building
			double north = (mx * cos - my * sin) * p.scale, west = (mx * sin + my * cos) * p.scale;
			pieces.push_back({{pic.file, to_fixed(p.u - west, 2), to_fixed(p.v - north, 2), width * p.scale,
					   height * p.scale, to_fixed(angle, 4), 0},
					  to_fixed((*room)[2] * p.scale + p.height, 1),
					  to_fixed((*room)[5] * p.scale + p.height, 1),
					  uint32_t(n)});
		}
	}
	std::vector<int> levels = levels_of(pieces, [&](uint32_t file) -> const std::vector<uint8_t> * {
		const std::optional<Stored> &s = picture(file);
		return s ? &s->solid : nullptr;
	});
	for (size_t i = 0; i < pieces.size(); i++)
		pieces[i].piece.level = levels[i];
	std::stable_sort(pieces.begin(), pieces.end(), [](const Laid &a, const Laid &b) { return a.bottom < b.bottom; });
	return pieces;
}

std::string pieces_text(const std::vector<Laid> &pieces)
{
	std::string out;
	char line[256];
	for (const Laid &l : pieces) {
		const Piece &p = l.piece;
		std::snprintf(line, sizeof line, "%u %.17g %.17g %.17g %.17g %.17g %d\n", p.file, p.u, p.v, p.width, p.height,
			      p.angle, p.level);
		out += line;
	}
	return out;
}
