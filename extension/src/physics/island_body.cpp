#include "physics/island_body.h"
#include "mesh/dual_contour.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

using namespace godot;

namespace {

// Rock is ~2600 kg/m^3. The demo uses a fifth of that: at true density a 3 m slab weighs
// twenty tonnes, lands like a dropped anvil and shrugs off the explosion impulse that freed
// it. Lighter reads as rubble. One constant, tuned by eye, spec §5's "one-constant tunables".
constexpr float kIslandDensity = 500.0f;

} // namespace

IslandBody::~IslandBody() {
	despawn();
}

bool IslandBody::spawn(RID space, RID scenario, const IslandSpawn &info,
		const ve::VolumeData *volume) {
	despawn();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || info.boxes.empty()) return false;
	info_ = info;

	float com[3] = {0, 0, 0};
	float box_volume = 0.0f;
	ve::box_compound_mass(info.boxes.data(), static_cast<int>(info.boxes.size()), com,
			&box_volume);
	// Mass from the SOLID volume, not the boxes': a component's cells are only partly full
	// where the surface crosses them, and a hollow shell should not weigh like a brick.
	const float solid_m3 = static_cast<float>(info.solid_voxels) * info.voxel * info.voxel *
			info.voxel;
	mass_ = std::max(solid_m3 * kIslandDensity, 1.0f);
	for (int a = 0; a < 3; a++) local_origin_[a] = info.lattice_origin[a] - com[a];

	body_ = ps->body_create();
	ps->body_set_mode(body_, PhysicsServer3D::BODY_MODE_RIGID);
	ps->body_set_collision_layer(body_, 1);
	ps->body_set_collision_mask(body_, 1);
	for (const ve::CellBox &b : info.boxes) {
		float lo[3], hi[3];
		b.world_aabb(lo, hi);
		RID shape = ps->box_shape_create();
		ps->shape_set_data(shape, Vector3(0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]),
									 0.5f * (hi[2] - lo[2])));
		Transform3D xf;
		xf.origin = Vector3(0.5f * (lo[0] + hi[0]) - com[0], 0.5f * (lo[1] + hi[1]) - com[1],
				0.5f * (lo[2] + hi[2]) - com[2]);
		ps->body_add_shape(body_, shape, xf);
		shapes_.push_back(shape);
	}
	ps->body_set_param(body_, PhysicsServer3D::BODY_PARAM_MASS, mass_);
	// Inertia is left at zero, which is Godot's "derive it from the shapes" sentinel: the
	// compound already has the right distribution and Jolt computes a better tensor from it
	// than any closed form over the cell set would.
	Transform3D at;
	at.origin = Vector3(com[0], com[1], com[2]);
	ps->body_set_state(body_, PhysicsServer3D::BODY_STATE_TRANSFORM, at);
	// Spec §6: "CCD on fast debris". A crumb is small and gets thrown hard; a slab is neither.
	ps->body_set_enable_continuous_collision_detection(body_, info.debris);
	ps->body_set_space(body_, space);

	const Vector3 imp(info.impulse[0], info.impulse[1], info.impulse[2]);
	if (imp.length_squared() > 0.0f) ps->body_apply_impulse(body_, imp);

	if (info.debris && volume && !volume->empty()) build_render_mesh(scenario, *volume);
	return true;
}

void IslandBody::despawn() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (ps) {
		if (body_.is_valid()) {
			ps->body_set_space(body_, RID());
			ps->free_rid(body_);
		}
		for (RID s : shapes_)
			if (s.is_valid()) ps->free_rid(s);
	}
	body_ = RID();
	shapes_.clear();
	if (instance_.is_valid()) {
		RenderingServer::get_singleton()->free_rid(instance_);
		instance_ = RID();
	}
	mesh_.unref();
	render_material_.unref();
	render_tris_ = 0;
	asleep_ = 0.0f;
	mass_ = 0.0f;
}

Transform3D IslandBody::transform() const {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !body_.is_valid()) return Transform3D();
	return ps->body_get_state(body_, PhysicsServer3D::BODY_STATE_TRANSFORM);
}

void IslandBody::tick(float dt) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !body_.is_valid()) return;
	const bool sleeping = ps->body_get_state(body_, PhysicsServer3D::BODY_STATE_SLEEPING);
	asleep_ = sleeping ? asleep_ + dt : 0.0f;
}

void IslandBody::sync_render() {
	if (!instance_.is_valid()) return;
	RenderingServer::get_singleton()->instance_set_transform(instance_, transform());
}

bool IslandBody::has_cel_material() const {
	ShaderMaterial *cel = Object::cast_to<ShaderMaterial>(render_material_.ptr());
	if (!cel || !cel->get_shader().is_valid()) return false;
	return cel->get_shader()->get_path() == String("res://shaders/cel_object.gdshader");
}

void IslandBody::build_render_mesh(RID scenario, const ve::VolumeData &volume) {
	// ve::dual_contour's convention: lattice index i holds the sample at local coordinate
	// i - 1, and a vertex lands at g.origin + (i - 1 + frac) * cell_size. Offsetting the
	// grid origin by one voxel therefore puts lattice index i at the volume's own sample i,
	// and subtracting the body's centre puts the whole mesh in BODY-LOCAL space.
	ve::DcGrid g;
	g.lattice = volume.dim;
	g.cell_size = info_.voxel;
	for (int a = 0; a < 3; a++) g.origin[a] = local_origin_[a] + info_.voxel;

	ve::MeshBuffer mb;
	ve::dual_contour(volume.sdf.data(), g, &mb);
	if (mb.triangle_count() == 0) return;

	PackedVector3Array verts;
	verts.resize(mb.vertex_count());
	Vector3 *vw = verts.ptrw();
	for (int i = 0; i < mb.vertex_count(); i++)
		vw[i] = Vector3(mb.positions[i * 3 + 0], mb.positions[i * 3 + 1],
				mb.positions[i * 3 + 2]);

	PackedInt32Array idx;
	idx.resize(static_cast<int64_t>(mb.indices.size()));
	int32_t *iw = idx.ptrw();
	for (size_t i = 0; i < mb.indices.size(); i++)
		iw[i] = static_cast<int32_t>(mb.indices[i]);

	// Area-weighted vertex normals from the faces. Without them the mesh renders unlit-black,
	// and the SDF gradient is not available on this side of the readback.
	PackedVector3Array normals;
	normals.resize(mb.vertex_count());
	Vector3 *nw = normals.ptrw();
	for (int i = 0; i < mb.vertex_count(); i++) nw[i] = Vector3();
	for (size_t t = 0; t + 2 < mb.indices.size(); t += 3) {
		const Vector3 &a = vw[mb.indices[t + 0]];
		const Vector3 &b = vw[mb.indices[t + 1]];
		const Vector3 &c = vw[mb.indices[t + 2]];
		const Vector3 fn = (b - a).cross(c - a); // unnormalised: area weighting for free
		nw[mb.indices[t + 0]] += fn;
		nw[mb.indices[t + 1]] += fn;
		nw[mb.indices[t + 2]] += fn;
	}
	for (int i = 0; i < mb.vertex_count(); i++)
		nw[i] = nw[i].length_squared() > 0.0f ? nw[i].normalized() : Vector3(0, 1, 0);

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh_.instantiate();
	mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<StandardMaterial3D> fallback;
	fallback.instantiate();
	fallback->set_albedo(Color(0.45f, 0.42f, 0.40f));
	render_material_ = fallback;
	if (ResourceLoader::get_singleton()) {
		Ref<Shader> shader = ResourceLoader::get_singleton()->load(
				"res://shaders/cel_object.gdshader");
		if (shader.is_valid()) {
			Ref<ShaderMaterial> cel;
			cel.instantiate();
			cel->set_shader(shader);
			cel->set_shader_parameter("base_color_linear", Vector3(0.45f, 0.42f, 0.40f));
			cel->set_shader_parameter("ambient_linear", Vector3(0.16f, 0.19f, 0.26f));
			render_material_ = cel;
		}
	}
	mesh_->surface_set_material(0, render_material_);
	render_tris_ = mb.triangle_count();

	RenderingServer *rs = RenderingServer::get_singleton();
	instance_ = rs->instance_create2(mesh_->get_rid(), scenario);
	rs->instance_set_transform(instance_, transform());
}
