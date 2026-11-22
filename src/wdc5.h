// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads a table of the game (a .db2 file in the WDC5 format). The file says by itself how each column is
// stored, so no outside definition is needed: only which column means what, which the caller knows.
// Sections the game keeps encrypted are skipped and counted, unless the caller says they came opened
// (the server has sent this PC their key). Ported from Twinmap Forever (app/src/shared/wdc5.ts).
//
// Layout: header, section headers, field sizes, field storage, pallet data, common data, then for each
// section its records, texts, id list, copy table and parent ids.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class Table {
public:
	struct Row {
		uint32_t id;
		/** The id of the row this one belongs to, in tables that have one (a "relationship"). */
		std::optional<uint32_t> parent;
		uint32_t record;   // where its record starts in the file
		uint32_t strings;  // where the texts of its section start
		uint32_t place;    // its place among the rows of every section
		uint32_t text_base; // the texts of the sections before its own
		uint32_t value_id; // the id its common values are kept under: a copy keeps its source's
	};

	/**
	 * bytes: the whole file. opens: whether the sections locked with the key of this name came opened.
	 * Throws std::runtime_error for a file that is not a WDC5 table, or a sparse one (not read here).
	 */
	explicit Table(std::vector<uint8_t> bytes, const std::function<bool(const std::string &)> &opens = nullptr);

	std::vector<Row> rows;
	/** Rows the game keeps encrypted and this PC has no key for: not read. */
	size_t encrypted_rows = 0;
	size_t fields() const { return columns.size(); }

	/** A whole number: element `at` of column `field`. */
	int32_t integer(const Row &row, size_t field, uint32_t at = 0) const;
	float decimal(const Row &row, size_t field, uint32_t at = 0) const;
	std::string text(const Row &row, size_t field) const;

private:
	struct Column {
		uint32_t offset;       // bits from the start of the record
		uint32_t bits;         // bits in the record, all elements together
		uint32_t element_bits; // bits of one element, for a column stored plainly
		uint32_t kind;         // 0 plain, 1 packed, 2 common, 3 pallet, 4 pallet array, 5 packed and signed
		uint32_t a, c;
		uint32_t block; // where its pallet data starts
		std::map<uint32_t, uint32_t> common;
	};
	std::vector<uint8_t> b;
	std::vector<Column> columns;
	uint32_t record_size = 0, all_rows = 0;

	uint32_t u32(size_t at) const;
	uint32_t bits_at(size_t byte, uint32_t bit, uint32_t count) const;
	int64_t raw(const Row &row, size_t field, uint32_t at) const;
};
