// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the player's own game files, straight from the game's archive (CASC) on their PC. Read only:
// nothing is written there. What the game keeps encrypted stays unread, but for the parts whose key
// the server has already sent to this PC. Ported from Twinmap Forever (app/src/main/client-files.ts,
// keys.ts, shared/salsa20.ts).
//
// The chain, from a file's number to its bytes:
//   .build.info            which build of the product is installed, and its build key
//   Data/config/../<key>   the build's configuration: where its "encoding" and "root" files are
//   Data/data/*.idx        local index: first 9 bytes of an encoded key -> archive, offset, size
//   Data/data/data.NNN     the archives; each record is a 30 byte header and a BLTE container.
//                          On some installs a record is the container alone, with no header
//                          (Twinmap issue 116): told by where the container starts
//   encoding               content key -> encoded key
//   root                   file number -> content key

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/** World of Warcraft: Forever, as the game's own files call it. */
constexpr const char *PRODUCT = "wowf";

using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 16>;

/** A file, or a part of one, that the game keeps encrypted and the server has sent this PC no key for. */
struct EncryptedFile : std::runtime_error {
	std::string key_name;
	explicit EncryptedFile(const std::string &name)
		: std::runtime_error("this game file is encrypted (key " + name + ") and the game has not been given its key"),
		  key_name(name)
	{
	}
};

/** The build of the product installed in a game folder (the one holding .build.info). */
struct Build {
	std::string version, build_key;
};
std::optional<Build> installed_build(const std::filesystem::path &folder);

/** The game folder: the first of these paths, or of their parents, that holds the product. */
std::optional<std::filesystem::path> game_folder_from(const std::vector<std::filesystem::path> &paths);

/** Where the game's installer said it put the game (Windows registry); none when it says nothing. */
std::optional<std::filesystem::path> registry_install_path();

/**
 * The keys the server has sent to the game in this folder, by the game's number of each: the game keeps
 * them in its cache (_forever_/Cache/ADB/<language>/DBCache.bin). None when there is no cache.
 */
std::map<uint32_t, Key> sent_keys(const std::filesystem::path &game_folder);

/** The game files of a game folder. Opening reads the build's two big tables once. */
class GameFiles {
public:
	/** Throws std::runtime_error when the folder holds no readable game. */
	GameFiles(const std::filesystem::path &folder, std::map<uint32_t, Key> keys = {});

	Build build;
	/** How many files the build lists. */
	size_t count() const { return file_count; }

	/**
	 * The bytes of a file by its number; none when the build has no such file. Throws EncryptedFile,
	 * unless skip_hidden: then the parts still locked come as zeros.
	 */
	std::optional<Bytes> read(uint32_t id, bool skip_hidden = false);

	/** Whether the parts locked with the key of this name are opened. */
	bool opens(const std::string &key_name) { return key_of(key_name) != nullptr; }

private:
	struct Entry {
		uint32_t id, where; // the file's number, and where its content key is in the root
		bool english;
	};
	std::filesystem::path data;
	std::map<int, std::string> newest; // index bucket -> its newest .idx file
	std::map<int, Bytes> buckets;      // index bucket -> its entries, read when first needed
	std::map<uint32_t, Key> keys;
	std::optional<std::map<std::string, uint32_t>> numbers; // key name -> the game's number of the key
	Bytes encoding, root;
	uint32_t page_size = 0, pages = 0, page_index = 0, page_data = 0;
	std::vector<Entry> entries; // by file number
	size_t file_count = 0;

	const Bytes &entries_of(int bucket);
	std::optional<Bytes> read_encoded(const uint8_t *ekey, bool skip_hidden);
	std::optional<Bytes> read_content(const uint8_t *ckey, bool skip_hidden);
	const Key *key_of(const std::string &key_name);
};
