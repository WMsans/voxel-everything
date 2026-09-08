#include <doctest/doctest.h>
#include "lod/lod_quad.h"
#include "lod/lod_skirt.h"
#include <cmath>
#include <cstdint>

// Execute the ACTUAL shared GLSL decoder in a native test. This shim supplies only GLSL
// scalar/vector operations; no geometry, packing or normal computation is duplicated.
// Brace initializers are supported by both C++20 and the shader's GLSL 460.
namespace shader {
using uint = uint32_t;
struct ivec2 {
	int x,y;
	ivec2(int a,int b):x(a),y(b) {}
};
template<class T> struct V3 {
	T x,y,z;
	explicit V3(T a):x(a),y(a),z(a) {}
	V3(T a,T b,T c):x(a),y(b),z(c) {}
	template<class U> explicit V3(V3<U> a):x(T(a.x)),y(T(a.y)),z(T(a.z)) {}
	T &operator[](int i) { return i==0 ? x : (i==1 ? y : z); }
	T operator[](int i) const { return i==0 ? x : (i==1 ? y : z); }
};
using ivec3=V3<int>;
using uvec3=V3<uint>;
using vec3=V3<float>;
using bvec3=V3<bool>;
template<class T> V3<T> operator+(V3<T> a,V3<T> b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
template<class T, class U> V3<T> operator+(V3<T> a,U b) { return a+V3<T>(T(b)); }
template<class T> V3<T> operator-(V3<T> a,V3<T> b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
template<class T, class U> V3<T> operator-(V3<T> a,U b) { return a-V3<T>(T(b)); }
template<class T, class U> V3<T> operator*(V3<T> a,U b) { return {T(a.x*b),T(a.y*b),T(a.z*b)}; }
template<class T, class U> V3<T> operator*(U a,V3<T> b) { return b*a; }
template<class T, class U> V3<T> operator/(V3<T> a,U b) { return {T(a.x/b),T(a.y/b),T(a.z/b)}; }
float dot(vec3 a,vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
vec3 cross(vec3 a,vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float inversesqrt(float a) { return 1.0f/std::sqrt(a); }
using std::abs;
using std::round;
using std::isnan;
using std::isinf;
#include "../../shaders/lod_quad.glslh"
}

TEST_CASE("boundary ribbon shared shader preserves parent shading and matches CPU geometry") {
	const float origin[3]={-127.5f,38.25f,91.0f};
	const shader::vec3 shader_origin(origin[0],origin[1],origin[2]);
	for (int axis=0; axis<3; ++axis) for (int sign=0; sign<2; ++sign)
		for (int face=0; face<6; ++face) for (int edge=0; edge<4; ++edge) {
			ve::LodQuadFields parent{};
			parent.axis=uint8_t(axis); parent.sign=uint8_t(sign);
			parent.u[0]=7; parent.u[1]=11; parent.u[2]=19;
			parent.u[face/2]=uint8_t((face&1) ? 31 : 0);
			parent.material=0xFFEF;
			// Nonplanar offsets make deriving the normal from the ribbon, or from the
			// wrong three parent corners, observably different from the original shading.
			for (int k=0; k<4; ++k) for (int a=0; a<3; ++a)
				parent.offset[k][a]=uint8_t((3+k*7+a*5)%32);
			int m0[3],m1[3];
			ve::lod_quad_corner_cell(parent,edge,m0);
			ve::lod_quad_corner_cell(parent,(edge+1)&3,m1);
			if (m0[face/2]!=m1[face/2] || m0[face/2]!=((face&1) ? 32 : 0)) continue;
			ve::LodQuad original; ve::lod_quad_pack(parent,&original);
			shader::uvec3 ow(original.w[0],original.w[1],original.w[2]);
			const auto normal=shader::lod_parent_normal(ow);
			for (int reverse=0; reverse<2; ++reverse) {
				auto s=parent; s.double_sided=1; s.reverse_winding=uint8_t(reverse);
				s.skirt_face=uint8_t(face); s.skirt_edge=uint8_t(edge);
				ve::LodQuad q; ve::lod_quad_pack(s,&q);
				ve::LodQuadFields decoded{}; ve::lod_quad_unpack(q,&decoded);
				CHECK(decoded.skirt_face==face); CHECK(decoded.skirt_edge==edge);
				CHECK(decoded.reverse_winding==reverse); CHECK(decoded.material==0xFFEF);
				for (int a=0; a<3; ++a) CHECK(decoded.u[a]==parent.u[a]);
				shader::uvec3 w(q.w[0],q.w[1],q.w[2]);
				const auto shading=shader::lod_parent_normal(w);
				float cpu_normal[3]; ve::lod_quad_normal(decoded,cpu_normal);
				for (int a=0; a<3; ++a) {
					CHECK(shading[a]==normal[a]); // exact, even for the reversed ribbon
					CHECK(cpu_normal[a]==doctest::Approx(normal[a]).epsilon(1e-5));
				}
				for (int k=0; k<4; ++k) {
					float cpu[3]; ve::lod_quad_corner_pos(decoded,k,origin,0.8f,cpu);
					const auto gpu=shader::lod_corner_pos(w,k,shader_origin,0.8f);
					for (int a=0; a<3; ++a) CHECK(cpu[a]==doctest::Approx(gpu[a]).epsilon(1e-5));
				}
			}
		}
}

TEST_CASE("boundary ribbon uses both free axes when a bounded inward solution exists") {
	// Removing multi-axis redistribution must reject these otherwise feasible +X edges.
	// Parent normal is proportional to (drop_x, drop_y, 16). The second fixture also
	// needs redistribution after the unconstrained minimum-length Z displacement exceeds 4.
	for (const auto drops : {shader::ivec2(15,15), shader::ivec2(18,9)}) {
		ve::LodQuadFields f{};
		f.axis=2; f.sign=1; f.u[0]=31; f.u[1]=10; f.u[2]=10;
		const int z[4]={31,31-drops.x,31-drops.x-drops.y,31-drops.y};
		for (int k=0; k<4; ++k) {
			f.offset[k][0]=uint8_t((k==0 || k==3) ? 31 : 16);
			f.offset[k][1]=uint8_t(k<2 ? 31 : 16);
			f.offset[k][2]=uint8_t(z[k]);
		}
		ve::LodQuad q; ve::lod_quad_pack(f,&q);
		std::vector<ve::LodQuad> quads{q};
		ve::LodSkirtStatus status;
		REQUIRE(ve::lod_append_skirts(&quads,&status)==2);
		CHECK(status.unsupported_edges==0);
		CHECK(status.capacity_edges==0);
		const float length=std::sqrt(float(drops.x*drops.x+drops.y*drops.y+256));
		const float normal[3]={drops.x/length,drops.y/length,16/length};
		for (size_t i=1; i<quads.size(); ++i) {
			ve::LodQuadFields s{}; ve::lod_quad_unpack(quads[i],&s);
			shader::uvec3 w(quads[i].w[0],quads[i].w[1],quads[i].w[2]);
			const float origin[3]={};
			for (int k=0; k<4; ++k) {
				float cpu[3]; ve::lod_quad_corner_pos(s,k,origin,1,cpu);
				const auto gpu=shader::lod_corner_pos(w,k,shader::vec3(0.0f),1);
				for (int a=0; a<3; ++a) CHECK(cpu[a]==doctest::Approx(gpu[a]).epsilon(1e-5));
			}
			if (s.reverse_winding) continue;
			for (int k : {2,3}) {
				float p[3],parent[3];
				ve::lod_quad_corner_pos(s,k,origin,1,p);
				ve::lod_quad_parent_corner_pos(f,k==2 ? 1 : 2,origin,1,parent);
				float inward=0;
				for (int a=0; a<3; ++a) {
					const float d=p[a]-parent[a];
					CHECK(std::abs(d)<=4.00001f);
					inward+=d*normal[a];
				}
				CHECK(p[0]-parent[0]==doctest::Approx(2));
				CHECK(inward<=-1.99999f);
			}
		}
	}
}

TEST_CASE("boundary ribbon shared shader leaves ordinary surface positions unchanged") {
	const float origin[3]={-80,120,0};
	for (int axis=0; axis<3; ++axis) for (int sign=0; sign<2; ++sign) {
		ve::LodQuadFields f{};
		f.axis=uint8_t(axis); f.sign=uint8_t(sign);
		f.u[0]=31; f.u[1]=0; f.u[2]=17;
		for (int k=0; k<4; ++k) for (int a=0; a<3; ++a) f.offset[k][a]=uint8_t(k*7+a);
		ve::LodQuad q; ve::lod_quad_pack(f,&q);
		shader::uvec3 w(q.w[0],q.w[1],q.w[2]);
		for (int k=0; k<4; ++k) {
			float cpu[3]; ve::lod_quad_corner_pos(f,k,origin,3.2f,cpu);
			auto gpu=shader::lod_corner_pos(w,k,shader::vec3(-80,120,0),3.2f);
			for (int a=0; a<3; ++a) CHECK(cpu[a]==doctest::Approx(gpu[a]).epsilon(1e-5));
		}
	}
}
