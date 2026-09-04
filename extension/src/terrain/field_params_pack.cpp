#include "terrain/field_params_pack.h"

#include <cstring>

namespace ve {

std::vector<uint8_t> pack_field_params_bytes(const ResolvedPipeline &p) {
	std::vector<uint8_t> bytes(p.params.empty() ? 16 : p.params.size() * 16, 0);
	for (size_t i = 0; i < p.params.size(); i++) {
		float v = p.params[i].value;
		std::memcpy(bytes.data() + i * 4, &v, 4);
	}
	return bytes;
}

} // namespace ve
