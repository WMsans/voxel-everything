#include <doctest/doctest.h>
#include "leaves/leaf_settings.h"
#include "leaves/leaf_settings_store.h"
#include <cmath>
#include <limits>

TEST_CASE("leaf settings clamp every knob, NaN included") {
	ve::LeafSettings s;
	s.reach_m = 1.0e9f;
	s.clumps_per_tree = 100000;
	s.clump_radius_m = -4.0f;
	s.canopy_roundness = std::nanf("");
	s.wind_strength = -1.0f;
	ve::clamp_leaf_settings(&s);
	CHECK(s.reach_m <= 1000.0f);
	CHECK(s.clumps_per_tree <= 128);
	CHECK(s.clump_radius_m >= 0.0f);
	CHECK(s.canopy_roundness >= 0.0f);
	CHECK(s.canopy_roundness <= 1.0f);
	CHECK(s.wind_strength >= 0.0f);
}

// Task 8 parked: the per-knob spot-checks above must become a sweep. One row per
// leaf_rows() entry: below-min and above-max each clamp to the DECLARED bound exactly
// (the bounds come from the table, so a row added later is covered without editing this
// test), and the float rows' NaN/+-inf edges pin clamp_setting's documented shape.
TEST_CASE("every leaf settings row clamps exactly to its declared bounds") {
	auto set_out_of_range = [](const ve::SettingRow<ve::LeafSettings> &r, ve::LeafSettings &s,
			bool low) {
		if (r.kind == ve::SettingKind::kFloat) {
			s.*r.f = low ? r.min - 1.0f : r.max + 1.0f;
		} else if (r.kind == ve::SettingKind::kInt || r.kind == ve::SettingKind::kEnum) {
			s.*r.i = low ? static_cast<int>(r.min) - 100 : static_cast<int>(r.max) + 100;
		} else {
			FAIL("leaf settings grew a row kind this sweep does not cover");
		}
	};
	for (const ve::SettingRow<ve::LeafSettings> &r : ve::leaf_rows()) {
		INFO("row " << r.name);
		if (r.kind == ve::SettingKind::kBool) {
			// A bool has no range; its round-trip through clamp_all must preserve both
			// states (the table's kBool path is `!= 0`, so 0/1 map to themselves).
			ve::LeafSettings s;
			s.*r.b = false;
			ve::clamp_leaf_settings(&s);
			CHECK_FALSE(s.*r.b);
			s.*r.b = true;
			ve::clamp_leaf_settings(&s);
			CHECK(s.*r.b);
			continue;
		}
		{
			ve::LeafSettings s;
			set_out_of_range(r, s, true);
			ve::clamp_leaf_settings(&s);
			if (r.kind == ve::SettingKind::kFloat) CHECK(s.*r.f == r.min);
			else CHECK(s.*r.i == static_cast<int>(r.min));
		}
		{
			ve::LeafSettings s;
			set_out_of_range(r, s, false);
			ve::clamp_leaf_settings(&s);
			if (r.kind == ve::SettingKind::kFloat) CHECK(s.*r.f == r.max);
			else CHECK(s.*r.i == static_cast<int>(r.max));
		}
		if (r.kind == ve::SettingKind::kFloat) {
			// clamp_setting's documented edges: NaN floors to lo, +-inf clamp to the
			// nearer bound. Rows outside floats can't reach it (ints round after clamping
			// and a bool has no range).
			ve::LeafSettings s;
			s.*r.f = std::nanf("");
			ve::clamp_leaf_settings(&s);
			CHECK(s.*r.f == r.min);
			s.*r.f = -std::numeric_limits<float>::infinity();
			ve::clamp_leaf_settings(&s);
			CHECK(s.*r.f == r.min);
			s.*r.f = std::numeric_limits<float>::infinity();
			ve::clamp_leaf_settings(&s);
			CHECK(s.*r.f == r.max);
		}
	}
}

// The old form (clamp a default store, clamp again, compare) was vacuous: defaults are
// already inside every bound, so both applies trivially agreed. This fails if the SECOND
// apply moves anything observable AND if the FIRST apply never took: every non-bool row
// starts deliberately out of range, and after the first clamp must read exactly its
// declared max, not the planted value.
TEST_CASE("clamping is idempotent, and the first apply took") {
	ve::LeafSettings a;
	for (const ve::SettingRow<ve::LeafSettings> &r : ve::leaf_rows()) {
		INFO("row " << r.name);
		if (r.kind == ve::SettingKind::kBool) {
			a.*r.b = true; // observable state for the second-apply comparison
		} else if (r.kind == ve::SettingKind::kFloat) {
			a.*r.f = r.max + 1.0f;
		} else {
			a.*r.i = static_cast<int>(r.max) + 100;
		}
	}
	ve::clamp_leaf_settings(&a);
	for (const ve::SettingRow<ve::LeafSettings> &r : ve::leaf_rows()) {
		INFO("first apply did not take on row " << r.name);
		if (r.kind == ve::SettingKind::kFloat) CHECK(a.*r.f == r.max);
		else if (r.kind == ve::SettingKind::kInt || r.kind == ve::SettingKind::kEnum)
			CHECK(a.*r.i == static_cast<int>(r.max));
	}
	ve::LeafSettings b = a;
	ve::clamp_leaf_settings(&b);
	for (const ve::SettingRow<ve::LeafSettings> &r : ve::leaf_rows()) {
		INFO("second apply moved row " << r.name);
		const ve::SettingValue va = ve::read(r, a);
		const ve::SettingValue vb = ve::read(r, b);
		CHECK(static_cast<int>(va.kind) == static_cast<int>(vb.kind));
		for (int k = 0; k < 3; k++) CHECK(va.v[k] == vb.v[k]);
	}
}

TEST_CASE("clumps_per_tree is capped at the scatter workgroup width") {
	// shaders/leaf_scatter.comp.glsl runs one workgroup per tree with local_size_x = 128,
	// one thread per candidate clump, so a tree must never need a second group.
	ve::LeafSettings s;
	s.clumps_per_tree = 1000;
	ve::clamp_leaf_settings(&s);
	CHECK(s.clumps_per_tree <= 128);
}

TEST_CASE("the store round-trips a knob by name and clamps on write") {
	ve::LeafSettingsStore store;
	CHECK(store.set_value("reach_m", 180.0f));
	CHECK(store.get().reach_m == doctest::Approx(180.0f));
	CHECK(store.set_value("reach_m", 1.0e9f));
	CHECK(store.get().reach_m <= 1000.0f);
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}

#include "leaves/leaf_layout.h"

namespace {
// A finite, well-conditioned view-projection: identity is enough for the layout arithmetic,
// which only needs the frustum planes to be extractable, not meaningful.
const float kIdentity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};
} // namespace

TEST_CASE("the params block is exactly 256 bytes") {
	// std140 padding cannot disagree with the C++ struct if the struct is 16 vec4.
	CHECK(sizeof(ve::LeafParams) == 256);
}

TEST_CASE("cam position is the first three floats of the params block") {
	ve::LeafSettings s;
	const float cam[3] = {12.0f, 34.0f, -56.0f};
	const ve::LeafLayout l = ve::leaf_layout(s, cam, kIdentity);
	CHECK(l.params.cam[0] == doctest::Approx(12.0f));
	CHECK(l.params.cam[1] == doctest::Approx(34.0f));
	CHECK(l.params.cam[2] == doctest::Approx(-56.0f));
}

TEST_CASE("the clump budget falls monotonically with distance") {
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	s.clumps_per_tree = 96;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	int prev = ve::leaf_clump_budget(l, 0.0f);
	CHECK(prev == 96);
	for (float d = 5.0f; d <= 250.0f; d += 5.0f) {
		const int n = ve::leaf_clump_budget(l, d);
		CHECK(n <= prev);
		CHECK(n >= 1); // a tree inside the reach always keeps SOME canopy
		prev = n;
	}
	CHECK(ve::leaf_clump_budget(l, 260.0f) == 0); // past the reach, nothing
}

TEST_CASE("fewer clumps are exactly compensated by larger ones") {
	// The whole point of the density LOD: total card area per tree is flat in distance, so a
	// distant crown keeps its silhouette instead of thinning into scattered dots.
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	s.clumps_per_tree = 96;
	s.clump_radius_m = 0.85f;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	const float near_area = 96.0f * 0.85f * 0.85f;
	for (float d = 0.0f; d < 250.0f; d += 10.0f) {
		const float r = ve::leaf_clump_radius(l, d);
		const float area = float(ve::leaf_clump_budget(l, d)) * r * r;
		CHECK(area == doctest::Approx(near_area).epsilon(0.12f));
	}
}

TEST_CASE("the cell box covers the reach and capacity bounds the dispatch") {
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	const float cam[3] = {1000.0f, 60.0f, -2000.0f};
	const ve::LeafLayout l = ve::leaf_layout(s, cam, kIdentity);
	// Every cell whose centre is within the reach must be inside the box.
	const float cell = l.cell_size_m;
	CHECK(float(l.cell_min.x) * cell <= cam[0] - s.reach_m);
	CHECK(float(l.cell_min.z) * cell <= cam[2] - s.reach_m);
	CHECK(float(l.cell_min.x + l.cell_dim.x) * cell >= cam[0] + s.reach_m);
	CHECK(float(l.cell_min.z + l.cell_dim.z) * cell >= cam[2] + s.reach_m);
	CHECK(l.dispatch_threads == l.cell_dim.x * l.cell_dim.z);
	CHECK(l.max_trees <= s.max_trees);
	CHECK(l.max_clumps <= s.max_clumps);
}

TEST_CASE("a disabled or zero-reach layout dispatches nothing") {
	ve::LeafSettings s;
	s.enabled = false;
	CHECK(ve::leaf_layout(s, kOrigin, kIdentity).dispatch_threads == 0);
	s.enabled = true;
	s.reach_m = 0.0f;
	CHECK(ve::leaf_layout(s, kOrigin, kIdentity).dispatch_threads == 0);
}

TEST_CASE("an unclamped snapshot is clamped internally") {
	ve::LeafSettings s;
	s.reach_m = 1.0e9f;
	s.clumps_per_tree = 100000;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	CHECK(l.reach_m <= 1000.0f);
	CHECK(ve::leaf_clump_budget(l, 0.0f) <= 128);
}

#include "terrain/pipeline_load.h"
#include "generator/generator.h"
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Same ifstream-over-repo reader the pipeline tests use; VE_REPO_ROOT points at the repo
// root (SConstruct) and ve_tests runs from extension/.
bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}

} // namespace

TEST_CASE("the layout's tree-shape params equal the shipped pipeline's tree stage") {
	// leaf_layout.cpp hardcodes the trees stage's params (there is no live UBO read there).
	// This is the pin: load the shipped pipeline through the engine's OWN resolver, so a
	// stage default edited in shaders/stages/trees.field.glslh -- or a future pipeline-level
	// override -- fails this test until leaf_layout.cpp moves with it. Exact float equality:
	// both sides arrive at a float from the same decimal text, so any literal change is
	// drift, and a tolerance would hide the edit that regrounds the canopies.
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/trees.pipeline",
			root + "/shaders/", &p, nullptr, &err), err);
	auto param = [&p](const char *name) {
		for (const ve::ParamDecl &d : p.params)
			if (d.name == name) return d.value;
		INFO("the shipped trees pipeline resolves no param " << name);
		return std::nanf(""); // NaN compares false against every literal above
	};
	ve::LeafSettings s;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity);
	CHECK(l.params.tree[0] == param("trees.cell"));
	CHECK(l.params.tree[1] == param("trees.density"));
	CHECK(l.params.tree[2] == param("trees.crown_radius"));
	CHECK(l.params.tree[3] == param("trees.trunk_radius"));
	CHECK(l.params.shape[0] == param("trees.trunk_height"));
	CHECK(l.params.shape[1] == param("trees.branch_radius_min"));
	CHECK(l.params.shape[2] == param("trees.max_slope"));
	// The lattice constant sizes the CPU dispatch grid, not just the uploaded block.
	CHECK(l.cell_size_m == param("trees.cell"));
	// shape[3] is the C++ surface constant, not a pipeline param -- pin the duplication.
	CHECK(l.params.shape[3] == ve::kSurfaceY);
	// The shipped game resolves default.pipeline, which includes the same trees stage with
	// no param overrides today. An author adding one there would move trunks against
	// canopies while trees.pipeline stays green -- pin it to the same literals.
	ve::ResolvedPipeline dp;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/default.pipeline",
			root + "/shaders/", &dp, nullptr, &err), err);
	auto dparam = [&dp](const char *name) {
		for (const ve::ParamDecl &d : dp.params)
			if (d.name == name) return d.value;
		INFO("the shipped default pipeline resolves no param " << name);
		return std::nanf("");
	};
	CHECK(l.params.tree[0] == dparam("trees.cell"));
	CHECK(l.params.tree[1] == dparam("trees.density"));
	CHECK(l.params.tree[2] == dparam("trees.crown_radius"));
	CHECK(l.params.tree[3] == dparam("trees.trunk_radius"));
	CHECK(l.params.shape[0] == dparam("trees.trunk_height"));
	CHECK(l.params.shape[1] == dparam("trees.branch_radius_min"));
	CHECK(l.params.shape[2] == dparam("trees.max_slope"));
	CHECK(l.cell_size_m == dparam("trees.cell"));
}
