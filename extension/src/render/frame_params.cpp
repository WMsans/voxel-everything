#include "render/frame_params.h"
#include <cmath>
#include <cstring>

namespace ve {

void probe_up_hint(const float fwd[3], float out_up[3]) {
	const bool vertical = std::fabs(fwd[1]) > 0.9f;
	out_up[0] = 0.0f;
	out_up[1] = vertical ? 0.0f : 1.0f;
	out_up[2] = vertical ? 1.0f : 0.0f;
}

ProbeCamera probe_camera(const float pos[3], const float fwd[3], int w, int h,
		float fov_y_rad, float z_near, float z_far) {
	ProbeCamera out;
	float up[3];
	probe_up_hint(fwd, up);
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	out.lod = lod_camera_perspective(pos, fwd, up, fov_y_rad, aspect, z_near, z_far, w, h);
	const CameraParams basis = CameraParams::looking_at(pos[0], pos[1], pos[2],
			fwd[0], fwd[1], fwd[2], up[0], up[1], up[2]);
	for (int i = 0; i < 3; i++) {
		out.right[i] = basis.cam_right[i];
		out.up[i] = basis.cam_up[i];
		out.fwd[i] = basis.cam_fwd[i];
	}
	out.tan_y = std::tan(fov_y_rad * 0.5f);
	out.tan_x = out.tan_y * aspect;
	return out;
}

void set_near_field_world(CameraParams *cp, const RegionWindow &win, int island_slots,
		IVec3 atlas_bricks) {
	cp->dims[0] = win.dim;
	cp->dims[1] = win.dim;
	cp->dims[2] = win.dim;
	cp->dims[3] = island_slots;
	cp->region_origin[0] = win.origin.x;
	cp->region_origin[1] = win.origin.y;
	cp->region_origin[2] = win.origin.z;
	cp->atlas_bricks[0] = atlas_bricks.x;
	cp->atlas_bricks[1] = atlas_bricks.y;
	cp->atlas_bricks[2] = atlas_bricks.z;
}

void set_near_field_flags(CameraParams *cp, uint32_t beauty_flags) {
	std::memcpy(&cp->cam_pos[3], &beauty_flags, sizeof(float));
}

} // namespace ve
