// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads game files by their numbers from a game folder, with the keys the server sent to it, and
// writes each to <out folder>/<number>.bin; parts locked with a key never sent come as zeros. Read only
// on the game's side.
//   casc-read <game folder> <out folder> <number>...

#include "casc.h"

#include <chrono>
#include <cstdio>
#include <fstream>

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "casc-read <game folder> <out folder> <number>...\n");
		return 2;
	}
	try {
		auto started = std::chrono::steady_clock::now();
		std::map<uint32_t, Key> keys = sent_keys(argv[1]);
		GameFiles files(argv[1], keys);
		double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::printf("build %s (%s): %zu files, %zu keys sent, opened in %.0f ms\n", files.build.version.c_str(),
			    files.build.build_key.c_str(), files.count(), keys.size(), took);
		int failed = 0;
		for (int i = 3; i < argc; i++) {
			uint32_t id = uint32_t(std::stoul(argv[i]));
			try {
				std::optional<Bytes> bytes;
				try {
					bytes = files.read(id);
				} catch (const EncryptedFile &e) {
					// Parts locked with a key never sent: read without them, as the converter does.
					std::printf("%u: part locked with key %s, read without it\n", id, e.key_name.c_str());
					bytes = files.read(id, true);
				}
				if (!bytes) {
					std::printf("%u: no such file\n", id);
					failed++;
					continue;
				}
				std::ofstream(std::filesystem::path(argv[2]) / (std::to_string(id) + ".bin"), std::ios::binary)
					.write(reinterpret_cast<const char *>(bytes->data()), std::streamsize(bytes->size()));
				std::printf("%u: %zu bytes\n", id, bytes->size());
			} catch (const std::exception &e) {
				std::printf("%u: %s\n", id, e.what());
				failed++;
			}
		}
		return failed ? 1 : 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
