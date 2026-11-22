// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later

#include "casc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t KEY_NAMES_FILE = 1302851; // the game's table of the names of its keys
constexpr size_t RECORD_HEADER = 30;         // bytes before the BLTE container of a record
constexpr size_t INDEX_ENTRY = 18;           // bytes: 9 of key, 5 of archive and offset, 4 of size
constexpr uint32_t NO_NAMES = 0x10000000;    // root block flag: the block carries no name hashes
constexpr uint32_t EN_US = 2;                // root block locale bit
constexpr uint32_t KEY_TABLE = 0xdf2f53cf;   // the game's number for its table of keys, in its cache
constexpr uint8_t VALID = 1; // a cache row's state; the others ("deleted", "invalid", "not public") hold no key

uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}
uint32_t be32(const uint8_t *p)
{
	return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

Bytes read_file(const fs::path &path)
{
	std::ifstream in(path, std::ios::binary);
	return Bytes((std::istreambuf_iterator<char>(in)), {});
}

std::string hex(const uint8_t *p, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < n; i++) {
		out += digits[p[i] >> 4];
		out += digits[p[i] & 15];
	}
	return out;
}

Bytes from_hex(const std::string &text)
{
	Bytes out;
	for (size_t i = 0; i + 1 < text.size(); i += 2)
		out.push_back(uint8_t(std::stoi(text.substr(i, 2), nullptr, 16)));
	return out;
}

Bytes inflated(const uint8_t *packed, size_t size)
{
	Bytes out;
	z_stream z{};
	if (inflateInit(&z) != Z_OK)
		throw std::runtime_error("zlib does not start");
	z.next_in = const_cast<uint8_t *>(packed);
	z.avail_in = uInt(size);
	int got = Z_OK;
	while (got == Z_OK) {
		out.resize(out.size() + 65536);
		z.next_out = out.data() + z.total_out;
		z.avail_out = uInt(out.size() - z.total_out);
		got = inflate(&z, Z_NO_FLUSH);
	}
	inflateEnd(&z);
	if (got != Z_STREAM_END)
		throw std::runtime_error("a deflated chunk does not inflate");
	out.resize(z.total_out);
	return out;
}

// Salsa20, the cipher the game locks parts of its files with: a key of 16 bytes and a nonce of 8 make a
// stream of bytes that is xored over the data.
Bytes salsa20(const Key &key, const uint8_t nonce[8], const uint8_t *data, size_t size)
{
	auto turn = [](uint32_t v, int by) { return v << by | v >> (32 - by); };
	auto quarter = [&](uint32_t *x, int a, int b, int c, int d) {
		x[b] ^= turn(x[a] + x[d], 7);
		x[c] ^= turn(x[b] + x[a], 9);
		x[d] ^= turn(x[c] + x[b], 13);
		x[a] ^= turn(x[d] + x[c], 18);
	};
	const uint8_t *c = reinterpret_cast<const uint8_t *>("expand 16-byte k");
	const uint8_t *k = key.data();
	// The sixteen words: constants on the diagonal, the key twice, the nonce, the count of blocks.
	uint32_t state[16] = {le32(c),      le32(k),          le32(k + 4), le32(k + 8), le32(k + 12), le32(c + 4),
			      le32(nonce), le32(nonce + 4), 0,           0,           le32(c + 8),  le32(k),
			      le32(k + 4), le32(k + 8),     le32(k + 12), le32(c + 12)};
	Bytes out(size);
	for (size_t at = 0; at < size; at += 64) {
		uint32_t x[16];
		std::memcpy(x, state, sizeof x);
		for (int round = 0; round < 10; round++) {
			quarter(x, 0, 4, 8, 12);
			quarter(x, 5, 9, 13, 1);
			quarter(x, 10, 14, 2, 6);
			quarter(x, 15, 3, 7, 11);
			quarter(x, 0, 1, 2, 3);
			quarter(x, 5, 6, 7, 4);
			quarter(x, 10, 11, 8, 9);
			quarter(x, 15, 12, 13, 14);
		}
		uint8_t block[64];
		for (int i = 0; i < 16; i++) {
			uint32_t w = x[i] + state[i];
			block[i * 4] = uint8_t(w);
			block[i * 4 + 1] = uint8_t(w >> 8);
			block[i * 4 + 2] = uint8_t(w >> 16);
			block[i * 4 + 3] = uint8_t(w >> 24);
		}
		for (size_t i = 0; i < 64 && at + i < size; i++)
			out[at + i] = data[at + i] ^ block[i];
		if (++state[8] == 0)
			state[9]++;
	}
	return out;
}

using KeyOf = std::function<const Key *(const std::string &)>;

// One chunk of a BLTE container: stored plain, deflated, or encrypted around one of those.
// plain_size: its size once decoded, 0 when not known.
Bytes chunk(const uint8_t *c, size_t size, size_t plain_size, uint32_t index, bool skip_hidden, const KeyOf &key_of)
{
	if (!size)
		throw std::runtime_error("an empty BLTE chunk");
	switch (c[0]) {
	case 'N':
		return Bytes(c + 1, c + size);
	case 'Z':
		return inflated(c + 1, size - 1);
	case 'E': {
		// After the mode: the size of the key's name (8) and the name, the size of the nonce and the
		// nonce, the cipher ("S" for Salsa20), the data. The nonce is changed by the chunk's place in the file.
		if (size < 12)
			throw std::runtime_error("a short encrypted BLTE chunk");
		uint8_t name[8];
		std::reverse_copy(c + 2, c + 10, name);
		std::string key_name = hex(name, 8);
		size_t nonce_size = c[10];
		const Key *key = c[1] == 8 && nonce_size <= 8 && 12 + nonce_size <= size && c[11 + nonce_size] == 'S'
					 ? key_of(key_name)
					 : nullptr;
		if (key) {
			uint8_t nonce[8] = {};
			std::memcpy(nonce, c + 11, nonce_size);
			for (int k = 0; k < 4; k++)
				nonce[k] ^= uint8_t(index >> (8 * k));
			Bytes open = salsa20(*key, nonce, c + 12 + nonce_size, size - 12 - nonce_size);
			return chunk(open.data(), open.size(), plain_size, index, skip_hidden, key_of);
		}
		if (skip_hidden && plain_size)
			return Bytes(plain_size);
		throw EncryptedFile(key_name);
	}
	default:
		throw std::runtime_error(std::string("BLTE chunk of unknown kind ") + char(c[0]));
	}
}

// A BLTE container: its chunks one after the other.
Bytes decode_blte(const uint8_t *b, size_t size, bool skip_hidden, const KeyOf &key_of)
{
	if (size < 8 || std::memcmp(b, "BLTE", 4) != 0)
		throw std::runtime_error("not a BLTE container");
	uint32_t header_size = be32(b + 4);
	if (!header_size)
		return chunk(b + 8, size - 8, 0, 0, skip_hidden, key_of);
	if (size < 12)
		throw std::runtime_error("a short BLTE header");
	uint32_t count = uint32_t(b[9]) << 16 | b[10] << 8 | b[11];
	Bytes out;
	size_t at = header_size;
	for (uint32_t i = 0; i < count; i++) {
		size_t entry = 12 + size_t(i) * 24;
		if (entry + 8 > header_size)
			throw std::runtime_error("a BLTE header shorter than its chunks");
		uint32_t part = be32(b + entry), plain = be32(b + entry + 4);
		if (at + part > size)
			throw std::runtime_error("a BLTE container shorter than its chunks");
		Bytes piece = chunk(b + at, part, plain, i, skip_hidden, key_of);
		out.insert(out.end(), piece.begin(), piece.end());
		at += part;
	}
	return out;
}

} // namespace

std::optional<Build> installed_build(const fs::path &folder)
{
	std::ifstream in(folder / ".build.info");
	std::string head, row;
	if (!std::getline(in, head))
		return std::nullopt;
	auto cells = [](const std::string &line) {
		std::vector<std::string> out;
		std::stringstream parts(line);
		for (std::string cell; std::getline(parts, cell, '|');)
			out.push_back(cell.substr(0, cell.find('!')));
		if (!line.empty() && line.back() == '|')
			out.emplace_back();
		return out;
	};
	std::vector<std::string> columns = cells(head);
	auto column = [&](const char *name) { return std::find(columns.begin(), columns.end(), name) - columns.begin(); };
	size_t product = column("Product"), key = column("Build Key"), version = column("Version");
	while (std::getline(in, row)) {
		if (!row.empty() && row.back() == '\r')
			row.pop_back();
		std::vector<std::string> values = cells(row);
		auto get = [&](size_t i) { return i < values.size() ? values[i] : std::string(); };
		if (get(product) == PRODUCT && !get(key).empty())
			return Build{get(version), get(key)};
	}
	return std::nullopt;
}

std::optional<fs::path> game_folder_from(const std::vector<fs::path> &paths)
{
	for (fs::path dir : paths) {
		while (!dir.empty()) {
			if (installed_build(dir))
				return dir;
			fs::path up = dir.parent_path();
			if (up == dir)
				break;
			dir = up;
		}
	}
	return std::nullopt;
}

std::optional<fs::path> registry_install_path()
{
#ifdef _WIN32
	wchar_t path[1024];
	DWORD size = sizeof path;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment\\World of Warcraft", L"InstallPath",
			 RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS)
		return fs::path(path);
#endif
	return std::nullopt;
}

std::map<uint32_t, Key> sent_keys(const fs::path &game_folder)
{
	std::map<uint32_t, Key> keys;
	std::error_code error;
	for (const fs::directory_entry &language : fs::directory_iterator(game_folder / "_forever_" / "Cache" / "ADB", error)) {
		// A cache the game is writing, or none for this language: done without, looked at again next time.
		Bytes cache = read_file(language.path() / "DBCache.bin");
		// A row: "XFTH", three numbers, the table, the row's number, the size of its data, its state, the data.
		uint8_t table[4] = {uint8_t(KEY_TABLE), uint8_t(KEY_TABLE >> 8), uint8_t(KEY_TABLE >> 16), uint8_t(KEY_TABLE >> 24)};
		for (auto at = cache.begin(); (at = std::search(at, cache.end(), table, table + 4)) != cache.end(); ++at) {
			size_t i = at - cache.begin();
			if (i < 16 || i + 16 > cache.size() || std::memcmp(&cache[i - 16], "XFTH", 4) != 0)
				continue;
			uint32_t id = le32(&cache[i + 4]);
			if (le32(&cache[i + 8]) == 16 && cache[i + 12] == VALID && i + 32 <= cache.size()) {
				Key key;
				std::memcpy(key.data(), &cache[i + 16], 16);
				keys[id] = key;
			} else {
				keys.erase(id);
			}
		}
	}
	// ponytail: only the cache is looked at. If a build moves a key into the game's own table of keys
	// (file 1302850), read it there too.
	return keys;
}

GameFiles::GameFiles(const fs::path &folder, std::map<uint32_t, Key> keys_) : keys(std::move(keys_))
{
	std::optional<Build> found = installed_build(folder);
	if (!found)
		throw std::runtime_error(std::string("no ") + PRODUCT + " game in " + folder.u8string());
	build = *found;
	const std::string &key = build.build_key;
	std::ifstream config(folder / "Data" / "config" / key.substr(0, 2) / key.substr(2, 2) / key);
	std::string encoding_key, root_key;
	for (std::string line; std::getline(config, line);) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		size_t at = line.find(" = ");
		if (at == std::string::npos)
			continue;
		std::string name = line.substr(0, at), value = line.substr(at + 3);
		if (name == "encoding" && value.find(' ') != std::string::npos)
			encoding_key = value.substr(value.find(' ') + 1, 32);
		else if (name == "root")
			root_key = value.substr(0, 32);
	}
	if (encoding_key.empty() || root_key.empty())
		throw std::runtime_error("the build configuration names no encoding or root file");

	// The local index: sixteen buckets, the newest file of each, read when first needed.
	data = folder / "Data" / "data";
	std::vector<std::string> names;
	std::error_code error;
	for (const fs::directory_entry &entry : fs::directory_iterator(data, error))
		names.push_back(entry.path().filename().u8string());
	std::sort(names.begin(), names.end());
	static const std::regex idx("^([0-9a-fA-F]{2})[0-9a-fA-F]{8}\\.idx$");
	for (const std::string &name : names) {
		std::smatch m;
		if (std::regex_match(name, m, idx))
			newest[std::stoi(m[1].str(), nullptr, 16)] = name; // sorted: a later version replaces an earlier one
	}

	// Encoding: pages of [key count, 5 bytes of size, content key, encoded keys], found through a page index.
	std::optional<Bytes> got = read_encoded(from_hex(encoding_key).data(), false);
	if (!got || got->size() < 22 || (*got)[0] != 'E' || (*got)[1] != 'N')
		throw std::runtime_error("the encoding file is missing or not one");
	encoding = std::move(*got);
	page_size = (uint32_t(encoding[5]) << 8 | encoding[6]) * 1024;
	pages = be32(&encoding[9]);
	page_index = 22 + be32(&encoding[18]);
	page_data = page_index + pages * 32;
	if (page_data + size_t(pages) * page_size > encoding.size())
		throw std::runtime_error("the encoding file is shorter than its pages");

	// Root: blocks of file numbers (as steps from the one before) and their content keys.
	got = read_content(from_hex(root_key).data(), false);
	if (!got || got->size() < 12)
		throw std::runtime_error("the root file is missing");
	root = std::move(*got);
	size_t at;
	uint32_t version = 0, total, named;
	if (le32(&root[4]) < 0x100) {
		at = le32(&root[4]);
		version = le32(&root[8]);
		total = le32(&root[12]);
		named = le32(&root[16]);
	} else {
		total = le32(&root[4]);
		named = le32(&root[8]);
		at = 12;
	}
	while (at + 12 <= root.size()) {
		uint32_t n = le32(&root[at]), flags, locale;
		if (version >= 2) {
			locale = le32(&root[at + 4]);
			flags = le32(&root[at + 8]) | le32(&root[at + 12]) | uint32_t(root[at + 16]) << 17;
			at += 17;
		} else {
			flags = le32(&root[at + 4]);
			locale = le32(&root[at + 8]);
			at += 12;
		}
		size_t keys_at = at + size_t(n) * 4;
		if (keys_at + size_t(n) * 16 > root.size())
			throw std::runtime_error("the root file is shorter than its blocks");
		uint32_t id = uint32_t(-1);
		for (uint32_t i = 0; i < n; i++) {
			id += uint32_t(int32_t(le32(&root[at + i * 4]))) + 1;
			entries.push_back({id, uint32_t(keys_at + size_t(i) * 16), (locale & EN_US) != 0});
		}
		at = keys_at + size_t(n) * 16 + (total != named && (flags & NO_NAMES) ? 0 : size_t(n) * 8);
	}
	std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.id < b.id; });
	for (size_t i = 0; i < entries.size(); i++)
		file_count += i == 0 || entries[i].id != entries[i - 1].id;
}

const Bytes &GameFiles::entries_of(int bucket)
{
	auto at = buckets.find(bucket);
	if (at != buckets.end())
		return at->second;
	Bytes entries_here;
	auto name = newest.find(bucket);
	if (name != newest.end()) {
		Bytes file = read_file(data / name->second);
		if (file.size() >= 4) {
			size_t start = (8 + le32(&file[0]) + 15) & ~size_t(15);
			if (start + 8 <= file.size()) {
				size_t length = std::min<size_t>(le32(&file[start]), file.size() - start - 8);
				entries_here.assign(file.begin() + start + 8, file.begin() + start + 8 + length);
			}
		}
	}
	return buckets.emplace(bucket, std::move(entries_here)).first->second;
}

std::optional<Bytes> GameFiles::read_encoded(const uint8_t *ekey, bool skip_hidden)
{
	uint8_t x = 0;
	for (int i = 0; i < 9; i++)
		x ^= ekey[i];
	const Bytes &list = entries_of((x & 15) ^ (x >> 4));
	for (long lo = 0, hi = long(list.size() / INDEX_ENTRY) - 1; lo <= hi;) {
		long mid = (lo + hi) / 2;
		const uint8_t *e = &list[size_t(mid) * INDEX_ENTRY];
		int order = std::memcmp(e, ekey, 9);
		if (order < 0) {
			lo = mid + 1;
		} else if (order > 0) {
			hi = mid - 1;
		} else {
			// Five bytes: the archive number in the top ten bits, the offset in the other thirty.
			uint32_t low = be32(e + 10), archive = uint32_t(e[9]) << 2 | low >> 30, offset = low & 0x3fffffff,
				 size = le32(e + 14);
			char name[16];
			std::snprintf(name, sizeof name, "data.%03u", archive);
			std::ifstream in(data / name, std::ios::binary);
			Bytes record(size);
			in.seekg(offset);
			in.read(reinterpret_cast<char *>(record.data()), size);
			// The index lists it, the archive does not hold it: a game still downloading, or damaged.
			if (size_t(in.gcount()) < size)
				throw std::runtime_error(std::string(name) + " ends before the file: the game is not fully downloaded");
			// The container comes after the header of the record, or at once where the record has none.
			bool bare = (size < RECORD_HEADER + 4 || std::memcmp(&record[RECORD_HEADER], "BLTE", 4) != 0) &&
				    size >= 4 && std::memcmp(&record[0], "BLTE", 4) == 0;
			size_t skip = bare ? 0 : RECORD_HEADER;
			if (size < skip)
				throw std::runtime_error(std::string(name) + " holds a record shorter than its header");
			return decode_blte(record.data() + skip, size - skip, skip_hidden,
					   [this](const std::string &key_name) { return key_of(key_name); });
		}
	}
	return std::nullopt;
}

std::optional<Bytes> GameFiles::read_content(const uint8_t *ckey, bool skip_hidden)
{
	uint32_t page = 0;
	for (long lo = 0, hi = long(pages) - 1; lo <= hi;) {
		long mid = (lo + hi) / 2;
		if (std::memcmp(&encoding[page_index + size_t(mid) * 32], ckey, 16) <= 0) {
			page = uint32_t(mid);
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	for (size_t at = page_data + size_t(page) * page_size, end = at + page_size; at + 38 <= end;) {
		uint8_t count = encoding[at];
		if (!count)
			break;
		if (std::memcmp(&encoding[at + 6], ckey, 16) == 0)
			return read_encoded(&encoding[at + 22], skip_hidden);
		at += 22 + size_t(count) * 16;
	}
	return std::nullopt;
}

std::optional<Bytes> GameFiles::read(uint32_t id, bool skip_hidden)
{
	auto range = std::equal_range(entries.begin(), entries.end(), Entry{id, 0, false},
				      [](const Entry &a, const Entry &b) { return a.id < b.id; });
	if (range.first == range.second)
		return std::nullopt;
	// A file in several languages is read in English, or in the first language the PC holds (a PC holds
	// only the languages the player installed). The rest are tried after, in the root's order.
	// ponytail: choose by the game's language when texts are shown.
	auto chosen = range.first;
	for (auto e = range.first; e != range.second; ++e)
		if (e->english)
			chosen = e;
	std::vector<uint32_t> tried{chosen->where};
	for (auto e = range.first; e != range.second; ++e)
		if (e != chosen)
			tried.push_back(e->where);
	for (uint32_t where : tried) {
		std::optional<Bytes> bytes = read_content(&root[where], skip_hidden);
		if (bytes)
			return bytes;
	}
	return std::nullopt;
}

const Key *GameFiles::key_of(const std::string &key_name)
{
	if (keys.empty())
		return nullptr;
	if (!numbers) {
		numbers.emplace(); // set first: reading the table of names must not ask for a key
		std::optional<Bytes> table = read(KEY_NAMES_FILE, true);
		if (table && table->size() >= 204 && std::memcmp(table->data(), "WDC5", 4) == 0 && le32(&(*table)[144]) == 8) {
			// Each row is the 8 bytes of a name; the rows' numbers follow the rows and the texts of their section.
			const Bytes &t = *table;
			for (uint32_t s = 0, sections = le32(&t[200]); s < sections; s++) {
				size_t p = 204 + size_t(s) * 40;
				if (p + 20 > t.size())
					break;
				size_t rows = le32(&t[p + 8]), count = le32(&t[p + 12]), ids = rows + count * 8 + le32(&t[p + 16]);
				for (size_t i = 0; i < count && ids + i * 4 + 4 <= t.size(); i++) {
					uint8_t name[8];
					std::reverse_copy(&t[rows + i * 8], &t[rows + i * 8 + 8], name);
					(*numbers)[hex(name, 8)] = le32(&t[ids + i * 4]);
				}
			}
		}
	}
	auto number = numbers->find(key_name);
	if (number == numbers->end())
		return nullptr;
	auto key = keys.find(number->second);
	return key == keys.end() ? nullptr : &key->second;
}
