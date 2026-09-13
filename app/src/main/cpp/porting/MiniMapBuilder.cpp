#include <MiniMapBuilder.hpp>

#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapedef/LandscapeDefinitionCache.hpp>
#include <landscapedef/LandscapeTex.hpp>

#include <algorithm>

namespace MiniMapBuilder
{
	namespace
	{
		// Upstream's waterline band, from
		// GLImageModifier::removeWaterFromBitmap: anything higher than the
		// water is solid, the 0.3 units directly below it are half
		// transparent, and everything below that is a hole.
		//
		// The band is what stops the coastline being a hard jagged edge at
		// this resolution - one texel is several world units across, so
		// without it the shore aliases badly.
		const float kShoreBand = 0.3f;

		const uint8_t kAlphaLand  = 255;
		const uint8_t kAlphaShore = 128;
		const uint8_t kAlphaWater = 0;

		uint8_t alphaFor(float height, float water)
		{
			if (height > water) return kAlphaLand;
			if (height > water - kShoreBand) return kAlphaShore;
			return kAlphaWater;
		}

		// The height under destination texel (x, y), by nearest sample.
		float heightUnder(const Heights &heights, int x, int y, int size)
		{
			const int hx = std::min(heights.width,
				int((float(x) + 0.5f) * float(heights.width) / float(size)));
			const int hy = std::min(heights.height,
				int((float(y) + 0.5f) * float(heights.height) / float(size)));
			return heights.at(hx, hy);
		}
	}

	float Heights::at(int x, int y) const
	{
		if (x < 0 || y < 0 || x > width || y > height) return 0.0f;
		const size_t i = (size_t) y * (width + 1) + x;
		if (i >= heights.size()) return 0.0f;
		return heights[i];
	}

	Heights captureHeights(ScorchedContext &context)
	{
		Heights out;
		HeightMap &hmap = context.getLandscapeMaps().getGroundMaps().getHeightMap();
		const int w = hmap.getMapWidth(), h = hmap.getMapHeight();
		if (w <= 0 || h <= 0) return out;

		out.width = w;
		out.height = h;
		out.heights.resize((size_t) (w + 1) * (h + 1));
		for (int y = 0; y <= h; y++) {
			for (int x = 0; x <= w; x++) {
				out.heights[(size_t) y * (w + 1) + x] = hmap.getHeight(x, y).asFloat();
			}
		}
		return out;
	}

	float waterHeight(ScorchedContext &context)
	{
		// Upstream's own "no water" value for this exact call.
		const float kNoWater = -50.0f;

		LandscapeTex *tex = context.getLandscapeMaps().getDefinitions().getTex();
		if (!tex || !tex->border) return kNoWater;
		if (tex->border->getType() != LandscapeTexType::eWater) return kNoWater;

		LandscapeTexBorderWater *water = (LandscapeTexBorderWater *) tex->border;
		return water->height.asFloat();
	}

	Image build(const LandscapeTextureBuilder::Texture &ground,
	            const Heights &heights,
	            float water,
	            int size)
	{
		Image out;
		if (size <= 0 || !ground.valid() || !heights.valid()) return out;

		out.size = size;
		out.argb.assign((size_t) size * size, 0);

		// A box average rather than a nearest sample. Upstream downsamples
		// with gluScaleImage, which box-filters; taking one texel in eight
		// instead would alias the ground texture's noise into a mess of
		// speckle at this size.
		for (int y = 0; y < size; y++) {
			const int sy0 = (int) ((long long) y * ground.height / size);
			const int sy1 = std::max(sy0 + 1, (int) ((long long) (y + 1) * ground.height / size));
			for (int x = 0; x < size; x++) {
				const int sx0 = (int) ((long long) x * ground.width / size);
				const int sx1 = std::max(sx0 + 1, (int) ((long long) (x + 1) * ground.width / size));

				unsigned long r = 0, g = 0, b = 0, n = 0;
				for (int sy = sy0; sy < sy1 && sy < ground.height; sy++) {
					const unsigned char *row = &ground.rgb[(size_t) sy * ground.width * 3];
					for (int sx = sx0; sx < sx1 && sx < ground.width; sx++) {
						r += row[sx * 3 + 0];
						g += row[sx * 3 + 1];
						b += row[sx * 3 + 2];
						n++;
					}
				}
				if (n == 0) n = 1;

				const uint8_t a = alphaFor(heightUnder(heights, x, y, size), water);
				out.argb[(size_t) y * size + x] =
					(uint32_t(a) << 24) |
					(uint32_t(r / n) << 16) |
					(uint32_t(g / n) << 8) |
					uint32_t(b / n);
			}
		}
		return out;
	}

	void updateWater(Image &image, const Heights &heights, float water)
	{
		if (!image.valid() || !heights.valid()) return;

		for (int y = 0; y < image.size; y++) {
			for (int x = 0; x < image.size; x++) {
				const uint8_t a = alphaFor(heightUnder(heights, x, y, image.size), water);
				uint32_t &texel = image.argb[(size_t) y * image.size + x];
				texel = (texel & 0x00FFFFFFu) | (uint32_t(a) << 24);
			}
		}
	}
}
