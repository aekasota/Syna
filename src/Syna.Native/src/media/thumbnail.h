#pragma once

#include "syna_api.h"

#include <cstdint>
#include <string>

namespace syna {

// Decodes the keyframe at or before at_us (software decoder, keyframes only) and writes a JPEG
// scaled to max_width. at_us < 0 picks a representative moment (10% in, at most 3 s).
syna_result make_thumbnail(const std::wstring& path, const std::wstring& out_jpeg, int max_width, int64_t at_us);

}  // namespace syna
