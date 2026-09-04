#include "terrain/stage_manifest.h"
#include <cstdlib>
#include <sstream>

namespace ve {
namespace {

// Trims ASCII space and tab from both ends. The directive grammar is whitespace-insensitive
// so that manifests can be column-aligned for readability.
std::string trim(const std::string &s) {
	const size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) return "";
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

bool parse_channel_type(const std::string &s, ChannelType *out) {
	if (s == "float") { *out = ChannelType::kFloat; return true; }
	if (s == "vec2")  { *out = ChannelType::kVec2;  return true; }
	if (s == "vec3")  { *out = ChannelType::kVec3;  return true; }
	if (s == "vec4")  { *out = ChannelType::kVec4;  return true; }
	if (s == "int")   { *out = ChannelType::kInt;   return true; }
	if (s == "uint")  { *out = ChannelType::kUint;  return true; }
	return false;
}

// Splits "name : type" into its halves. Returns false when the colon is missing.
bool split_typed(const std::string &rest, std::string *name, std::string *type) {
	const size_t colon = rest.find(':');
	if (colon == std::string::npos) return false;
	*name = trim(rest.substr(0, colon));
	*type = trim(rest.substr(colon + 1));
	return !name->empty() && !type->empty();
}

} // namespace

bool parse_stage_manifest(const std::string &source, StageManifest *out, std::string *error) {
	*out = StageManifest{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::istringstream in(source);
	std::string line;
	std::ostringstream body;
	bool kind_seen = false;
	while (std::getline(in, line)) {
		const std::string t = trim(line);
		if (t.rfind("//!", 0) != 0) { body << line << '\n'; continue; }
		const std::string d = trim(t.substr(3));
		const size_t sp = d.find_first_of(" \t");
		const std::string key = sp == std::string::npos ? d : d.substr(0, sp);
		const std::string rest = sp == std::string::npos ? "" : trim(d.substr(sp));

		if (key == "stage") { out->name = rest; }
		else if (key == "kind") {
			if (rest == "field") out->kind = StageKind::kField;
			else if (rest == "map") out->kind = StageKind::kMap;
			else return fail("unknown //!kind: " + rest);
			kind_seen = true;
		}
		else if (key == "in" || key == "out") {
			std::string n, ty;
			if (!split_typed(rest, &n, &ty)) return fail("//!" + key + " needs 'name : type'");
			// A dotted name is a map-stage resource reference (scope.name), not a FieldCtx
			// channel. Per spec section 4 only //!sample declares resources, so accept and
			// validate the syntax here but record nothing; Plan B will add resource
			// read/write lists. (Plan A rejects map stages at resolve time, so this is
			// unobservable downstream.)
			if (n.find('.') != std::string::npos) {
			} else {
				ChannelType ct;
				if (!parse_channel_type(ty, &ct)) return fail("unknown channel type: " + ty);
				ChannelDecl c; c.name = n; c.type = ct;
				(key == "in" ? out->reads : out->writes).push_back(c);
			}
		}
		else if (key == "sample") {
			std::string n, ty;
			if (!split_typed(rest, &n, &ty)) return fail("//!sample needs 'name : type'");
			ResourceDecl r; r.name = n; r.type = ty;
			out->samples.push_back(r);
		}
		else if (key == "param") {
			const size_t eq = rest.find('=');
			if (eq == std::string::npos) return fail("//!param needs a default: " + rest);
			std::string n, ty;
			if (!split_typed(trim(rest.substr(0, eq)), &n, &ty))
				return fail("//!param needs 'name : type = default'");
			ChannelType ct;
			if (!parse_channel_type(ty, &ct)) return fail("unknown param type: " + ty);
			ParamDecl p; p.name = n; p.type = ct;
			p.value = float(std::atof(trim(rest.substr(eq + 1)).c_str()));
			out->params.push_back(p);
		}
		else if (key == "lipschitz") { out->lipschitz = float(std::atof(rest.c_str())); }
		else if (key == "bounds")    { out->bounds = float(std::atof(rest.c_str())); }
		else if (key == "iterate")   { out->iterate = std::atoi(rest.c_str()); }
		else if (key == "cpu")       { out->cpu_symbol = rest; }
		else if (key == "domain") {
			const size_t sp2 = rest.find_first_of(" \t");
			out->domain = sp2 == std::string::npos ? rest : rest.substr(0, sp2);
			if (sp2 != std::string::npos) {
				const std::string ext = trim(rest.substr(sp2));
				const size_t x = ext.find('x');
				if (x == std::string::npos) return fail("//!domain extent needs WxH: " + ext);
				out->domain_w = std::atoi(ext.substr(0, x).c_str());
				out->domain_h = std::atoi(ext.substr(x + 1).c_str());
			}
		}
		else return fail("unknown directive: //!" + key);
	}

	if (out->name.empty()) return fail("missing //!stage <name>");
	if (!kind_seen) return fail("missing //!kind field|map");
	if (out->iterate < 1) return fail("//!iterate must be >= 1");
	out->body = body.str();
	return true;
}

int channel_component_count(ChannelType t) {
	switch (t) {
		case ChannelType::kVec2: return 2;
		case ChannelType::kVec3: return 3;
		case ChannelType::kVec4: return 4;
		default: return 1;
	}
}

const char *channel_glsl_type(ChannelType t) {
	switch (t) {
		case ChannelType::kVec2: return "vec2";
		case ChannelType::kVec3: return "vec3";
		case ChannelType::kVec4: return "vec4";
		case ChannelType::kInt:  return "int";
		case ChannelType::kUint: return "uint";
		default: return "float";
	}
}

} // namespace ve
