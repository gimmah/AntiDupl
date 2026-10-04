// Decoder benchmark harness (Linux/macOS, no Windows dependencies).
//
// Mirrors the per-image work done by TDataCollector::FillPixelData in
// src/AntiDupl: read file into memory -> probe header -> decode to a 32-bit
// view -> convert to gray. Decoder calls match the ones used by the core:
//   JPEG : libjpeg-turbo  tjDecompress2(TJPF_RGBA)        (adTurboJpeg.cpp)
//   WEBP : libwebp        WebPDecodeBGRAInto              (adWebp.cpp)
//   PNG  : libpng  (stand-in for GDI+ on Windows)         (adGdiplus.cpp)
//   BMP  : minimal 24/32-bit reader (stand-in for GDI+)   (adGdiplus.cpp)
//
// Variants (--opt, comma separated; "none" = baseline behaviour):
//   fastjpeg  TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT for JPEG
//   reuse     per-thread reusable decode buffer instead of malloc/free per image
//   earlyfree release the 32-bit view as soon as gray is computed
//   seqread   posix_fadvise(SEQUENTIAL) + single read() for file loading
//
// Output: one JSON document on stdout.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <png.h>
#include <turbojpeg.h>
#include <webp/decode.h>

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Opts
{
    bool fastjpeg = false, reuse = false, earlyfree = false, seqread = false;
};

enum Fmt { JPEG = 0, PNG, WEBP, BMP, UNKNOWN, FMT_COUNT };
static const char *FMT_NAME[] = {"JPEG", "PNG", "WEBP", "BMP", "UNKNOWN"};

struct Stage
{
    double read = 0, probe = 0, decode = 0, gray = 0;
    uint64_t count = 0, bytes = 0, pixels = 0, fails = 0;
    uint64_t grayHash = 0;
};

static Fmt Detect(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return JPEG;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return PNG;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return WEBP;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return BMP;
    return UNKNOWN;
}

// Per-thread scratch: file buffer, 32-bit pixel buffer, gray buffer.
struct Scratch
{
    std::vector<uint8_t> file;
    uint8_t *pix = nullptr; size_t pixCap = 0;
    std::vector<uint8_t> gray;
    tjhandle tj = nullptr;
    ~Scratch() { free(pix); if (tj) tjDestroy(tj); }

    // Returns a buffer of at least `size` bytes. With `reuse` it is kept
    // between calls; otherwise it is a fresh allocation every time (as the
    // core does when it creates a TView per image) released by Release().
    uint8_t *Pixels(size_t size, bool reuse)
    {
        if (reuse) {
            if (size > pixCap) { free(pix); pix = (uint8_t *)aligned_alloc(64, (size + 63) & ~size_t(63)); pixCap = size; }
            return pix;
        }
        return (uint8_t *)aligned_alloc(64, (size + 63) & ~size_t(63));
    }
    void Release(uint8_t *p, bool reuse) { if (!reuse) free(p); }
};

static bool ReadFile(const char *path, Scratch &s, bool seq)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return false; }
    size_t size = (size_t)st.st_size;
    if (seq) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    s.file.resize(size);
    size_t got = 0;
    if (seq) {
        while (got < size) { ssize_t r = read(fd, s.file.data() + got, size - got); if (r <= 0) break; got += (size_t)r; }
    } else {
        // 64 KiB chunks, like a naive buffered loop.
        while (got < size) { ssize_t r = read(fd, s.file.data() + got, std::min<size_t>(65536, size - got)); if (r <= 0) break; got += (size_t)r; }
    }
    close(fd);
    return got == size;
}

// Decodes to a 4-byte/pixel buffer (stride = w*4). Returns buffer or null.
static uint8_t *Decode(Fmt f, const uint8_t *d, size_t n, Scratch &s, const Opts &o,
                       int &w, int &h, bool &fromScratch)
{
    fromScratch = o.reuse;
    switch (f) {
    case JPEG: {
        if (!s.tj) s.tj = tjInitDecompress();
        int sub, cs;
        if (tjDecompressHeader3(s.tj, d, (unsigned long)n, &w, &h, &sub, &cs) != 0 || !w || !h) return nullptr;
        uint8_t *p = s.Pixels((size_t)w * h * 4, o.reuse);
        int flags = o.fastjpeg ? (TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT) : 0;
        if (tjDecompress2(s.tj, d, n, p, w, 0, h, TJPF_RGBA, flags) != 0 && tjGetErrorCode(s.tj) != TJERR_WARNING) {
            s.Release(p, o.reuse); return nullptr;
        }
        return p;
    }
    case WEBP: {
        WebPBitstreamFeatures ft;
        if (WebPGetFeatures(d, n, &ft) != VP8_STATUS_OK) return nullptr;
        w = ft.width; h = ft.height;
        size_t sz = (size_t)w * h * 4;
        uint8_t *p = s.Pixels(sz, o.reuse);
        if (!WebPDecodeBGRAInto(d, n, p, sz, w * 4)) { s.Release(p, o.reuse); return nullptr; }
        return p;
    }
    case PNG: {
        png_image img; memset(&img, 0, sizeof img); img.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&img, d, n)) return nullptr;
        img.format = PNG_FORMAT_BGRA;
        w = (int)img.width; h = (int)img.height;
        uint8_t *p = s.Pixels((size_t)w * h * 4, o.reuse);
        if (!png_image_finish_read(&img, nullptr, p, w * 4, nullptr)) { png_image_free(&img); s.Release(p, o.reuse); return nullptr; }
        png_image_free(&img);
        return p;
    }
    case BMP: {
        if (n < 54) return nullptr;
        uint32_t off = *(const uint32_t *)(d + 10);
        int32_t bw = *(const int32_t *)(d + 18), bh = *(const int32_t *)(d + 22);
        uint16_t bpp = *(const uint16_t *)(d + 28);
        if (bw <= 0 || bh == 0 || (bpp != 24 && bpp != 32)) return nullptr;
        bool bottomUp = bh > 0; int ah = bh > 0 ? bh : -bh;
        w = bw; h = ah;
        size_t stride = ((size_t)bw * bpp / 8 + 3) & ~size_t(3);
        if (off + stride * ah > n) return nullptr;
        uint8_t *p = s.Pixels((size_t)w * h * 4, o.reuse);
        for (int y = 0; y < ah; ++y) {
            const uint8_t *src = d + off + stride * (bottomUp ? ah - 1 - y : y);
            uint8_t *dst = p + (size_t)y * w * 4;
            if (bpp == 32) memcpy(dst, src, (size_t)w * 4);
            else for (int x = 0; x < w; ++x) { dst[4*x] = src[3*x]; dst[4*x+1] = src[3*x+1]; dst[4*x+2] = src[3*x+2]; dst[4*x+3] = 0xFF; }
        }
        return p;
    }
    default: return nullptr;
    }
}

static void ToGray(const uint8_t *bgra, int w, int h, std::vector<uint8_t> &gray)
{
    gray.resize((size_t)w * h);
    const uint8_t *s = bgra; uint8_t *g = gray.data();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i, s += 4)
        g[i] = (uint8_t)((29 * s[0] + 150 * s[1] + 77 * s[2] + 128) >> 8);
}

int main(int argc, char **argv)
{
    int threads = 1, repeat = 1;
    Opts o;
    std::vector<std::string> files;
    std::string listPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (a == "--list" && i + 1 < argc) listPath = argv[++i];
        else if (a == "--opt" && i + 1 < argc) {
            std::string v = argv[++i];
            o.fastjpeg = v.find("fastjpeg") != std::string::npos;
            o.reuse = v.find("reuse") != std::string::npos;
            o.earlyfree = v.find("earlyfree") != std::string::npos;
            o.seqread = v.find("seqread") != std::string::npos;
        }
    }
    if (listPath.empty()) { fprintf(stderr, "usage: decode_bench --list files.txt [--threads N] [--repeat R] [--opt a,b]\n"); return 2; }
    {
        FILE *f = fopen(listPath.c_str(), "r"); char line[4096];
        while (f && fgets(line, sizeof line, f)) { size_t l = strlen(line); while (l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l] = 0; if (l) files.push_back(line); }
        if (f) fclose(f);
    }
    std::vector<std::string> jobs;
    for (int r = 0; r < repeat; ++r) jobs.insert(jobs.end(), files.begin(), files.end());

    std::atomic<size_t> next{0};
    std::vector<std::map<int, Stage>> perThread(threads);
    std::vector<double> busy(threads, 0);

    auto t0 = Clock::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
        Scratch s;
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= jobs.size()) break;
            auto a = Clock::now();
            bool ok = ReadFile(jobs[i].c_str(), s, o.seqread);
            auto b = Clock::now();
            Fmt f = ok ? Detect(s.file.data(), s.file.size()) : UNKNOWN;
            Stage &st = perThread[t][f];
            st.read += Ms(a, b); st.count++; st.bytes += s.file.size();
            if (!ok || f == UNKNOWN) { st.fails++; continue; }
            auto c = Clock::now();
            st.probe += Ms(b, c);
            int w = 0, h = 0; bool scratchOwned;
            uint8_t *p = Decode(f, s.file.data(), s.file.size(), s, o, w, h, scratchOwned);
            auto d = Clock::now();
            st.decode += Ms(c, d);
            if (!p) { st.fails++; continue; }
            ToGray(p, w, h, s.gray);
            if (o.earlyfree) s.Release(p, o.reuse);
            auto e = Clock::now();
            st.gray += Ms(d, e);
            st.pixels += (uint64_t)w * h;
            // Checksum so the work can't be optimised away and results can be compared.
            uint64_t hsh = 1469598103934665603ULL;
            for (size_t k = 0; k < s.gray.size(); k += 997) hsh = (hsh ^ s.gray[k]) * 1099511628211ULL;
            st.grayHash ^= hsh;
            if (!o.earlyfree) s.Release(p, o.reuse);
            busy[t] += Ms(a, Clock::now());
        }
    });
    for (auto &th : pool) th.join();
    double wall = Ms(t0, Clock::now());

    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    std::map<int, Stage> tot;
    for (auto &m : perThread) for (auto &kv : m) {
        Stage &x = tot[kv.first]; const Stage &y = kv.second;
        x.read += y.read; x.probe += y.probe; x.decode += y.decode; x.gray += y.gray;
        x.count += y.count; x.bytes += y.bytes; x.pixels += y.pixels; x.fails += y.fails; x.grayHash ^= y.grayHash;
    }
    double cpu = 0; for (double b : busy) cpu += b;

    printf("{\n \"threads\": %d, \"repeat\": %d, \"images\": %zu, \"wall_ms\": %.2f, \"cpu_busy_ms\": %.2f,\n",
           threads, repeat, jobs.size(), wall, cpu);
    printf(" \"images_per_s\": %.2f, \"peak_rss_mb\": %.1f,\n", jobs.size() / (wall / 1000.0), ru.ru_maxrss / 1024.0);
    printf(" \"opts\": {\"fastjpeg\": %d, \"reuse\": %d, \"earlyfree\": %d, \"seqread\": %d},\n", o.fastjpeg, o.reuse, o.earlyfree, o.seqread);
    printf(" \"formats\": {\n");
    bool first = true;
    for (auto &kv : tot) {
        const Stage &s = kv.second;
        printf("%s  \"%s\": {\"count\": %llu, \"fails\": %llu, \"mb\": %.1f, \"mpix\": %.1f, \"read_ms\": %.2f, \"probe_ms\": %.2f, "
               "\"decode_ms\": %.2f, \"gray_ms\": %.2f, \"decode_ms_per_img\": %.3f, \"decode_mpix_per_s\": %.1f, \"gray_hash\": \"%016llx\"}",
               first ? "" : ",\n", FMT_NAME[kv.first], (unsigned long long)s.count, (unsigned long long)s.fails, s.bytes / 1e6,
               s.pixels / 1e6, s.read, s.probe, s.decode, s.gray, s.count ? s.decode / s.count : 0.0,
               s.decode > 0 ? (s.pixels / 1e6) / (s.decode / 1000.0) : 0.0, (unsigned long long)s.grayHash);
        first = false;
    }
    printf("\n }\n}\n");
    return 0;
}
