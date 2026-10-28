#pragma once

// Main processor code the build does not know, translated while the game runs (docs/ee-live.md).
//
// A game build translates the code of the program and of the code windows it has seen (eerecomp). Code a game copies into a window
// later, or a function of a window image that nobody called while the build explored, has no translation, and the runner used to stop
// with "missing code". Now the game waits: the runner takes the window's image from memory (the way scripts/windows.py capture does),
// translates it with the same translator as the build (recomp/eerecomp_gen.cpp), compiles the C++ into a library with a C++ compiler,
// loads it, adds it to the modules it has (add_modules) and goes on. The libraries stay in a cache folder next to the game.

#include <cstdint>
#include <string>

namespace ps2::ee::live {

// Called once, after the generated code is registered and before the game starts. program is the game's executable (its code windows
// are read from it), cache is the folder for the libraries (made when the first one is built), game_dir the folder port.sh made
// (entries.txt and overlays\ in it get what was found), or empty. Loads what earlier runs left in the cache. With no C++ compiler
// found it logs one line and only the loaded libraries are used.
void start(const std::string &program, const std::string &cache, const std::string &game_dir);

bool active();

// The main processor reached pc where nothing is translated. True when code for pc was translated, compiled and loaded (the caller
// looks again); false when that is not possible (nothing is changed; the reason is in the log).
bool translate_missing(std::uint32_t pc);

}  // namespace ps2::ee::live
