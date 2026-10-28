#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

// Paths and environment values are UTF-8 inside the runner. Windows reads a char* path in the ANSI code page, so a name with
// characters outside it (a Chinese folder name, say) breaks there; every file the runner opens for the user goes through these.
namespace ps2::os {

inline std::filesystem::path path(const std::string &text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception &) {
        return std::filesystem::path(text);  // not valid UTF-8: take it as the system's own encoding
    }
}

inline std::string utf8(const std::filesystem::path &p) {
    const std::u8string text = p.u8string();
    return std::string(text.begin(), text.end());
}

inline std::FILE *open(const std::string &text, const char *mode) {
#ifdef _WIN32
    return _wfopen(path(text).c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(text.c_str(), mode);
#endif
}

inline std::string env(const char *name) {
#ifdef _WIN32
    const wchar_t *value = _wgetenv(std::wstring(name, name + std::strlen(name)).c_str());
    return value ? utf8(value) : std::string();
#else
    const char *value = std::getenv(name);
    return value ? value : std::string();
#endif
}

// Sets an environment value for this process and the code it runs later; the value is UTF-8. env() reads back exactly what
// was set, also for names with characters outside the ANSI code page. Returns false when the system refused.
inline bool set_env(const char *name, const std::string &value) {
#ifdef _WIN32
    return _wputenv_s(std::wstring(name, name + std::strlen(name)).c_str(), path(value).c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

}
