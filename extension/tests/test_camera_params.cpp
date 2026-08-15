#include <doctest/doctest.h>
#include "render/camera_params.h"

TEST_CASE("looking_at builds an orthonormal basis aligned to forward") {
	auto cp = ve::CameraParams::looking_at(1, 2, 3, 0, 0, -1, 0, 1, 0);
	CHECK(cp.cam_pos[0] == 1);
	CHECK(cp.cam_pos[2] == 3);
	CHECK(cp.cam_fwd[2] == doctest::Approx(-1.0));
	auto dot = [](const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
	CHECK(dot(cp.cam_right, cp.cam_right) == doctest::Approx(1.0));
	CHECK(dot(cp.cam_up, cp.cam_up) == doctest::Approx(1.0));
	CHECK(dot(cp.cam_right, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_up, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_right, cp.cam_up) == doctest::Approx(0.0));
}

TEST_CASE("looking_at handles degenerate up hint (parallel to forward)") {
	auto cp = ve::CameraParams::looking_at(0, 0, 0, 0, 1, 0, 0, 1, 0);
	auto dot = [](const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
	CHECK(dot(cp.cam_right, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_up, cp.cam_up) == doctest::Approx(1.0));
}

TEST_CASE("M2 layout: 128 bytes with region_origin and atlas_bricks") {
	ve::CameraParams cp = ve::CameraParams::looking_at(0, 0, 0, 0, 0, -1, 0, 1, 0);
	cp.dims[0] = 64; cp.dims[1] = 8; cp.dims[2] = 64;      // world size in REGIONS
	cp.region_origin[1] = -2;                              // origin_bricks (0,-64,0) / 32
	cp.atlas_bricks[0] = 64; cp.atlas_bricks[2] = 32;
	CHECK(sizeof(cp) == 128);
	CHECK(cp.dims[1] == 8);
	CHECK(cp.region_origin[1] == -2);
	CHECK(cp.atlas_bricks[2] == 32);
}
