#include <doctest/doctest.h>
#include "render/shader_loader.h"
#include <filesystem>
#include <fstream>

static std::filesystem::path write_file(const std::filesystem::path &dir,
		const std::string &name, const std::string &content) {
	std::filesystem::create_directories(dir);
	auto path = dir / name;
	std::ofstream f(path);
	f << content;
	return path;
}

TEST_CASE("loads plain file unchanged") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl1";
	auto p = write_file(dir, "a.glsl", "#version 460\nvoid main() {}\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(err.empty());
	CHECK(out == "#version 460\nvoid main() {}\n");
}

TEST_CASE("expands includes inline, recursively") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl2";
	write_file(dir, "common.glsl", "const float X = 1.0;\n");
	write_file(dir, "inner.glsl", "#include \"common.glsl\"\nconst float Y = X;\n");
	auto p = write_file(dir, "main.glsl", "#version 460\n#include \"inner.glsl\"\nvoid main() {}\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(err.empty());
	CHECK(out.find("const float X = 1.0;") != std::string::npos);
	CHECK(out.find("const float Y = X;") != std::string::npos);
	CHECK(out.find("#include") == std::string::npos);
}

TEST_CASE("include cycle reports error") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl3";
	write_file(dir, "a.glsl", "#include \"b.glsl\"\n");
	auto p = write_file(dir, "b.glsl", "#include \"a.glsl\"\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(out.empty());
	CHECK(err.find("cycle") != std::string::npos);
}

TEST_CASE("missing file reports error") {
	std::string err;
	auto out = ve::load_shader_source("/nonexistent/x.glsl", "/nonexistent", &err);
	CHECK(out.empty());
	CHECK_FALSE(err.empty());
}

TEST_CASE("malformed include (missing closing quote) reports error") {
	// Regression: expand() used to run line.substr(pos+key.size(), end-pos-key.size())
	// with end == npos, which either threw std::out_of_range (process abort, violating
	// the fail-soft policy) or misreported the include as "cannot open". It must report
	// a clean "malformed include" error instead.
	auto dir = std::filesystem::temp_directory_path() / "ve_sl4";
	auto p = write_file(dir, "broken.glsl", "#include \"broken\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(out.empty());
	CHECK(err.find("malformed") != std::string::npos);
}
