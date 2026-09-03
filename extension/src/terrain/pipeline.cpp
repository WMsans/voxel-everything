#include "terrain/pipeline.h"
#include <cstdlib>
#include <sstream>

namespace ve {
namespace {
std::string trim(const std::string &s) {
	const size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) return "";
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}
bool is_indented(const std::string &line) {
	return !line.empty() && (line[0] == ' ' || line[0] == '\t');
}
} // namespace

bool parse_pipeline_desc(const std::string &source, PipelineDesc *out, std::string *error) {
	*out = PipelineDesc{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::istringstream in(source);
	std::string line;
	while (std::getline(in, line)) {
		const bool indented = is_indented(line);
		const std::string t = trim(line);
		if (t.empty() || t[0] == '#') continue;

		const size_t sp = t.find_first_of(" \t");
		const std::string key = sp == std::string::npos ? t : t.substr(0, sp);
		const std::string rest = sp == std::string::npos ? "" : trim(t.substr(sp));

		// Indentation is the grammar: an indented line is a param override on the stage
		// above it. This keeps a pipeline readable as a list without needing a nested
		// block syntax or a real file format.
		if (indented) {
			if (out->stages.empty()) return fail("param override before any stage: " + key);
			out->stages.back().param_overrides.emplace_back(key, float(std::atof(rest.c_str())));
			continue;
		}
		if (key == "seed") out->seed = uint32_t(std::strtoul(rest.c_str(), nullptr, 10));
		else if (key == "lipschitz") out->lipschitz_override = float(std::atof(rest.c_str()));
		else if (key == "allow_gpu_only") out->allow_gpu_only = std::atoi(rest.c_str()) != 0;
		else if (key == "stage") {
			if (rest.empty()) return fail("stage needs a path");
			PipelineStageRef r; r.path = rest;
			out->stages.push_back(r);
		}
		else return fail("unknown pipeline key: " + key);
	}
	if (out->stages.empty()) return fail("pipeline declares no stage");
	return true;
}

} // namespace ve
