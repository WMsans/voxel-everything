#include "terrain/stage_library.h"

namespace ve {

std::vector<std::string> split_binding_names(const std::string &list) {
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&]() {
		const size_t b = cur.find_first_not_of(" \t");
		if (b == std::string::npos) { cur.clear(); return; }
		out.push_back(cur.substr(b, cur.find_last_not_of(" \t") - b + 1));
		cur.clear();
	};
	for (char c : list) {
		if (c == ',') flush();
		else cur += c;
	}
	flush();
	return out;
}

StageLibrary &StageLibrary::instance() {
	// Function-local static: registration happens from other translation units' static
	// initializers, and this is the standard way to dodge the static init order fiasco.
	static StageLibrary lib;
	return lib;
}

void StageLibrary::register_stage(const StageBinding &b) {
	for (auto &e : entries_)
		if (e.symbol == b.symbol) { e = b; return; }
	entries_.push_back(b);
}

const StageBinding *StageLibrary::lookup(const std::string &symbol) const {
	for (const auto &e : entries_)
		if (e.symbol == symbol) return &e;
	return nullptr;
}

StageRegistrar::StageRegistrar(const char *symbol, StageFn fn, const char *slot_names,
		const char *param_names) {
	StageBinding b;
	b.symbol = symbol;
	b.fn = fn;
	b.slot_names = slot_names;
	b.param_names = param_names;
	StageLibrary::instance().register_stage(b);
}

} // namespace ve
