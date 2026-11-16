#pragma once

// The little of libnx that the deko3d shim touches, for the PC harness (scripts/deko_harness.py): every other libnx header is
// ARM-specific, and the shim only ever asks for the console's default window.

#include <cstdint>

typedef std::uint32_t u32;

typedef struct NWindow NWindow;

extern "C" NWindow *nwindowGetDefault();
