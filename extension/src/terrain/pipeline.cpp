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
		else if (key == "lipschitz") out->lipschitz_ceiling = float(std::atof(rest.c_str()));
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
// Every "P.<ident>" token in a stage body, with // comments stripped first so a name
// mentioned in prose is not a read. A preceding identifier character means this is the
// tail of a longer name (XP.foo), not a params access.
std::vector<std::string> param_reads(const std::string &body) {
	std::vector<std::string> out;
	std::string src;
	src.reserve(body.size());
	for (size_t i = 0; i < body.size();) {
		if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/') {
			while (i < body.size() && body[i] != '\n') i++;
		} else {
			src += body[i++];
		}
	}
	auto ident_char = [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_';
	};
	for (size_t i = 0; i + 1 < src.size(); i++) {
		if (src[i] != 'P' || src[i + 1] != '.') continue;
		if (i > 0 && ident_char(src[i - 1])) continue;
		size_t j = i + 2;
		while (j < src.size() && ident_char(src[j])) j++;
		if (j > i + 2) out.push_back(src.substr(i + 2, j - i - 2));
		i = j - 1;
	}
	return out;
}

// "hills.amp_a" -> "hills_amp_a", the way generate_field_glslh flattens it into the UBO.
std::string flat_ident(const std::string &dotted) {
	std::string s = dotted;
	for (char &c : s) if (c == '.') c = '_';
	return s;
}

void hash_feed(uint64_t &h, const std::string &s) {
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
}

// Every contributor, so a rejected pipeline tells the artist which stage to budget rather
// than just that the total is too large.
std::string bound_report(float lip, float ceiling, const std::vector<StageManifest> &st) {
	std::string s = "pipeline gradient bound " + std::to_string(lip) +
			" exceeds the declared ceiling " + std::to_string(ceiling) +
			"; raise the ceiling only if the raymarcher can afford the steps:";
	for (const StageManifest &m : st) {
		if (m.lipschitz_mode == LipschitzMode::kNone) continue;
		s += "\n  " + m.name + ": " +
				(m.lipschitz_mode == LipschitzMode::kAdd ? "add " : "mul ") +
				std::to_string(m.lipschitz);
	}
	return s;
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
	// Starts at zero: the first sdf writer establishes the field and adds its own bound.
	float lip = 0.0f;
	bool seen_sdf_writer = false;
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
			// allow_gpu_only is a deliberate opt-in, so this is not an error -- but it
			// stops being silent. Everything that evaluates the field on the CPU will
			// disagree with what the player sees.
			out->warnings.push_back("stage '" + m.name + "' has no //!cpu mirror, so these "
					"CPU consumers will diverge from the rendered field: collider meshing, "
					"island extraction, raycast, and consolidation");
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

		bool writes_sdf = false;
		for (const ChannelDecl &w : m.writes)
			if (w.name == "sdf") writes_sdf = true;
		for (const auto &ov : desc.stages[i].param_overrides) {
			bool found = false;
			for (const ParamDecl &p : m.params)
				if (p.name == ov.first) { found = true; break; }
			if (!found)
				return fail("stage '" + m.name + "' has no param '" + ov.first + "'");
		}
		if (writes_sdf && !desc.stages[i].param_overrides.empty())
			return fail("stage '" + m.name + "' has parameter overrides that may change its "
					"gradient bound; static //!lipschitz is not recomputed");
		for (const auto &ov : desc.stages[i].param_overrides)
			for (ParamDecl &p : m.params)
				if (p.name == ov.first) { p.value = ov.second; break; }
		for (const ParamDecl &p : m.params) {
			ParamDecl flat = p;
			flat.name = m.name + "." + p.name;
			out->params.push_back(flat);
		}

		if (writes_sdf) {
			if (m.lipschitz_mode == LipschitzMode::kNone)
				return fail("stage '" + m.name + "' writes sdf but declares no "
						"//!lipschitz <add|mul> <n>; an unbounded field tunnels the raymarcher");
			if (!seen_sdf_writer && m.lipschitz_mode != LipschitzMode::kAdd)
				return fail("stage '" + m.name + "' is the first stage to write sdf, so its "
						"//!lipschitz mode must be 'add': it establishes the field, and "
						"multiplying a zero bound is not a bound");
			lip = m.lipschitz_mode == LipschitzMode::kAdd ? lip + m.lipschitz
			                                              : lip * m.lipschitz;
			seen_sdf_writer = true;
		} else if (m.lipschitz_mode != LipschitzMode::kNone) {
			return fail("stage '" + m.name + "' declares //!lipschitz but writes no sdf, so it "
					"cannot move the gradient of the distance field");
		}
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

	// Cross-stage parameter reads must be declared. The GLSL reads another stage's param
	// through the flattened ident (P.hills_amp_a) and the resolver used not to see that at
	// all -- so overriding hills.amp_a in a pipeline file silently diverged the CPU mirror,
	// which had the old value as a literal. Declaring the read is what carries the value
	// into the mirror's blob.
	for (size_t i = 0; i < out->stages.size(); i++) {
		const StageManifest &m = out->stages[i];
		for (const std::string &use : m.uses) {
			bool found = false;
			for (const ParamDecl &pd : out->params)
				if (pd.name == use) { found = true; break; }
			if (!found)
				return fail("stage '" + m.name + "' declares //!use " + use +
						", but no stage in this pipeline declares that param");
		}
		for (const std::string &read : param_reads(m.body)) {
			bool ok = false;
			for (const ParamDecl &pd : m.params)
				if (flat_ident(m.name + "." + pd.name) == read) { ok = true; break; }
			for (const std::string &use : m.uses)
				if (flat_ident(use) == read) { ok = true; break; }
			if (!ok)
				return fail("stage '" + m.name + "' reads P." + read +
						", which is neither its own param nor a declared //!use");
		}
	}

	// Sorted so set-1 binding indices are a pure function of the resource names, which keeps
	// the generated GLSL stable and diffable across unrelated pipeline edits.
	for (size_t a = 0; a + 1 < out->resources.size(); a++)
		for (size_t b = a + 1; b < out->resources.size(); b++)
			if (out->resources[b].name < out->resources[a].name)
				std::swap(out->resources[a], out->resources[b]);

	out->lipschitz = lip;
	if (desc.lipschitz_ceiling > 0.0f && lip > desc.lipschitz_ceiling)
		return fail(bound_report(lip, desc.lipschitz_ceiling, out->stages));
	hash_feed(h, std::to_string(desc.seed));
	out->hash = h;
	return true;
}

} // namespace ve
