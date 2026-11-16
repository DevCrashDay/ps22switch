# Project overview

**Name:** ps22switch (GitHub `DevCrashDay/ps22switch`; local `F:\ps22switch`).
**Owner / tester:** Ash (GitHub `DevCrashDay`, git identity `xcrashdayx`). Tests on a real Nintendo Switch 1.
**Based on:** exoteric33/ps2-native-recomp (MIT), itself based on Samir Nuri's ps2-web-recomp.

## Purpose
Turn a PS2 game disc the user owns into a native Nintendo Switch 1 homebrew program (`.nro`) by **static recompilation**: the game's
MIPS R5900 code is translated to C++ on a PC and cross-compiled for the Switch's ARM Cortex-A57; the rest of the PS2 is emulated by a
C++ runtime. The first and only target so far is **Tekken 4 (USA, SLUS_203.28)**, chosen because its Windows recompilation already
plays at 60 fps.

## Goal stated by the user
"implement a ps2 to switch 1 version" → "there is no point ... if we dont reach full gameplay speed" → project goal set in the
project chat: "implement a vulkan renderer to achieve maximum performance on a switch 1". Success = Tekken 4 at full speed (60 fps)
including fights, on real hardware.

## Where it stands (2026-11-04)
- Tekken 4 boots, plays menus, story screens and fights on a real Switch 1 with correct sound and 3D.
- Speed with the overclock (CPU 1785 / GPU 921.6 / memory 1600 MHz) and the hardware-rasterizer renderer (`PS2_GS_HW=1`):
  **menus 60 fps, story/FMV 45–55 fps, fights 22–27 fps** (last measured 2026-10-30). Fights are limited by the Switch GPU and by the
  thread that feeds it.
- Seven later GPU optimisations and eight ready-made test builds have **not** been measured on the console yet.
- No development since 2026-10-30 15:00.

## Main building blocks
1. PC pipeline (upstream): `scripts/port.sh` → `eerecomp`/`vurecomp` → `generated/<game>/*.cpp`.
2. Switch build: `scripts/switch_port.py` → WSL devkitA64 → `.nro` + SD-card folder with `launch.txt`.
3. Runtime: `runtime/` (EE/IOP/VU/GS/SPU2/IPU emulation), `host/` (SDL2 window, audio, pads, Switch launch code).
4. Graphics: upstream WebGPU GS renderer (`gpu.cpp`, `gs.wgsl`) running on Vulkan through a new shim (`runtime/src/gs/gpu/vk`),
   with Mesa NVK linked statically; plus a new PCSX2-style hardware-rasterizer path (`hw.wgsl`).

## Non-goals / constraints
- No game data, generated code, binaries or console keys in the repository.
- No JIT on the Switch: code not found on the PC cannot run there.
- Homebrew only (custom firmware); the forwarder NSP needs signature patches.

See [HANDOFF.md](HANDOFF.md) for the full index.
