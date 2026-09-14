#include <doctest/doctest.h>
#include "render/frame_params.h"
#include <cmath>
#include <cstring>

namespace {
void project(const ve::LodCamera &c, const float p[3], float ndc[3]) {
	float clip[4] = {};
	for (int r = 0; r < 4; r++)
		clip[r] = c.view_proj[0 * 4 + r] * p[0] + c.view_proj[1 * 4 + r] * p[1] +
				c.view_proj[2 * 4 + r] * p[2] + c.view_proj[3 * 4 + r];
	for (int i = 0; i < 3; i++) ndc[i] = clip[i] / clip[3];
}
} // namespace

// The rule every frame-rebuilding probe used: world up, unless the view is near-vertical.
TEST_CASE("probe up hint is world up except for near-vertical views") {
	float up[3];
	const float oblique[3] = {0.5f, -0.5f, 0.5f};
	ve::probe_up_hint(oblique, up);
	CHECK(up[0] == 0.0f); CHECK(up[1] == 1.0f); CHECK(up[2] == 0.0f);
	const float down[3] = {0.0f, -1.0f, 0.0f};
	ve::probe_up_hint(down, up);
	CHECK(up[0] == 0.0f); CHECK(up[1] == 0.0f); CHECK(up[2] == 1.0f);
}

TEST_CASE("probe camera basis is CameraParams::looking_at's basis") {
	const float pos[3] = {30.0f, 70.0f, 30.0f};
	const float fwd[3] = {0.57735f, -0.57735f, 0.57735f};
	const ve::ProbeCamera pc = ve::probe_camera(pos, fwd, 128, 64, 1.0471975512f, 0.05f, 4000.0f);
	const ve::CameraParams cp = ve::CameraParams::looking_at(pos[0], pos[1], pos[2],
			fwd[0], fwd[1], fwd[2], 0.0f, 1.0f, 0.0f);
	for (int i = 0; i < 3; i++) {
		CHECK(pc.right[i] == doctest::Approx(cp.cam_right[i]));
		CHECK(pc.up[i] == doctest::Approx(cp.cam_up[i]));
		CHECK(pc.fwd[i] == doctest::Approx(cp.cam_fwd[i]));
	}
	CHECK(pc.tan_y == doctest::Approx(std::tan(1.0471975512f * 0.5f)));
	CHECK(pc.tan_x == doctest::Approx(pc.tan_y * 2.0f));
	CHECK(pc.lod.viewport[0] == 128);
	CHECK(pc.lod.viewport[1] == 64);
}

// The raymarcher derives each pixel's ray from (right, up, tan_x, tan_y); the rasters use
// view_proj. They must agree about where a world point lands, or the near and far fields
// disagree on the pixel grid.
TEST_CASE("probe camera tangents and view_proj agree on the pixel grid") {
	const float pos[3] = {30.0f, 70.0f, 30.0f};
	const float fwd[3] = {0.57735f, -0.57735f, 0.57735f};
	const ve::ProbeCamera pc = ve::probe_camera(pos, fwd, 128, 64, 1.0471975512f, 0.05f, 4000.0f);
	const float d = 20.0f;
	float centre[3], ndc[3];
	for (int i = 0; i < 3; i++) centre[i] = pos[i] + pc.fwd[i] * d;
	project(pc.lod, centre, ndc);
	CHECK(ndc[0] == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(ndc[1] == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(ndc[2] > 0.0f);
	CHECK(ndc[2] < 1.0f);
	float side[3];
	for (int i = 0; i < 3; i++) side[i] = centre[i] + pc.right[i] * (d * pc.tan_x * 0.5f);
	project(pc.lod, side, ndc);
	CHECK(std::fabs(ndc[0]) == doctest::Approx(0.5f).epsilon(1e-3));
	float above[3];
	for (int i = 0; i < 3; i++) above[i] = centre[i] + pc.up[i] * (d * pc.tan_y * 0.5f);
	project(pc.lod, above, ndc);
	CHECK(std::fabs(ndc[1]) == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("probe camera depth is reverse-Z") {
	const float pos[3] = {0.0f, 10.0f, 0.0f};
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const ve::ProbeCamera pc = ve::probe_camera(pos, fwd, 64, 64, 1.0471975512f, 0.05f, 4000.0f);
	const float near_p[3] = {0.0f, 10.0f, -5.0f};
	const float far_p[3] = {0.0f, 10.0f, -500.0f};
	float a[3], b[3];
	project(pc.lod, near_p, a);
	project(pc.lod, far_p, b);
	CHECK(a[2] > b[2]);
}

TEST_CASE("set_near_field_world fills the world fields and nothing else") {
	ve::CameraParams cp = ve::CameraParams::looking_at(0, 0, 0, 0, 0, -1, 0, 1, 0);
	cp.params[0] = 0.25f; cp.params[1] = 0.5f; cp.params[2] = 123.0f; cp.params[3] = -1.0f;
	cp.cam_pos[3] = 7.0f;
	cp.region_origin[3] = 11;
	cp.atlas_bricks[3] = 13;
	ve::RegionWindow win;
	win.origin = {-3, 1, 4};
	win.dim = 32;
	ve::set_near_field_world(&cp, win, 5, {64, 16, 48});
	CHECK(cp.dims[0] == 32); CHECK(cp.dims[1] == 32); CHECK(cp.dims[2] == 32);
	CHECK(cp.dims[3] == 5);
	CHECK(cp.region_origin[0] == -3); CHECK(cp.region_origin[1] == 1); CHECK(cp.region_origin[2] == 4);
	CHECK(cp.atlas_bricks[0] == 64); CHECK(cp.atlas_bricks[1] == 16); CHECK(cp.atlas_bricks[2] == 48);
	CHECK(cp.params[0] == 0.25f); CHECK(cp.params[1] == 0.5f);
	CHECK(cp.params[2] == 123.0f); CHECK(cp.params[3] == -1.0f);
	CHECK(cp.cam_pos[3] == 7.0f);
	CHECK(cp.region_origin[3] == 11);
	CHECK(cp.atlas_bricks[3] == 13);
}

TEST_CASE("set_near_field_flags stores the bit pattern in cam_pos.w") {
	ve::CameraParams cp = ve::CameraParams::looking_at(0, 0, 0, 0, 0, -1, 0, 1, 0);
	const uint32_t flags = 0x8000'0105u;
	ve::set_near_field_flags(&cp, flags);
	uint32_t back = 0;
	std::memcpy(&back, &cp.cam_pos[3], sizeof(float));
	CHECK(back == flags);
}
