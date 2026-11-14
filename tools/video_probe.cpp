// ============================================================================
// video_probe.cpp -- local H.264 smoke feed: verifies the "decode -> letterbox present" path
// ============================================================================
// No protocol, no network, no Bluetooth: feed raw AnnexB H.264 straight into SdlVideoSink and
// visually confirm decoding works, aspect ratio is right, and frame rate keeps up.
//
// It is the **bisection point** for debugging "black screen" (its main value):
//   Picture appears -> problem is on the protocol side (RTSP / encryption / framing / config-frame order)
//   No picture -> problem is on the decode side (extradata / pixel format / textures)
//
// Generate a test stream (any container with H.264 works):
//   ffmpeg -i in.mp4 -c:v libx264 -bsf:v h264_mp4toannexb -an out.h264
//
// Usage:
//   video_probe --h264 out.h264 [--fps 30] [--loop] [--reset-at 60] [--frames N]
#include "annexb.h"
#include "media/sdl_video_sink.h"
#include "screenshot.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// AnnexB parsing lives in tools/annexb.h -- shared with the fake phone so both interpret the stream identically
// (the "probe shows a picture but the real device is black" bisection only means something when both sides share code).
using allplay::annexb::buildAvcC;
using allplay::annexb::NalRef;
using allplay::annexb::scanNals;
using allplay::annexb::splitAus;

namespace {

struct Options {
    std::string h264;              // --h264, required (raw AnnexB elementary stream)
    int         fps      = 30;
    bool        loop     = false;
    int         resetAt  = 0;      // >0: call sink.reset() from another thread at the Nth AU
    int         frames   = 0;      // >0: exit after rendering N frames (for container smoke tests)
    int         winW     = 1280;
    int         winH     = 720;
    std::string dump;              // --dump, save the last rendered frame as PPM before exit
    bool        help     = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "Usage: %s --h264 FILE [options]\n"
        "  --h264 PATH       raw AnnexB H.264 elementary stream (required)\n"
        "  --fps N           feed at N frames/sec (default 30)\n"
        "  --loop            replay from the start when finished\n"
        "  --reset-at N      call reset() from another thread at access unit N to verify keyframe recovery\n"
        "  --frames N        exit after rendering N frames (headless smoke test, e.g. SDL_VIDEODRIVER=dummy)\n"
        "  --window WxH      window size (default 1280x720)\n"
        "  --dump PATH       save the last rendered frame as PPM before exiting (for automated color checks)\n"
        "  -h, --help        show this help\n"
        "\n"
        "Exit: window closed / ESC / playback finished (without --loop).\n",
        argv0);
}

bool parseScreen(const char* s, int* w, int* h)
{
    return std::sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "argument %s is missing a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--h264")) {
            o->h264 = need(a);
        } else if (!std::strcmp(a, "--fps")) {
            o->fps = std::atoi(need(a));
            if (o->fps <= 0 || o->fps > 240) {
                std::fprintf(stderr, "--fps out of range (1..240)\n");
                return false;
            }
        } else if (!std::strcmp(a, "--loop")) {
            o->loop = true;
        } else if (!std::strcmp(a, "--reset-at")) {
            o->resetAt = std::atoi(need(a));
        } else if (!std::strcmp(a, "--frames")) {
            o->frames = std::atoi(need(a));
        } else if (!std::strcmp(a, "--window")) {
            if (!parseScreen(need(a), &o->winW, &o->winH)) {
                std::fprintf(stderr, "--window requires WxH, e.g. 1280x720\n");
                return false;
            }
        } else if (!std::strcmp(a, "--dump")) {
            o->dump = need(a);
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            o->help = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s (see -h for help)\n", a);
            return false;
        }
    }
    if (!o->help && o->h264.empty()) {
        std::fprintf(stderr, "--h264 FILE is required (see -h for help)\n");
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fprintf(stderr, "%s is an empty file\n", path.c_str());
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = std::fread(out->data(), 1, size_t(n), f);
    std::fclose(f);
    if (got != size_t(n)) {
        std::fprintf(stderr, "read %s got only %zu/%ld bytes\n", path.c_str(), got, n);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 2;
    if (opt.help) {
        printUsage(argv[0]);
        return 0;
    }

    std::vector<uint8_t> buf;
    if (!readFile(opt.h264, &buf)) return 1;

    const auto nals = scanNals(buf);
    const auto aus  = splitAus(buf, nals);
    if (aus.empty()) {
        std::fprintf(stderr, "no start codes found in %s -- not an AnnexB stream?\n", opt.h264.c_str());
        return 1;
    }

    std::vector<uint8_t> avcC;
    std::string err;
    if (!buildAvcC(buf, nals, &avcC, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("[probe] %s: %zu bytes, %zu NALs, %zu access units, avcC %zu bytes\n",
                opt.h264.c_str(), buf.size(), nals.size(), aus.size(), avcC.size());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[probe] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("AllPlay video_probe",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       opt.winW, opt.winH, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "[probe] window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    bool vsync = true;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        vsync = false;
    }
    if (!ren) {
        std::fprintf(stderr, "[probe] renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    allplay::SdlVideoSink sink;
    // width/height are just negotiated values; the decoder does not rely on them (real size comes from SPS)
    sink.configure(core::VideoCodec::H264, opt.winW, opt.winH, avcC);

    const double frameMs = 1000.0 / double(opt.fps);
    Uint64       t0      = SDL_GetTicks64();
    Uint64       base    = t0;
    size_t       au      = 0;
    size_t       fedTotal = 0;
    bool         fedAll  = false;
    Uint64       doneAt  = 0;
    Uint64       feedEnd = 0;   ///< when feeding finished (for decode rate; excludes the final display tail)
    bool         running  = true;
    bool         reported = false;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        // -- feed frames at the target frame rate --
        const Uint64 now = SDL_GetTicks64();
        while (!fedAll && now >= base + Uint64(double(au) * frameMs)) {
            if (opt.resetAt > 0 && int(au) == opt.resetAt) {
                std::printf("[probe] AU %zu: calling reset() from another thread (simulating a keyframe request from the RTSP thread)\n", au);
                std::thread([&sink] { sink.reset(); }).join();
            }
            const auto& r = aus[au];
            sink.feedAnnexB(buf.data() + r.first, r.second - r.first);
            ++fedTotal;
            if (opt.frames > 0 && fedTotal >= size_t(opt.frames) && feedEnd == 0) feedEnd = now;
            if (++au >= aus.size()) {
                fedAll = true;
                doneAt = now;
                if (opt.loop) {
                    au = 0;
                    base = now;
                    fedAll = false;
                }
            }
        }
        // Feeding done and not looping -> linger 400ms to finish displaying the last frame, then exit
        if (fedAll && now - doneAt > 400) running = false;

        // -- present --
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        int vw = 0, vh = 0;
        sink.intrinsicSize(&vw, &vh);
        int winW = 0, winH = 0;
        SDL_GetRendererOutputSize(ren, &winW, &winH);
        SDL_Rect dst;
        allplay::fitLetterbox(vw, vh, winW, winH, &dst);
        sink.present(ren, dst);
        SDL_RenderPresent(ren);

        if (!reported && vw > 0 && vh > 0) {
            std::printf("[probe] decoded first frame: %dx%d (letterbox placement %d,%d %dx%d)\n",
                        vw, vh, dst.x, dst.y, dst.w, dst.h);
            reported = true;
        }

        // --frames counts **video frames fed in**, not render-loop iterations -- those vary with the
        // render backend (vsync or not), and a smoke test on a software-rendered machine would quietly "pass" after a few frames.
        if (opt.frames > 0 && fedTotal >= size_t(opt.frames)) running = false;
        if (!vsync) SDL_Delay(2);   // software/dummy renderer -- don't spin the CPU flat out
    }

    if (!opt.dump.empty()) allplay::dumpRender(ren, opt.dump);

    int fw = 0, fh = 0;
    sink.intrinsicSize(&fw, &fh);
    const Uint64 elapsed = SDL_GetTicks64() - t0;
    std::printf("[probe] fed %zu/%zu AUs: decoded %llu frames, shallow queue dropped %llu, presented %llu, %s, took %llu ms\n",
                au, aus.size(), (unsigned long long)sink.decoded(),
                (unsigned long long)sink.dropped(),
                (unsigned long long)sink.presented(),
                sink.configured() ? "decoder OK" : "decoder not configured",
                (unsigned long long)elapsed);
    // Judge performance by **decode rate** (time spent feeding frames), not by present fps:
    // present rate is gated by the render backend (absurdly slow when headless/software-rendered), so using it to judge
    // "can this CPU software-decode" blames rendering on the decoder. Dropped frames are the shallow queue's design, not a decode failure.
    const Uint64 feedMs = feedEnd > t0 ? feedEnd - t0 : elapsed;
    if (feedMs > 0) {
        std::printf("[probe] decode %.1f fps (fed %zu frames in %llu ms); present %.1f fps\n",
                    double(sink.decoded()) * 1000.0 / double(feedMs),
                    fedTotal, (unsigned long long)feedMs,
                    double(sink.presented()) * 1000.0 / double(elapsed));
    }
    if (fw == 0) std::printf("[probe] ⚠️ no frames decoded (%dx%d) -- check above for decoder errors\n", fw, fh);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return sink.presented() > 0 ? 0 : 1;
}
