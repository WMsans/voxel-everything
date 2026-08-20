#include "render/shader_loader.h"
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace ve {

namespace {
std::mutex g_shader_override_mutex;
std::map<std::string, std::string> g_shader_overrides;

bool find_override(const std::string &path, std::string *out) {
	std::lock_guard<std::mutex> lock(g_shader_override_mutex);
	auto it = g_shader_overrides.find(path);
	if (it == g_shader_overrides.end()) {
		const size_t slash = path.find_last_of("/\\");
		const std::string base = slash == std::string::npos
				? path : path.substr(slash + 1);
		it = g_shader_overrides.find(base);
	}
	if (it == g_shader_overrides.end()) return false;
	if (out) *out = it->second;
	return true;
}
} // namespace

// `stack` is the ancestry of the file being expanded and detects cycles; `included` is every
// file already emitted anywhere and gives `#pragma once` semantics. Both are needed: common
// headers are pulled in down several paths (a diamond, which must expand once and is not an
// error) while a true cycle must still be reported rather than silently truncated.
static bool expand(const std::string &path, const std::string &include_dir,
		std::set<std::string> &stack, std::set<std::string> &included,
		std::ostringstream &out, std::string *error) {
	if (stack.count(path)) {
		if (error) *error = "include cycle at " + path;
		return false;
	}
	if (included.count(path)) return true; // already emitted: skip, not an error

	std::string content;
	std::string override_source;
	if (find_override(path, &override_source)) {
		content = std::move(override_source);
	} else {
		std::ifstream f(path);
		if (!f) {
			if (error) *error = "cannot open " + path;
			return false;
		}
		content.assign(std::istreambuf_iterator<char>(f),
				std::istreambuf_iterator<char>());
	}

	stack.insert(path);
	included.insert(path);
	std::istringstream in(content);
	std::string line;
	while (std::getline(in, line)) {
		const std::string key = "#include \"";
		const auto pos = line.find(key);
		if (pos != std::string::npos) {
			const auto end = line.find('"', pos + key.size());
			// Fail-soft: never let a malformed directive escape as an exception
			// (std::out_of_range from substr) or as a misleading "cannot open".
			// (M1 regression test; kept alongside the include-once semantics.)
			if (end == std::string::npos) {
				if (error) *error = "malformed include in " + path;
				return false;
			}
			const std::string name = line.substr(pos + key.size(), end - pos - key.size());
			if (!expand(include_dir + "/" + name, include_dir, stack, included, out, error))
				return false;
		} else {
			out << line << '\n';
		}
	}
	stack.erase(path);
	return true;
}

std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error) {
	std::set<std::string> stack, included;
	std::ostringstream out;
	if (!expand(path, include_dir, stack, included, out, error)) return "";
	return out.str();
}

std::string strip_shader_annotations(const std::string &src) {
	std::istringstream in(src);
	std::ostringstream out;
	std::string line;
	while (std::getline(in, line)) {
		const size_t first = line.find_first_not_of(" \t\r");
		const bool annotation = first != std::string::npos && line[first] == '#' &&
				first + 1 < line.size() && line[first + 1] == '[';
		if (!annotation) out << line << '\n';
	}
	return out.str();
}

void set_shader_source_override(const std::string &key, const std::string &source) {
	std::lock_guard<std::mutex> lock(g_shader_override_mutex);
	g_shader_overrides[key] = source;
}

void clear_shader_source_override(const std::string &key) {
	std::lock_guard<std::mutex> lock(g_shader_override_mutex);
	g_shader_overrides.erase(key);
}

void clear_shader_source_overrides() {
	std::lock_guard<std::mutex> lock(g_shader_override_mutex);
	g_shader_overrides.clear();
}

} // namespace ve
