#include "InstanceBuffer.hpp"

namespace ScorchDroidInstances
{
	void pack(const std::vector<Instance> &instances, std::vector<float> &out)
	{
		out.reserve(out.size() + instances.size() * kFloatsPerInstance);
		for (std::vector<Instance>::const_iterator itor = instances.begin();
			 itor != instances.end(); ++itor)
		{
			// Attribute 2: (x, y, z, scale)
			out.push_back(itor->x);
			out.push_back(itor->y);
			out.push_back(itor->z);
			out.push_back(itor->scale);
			// Attribute 3: (rotation, r, g, b)
			out.push_back(itor->rotationRadians);
			out.push_back(itor->r);
			out.push_back(itor->g);
			out.push_back(itor->b);
		}
	}
}
