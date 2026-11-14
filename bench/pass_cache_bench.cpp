// Bench + correctness checks for the shared pass cache, outside AE.
//
//   pass_cache_bench speed  <file.exr> [layers=120] [frames=10] [threads=16] [mode=cache|nocache]
//       Simulates a chase comp: `layers` effect instances spread over the
//       file's RGBA passes, rendering `frames` frames of a still on
//       `threads` threads. nocache = what the file-direct path did before
//       (decode the pass per layer per frame); cache = shared pass cache.
//       Prints per-frame time and peak memory. Run each mode in its own
//       process so the peak-memory numbers don't mix.
//
//   pass_cache_bench identical <file.exr>
//       Every pass from the cache vs. an independent decode done the way
//       the file-direct render does it: must match bit for bit.
//
//   pass_cache_bench rerender <a.exr> <b.exr> <scratch.exr>
//       Copies a -> scratch, fills the cache, overwrites scratch with b in
//       place (same path, same pass names), and checks that the next
//       acquire returns b's pixels. Also checks the settle guard: a file
//       written moments ago is not cached.

#include "exrdemux_pass_cache.h"

#include <ImfMultiPartInputFile.h>
#include <ImfInputPart.h>
#include <ImfFrameBuffer.h>
#include <ImfChannelList.h>
#include <ImathBox.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

using namespace exrdemux;
using clk = std::chrono::steady_clock;

namespace {

double PeakMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
#else
    return 0;
#endif
}

struct Pass { std::string name; PassChannels ch; };

// RGBA passes, the ones AE's channel suite exposes (the production case).
std::vector<Pass> RgbaPasses(const std::string& path) {
    std::vector<Pass> out;
    std::map<std::string, Pass> by;
    std::vector<std::string> order;
    Imf::MultiPartInputFile f(path.c_str());
    for (int p = 0; p < f.parts(); ++p) {
        const Imf::ChannelList& cl = f.header(p).channels();
        for (auto it = cl.begin(); it != cl.end(); ++it) {
            std::string n = it.name();
            size_t dot = n.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string layer = n.substr(0, dot), suf = n.substr(dot + 1);
            if (!by.count(layer)) { by[layer].name = layer; by[layer].ch.part_idx = p; order.push_back(layer); }
            PassChannels& c = by[layer].ch;
            if      (suf == "R" || suf == "r") { c.r_name = n; c.has_r = true; }
            else if (suf == "G" || suf == "g") { c.g_name = n; c.has_g = true; }
            else if (suf == "B" || suf == "b") { c.b_name = n; c.has_b = true; }
            else if (suf == "A" || suf == "a") { c.a_name = n; c.has_a = true; }
        }
    }
    for (auto& n : order) {
        const PassChannels& c = by[n].ch;
        if (c.has_r && c.has_g && c.has_b && c.has_a) out.push_back(by[n]);
    }
    return out;
}

// The file-direct render's decode, as it was before the cache: planar
// HALF buffers, A defaults to 1.
struct Planar { int w = 0, h = 0; std::vector<half> r, g, b, a; };
Planar DecodePlanar(const std::string& path, const PassChannels& ch) {
    Planar out;
    Imf::MultiPartInputFile file(path.c_str());
    const Imf::Header& header = file.header(ch.part_idx);
    Imath::Box2i dw = header.dataWindow();
    int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    out.w = w; out.h = h;
    size_t n = size_t(w) * h;
    out.r.assign(n, half(0.0f)); out.g.assign(n, half(0.0f));
    out.b.assign(n, half(0.0f)); out.a.assign(n, half(1.0f));
    Imf::FrameBuffer fb;
    auto attach = [&](const std::string& name, std::vector<half>& buf) {
        char* base = reinterpret_cast<char*>(buf.data()) - (dw.min.x + dw.min.y * w) * sizeof(half);
        fb.insert(name.c_str(), Imf::Slice(Imf::HALF, base, sizeof(half), sizeof(half) * size_t(w)));
    };
    if (ch.has_r) attach(ch.r_name, out.r);
    if (ch.has_g) attach(ch.g_name, out.g);
    if (ch.has_b) attach(ch.b_name, out.b);
    if (ch.has_a) attach(ch.a_name, out.a);
    Imf::InputPart input(file, ch.part_idx);
    input.setFrameBuffer(fb);
    input.readPixels(dw.min.y, dw.max.y);
    return out;
}

// Stand-in for the per-layer output write (32 bpc, identity adjust): what
// each layer does with its pass once it has one.
void ConvertToFloat(const half* argb, int w, int h, std::vector<float>& out) {
    out.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(argb[i]);
}
void ConvertPlanar(const Planar& p, std::vector<float>& out) {
    out.resize(size_t(p.w) * p.h * 4);
    for (size_t i = 0, n = size_t(p.w) * p.h; i < n; ++i) {
        out[i * 4 + 0] = p.a[i]; out[i * 4 + 1] = p.r[i];
        out[i * 4 + 2] = p.g[i]; out[i * 4 + 3] = p.b[i];
    }
}

bool SameAsPlanar(const PassPixels& px, const Planar& p, size_t* diffs_out) {
    size_t diffs = 0;
    if (px.width != p.w || px.height != p.h) { *diffs_out = SIZE_MAX; return false; }
    for (size_t i = 0, n = size_t(p.w) * p.h; i < n; ++i) {
        const half* c = &px.argb[i * 4];
        diffs += c[0].bits() != p.a[i].bits();
        diffs += c[1].bits() != p.r[i].bits();
        diffs += c[2].bits() != p.g[i].bits();
        diffs += c[3].bits() != p.b[i].bits();
    }
    *diffs_out = diffs;
    return diffs == 0;
}

int Speed(int argc, char** argv) {
    std::string path = argv[2];
    int layers  = argc > 3 ? std::atoi(argv[3]) : 120;
    int frames  = argc > 4 ? std::atoi(argv[4]) : 10;
    int threads = argc > 5 ? std::atoi(argv[5]) : 16;
    bool cache  = !(argc > 6 && std::string(argv[6]) == "nocache");
    SetPassCacheSettleSeconds(0);

    std::vector<Pass> passes = RgbaPasses(path);
    if (passes.empty()) { std::printf("no RGBA passes\n"); return 1; }
    std::printf("%s: %zu RGBA passes, %d layers, %d frames, %d threads, mode=%s\n",
                std::filesystem::path(path).filename().string().c_str(),
                passes.size(), layers, frames, threads, cache ? "cache" : "nocache");

    double total_s = 0;
    for (int f = 0; f < frames; ++f) {
        std::atomic<int> next{0};
        auto t0 = clk::now();
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
            std::vector<float> out;
            for (int l; (l = next++) < layers;) {
                const Pass& p = passes[size_t(l) % passes.size()];
                if (cache) {
                    auto px = AcquirePass(path, p.name, p.ch);
                    if (px) ConvertToFloat(px->argb.data(), px->width, px->height, out);
                } else {
                    ConvertPlanar(DecodePlanar(path, p.ch), out);
                }
            }
        });
        for (auto& th : pool) th.join();
        double s = std::chrono::duration<double>(clk::now() - t0).count();
        total_s += s;
        std::printf("  frame %2d: %7.3f s\n", f + 1, s);
    }
    PassCacheStats st = GetPassCacheStats();
    std::printf("mean %.3f s/frame   peak working set %.0f MB   cache %.0f MB in %zu passes (decodes=%llu hits=%llu)\n",
                total_s / frames, PeakMB(), st.bytes / 1048576.0, st.entries,
                (unsigned long long)st.decodes, (unsigned long long)st.hits);
    return 0;
}

int Identical(char** argv) {
    std::string path = argv[2];
    SetPassCacheSettleSeconds(0);
    std::vector<Pass> passes = RgbaPasses(path);
    int bad = 0;
    for (const Pass& p : passes) {
        auto px = AcquirePass(path, p.name, p.ch);
        size_t diffs = 0;
        bool ok = px && SameAsPlanar(*px, DecodePlanar(path, p.ch), &diffs);
        if (!ok) { ++bad; std::printf("  %-30s DIFFERS (%zu)\n", p.name.c_str(), diffs); }
        ClearPassCache();   // keep memory flat
    }
    std::printf("identical: %zu passes checked, %d differ\n", passes.size(), bad);
    return bad ? 1 : 0;
}

int Rerender(char** argv) {
    namespace fs = std::filesystem;
    std::string a = argv[2], b = argv[3], scratch = argv[4];
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fails;
    };

    fs::copy_file(a, scratch, fs::copy_options::overwrite_existing);
    // Copies keep the source mtime; stamp it "now - 1 h" so it is settled.
    fs::last_write_time(scratch, fs::file_time_type::clock::now() - std::chrono::hours(1));
    SetPassCacheSettleSeconds(2);

    std::vector<Pass> passes = RgbaPasses(scratch);
    const Pass& p = passes[passes.size() / 2];
    PassResult r;
    auto first = AcquirePass(scratch, p.name, p.ch, &r);
    check(first && r == PassResult::kDecoded, "first acquire decodes A");
    auto again = AcquirePass(scratch, p.name, p.ch, &r);
    check(again && r == PassResult::kHit && again == first, "second acquire is a cache hit");

    // Re-render in place: same path, same pass names, new pixels.
    fs::copy_file(b, scratch, fs::copy_options::overwrite_existing);
    fs::last_write_time(scratch, fs::file_time_type::clock::now());
    auto fresh = AcquirePass(scratch, p.name, p.ch, &r);
    check(!fresh && r == PassResult::kUnstable,
          "just-written file is not cached (caller renders uncached = new file)");
    check(GetPassCacheStats().entries == 0, "old file's passes dropped at once");

    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    auto settled = AcquirePass(scratch, p.name, p.ch, &r);
    size_t diffs = 0;
    check(settled && r == PassResult::kDecoded &&
          SameAsPlanar(*settled, DecodePlanar(b, p.ch), &diffs),
          "after settling: cache holds B's pixels bit for bit");
    size_t diffs_a = 0;
    check(settled && !SameAsPlanar(*settled, DecodePlanar(a, p.ch), &diffs_a),
          "and they are not A's pixels (test files differ)");

    // A re-render that lands with the same size: mtime alone must catch it.
    fs::copy_file(a, scratch, fs::copy_options::overwrite_existing);
    fs::last_write_time(scratch, fs::file_time_type::clock::now() - std::chrono::minutes(30));
    auto back = AcquirePass(scratch, p.name, p.ch, &r);
    check(back && r == PassResult::kDecoded &&
          SameAsPlanar(*back, DecodePlanar(a, p.ch), &diffs),
          "overwrite with A again (older mtime): cache returns A");

    // Many threads hitting a just-changed file at once decode it only once.
    ClearPassCache();
    PassCacheStats before = GetPassCacheStats();
    std::vector<std::thread> pool;
    std::atomic<int> got{0};
    for (int t = 0; t < 32; ++t) pool.emplace_back([&] {
        if (AcquirePass(scratch, p.name, p.ch)) ++got;
    });
    for (auto& th : pool) th.join();
    PassCacheStats after = GetPassCacheStats();
    check(got == 32 && after.decodes - before.decodes == 1,
          "32 concurrent requests -> 1 decode, 32 results");

    fs::remove(scratch);
    std::printf("rerender: %d failures\n", fails);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "speed")     return Speed(argc, argv);
    if (argc >= 3 && std::string(argv[1]) == "identical") return Identical(argv);
    if (argc >= 5 && std::string(argv[1]) == "rerender")  return Rerender(argv);
    std::fprintf(stderr, "usage: see header of pass_cache_bench.cpp\n");
    return 2;
}
