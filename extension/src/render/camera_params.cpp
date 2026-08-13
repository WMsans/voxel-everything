#include "render/camera_params.h"
#include <cmath>

namespace ve {

CameraParams CameraParams::looking_at(float ox, float oy, float oz,
		float fx, float fy, float fz, float ux, float uy, float uz) {
	CameraParams cp{};
	cp.cam_pos[0] = ox; cp.cam_pos[1] = oy; cp.cam_pos[2] = oz;

	float fl = sqrtf(fx * fx + fy * fy + fz * fz);
	fx /= fl; fy /= fl; fz /= fl;
	// right = normalize(fwd x up_hint); if degenerate, pick another hint
	float rx = fy * uz - fz * uy, ry = fz * ux - fx * uz, rz = fx * uy - fy * ux;
	float rl = sqrtf(rx * rx + ry * ry + rz * rz);
	if (rl < 1e-5f) {
		ux = 1; uy = 0; uz = 0;
		rx = fy * uz - fz * uy; ry = fz * ux - fx * uz; rz = fx * uy - fy * ux;
		rl = sqrtf(rx * rx + ry * ry + rz * rz);
	}
	rx /= rl; ry /= rl; rz /= rl;
	// up = right x fwd
	const float upx = ry * fz - rz * fy, upy = rz * fx - rx * fz, upz = rx * fy - ry * fx;

	cp.cam_right[0] = rx; cp.cam_right[1] = ry; cp.cam_right[2] = rz;
	cp.cam_up[0] = upx; cp.cam_up[1] = upy; cp.cam_up[2] = upz;
	cp.cam_fwd[0] = fx; cp.cam_fwd[1] = fy; cp.cam_fwd[2] = fz;
	cp.params[2] = 200.0f;
	return cp;
}

} // namespace ve
