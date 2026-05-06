// EXRDemux — scriptable multilayer EXR plugin for Adobe After Effects.
//
// Architecture note (learned during Phase 2):
//
// AE's plugin lifecycle does NOT make the source layer's footage path
// available at PF_Cmd_PARAMS_SETUP. AEGP_GetEffectLayer returns null
// there. The SDK samples that use it (PathMaster, Resizer, SmartyPants)
// only call it during render-time selectors. AE's popup string list is
// also locked once PARAMS_SETUP returns — there is no API to update it.
//
// So the popup necessarily uses generic slot labels ("Layer 1", "Layer 2",
// ...). The user picks a slot; the plugin's "real" persistence is the
// hidden Layer Hash (Phase 3): when the popup changes, plugin captures the
// channel name at that slot from the current EXR, hashes it, and stores
// the hash. At render time we resolve hash -> name -> current channel
// index, so re-renders with shifted channels still hit the right pixels.
//
// Param layout:
//   [0] PARAM_INPUT  - implicit input layer (counted but not added)
//   [1] PARAM_LAYER  - popup of generic slot labels (locked at PARAMS_SETUP)
//   [2] PARAM_HASH   - hidden float slider; stores 32-bit hash of channel
//                      name (set in Phase 3, used at render in Phase 4)
//   [3] PARAM_BLACK  - Black Point (rescale floor)
//   [4] PARAM_WHITE  - White Point (rescale ceiling)
//   [5] PARAM_UNMULT - UnMult premultiplied alpha
//
// Param layout:
//   [0] PARAM_INPUT  - implicit input layer (counted but not added)
//   [1] PARAM_LAYER  - popup of layer names from the source EXR
//   [2] PARAM_HASH   - hidden float slider; stores 32-bit hash of channel name
//                      so selection survives EXR re-renders with shifted indices
//   [3] PARAM_BLACK  - Black Point (rescale floor)
//   [4] PARAM_WHITE  - White Point (rescale ceiling)
//   [5] PARAM_UNMULT - UnMult premultiplied alpha

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"
#include "AE_EffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectUI.h"
#include "AE_GeneralPlug.h"
#include "AE_ChannelSuites.h"
#include "Param_Utils.h"
#include "entry.h"
#include "AEFX_SuiteHandlerTemplate.h"
#include "AEFX_SuiteHelper.h"
#include "AEGP_SuiteHandler.h"

#include "exrdemux_dialog.h"

#include <Imath/half.h>
#include <ImfMultiPartInputFile.h>
#include <ImfInputPart.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImathBox.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include <set>

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif
#ifdef AE_OS_MAC
    #include <CoreFoundation/CoreFoundation.h>
#endif

#define EXRDEMUX_MAJOR_VERSION 0
#define EXRDEMUX_MINOR_VERSION 1
#define EXRDEMUX_BUG_VERSION   0
#define EXRDEMUX_STAGE_VERSION PF_Stage_DEVELOP
#define EXRDEMUX_BUILD_VERSION 1

namespace {

enum {
    PARAM_INPUT = 0,
    PARAM_LAYER,
    PARAM_PICK_BUTTON,
    PARAM_HASH_HI,
    PARAM_HASH_LO,
    PARAM_BLACK,
    PARAM_WHITE,
    PARAM_UNMULT,
    PARAM_COUNT
};

// Param IDs (must be unique and stable across plugin versions for
// project file compatibility). Adding new params must use a fresh ID.
//
// The hash is split into two 16-bit halves because PF_Param_FLOAT_SLIDER
// round-trips through 32-bit float somewhere in AE's persistence path,
// and integers above ~16M lose precision. 16-bit halves stay exact.
enum {
    ID_LAYER       = 1,
    ID_BLACK       = 3,
    ID_WHITE       = 4,
    ID_UNMULT      = 5,
    ID_PICK_BUTTON = 6,
    ID_HASH_HI     = 7,
    ID_HASH_LO     = 8
    // ID 2 retired (was a single 32-bit hash, lost precision).
};

// (Win32 dialog template IDs now live in exrdemux_dialog_win.cpp,
//  matching the .rc template.)

// Static popup slot count. AE locks the popup's choice list at
// PARAMS_SETUP and provides no API to mutate it later, so the visible
// labels are generic ("Layer 1"..."Layer 256"). The Pick Layer button
// shows real names via a native modal; the popup is the scriptable
// backing store.
constexpr int kLayerSlotCount = 256;

// Build the pipe-separated label list once. 256 slots, "Layer 1".."Layer 256".
// C++11 magic-static init is thread-safe; the buffer's lifetime is the
// process so the pointer we hand AE stays valid forever.
const char* StaticSlotLabels() {
    static const std::string buf = []{
        std::string s;
        s.reserve(kLayerSlotCount * 12);
        for (int i = 1; i <= kLayerSlotCount; ++i) {
            if (i > 1) s += '|';
            s += "Layer ";
            s += std::to_string(i);
        }
        return s;
    }();
    return buf.c_str();
}

void link_check_openexr() {
    Imath::half h(1.0f);
    (void)h;
}

// FNV-1a 32-bit hash. Used to persist channel-name selection in a
// scriptable numeric param so the choice survives re-renders that shift
// the EXR's channel ordering (the original user-pain motivating this
// project). Must match the JS implementation in
// tests/jsx/exrdemux_helpers.jsx so JSX automation can compute the
// same hash without going through the dialog.
uint32_t HashLayerName(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// Get the file path of the EXR backing the layer this effect is applied to.
// Returns empty string on any failure. Writes a human-readable diagnostic
// describing which step failed to *diag_out (so the popup can surface it
// during early development).
std::string GetSourceExrPath(PF_InData* in_data, std::string* diag_out) {
    auto fail = [&](const char* msg) -> std::string {
        if (diag_out) *diag_out = msg;
        return "";
    };

    if (!in_data)                     return fail("no in_data");
    if (!in_data->effect_ref)         return fail("no effect_ref");
    if (!in_data->pica_basicP)        return fail("no pica_basicP");

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    AEGP_LayerH layer = nullptr;
    A_Err err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
        in_data->effect_ref, &layer);
    if (err)    return fail("AEGP_GetEffectLayer error");
    if (!layer) return fail("AEGP_GetEffectLayer null");

    AEGP_ItemH item = nullptr;
    err = suites.LayerSuite7()->AEGP_GetLayerSourceItem(layer, &item);
    if (err)   return fail("AEGP_GetLayerSourceItem error");
    if (!item) return fail("AEGP_GetLayerSourceItem null");

    AEGP_FootageH footage = nullptr;
    err = suites.FootageSuite5()->AEGP_GetMainFootageFromItem(item, &footage);
    if (err)      return fail("AEGP_GetMainFootageFromItem error");
    if (!footage) return fail("AEGP_GetMainFootageFromItem null");

    AEGP_MemHandle path_handle = nullptr;
    err = suites.FootageSuite5()->AEGP_GetFootagePath(
        footage, 0, AEGP_FOOTAGE_MAIN_FILE_INDEX, &path_handle);
    if (err)          return fail("AEGP_GetFootagePath error");
    if (!path_handle) return fail("AEGP_GetFootagePath null");

    A_UTF16Char* utf16 = nullptr;
    suites.MemorySuite1()->AEGP_LockMemHandle(
        path_handle, reinterpret_cast<void**>(&utf16));

    std::string result;
    if (utf16) {
#ifdef AE_OS_WIN
        // A_UTF16Char is 16-bit, matches wchar_t on Windows.
        const wchar_t* wide = reinterpret_cast<const wchar_t*>(utf16);
        int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            result.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], n, nullptr, nullptr);
        }
#elif defined(AE_OS_MAC)
        // A_UTF16Char is 16-bit unsigned, matches UniChar (NOT wchar_t,
        // which is 32-bit on Mac). Build a CFString and convert to UTF-8.
        size_t len = 0;
        while (utf16[len] != 0) ++len;
        CFStringRef cfstr = CFStringCreateWithCharacters(
            nullptr, reinterpret_cast<const UniChar*>(utf16),
            static_cast<CFIndex>(len));
        if (cfstr) {
            CFIndex utf8_max = CFStringGetMaximumSizeForEncoding(
                CFStringGetLength(cfstr), kCFStringEncodingUTF8) + 1;
            std::vector<char> buf(static_cast<size_t>(utf8_max), 0);
            if (CFStringGetCString(cfstr, buf.data(), utf8_max, kCFStringEncodingUTF8)) {
                result = buf.data();
            }
            CFRelease(cfstr);
        }
#endif
    }

    suites.MemorySuite1()->AEGP_UnlockMemHandle(path_handle);
    suites.MemorySuite1()->AEGP_FreeMemHandle(path_handle);

    if (result.empty() && diag_out) *diag_out = "path empty after lock";
    return result;
}

// Group the EXR's channels into "layers". Iterates all parts (Blender's
// newer multilayer output may be multipart). For each channel name, the
// "layer" is everything before the last dot. Bare channels (no dot, e.g.
// plain "R") collapse into "(default)". Optional all_channels output
// captures every raw channel name for diagnostics.
std::vector<std::string> EnumerateExrLayers(
    const std::string& path,
    std::vector<std::string>* all_channels = nullptr,
    int* part_count = nullptr)
{
    std::vector<std::string> layers;
    if (path.empty()) return layers;

    try {
        Imf::MultiPartInputFile file(path.c_str());
        if (part_count) *part_count = file.parts();
        if (file.parts() == 0) return layers;

        std::set<std::string> seen;
        bool has_default = false;

        for (int p = 0; p < file.parts(); ++p) {
            const Imf::ChannelList& channels = file.header(p).channels();
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                std::string name = it.name();
                if (all_channels) all_channels->push_back(name);

                size_t dot = name.find_last_of('.');
                if (dot == std::string::npos) {
                    has_default = true;
                } else {
                    std::string layer = name.substr(0, dot);
                    if (seen.insert(layer).second) {
                        layers.push_back(layer);
                    }
                }
            }
        }

        if (has_default) {
            layers.insert(layers.begin(), "(default)");
        }
    } catch (const std::exception&) {
        layers.clear();
    }
    return layers;
}

// ===== AE channel-suite layer source =============================
//
// AE's bundled importer decodes the multilayer EXR once into RAM and
// exposes every channel via PF_ChannelSuite. We use it as the source
// of truth for what layers exist + how to read their pixels — this
// avoids re-decoding the file on every render and is the same API
// EXtractoR uses (which is why it's so much faster).

// Channel-name suffixes that mark a "this is the whole-layer 4-channel
// ARGB chunk" entry. AE's importer sometimes uses uppercase, sometimes
// lowercase (cryptos use lowercase).
bool EndsWithArgbSuffix(const std::string& name) {
    static const char* kSuffixes[] = {".ARGB", ".argb"};
    for (const char* s : kSuffixes) {
        size_t slen = std::strlen(s);
        if (name.size() >= slen &&
            name.compare(name.size() - slen, slen, s) == 0) {
            return true;
        }
    }
    return false;
}

// Strip the .ARGB/.argb suffix and de-duplicate a doubled prefix of the
// form "X.X" -> "X". Examples:
//   "Top Light Down.ARGB"                          -> "Top Light Down"
//   "CurtainColumn_001.CurtainColumn_001.ARGB"     -> "CurtainColumn_001"
//   "Image.Image.argb"                             -> "Image"
std::string DisplayNameFromArgbChannel(const std::string& ae_name) {
    size_t last_dot = ae_name.find_last_of('.');
    if (last_dot == std::string::npos) return ae_name;
    std::string prefix = ae_name.substr(0, last_dot);

    size_t mid_dot = prefix.find_last_of('.');
    if (mid_dot != std::string::npos) {
        std::string left  = prefix.substr(0, mid_dot);
        std::string right = prefix.substr(mid_dot + 1);
        if (left == right) return left;
    }
    return prefix;
}

struct ChannelEntry {
    std::string    display_name;
    PF_ChannelRef  ref;
    PF_ChannelDesc desc;
};

// Enumerate the input layer's ARGB-chunk channels. Each entry's
// display_name is what we hash + show to the user; ref is the handle
// AE wants for PF_CheckoutLayerChannel.
std::vector<ChannelEntry> EnumerateChannelSuiteLayers(
    PF_InData* in_data, PF_OutData* out_data)
{
    std::vector<ChannelEntry> entries;
    if (!in_data || !out_data) return entries;

    PF_ChannelSuite1* cs = nullptr;
    if (AEFX_AcquireSuite(in_data, out_data, kPFChannelSuite1,
                          kPFChannelSuiteVersion1, "channel suite",
                          reinterpret_cast<void**>(&cs)) != PF_Err_NONE || !cs) {
        return entries;
    }

    A_long count = 0;
    cs->PF_GetLayerChannelCount(in_data->effect_ref, PARAM_INPUT, &count);

    entries.reserve(static_cast<size_t>(count));
    for (A_long i = 0; i < count; ++i) {
        PF_Boolean     found = FALSE;
        ChannelEntry   e;
        std::memset(&e.ref,  0, sizeof(e.ref));
        std::memset(&e.desc, 0, sizeof(e.desc));
        if (cs->PF_GetLayerChannelIndexedRefAndDesc(
                in_data->effect_ref, PARAM_INPUT,
                static_cast<PF_ChannelIndex>(i),
                &found, &e.ref, &e.desc) != PF_Err_NONE || !found) {
            continue;
        }
        if (e.desc.dimension != 4) continue;          // skip per-channel scalars
        if (!EndsWithArgbSuffix(e.desc.name)) continue;

        e.display_name = DisplayNameFromArgbChannel(e.desc.name);
        entries.push_back(e);
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFChannelSuite1,
                      kPFChannelSuiteVersion1, "channel suite");
    return entries;
}

// ===== EXR rendering (channel-suite path) =========================

// Where in the EXR each of the layer's R/G/B/A channels lives. For
// multipart files all four typically share one part.
struct LayerLocation {
    int part_idx = -1;
    std::string r_name, g_name, b_name, a_name;
    bool has_r = false, has_g = false, has_b = false, has_a = false;
};

// Cached per-file metadata so repeated renders / UI redraws don't re-parse
// the EXR header and re-iterate every channel of every part.
//   layers    — the list of layer-name groupings (file authoring order)
//   locations — for each layer, which part and which channel names hold its
//               R/G/B/A. Pre-computed at cache-fill time.
struct ExrMetadata {
    std::vector<std::string>           layers;
    std::map<std::string, LayerLocation> locations;
};

// Cached metadata + the file fingerprint at the time we built it. mtime+size
// catches in-place re-renders (same path, new bytes) — the original user-pain
// case. AE's channel suite holds a stale name list across these re-renders;
// we trust this file-side cache as the source of truth for "what's actually
// in the EXR right now" and fall back to file-direct rendering when the
// channel suite disagrees.
struct CachedMeta {
    int64_t     mtime_ns = 0;
    int64_t     size     = 0;
    ExrMetadata meta;
};

static std::mutex                          g_meta_mutex;
static std::map<std::string, CachedMeta>   g_meta_cache;

struct FileStamp {
    int64_t mtime_ns = 0;
    int64_t size     = 0;
    bool    valid    = false;
};

FileStamp StampFile(const std::string& path) {
    FileStamp s;
    if (path.empty()) return s;
    try {
        std::error_code ec;
        std::filesystem::path p(path);
        if (!std::filesystem::is_regular_file(p, ec) || ec) return s;
        auto ftime = std::filesystem::last_write_time(p, ec);
        if (ec) return s;
        s.mtime_ns = static_cast<int64_t>(ftime.time_since_epoch().count());
        auto sz = std::filesystem::file_size(p, ec);
        if (ec) return s;
        s.size = static_cast<int64_t>(sz);
        s.valid = true;
    } catch (...) {
        // std::filesystem can throw on weird paths on Windows; treat as no-stamp.
    }
    return s;
}

// Build (without locking) — caller must already hold the lock if mutating.
ExrMetadata BuildExrMetadata(const std::string& path) {
    ExrMetadata meta;
    if (path.empty()) return meta;

    try {
        Imf::MultiPartInputFile file(path.c_str());

        std::set<std::string> seen;
        bool has_default = false;
        for (int p = 0; p < file.parts(); ++p) {
            const Imf::ChannelList& channels = file.header(p).channels();
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                std::string ch_name = it.name();
                size_t dot = ch_name.find_last_of('.');
                if (dot == std::string::npos) {
                    has_default = true;
                    continue;
                }
                std::string layer = ch_name.substr(0, dot);
                std::string suffix = ch_name.substr(dot + 1);

                if (seen.insert(layer).second) {
                    meta.layers.push_back(layer);
                    meta.locations[layer].part_idx = p;
                }

                LayerLocation& loc = meta.locations[layer];
                // Cryptomattes (and some other non-color channels) use
                // lowercase r/g/b/a per OpenEXR convention. AE's channel
                // suite still surfaces them as 4-channel ARGB chunks; if we
                // didn't recognize the lowercase form here, file_names would
                // be missing those layers and ChannelSuiteMatchesFile would
                // false-positive on staleness, killing the suite fast path.
                if      (suffix == "R" || suffix == "r") { loc.r_name = ch_name; loc.has_r = true; }
                else if (suffix == "G" || suffix == "g") { loc.g_name = ch_name; loc.has_g = true; }
                else if (suffix == "B" || suffix == "b") { loc.b_name = ch_name; loc.has_b = true; }
                else if (suffix == "A" || suffix == "a") { loc.a_name = ch_name; loc.has_a = true; }
            }
        }
        if (has_default) {
            meta.layers.insert(meta.layers.begin(), "(default)");
        }
    } catch (const std::exception&) {
        // Bad file or non-EXR — return whatever partial data we have.
    }
    return meta;
}

// Returns metadata for the given file. Stat-validated: if the file's mtime
// or size has changed since the cached entry was built, we rebuild. This is
// what makes in-place re-renders work — same path, new contents, new
// metadata.
ExrMetadata GetExrMetadata(const std::string& path) {
    if (path.empty()) return {};
    FileStamp stamp = StampFile(path);

    {
        std::lock_guard<std::mutex> lock(g_meta_mutex);
        auto it = g_meta_cache.find(path);
        if (it != g_meta_cache.end()) {
            const CachedMeta& c = it->second;
            // No valid stamp (file just vanished, network glitch, etc.):
            // trust the cache rather than blowing it away.
            if (!stamp.valid) return c.meta;
            if (c.mtime_ns == stamp.mtime_ns && c.size == stamp.size) {
                return c.meta;
            }
        }
    }

    // Build outside the lock so concurrent renders don't block each other.
    ExrMetadata fresh = BuildExrMetadata(path);

    if (stamp.valid) {
        std::lock_guard<std::mutex> lock(g_meta_mutex);
        CachedMeta entry;
        entry.mtime_ns = stamp.mtime_ns;
        entry.size     = stamp.size;
        entry.meta     = fresh;
        g_meta_cache[path] = entry;
    }
    return fresh;
}

// Look up the layer name whose hash matches `hash` against pre-fetched
// metadata. Empty string = no match.
std::string ResolveLayerByHash(const ExrMetadata& meta, uint32_t hash) {
    for (const auto& layer : meta.layers) {
        if (HashLayerName(layer) == hash) return layer;
    }
    return "";
}

// Convenience wrapper for callers that don't already have metadata in hand.
std::string ResolveLayerByHash(const std::string& path, uint32_t hash) {
    if (path.empty()) return "";
    return ResolveLayerByHash(GetExrMetadata(path), hash);
}

// Apply Black/White Point rescaling and optional UnMult, in place.
inline void ApplyAdjust(float& r, float& g, float& b, float a,
                        double black, double white, bool unmult) {
    double range = white - black;
    double scale = (range != 0.0) ? 1.0 / range : 1.0;
    r = static_cast<float>((r - black) * scale);
    g = static_cast<float>((g - black) * scale);
    b = static_cast<float>((b - black) * scale);
    if (unmult && a > 0.0f) {
        r /= a;
        g /= a;
        b /= a;
    }
}

inline A_u_short ClampTo16(float v) {
    float scaled = v * static_cast<float>(PF_MAX_CHAN16);
    if (scaled <= 0.0f) return 0;
    if (scaled >= PF_MAX_CHAN16) return PF_MAX_CHAN16;
    return static_cast<A_u_short>(scaled + 0.5f);
}

inline A_u_char ClampTo8(float v) {
    float scaled = v * static_cast<float>(PF_MAX_CHAN8);
    if (scaled <= 0.0f) return 0;
    if (scaled >= PF_MAX_CHAN8) return PF_MAX_CHAN8;
    return static_cast<A_u_char>(scaled + 0.5f);
}

// Fill an output PF_EffectWorld with black (0,0,0,0). Used when we can't
// resolve the requested layer (deleted from re-rendered EXR, etc.).
void FillBlack(PF_EffectWorld* out, PF_PixelFormat fmt) {
    if (!out) return;
    for (A_long y = 0; y < out->height; ++y) {
        char* row = reinterpret_cast<char*>(out->data) + y * out->rowbytes;
        std::memset(row, 0, std::abs(out->rowbytes));
    }
}

// Per-call timings (microseconds) so we can find perf hot spots from the
// log without an external profiler.
struct RenderTimings {
    long long open_us = 0;
    long long read_us = 0;
    long long convert_us = 0;
};

// Core EXR-to-output render. Reads the (resolved) layer's R/G/B/A
// scanlines, applies adjustments, writes to the output world in its
// native pixel format.
PF_Err RenderExrLayer(const std::string& path,
                      const std::string& layer_name,
                      double black, double white, bool unmult,
                      PF_EffectWorld* output,
                      PF_PixelFormat fmt,
                      RenderTimings* timings = nullptr,
                      std::string* decoder_error_out = nullptr) {
    if (!output || !output->data) return PF_Err_NONE;

    if (path.empty() || layer_name.empty()) {
        FillBlack(output, fmt);
        return PF_Err_NONE;
    }

    using clock = std::chrono::high_resolution_clock;
    auto t_start = clock::now();

    try {
        ExrMetadata meta = GetExrMetadata(path);
        auto loc_it = meta.locations.find(layer_name);
        if (loc_it == meta.locations.end()) { FillBlack(output, fmt); return PF_Err_NONE; }
        const LayerLocation& loc = loc_it->second;
        if (loc.part_idx < 0) { FillBlack(output, fmt); return PF_Err_NONE; }

        Imf::MultiPartInputFile file(path.c_str());
        auto t_opened = clock::now();

        const Imf::Header& header = file.header(loc.part_idx);
        Imath::Box2i dw = header.dataWindow();
        int w = dw.max.x - dw.min.x + 1;
        int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) { FillBlack(output, fmt); return PF_Err_NONE; }

        // Allocate half buffers for the four channels (default A=1.0).
        std::vector<half> rBuf(static_cast<size_t>(w) * h, half(0.0f));
        std::vector<half> gBuf(static_cast<size_t>(w) * h, half(0.0f));
        std::vector<half> bBuf(static_cast<size_t>(w) * h, half(0.0f));
        std::vector<half> aBuf(static_cast<size_t>(w) * h, half(1.0f));

        Imf::FrameBuffer fb;
        auto attach = [&](const std::string& name, std::vector<half>& buf) {
            char* base = reinterpret_cast<char*>(buf.data())
                       - (dw.min.x + dw.min.y * w) * sizeof(half);
            fb.insert(name.c_str(),
                Imf::Slice(Imf::HALF, base, sizeof(half),
                           sizeof(half) * static_cast<size_t>(w)));
        };
        if (loc.has_r) attach(loc.r_name, rBuf);
        if (loc.has_g) attach(loc.g_name, gBuf);
        if (loc.has_b) attach(loc.b_name, bBuf);
        if (loc.has_a) attach(loc.a_name, aBuf);

        Imf::InputPart input(file, loc.part_idx);
        input.setFrameBuffer(fb);
        input.readPixels(dw.min.y, dw.max.y);
        auto t_read = clock::now();

        // Write to output. We assume 1:1 pixel mapping with the EXR's
        // data window — if AE's output is larger we leave the rest
        // black, smaller we crop. Phase 4.x will handle resampling.
        A_long out_w = output->width;
        A_long out_h = output->height;

        for (A_long y = 0; y < out_h; ++y) {
            char* row = reinterpret_cast<char*>(output->data) + y * output->rowbytes;
            int src_y = y;
            bool y_in_range = (src_y >= 0 && src_y < h);

            for (A_long x = 0; x < out_w; ++x) {
                int src_x = x;
                float fr = 0, fg = 0, fb_ = 0, fa = 0;
                if (y_in_range && src_x >= 0 && src_x < w) {
                    size_t i = static_cast<size_t>(src_y) * w + src_x;
                    fr  = static_cast<float>(rBuf[i]);
                    fg  = static_cast<float>(gBuf[i]);
                    fb_ = static_cast<float>(bBuf[i]);
                    fa  = static_cast<float>(aBuf[i]);
                    ApplyAdjust(fr, fg, fb_, fa, black, white, unmult);
                }

                switch (fmt) {
                    case PF_PixelFormat_ARGB128: {
                        PF_PixelFloat* p = reinterpret_cast<PF_PixelFloat*>(row) + x;
                        p->alpha = fa;
                        p->red   = fr;
                        p->green = fg;
                        p->blue  = fb_;
                        break;
                    }
                    case PF_PixelFormat_ARGB64: {
                        PF_Pixel16* p = reinterpret_cast<PF_Pixel16*>(row) + x;
                        p->alpha = ClampTo16(fa);
                        p->red   = ClampTo16(fr);
                        p->green = ClampTo16(fg);
                        p->blue  = ClampTo16(fb_);
                        break;
                    }
                    case PF_PixelFormat_ARGB32:
                    default: {
                        PF_Pixel8* p = reinterpret_cast<PF_Pixel8*>(row) + x;
                        p->alpha = ClampTo8(fa);
                        p->red   = ClampTo8(fr);
                        p->green = ClampTo8(fg);
                        p->blue  = ClampTo8(fb_);
                        break;
                    }
                }
            }
        }
        auto t_done = clock::now();
        if (timings) {
            auto us = [](clock::time_point a, clock::time_point b) {
                return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
            };
            timings->open_us    = us(t_start, t_opened);
            timings->read_us    = us(t_opened, t_read);
            timings->convert_us = us(t_read, t_done);
        }
        return PF_Err_NONE;
    } catch (const std::exception& e) {
        // OpenEXR raises here for decoder failures (e.g. DWAB chunks that
        // OpenEXR 3.4 can't decompress on certain Blender 5.x outputs) and
        // for malformed files generally. Surface the message so the caller
        // can choose to fall back to the channel-suite path; FillBlack so
        // AE keeps rendering rather than erroring out.
        if (decoder_error_out) *decoder_error_out = e.what();
        FillBlack(output, fmt);
        return PF_Err_NONE;
    }
}

// True when AE's channel-suite enumeration agrees with the EXR file on disk
// about which 4-channel layers exist. When false, AE's importer is holding a
// stale name list (typical after an in-place re-render — see commit
// "Mac scaffolding..." era for the diagnosis) and the suite's refs may point
// to fresh pixel data under wrong names. We treat that as the trigger to
// render via direct file read instead.
//
// Both sides filter to layers that have R+G+B+A in the file. AE's channel
// suite only surfaces 4-channel chunks (PF_ChannelSuite skips per-channel
// scalars and 3-channel layers); the file side must match that filter or we
// false-positive on staleness for any multilayer file containing 3-channel
// passes (Blender's per-light AOVs, denoising AOVs, etc.).
bool ChannelSuiteMatchesFile(const std::vector<ChannelEntry>& suite,
                              const ExrMetadata& file_meta) {
    std::set<std::string> suite_names;
    for (const auto& e : suite) suite_names.insert(e.display_name);

    std::set<std::string> file_names;
    for (const auto& kv : file_meta.locations) {
        const LayerLocation& loc = kv.second;
        if (loc.has_r && loc.has_g && loc.has_b && loc.has_a) {
            file_names.insert(kv.first);
        }
    }
    return suite_names == file_names;
}

// Read the chosen layer's ARGB chunk via PF_ChannelSuite (data already
// decoded by AE's importer — no file read), apply Black/White Point +
// UnMult, write to the output world in its native pixel format. Caller has
// already enumerated entries and confirmed the suite is fresh.
PF_Err RenderViaChannelSuiteEntries(
        PF_InData* in_data, PF_OutData* out_data,
        const std::vector<ChannelEntry>& entries,
        uint32_t hash, double black, double white, bool unmult,
        PF_EffectWorld* output, PF_PixelFormat fmt,
        RenderTimings* timings = nullptr) {
    if (!output || !output->data) return PF_Err_NONE;
    if (timings) *timings = RenderTimings{};

    using clock = std::chrono::high_resolution_clock;
    auto t_start = clock::now();

    const ChannelEntry* picked = nullptr;
    for (const auto& e : entries) {
        if (HashLayerName(e.display_name) == hash) { picked = &e; break; }
    }
    if (!picked) { FillBlack(output, fmt); return PF_Err_NONE; }
    auto t_resolved = clock::now();

    // Acquire the channel suite for the actual checkout.
    PF_ChannelSuite1* cs = nullptr;
    if (AEFX_AcquireSuite(in_data, out_data, kPFChannelSuite1,
                          kPFChannelSuiteVersion1, "channel suite",
                          reinterpret_cast<void**>(&cs)) != PF_Err_NONE || !cs) {
        FillBlack(output, fmt);
        return PF_Err_NONE;
    }

    PF_ChannelChunk chunk;
    std::memset(&chunk, 0, sizeof(chunk));
    PF_ChannelRef ref = picked->ref;  // suite mutates it; pass our copy
    PF_Err checkout_err = cs->PF_CheckoutLayerChannel(
        in_data->effect_ref, &ref,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        PF_DataType_FLOAT, &chunk);

    auto t_opened = clock::now();

    if (checkout_err != PF_Err_NONE || !chunk.dataPV ||
        chunk.dimensionL != 4 || chunk.data_type != PF_DataType_FLOAT) {
        AEFX_ReleaseSuite(in_data, out_data, kPFChannelSuite1,
                          kPFChannelSuiteVersion1, "channel suite");
        FillBlack(output, fmt);
        return PF_Err_NONE;
    }

    // The chunk is interleaved ARGB float (4 floats per pixel = 16 bytes,
    // matching PF_PixelFloat layout).
    A_long out_w = output->width;
    A_long out_h = output->height;
    A_long src_w = chunk.widthL;
    A_long src_h = chunk.heightL;

    // For 32bpc + identity adjust + unmult-off, we could memcpy each row.
    // Keep the unified per-pixel loop for now — Phase 4.x can fast-path.
    for (A_long y = 0; y < out_h; ++y) {
        char* row = reinterpret_cast<char*>(output->data) + y * output->rowbytes;
        bool y_in_range = (y >= 0 && y < src_h);
        const float* src_row = y_in_range
            ? reinterpret_cast<const float*>(
                  reinterpret_cast<const char*>(chunk.dataPV) + y * chunk.row_bytesL)
            : nullptr;

        for (A_long x = 0; x < out_w; ++x) {
            float fa = 0, fr = 0, fg = 0, fb_ = 0;
            if (src_row && x >= 0 && x < src_w) {
                const float* p = src_row + x * 4;
                fa  = p[0]; fr  = p[1]; fg  = p[2]; fb_ = p[3];
                ApplyAdjust(fr, fg, fb_, fa, black, white, unmult);
            }

            switch (fmt) {
                case PF_PixelFormat_ARGB128: {
                    PF_PixelFloat* po = reinterpret_cast<PF_PixelFloat*>(row) + x;
                    po->alpha = fa; po->red = fr; po->green = fg; po->blue = fb_;
                    break;
                }
                case PF_PixelFormat_ARGB64: {
                    PF_Pixel16* po = reinterpret_cast<PF_Pixel16*>(row) + x;
                    po->alpha = ClampTo16(fa);
                    po->red   = ClampTo16(fr);
                    po->green = ClampTo16(fg);
                    po->blue  = ClampTo16(fb_);
                    break;
                }
                default: {
                    PF_Pixel8* po = reinterpret_cast<PF_Pixel8*>(row) + x;
                    po->alpha = ClampTo8(fa);
                    po->red   = ClampTo8(fr);
                    po->green = ClampTo8(fg);
                    po->blue  = ClampTo8(fb_);
                    break;
                }
            }
        }
    }
    auto t_done = clock::now();

    cs->PF_CheckinLayerChannel(in_data->effect_ref, &ref, &chunk);
    AEFX_ReleaseSuite(in_data, out_data, kPFChannelSuite1,
                      kPFChannelSuiteVersion1, "channel suite");

    if (timings) {
        auto us = [](clock::time_point a, clock::time_point b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };
        // open = enumerate-and-find,  read = AE's cache checkout,
        // convert = per-pixel adjust + format.
        timings->open_us    = us(t_start,    t_resolved);
        timings->read_us    = us(t_resolved, t_opened);
        timings->convert_us = us(t_opened,   t_done);
    }
    return PF_Err_NONE;
}

// Forward decl — defined alongside LogPerf below. Called from RenderHash on
// the unusual code paths (staleness mismatch, decoder-error fallback).
void LogEvent(const std::string& message);

// Top-level render dispatcher. Picks between the fast channel-suite path and
// the slow file-direct path based on whether AE's importer agrees with the
// EXR file on disk. After an in-place re-render AE's channel suite holds a
// stale name list; we detect that and route through file-direct rendering so
// the user's name-based selection still resolves correctly.
//
// On return, `path_kind_out` is "suite" for the fast path, "file" for the
// fallback, or "file->suite" when file-direct's decoder threw and we recovered
// via the suite (caller logs it). `layer_out` is the resolved layer name
// (empty if no match — output is then black).
PF_Err RenderHash(PF_InData* in_data, PF_OutData* out_data,
                  uint32_t hash, double black, double white, bool unmult,
                  PF_EffectWorld* output, PF_PixelFormat fmt,
                  RenderTimings* timings,
                  const char** path_kind_out,
                  std::string* layer_out) {
    if (path_kind_out) *path_kind_out = "?";
    if (layer_out) layer_out->clear();
    if (timings) *timings = RenderTimings{};

    // File side: path + (mtime-validated) metadata.
    std::string path = GetSourceExrPath(in_data, nullptr);
    ExrMetadata meta = path.empty() ? ExrMetadata{} : GetExrMetadata(path);

    // Channel-suite side: enumerate every render. Cheap (no file I/O), and
    // gives us both the staleness signal and the refs for the fast path.
    std::vector<ChannelEntry> entries =
        EnumerateChannelSuiteLayers(in_data, out_data);

    bool fresh = !path.empty() && ChannelSuiteMatchesFile(entries, meta);

    if (fresh) {
        if (path_kind_out) *path_kind_out = "suite";
        if (layer_out) {
            for (const auto& e : entries) {
                if (HashLayerName(e.display_name) == hash) {
                    *layer_out = e.display_name; break;
                }
            }
        }
        return RenderViaChannelSuiteEntries(in_data, out_data, entries,
                                            hash, black, white, unmult,
                                            output, fmt, timings);
    }

    // Mismatch between AE's surface and the EXR on disk — emit a one-time
    // diagnostic so we can see exactly what AE exposes for files where the
    // strict equality check trips. Limit volume by listing names only and
    // truncating the lists.
    if (!path.empty()) {
        std::string msg = "Staleness-mismatch  path=" + path;
        msg += "\n  suite[" + std::to_string(entries.size()) + "]:";
        size_t n = 0;
        for (const auto& e : entries) {
            if (n++ >= 8) { msg += " ..."; break; }
            msg += " '" + e.display_name + "'";
        }
        size_t file_4ch = 0;
        for (const auto& kv : meta.locations) {
            const LayerLocation& loc = kv.second;
            if (loc.has_r && loc.has_g && loc.has_b && loc.has_a) ++file_4ch;
        }
        msg += "\n  file-4ch[" + std::to_string(file_4ch) + "]:";
        n = 0;
        for (const auto& kv : meta.locations) {
            const LayerLocation& loc = kv.second;
            if (!(loc.has_r && loc.has_g && loc.has_b && loc.has_a)) continue;
            if (n++ >= 8) { msg += " ..."; break; }
            msg += " '" + kv.first + "'";
        }
        LogEvent(msg);
    }

    // Suite is stale (or we couldn't get a path). Try direct file read first
    // so name-based selection survives in-place re-renders. If the file's
    // decoder throws (DWAB-on-Blender-5.x is the known case), fall back to
    // suite render: AE's importer may have decoded successfully where our
    // statically-linked OpenEXR can't.
    std::string layer = ResolveLayerByHash(meta, hash);
    std::string decoder_err;
    PF_Err err = RenderExrLayer(path, layer, black, white, unmult,
                                output, fmt, timings, &decoder_err);
    if (decoder_err.empty()) {
        if (path_kind_out) *path_kind_out = "file";
        if (layer_out) *layer_out = layer;
        return err;
    }

    LogEvent("Decoder-error  " + decoder_err + "  path=" + path
             + "  layer=" + layer + "  -> falling back to suite render");

    if (path_kind_out) *path_kind_out = "file->suite";
    if (layer_out) {
        for (const auto& e : entries) {
            if (HashLayerName(e.display_name) == hash) {
                *layer_out = e.display_name; break;
            }
        }
    }
    return RenderViaChannelSuiteEntries(in_data, out_data, entries,
                                        hash, black, white, unmult,
                                        output, fmt, timings);
}

// Path to the user's Desktop, where we drop the diagnostic perf log.
// Empty string if unavailable. Used only for development debugging.
std::string DesktopFilePath(const char* filename) {
#ifdef AE_OS_WIN
    if (const char* home = std::getenv("USERPROFILE")) {
        return std::string(home) + "\\Desktop\\" + filename;
    }
#elif defined(AE_OS_MAC)
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/Desktop/" + filename;
    }
#endif
    return std::string();
}

// Append a single perf line to the desktop log. No-op unless the user has
// opted in via the EXRDEMUX_PERF_LOG environment variable (any non-empty
// value enables it). Keeps the diagnostic available when needed without
// scattering exrdemux_perf.txt onto every teammate's desktop in the common
// case.
void LogPerf(const char* selector, long long total_us,
             const RenderTimings& t, const std::string& path,
             const std::string& layer) {
    const char* opt_in = std::getenv("EXRDEMUX_PERF_LOG");
    if (!opt_in || opt_in[0] == '\0') return;

    std::string log_path = DesktopFilePath("exrdemux_perf.txt");
    if (log_path.empty()) return;
    if (FILE* f = std::fopen(log_path.c_str(), "ab")) {
        std::fprintf(f,
            "%s total=%lldus open=%lldus read=%lldus convert=%lldus  layer=%s  path=%s\n",
            selector, total_us, t.open_us, t.read_us, t.convert_us,
            layer.empty() ? "<none>" : layer.c_str(),
            path.empty() ? "<none>" : path.c_str());
        std::fclose(f);
    }
}

// Append an event line to the desktop log unconditionally — used for unusual
// code paths (suite/file naming mismatch, decoder-error fallback) that we
// want visibility into even when the user hasn't opted into perf logging.
// Normal rendering produces no calls here, so the desktop file only appears
// when something interesting happens.
void LogEvent(const std::string& message) {
    std::string log_path = DesktopFilePath("exrdemux_perf.txt");
    if (log_path.empty()) return;
    if (FILE* f = std::fopen(log_path.c_str(), "ab")) {
        std::fprintf(f, "%s\n", message.c_str());
        std::fclose(f);
    }
}

// (Layer picker dialog implementation lives in:
//    src/exrdemux_dialog_win.cpp  (Win32, backed by exrdemux_dialog.rc)
//    src/exrdemux_dialog_mac.mm   (AppKit NSAlert + NSPopUpButton)
//  Both expose the same `int ShowLayerPickerDialog(...)` from
//  exrdemux_dialog.h.)

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data,
                   PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    out_data->my_version = PF_VERSION(
        EXRDEMUX_MAJOR_VERSION,
        EXRDEMUX_MINOR_VERSION,
        EXRDEMUX_BUG_VERSION,
        EXRDEMUX_STAGE_VERSION,
        EXRDEMUX_BUILD_VERSION);

    // 16bpc + 32bpc float, Smart Render for the float path, MFR-safe.
    // SEND_UPDATE_PARAMS_UI lets us re-apply the "Layer: <name>" label
    // every time the Effect Controls panel redraws, so the selection
    // stays visible after switching to another AE layer and back.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE
                         | PF_OutFlag_PIX_INDEPENDENT
                         | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE
                         | PF_OutFlag2_SUPPORTS_SMART_RENDER
                         | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data,
                   PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    PF_Err     err = PF_Err_NONE;
    PF_ParamDef def;

    // [1] Layer popup. Hidden from the Effect Controls panel — its only
    // job is to be the JSX-scriptable backing store for the slot index.
    // (AE's popup string list is locked at PARAMS_SETUP and can't be
    // updated to real layer names; we surface real names via the button
    // below + dynamic display-name updates.)
    AEFX_CLR_STRUCT(def);
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_POPUP("Layer",
                 kLayerSlotCount,
                 1,                       // 1-based default
                 StaticSlotLabels(),
                 ID_LAYER);

    // [2] Pick Layer button — opens a native modal listbox of real names.
    // SUPERVISE flag tells AE to send PF_Cmd_USER_CHANGED_PARAM on click.
    // The param's display name (left of the button) is updated dynamically
    // to "Layer: <selected layer>" after each pick.
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_BUTTON;
    def.flags      = PF_ParamFlag_SUPERVISE;
    PF_STRNNCPY(def.PF_DEF_NAME, "Layer", sizeof(def.PF_DEF_NAME));
    def.u.button_d.u.PF_DEF_NAMESPTR = "Pick Layer...";
    def.uu.id = ID_PICK_BUTTON;
    if (PF_Err pe = PF_ADD_PARAM(in_data, -1, &def)) return pe;

    // [3,4] Layer-name hash split into hi/lo 16-bit halves. Stored as two
    // float sliders so they survive AE's float32 persistence quantization
    // (a single 32-bit hash slider rounds to the nearest representable
    // float and corrupts integer hashes above ~16M). Hidden from UI;
    // populated on Pick Layer click and read at render time.
    //
    // We use PF_ADD_FLOAT_SLIDER (not the *X variant) because *X calls
    // AEFX_CLR_STRUCT(def) internally — wiping our ui_flags = INVISIBLE
    // before AE sees it. The non-X form lets pre-set ui_flags survive.
    AEFX_CLR_STRUCT(def);
    def.flags    = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDER("Layer Hash Hi",
                        0, 65535, 0, 65535,
                        AEFX_DEFAULT_CURVE_TOLERANCE,
                        0.0, 0,
                        PF_ValueDisplayFlag_NONE,
                        false,
                        ID_HASH_HI);

    AEFX_CLR_STRUCT(def);
    def.flags    = PF_ParamFlag_CANNOT_TIME_VARY | PF_ParamFlag_CANNOT_INTERP;
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_FLOAT_SLIDER("Layer Hash Lo",
                        0, 65535, 0, 65535,
                        AEFX_DEFAULT_CURVE_TOLERANCE,
                        0.0, 0,
                        PF_ValueDisplayFlag_NONE,
                        false,
                        ID_HASH_LO);

    // [3] Black Point — same intent as EXtractoR's Black Point.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Black Point",
                         -1e10, 1e10,
                         -1.0, 1.0,
                         0.0,
                         4,
                         PF_ValueDisplayFlag_NONE,
                         0,
                         ID_BLACK);

    // [4] White Point — same intent as EXtractoR's White Point.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("White Point",
                         -1e10, 1e10,
                         -1.0, 1.0,
                         1.0,
                         4,
                         PF_ValueDisplayFlag_NONE,
                         0,
                         ID_WHITE);

    // [5] UnMult — same intent as EXtractoR's UnMult.
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("UnMult", "", FALSE, 0, ID_UNMULT);

    out_data->num_params = PARAM_COUNT;
    return err;
}

// Read the (split) layer-name hash from the param array.
uint32_t HashFromParams(PF_ParamDef* params[]) {
    uint32_t hi = static_cast<uint32_t>(params[PARAM_HASH_HI]->u.fs_d.value) & 0xFFFFu;
    uint32_t lo = static_cast<uint32_t>(params[PARAM_HASH_LO]->u.fs_d.value) & 0xFFFFu;
    return (hi << 16) | lo;
}

PF_PixelFormat GetOutputPixelFormat(PF_InData* in_data, PF_OutData* out_data,
                                    PF_EffectWorld* world) {
    PF_PixelFormat fmt = PF_PixelFormat_ARGB32;
    PF_WorldSuite2* ws = nullptr;
    if (AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
                          "world suite",
                          reinterpret_cast<void**>(&ws)) == PF_Err_NONE && ws) {
        ws->PF_GetPixelFormat(world, &fmt);
        AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
                          "world suite");
    }
    return fmt;
}

// Legacy 8/16bpc render path. Reads params directly from the array,
// resolves hash → layer name, dispatches to RenderExrLayer.
PF_Err Render(PF_InData* in_data, PF_OutData* out_data,
              PF_ParamDef* params[], PF_LayerDef* output) {
    link_check_openexr();
    auto t_start = std::chrono::high_resolution_clock::now();

    uint32_t hash = HashFromParams(params);
    double black  = params[PARAM_BLACK]->u.fs_d.value;
    double white  = params[PARAM_WHITE]->u.fs_d.value;
    bool unmult   = params[PARAM_UNMULT]->u.bd.value != 0;

    PF_PixelFormat fmt = GetOutputPixelFormat(in_data, out_data, output);
    RenderTimings rt;
    const char* path_kind = "?";
    std::string layer_used;
    PF_Err err = RenderHash(in_data, out_data, hash,
                            black, white, unmult, output, fmt,
                            &rt, &path_kind, &layer_used);

    auto total = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - t_start).count();
    char selector[32];
    std::snprintf(selector, sizeof(selector), "Render(%s)", path_kind);
    LogPerf(selector, total, rt, path_kind, layer_used);
    return err;
}

// Called when the user clicks our "Pick Layer..." button (or interacts
// with any SUPERVISE-flagged param). We open a modal layer picker, and
// on a confirmed selection we update the Layer popup's display name to
// show the chosen layer name plus stash the index back into the popup.
PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data,
                        PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* which) {
    if (!which || which->param_index != PARAM_PICK_BUTTON) {
        return PF_Err_NONE;
    }

    // For the dialog we read the EXR file directly so we can present
    // layers in Blender's authoring order. The channel suite only ever
    // exposes them alphabetically. File parse takes a few ms — fine
    // for an interactive click. (Render still uses the in-memory
    // channel suite path; ordering doesn't matter there.)
    std::string path = GetSourceExrPath(in_data, nullptr);
    std::vector<std::string> display_names;
    if (!path.empty()) display_names = EnumerateExrLayers(path);
    if (display_names.empty()) return PF_Err_NONE;

    int picked = ShowLayerPickerDialog(display_names);
    if (picked < 0 || picked >= static_cast<int>(display_names.size())) {
        return PF_Err_NONE;  // user cancelled
    }

    const std::string& name = display_names[picked];
    uint32_t hash = HashLayerName(name);

    // Persist:
    //   - PARAM_HASH_HI / PARAM_HASH_LO (FNV-1a 32-bit name hash, split
    //     across two float sliders to survive AE's float32 persistence)
    //   - PARAM_LAYER (1-based popup index, scriptable convenience)
    if (params && params[PARAM_LAYER]) {
        params[PARAM_LAYER]->u.pd.value = static_cast<short>(picked + 1);
        params[PARAM_LAYER]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }
    if (params && params[PARAM_HASH_HI]) {
        params[PARAM_HASH_HI]->u.fs_d.value =
            static_cast<double>((hash >> 16) & 0xFFFFu);
        params[PARAM_HASH_HI]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }
    if (params && params[PARAM_HASH_LO]) {
        params[PARAM_HASH_LO]->u.fs_d.value =
            static_cast<double>(hash & 0xFFFFu);
        params[PARAM_HASH_LO]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    }

    // Update the button's left-side display name to "Layer: <selected>"
    // so the user can see the current selection at a glance without
    // re-opening the dialog. The button's face text ("Pick Layer...")
    // must be preserved explicitly — AEFX_CLR_STRUCT zeroes the union
    // and AE reads the (now-null) namesptr otherwise, blanking the button.
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_BUTTON;
    def.u.button_d.u.PF_DEF_NAMESPTR = "Pick Layer...";
    std::string new_name = "Layer: " + name;
    PF_STRNNCPY(def.PF_DEF_NAME, new_name.c_str(), sizeof(def.PF_DEF_NAME));
    suites.ParamUtilsSuite3()->PF_UpdateParamUI(
        in_data->effect_ref, PARAM_PICK_BUTTON, &def);

    return PF_Err_NONE;
}

// SEQUENCE_SETUP runs once per effect instance. We don't strictly need
// it any more — UserChangedParam reads the EXR fresh on each click —
// but keeping it as a no-op stub means future phases (cache, sequence
// state for the hash-resolution render path) have a hook to extend.
PF_Err SequenceSetup(PF_InData* /*in_data*/, PF_OutData* /*out_data*/) {
    return PF_Err_NONE;
}

// AE issues this whenever the Effect Controls panel needs a redraw —
// switching to a different AE layer and back, time scrubbing, etc.
// Without it, the "Layer: <name>" label set during USER_CHANGED_PARAM
// vanishes the next time AE re-renders the panel from scratch.
//
// We re-resolve the stored hash to a layer name and re-apply the label.
// File enum is the primary source: it always reflects the current EXR (the
// metadata cache auto-invalidates on mtime change). Channel-suite enum is a
// fallback for the brief window where AEGP isn't ready to give us a path.
PF_Err UpdateParamsUI(PF_InData* in_data, PF_OutData* out_data,
                     PF_ParamDef* params[]) {
    if (!params || !in_data || !in_data->effect_ref) return PF_Err_NONE;

    uint32_t hash = HashFromParams(params);
    if (hash == 0) return PF_Err_NONE;  // no selection yet

    std::string name;
    std::string path = GetSourceExrPath(in_data, nullptr);
    if (!path.empty()) {
        // Authoritative: if file metadata is available, the answer here is
        // the answer. An empty name means the hash truly doesn't match a
        // layer in the current file → label as (missing).
        name = ResolveLayerByHash(GetExrMetadata(path), hash);
    } else {
        // No path (AEGP not ready yet for this effect). Fall back to the
        // channel-suite enum — it may be stale after re-renders, but it's
        // all we have in this transient window.
        std::vector<ChannelEntry> entries =
            EnumerateChannelSuiteLayers(in_data, out_data);
        for (const auto& e : entries) {
            if (HashLayerName(e.display_name) == hash) {
                name = e.display_name; break;
            }
        }
    }

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_BUTTON;
    def.u.button_d.u.PF_DEF_NAMESPTR = "Pick Layer...";
    std::string label = "Layer: " + (name.empty() ? std::string("(missing)") : name);
    PF_STRNNCPY(def.PF_DEF_NAME, label.c_str(), sizeof(def.PF_DEF_NAME));
    suites.ParamUtilsSuite3()->PF_UpdateParamUI(
        in_data->effect_ref, PARAM_PICK_BUTTON, &def);
    return PF_Err_NONE;
}

// Smart Render: required for 32bpc float color (PF_OutFlag2_FLOAT_COLOR_AWARE).
// AE calls PreRender first to declare needs, then SmartRender to do the work.
PF_Err PreRender(PF_InData* in_data, PF_OutData* /*out_data*/,
                 PF_PreRenderExtra* extra) {
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    PF_Err err = extra->cb->checkout_layer(
        in_data->effect_ref,
        PARAM_INPUT,
        PARAM_INPUT,
        &req,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &in_result);
    if (err) return err;

    extra->output->result_rect     = in_result.result_rect;
    extra->output->max_result_rect = in_result.max_result_rect;
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data,
                   PF_SmartRenderExtra* extra) {
    link_check_openexr();
    auto t_start = std::chrono::high_resolution_clock::now();

    PF_EffectWorld* output_worldP = nullptr;
    PF_Err err = extra->cb->checkout_output(in_data->effect_ref, &output_worldP);
    if (err || !output_worldP) return err;

    // Read each param at the current time. Tiny values, simpler to
    // checkout/checkin here than to stash through pre_render_data.
    auto read_double = [&](PF_ParamIndex idx, double& out) {
        PF_ParamDef p; AEFX_CLR_STRUCT(p);
        if (PF_CHECKOUT_PARAM(in_data, idx, in_data->current_time,
                              in_data->time_step, in_data->time_scale,
                              &p) == PF_Err_NONE) {
            out = p.u.fs_d.value;
            PF_CHECKIN_PARAM(in_data, &p);
        }
    };
    auto read_bool = [&](PF_ParamIndex idx, bool& out) {
        PF_ParamDef p; AEFX_CLR_STRUCT(p);
        if (PF_CHECKOUT_PARAM(in_data, idx, in_data->current_time,
                              in_data->time_step, in_data->time_scale,
                              &p) == PF_Err_NONE) {
            out = p.u.bd.value != 0;
            PF_CHECKIN_PARAM(in_data, &p);
        }
    };

    double hi_d = 0, lo_d = 0, black = 0, white = 1;
    bool unmult = false;
    read_double(PARAM_HASH_HI, hi_d);
    read_double(PARAM_HASH_LO, lo_d);
    read_double(PARAM_BLACK,   black);
    read_double(PARAM_WHITE,   white);
    read_bool  (PARAM_UNMULT,  unmult);

    uint32_t hash = ((static_cast<uint32_t>(hi_d) & 0xFFFFu) << 16)
                  |  (static_cast<uint32_t>(lo_d) & 0xFFFFu);

    PF_PixelFormat fmt = GetOutputPixelFormat(in_data, out_data, output_worldP);
    RenderTimings rt;
    const char* path_kind = "?";
    std::string layer_used;
    PF_Err render_err = RenderHash(in_data, out_data, hash,
                                   black, white, unmult,
                                   output_worldP, fmt,
                                   &rt, &path_kind, &layer_used);

    auto total = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - t_start).count();
    char selector[32];
    std::snprintf(selector, sizeof(selector), "SmartRender(%s)", path_kind);
    LogPerf(selector, total, rt, path_kind, layer_used);
    return render_err;
}

}  // namespace

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;
    // matchName: AE caps at 31 chars (PF_MAX_EFFECT_NAME_LEN). Must match
    // the AE_Effect_Match_Name in exrdemux.r exactly. Stable across releases.
    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "EXRDemux",
        "tdcarney EXRDemux",
        "EXR",
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com/thedavidcarney/EXRDemux");
    return result;
}

extern "C" DllExport
PF_Err EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra)
{
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETUP:
                err = SequenceSetup(in_data, out_data);
                break;
            case PF_Cmd_USER_CHANGED_PARAM:
                err = UserChangedParam(in_data, out_data, params,
                    reinterpret_cast<const PF_UserChangedParamExtra*>(extra));
                break;
            case PF_Cmd_UPDATE_PARAMS_UI:
                err = UpdateParamsUI(in_data, out_data, params);
                break;
            case PF_Cmd_RENDER:
                err = Render(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data,
                                reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data,
                                  reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
            default:
                break;
        }
    } catch (const PF_Err& thrown) {
        err = thrown;
    }
    return err;
}
