#include <LandscapeTextureBuilder.hpp>

#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapedef/LandscapeTex.hpp>
#include <image/ImageFactory.hpp>
#include <image/Image.hpp>
#include <common/Defines.hpp>

#include "SkyDescription.hpp"
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
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

	// How strongly a scorch mark covers the ground at [distance] cells from
	// the blast centre. This reproduces the `explosionDistance` curve
	// DeformLandscapeCache precomputes and DeformLandscape hands to
	// DeformTextures as its blend weight:
	//
	//     depth    = sin((radius - distance) / radius * PI/2) * radius
	//     coverage = min(depth / radius * 3, 1)
	//
	// which simplifies to the below. The *3 then clamp is what makes the
	// mark mostly solid with a soft edge, rather than a smooth dome - so
	// dropping it would visibly change the look, not just the maths.
	float scorchFalloff(float distance, float radius)
	{
		if (radius <= 0.0f || distance >= radius) return 0.0f;
		const float kHalfPi = 1.5707963268f;
		float coverage = std::sin((radius - distance) / radius * kHalfPi) * 3.0f;
		return std::min(coverage, 1.0f);
	}

	// Upstream's ExplosionTextures::getScorchBitmap: a weapon can name its
	// own <deformtexture>, otherwise the landscape definition's <scorch>
	// image is used. Both are ordinary mod files, so ImageFactory (which is
	// in src/common, unlike the rest of that class) loads either.
	//
	// Not cached: a scorch is painted once per blast into a persistent
	// buffer, so the load happens at most a few times a round, and caching
	// would mean owning invalidation across landscape changes for no real
	// gain.
	Image loadScorchImage(ScorchedContext &context, const std::string &textureName)
	{
		if (!textureName.empty() && S3D::fileExists(S3D::getModFile(textureName)))
		{
			return ImageFactory::loadImage(S3D::eModLocation, textureName);
		}

		LandscapeTex *tex = context.getLandscapeMaps().getDefinitions().getTex();
		if (!tex || tex->scorch.empty()) return Image();
		return ImageFactory::loadImage(S3D::eModLocation, tex->scorch);
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

Rect applyScorch(ScorchedContext &context, Texture &texture,
				 int centreX, int centreY, float radius,
				 const std::string &textureName)
{
	Rect touched;
	if (!texture.valid() || radius <= 0.0f) return touched;

	HeightMap &hmap = context.getLandscapeMaps().getGroundMaps().getHeightMap();
	if (hmap.getMapWidth() <= 0 || hmap.getMapHeight() <= 0) return touched;

	Image scorch = loadScorchImage(context, textureName);
	if (scorch.getWidth() <= 0 || scorch.getHeight() <= 0 || scorch.getComponents() < 1) return touched;

	// Upstream's own radius clamp - DeformLandscape refuses to work with a
	// bigger one, so a mark can never be wider than the crater under it.
	int iradius = (int) radius + 1;
	if (iradius > 49) iradius = 49;

	// Heightmap cells -> texture pixels. The texture is stretched over the
	// whole landscape (see the mesh UVs in renderer_jni.cpp), so this is
	// just the ratio of the two resolutions.
	const float pixelsPerCellX = (float) texture.width / (float) hmap.getMapWidth();
	const float pixelsPerCellY = (float) texture.height / (float) hmap.getMapHeight();

	int x0 = (int) std::floor((centreX - iradius) * pixelsPerCellX);
	int y0 = (int) std::floor((centreY - iradius) * pixelsPerCellY);
	int x1 = (int) std::ceil((centreX + iradius) * pixelsPerCellX);
	int y1 = (int) std::ceil((centreY + iradius) * pixelsPerCellY);
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, texture.width - 1);
	y1 = std::min(y1, texture.height - 1);
	if (x0 > x1 || y0 > y1) return touched;  // entirely off the map

	for (int py = y0; py <= y1; py++) {
		// Pixel centre back in heightmap-cell space, relative to the blast.
		const float cellY = ((float) py + 0.5f) / pixelsPerCellY - (float) centreY;
		for (int px = x0; px <= x1; px++) {
			const float cellX = ((float) px + 0.5f) / pixelsPerCellX - (float) centreX;

			const float distance = std::sqrt(cellX * cellX + cellY * cellY);
			const float mag = scorchFalloff(distance, radius);
			if (mag <= 0.0f) continue;

			// Tile the scorch image in texture space, exactly as upstream
			// does - so two craters side by side don't show the same stamp.
			const int sx = ((px % scorch.getWidth()) + scorch.getWidth()) % scorch.getWidth();
			const int sy = ((py % scorch.getHeight()) + scorch.getHeight()) % scorch.getHeight();
			unsigned char *src = scorch.getBitsPos(sx, sy);
			unsigned char *dest = &texture.rgb[(size_t(py) * texture.width + px) * 3];

			// A single-component scorch image is greyscale, so the one
			// value drives all three channels (upstream branches the same
			// way rather than assuming RGB).
			const bool greyscale = (scorch.getComponents() == 1);
			for (int c = 0; c < 3; c++) {
				const float s = (float) src[greyscale ? 0 : c];
				dest[c] = (unsigned char) std::min(255.0f, std::max(0.0f,
					s * mag + (float) dest[c] * (1.0f - mag)));
			}
		}
	}

	touched.x = x0;
	touched.y = y0;
	touched.width = x1 - x0 + 1;
	touched.height = y1 - y0 + 1;
	return touched;
}

Texture applyMovementMask(
	const Texture &source, const unsigned char *mask, int maskWidth, int maskHeight)
{
	Texture result;
	if (!source.valid() || !mask || maskWidth <= 0 || maskHeight <= 0) return result;

	result.width = source.width;
	result.height = source.height;
	result.rgb = source.rgb;

	// Landscape square under a texel. The texture covers the whole
	// landscape once (see build()), so this is a straight scale.
	auto sampleMask = [&](int px, int py) -> bool {
		int mx = px * maskWidth / source.width;
		int my = py * maskHeight / source.height;
		mx = std::min(std::max(mx, 0), maskWidth - 1);
		my = std::min(std::max(my, 0), maskHeight - 1);
		return mask[(size_t) my * maskWidth + mx] != 0;
	};

	for (int y = 0; y < result.height; y++) {
		const int y1 = std::min(y + 1, result.height - 1);
		for (int x = 0; x < result.width; x++) {
			const int x1 = std::min(x + 1, result.width - 1);

			const bool here = sampleMask(x, y);
			const bool right = sampleMask(x1, y);
			const bool below = sampleMask(x, y1);

			unsigned char *dest = &result.rgb[((size_t) y * result.width + x) * 3];
			if (here != right || here != below) {
				// The edge of where you can get to - upstream draws it in
				// flat red, which reads against every ground texture the
				// game ships.
				dest[0] = 255; dest[1] = 0; dest[2] = 0;
			} else if (!here) {
				dest[0] /= 4; dest[1] /= 4; dest[2] /= 4;
			}
		}
	}
	return result;
}

// Upstream's ImageModifier::findIntersection: march from [start] towards
// [end] a texel at a time, reporting the greatest depth by which the ground
// rises above the ray. That depth - not a distance along the ray - is what
// upstream softens the shadow edge with.
static bool findGroundIntersection(HeightMap &hMap,
								   float startX, float startY, float startZ,
								   float endX, float endY, float endZ,
								   float &depth, float stopDepth)
{
	bool blocked = false;
	depth = 0.0f;

	float px = startX, py = startY, pz = startZ;
	float dx = endX - startX, dy = endY - startY, dz = endZ - startZ;

	// One heightmap cell per step along whichever axis moves fastest.
	const float ax = std::fabs(dx), ay = std::fabs(dy);
	const float scale = (ax > ay) ? ax : ay;
	if (scale < 0.0001f) return false;
	dx /= scale; dy /= scale; dz /= scale;

	const int width = hMap.getMapWidth();
	const int height = hMap.getMapHeight();

	while (px >= 0.0f && py >= 0.0f && px <= (float) width && py <= (float) height) {
		const float ground =
			hMap.getHeight((int) px, (int) py).asFloat() - 0.1f;
		const float above = ground - pz;
		if (above > 0.0f) {
			if (above > depth) depth = above;
			blocked = true;
			if (depth > stopDepth) return blocked;
		}
		px += dx; py += dy; pz += dz;
	}
	return blocked;
}

bool applyLightMap(ScorchedContext &context, Texture &texture)
{
	if (!texture.valid()) return false;

	HeightMap &hMap = context.getLandscapeMaps().getGroundMaps().getHeightMap();
	if (hMap.getMapWidth() <= 0 || hMap.getMapHeight() <= 0) return false;

	ScorchDroidSky::Description sky = ScorchDroidSky::describe(context);

	// Upstream's own two constants: a 256x256 light map regardless of the
	// texture's size, and a soft edge three units deep.
	const int lightMapSize = 256;
	const float softShadow = 3.0f;

	std::vector<float> light((size_t) lightMapSize * lightMapSize * 3, 1.0f);
	for (int y = 0; y < lightMapSize; y++) {
		for (int x = 0; x < lightMapSize; x++) {
			const float dx = (float) x / (float) lightMapSize * (float) hMap.getMapWidth();
			const float dy = (float) y / (float) lightMapSize * (float) hMap.getMapHeight();
			const float dz = hMap.getInterpHeight(
				fixed::fromFloat(dx), fixed::fromFloat(dy)).asFloat();

			FixedVector fixedNormal;
			hMap.getInterpNormal(fixed::fromFloat(dx), fixed::fromFloat(dy), fixedNormal);

			float sx = sky.sunPosition[0] - dx;
			float sy = sky.sunPosition[1] - dy;
			float sz = sky.sunPosition[2] - dz;
			const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
			if (slen > 0.0001f) { sx /= slen; sy /= slen; sz /= slen; }

			// Upstream's half-lambert: (n.l)/2 + 0.5, so ground facing away
			// still catches something rather than going flat black.
			float lambert = (fixedNormal[0].asFloat() * sx +
							 fixedNormal[1].asFloat() * sy +
							 fixedNormal[2].asFloat() * sz) * 0.5f + 0.5f;

			float depth = 0.0f;
			if (findGroundIntersection(hMap, dx, dy, dz,
					sky.sunPosition[0], sky.sunPosition[1], sky.sunPosition[2],
					depth, softShadow)) {
				lambert *= (depth < softShadow) ? (1.0f - depth / softShadow) : 0.0f;
			}

			float *out = &light[((size_t) y * lightMapSize + x) * 3];
			for (int c = 0; c < 3; c++) {
				out[c] = std::min(1.0f, sky.diffuse[c] * lambert + sky.ambience[c]);
			}
		}
	}

	// Multiply into the texture, nearest-sampled. Upstream scales the light
	// map up with gluScaleImage first; sampling straight from it is the same
	// thing without the intermediate copy, and the map is smooth enough that
	// the difference doesn't show.
	for (int y = 0; y < texture.height; y++) {
		const int ly = std::min(y * lightMapSize / texture.height, lightMapSize - 1);
		for (int x = 0; x < texture.width; x++) {
			const int lx = std::min(x * lightMapSize / texture.width, lightMapSize - 1);
			const float *lit = &light[((size_t) ly * lightMapSize + lx) * 3];
			unsigned char *dest = &texture.rgb[((size_t) y * texture.width + x) * 3];
			for (int c = 0; c < 3; c++) {
				dest[c] = (unsigned char) std::min(255.0f,
					std::max(0.0f, (float) dest[c] * lit[c]));
			}
		}
	}
	return true;
}

std::vector<unsigned char> buildShoreMask(ScorchedContext &context,
										  float waterHeight, int size,
										  float reach)
{
	std::vector<unsigned char> mask;
	if (size <= 0 || reach <= 0.0f) return mask;

	HeightMap &hMap = context.getLandscapeMaps().getGroundMaps().getHeightMap();
	const int mapW = hMap.getMapWidth(), mapH = hMap.getMapHeight();
	if (mapW <= 0 || mapH <= 0) return mask;

	mask.assign((size_t) size * size, 0);
	for (int y = 0; y < size; y++) {
		const int sy = std::min(y * mapH / size, mapH - 1);
		for (int x = 0; x < size; x++) {
			const int sx = std::min(x * mapW / size, mapW - 1);
			const float ground = hMap.getHeight(sx, sy).asFloat();

			// Only *under* the water: foam belongs on the submerged shelf
			// by the shore, not on dry land, and not out in deep water.
			const float depth = waterHeight - ground;
			if (depth < 0.0f || depth > reach) continue;

			// 255 at the waterline, fading to nothing at full depth.
			const float strength = 1.0f - (depth / reach);
			mask[(size_t) y * size + x] =
				(unsigned char) std::min(255.0f, std::max(0.0f, strength * 255.0f));
		}
	}
	return mask;
}

}  // namespace LandscapeTextureBuilder
