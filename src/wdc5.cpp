// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wdc5.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

constexpr size_t HEADER = 204, SECTION = 40, STORAGE = 24;

} // namespace

uint32_t Table::u32(size_t at) const
{
	if (at + 4 > b.size())
		throw std::runtime_error("a WDC5 table shorter than it says");
	return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24;
}

// Up to 32 bits from any bit of the file.
uint32_t Table::bits_at(size_t byte, uint32_t bit, uint32_t count) const
{
	uint64_t value = 0;
	for (int i = 7; i >= 0; i--) {
		size_t p = byte + (bit >> 3) + size_t(i);
		value = value << 8 | (p < b.size() ? b[p] : 0);
	}
	value >>= bit & 7;
	return uint32_t(count >= 32 ? value : value & ((uint64_t(1) << count) - 1));
}

Table::Table(std::vector<uint8_t> bytes, const std::function<bool(const std::string &)> &opens) : b(std::move(bytes))
{
	if (b.size() < HEADER || std::memcmp(b.data(), "WDC5", 4) != 0)
		throw std::runtime_error("not a WDC5 table");
	record_size = u32(144);
	uint32_t flags = b[172] | b[173] << 8, id_index = b[174] | b[175] << 8;
	uint32_t total_fields = u32(176), storage_size = u32(188), pallet_size = u32(196), section_count = u32(200);
	// ponytail: sparse tables (ItemSparse) keep rows at their own size; none of the tables read here is one.
	if (flags & 1)
		throw std::runtime_error("a sparse WDC5 table: not read here");

	struct Section {
		bool encrypted;
		uint32_t offset, rows, texts, end, ids, parents, offset_map, copies;
	};
	std::vector<Section> sections;
	size_t at = HEADER;
	for (uint32_t i = 0; i < section_count; i++) {
		size_t p = at + i * SECTION;
		char name[17];
		std::snprintf(name, sizeof name, "%08x%08x", u32(p + 4), u32(p));
		bool locked = u32(p) || u32(p + 4);
		sections.push_back({locked && !(opens && opens(name)), u32(p + 8), u32(p + 12), u32(p + 16), u32(p + 20),
				    u32(p + 24), u32(p + 28), u32(p + 32), u32(p + 36)});
	}
	at += section_count * SECTION;
	std::vector<uint32_t> sizes;
	for (uint32_t i = 0; i < total_fields; i++)
		sizes.push_back(uint32_t(32 - int16_t(b[at + i * 4] | b[at + i * 4 + 1] << 8)));
	at += total_fields * 4;
	for (uint32_t i = 0; i < storage_size / STORAGE; i++) {
		size_t p = at + i * STORAGE;
		columns.push_back({uint32_t(b[p] | b[p + 1] << 8), uint32_t(b[p + 2] | b[p + 3] << 8),
				   i < sizes.size() ? sizes[i] : 32, u32(p + 8), u32(p + 12), u32(p + 20), u32(p + 4), {}});
	}
	at += storage_size;
	size_t pallet = at, common = at + pallet_size;
	for (Column &c : columns) {
		uint32_t size = c.block;
		if (c.kind == 3 || c.kind == 4) {
			c.block = uint32_t(pallet);
			pallet += size;
		} else if (c.kind == 2) {
			for (uint32_t i = 0; i + 8 <= size; i += 8)
				c.common[u32(common + i)] = u32(common + i + 4);
			common += size;
		}
	}

	for (const Section &s : sections)
		all_rows += s.rows;
	uint32_t rows_before = 0, texts_before = 0;
	for (const Section &s : sections) {
		uint32_t first_row = rows_before, first_text = texts_before;
		rows_before += s.rows;
		texts_before += s.texts;
		if (s.encrypted) {
			encrypted_rows += s.rows;
			continue;
		}
		uint32_t strings = s.offset + s.rows * record_size, id_list = strings + s.texts;
		uint32_t copy_table = id_list + s.ids, offset_map = copy_table + s.copies * 8, parents_at = offset_map + s.offset_map * 6;
		std::map<uint32_t, uint32_t> parents; // row of the section -> its parent's id
		if (s.parents)
			for (uint32_t i = 0, n = u32(parents_at); i < n; i++)
				parents[u32(parents_at + 12 + i * 8 + 4)] = u32(parents_at + 12 + i * 8);

		std::map<uint32_t, size_t> made; // id -> place in rows
		for (uint32_t i = 0; i < s.rows; i++) {
			Row row{0, std::nullopt, s.offset + i * record_size, strings, first_row + i, first_text, 0};
			row.id = s.ids ? u32(id_list + i * 4) : uint32_t(raw(row, id_index, 0));
			row.value_id = row.id;
			auto parent = parents.find(i);
			if (parent != parents.end())
				row.parent = parent->second;
			made[row.id] = rows.size();
			rows.push_back(row);
		}
		// A copy is a row with another id and the same values.
		for (uint32_t i = 0; i < s.copies; i++) {
			auto source = made.find(u32(copy_table + i * 8 + 4));
			if (source != made.end()) {
				Row copy = rows[source->second];
				copy.id = u32(copy_table + i * 8);
				rows.push_back(copy);
			}
		}
	}
}

int64_t Table::raw(const Row &row, size_t field, uint32_t at) const
{
	if (field >= columns.size())
		throw std::runtime_error("no column " + std::to_string(field));
	const Column &f = columns[field];
	switch (f.kind) {
	case 2: {
		auto value = f.common.find(row.value_id);
		return value == f.common.end() ? f.a : value->second;
	}
	case 3:
		return u32(f.block + size_t(bits_at(row.record, f.offset, f.bits)) * 4);
	case 4:
		return u32(f.block + (size_t(bits_at(row.record, f.offset, f.bits)) * f.c + at) * 4);
	case 0:
		return bits_at(row.record, f.offset + at * f.element_bits, f.element_bits);
	default: {
		int64_t value = bits_at(row.record, f.offset, f.bits);
		// Sign of a packed number: its top bit.
		if ((f.kind == 5 || (f.c & 1)) && f.bits < 32 && value >= (int64_t(1) << (f.bits - 1)))
			value -= int64_t(1) << f.bits;
		return value;
	}
	}
}

int32_t Table::integer(const Row &row, size_t field, uint32_t at) const
{
	return int32_t(uint32_t(raw(row, field, at)));
}

float Table::decimal(const Row &row, size_t field, uint32_t at) const
{
	uint32_t bits = uint32_t(raw(row, field, at));
	float out;
	std::memcpy(&out, &bits, 4);
	return out;
}

// A text is stored as the distance from its own column to its place among the texts, counted as if the
// rows of every section came first and the texts of every section after them.
std::string Table::text(const Row &row, size_t field) const
{
	uint32_t distance = uint32_t(raw(row, field, 0));
	if (!distance)
		return "";
	int64_t start = int64_t(row.strings) + int64_t(row.place) * record_size + (columns[field].offset >> 3) + distance -
			int64_t(all_rows) * record_size - row.text_base;
	if (start < 0 || size_t(start) >= b.size())
		return "";
	const char *from = reinterpret_cast<const char *>(&b[size_t(start)]);
	return std::string(from, strnlen(from, b.size() - size_t(start)));
}
