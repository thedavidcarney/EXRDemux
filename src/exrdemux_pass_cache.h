// Process-wide cache of decoded EXR passes, shared by every EXRDemux
// instance in the process.
//
// A chase comp can stack hundreds of EXRDemux layers over a few dozen
// passes of one multipart EXR. Without sharing, every layer fetches the
// full-frame pass on every frame. Here each (file, pass) is decoded once,
// straight from the file on disk, and every layer and frame that needs it
// reads the same copy.
//
// Freshness: the file's mtime + size are checked on every acquire. When
// the file is re-rendered in place, every cached pass of that file is
// dropped and the next acquire decodes the new file. The copy is decoded
// from the file itself (never taken from AE), so it can't hold pixels
// older than what's on disk. A file that is still being written (stamp
// changes during the read, or modified in the last couple of seconds) is
// not cached; callers then render the uncached way.
//
// No AE dependencies, so the bench tool can link it directly.

#pragma once

#include <Imath/half.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace exrdemux {

// One pass decoded to half floats, interleaved A,R,G,B per pixel (the
// channel-suite chunk's order). Missing channels hold the same defaults the
// file-direct path always used: R/G/B = 0, A = 1.
struct PassPixels {
    int width = 0, height = 0;        // data window size
    int offset_x = 0, offset_y = 0;   // data window min - display window min
    std::vector<half> argb;

    size_t bytes() const { return argb.size() * sizeof(half); }
};

// Which part and channels hold the pass (from the file's metadata).
struct PassChannels {
    int part_idx = -1;
    std::string r_name, g_name, b_name, a_name;
    bool has_r = false, has_g = false, has_b = false, has_a = false;
};

enum class PassResult {
    kHit,          // served from cache
    kDecoded,      // decoded now, cached
    kDisabled,     // cache off (EXRDEMUX_PASS_CACHE=0) or budget 0
    kUnstable,     // file changing / just written / unstat-able — not cached
    kDecodeError,  // decoder threw for this file version (remembered)
};

const char* PassResultName(PassResult r);

// Returns the decoded pass, or nullptr when it can't come from the cache
// (see `result_out`); the caller then renders its uncached way.
// `decoder_error_out` gets the decoder's message on kDecodeError.
std::shared_ptr<const PassPixels> AcquirePass(
    const std::string& path,
    const std::string& layer_name,
    const PassChannels& channels,
    PassResult* result_out = nullptr,
    std::string* decoder_error_out = nullptr);

struct PassCacheStats {
    uint64_t hits = 0, decodes = 0, evictions = 0, invalidations = 0;
    size_t   bytes = 0, entries = 0, budget = 0, peak_bytes = 0;
};
PassCacheStats GetPassCacheStats();

// Default budget: EXRDEMUX_CACHE_MB if set (0 disables), else 8 GB.
// EXRDEMUX_PASS_CACHE=0 also disables. Tests may override.
void   SetPassCacheBudget(size_t bytes);
size_t PassCacheBudget();

// Seconds a file must sit unmodified before its passes are cached.
// Default 2. Tests set 0.
void SetPassCacheSettleSeconds(double seconds);

void ClearPassCache();

}  // namespace exrdemux
