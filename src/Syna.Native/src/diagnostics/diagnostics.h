#pragma once

#include "syna_api.h"

#include <string>

namespace syna {

// Adapter, D3D11 hardware decode profiles, encoders that actually open, FFmpeg version/license.
std::string system_info_json();

// CPU/memory of this process and its GPU engine utilization (PDH "GPU Engine" counters).
// Utilization needs two samples; the first call reports 0. Takes a few ms: not for the UI thread.
syna_result sample_process_stats(syna_process_stats& out);

}  // namespace syna
