// A varying that carries a BIT PATTERN must be `flat`.
//
// leaf.vert.glsl passed the per-clump hash as a smooth `float` and leaf.frag.glsl ran
// floatBitsToUint() on it. The interpolator does not hand back the vertex value: it
// recomputes a perspective-weighted sum per fragment, landing a ULP away and drifting as the
// billboard re-faces the camera. One ULP is invisible in a quantity and total in a hash --
// tree_hash(h) resolved to a different tint every frame, which is the leaf colour flicker.
//
// This is a class of bug, not one shader's slip, so the check scans every fragment shader
// rather than pinning leaf.frag.glsl: any varying whose bits are reinterpreted has to be
// declared flat on the fragment side.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path &p) {
	std::ifstream f(p);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

// "layout(location = 5) in flat uint v_hash;" -> is v_hash flat?
bool declared_flat(const std::string &src, const std::string &name) {
	const std::regex decl("\\bin\\s+(flat\\s+)?\\w+\\s+" + name + "\\s*;");
	std::smatch m;
	if (!std::regex_search(src, m, decl)) return false; // not a varying at all
	return m[1].matched;
}

} // namespace

TEST_CASE("no fragment shader reinterprets the bits of a smooth varying") {
	const std::filesystem::path shaders = std::filesystem::path(VE_REPO_ROOT) / "shaders";
	REQUIRE(std::filesystem::is_directory(shaders));

	const std::regex bit_cast("(?:floatBitsToUint|floatBitsToInt)\\s*\\(\\s*(v_\\w+)");
	int scanned = 0;
	for (const auto &entry : std::filesystem::directory_iterator(shaders)) {
		const std::string name = entry.path().filename().string();
		if (name.size() < 11 || name.compare(name.size() - 10, 10, ".frag.glsl") != 0) continue;
		scanned++;
		const std::string src = read_file(entry.path());
		for (std::sregex_iterator it(src.begin(), src.end(), bit_cast), end; it != end; ++it) {
			const std::string varying = (*it)[1].str();
			INFO(name << " reinterprets the bits of smooth varying " << varying);
			CHECK(declared_flat(src, varying));
		}
	}
	CHECK(scanned > 0); // a glob that matches nothing must not pass silently
}
