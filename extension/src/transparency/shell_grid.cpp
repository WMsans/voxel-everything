#include "transparency/shell_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <algorithm>
#include <cmath>

namespace ve {

void shell_chunk_origin(IVec3 c, float out[3]) {
	out[0] = static_cast<float>(c.x) * kShellChunkSize;
	out[1] = static_cast<float>(c.y) * kShellChunkSize;
	out[2] = static_cast<float>(c.z) * kShellChunkSize;
}

void shell_chunk_aabb(IVec3 c, float lo[3], float hi[3]) {
	shell_chunk_origin(c, lo);
	for (int a = 0; a < 3; a++) hi[a] = lo[a] + kShellChunkSize;
}

IVec3 shell_chunk_of_point(float x, float y, float z) {
	return IVec3{static_cast<int>(std::floor(x / kShellChunkSize)),
			static_cast<int>(std::floor(y / kShellChunkSize)),
			static_cast<int>(std::floor(z / kShellChunkSize))};
}

namespace {

bool boxes_overlap(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3]) {
	for (int a = 0; a < 3; a++)
		if (alo[a] > bhi[a] || ahi[a] < blo[a]) return false;
	return true;
}

bool bytes_have_transparent(const uint8_t *mat, size_t n) {
	// At most a handful of transparent ids; test each byte against the table.
	for (size_t i = 0; i < n; i++)
		if (mat[i] != 0 && material_transparent(mat[i])) return true;
	return false;
}

} // namespace

bool volume_has_transparent(const VolumeData &v) {
	return v.valid() && bytes_have_transparent(v.mat.data(), v.mat.size());
}

void transparent_boxes(const EditOp *ops, int op_count, const VolumeSet *volumes,
		const OverrideStore *overrides, const float lo[3], const float hi[3],
		std::vector<ShellBox> *out) {
	if (!out) return;
	for (int i = 0; ops && i < op_count; i++) {
		const EditOp &op = ops[i];
		bool transparent = false;
		if (op.type == kOpSphereAdd || op.type == kOpSpherePaint) {
			transparent = material_transparent(static_cast<uint16_t>(op.material));
		} else if (op.type == kOpVolumeAdd && volumes) {
			const VolumeData *v = volumes->get(static_cast<int>(op.aux[0]));
			transparent = v && volume_has_transparent(*v);
		}
		if (!transparent) continue;
		ShellBox b{};
		op_world_aabb(op, b.lo, b.hi);
		out->push_back(b);
	}
	if (!overrides) return;
	// ponytail: rescans the material bytes of every override brick in range on each call;
	// cache a per-slot verdict keyed by consolidation if this shows up in a profile.
	overrides->for_each([&](IVec3 brick, const OverrideBrick &ob) {
		ShellBox b{};
		brick_world_origin(brick, b.lo);
		for (int a = 0; a < 3; a++) b.hi[a] = b.lo[a] + kBrickSize;
		if (!boxes_overlap(b.lo, b.hi, lo, hi)) return;
		if (bytes_have_transparent(ob.mat, static_cast<size_t>(kBrickVoxelCount))) out->push_back(b);
	});
}

void shell_candidates(const std::vector<ShellBox> &boxes, const float cam[3], float radius_m,
		std::vector<IVec3> *out) {
	if (!out) return;
	out->clear();
	std::map<std::tuple<int, int, int>, IVec3> seen;
	for (const ShellBox &b : boxes) {
		// One cell of pad: a shell quad's corners sit in the cells around its edge.
		const IVec3 c0 = shell_chunk_of_point(b.lo[0] - kShellCell, b.lo[1] - kShellCell,
				b.lo[2] - kShellCell);
		const IVec3 c1 = shell_chunk_of_point(b.hi[0] + kShellCell, b.hi[1] + kShellCell,
				b.hi[2] + kShellCell);
		for (int z = c0.z; z <= c1.z; z++)
			for (int y = c0.y; y <= c1.y; y++)
				for (int x = c0.x; x <= c1.x; x++) {
					float lo[3], hi[3];
					shell_chunk_aabb(IVec3{x, y, z}, lo, hi);
					float d2 = 0.0f;
					for (int a = 0; a < 3; a++) {
						const float d = std::max(std::max(lo[a] - cam[a], cam[a] - hi[a]), 0.0f);
						d2 += d * d;
					}
					if (d2 > radius_m * radius_m) continue;
					seen[std::tuple<int, int, int>{z, y, x}] = IVec3{x, y, z};
				}
	}
	out->reserve(seen.size());
	for (const auto &[k, c] : seen) out->push_back(c);
}

void ShellGrid::set_candidates(const std::vector<IVec3> &chunks, std::vector<IVec3> *evicted) {
	std::map<Key, Node> next;
	for (IVec3 c : chunks) {
		const auto it = nodes_.find(key(c));
		next[key(c)] = it != nodes_.end() ? it->second : Node{};
	}
	if (evicted)
		for (const auto &[k, n] : nodes_)
			if (next.find(k) == next.end())
				evicted->push_back(IVec3{std::get<2>(k), std::get<1>(k), std::get<0>(k)});
	nodes_.swap(next);
}

void ShellGrid::mark_dirty(const float lo[3], const float hi[3]) {
	// Two cells of pad, as LoD chunks use: the half-cell tent reaches past a chunk face.
	const float pad = 2.0f * kShellCell;
	const IVec3 c0 = shell_chunk_of_point(lo[0] - pad, lo[1] - pad, lo[2] - pad);
	const IVec3 c1 = shell_chunk_of_point(hi[0] + pad, hi[1] + pad, hi[2] + pad);
	for (auto &[k, n] : nodes_) {
		const int x = std::get<2>(k), y = std::get<1>(k), z = std::get<0>(k);
		if (x >= c0.x && x <= c1.x && y >= c0.y && y <= c1.y && z >= c0.z && z <= c1.z)
			n.dirty = true;
	}
}

void ShellGrid::requests(const float cam[3], int max, std::vector<IVec3> *out) const {
	if (!out) return;
	out->clear();
	std::vector<std::pair<float, IVec3>> want;
	for (const auto &[k, n] : nodes_) {
		if (n.state == kShellBuilding) continue;
		if (n.state != kShellUnknown && !n.dirty) continue;
		const IVec3 c{std::get<2>(k), std::get<1>(k), std::get<0>(k)};
		float lo[3], hi[3];
		shell_chunk_aabb(c, lo, hi);
		float d2 = 0.0f;
		for (int a = 0; a < 3; a++) {
			const float d = 0.5f * (lo[a] + hi[a]) - cam[a];
			d2 += d * d;
		}
		want.push_back({d2, c});
	}
	std::stable_sort(want.begin(), want.end(),
			[](const auto &a, const auto &b) { return a.first < b.first; });
	for (int i = 0; i < static_cast<int>(want.size()) && i < max; i++)
		out->push_back(want[static_cast<size_t>(i)].second);
}

void ShellGrid::note_building(IVec3 c) {
	const auto it = nodes_.find(key(c));
	if (it == nodes_.end()) return;
	it->second.state = kShellBuilding;
	it->second.dirty = false;
}

void ShellGrid::note_result(IVec3 c, bool has_quads) {
	const auto it = nodes_.find(key(c));
	if (it == nodes_.end()) return;
	it->second.state = has_quads ? kShellReady : kShellEmpty; // dirty is left as it is
}

void ShellGrid::note_failed(IVec3 c) {
	const auto it = nodes_.find(key(c));
	if (it != nodes_.end()) it->second.state = kShellUnknown;
}

ShellState ShellGrid::state(IVec3 c) const {
	const auto it = nodes_.find(key(c));
	return it == nodes_.end() ? kShellUnknown : it->second.state;
}

bool ShellGrid::dirty(IVec3 c) const {
	const auto it = nodes_.find(key(c));
	return it != nodes_.end() && it->second.dirty;
}

int ShellGrid::size() const { return static_cast<int>(nodes_.size()); }
void ShellGrid::clear() { nodes_.clear(); }

void island_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		IslandMeshKind kind, std::vector<IslandShellBlock> *out) {
	if (!out) return;
	out->clear();
	const bool shell = kind == IslandMeshKind::kShell;
	if (shell && !volume_has_transparent(v)) return;
	const int n = kLodChunkLattice;
	const int blocks = (v.dim + kLodChunkCells - 1) / kLodChunkCells;
	std::vector<uint8_t> lat(static_cast<size_t>(n) * n * n);
	std::vector<uint16_t> mat(lat.size());
	std::vector<uint8_t> opaque(shell ? 0 : lat.size());
	const uint8_t outside = encode_sdf(kSdfRange);
	for (int bz = 0; bz < blocks; bz++)
		for (int by = 0; by < blocks; by++)
			for (int bx = 0; bx < blocks; bx++) {
				bool any = false;
				// Lattice index i holds the island sample at block * 32 + i - 1 (the LoD
				// lattice's one-cell overlap below the origin). Outside the island: air.
				for (int z = 0; z < n; z++)
					for (int y = 0; y < n; y++)
						for (int x = 0; x < n; x++) {
							const int sx = bx * kLodChunkCells + x - 1;
							const int sy = by * kLodChunkCells + y - 1;
							const int sz = bz * kLodChunkCells + z - 1;
							const int i = lod_lattice_index(x, y, z);
							if (sx < 0 || sy < 0 || sz < 0 || sx >= v.dim || sy >= v.dim || sz >= v.dim) {
								lat[static_cast<size_t>(i)] = outside;
								mat[static_cast<size_t>(i)] = 0;
								continue;
							}
							const int s = VolumeSet::voxel_index(v.dim, sx, sy, sz);
							lat[static_cast<size_t>(i)] = v.sdf[static_cast<size_t>(s)];
							mat[static_cast<size_t>(i)] = v.mat[static_cast<size_t>(s)];
							const uint8_t m = v.mat[static_cast<size_t>(s)];
							any = any || (shell ? material_transparent(m)
								      : decode_sdf(v.sdf[static_cast<size_t>(s)]) <= 0.0f &&
															     !material_transparent(m));
						}
				if (!any) continue;
				LodContourResult r;
				if (shell) {
					lod_contour(lat.data(), mat.data(), &r, true);
				} else {
					lod_opaque_lattice(lat.data(), mat.data(), voxel, opaque.data());
					lod_contour(opaque.data(), mat.data(), &r, false);
				}
				if (r.quads.empty()) continue;
				IslandShellBlock b;
				b.origin_local[0] = lattice_origin[0] + static_cast<float>(bx * kLodChunkCells) * voxel;
				b.origin_local[1] = lattice_origin[1] + static_cast<float>(by * kLodChunkCells) * voxel;
				b.origin_local[2] = lattice_origin[2] + static_cast<float>(bz * kLodChunkCells) * voxel;
				b.quads = std::move(r.quads);
				b.normals = std::move(r.normals);
				out->push_back(std::move(b));
			}
}

float shell_thickness(float r, float g, float z_opaque, float z_front, float sky_thickness_m) {
	const float z_exit = z_opaque > 0.0f ? z_opaque : z_front + sky_thickness_m;
	return std::max(r + g * z_exit, 0.0f);
}

} // namespace ve