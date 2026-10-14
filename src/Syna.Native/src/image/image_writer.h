#pragma once

#include "syna_api.h"

#include <cstdint>
#include <string>

namespace syna {

// Encodes tightly packed or strided BGRA pixels to PNG or JPEG with WIC. Writes to a temp file and
// renames, so a failed encode never leaves a truncated image behind.
syna_result write_image(const std::wstring& path, const uint8_t* bgra, int width, int height, int stride, bool jpeg, float quality);

}  // namespace syna
