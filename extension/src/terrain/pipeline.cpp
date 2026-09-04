#include "terrain/pipeline.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

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

int ResolvedPipeline::channel_slot(const std::string &name) const {
	for (size_t i = 0; i < channels.size(); i++)
		if (channels[i].name == name) return int(i);
	return -1;
}

namespace {
void hash_feed(uint64_t &h, const std::string &s) {
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
}
} // namespace

bool resolve_pipeline(const PipelineDesc &desc, const std::vector<StageManifest> &loaded,
		ResolvedPipeline *out, std::string *error) {
	*out = ResolvedPipeline{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };
	if (loaded.size() != desc.stages.size())
		return fail("manifest count does not match pipeline stage count");

	// Built-ins occupy slots 0..2 so that every pipeline agrees on them, which is what lets
	// the CPU mirror and the generated GLSL index the context identically.
	out->channels.push_back({"p", ChannelType::kVec3, 0});
	out->channels.push_back({"sdf", ChannelType::kFloat, 1});
	out->channels.push_back({"material", ChannelType::kUint, 2});

	bool wrote_sdf = false;
	float lip = 1.0f;
	uint64_t h = 1469598103934665603ull;

	for (size_t i = 0; i < loaded.size(); i++) {
		StageManifest m = loaded[i];
		if (m.kind != StageKind::kField)
			return fail("stage '" + m.name + "' is a map stage; Plan A resolves field stages only");
		for (size_t j = 0; j < i; j++)
			if (loaded[j].name == m.name) return fail("duplicate stage name: " + m.name);
		if (m.cpu_symbol.empty()) {
			if (!desc.allow_gpu_only)
				return fail("stage '" + m.name + "' has no //!cpu mirror; set allow_gpu_only "
						"to accept a GPU-authoritative field");
			out->cpu_exact = false;
		}

		for (const ChannelDecl &r : m.reads) {
			const int slot = out->channel_slot(r.name);
			if (slot < 0)
				return fail("stage '" + m.name + "' reads channel '" + r.name +
						"' that no earlier stage writes");
			if (out->channels[size_t(slot)].type != r.type)
				return fail("stage '" + m.name + "' reads channel '" + r.name +
						"' at a conflicting type");
		}
		for (const ChannelDecl &w : m.writes) {
			const int slot = out->channel_slot(w.name);
			if (slot < 0) {
				out->channels.push_back({w.name, w.type, int(out->channels.size())});
			} else if (out->channels[size_t(slot)].type != w.type) {
				return fail("stage '" + m.name + "' writes channel '" + w.name +
						"' at a conflicting type");
			}
			if (w.name == "sdf") wrote_sdf = true;
		}

		for (const ResourceDecl &r : m.samples) {
			bool seen = false;
			for (const ResourceDecl &e : out->resources) {
				if (e.name != r.name) continue;
				if (e.type != r.type)
					return fail("resource '" + r.name + "' declared at two types");
				seen = true;
				break;
			}
			if (!seen) out->resources.push_back(r);
		}

		for (const auto &ov : desc.stages[i].param_overrides) {
			bool found = false;
			for (ParamDecl &p : m.params)
				if (p.name == ov.first) { p.value = ov.second; found = true; break; }
			if (!found)
				return fail("stage '" + m.name + "' has no param '" + ov.first + "'");
		}
		for (const ParamDecl &p : m.params) {
			ParamDecl flat = p;
			flat.name = m.name + "." + p.name;
			out->params.push_back(flat);
		}

		lip *= (m.lipschitz > 0.0f ? m.lipschitz : 1.0f);
		hash_feed(h, m.name);
		hash_feed(h, m.body);
		for (const ParamDecl &p : m.params) {
			hash_feed(h, p.name);
			hash_feed(h, std::to_string(p.value));
		}
		out->stages.push_back(m);
	}

	if (!wrote_sdf)
		return fail("pipeline never writes channel 'sdf'; the final field stage must produce one");

	// Sorted so set-1 binding indices are a pure function of the resource names, which keeps
	// the generated GLSL stable and diffable across unrelated pipeline edits.
	for (size_t a = 0; a + 1 < out->resources.size(); a++)
		for (size_t b = a + 1; b < out->resources.size(); b++)
			if (out->resources[b].name < out->resources[a].name)
				std::swap(out->resources[a], out->resources[b]);

	out->lipschitz = desc.lipschitz_override > 0.0f ? desc.lipschitz_override : lip;
	hash_feed(h, std::to_string(desc.seed));
	out->hash = h;
	return true;
}

} // namespace ve
