#include "generator/generator.h"

namespace ve {

FieldSample Generator::sample_gradient(float x, float y, float z) const {
	Sample s = sample_ground(x, y, z);
	const float e = 0.01f;
	const float dx = (sample_ground(x + e, y, z).sdf - sample_ground(x - e, y, z).sdf) / (2.0f * e);
	const float dy = (sample_ground(x, y + e, z).sdf - sample_ground(x, y - e, z).sdf) / (2.0f * e);
	const float dz = (sample_ground(x, y, z + e).sdf - sample_ground(x, y, z - e).sdf) / (2.0f * e);
	FieldSample fs{};
	fs.sdf = s.sdf;
	fs.material = s.material;
	fs.gradient[0] = dx;
	fs.gradient[1] = dy;
	fs.gradient[2] = dz;
	fs.exact_gradient = false;
	return fs;
}

} // namespace ve
