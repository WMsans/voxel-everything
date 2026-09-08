#include <doctest/doctest.h>
#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace {
using Point = std::array<float, 3>;
using Cell = std::array<int, 3>;
using Edge = std::pair<Cell, Cell>;
Point sub(Point a, Point b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Point cross(Point a, Point b) {
	return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
float dot(Point a, Point b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
bool same(Point a, Point b) { return dot(sub(a,b), sub(a,b)) < 1e-10f; }
std::array<Point, 4> positions(const ve::LodQuad &q, Point origin = {}, float cell = 1) {
	ve::LodQuadFields f{};
	ve::lod_quad_unpack(q, &f);
	std::array<Point, 4> p;
	for (int k=0; k<4; ++k) ve::lod_quad_corner_pos(f, k, origin.data(), cell, p[k].data());
	return p;
}
Edge edge_key(const ve::LodQuadFields &f, int k) {
	Cell a, b;
	ve::lod_quad_corner_cell(f, k, a.data());
	ve::lod_quad_corner_cell(f, (k+1)%4, b.data());
	if (b<a) std::swap(a,b);
	return {a,b};
}
ve::LodQuad flat_quad(int axis, int sign, int boundary_axis, int boundary) {
	ve::LodQuadFields f{};
	f.axis = uint8_t(axis); f.sign = uint8_t(sign); f.material = 0xBEEF;
	for (int a=0; a<3; ++a) f.u[a] = 10;
	f.u[boundary_axis] = uint8_t(boundary);
	for (auto &o : f.offset) for (auto &v : o) v = 16;
	ve::LodQuad q;
	ve::lod_quad_pack(f, &q);
	return q;
}
std::vector<ve::LodQuad> plane(Point origin, float cell, int axis, int sign, float slope) {
	const int n = ve::kLodChunkLattice;
	const int b=(axis+1)%3, c=(axis+2)%3;
	std::vector<uint8_t> lattice(size_t(n)*n*n);
	std::vector<uint16_t> material(lattice.size(), 7);
	for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
		Point p = {origin[0]+(x-1)*cell, origin[1]+(y-1)*cell, origin[2]+(z-1)*cell};
		float d = p[axis]-16.25f-slope*(p[b]-64)-slope*0.5f*(p[c]-32);
		lattice[ve::lod_lattice_index(x,y,z)] = ve::encode_sdf(d*(sign ? 1 : -1)*0.2f/cell);
	}
	ve::LodContourResult r;
	ve::lod_contour(lattice.data(), material.data(), &r);
	return r.quads;
}
struct Triangle { Point a,b,c; };
void append_triangles(std::vector<Triangle> &out, const std::vector<ve::LodQuad> &quads,
		Point origin, float cell) {
	for (auto &q : quads) {
		auto p=positions(q,origin,cell);
		out.push_back({p[0],p[1],p[2]}); out.push_back({p[0],p[2],p[3]});
	}
}
// Moller-Trumbore with BACKFACE CULLING: a second winding must really cover the reverse ray.
bool hit(const std::vector<Triangle> &tris, Point origin, Point dir) {
	for (auto &t : tris) {
		Point e1=sub(t.b,t.a), e2=sub(t.c,t.a), h=cross(dir,e2);
		float det=dot(e1,h);
		if (det<=1e-7f) continue;
		Point s=sub(origin,t.a);
		float u=dot(s,h)/det;
		Point q=cross(s,e1);
		float v=dot(dir,q)/det, distance=dot(e2,q)/det;
		if (u>=-1e-5f && v>=-1e-5f && u+v<=1.00001f && distance>=0) return true;
	}
	return false;
}
}

TEST_CASE("boundary ribbons attach to original edges and extend outward into solid without clamping") {
	for (int axis=0; axis<3; ++axis) for (int sign=0; sign<2; ++sign)
		for (int b=0; b<3; ++b) if (b!=axis) for (int boundary : {0,31}) {
			ve::LodQuad q=flat_quad(axis,sign,b,boundary);
			ve::LodQuadFields f{}; ve::lod_quad_unpack(q,&f);
			f.u[axis]=uint8_t(sign ? 0 : 31); // previously zero clamped displacement
			ve::lod_quad_pack(f,&q);
			auto parent=positions(q);
			std::vector<ve::LodQuad> quads{q};
			const int c=3-axis-b;
			for (int db=-1; db<=1; ++db) for (int dc=-1; dc<=1; ++dc) {
				if ((db==0 && dc==0) || boundary+db<0 || boundary+db>31) continue;
				auto neighbour=f; neighbour.u[b]=uint8_t(boundary+db);
				neighbour.u[c]=uint8_t(10+dc); neighbour.material=7;
				ve::LodQuad n; ve::lod_quad_pack(neighbour,&n); quads.push_back(n);
			}
			const size_t surface=quads.size();
			ve::lod_append_skirts(&quads);
			std::vector<ve::LodQuad> ribbons;
			for (size_t i=surface; i<quads.size(); ++i) {
				ve::LodQuadFields s{}; ve::lod_quad_unpack(quads[i],&s);
				if (s.material==0xBEEF) ribbons.push_back(quads[i]);
			}
			REQUIRE(ribbons.size()==2);
			auto p=positions(ribbons[0]);
			int attached=0, extended=0;
			for (auto v : p) {
				bool found=false;
				for (auto a : parent) found |= same(v,a);
				if (found) { ++attached; continue; }
				++extended;
				CHECK((v[axis]-parent[0][axis])*(sign ? 1 : -1)<-0.5f);
				float edge=boundary==0 ? -15.0f/31 : 31+16.0f/31;
				CHECK((v[b]-edge)*(boundary==0 ? -1 : 1)>0.5f);
			}
			CHECK(attached==2); CHECK(extended==2);
			CHECK(dot(cross(sub(p[1],p[0]),sub(p[2],p[0])),
					cross(sub(p[1],p[0]),sub(p[2],p[0])))>0.1f);
			ve::LodQuadFields s{},r{};
			ve::lod_quad_unpack(ribbons[0],&s); ve::lod_quad_unpack(ribbons[1],&r);
			CHECK(s.material==0xBEEF); CHECK(r.material==0xBEEF);
			CHECK(s.sign==f.sign); CHECK(r.sign==f.sign);
			CHECK(s.double_sided==1); CHECK(r.double_sided==1);
			CHECK((ribbons[0].w[2]^ribbons[1].w[2])==0x80000000u);
			auto reverse=positions(ribbons[1]);
			const int order[4]={0,3,2,1};
			for (int k=0; k<4; ++k) CHECK(same(reverse[k],p[order[k]]));
		}
}

TEST_CASE("boundary ribbons count exposed cell-pair edges not every u31 quad") {
	for (int axis=0; axis<3; ++axis) for (int sign=0; sign<2; ++sign) {
		// A complete plane in the LAST cell of the normal axis: all 1024 quads have
		// u[axis]==31, but only the 128 perimeter EDGES (including both at corners) are exposed.
		std::vector<ve::LodQuad> quads;
		for (int x=0; x<32; ++x) for (int y=0; y<32; ++y) {
			ve::LodQuadFields f{};
			ve::lod_quad_unpack(flat_quad(axis,sign,(axis+1)%3,x),&f);
			f.u[axis]=31; f.u[(axis+2)%3]=uint8_t(y);
			ve::LodQuad q; ve::lod_quad_pack(f,&q); quads.push_back(q);
		}
		auto originals=quads;
		std::map<Edge,int> incidence;
		for (auto q : originals) {
			ve::LodQuadFields f{}; ve::lod_quad_unpack(q,&f);
			for (int k=0; k<4; ++k) ++incidence[edge_key(f,k)];
		}
		REQUIRE(ve::lod_append_skirts(&quads)==256);
		CHECK(quads.size()==1280);
		for (size_t i=0; i<originals.size(); ++i)
			for (int w=0; w<3; ++w) CHECK(quads[i].w[w]==originals[i].w[w]);
		// Every exposed edge must be attached exactly once (two triangles/windings excluded).
		std::map<Edge,int> attached;
		for (size_t i=originals.size(); i<quads.size(); i+=2) {
			auto p=positions(quads[i]);
			int matches=0;
			for (auto q : originals) {
				ve::LodQuadFields f{}; ve::lod_quad_unpack(q,&f);
				auto v=positions(q);
				for (int k=0; k<4; ++k) if ((same(p[0],v[k]) && same(p[1],v[(k+1)%4])) ||
						(same(p[1],v[k]) && same(p[0],v[(k+1)%4]))) {
					CHECK(incidence[edge_key(f,k)]==1);
					++attached[edge_key(f,k)]; ++matches;
				}
			}
			CHECK(matches==1);
		}
		for (auto &[edge,count] : incidence) if (count==1) CHECK(attached[edge]==1);
		CHECK(ve::lod_append_skirts(&quads)==0); // repeat calls must not duplicate coplanar ribbons
	}
}

TEST_CASE("boundary ribbons close 2to1 plane gaps for culled rays from both directions including chunk corners") {
	for (int axis=0; axis<3; ++axis) for (int sign=0; sign<2; ++sign)
		for (bool coarse_left : {false,true}) for (float slope : {0.0f,0.25f,-0.25f}) {
			CAPTURE(axis); CAPTURE(sign); CAPTURE(coarse_left); CAPTURE(slope);
			const int b=(axis+1)%3, c=(axis+2)%3;
			std::vector<Triangle> bare, skirted;
			for (int chunk=0; chunk<3; ++chunk) {
				float cell=chunk==0 ? 2 : 1;
				Point origin{};
				origin[b]=chunk==0 ? (coarse_left ? 0 : 64) : (coarse_left ? 64 : 32);
				origin[c]=chunk==2 ? 32 : 0;
				auto quads=plane(origin,cell,axis,sign,slope);
				append_triangles(bare,quads,origin,cell);
				ve::lod_append_skirts(&quads);
				append_triangles(skirted,quads,origin,cell);
			}
			if (coarse_left && slope==0) {
				// Coarse max = 64-30/31; fine min = 64-15/31. This ray is in the real gap.
				Point o{},d{}; o[b]=63.25f; o[c]=16; o[axis]=sign ? 80 : -40;
				d[axis]=sign ? -1 : 1;
				CHECK_FALSE(hit(bare,o,d));
			}
			int misses=0;
			for (float along : {-0.1f,0.1f,15.7f,31.1f,31.4f,31.7f,32.1f,47.3f,62.7f})
				for (float across : {-1.1f,-0.85f,-0.6f,-0.35f,-0.1f,0.15f})
					for (int dir : {-1,1}) {
						Point o{},d{}; o[b]=64+across; o[c]=along;
						o[axis]=dir<0 ? 80 : -40; d[axis]=float(dir);
						// Ordinary surface quads intentionally cull rays originating inside solid.
						// Require reverse-ray coverage at real holes, not under intact surfaces.
						if (dir==(sign ? 1 : -1)) {
							Point air=o, toward_air{}; air[axis]=sign ? 80 : -40;
							toward_air[axis]=sign ? -1.0f : 1.0f;
							if (hit(bare,air,toward_air)) continue;
						}
						if (!hit(skirted,o,d)) ++misses;
					}
			CHECK(misses==0);
		}
}

TEST_CASE("boundary ribbons respect the cap without a one-sided tail") {
	std::vector<ve::LodQuad> quads;
	// Interior filler is deliberately not boundary geometry.
	ve::LodQuad q=flat_quad(1,1,0,10);
	quads.resize(size_t(ve::kLodMaxQuadsPerChunk)-5,q);
	quads.push_back(flat_quad(1,1,0,0));
	ve::LodQuadFields adjacent{}; ve::lod_quad_unpack(flat_quad(1,1,0,0),&adjacent);
	adjacent.u[2]=11;
	ve::LodQuad second; ve::lod_quad_pack(adjacent,&second); quads.push_back(second);
	std::vector<ve::LodQuadNormals> normals(quads.size());
	for (auto &n : normals) for (uint16_t &corner : n.corner) corner = 0x1234u;
	CHECK(ve::lod_append_skirts(&quads, &normals)==2);
	CHECK(quads.size()==size_t(ve::kLodMaxQuadsPerChunk)-1);
	REQUIRE(normals.size()==quads.size());
	for (size_t i=normals.size()-2; i<normals.size(); ++i)
		for (uint16_t corner : normals[i].corner) CHECK(corner==0x1234u);
}

TEST_CASE("empty boundary ribbon input is safe") {
	std::vector<ve::LodQuad> quads;
	CHECK(ve::lod_append_skirts(nullptr)==0);
	CHECK(ve::lod_append_skirts(&quads)==0);
}

TEST_CASE("boundary ribbon keeps corner overlap already directed into solid") {
	ve::LodQuadFields f{};
	ve::lod_quad_unpack(flat_quad(2,0,0,31),&f);
	f.u[2]=31;
	f.double_sided=1; f.skirt_face=5; f.skirt_edge=1;
	ve::LodQuad q; ve::lod_quad_pack(f,&q);
	auto p=positions(q);
	// Stored edge 1 ends at canonical corner 2: the +X,+Z endpoint.
	float original[3]; const float origin[3]={};
	f.double_sided=0;
	ve::lod_quad_corner_pos(f,2,origin,1,original);
	CHECK(p[3][0]-original[0]==doctest::Approx(2));
	CHECK(p[3][1]-original[1]==doctest::Approx(0));
	CHECK(p[3][2]-original[2]==doctest::Approx(2));
}

TEST_CASE("boundary ribbon rejects a near tangent edge instead of extruding across four chunks") {
	ve::LodQuadFields f{};
	ve::lod_quad_unpack(flat_quad(2,1,0,31),&f);
	// First triangle's normal is proportional to (31,0,1), all coordinates representable.
	for (int k=0; k<4; ++k) {
		f.offset[k][0]=uint8_t((k==0 || k==3) ? 31 : 1);
		f.offset[k][2]=uint8_t((k==0 || k==3) ? 31 : 0);
	}
	ve::LodQuad q; ve::lod_quad_pack(f,&q);
	std::vector<ve::LodQuad> quads{q};
	// A two-cell outward/two-cell inward solution needs about 124 cells along Z.
	// Unsupported coverage must be rejected, not silently emitted or clamped.
	CHECK(ve::lod_append_skirts(&quads)==0);
	CHECK(quads.size()==1);
}
