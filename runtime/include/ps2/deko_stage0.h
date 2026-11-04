#pragma once

#include <cstdint>

namespace ps2 {

// Stage 0 of the deko3d renderer (docs/deko3d.md): open a deko3d device, make a swap chain on the console's own default window,
// clear every back buffer to a colour and present it, and nothing else. No SDL, no Vulkan, no game: a failure here is deko3d's or the
// window's, not the runtime's. `PS2_DEKO3D=<frames>` in launch.txt runs it instead of the game (host/main.cpp), and the `[deko]` lines
// it writes to ps2run.log say how each step went.
//
// `frames` 0 means "whatever PS2_DEKO3D says, else 300" (five seconds at 60 Hz). Returns 0 when the frames were presented.
int deko_stage0(std::uint32_t frames = 0);

}
