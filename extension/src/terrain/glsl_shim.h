// GLSL scalar and vector operations, so a shader header can be executed natively by the CPU
// mirror and by doctest. INCLUDE IT INSIDE YOUR OWN NAMESPACE, after <cmath>, <cstdint> and
// <cstring>: every name here must shadow the global C maths functions, which only happens
// when they are declared in the same namespace the shader header lands in. No #pragma once
// on purpose -- each namespace gets its own copy.
//
// Nothing here is shader maths; it is the GLSL vocabulary the headers are written in.
using uint = uint32_t;
template<class T> struct V2 {
	T x, y;
	V2() : x(0), y(0) {}
	explicit V2(T a) : x(a), y(a) {}
	V2(T a, T b) : x(a), y(b) {}
};
template<class T> struct V3 {
	T x, y, z;
	V3() : x(0), y(0), z(0) {}
	explicit V3(T a) : x(a), y(a), z(a) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
};
template<class T> struct V4 {
	T x, y, z, w;
	V4() : x(0), y(0), z(0), w(0) {}
	explicit V4(T a) : x(a), y(a), z(a), w(a) {}
	V4(T a, T b, T c, T d) : x(a), y(b), z(c), w(d) {}
};
using vec2 = V2<float>;
using vec3 = V3<float>;
using vec4 = V4<float>;
using ivec2 = V2<int>;
using uvec2 = V2<uint>;
template<class T> V2<T> operator+(V2<T> a, V2<T> b) { return {a.x + b.x, a.y + b.y}; }
template<class T> V2<T> operator-(V2<T> a, V2<T> b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
inline vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float sqrt(float v) { return std::sqrt(v); }
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float floor(float v) { return std::floor(v); }
inline float abs(float v) { return std::fabs(v); }
inline float length(vec3 a) { return sqrt(dot(a, a)); }
inline float length(vec2 a) { return sqrt(dot(a, a)); }
inline vec3 normalize(vec3 a) { return a / length(a); }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
inline float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
