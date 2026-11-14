#include "exrdemux_pass_cache.h"

#include <ImfMultiPartInputFile.h>
#include <ImfInputPart.h>
#include <ImfFrameBuffer.h>
#include <ImathBox.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace exrdemux {

namespace {

struct Stamp {
    int64_t mtime = 0;
    int64_t size  = 0;
    bool    valid = false;
    bool    settled = false;   // unmodified for at least the settle time
    bool operator==(const Stamp& o) const {
        return valid && o.valid && mtime == o.mtime && size == o.size;
    }
};

double g_settle_seconds = 2.0;

Stamp StampOf(const std::string& path) {
    Stamp s;
    try {
        std::error_code ec;
        std::filesystem::path p(path);
        if (!std::filesystem::is_regular_file(p, ec) || ec) return s;
        auto ftime = std::filesystem::last_write_time(p, ec);
        if (ec) return s;
        auto sz = std::filesystem::file_size(p, ec);
        if (ec) return s;
        s.mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
        s.size  = static_cast<int64_t>(sz);
        s.valid = true;
        auto age = std::filesystem::file_time_type::clock::now() - ftime;
        s.settled = std::chrono::duration<double>(age).count() >= g_settle_seconds;
    } catch (...) {
        // std::filesystem can throw on odd paths on Windows; treat as no-stamp.
    }
    return s;
}

// One pass slot. `ready` flips once the decoding thread is done; other
// threads asking for the same pass meanwhile wait on `cv` instead of
// decoding it again.
struct Slot {
    bool ready = false;
    std::shared_ptr<const PassPixels> pixels;   // null = decode failed
    std::string error;
    size_t   bytes = 0;
    uint64_t last_use = 0;
};

// All cached passes of one file, valid for exactly one file stamp.
struct FileEntry {
    Stamp stamp;
    std::map<std::string, std::shared_ptr<Slot>> passes;
};

std::mutex              g_mutex;
std::condition_variable g_cv;
std::map<std::string, FileEntry> g_files;
PassCacheStats          g_stats;
uint64_t                g_tick = 0;
bool                    g_budget_set = false;

size_t DefaultBudget() {
    if (const char* on = std::getenv("EXRDEMUX_PASS_CACHE")) {
        if (on[0] == '0' && on[1] == '\0') return 0;
    }
    if (const char* mb = std::getenv("EXRDEMUX_CACHE_MB")) {
        char* end = nullptr;
        unsigned long long v = std::strtoull(mb, &end, 10);
        if (end != mb) return static_cast<size_t>(v) * 1024ull * 1024ull;
    }
    return size_t(8) * 1024 * 1024 * 1024;
}

size_t BudgetLocked() {
    if (!g_budget_set) { g_stats.budget = DefaultBudget(); g_budget_set = true; }
    return g_stats.budget;
}

// Drop least-recently-used finished passes until under budget. Renders
// still holding a pass keep it alive through their shared_ptr.
void EvictLocked() {
    size_t budget = BudgetLocked();
    while (g_stats.bytes > budget) {
        FileEntry* victim_file = nullptr;
        std::map<std::string, std::shared_ptr<Slot>>::iterator victim;
        uint64_t oldest = UINT64_MAX;
        for (auto& f : g_files) {
            for (auto it = f.second.passes.begin(); it != f.second.passes.end(); ++it) {
                const Slot& s = *it->second;
                if (s.ready && s.bytes > 0 && s.last_use < oldest) {
                    oldest = s.last_use; victim_file = &f.second; victim = it;
                }
            }
        }
        if (!victim_file) return;
        g_stats.bytes -= victim->second->bytes;
        victim_file->passes.erase(victim);
        ++g_stats.evictions;
    }
}

void DropFileLocked(std::map<std::string, FileEntry>::iterator it) {
    for (auto& kv : it->second.passes) g_stats.bytes -= kv.second->bytes;
    g_files.erase(it);
    ++g_stats.invalidations;
}

// Same decode as the file-direct render: OpenEXR into HALF slices, so the
// values are identical to what that path always produced.
std::shared_ptr<PassPixels> Decode(const std::string& path,
                                   const PassChannels& ch,
                                   std::string* error) {
    try {
        Imf::MultiPartInputFile file(path.c_str());
        if (ch.part_idx < 0 || ch.part_idx >= file.parts()) {
            *error = "part index out of range";
            return nullptr;
        }
        const Imf::Header& header = file.header(ch.part_idx);
        Imath::Box2i dw   = header.dataWindow();
        Imath::Box2i disp = header.displayWindow();
        int w = dw.max.x - dw.min.x + 1;
        int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) { *error = "empty data window"; return nullptr; }

        auto px = std::make_shared<PassPixels>();
        px->width = w;
        px->height = h;
        px->offset_x = dw.min.x - disp.min.x;
        px->offset_y = dw.min.y - disp.min.y;
        px->argb.assign(static_cast<size_t>(w) * h * 4, half(0.0f));
        for (size_t i = 0; i < px->argb.size(); i += 4) px->argb[i] = half(1.0f);

        const size_t xs = 4 * sizeof(half);
        const size_t ys = xs * static_cast<size_t>(w);
        Imf::FrameBuffer fb;
        auto attach = [&](const std::string& name, int comp) {
            char* base = reinterpret_cast<char*>(px->argb.data() + comp)
                       - (static_cast<ptrdiff_t>(dw.min.x) * xs
                          + static_cast<ptrdiff_t>(dw.min.y) * ys);
            fb.insert(name.c_str(), Imf::Slice(Imf::HALF, base, xs, ys));
        };
        if (ch.has_a) attach(ch.a_name, 0);
        if (ch.has_r) attach(ch.r_name, 1);
        if (ch.has_g) attach(ch.g_name, 2);
        if (ch.has_b) attach(ch.b_name, 3);

        Imf::InputPart input(file, ch.part_idx);
        input.setFrameBuffer(fb);
        input.readPixels(dw.min.y, dw.max.y);
        return px;
    } catch (const std::exception& e) {
        *error = e.what();
    } catch (...) {
        *error = "unknown decoder error";
    }
    return nullptr;
}

}  // namespace

const char* PassResultName(PassResult r) {
    switch (r) {
        case PassResult::kHit:         return "hit";
        case PassResult::kDecoded:     return "decoded";
        case PassResult::kDisabled:    return "disabled";
        case PassResult::kUnstable:    return "unstable";
        case PassResult::kDecodeError: return "decode-error";
    }
    return "?";
}

std::shared_ptr<const PassPixels> AcquirePass(
        const std::string& path, const std::string& layer_name,
        const PassChannels& channels,
        PassResult* result_out, std::string* decoder_error_out) {
    auto result = [&](PassResult r) { if (result_out) *result_out = r; };

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (BudgetLocked() == 0) { result(PassResult::kDisabled); return nullptr; }
    }
    if (path.empty() || layer_name.empty() || channels.part_idx < 0) {
        result(PassResult::kUnstable);
        return nullptr;
    }

    // Stat on every acquire: this is what makes an in-place re-render show
    // up on the very next render.
    Stamp stamp = StampOf(path);

    std::shared_ptr<Slot> slot;
    bool decode_here = false;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        auto fit = g_files.find(path);
        if (fit != g_files.end() && !(fit->second.stamp == stamp)) {
            DropFileLocked(fit);     // file changed (or vanished): forget it all
            fit = g_files.end();
        }
        if (!stamp.valid || !stamp.settled) {
            result(PassResult::kUnstable);
            return nullptr;
        }
        if (fit == g_files.end()) {
            fit = g_files.emplace(path, FileEntry{}).first;
            fit->second.stamp = stamp;
        }
        auto& passes = fit->second.passes;
        auto pit = passes.find(layer_name);
        if (pit == passes.end()) {
            slot = std::make_shared<Slot>();
            passes.emplace(layer_name, slot);
            decode_here = true;
        } else {
            slot = pit->second;
        }
        slot->last_use = ++g_tick;

        if (!decode_here) {
            g_cv.wait(lock, [&] { return slot->ready; });
            if (!slot->pixels) {
                if (decoder_error_out) *decoder_error_out = slot->error;
                result(slot->error.empty() ? PassResult::kUnstable
                                           : PassResult::kDecodeError);
                return nullptr;
            }
            ++g_stats.hits;
            result(PassResult::kHit);
            return slot->pixels;
        }
    }

    // Decode outside the lock so other passes keep flowing.
    std::string error;
    std::shared_ptr<PassPixels> px = Decode(path, channels, &error);
    Stamp after = StampOf(path);
    bool stable = (after == stamp);

    std::lock_guard<std::mutex> lock(g_mutex);
    slot->ready = true;
    if (!stable) {
        // Written to while we read it. Don't keep anything for this stamp.
        auto fit = g_files.find(path);
        if (fit != g_files.end() && fit->second.stamp == stamp) DropFileLocked(fit);
        g_cv.notify_all();
        result(PassResult::kUnstable);
        return nullptr;
    }
    if (!px) {
        slot->error = error;          // remembered for this file version
        g_cv.notify_all();
        if (decoder_error_out) *decoder_error_out = error;
        result(PassResult::kDecodeError);
        return nullptr;
    }
    slot->pixels = px;
    slot->bytes  = px->bytes();
    // The slot may already be gone if the file entry was dropped while we
    // decoded; only count bytes for slots still in the map.
    auto fit = g_files.find(path);
    if (fit != g_files.end() && fit->second.stamp == stamp &&
        fit->second.passes.count(layer_name) &&
        fit->second.passes[layer_name] == slot) {
        g_stats.bytes += slot->bytes;
        if (g_stats.bytes > g_stats.peak_bytes) g_stats.peak_bytes = g_stats.bytes;
        ++g_stats.decodes;
        EvictLocked();
    } else {
        slot->bytes = 0;
    }
    g_cv.notify_all();
    result(PassResult::kDecoded);
    return px;
}

PassCacheStats GetPassCacheStats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    BudgetLocked();
    PassCacheStats s = g_stats;
    s.entries = 0;
    for (const auto& f : g_files) s.entries += f.second.passes.size();
    return s;
}

void SetPassCacheBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.budget = bytes;
    g_budget_set = true;
    EvictLocked();
}

size_t PassCacheBudget() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return BudgetLocked();
}

void SetPassCacheSettleSeconds(double seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settle_seconds = seconds;
}

void ClearPassCache() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_files.clear();
    g_stats.bytes = 0;
}

}  // namespace exrdemux
