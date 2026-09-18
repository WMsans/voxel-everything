#include "terrain/pipeline_load.h"

#include <vector>

#include "terrain/stage_manifest.h"

namespace ve {

bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out,
		std::vector<std::string> *warnings, std::string *error) {
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::string src;
	if (!reader(pipeline_path, &src)) return fail("cannot read " + pipeline_path);

	PipelineDesc desc;
	if (!parse_pipeline_desc(src, &desc, error)) return false;

	std::vector<StageManifest> loaded;
	for (const PipelineStageRef &r : desc.stages) {
		const std::string path = stage_root + r.path;
		std::string stage_src;
		if (!reader(path, &stage_src)) return fail("cannot read " + path);
		StageManifest m;
		if (!parse_stage_manifest(stage_src, &m, error)) {
			// The parser's message names the directive, not the file; prefix it so a
			// pipeline with a dozen stages says which one is broken.
			if (error) *error = path + ": " + *error;
			return false;
		}
		loaded.push_back(m);
	}

	if (!resolve_pipeline(desc, loaded, out, error)) return false;
	if (warnings != nullptr) *warnings = out->warnings;
	return true;
}

} // namespace ve
