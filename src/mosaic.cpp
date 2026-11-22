// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mosaic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>

namespace {

constexpr int AROUND = 160;        // yards of nothing kept around the rooms: the minimap sees that far past the last wall
constexpr int PIXELS_PER_YARD = 2; // of a room's picture

double reach(const Piece &p)
{
	return std::hypot(p.width, p.height) / 2;
}

// JavaScript's Math.round, for the numbers met here.
double js_round(double x)
{
	return std::floor(x + 0.5);
}

// A double stored in a Uint8ClampedArray: to the nearest, a half to the even one.
uint8_t clamped(double x)
{
	return static_cast<uint8_t>(std::nearbyint(std::clamp(x, 0.0, 255.0)));
}

// The pixels of a room's DXT1 blocks, four bytes each; a pixel left out by a block of one bit
// of transparency stays at 0.
std::vector<uint8_t> room_pixels(const std::vector<uint8_t> &dxt, int width, int height)
{
	std::vector<uint8_t> rgba(size_t(width) * height * 4);
	using Color = std::array<double, 3>;
	auto color = [](uint16_t c) {
		return Color{((c >> 11) & 31) * 255.0 / 31, ((c >> 5) & 63) * 255.0 / 63, (c & 31) * 255.0 / 31};
	};
	size_t p = 0;
	for (int by = 0; by < height; by += 4) {
		for (int bx = 0; bx < width; bx += 4, p += 8) {
			if (p + 8 > dxt.size())
				return rgba;
			uint16_t c0, c1;
			uint32_t choice;
			std::memcpy(&c0, &dxt[p], 2);
			std::memcpy(&c1, &dxt[p + 2], 2);
			std::memcpy(&choice, &dxt[p + 4], 4);
			Color a = color(c0), b = color(c1), four[4];
			bool clear = false;
			four[0] = a;
			four[1] = b;
			for (int i = 0; i < 3; i++) {
				if (c0 > c1) {
					four[2][i] = (2 * a[i] + b[i]) / 3;
					four[3][i] = (a[i] + 2 * b[i]) / 3;
				} else {
					four[2][i] = (a[i] + b[i]) / 2;
					clear = true;
				}
			}
			for (int i = 0; i < 16; i++) {
				int k = (choice >> (i * 2)) & 3;
				if (k == 3 && clear)
					continue;
				size_t at = (size_t(by + (i >> 2)) * width + bx + (i & 3)) * 4;
				if (at + 4 > rgba.size())
					continue;
				rgba[at] = clamped(four[k][0]);
				rgba[at + 1] = clamped(four[k][1]);
				rgba[at + 2] = clamped(four[k][2]);
				rgba[at + 3] = 255;
			}
		}
	}
	return rgba;
}

} // namespace

Mosaic mosaic_of_pieces(const std::vector<Piece> &pieces, const PictureOf &picture_of)
{
	Mosaic m;
	if (pieces.empty())
		return m;
	double lu = INFINITY, lv = INFINITY, hu = -INFINITY, hv = -INFINITY;
	for (const Piece &p : pieces) {
		lu = std::min(lu, p.u - reach(p));
		lv = std::min(lv, p.v - reach(p));
		hu = std::max(hu, p.u + reach(p));
		hv = std::max(hv, p.v + reach(p));
	}
	m.u0 = int(std::floor(lu)) - AROUND;
	m.v0 = int(std::floor(lv)) - AROUND;
	m.width = int(std::ceil(hu)) + AROUND - m.u0;
	m.height = int(std::ceil(hv)) + AROUND - m.v0;
	m.grey.assign(size_t(m.width) * m.height, 0);

	for (const Piece &p : pieces) {
		const std::vector<uint8_t> &dxt = picture_of(p.file);
		if (dxt.empty())
			continue;
		int w = int(js_round(p.width * PIXELS_PER_YARD)), h = int(js_round(p.height * PIXELS_PER_YARD));
		std::vector<uint8_t> rgba = room_pixels(dxt, w, h);
		int r = int(std::ceil(reach(p))) + 1;
		double cos = std::cos(p.angle), sin = std::sin(p.angle);
		int pu = int(std::floor(p.u)), pv = int(std::floor(p.v));
		for (int v = pv - r; v < pv + r; v++) {
			for (int u = pu - r; u < pu + r; u++) {
				double du = u + 0.5 - p.u, dv = v + 0.5 - p.v;
				double x = du * cos - dv * sin + p.width / 2, y = du * sin + dv * cos + p.height / 2;
				if (x < 0 || y < 0 || x >= p.width || y >= p.height)
					continue;
				int px = std::min(w - 1, int(std::floor(x * PIXELS_PER_YARD)));
				int py = std::min(h - 1, int(std::floor(y * PIXELS_PER_YARD)));
				const uint8_t *c = &rgba[(size_t(py) * w + px) * 4];
				if (c[3])
					m.grey[size_t(v - m.v0) * m.width + (u - m.u0)] =
						uint8_t(js_round(c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114));
			}
		}
	}
	return m;
}

std::vector<Mosaic> mosaics_by_level(const std::vector<Piece> &pieces, const PictureOf &picture_of)
{
	std::vector<Mosaic> out;
	int top = 0;
	for (const Piece &p : pieces)
		top = std::max(top, p.level);
	for (int level = 0; level <= top && !pieces.empty(); level++) {
		std::vector<Piece> upto;
		std::copy_if(pieces.begin(), pieces.end(), std::back_inserter(upto),
			     [level](const Piece &p) { return p.level <= level; });
		out.push_back(mosaic_of_pieces(upto, picture_of));
	}
	return out;
}
