#include <doctest/doctest.h>
#include "grass/grass_layout.h"
#include "leaves/leaf_layout.h"
#include "render/scatter_reuse.h"

namespace {

template <class P>
P sample_params() {
	P p{};
	p.cam[0] = 8.0f; p.cam[1] = 62.7f; p.cam[2] = 8.0f; p.cam[3] = 120.0f;
	for (int i = 0; i < 6; i++) {
		p.planes[i][0] = 0.6f; p.planes[i][1] = 0.0f; p.planes[i][2] = -0.8f;
		p.planes[i][3] = 412.5f;
	}
	p.wind[0] = 0.35f; p.wind[1] = 0.6f; p.wind[2] = 0.04f; p.wind[3] = 12.0f;
	p.limits[0] = 600000;
	return p;
}

} // namespace

TEST_CASE_TEMPLATE("scatter reuse ignores time: wind is applied by the raster", P,
		ve::GrassParams, ve::LeafParams) {
	const P a = sample_params<P>();
	P b = a;
	b.wind[3] = 99.0f;
	CHECK(ve::same_scatter_inputs(a, b));
}

TEST_CASE_TEMPLATE("scatter reuse tolerates float noise in a still camera", P,
		ve::GrassParams, ve::LeafParams) {
	const P a = sample_params<P>();
	P b = a;
	b.cam[1] += 1.0e-5f;      // CharacterBody settling, far below a blade
	b.planes[2][3] += 1.0e-4f; // 412.5 m plane distance, relative 2.4e-7
	CHECK(ve::same_scatter_inputs(a, b));
}

TEST_CASE_TEMPLATE("scatter reuse rescatters when the camera actually moves or turns", P,
		ve::GrassParams, ve::LeafParams) {
	const P a = sample_params<P>();
	P moved = a;
	moved.cam[0] += 0.01f;
	CHECK_FALSE(ve::same_scatter_inputs(a, moved));
	P turned = a;
	turned.planes[0][0] += 1.0e-3f;
	CHECK_FALSE(ve::same_scatter_inputs(a, turned));
}

TEST_CASE_TEMPLATE("scatter reuse compares everything past the frustum exactly", P,
		ve::GrassParams, ve::LeafParams) {
	const P a = sample_params<P>();
	P b = a;
	b.limits[0] += 1; // an int one apart is not "close"
	CHECK_FALSE(ve::same_scatter_inputs(a, b));
	P c = a;
	c.wind[0] += 1.0e-6f; // a settings float has no tolerance: the user moved a slider
	CHECK_FALSE(ve::same_scatter_inputs(a, c));
}
