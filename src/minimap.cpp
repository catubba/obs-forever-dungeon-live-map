// OBS Forever Dungeon Live Map
// Copyright (C) 2026 catubba
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The numbers and their kinds (double, or float where Twinmap keeps a Float32Array) follow Twinmap,
// so that the same minimap gives the same place.

#include "minimap.h"

#include <algorithm>
#include <cmath>
#include <numeric>

const double INDOOR_YARDS[6] = {300, 240, 180, 120, 80, 50};
const double OUTDOOR_YARDS[6] = {466.67, 400, 333.33, 266.67, 200, 133.33};

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double KEEP = 0.8;  // of the radius: the rim has buttons over it
constexpr double ARROW = 0.1; // of the radius, around the middle: the arrow of the character and the dots of the party
constexpr int STEPS[3] = {6, 2, 1}; // yards a pixel, from the look at the whole dungeon to the last yard
constexpr double NEAR = 12; // yards around the last place where the next is looked for: more than a second of running
constexpr double LIT = 40;  // of 255: a pixel lighter than this shows something; under a roof the background is black
constexpr double BAND = 9;  // yards around what the minimap shows that still count: walls are told by the black beside them
// Of the round middle: less lit than this and the minimap shows nothing to look for, only the dots of the party.
constexpr double SHOWN = 0.02;

// The follower.
constexpr double EVERY = 500;    // ms between two looks at the minimap
constexpr double LOST_FOR = 2000; // ms between two looks through the whole dungeon while the place is not known
constexpr double KEPT = 3000;    // ms a place found is still given without a newer one
constexpr double RUNNING = 2;    // yards between two looks that tell the character is running
// A place a step from the one of a moment ago is taken at this likeness: the dots of a party, a ping, a tooltip
// over the minimap lower it, and a wrong place cannot be more than a few yards off.
constexpr double FOLLOW = 0.6;
constexpr double CLEARLY = 0.01; // how much more like another picture the minimap must be before it is taken

double js_round(double x)
{
	return std::floor(x + 0.5);
}

double grey(const uint8_t *rgb)
{
	return rgb[0] * 0.299 + rgb[1] * 0.587 + rgb[2] * 0.114;
}

// How much of the round middle of the minimap shows something: 0 to 1.
double shown_part(const Box &box)
{
	int all = 0, lit = 0;
	double half = box.size / 2.0;
	for (int y = 0; y < box.size; y++) {
		for (int x = 0; x < box.size; x++) {
			double dist = std::hypot(x + 0.5 - half, y + 0.5 - half) / half;
			if (dist >= KEEP || dist <= ARROW)
				continue;
			all++;
			if (grey(&box.rgb[(size_t(y) * box.size + x) * 3]) > LIT)
				lit++;
		}
	}
	return all ? double(lit) / all : 0;
}

// The round middle of the minimap at `step` yards a pixel: where each pixel that counts is, and how far
// its grey is from their mean.
struct Template {
	int size;
	std::vector<int> dx, dy;
	std::vector<float> value;
	double power; // the sum of the squares of the values, before they are kept as float
};

Template template_of(const Box &box, double across, int step, int thin)
{
	int size = int(js_round(across / step));
	double per = double(box.size) / size;
	std::vector<float> greys(size_t(size) * size);
	std::vector<uint8_t> counts(size_t(size) * size), lit(size_t(size) * size);
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			double dist = std::hypot(x + 0.5 - size / 2.0, y + 0.5 - size / 2.0) / (size / 2.0);
			if (dist >= KEEP || dist <= ARROW)
				continue;
			// The mean of the pixels of the screen under this one.
			int x0 = int(std::floor(x * per)), x1 = std::max(x0 + 1, int(std::floor((x + 1) * per)));
			int y0 = int(std::floor(y * per)), y1 = std::max(y0 + 1, int(std::floor((y + 1) * per)));
			double g = 0;
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++)
					g += grey(&box.rgb[(size_t(sy) * box.size + sx) * 3]);
			g /= (x1 - x0) * (y1 - y0);
			greys[size_t(y) * size + x] = float(g);
			counts[size_t(y) * size + x] = 1;
			if (g > LIT)
				lit[size_t(y) * size + x] = 1;
		}
	}
	// How many lit pixels there are up to each one, to ask of any square whether it holds one.
	int s = size + 1;
	std::vector<int> sums(size_t(s) * s);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			sums[(y + 1) * s + x + 1] = lit[size_t(y) * size + x] + sums[y * s + x + 1] + sums[(y + 1) * s + x] -
						    sums[y * s + x];
	int band = int(std::ceil(BAND / step));
	Template t{size, {}, {}, {}, 0};
	std::vector<double> value;
	double sum = 0;
	for (int y = 0; y < size; y += thin) {
		for (int x = 0; x < size; x += thin) {
			if (!counts[size_t(y) * size + x])
				continue;
			// Only what the minimap shows and a band around it: the game leaves out rooms the map has.
			int x0 = std::max(0, x - band), x1 = std::min(size, x + band + 1);
			int y0 = std::max(0, y - band), y1 = std::min(size, y + band + 1);
			if (sums[y1 * s + x1] - sums[y0 * s + x1] - sums[y1 * s + x0] + sums[y0 * s + x0] == 0)
				continue;
			t.dx.push_back(x);
			t.dy.push_back(y);
			value.push_back(greys[size_t(y) * size + x]);
			sum += greys[size_t(y) * size + x];
		}
	}
	double mean = sum / value.size();
	for (double &v : value) {
		v -= mean;
		t.power += v * v;
	}
	for (double v : value)
		t.value.push_back(float(v));
	return t;
}

// How alike the template and the rooms are with the template's corner on (x, y): 1 the same picture.
double likeness(const Levels::At &level, const Template &t, int x, int y)
{
	double cross = 0, sum = 0, squares = 0;
	size_t n = t.value.size();
	const float *g = level.grey.data();
	for (size_t i = 0; i < n; i++) {
		double v = g[size_t(y + t.dy[i]) * level.width + x + t.dx[i]];
		cross += v * t.value[i];
		sum += v;
		squares += v * v;
	}
	double spread = squares - (sum * sum) / n;
	return spread <= 1e-6 || t.power <= 1e-6 ? 0 : cross / std::sqrt(spread * t.power);
}

struct Corner {
	int x, y;
	double score;
};

// The best corner for the template in a rectangle of corners, both ends counted.
Corner best(const Levels::At &level, const Template &t, int x0, int y0, int x1, int y1)
{
	Corner found{-1, -1, -1};
	for (int y = std::max(0, y0); y <= std::min(level.height - t.size, y1); y++) {
		for (int x = std::max(0, x0); x <= std::min(level.width - t.size, x1); x++) {
			double score = likeness(level, t, x, y);
			if (score > found.score)
				found = {x, y, score};
		}
	}
	return found;
}

} // namespace

Levels levels_of(const Mosaic &m)
{
	Levels out{m.u0, m.v0, m.level, m.open, {}};
	for (int step : STEPS) {
		Levels::At at{step, m.width / step, m.height / step, {}};
		at.grey.resize(size_t(at.width) * at.height);
		for (int y = 0; y < at.height; y++) {
			for (int x = 0; x < at.width; x++) {
				double sum = 0;
				for (int dy = 0; dy < step; dy++)
					for (int dx = 0; dx < step; dx++)
						sum += m.grey[size_t(y * step + dy) * m.width + x * step + dx];
				at.grey[size_t(y) * at.width + x] = float(sum / (step * step));
			}
		}
		out.at.push_back(std::move(at));
	}
	return out;
}

std::optional<Found> locate(const Levels &levels, const Box &box, double across, const std::optional<Found> &close)
{
	if (!(across > 0) || box.size < 32 || shown_part(box) < SHOWN)
		return std::nullopt;
	std::optional<Corner> at;
	int yards = 0;
	for (const Levels::At &level : levels.at) {
		bool first = !at;
		if (first && close && level.step == STEPS[0])
			continue; // the last place is a better start than the coarse look
		// One pixel in four of the template is enough past the coarse look.
		Template t = template_of(box, across, level.step, level.step == STEPS[0] ? 1 : 2);
		if (at) {
			// Around the place found at the step before, by as much as that step could be wrong.
			int x = int(js_round(double(at->x) * yards / level.step)), y = int(js_round(double(at->y) * yards / level.step));
			int reach = int(std::ceil(double(yards) / level.step)) + 1;
			at = best(level, t, x - reach, y - reach, x + reach, y + reach);
		} else if (close) {
			int x = int(js_round((close->u - levels.u0 - across / 2) / level.step));
			int y = int(js_round((close->v - levels.v0 - across / 2) / level.step));
			int reach = int(std::ceil(NEAR / level.step));
			at = best(level, t, x - reach, y - reach, x + reach, y + reach);
		} else {
			at = best(level, t, 0, 0, level.width, level.height);
		}
		yards = level.step;
		if (at->x < 0)
			return std::nullopt;
	}
	if (!at)
		return std::nullopt;
	return Found{levels.u0 + at->x + across / 2, levels.v0 + at->y + across / 2, at->score};
}

std::optional<double> facing_of(const Box &box)
{
	// Measured on a minimap of 238 pixels: the arrow fits in 13 pixels around the middle, its eye in 9.
	double scale = box.size / 238.0, middle = box.size / 2.0;
	int reach = int(js_round(13 * scale));
	int n = 0, eyes = 0;
	double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, ex = 0, ey = 0;
	int m = int(std::floor(middle));
	for (int py = m - reach; py <= m + reach; py++) {
		for (int px = m - reach; px <= m + reach; px++) {
			if (px < 0 || py < 0 || px >= box.size || py >= box.size)
				continue;
			double x = px + 0.5 - middle, y = py + 0.5 - middle, dist = std::hypot(x, y);
			if (dist > reach)
				continue;
			const uint8_t *c = &box.rgb[(size_t(py) * box.size + px) * 3];
			int r = c[0], g = c[1], b = c[2];
			int most = std::max({r, g, b}), least = std::min({r, g, b});
			if (most > 150 && most - least < 38) {
				n++;
				sx += x;
				sy += y;
				sxx += x * x;
				syy += y * y;
				sxy += x * y;
			}
			if (b > r + 40 && b > g + 30 && b < 190 && dist <= 9 * scale) {
				eyes++;
				ex += x;
				ey += y;
			}
		}
	}
	if (n < 12 * scale * scale || eyes < 3)
		return std::nullopt;
	// The long way of the light pixels: the direction in which they are spread the most.
	double cxx = sxx / n - (sx / n) * (sx / n), cyy = syy / n - (sy / n) * (sy / n), cxy = sxy / n - (sx / n) * (sy / n);
	double turn = std::atan2(2 * cxy, cxx - cyy) / 2;
	double ax = std::cos(turn), ay = std::sin(turn);
	if (ax * (ex / eyes) + ay * (ey / eyes) > 0) { // the point is away from the eye
		ax = -ax;
		ay = -ay;
	}
	return std::fmod(std::atan2(ax, -ay) * 180 / PI + 360, 360);
}

std::vector<double> widths_of(int zoom)
{
	std::vector<double> out;
	if (zoom < 0 || zoom > 5)
		return out;
	for (double width : {INDOOR_YARDS[zoom], OUTDOOR_YARDS[zoom]})
		if (std::none_of(out.begin(), out.end(), [&](double w) { return std::abs(w - width) < width * 0.03; }))
			out.push_back(width);
	return out;
}

Follower::Follower(const std::vector<Mosaic> &mosaics, std::vector<double> widths_) : widths(std::move(widths_))
{
	for (const Mosaic &m : mosaics)
		floors.push_back(levels_of(m));
}

std::optional<Inside> Follower::fresh(double now) const
{
	if (last && now - last->at <= KEPT)
		return last->inside;
	return std::nullopt;
}

std::optional<Inside> Follower::look(const Box &box, double now)
{
	if (now - looked_at < EVERY)
		return fresh(now);
	looked_at = now;
	if (floors.empty() || widths.empty())
		return std::nullopt;
	std::optional<Last> known = last && now - last->at <= KEPT ? last : std::nullopt;
	if (!known && now - searched_at < LOST_FOR)
		return std::nullopt;
	if (!known)
		searched_at = now;

	// With no place known: the whole dungeon on one picture at one width, the next of them at the next try.
	// The picture with every room first, then down the levels, then the open ground.
	std::optional<Found> around;
	if (known) {
		around = Found{known->inside.u, known->inside.v, 0};
	} else {
		size_t count = floors.size();
		size_t turn = lost_tries++ % (count * widths.size());
		size_t rooms = std::count_if(floors.begin(), floors.end(), [](const Levels &f) { return !f.open; });
		std::vector<size_t> order(count);
		std::iota(order.begin(), order.end(), 0);
		auto key = [rooms](size_t a) { return a < rooms ? double(rooms) - 1 - a : double(a); };
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
		// The picture and the width the character was last found on before any other.
		std::vector<size_t> first = order;
		std::vector<double> wide = widths;
		if (last) {
			first = {last->picture};
			for (size_t i : order)
				if (i != last->picture)
					first.push_back(i);
			if (std::find(widths.begin(), widths.end(), last->across) != widths.end()) {
				wide = {last->across};
				for (double w : widths)
					if (w != last->across)
						wide.push_back(w);
			}
		}
		around = locate(floors[first[turn % count]], box, wide[turn / count], std::nullopt);
	}
	if (!around)
		return fresh(now);

	// Every picture at every width, around that place. While the picture and the width of a moment ago still
	// fit, only the other levels at that width are tried.
	std::optional<Found> found, before;
	size_t picture = 0;
	double across = 0;
	std::optional<Found> still = known ? locate(floors[known->picture], box, known->across, around) : std::nullopt;
	bool steady = known && still && still->score >= SURE;
	for (size_t i = 0; i < floors.size(); i++) {
		if (steady && i != known->picture && (floors[i].open || floors[known->picture].open))
			continue;
		std::vector<double> tried = steady ? std::vector<double>{known->across} : widths;
		for (double width : tried) {
			std::optional<Found> here = steady && i == known->picture ? still : locate(floors[i], box, width, around);
			if (!here)
				continue;
			if (known && i == known->picture && width == known->across)
				before = here;
			// Alike on several: the last of them, which among the levels is the one with every room on show.
			if (!found || here->score >= found->score) {
				found = here;
				picture = i;
				across = width;
			}
		}
	}
	// The picture and the width of a moment ago stay unless another is clearly more like the minimap.
	if (known && before && found && before->score >= found->score - CLEARLY) {
		found = before;
		picture = known->picture;
		across = known->across;
	}
	if (!found || found->score < (known ? FOLLOW : SURE))
		return fresh(now);

	// Running, the way the place moved is the surest facing; standing, the arrow of the minimap says it.
	double du = known ? found->u - known->inside.u : 0, dv = known ? found->v - known->inside.v : 0;
	std::optional<double> facing;
	if (std::hypot(du, dv) >= RUNNING)
		facing = std::fmod(std::atan2(du, -dv) * 180 / PI + 360, 360);
	else if (!(facing = facing_of(box)) && last)
		facing = last->inside.facing;
	last = Last{{found->u, found->v, facing, floors[picture].level}, now, picture, across};
	lost_tries = 0;
	return fresh(now);
}
