#include "terrain/stage_library.h"

namespace ve {

StageLibrary &StageLibrary::instance() {
	// Function-local static: registration happens from other translation units' static
	// initializers, and this is the standard way to dodge the static init order fiasco.
	static StageLibrary lib;
	return lib;
}

void StageLibrary::register_stage(const std::string &symbol, StageFn fn) {
	for (auto &e : entries_)
		if (e.first == symbol) { e.second = fn; return; }
	entries_.emplace_back(symbol, fn);
}

StageFn StageLibrary::lookup(const std::string &symbol) const {
	for (const auto &e : entries_)
		if (e.first == symbol) return e.second;
	return nullptr;
}

StageRegistrar::StageRegistrar(const char *symbol, StageFn fn) {
	StageLibrary::instance().register_stage(symbol, fn);
}

} // namespace ve
