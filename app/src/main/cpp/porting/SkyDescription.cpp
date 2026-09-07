#include "SkyDescription.hpp"

#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapedef/LandscapeDefinitions.hpp>
#include <landscapedef/LandscapeTex.hpp>
#include <image/ImageFactory.hpp>
#include <common/Defines.hpp>

#include <algorithm>
#include <cmath>

namespace ScorchDroidSky
{
	Description describe(ScorchedContext &context)
	{
		Description sky;

		LandscapeTex *tex = context.getLandscapeMaps().getDefinitions().getTex();
		if (!tex) return sky;

		sky.horizonGlow = !tex->nohorizonglow;
		for (int i = 0; i < 3; i++) {
			sky.fog[i] = tex->fog[i];
			sky.sunColor[i] = tex->suncolor[i];
		}

		// Upstream's Sun::setPosition, minus the 900-unit radius and the
		// map-centre offset - only the direction is wanted here.
		const float xy = tex->skysunxy * 3.14159265f / 180.0f;
		const float yz = tex->skysunyz * 3.14159265f / 180.0f;
		sky.sunDirection[0] = sinf(xy) * cosf(yz);
		sky.sunDirection[1] = cosf(xy) * cosf(yz);
		sky.sunDirection[2] = sinf(yz);
		const float length = sqrtf(
			sky.sunDirection[0] * sky.sunDirection[0] +
			sky.sunDirection[1] * sky.sunDirection[1] +
			sky.sunDirection[2] * sky.sunDirection[2]);
		if (length > 0.0001f) {
			for (int i = 0; i < 3; i++) sky.sunDirection[i] /= length;
		}

		if (tex->skycolormap.empty()) return sky;
		Image colors = ImageFactory::loadImage(S3D::eModLocation, tex->skycolormap);
		if (!colors.getBits() || colors.getWidth() <= 0 || colors.getHeight() <= 0) {
			return sky;
		}

		// Hemisphere::drawColored's own indexing:
		//   bitmapIndex = daytime * components + (16 * components) * heightIndex
		// i.e. a row is 16 pixels of time-of-day and the row number is the
		// height above the horizon. Clamped rather than trusted: a mod can
		// ship any image here, and reading off the end of it would be a
		// crash rather than a wrong colour.
		const int components = colors.getComponents();
		const int rowStride = kGradientSteps * components;
		const int daytime = std::min(std::max(tex->skytimeofday, 0), kGradientSteps - 1);
		const int totalBytes = colors.getWidth() * colors.getHeight() * components;

		for (int step = 0; step < kGradientSteps; step++) {
			const int index = daytime * components + rowStride * step;
			if (index < 0 || index + 2 >= totalBytes) return sky;  // not upstream's layout
			const unsigned char *pixel = &colors.getBits()[index];
			for (int i = 0; i < 3; i++) {
				sky.gradient[step][i] = (components >= 3 ? pixel[i] : pixel[0]) / 255.0f;
			}
		}

		sky.valid = true;
		return sky;
	}
}
