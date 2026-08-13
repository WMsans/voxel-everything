#include "render/shader_loader.h"
#include <fstream>
#include <set>
#include <sstream>

namespace ve {

static bool expand(const std::string &path, const std::string &include_dir,
		std::set<std::string> &stack, std::ostringstream &out, std::string *error) {
	if (stack.count(path)) {
		if (error) *error = "include cycle at " + path;
		return false;
	}
	std::ifstream f(path);
	if (!f) {
		if (error) *error = "cannot open " + path;
		return false;
	}
	stack.insert(path);
	std::string line;
	while (std::getline(f, line)) {
		const std::string key = "#include \"";
		const auto pos = line.find(key);
		if (pos != std::string::npos) {
			const auto end = line.find('"', pos + key.size());
			if (end == std::string::npos) {
				// Fail-soft: never let a malformed directive escape as an exception
				// (std::out_of_range from substr) or as a misleading "cannot open".
				if (error) *error = "malformed include in " + path;
				return false;
			}
			const std::string name = line.substr(pos + key.size(), end - pos - key.size());
			if (!expand(include_dir + "/" + name, include_dir, stack, out, error)) return false;
		} else {
			out << line << '\n';
		}
	}
	stack.erase(path);
	return true;
}

std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error) {
	std::set<std::string> stack;
	std::ostringstream out;
	if (!expand(path, include_dir, stack, out, error)) return "";
	return out.str();
}

} // namespace ve
