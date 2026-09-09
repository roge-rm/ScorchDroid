#include <ShoreBreakers.h>

#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>

#include <cmath>

namespace
{
	struct Context
	{
		int mapWidth = 0, mapHeight = 0;
		std::vector<unsigned char> marked;   // one per cell, the shore points
		unsigned int rng = 1;
		float random()
		{
			// Stands in for upstream's RAND (rand() / RAND_MAX); its own
			// generator so the result is reproducible from the seed.
			rng = rng * 1664525u + 1013904223u;
			return (float) ((rng >> 8) & 0xffffffu) / (float) 0x1000000u;
		}
		bool isMarked(int x, int y) const
		{
			if (x < 0 || y < 0 || x >= mapWidth || y >= mapHeight) return false;
			return marked[(size_t) y * mapWidth + x] != 0;
		}
	};

	float groundHeight(ScorchedContext &context, int x, int y)
	{
		return context.getLandscapeMaps().getGroundMaps().getHeight(x, y).asFloat();
	}

	// WaterWaves::findPoints: a cell within 4 units below the waterline
	// whose four neighbours include one above it. (Upstream's pointsMult
	// is 1, so every cell is looked at.)
	void findPoints(ScorchedContext &sc, Context &c, float waterHeight)
	{
		for (int y = 1; y < c.mapHeight - 1; y++) {
			for (int x = 1; x < c.mapWidth - 1; x++) {
				const float h = groundHeight(sc, x, y);
				if (!(h > waterHeight - 4.0f && h < waterHeight)) continue;
				if (groundHeight(sc, x + 1, y) > waterHeight ||
					groundHeight(sc, x - 1, y) > waterHeight ||
					groundHeight(sc, x, y + 1) > waterHeight ||
					groundHeight(sc, x, y - 1) > waterHeight) {
					c.marked[(size_t) y * c.mapWidth + x] = 1;
				}
			}
		}
	}

	struct Point { float x, y; };

	// WaterWaves::findPath, which is tail-recursive: take the point, unmark
	// it, and step to the first marked neighbour in upstream's order.
	void findPath(Context &c, std::vector<Point> &points, int x, int y)
	{
		static const int order[8][2] = {
			{ 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 },
			{ -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 },
		};
		while (x >= 0 && x < c.mapWidth && y >= 0 && y < c.mapHeight) {
			points.push_back({ (float) x, (float) y });
			c.marked[(size_t) y * c.mapWidth + x] = 0;
			bool moved = false;
			for (int i = 0; i < 8; i++) {
				const int nx = x + order[i][0], ny = y + order[i][1];
				if (c.isMarked(nx, ny)) { x = nx; y = ny; moved = true; break; }
			}
			if (!moved) return;
		}
	}

	// WaterWaves::constructLines, step for step - including its comparison
	// of a squared distance against a plain one, and its stepping back a
	// point after each segment so consecutive segments overlap.
	void constructLines(ScorchedContext &sc, Context &c, float waterHeight,
						std::vector<Point> &points, std::vector<ScorchDroidBreakers::Segment> &out)
	{
		if (points.empty()) return;
		Point point = points.front();
		int dist = (int) (c.random() * 10.0f + 1.0f);
		for (int i = 0; i < (int) points.size(); i++) {
			const Point current = points[i];
			const int diffX = (int) current.x - (int) point.x;
			const int diffY = (int) current.y - (int) point.y;
			const int actualDist = diffX * diffX + diffY * diffY;
			if (actualDist >= dist) {
				// grad = point - current; perp = (grad.y, -grad.x) normalised.
				const float gx = point.x - current.x, gy = point.y - current.y;
				float px = gy, py = -gx;
				float mag = sqrtf(px * px + py * py);
				if (mag == 0.0f) mag = 0.00001f;
				px /= mag; py /= mag;

				const int newX = (int) (current.x + px * 3.0f);
				const int newY = (int) (current.y + py * 3.0f);
				if (!(newX <= 0 || newY <= 0 || newX >= c.mapWidth || newY >= c.mapHeight)) {
					ScorchDroidBreakers::Segment s;
					if (groundHeight(sc, newX, newY) > waterHeight) {
						px = -px; py = -py;
						s.ax = current.x - px / 3.0f; s.ay = current.y - py / 3.0f;
						s.bx = point.x - px / 3.0f;   s.by = point.y - py / 3.0f;
						s.cx = point.x + px * 6.0f;   s.cy = point.y + py * 6.0f;
						s.dx = current.x + px * 6.0f; s.dy = current.y + py * 6.0f;
					} else {
						s.ax = point.x - px / 3.0f;   s.ay = point.y - py / 3.0f;
						s.bx = current.x - px / 3.0f; s.by = current.y - py / 3.0f;
						s.cx = current.x + px * 6.0f; s.cy = current.y + py * 6.0f;
						s.dx = point.x + px * 6.0f;   s.dy = point.y + py * 6.0f;
					}
					s.perpX = px; s.perpY = py;
					s.set = (c.random() > 0.5f) ? 0 : 1;
					out.push_back(s);
				}

				if (i > 1) {
					i -= 1;
					point = points[i];
				} else {
					point = current;
				}
				dist = (int) (c.random() * 10.0f + 1.0f);
			}
		}
	}
}

namespace ScorchDroidBreakers
{
	std::vector<Segment> build(ScorchedContext &context, float waterHeight, unsigned int seed)
	{
		std::vector<Segment> result;
		Context c;
		c.mapWidth = context.getLandscapeMaps().getGroundMaps().getLandscapeWidth();
		c.mapHeight = context.getLandscapeMaps().getGroundMaps().getLandscapeHeight();
		if (c.mapWidth <= 2 || c.mapHeight <= 2) return result;
		c.marked.assign((size_t) c.mapWidth * c.mapHeight, 0);
		c.rng = seed ? seed : 1u;

		findPoints(context, c, waterHeight);

		// WaterWaves::findNextPath: the first marked point (scanning from
		// 1, as upstream does) starts a path; repeat until none are left.
		std::vector<Point> points;
		for (int y = 1; y < c.mapHeight; y++) {
			for (int x = 1; x < c.mapWidth; x++) {
				if (!c.isMarked(x, y)) continue;
				points.clear();
				findPath(c, points, x, y);
				constructLines(context, c, waterHeight, points, result);
			}
		}
		return result;
	}
}
