#pragma once
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <vector>
#include "generator/volume_set.h"
#include "mesh/box_merge.h"

namespace godot {

// Everything a component needs to become a body. Filled by IslandManager (Task 13) from the
// component, its extraction and the edit that freed it.
struct IslandSpawn {
	int volume_slot = -1; // ve::VolumeSet slot holding the CPU copy (mass, resample, mesh)
	int atlas_slot = -1;  // IslandAtlas slot; -1 for debris, which is not raymarched
	std::vector<ve::CellBox> boxes;
	float lattice_origin[3] = {0, 0, 0}; // world, at birth
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	int solid_voxels = 0;
	float impulse[3] = {0, 0, 0}; // spec §6's "explosions apply radial impulses"
	bool debris = false;
};

// One dynamic piece of terrain: a Jolt rigid body carrying a box compound, its sleep clock,
// and (for debris) the mesh and RenderingServer instance that draw it.
//
// Server-direct, like ColliderStreamer and for the same reasons; main thread only, like
// PhysicsServer3D itself.
class IslandBody {
public:
	~IslandBody();

	// `scenario` is the World3D scenario used by the cel render instance. Islands may also
	// retain their atlas descriptor for the existing raymarch path.
	// The body's ORIGIN is the compound's centre, so it tumbles about itself, and
	// `local_lattice_origin()` is where the volume sits relative to that.
	bool spawn(RID space, RID scenario, const IslandSpawn &info, const ve::VolumeData *volume);
	void despawn();

	bool live() const { return body_.is_valid(); }
	RID body() const { return body_; }
	const IslandSpawn &info() const { return info_; }
	float mass() const { return mass_; }
	int shape_count() const { return static_cast<int>(shapes_.size()); }
	bool has_render_mesh() const { return mesh_.is_valid(); }
	bool has_cel_material() const;
	int render_triangles() const { return render_tris_; }
	const float *local_lattice_origin() const { return local_origin_; }

	Transform3D transform() const;
	// Polls PhysicsServer3D's BODY_STATE_SLEEPING and accumulates. Spec §6 says "Jolt sleep
	// events drive the re-merge hook"; this is that bit, read once a frame instead of
	// signalled, because the manager already runs every frame.
	void tick(float dt);
	float asleep_seconds() const { return asleep_; }
	// Pushes the body transform to the debris instance. No-op for a raymarched island: its
	// descriptor carries the transform instead.
	void sync_render();

private:
	void build_render_mesh(RID scenario, const ve::VolumeData &volume);

	IslandSpawn info_;
	RID body_;
	std::vector<RID> shapes_;
	Ref<ArrayMesh> mesh_;
	Ref<Material> render_material_;
	RID instance_;
	float mass_ = 0.0f;
	float asleep_ = 0.0f;
	int render_tris_ = 0;
	float local_origin_[3] = {0, 0, 0};
};

} // namespace godot
