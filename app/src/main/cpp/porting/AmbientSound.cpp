#include <AmbientSound.h>

#include <XML/XMLFile.hpp>
#include <common/DefinesScorched.hpp>
#include <engine/ScorchedContext.hpp>
#include <landscapedef/LandscapeDefinitionCache.hpp>
#include <landscapemap/LandscapeMaps.hpp>

namespace
{
	// One <sound> block, as upstream writes them:
	//
	//   <sound>
	//     <position type='water'><falloff>1.0</falloff></position>
	//     <timing type='looped'></timing>
	//     <sound type='file'><file>data/wav/misc/oceanwaves.wav</file>
	//                        <gain>1.0</gain></sound>
	//   </sound>
	//
	// The inner <sound> shares its tag with the outer one, which is why this
	// walks the nodes rather than searching by name.
	void readSounds(const std::string &path, std::vector<ScorchDroidAmbient::Sound> &out)
	{
		XMLFile file;
		if (!file.readFile(path)) return;
		if (!file.getRootNode()) return;

		XMLNode *soundNode = nullptr;
		while (file.getRootNode()->getNamedChild("sound", soundNode, false))
		{
			ScorchDroidAmbient::Sound sound;

			XMLNode *timingNode = nullptr;
			if (soundNode->getNamedChild("timing", timingNode, false))
			{
				std::string type;
				if (timingNode->getNamedParameter("type", type, false))
				{
					sound.looped = (type == "looped");
				}
				if (!sound.looped)
				{
					timingNode->getNamedChild("min", sound.minSeconds, false);
					timingNode->getNamedChild("max", sound.maxSeconds, false);
				}
			}

			XMLNode *innerNode = nullptr;
			if (soundNode->getNamedChild("sound", innerNode, false))
			{
				std::string file;
				// Upstream allows several files and picks one at random per
				// play; nothing it ships uses more than one, so the first is
				// taken and the rest ignored rather than played together.
				if (innerNode->getNamedChild("file", file, false))
				{
					sound.file = S3D::getModFile(file);
				}
				innerNode->getNamedChild("gain", sound.gain, false);
			}

			if (!sound.file.empty()) out.push_back(sound);
		}
	}
}

namespace ScorchDroidAmbient
{
	std::vector<Sound> forCurrentLandscape(ScorchedContext &context)
	{
		// The definition names its texture file - a path, already, which is
		// why this can be read straight off disk without inventing one.
		const char *texFile =
			context.getLandscapeMaps().getDefinitions().getDefinition().getTex();
		if (!texFile || !texFile[0]) return std::vector<Sound>();
		return forTexFile(S3D::getModFile(texFile));
	}

	std::vector<Sound> forTexFile(const std::string &texPath)
	{
		std::vector<Sound> found;
		XMLFile file;
		if (!file.readFile(texPath)) return found;
		if (!file.getRootNode()) return found;

		XMLNode *soundsNode = nullptr;
		if (!file.getRootNode()->getNamedChild("ambientsounds", soundsNode, false))
		{
			return found;
		}

		std::string include;
		while (soundsNode->getNamedChild("ambientsound", include, false))
		{
			readSounds(S3D::getModFile(include), found);
		}
		return found;
	}
}
