#include "TreeGeometry.hpp"

#include <cmath>

namespace ScorchDroidTrees
{
	namespace
	{
		const float kPi = 3.14159265358979323846f;
		const float kDegToRad = kPi / 180.0f;

		// A tiny deterministic generator standing in for upstream's RAND.
		// Any cheap sequence would do; what matters is that it is seeded per
		// tree type rather than shared global state, so the same type builds
		// identically every run. See the header for why.
		struct Random
		{
			unsigned int state;
			explicit Random(unsigned int seed) : state(seed * 2654435761u + 1u) {}
			// 0..1
			float next()
			{
				state = state * 1664525u + 1013904223u;
				return (float) ((state >> 8) & 0xFFFFFF) / (float) 0xFFFFFF;
			}
		};

		struct Vec3 { float x, y, z; };

		Vec3 cross(const Vec3 &a, const Vec3 &b)
		{
			return Vec3{ a.y * b.z - a.z * b.y,
						 a.z * b.x - a.x * b.z,
						 a.x * b.y - a.y * b.x };
		}
		Vec3 sub(const Vec3 &a, const Vec3 &b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }

		// Upstream's geometry is Z-up (its models' convention); the renderer
		// is Y-up. Same remap uploadMeshGroup uses, negation included - a
		// plain axis swap is a reflection and would wind every face
		// backwards.
		void emit(std::vector<float> &out,
				  float px, float py, float pz,
				  float nx, float ny, float nz,
				  float u, float v)
		{
			out.push_back(px);  out.push_back(pz);  out.push_back(-py);
			out.push_back(nx);  out.push_back(nz);  out.push_back(-ny);
			out.push_back(u);   out.push_back(v);
		}

		// One vertex of a fan/quad, before the remap.
		struct Vert
		{
			Vec3 p, n;
			float u, v;
		};

		void emitVert(std::vector<float> &out, const Vert &vert)
		{
			emit(out, vert.p.x, vert.p.y, vert.p.z,
				 vert.n.x, vert.n.y, vert.n.z, vert.u, vert.v);
		}

		void emitTriangle(std::vector<float> &out, const Vert &a, const Vert &b, const Vert &c)
		{
			emitVert(out, a); emitVert(out, b); emitVert(out, c);
		}

		// ModelRendererTree::drawPineLevel - one skirt of branches as a
		// triangle fan from a point at [height] down to a ring of radius
		// [width] at [lowheight]. The texture coordinate sweeps a circle of
		// radius [texWidth] about the atlas cell centre, which is what makes
		// the layer read as foliage from above.
		void pineLevel(std::vector<float> &out, Random &random,
					   float texX, float texY, float width, float height, float lowheight,
					   float texWidth = 0.125f, float count = 5.0f,
					   bool doubleSide = false, float angOffset = 0.0f)
		{
			// Upstream walks i from 360 down to 0 inclusive, so there are
			// count + 1 rim vertices and count triangles.
			std::vector<Vert> rim;
			for (float i = 360.0f; i >= 0.0f; i -= 360.0f / count)
			{
				const float diff = random.next() * 40.0f - 20.0f;
				Vert vert;
				vert.p = Vec3{ sinf(i * kDegToRad) * width,
							   cosf(i * kDegToRad) * width,
							   lowheight };
				vert.n = Vec3{ sinf((i + diff) * kDegToRad) * (height - lowheight),
							   cosf((i + diff) * kDegToRad) * (height - lowheight),
							   width };
				vert.u = texX + sinf((i + angOffset) * kDegToRad) * texWidth;
				vert.v = texY + cosf((i + angOffset) * kDegToRad) * texWidth;
				rim.push_back(vert);
			}

			Vert centre;
			centre.p = Vec3{ 0.0f, 0.0f, height };
			centre.n = Vec3{ 0.0f, 0.0f, 1.0f };
			centre.u = texX;
			centre.v = texY;

			for (size_t i = 0; i + 1 < rim.size(); i++)
			{
				emitTriangle(out, centre, rim[i], rim[i + 1]);
			}

			if (!doubleSide) return;

			// Upstream's second pass runs the other way round with negated
			// normals, giving the layer a lit underside.
			std::vector<Vert> back;
			for (float i = 0.0f; i <= 360.0f; i += 360.0f / count)
			{
				const float diff = random.next() * 40.0f - 20.0f;
				Vert vert;
				vert.p = Vec3{ sinf(i * kDegToRad) * width,
							   cosf(i * kDegToRad) * width,
							   lowheight };
				vert.n = Vec3{ -sinf((i + diff) * kDegToRad) * (height - lowheight),
							   -cosf((i + diff) * kDegToRad) * (height - lowheight),
							   -width };
				vert.u = texX + sinf((i + angOffset) * kDegToRad) * texWidth;
				vert.v = texY + cosf((i + angOffset) * kDegToRad) * texWidth;
				back.push_back(vert);
			}
			for (size_t i = 0; i + 1 < back.size(); i++)
			{
				emitTriangle(out, centre, back[i], back[i + 1]);
			}
		}

		// ModelRendererTree::drawPineTrunc - the trunk, a very low-sided
		// cone from the ground ring up to a point. Its texture coordinates
		// come from the bark strip at the edge of the atlas.
		void pineTrunk(std::vector<float> &out,
					   float width, float height, float lowheight,
					   float x = 0.875f, float y = 0.0f, float w = 0.125f,
					   float h = 0.1f, float steps = 3.0f)
		{
			std::vector<Vert> rim;
			for (float i = 360.0f; i >= 0.0f; i -= 360.0f / steps)
			{
				Vert vert;
				vert.p = Vec3{ sinf(i * kDegToRad) * width,
							   cosf(i * kDegToRad) * width,
							   lowheight };
				vert.n = Vec3{ sinf(i * kDegToRad), cosf(i * kDegToRad), 0.0f };
				// Upstream's own expression, kept verbatim - it walks along
				// the bark strip rather than wrapping it.
				vert.u = x + w * ((float) (((int) (i * 5.0f)) % 360) / 360.0f);
				vert.v = y + h;
				rim.push_back(vert);
			}

			Vert centre;
			centre.p = Vec3{ 0.0f, 0.0f, height };
			centre.n = Vec3{ 0.0f, 0.0f, 1.0f };
			centre.u = x;
			centre.v = y;

			for (size_t i = 0; i + 1 < rim.size(); i++)
			{
				emitTriangle(out, centre, rim[i], rim[i + 1]);
			}
		}

		// ModelRendererTree::drawPalmTrunc - a tapered cylinder as a quad
		// strip, capped with a small fan.
		void palmTrunk(std::vector<float> &out,
					   float width, float height, float count,
					   float x = 0.0f, float y = 0.0f, float w = 1.0f, float h = 0.125f)
		{
			bool tex = false;
			std::vector<Vert> strip;
			for (float i = 360.0f; i >= 0.0f; i -= 360.0f / count)
			{
				const Vec3 normal{ sinf(i * kDegToRad), cosf(i * kDegToRad), 0.0f };

				Vert top;
				top.p = Vec3{ sinf(i * kDegToRad) * (width - 0.1f),
							  cosf(i * kDegToRad) * (width - 0.1f),
							  height };
				top.n = normal;
				top.u = x;
				top.v = tex ? y : y + h;
				strip.push_back(top);

				Vert bottom;
				bottom.p = Vec3{ sinf(i * kDegToRad) * width,
								 cosf(i * kDegToRad) * width,
								 0.0f };
				bottom.n = normal;
				bottom.u = x + w;
				bottom.v = tex ? y : y + h;
				strip.push_back(bottom);

				tex = !tex;
			}

			// Quad strip to triangles, keeping the strip's alternating
			// winding.
			for (size_t i = 0; i + 3 < strip.size(); i += 2)
			{
				emitTriangle(out, strip[i], strip[i + 1], strip[i + 2]);
				emitTriangle(out, strip[i + 2], strip[i + 1], strip[i + 3]);
			}

			std::vector<Vert> cap;
			for (float i = 360.0f; i >= 0.0f; i -= 360.0f / count)
			{
				Vert vert;
				vert.p = Vec3{ sinf(i * kDegToRad) * 0.03f,
							   cosf(i * kDegToRad) * 0.03f,
							   height - 0.01f };
				vert.n = Vec3{ sinf(i * kDegToRad), cosf(i * kDegToRad), 0.0f };
				vert.u = x + w;
				vert.v = tex ? y : y + h;
				cap.push_back(vert);
				tex = !tex;
			}

			Vert centre;
			centre.p = Vec3{ 0.0f, 0.0f, height + 0.05f };
			centre.n = Vec3{ 0.0f, 0.0f, 1.0f };
			centre.u = x + w;
			centre.v = y;

			for (size_t i = 0; i + 1 < cap.size(); i++)
			{
				emitTriangle(out, centre, cap[i], cap[i + 1]);
			}
		}

		// ModelRendererTree::drawPalmLevel - the fronds, four quads per
		// step, each a long tapering blade with its own random droop.
		void palmLevel(std::vector<float> &out, Random &random,
					   float width1, float w2, float height, float height2,
					   float count, float texX, float texY)
		{
			auto quad = [&](const Vec3 &p1, const Vec3 &p2, const Vec3 &p3, const Vec3 &p4,
							const Vec3 &normal,
							float u1, float v1, float u2, float v2,
							float u3, float v3, float u4, float v4) {
				Vert a{ p1, normal, u1, v1 };
				Vert b{ p2, normal, u2, v2 };
				Vert c{ p3, normal, u3, v3 };
				Vert d{ p4, normal, u4, v4 };
				emitTriangle(out, a, b, c);
				emitTriangle(out, a, c, d);
			};

			for (float i = 360.0f; i >= 0.0f; )
			{
				const float diff = 0.5f * random.next() - 0.25f;
				const float width2 = (w2 * random.next() * 0.3f) + (0.7f * w2);

				const float sinLow = sinf((i - 15.0f) * kDegToRad);
				const float cosLow = cosf((i - 15.0f) * kDegToRad);
				const float sinMid = sinf(i * kDegToRad);
				const float cosMid = cosf(i * kDegToRad);
				const float sinHigh = sinf((i + 15.0f) * kDegToRad);
				const float cosHigh = cosf((i + 15.0f) * kDegToRad);

				const Vec3 A1{ sinLow * width1, cosLow * width1, height };
				const Vec3 A2{ sinMid * width1, cosMid * width1, height2 };
				const Vec3 A3{ sinMid * width2, cosMid * width2, height2 + diff };
				const Vec3 A4{ sinLow * width2, cosLow * width2, height + diff };
				quad(A1, A2, A3, A4, cross(sub(A3, A4), sub(A3, A2)),
					 texX, texY, texX, texY + 0.123f,
					 texX + 0.37f, texY + 0.123f, texX + 0.37f, texY);

				const Vec3 B1{ sinLow * width2, cosLow * width2, height + diff };
				const Vec3 B2{ sinMid * width2, cosMid * width2, height2 + diff };
				const Vec3 B3{ sinMid * width1, cosMid * width1, height2 };
				const Vec3 B4{ sinLow * width1, cosLow * width1, height };
				quad(B1, B2, B3, B4, cross(sub(B1, B3), sub(B2, B1)),
					 texX + 0.37f, texY, texX + 0.37f, texY + 0.123f,
					 texX, texY + 0.123f, texX, texY);

				const Vec3 C1{ sinHigh * width2, cosHigh * width2, height + diff };
				const Vec3 C2{ sinMid * width2, cosMid * width2, height2 + diff };
				const Vec3 C3{ sinMid * width1, cosMid * width1, height2 };
				const Vec3 C4{ sinHigh * width1, cosHigh * width1, height };
				quad(C1, C2, C3, C4, cross(sub(C2, C1), sub(C3, C2)),
					 texX + 0.37f, texY, texX + 0.37f, texY + 0.123f,
					 texX, texY + 0.123f, texX, texY);

				const Vec3 D1{ sinHigh * width1, cosHigh * width1, height };
				const Vec3 D2{ sinMid * width1, cosMid * width1, height2 };
				const Vec3 D3{ sinMid * width2, cosMid * width2, height2 + diff };
				const Vec3 D4{ sinHigh * width2, cosHigh * width2, height + diff };
				quad(D1, D2, D3, D4, cross(sub(D4, D3), sub(D2, D4)),
					 texX, texY, texX, texY + 0.123f,
					 texX + 0.37f, texY + 0.123f, texX + 0.37f, texY);

				i -= 360.0f / (random.next() * (count - 1.0f) + count);
			}
		}
	}

	const char *atlasImage(Atlas atlas)
	{
		switch (atlas) {
			case eAtlasPineA: return "data/textures/pine2.bmp";
			case eAtlasPineB: return "data/textures/pine3.bmp";
			case eAtlasPalmA: return "data/textures/pine.bmp";
			case eAtlasPalmB: return "data/textures/palm2.bmp";
			case eAtlasOak:   return "data/textures/oak.bmp";
			default:          return "";
		}
	}

	const char *atlasMask(Atlas atlas)
	{
		switch (atlas) {
			case eAtlasPineA: return "data/textures/pine2a.bmp";
			case eAtlasPineB: return "data/textures/pine3a.bmp";
			case eAtlasPalmA: return "data/textures/pinea.bmp";
			case eAtlasPalmB: return "data/textures/palm2a.bmp";
			case eAtlasOak:   return "data/textures/oaka.bmp";
			default:          return "";
		}
	}

	Atlas atlasFor(TreeModelFactory::TreeType type)
	{
		switch (type) {
			case TreeModelFactory::ePineNormal:
			case TreeModelFactory::ePineBurnt:
			case TreeModelFactory::ePineSnow:
			case TreeModelFactory::ePineYellow:
			case TreeModelFactory::ePineLight:
				return eAtlasPineA;
			case TreeModelFactory::ePine2:
			case TreeModelFactory::ePine3:
			case TreeModelFactory::ePine4:
			case TreeModelFactory::ePine2Snow:
			case TreeModelFactory::ePine3Snow:
			case TreeModelFactory::ePine4Snow:
				return eAtlasPineB;
			case TreeModelFactory::ePalmNormal:
			case TreeModelFactory::ePalmBurnt:
			case TreeModelFactory::ePalm2:
			case TreeModelFactory::ePalm3:
			case TreeModelFactory::ePalm4:
				return eAtlasPalmA;
			case TreeModelFactory::ePalmB:
			case TreeModelFactory::ePalmB2:
			case TreeModelFactory::ePalmB3:
			case TreeModelFactory::ePalmB4:
			case TreeModelFactory::ePalmB5:
			case TreeModelFactory::ePalmB6:
			case TreeModelFactory::ePalmB7:
				return eAtlasPalmB;
			default:
				return eAtlasOak;
		}
	}

	bool isBurnt(TreeModelFactory::TreeType type)
	{
		return type == TreeModelFactory::ePineBurnt ||
			   type == TreeModelFactory::ePalmBurnt;
	}

	int build(TreeModelFactory::TreeType type, std::vector<float> &out)
	{
		const size_t startFloats = out.size();
		// Seeded by the type, so the same kind of tree is identical every
		// run and two kinds do not share a sequence.
		Random random((unsigned int) type + 7u);

		switch (type)
		{
		// Pine family: a trunk and three skirts, sampling one column of the
		// atlas. The (texX, texY) pairs are upstream's own.
		case TreeModelFactory::ePineNormal:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.625f, 0.875f, 0.7f, 0.3f, 0.1f);
			pineLevel(out, random, 0.375f, 0.875f, 0.5f, 0.7f, 0.2f);
			pineLevel(out, random, 0.125f, 0.875f, 0.3f, 1.1f, 0.5f);
			break;
		case TreeModelFactory::ePineSnow:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.625f, 0.625f, 0.7f, 0.3f, 0.1f);
			pineLevel(out, random, 0.375f, 0.625f, 0.5f, 0.7f, 0.2f);
			pineLevel(out, random, 0.125f, 0.625f, 0.3f, 1.1f, 0.5f);
			break;
		case TreeModelFactory::ePineYellow:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.625f, 0.375f, 0.7f, 0.3f, 0.1f);
			pineLevel(out, random, 0.375f, 0.375f, 0.5f, 0.7f, 0.2f);
			pineLevel(out, random, 0.125f, 0.375f, 0.3f, 1.1f, 0.5f);
			break;
		case TreeModelFactory::ePineLight:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.625f, 0.125f, 0.7f, 0.3f, 0.1f);
			pineLevel(out, random, 0.375f, 0.125f, 0.5f, 0.7f, 0.2f);
			pineLevel(out, random, 0.125f, 0.125f, 0.3f, 1.1f, 0.5f);
			break;
		case TreeModelFactory::ePineBurnt:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.875f, 0.875f, 0.7f, 0.3f, 0.1f);
			pineLevel(out, random, 0.875f, 0.875f, 0.5f, 0.7f, 0.2f);
			pineLevel(out, random, 0.875f, 0.875f, 0.3f, 1.1f, 0.5f);
			break;

		// Pine 2-4 and their snow variants use the denser eight-sided skirt
		// off the second atlas.
		case TreeModelFactory::ePine2:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.18f, 0.836f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.836f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.836f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;
		case TreeModelFactory::ePine3:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.18f, 0.5f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.5f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.5f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;
		case TreeModelFactory::ePine4:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.18f, 0.172f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.172f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.18f, 0.172f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;
		case TreeModelFactory::ePine2Snow:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.52f, 0.836f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.836f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.836f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;
		case TreeModelFactory::ePine3Snow:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.52f, 0.5f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.5f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.5f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;
		case TreeModelFactory::ePine4Snow:
			pineTrunk(out, 0.1f, 1.1f, 0.0f);
			pineLevel(out, random, 0.52f, 0.172f, 0.7f, 0.3f, 0.1f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.172f, 0.5f, 0.7f, 0.2f, 0.18f, 8.0f);
			pineLevel(out, random, 0.52f, 0.172f, 0.3f, 1.1f, 0.5f, 0.18f, 8.0f);
			break;

		// Palms: a tapering trunk and a crown of fronds.
		case TreeModelFactory::ePalmNormal:
			palmTrunk(out, 0.07f, 0.7f, 5.0f);
			palmLevel(out, random, 0.0f, 0.6f, 0.6f, 0.8f, 7.0f, 0.0f, 0.365f);
			break;
		case TreeModelFactory::ePalm2:
			palmTrunk(out, 0.07f, 0.7f, 5.0f);
			palmLevel(out, random, 0.0f, 0.6f, 0.6f, 0.8f, 7.0f, 0.39f, 0.365f);
			break;
		case TreeModelFactory::ePalm3:
			palmTrunk(out, 0.07f, 0.7f, 5.0f);
			palmLevel(out, random, 0.0f, 0.6f, 0.6f, 0.8f, 7.0f, 0.0f, 0.25f);
			break;
		case TreeModelFactory::ePalm4:
			palmTrunk(out, 0.07f, 0.7f, 5.0f);
			palmLevel(out, random, 0.0f, 0.6f, 0.6f, 0.8f, 7.0f, 0.39f, 0.25f);
			break;
		case TreeModelFactory::ePalmBurnt:
			// Upstream draws the burnt palm as a bare trunk - no fronds.
			palmTrunk(out, 0.07f, 0.7f, 5.0f);
			break;

		// The "palm B" set is really a broadleaf: a short trunk with two
		// wide double-sided skirts.
		case TreeModelFactory::ePalmB:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.164f, 0.836f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.164f, 0.836f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB2:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.5f, 0.836f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.5f, 0.836f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB3:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.836f, 0.836f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.836f, 0.836f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB4:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.164f, 0.5f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.164f, 0.5f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB5:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.5f, 0.5f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.5f, 0.5f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB6:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.164f, 0.164f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.164f, 0.164f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;
		case TreeModelFactory::ePalmB7:
			palmTrunk(out, 0.07f, 0.5f, 5.0f, 0.664f, 0.0f, 0.172f, 0.656f);
			pineLevel(out, random, 0.164f, 0.5f, 0.5f, 0.4f, 0.3f, 0.164f, 8.0f, true, 15.0f);
			pineLevel(out, random, 0.164f, 0.5f, 0.4f, 0.5f, 0.4f, 0.164f, 8.0f, false);
			break;

		// Oaks: a thin tall trunk under three broad skirts.
		case TreeModelFactory::eOak:
			pineTrunk(out, 0.02f, 0.75f, 0.0f, 0.836f, 0.0f, 0.164f, 0.164f, 8.0f);
			pineLevel(out, random, 0.5f, 0.836f, 0.4f, 0.5f, 0.3f, 0.164f, 8.0f, false, 15.0f);
			pineLevel(out, random, 0.164f, 0.836f, 0.3f, 0.6f, 0.5f, 0.164f, 8.0f, false);
			pineLevel(out, random, 0.5f, 0.836f, 0.2f, 0.8f, 0.6f, 0.164f, 8.0f, true, 15.0f);
			break;
		case TreeModelFactory::eOak2:
			pineTrunk(out, 0.02f, 0.75f, 0.0f, 0.836f, 0.0f, 0.164f, 0.164f, 8.0f);
			pineLevel(out, random, 0.164f, 0.5f, 0.4f, 0.5f, 0.3f, 0.164f, 8.0f, false, 15.0f);
			pineLevel(out, random, 0.836f, 0.836f, 0.3f, 0.6f, 0.5f, 0.164f, 8.0f, false);
			pineLevel(out, random, 0.164f, 0.5f, 0.2f, 0.8f, 0.6f, 0.164f, 8.0f, true, 15.0f);
			break;
		case TreeModelFactory::eOak3:
			pineTrunk(out, 0.02f, 0.75f, 0.0f, 0.836f, 0.0f, 0.164f, 0.164f, 8.0f);
			pineLevel(out, random, 0.836f, 0.5f, 0.4f, 0.5f, 0.3f, 0.164f, 8.0f, false, 15.0f);
			pineLevel(out, random, 0.5f, 0.5f, 0.3f, 0.6f, 0.5f, 0.164f, 8.0f, false);
			pineLevel(out, random, 0.836f, 0.5f, 0.2f, 0.8f, 0.6f, 0.164f, 8.0f, true, 15.0f);
			break;
		case TreeModelFactory::eOak4:
			pineTrunk(out, 0.02f, 0.75f, 0.0f, 0.836f, 0.0f, 0.164f, 0.164f, 8.0f);
			pineLevel(out, random, 0.5f, 0.164f, 0.4f, 0.5f, 0.3f, 0.164f, 8.0f, false, 15.0f);
			pineLevel(out, random, 0.164f, 0.164f, 0.3f, 0.6f, 0.5f, 0.164f, 8.0f, false);
			pineLevel(out, random, 0.5f, 0.164f, 0.2f, 0.8f, 0.6f, 0.164f, 8.0f, true, 15.0f);
			break;

		default:
			break;
		}

		return (int) ((out.size() - startFloats) / kFloatsPerVertex);
	}
}
