#include <ParticleTextures.h>

#include <XML/XMLFile.hpp>
#include <XML/XMLNode.hpp>
#include <image/ImageFactory.hpp>
#include <image/Image.hpp>
#include <common/Defines.hpp>

#include <cmath>
#include <cstring>

namespace
{
	using namespace ScorchDroidParticleTextures;

	// Nearest resample of one image into a 128-square RGBA layer. Sizes
	// upstream ships: explosions 128, smoke and ring 64, the particle 32.
	void appendLayer(Atlas &atlas, Image &image)
	{
		const size_t start = atlas.rgba.size();
		atlas.rgba.resize(start + (size_t) kSize * kSize * 4, 0);
		unsigned char *dst = &atlas.rgba[start];
		const int comps = image.getComponents();
		for (int y = 0; y < kSize; y++) {
			const int sy = std::min(image.getHeight() - 1, y * image.getHeight() / kSize);
			for (int x = 0; x < kSize; x++) {
				const int sx = std::min(image.getWidth() - 1, x * image.getWidth() / kSize);
				const unsigned char *src = image.getBitsPos(sx, sy);
				unsigned char *out = dst + ((size_t) y * kSize + x) * 4;
				if (comps >= 3) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; }
				else { out[0] = out[1] = out[2] = src[0]; }
				// The image as its own alpha, as upstream's loader combines it:
				// the average of the three channels.
				out[3] = (comps >= 4) ? src[3]
					: (comps >= 3) ? (unsigned char) (((int) src[0] + src[1] + src[2]) / 3)
					: src[0];
			}
		}
		atlas.layers++;
	}

	bool appendFile(Atlas &atlas, const std::string &file)
	{
		Image image = ImageFactory::loadImage(S3D::eModLocation, file, file, false);
		if (!image.getBits() || image.getWidth() <= 0 || image.getHeight() <= 0) return false;
		appendLayer(atlas, image);
		return true;
	}
}

namespace ScorchDroidParticleTextures
{
	const Set *Atlas::find(const std::string &name) const
	{
		for (size_t i = 0; i < sets.size(); i++) {
			if (sets[i].name == name) return &sets[i];
		}
		return nullptr;
	}

	Atlas load()
	{
		Atlas atlas;

		// Layer 0: the soft disc, alpha 1 - r^2, white.
		atlas.rgba.resize((size_t) kSize * kSize * 4, 0);
		for (int y = 0; y < kSize; y++) {
			for (int x = 0; x < kSize; x++) {
				const float dx = ((float) x + 0.5f) / kSize * 2.0f - 1.0f;
				const float dy = ((float) y + 0.5f) / kSize * 2.0f - 1.0f;
				const float r2 = dx * dx + dy * dy;
				const float a = r2 >= 1.0f ? 0.0f : (1.0f - r2);
				unsigned char *out = &atlas.rgba[((size_t) y * kSize + x) * 4];
				out[0] = out[1] = out[2] = 255;
				out[3] = (unsigned char) (a * 255.0f);
			}
		}
		atlas.layers = 1;
		Set disc; disc.name = "disc"; disc.firstLayer = 0; disc.count = 1;
		atlas.sets.push_back(disc);

		XMLFile file;
		if (file.readFile(S3D::getModFile("data/textureset.xml")) && file.getRootNode()) {
			std::list<XMLNode *> &children = file.getRootNode()->getChildren();
			for (std::list<XMLNode *>::iterator it = children.begin(); it != children.end(); ++it) {
				XMLNode *node = *it;
				if (strcmp(node->getName(), "textureset") != 0) continue;
				XMLNode *nameNode = nullptr;
				if (!node->getNamedParameter("name", nameNode, false)) continue;
				Set set;
				set.name = nameNode->getContent();
				set.firstLayer = atlas.layers;
				std::list<XMLNode *> &textures = node->getChildren();
				for (std::list<XMLNode *>::iterator t = textures.begin(); t != textures.end(); ++t) {
					if (strcmp((*t)->getName(), "texture") != 0) continue;
					if (appendFile(atlas, std::string("data/") + (*t)->getContent())) set.count++;
				}
				if (set.count > 0) atlas.sets.push_back(set);
			}
		}

		Set talk; talk.name = "talk"; talk.firstLayer = atlas.layers;
		if (appendFile(atlas, "data/textures/talk.bmp")) { talk.count = 1; atlas.sets.push_back(talk); }

		return atlas;
	}
}
