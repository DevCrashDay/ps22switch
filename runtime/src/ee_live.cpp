#include "ps2/ee_live.hpp"

#ifdef __EMSCRIPTEN__

// The browser build has no compiler to call: code the build does not know still stops the game there.
namespace ps2::ee::live {
void start(const std::string &, const std::string &, const std::string &) {}
bool active() { return false; }
bool translate_missing(std::uint32_t) { return false; }
}  // namespace ps2::ee::live

#else

#include "live_build.hpp"

#include "eerecomp_gen.hpp"
#include "ps2/ee_live_abi.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The headers the generated code includes, embedded in the runner (cmake/embed.cmake) so the library is compiled against exactly the
// layouts this runner has, whatever sits next to it.
extern const char kLiveHdrCpu[], kLiveHdrFpu[], kLiveHdrVu[], kLiveHdrMemory[], kLiveHdrMmi[], kLiveHdrGenerated[], kLiveHdrEeLiveAbi[];

namespace lb = ::ps2::live;
namespace fs = std::filesystem;

namespace ps2::ee::live {

namespace {

// Bump when recomp/codegen.cpp or recomp/analysis.cpp change what they write for the same code in a way the headers do not show: an
// old library would otherwise still be loaded (the cache also drops libraries of another runner version by the stamp).
constexpr const char *kGeneratorVersion = "ee-live 1";

using Clock = lb::Clock;

struct State {
    fs::path program;       // the game's executable
    fs::path cache;         // ee-live: the libraries
    fs::path game_dir;      // the folder port.sh made, or empty (entries.txt, overlays\)
    fs::path compiler;
    std::vector<std::uint32_t> windows;   // bases of the code windows of the program
    std::vector<std::pair<std::uint32_t, std::uint32_t>> segments;   // the code the program itself has: {address, size}
    std::uint64_t stamp = 0;
    std::vector<std::string> flags;
    std::size_t jobs = 4;
    bool keep_work = false;
    bool ready = false;     // a compiler was found and the cache folder is usable: misses can be translated
    bool headers = false;
    int failures = 0;
    std::set<std::uint32_t> failed;       // addresses that were tried and did not work: not again
    std::unique_ptr<recomp::StaticProgram> program_analysis;   // built at the first translation, for the calls into a window
    std::vector<std::uint32_t> main_entries;
    bool analysed = false;
    std::size_t libraries = 0;            // loaded from the cache or made in this run
    Clock::time_point began = Clock::now();
};

State *g = nullptr;  // never freed: a compile may still be running while the process ends

void say(const char *format, auto... args) {
    std::fprintf(stderr, "[ee-live +%.1fs] ", lb::seconds_since(g->began));
    if constexpr (sizeof...(args) == 0) std::fputs(format, stderr);
    else std::fprintf(stderr, format, args...);
    std::fprintf(stderr, "\n");
}

EeLiveHost &host() {
    static EeLiveHost h = [] {
        EeLiveHost x{};
        x.layout = static_cast<std::uint64_t>(sizeof(Cpu)) | (static_cast<std::uint64_t>(sizeof(EeLiveHost)) << 32);
        x.ram = g_ram;
        x.spr = g_spr;
        x.watch_on = g_watch_on;
        x.watch_base = g_watch_base;
        x.watch_end = g_watch_end;
        x.dispatch = &ps2::dispatch;
#define X(R, name, params, args) x.name = &ps2::name;
        PS2_EE_LIVE_FUNCTIONS(X)
#undef X
        return x;
    }();
    return h;
}

// Loads one library of the cache and adds its modules to the ones the game has. False when it is not usable.
bool load_library(const fs::path &path, std::size_t &modules) {
    void *library = lb::open_library(path);
    if (!library) return false;
    using StampFn = std::uint64_t (*)();
    using InitFn = const ModuleInfo *(*)(const EeLiveHost *, std::uint32_t *);
    const auto stamp = reinterpret_cast<StampFn>(lb::find_symbol(library, "ps2_ee_live_stamp"));
    const auto init = reinterpret_cast<InitFn>(lb::find_symbol(library, "ps2_ee_live_init"));
    if (!stamp || !init || stamp() != g->stamp) return false;  // stays loaded: it cannot be unloaded safely, and it is rare
    std::uint32_t n = 0;
    const ModuleInfo *table = init(&host(), &n);
    if (!table || !n) return false;
    ps2::add_modules(table, n);
    modules += n;
    ++g->libraries;
    return true;
}

bool is_cache_library(const fs::path &p, std::string &prefix) {
    const std::string name = ps2::os::utf8(p.filename());
    const std::size_t dash = name.find('-');
    if (dash != 8 || !name.ends_with(lb::kLibraryExtension)) return false;
    for (std::size_t i = 0; i < 8; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
    prefix = name.substr(0, 8);
    return true;
}

std::vector<std::uint8_t> read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Puts what was found where port.sh keeps it, when the game folder has them: the entry point in entries.txt and the captured window
// in overlays\, so that the next build of the game translates it ahead of time (scripts/windows.py capture does the same).
void remember(const std::string &name, std::uint32_t base, const std::vector<std::uint8_t> &image, const std::vector<std::uint32_t> &entries, bool program) {
    if (g->game_dir.empty()) return;
    std::error_code error;
    const fs::path entries_file = g->game_dir / "entries.txt";
    if (fs::is_regular_file(entries_file, error)) {
        std::set<std::uint32_t> known;
        const recomp::Entries have = recomp::read_entries(ps2::os::utf8(entries_file));
        if (program) known.insert(have.main.begin(), have.main.end());
        else if (const auto it = have.modules.find(name); it != have.modules.end()) known.insert(it->second.begin(), it->second.end());
        std::string text;
        for (const std::uint32_t entry : entries) {
            if (!known.insert(program ? entry : entry - base).second) continue;
            char token[96];
            if (program) std::snprintf(token, sizeof token, "%x\n", entry);   // an entry point of the program itself
            else std::snprintf(token, sizeof token, "%s:%x\n", name.c_str(), entry - base);
            text += token;
        }
        if (!text.empty()) {
            // The file ends with a newline in the way port.sh writes it; if some other tool left it without, start a new line.
            bool needs_newline = false;
            if (std::ifstream in(entries_file, std::ios::binary | std::ios::ate); in && in.tellg() > 0) {
                in.seekg(-1, std::ios::end);
                needs_newline = in.get() != '\n';
            }
            std::ofstream out(entries_file, std::ios::binary | std::ios::app);
            if (needs_newline) out << '\n';
            out << text;
        }
    }
    const fs::path overlays = g->game_dir / "overlays";
    if (!program && fs::is_directory(overlays, error)) {
        const fs::path file = overlays / (name + ".bin");
        if (!fs::exists(file, error)) {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
        }
    }
}

}  // namespace

void start(const std::string &program, const std::string &cache, const std::string &game_dir) {
    if (g || cache.empty()) return;
    g = new State;
    g->program = ps2::os::path(program);
    g->cache = ps2::os::path(cache);
    g->game_dir = ps2::os::path(game_dir);
    const std::vector<std::uint8_t> elf = read_file(g->program);
    g->windows = recomp::code_windows(elf);
    g->segments = recomp::code_segments(elf);
    if (elf.empty()) return;  // the program cannot be read (the runner will say so): nothing to translate for

    // PS2_EE_LIVE_OPT=-O0 .. -O3 chooses the optimisation level of the libraries; the game waits while one is made, so a quick compile
    // beats fast code (docs/ee-live.md has the measurements).
    std::string opt = "-O1";
    if (const std::string v = ps2::os::env("PS2_EE_LIVE_OPT"); !v.empty()) opt = v;
    g->flags = lb::compile_flags(opt);
    g->keep_work = !ps2::os::env("PS2_EE_LIVE_KEEP").empty();
    g->jobs = std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 2, 6);
    if (const std::string v = ps2::os::env("PS2_EE_LIVE_JOBS"); !v.empty()) g->jobs = std::clamp<std::size_t>(std::strtoul(v.c_str(), nullptr, 10), 1, 16);

    // The stamp says which libraries this runner may load: the translator, the headers, the flags and the program they were made for.
    std::uint64_t h = 0xCBF29CE484222325ull;
    h = lb::fnv(h, kGeneratorVersion, std::strlen(kGeneratorVersion));
    for (const char *text : {kLiveHdrCpu, kLiveHdrFpu, kLiveHdrVu, kLiveHdrMemory, kLiveHdrMmi, kLiveHdrGenerated, kLiveHdrEeLiveAbi})
        h = lb::fnv(h, text, std::strlen(text));
    for (const std::string &f : g->flags) h = lb::fnv(h, f.data(), f.size() + 1);
    h = lb::fnv(h, elf.data(), elf.size());
    const std::size_t layout = sizeof(Cpu);
    h = lb::fnv(h, &layout, sizeof layout);
    g->stamp = h;

    // What earlier runs left: load it now, and clear out what another version made.
    std::size_t modules = 0, files = 0;
    std::error_code error;
    if (fs::is_directory(g->cache, error)) {
        const std::string mine = lb::hex(g->stamp >> 32, 8);
        std::vector<fs::path> libraries;
        for (const auto &e : fs::directory_iterator(g->cache, error)) {
            std::string prefix;
            if (e.is_regular_file() && is_cache_library(e.path(), prefix)) {
                if (prefix == mine) libraries.push_back(e.path());
                else fs::remove(e.path(), error);
            } else if (e.is_directory() && ps2::os::utf8(e.path().filename()).rfind("work-", 0) == 0) {
                fs::remove_all(e.path(), error);  // a build the last run did not finish
            }
        }
        std::sort(libraries.begin(), libraries.end());
        for (const fs::path &p : libraries) {
            if (load_library(p, modules)) ++files;
            else fs::remove(p, error);
        }
    }
    if (files) say("loaded %zu translated code windows from %zu files in %s", modules, files, cache.c_str());

    g->compiler = lb::find_compiler();
    if (g->compiler.empty()) {
        say("no C++ compiler found (PS2_VU_CXX names one, or put one in a compiler folder next to the runner): code the build does not have stops the game");
        return;
    }
    lb::use_environment();
    g->ready = true;
    say("translating code the build does not have while the game runs, with %s, into %s", ps2::os::utf8(g->compiler).c_str(), cache.c_str());
}

bool active() { return g && g->ready; }

bool translate_missing(std::uint32_t pc) {
    if (!g || !g->ready || g->failed.count(pc)) return false;
    const auto began = Clock::now();
    std::error_code error;

    // Which window is it in: the same rule as scripts/windows.py capture (a window reaches as far as the game wrote to it).
    std::uint32_t base = 0, size = 0;
    bool found = false;
    for (const std::uint32_t b : g->windows) {
        const std::uint32_t extent = recomp::window_extent(g_ram, kRamSize, b);
        if (pc - b < extent && std::any_of(g_ram + b, g_ram + b + extent, [](std::uint8_t v) { return v != 0; })) {
            base = b;
            size = extent;
            found = true;
            break;
        }
    }
    // Not in a window: code of the program itself that its translation did not reach (a function of the program that only window code
    // calls, say; port.sh finds those by running the game and adds them to entries.txt). The program does not change, so two units
    // of memory around the address are the image.
    bool region = false;
    if (!found)
        for (const auto &[address, bytes] : g->segments)
            if (pc - address < bytes) {
                base = pc & ~(recomp::kWindowUnit - 1);
                size = std::min<std::uint32_t>(2 * recomp::kWindowUnit, kRamSize - base);
                found = region = true;
                break;
            }
    if (!found) {
        g->failed.insert(pc);
        say("no translated code at 0x%08X, and it is in none of the %zu code windows and not in the code of the program: the game stops", pc, g->windows.size());
        return false;
    }
    std::vector<std::uint8_t> image(g_ram + base, g_ram + base + size);
    const std::string name = (region ? "main_" : "") + recomp::window_name(base, image.data(), image.size());
    const bool covered = region || ps2::window_module_in_place(pc);   // some translation of this content is there, it lacks this entry point
    if (region) say("no translated code at 0x%08X (the program's own code, not reached by the build): translating it now, the game waits", pc);
    else say("no translated code at 0x%08X (window 0x%08X, image %s, %u KiB): translating it now, the game waits", pc, base, name.c_str(), size / 1024);

    recomp::LiveRequest request;
    request.base = base;
    request.image = image;
    request.name = name;
    request.stamp = g->stamp;
    request.entries.push_back(pc);
    std::size_t static_calls = 0, known_entries = 0;
    if (!covered) {
        // A content nothing in the build has seen: the calls the program itself makes into the window, and what entries.txt knows of this
        // image from before, are translated together with the entry point that was missed, so that this is not repeated for each of them.
        if (!g->analysed) {
            g->analysed = true;
            if (!g->game_dir.empty() && fs::is_regular_file(g->game_dir / "entries.txt", error))
                g->main_entries = recomp::read_entries(ps2::os::utf8(g->game_dir / "entries.txt")).main;
            try {
                g->program_analysis = std::make_unique<recomp::StaticProgram>(ps2::os::utf8(g->program), g->main_entries);
            } catch (const std::exception &e) {
                say("cannot read the program for its calls into the window: %s", e.what());
            }
        }
        if (g->program_analysis)
            for (const std::uint32_t t : g->program_analysis->calls_into(base, size)) {
                request.entries.push_back(t);
                ++static_calls;
            }
        if (!g->game_dir.empty() && fs::is_regular_file(g->game_dir / "entries.txt", error)) {
            const recomp::Entries have = recomp::read_entries(ps2::os::utf8(g->game_dir / "entries.txt"));
            if (const auto it = have.modules.find(name); it != have.modules.end())
                for (const std::uint32_t off : it->second) {
                    request.entries.push_back(base + off);
                    ++known_entries;
                }
        }
    }
    std::sort(request.entries.begin(), request.entries.end());
    request.entries.erase(std::unique(request.entries.begin(), request.entries.end()), request.entries.end());

    const std::string id = lb::hex(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()), 11);
    request.prefix = "m_" + name + "_" + id + "_";
    const fs::path work = g->cache / ("work-" + id);
    const fs::path src = work / "src";
    fs::create_directories(src, error);
    if (error || !(g->headers = g->headers || lb::write_headers(g->cache / "include", {{"cpu.hpp", kLiveHdrCpu}, {"fpu.hpp", kLiveHdrFpu}, {"vu.hpp", kLiveHdrVu}, {"memory.hpp", kLiveHdrMemory}, {"mmi.hpp", kLiveHdrMmi}, {"generated.hpp", kLiveHdrGenerated}, {"ee_live_abi.hpp", kLiveHdrEeLiveAbi}}))) {
        say("cannot write into %s: the game stops", ps2::os::utf8(g->cache).c_str());
        g->failed.insert(pc);
        return false;
    }

    const auto fail = [&](const char *why) {
        g->failed.insert(pc);
        if (++g->failures >= 2) {
            g->ready = false;
            say("translating failed twice: no more code is translated while playing (a log is in %s)", ps2::os::utf8(work).c_str());
        } else {
            say("%s", why);
        }
        return false;
    };

    lb::Timing timing;
    auto t = Clock::now();
    const recomp::LiveResult generated = recomp::generate_live(request, src);
    timing.generate = lb::seconds_since(t);
    if (!generated.ok) {
        say("the translator could not use 0x%08X: %s", pc, generated.error.c_str());
        return fail("the game stops");
    }

    lb::Job job;
    for (const auto &e : fs::directory_iterator(src, error))
        if (e.path().extension() == ".cpp") job.sources.push_back(e.path());
    std::sort(job.sources.begin(), job.sources.end());
    job.include_dir = g->cache / "include";
    job.work = work;
    job.library = work / ("library" + std::string(lb::kLibraryExtension));
    job.compiler = g->compiler;
    job.flags = g->flags;
    job.jobs = std::min(g->jobs, job.sources.size());
    job.below_normal = false;  // the game is paused: the player waits for this
    job.say = [](const std::string &text) { say("%s", text.c_str()); };
    if (!lb::compile_and_link(job, timing)) return fail("the game stops");

    const fs::path final_path = g->cache / (lb::hex(g->stamp >> 32, 8) + "-" + id + lb::kLibraryExtension);
    fs::rename(job.library, final_path, error);
    if (error) {
        say("cannot move the library into %s: %s", ps2::os::utf8(g->cache).c_str(), error.message().c_str());
        return fail("the game stops");
    }
    if (!g->keep_work) fs::remove_all(work, error);
    std::size_t modules = 0;
    if (!load_library(final_path, modules)) {
        say("the library made just now does not load (%s)", ps2::os::utf8(final_path.filename()).c_str());
        return fail("the game stops");
    }
    g->failures = 0;
    say("switched in after %.1f s (translate %.1f, compile %.1f, link %.1f): %zu entry points%s%s, %zu instructions, %zu files", lb::seconds_since(began), timing.generate, timing.compile, timing.link,
        generated.accepted.size(), static_calls ? " (calls of the program included)" : "", known_entries ? " (entries.txt included)" : "", generated.code, job.sources.size());
    remember(name, base, image, {pc}, region);
    return true;
}

}  // namespace ps2::ee::live

#endif
