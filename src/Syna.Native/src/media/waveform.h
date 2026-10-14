#pragma once

#include "jobs/job.h"

#include <string>

namespace syna {

// Decodes every audio track (video is skipped at the demuxer, so its data is not even read) and
// writes peak/RMS envelopes aligned to the file timeline.
//
// File layout (little-endian):
//   char[4] "SYNW", u32 version (1), u32 track_count, u32 buckets_per_second, i64 duration_us,
//   per track: u32 bucket_count, then bucket_count x { u16 peak, u16 rms } (linear, 65535 = 0 dBFS).
syna_result extract_waveform(const std::wstring& path, const std::wstring& out_file, int buckets_per_second, Job& job);

}  // namespace syna
