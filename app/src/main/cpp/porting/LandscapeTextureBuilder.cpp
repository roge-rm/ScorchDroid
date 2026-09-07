#include <LandscapeTextureBuilder.hpp>

#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapedef/LandscapeTex.hpp>
#include <image/ImageFactory.hpp>
#include <image/Image.hpp>
#include <common/Defines.hpp>

#include <algorithm>
#include <cmath>

namespace
{
	// Upstream's constants, kept identical so the generated ground reads
	// the same as the PC game's (GLImageModifier::addHeightToBitmap).
	const float kMaxHeight = 30.0f;             // last texture band ends here
	const float kBlendHeightFactor = 0.4f;      // start blending 40% into a band
	const float kBlendNormalSlopeStart = 0.8f;  // start showing rock below this normal.z
	const float kBlendNormalSlopeLength = 0.3f;
	const float kBlendNormalShoreStart = 0.8f;
	const float kBlendNormalShoreLength = 0.1f;
	const float kNoiseMax = 0.4f;
	const int   kNumberSources = 4;

	// Source textures tile across the landscape rather than being stretched
	// over it (upstream uses a wrapping ImageItterator for the same effect).
	void addBlended(float *dest, Image &src, int x, int y, float amount)
	{
		if (amount <= 0.0f || src.getWidth() <= 0 || src.getHeight() <= 0) return;
		int sx = ((x % src.getWidth()) + src.getWidth()) % src.getWidth();
		int sy = ((y % src.getHeight()) + src.getHeight()) % src.getHeight();
		unsigned char *bits = src.getBitsPos(sx, sy);
		dest[0] += bits[0] * amount;
		dest[1] += bits[1] * amount;
		dest[2] += bits[2] * amount;
	}
}

namespace LandscapeTextureBuilder
{

Texture build(ScorchedContext &context, int size, std::string *error)
{
	Texture result;
	auto fail = [&](const char *why) { if (error) *error = why; return result; };
	if (size <= 0) return fail("bad size");

	HeightMap &hmap = context.getLandscapeMaps().getGroundMaps().getHeightMap();
	if (hmap.getMapWidth() <= 0 || hmap.getMapHeight() <= 0) return fail("no landscape generated yet");

	LandscapeTex *tex = context.getLandscapeMaps().getDefinitions().getTex();
	if (!tex || !tex->texture) return fail("landscape definition has no texture block");

	// Upstream supports two ground-texture styles and real landscapes use
	// both, so we need both too (see Landscape::generate()'s two branches).
	// eTextureFile is the simple one: the landscape ships a ready-made
	// ground image, so there's nothing to blend - just resample it.
	if (tex->texture->getType() == LandscapeTexType::eTextureFile) {
		LandscapeTexTextureFile *fileTex = (LandscapeTexTextureFile *) tex->texture;
		Image loaded = ImageFactory::loadImage(S3D::eModLocation, fileTex->texture);
		if (loaded.getWidth() <= 0) {
			if (error) *error = std::string("could not load landscape texture image: ") + fileTex->texture;
			return result;
		}

		// Nearest-neighbour resample into our own buffer. Upstream calls
		// Image::createResize() here, which this build of Image doesn't
		// have - and sampling directly avoids the dependency entirely.
		result.width = size;
		result.height = size;
		result.rgb.assign(size_t(size) * size * 3, 0);
		for (int y = 0; y < size; y++) {
			int sy = std::min(loaded.getHeight() - 1, y * loaded.getHeight() / size);
			for (int x = 0; x < size; x++) {
				int sx = std::min(loaded.getWidth() - 1, x * loaded.getWidth() / size);
				unsigned char *src = loaded.getBitsPos(sx, sy);
				unsigned char *dest = &result.rgb[(size_t(y) * size + x) * 3];
				dest[0] = src[0];
				dest[1] = src[1];
				dest[2] = src[2];
			}
		}
		return result;
	}

	if (tex->texture->getType() != LandscapeTexType::eTextureGenerate) {
		return fail("landscape texture style is neither eTextureGenerate nor eTextureFile");
	}
	LandscapeTexTextureGenerate *generate = (LandscapeTexTextureGenerate *) tex->texture;

	// Same loader upstream uses - these are ordinary mod-relative image
	// files, and src/common/image (png/jpg/bmp) is in our build.
	Image sources[kNumberSources] = {
		ImageFactory::loadImage(S3D::eModLocation, generate->texture0),
		ImageFactory::loadImage(S3D::eModLocation, generate->texture1),
		ImageFactory::loadImage(S3D::eModLocation, generate->texture2),
		ImageFactory::loadImage(S3D::eModLocation, generate->texture3),
	};
	Image rock = ImageFactory::loadImage(S3D::eModLocation, generate->rockside);
	Image shore = ImageFactory::loadImage(S3D::eModLocation, generate->shore);

	const char *sourceNames[kNumberSources] = {
		generate->texture0.c_str(), generate->texture1.c_str(),
		generate->texture2.c_str(), generate->texture3.c_str(),
	};
	for (int i = 0; i < kNumberSources; i++) {
		if (sources[i].getWidth() <= 0) {
			if (error) *error = std::string("could not load ground texture image: ") + sourceNames[i];
			return result;
		}
	}

	// Noise term needs the map's peak height (upstream scans for it too).
	float maxMapHeight = 0.0f;
	for (int y = 0; y < hmap.getMapHeight(); y++) {
		for (int x = 0; x < hmap.getMapWidth(); x++) {
			maxMapHeight = std::max(maxMapHeight, hmap.getHeight(x, y).asFloat());
		}
	}
	if (maxMapHeight <= 0.0f) maxMapHeight = 1.0f;

	result.width = size;
	result.height = size;
	result.rgb.assign(size_t(size) * size * 3, 0);

	const float hdx = (float) hmap.getMapWidth() / (float) size;
	const float hdy = (float) hmap.getMapHeight() / (float) size;

	FixedVector normalVec;
	for (int by = 0; by < size; by++) {
		float hy = by * hdy;
		for (int bx = 0; bx < size; bx++) {
			float hx = bx * hdx;

			hmap.getInterpNormal(fixed::fromFloat(hx), fixed::fromFloat(hy), normalVec);
			float normalZ = normalVec[2].asFloat();
			float height = hmap.getInterpHeight(fixed::fromFloat(hx), fixed::fromFloat(hy)).asFloat();

			// Sample the map from the opposite corner as cheap noise, so the
			// height bands don't band up into visible contour lines.
			float offsetHeight = hmap.getInterpHeight(
				fixed::fromFloat((float) hmap.getMapWidth() - hx),
				fixed::fromFloat((float) hmap.getMapHeight() - hy)).asFloat();
			height *= (1.0f - (kNoiseMax / 2.0f)) + ((offsetHeight * kNoiseMax) / maxMapHeight);

			// Which height band, and how far into it (for cross-fading).
			float heightPer = (height / kMaxHeight) * (float) kNumberSources;
			int heightIndex = (int) heightPer;
			if (heightIndex < 0) heightIndex = 0;
			if (heightIndex >= kNumberSources) heightIndex = kNumberSources - 1;

			float blendFirst = 1.0f, blendSecond = 0.0f;
			if (heightIndex < kNumberSources - 1) {
				float remainder = heightPer - heightIndex;
				if (remainder > kBlendHeightFactor) {
					remainder -= kBlendHeightFactor;
					blendSecond = remainder / (1.0f - kBlendHeightFactor);
					blendFirst = 1.0f - blendSecond;
				}
			}

			// Steep ground shows rock; near-flat ground at the waterline
			// shows sand. normal.z is 1 on flat ground, smaller on slopes.
			float blendSide = 0.0f, blendShore = 0.0f;
			if (normalZ < kBlendNormalSlopeStart) {
				if (normalZ < kBlendNormalSlopeStart - kBlendNormalSlopeLength) {
					blendSide = 1.0f;
					blendFirst = blendSecond = 0.0f;
				} else {
					float remainder = (normalZ - (kBlendNormalSlopeStart - kBlendNormalSlopeLength)) / kBlendNormalSlopeLength;
					blendSide = 1.0f - remainder;
					blendFirst *= remainder;
					blendSecond *= remainder;
				}
			} else if (normalZ > kBlendNormalShoreStart && height > 3.5f && height < 5.5f) {
				if (normalZ > kBlendNormalShoreStart + kBlendNormalShoreLength) {
					blendShore = 1.0f;
					blendFirst = blendSecond = 0.0f;
				} else {
					float remainder = (normalZ - kBlendNormalSlopeStart) / kBlendNormalSlopeLength;
					blendShore = 1.0f - remainder;
					blendFirst *= remainder;
					blendSecond *= remainder;
				}
			}

			float rgb[3] = { 0.0f, 0.0f, 0.0f };
			addBlended(rgb, sources[heightIndex], bx, by, blendFirst);
			if (blendSecond > 0.0f) addBlended(rgb, sources[heightIndex + 1], bx, by, blendSecond);
			if (blendSide > 0.0f) addBlended(rgb, rock, bx, by, blendSide);
			if (blendShore > 0.0f) addBlended(rgb, shore, bx, by, blendShore);

			unsigned char *dest = &result.rgb[(size_t(by) * size + bx) * 3];
			for (int c = 0; c < 3; c++) {
				dest[c] = (unsigned char) std::min(255.0f, std::max(0.0f, rgb[c]));
			}
		}
	}

	return result;
}

}  // namespace LandscapeTextureBuilder
