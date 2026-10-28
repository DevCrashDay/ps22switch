#include "vurecomp_gen.hpp"

#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char **argv) {
#ifdef _WIN32
    // argv is in the ANSI code page and loses every character outside it: take the UTF-16 command line and use it as UTF-8
    std::vector<std::string> utf8_args;
    std::vector<char *> utf8_argv;
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) utf8_args.push_back(ps2::os::utf8(wide[i]));
        LocalFree(wide);
        for (std::string &a : utf8_args) utf8_argv.push_back(a.data());
        utf8_argv.push_back(nullptr);
        argc = wide_argc;
        argv = utf8_argv.data();
    }
#endif
    if (argc < 3) {
        std::fprintf(stderr, "usage: vurecomp <recorded programs dir> <out dir>\n");
        return 1;
    }
    const fs::path in_dir = ps2::os::path(argv[1]), out_dir = ps2::os::path(argv[2]);
    std::vector<ps2::vurecomp::Input> inputs;
    for (const auto &e : fs::directory_iterator(in_dir)) {
        if (e.path().extension() != ".bin") continue;
        unsigned long long key = 0;
        unsigned entry = 0;
        if (std::sscanf(ps2::os::utf8(e.path().stem()).c_str(), "%llx_%x", &key, &entry) != 2) continue;
        std::ifstream f(e.path(), std::ios::binary);
        ps2::vurecomp::Input in{key, entry, std::vector<std::uint8_t>(16384)};
        f.read(reinterpret_cast<char *>(in.code.data()), static_cast<std::streamsize>(in.code.size()));
        inputs.push_back(std::move(in));
    }
    const ps2::vurecomp::Result result = ps2::vurecomp::generate_files(std::move(inputs), out_dir, 0);
    std::printf("%zu VU1 programs, %zu of them jump targets%s\n", result.programs, result.jump_targets, result.skipped ? ", the rest left to the interpreter" : "");
    return 0;
}
