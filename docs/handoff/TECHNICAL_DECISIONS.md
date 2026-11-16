# Technical decisions and failed approaches

## Part 1 — Decision log

### D01 — Build on exoteric33/ps2-native-recomp (static recompilation) for the Switch
- **Reasoning:** Tekken 4 already plays at 60 fps on Windows with it; static recompilation avoids a JIT (which Horizon homebrew cannot
  easily do).
- **Alternatives:** an emulator port (NetherSX2/PCSX2-based, ARMSX2-NX). Not chosen: the project's purpose is a native recomp.
- **Consequence:** all code must be found on the PC; no run-time compiler on the Switch (D05). **Still valid.**

### D02 — Tekken 4 is the first and only target game
- User, 2026-10-29 22:35Z: "our first test subject is to get tekken 4 running on the switch not tekken 5. bc tekken 4s recompt to
  windows was succesfull". **Valid.** Tekken 5 files exist but are out of scope.

### D03 — Build in WSL (`devkita64`) with devkitA64, driven from Windows Python
- **Reasoning:** devkitPro was already set up in WSL; Windows tools produce the generated code.
- **Consequence:** build dir in WSL `/tmp` (speed); MSYS path mangling pitfalls; WSL1 file I/O on `/mnt/f` is slow. **Valid.**

### D04 — 16 MiB thread stacks, explicit core placement, kernel futex
- libnx default stack 128 KiB is too small for translated code; Horizon never migrates threads; 1 ms sleeps lost hand-overs.
  **Valid; do not revert.**

### D05 — No live EE/VU translation on the Switch; exploring runs on the PC instead
- **Reasoning:** no compiler on the console; same as the browser build. `switch_port.py` runs `port.sh --native-only` (exploring runs +
  VU1 compile) so the Switch build knows as much code as possible.
- **Consequence:** BUG-009. **Valid, but reconsider** an EE interpreter fallback if new game areas stop.

### D06 — `-ffp-contract=off` on the Switch
- GCC fuses `a*b+c` into FMA on ARM; one rounding instead of the PS2's two changes FPU/VU results. **Mandatory; do not remove.**
- Note: the GPU side is separate — SPIR-V variants exist with and without explicit `fma()` (`PS2_VK_FMA`, default 1).

### D07 — `launch.txt` next to the .nro (argv lines + `NAME=value` environment)
- Lets the Switch take the same switches as the PC without rebuilding; set before static init. **Valid.**
- Pitfall: `switch_port.py` regenerates it (BUG-002).

### D08 — GPU renderer instead of tuning the software renderer
- Software renderer = ~87 % of time on PC; 1 fps on Switch. Tuning estimate 1.5–3x. **User decision:** "there is no point in 1 if we
  dont reach full gameplay speed, so do 2 instead." **Valid.**

### D09 — Vulkan through Mesa NVK (OpenTrickyNX's SDK), not OpenGL, not Deko3D (for now)
- **Reasoning:** the user's/coordinator's goal ("implement a vulkan renderer", "like ... OpenTrickyNX"); NVK is already proven in the
  user's SSX Tricky port; NetherSX2 runs PCSX2's Vulkan HW renderer on NVK.
- **Alternatives:** OpenGL via devkitPro Mesa (first suggested by the agent; rejected by the goal); Deko3D (fastest, bypasses Mesa, but
  a separate backend; deferred).
- **Consequences:** NVK quirks (software robust access, strong atomics, no Rust debug printing); static linking recipe; SDL2's EGL/glapi
  must come from the NVK SDK. **Valid;** Deko3D stays an option if NVK overhead becomes the limit.

### D10 — Implement the WebGPU C API on Vulkan (a shim) instead of porting `gpu.cpp` to Vulkan
- **Reasoning:** `gpu.cpp` uses ~60 WebGPU functions, mostly compute; a shim keeps the renderer identical to upstream and checkable
  pixel-for-pixel on the PC (wgpu-native vs shim).
- **Alternatives:** rewrite the renderer directly on Vulkan; run wgpu-native on the Switch (not available for Horizon).
- **Consequences:** WebGPU's conservative ordering (barrier after each dispatch) costs on Tegra (BUG-022); no surfaces → picture readback
  (BUG-007). **Valid for correctness; reconsider** for performance (relax barriers in the shim where WebGPU semantics allow, add surface).

### D11 — Shaders converted offline with naga 29; SPIR-V table checked by sha256
- naga 29 matches wgpu-native 29's WGSL; the table is committed (`spirv_table.cpp`, 1.6 MB) so builds do not need Rust. CMake stops on
  mismatch. **Valid.**

### D12 — Robust buffer access off; indices clamped in the shader (`Restrict`)
- NVK implements robust access with a branch per access (~480 in raster). Clamping is branch-free and WebGPU-legal. Menus 19–26 → 24–34
  fps. **Valid; do not turn robust access back on except for debugging (`PS2_VK_ROBUST=1`).**

### D13 — In-flight cap of 8 submissions
- Unbounded queueing made NVK lose the device (230+ submissions). `PS2_VK_IN_FLIGHT` tunes it (test build D uses 3). **Valid.**

### D14 — Built-in overclock 1785 / 921.6 / 1600 MHz
- User request ("like you did for opentricky"). CPU side became sufficient for menus. sys-clk wins unless `PS2_CLOCK_FORCE=1`.
  **Valid.** Battery/thermal impact not assessed.

### D15 — Hardware-rasterizer GS path (PCSX2-style) behind `PS2_GS_HW=1`; compute renderer stays the reference
- **Reasoning:** compute raster near its limit (104 registers/thread on Tegra, every pixel walks every primitive); options card answered
  "go ahead" by the user.
- **Alternatives:** squeeze the compute renderer (1.5–2x estimated); CPU-side wins first.
- **Design choice:** VRAM stays the truth (bit-exact uploads, CLUTs, copies, display); targets synced lazily per page. Developed in
  WGSL/WebGPU first so it could be verified on PC with wgpu-native, then the shim gained graphics support.
- **Consequences:** BUG-003/004/005/006. **Valid;** making it the Switch default is design milestone 4 (pending).

### D16 — NetherSX2 memory settings (GPU heap reserve 512/128 MB, transfer memory 16 MB)
- Written fresh (license). **Valid.**

### D17 — CLUT loads: CPU-built owner table instead of per-entry history scan
- After the O(n²) regression. **Valid.**

### D18 — Lazy target images (keep colour/depth between batches)
- Fills/write-backs 1400 → 600 per second on PC. Accepts small depth differences (BUG-005). **Valid; reconsider** depth rounding.

### D19 — Read VRAM through a second, read-only binding in render passes
- NVK atomic loads are "strong" and skip L1. **Valid.**

### D20 — Upload/CLUT reordering and render-pass merging (switchable)
- `PS2_PRE_REORDER=0`, `PS2_HW_MERGE=0` exist so the Switch tests can measure them. PC: identical pictures. Switch benefit: unmeasured.

### D21 — Forwarder NSP from OpenTrickyNX
- User request. GPL-3.0 tooling kept in `tools/forwarder` (git-ignored, outside the MIT repo). **Valid.**

### D22 — Repository hygiene: no game data, generated code, binaries, keys in git
- `.gitignore` excludes `games/`, `generated/`, `tools/`, `build/`, `scratch/`, `*.iso`, `*.elf`, `*.ps2`, images, etc. **Mandatory.**

### D23 — Work on branch `claude/project-thread-i8q90o`, no PR opened
- No PR was requested by the user. `main` = `f89a7c6` (first Switch commit). **Pending a user decision** to merge.

## Part 2 — Approaches that were tried and failed (do NOT repeat)

| # | Approach | Where/when | Result | Why it failed | Lesson |
|---|---|---|---|---|---|
| X1 | Software GS renderer on the Switch | `f89a7c6`/`601f70c`, 2026-10-29 | ~1 fps | CPU rasterisation far too slow on Cortex-A57 | Need the GPU. |
| X2 | One 16 MiB staging block per submission | `584dd8e` | crash at start (OOM) | blocks piled up with several submissions in flight | Share one ring (`25fb2ba`). |
| X3 | Unbounded GPU queueing | `584dd8e`/`25fb2ba` | frozen first frame, `device lost` | 230+ submissions behind | Cap in flight (`f42606b`). |
| X4 | Choosing a non-coherent memory type to speed up the raster shader | `518f6b6` | no change | Switch has two memory types, both the same RAM | Memory type is not a lever on Tegra. |
| X5 | Compute raster tile-size variants (TILE 4/8/16) | `518f6b6` | not a breakthrough (kept, selectable via `PS2_TILE`) | the design itself is register-bound | — |
| X6 | `NAK_DEBUG=print` + `MESA_SHADER_CACHE_DISABLE=true` | 2026-10-30 09:03Z | immediate crash | Mesa Rust stdout on Horizon | Use `PS2_VK_STATS=1`. |
| X7 | Redirect stdout / fds 1–2 to a file to make X6 work | `1549962` | still crashed | Rust std I/O not Horizon-compatible | Abandon NAK printing. (The redirect itself is kept and harmless.) |
| X8 | First `PS2_PARTS` sampler thread (default stack) | `9709c64` | crash at start | thread created without a known stack size on Horizon | Always create Switch threads with explicit stacks (`6dcfc5d`). |
| X9 | Robust buffer access in the driver | until `526bf32` | ~480 branches in raster, slow | NVK software implementation | Clamp in the shader. |
| X10 | Squeezing the compute raster further | considered 09:24Z | not pursued | estimated 1.5–2x, cannot reach 60 | HW rasterizer chosen. |
| X11 | Parallel CLUT loads with per-entry walk over earlier loads | `ce560bf` | "now slower" (~0.58 s GPU/s, 18 ms per batch) | O(loads²) with hundreds of loads per batch | Precompute owner table on the CPU (`d9b9eef`). |
| X12 | Texture pre-decode on the HW path (`decode` passes, as the compute path does) | until `ce560bf` | ~170 ms GPU/s in fights | redundant for HW path | Removed; re-tried as optional `PS2_HW_DECODE` (X13). |
| X13 | `PS2_HW_DECODE=1` cached pre-decode per submit | `8e693c9` | PC: identical but slightly slower | ~4 700 decode dispatches/s | Unproven on Switch; a real cross-frame texture cache (keyed, invalidated by VRAM writes) is the better version. |
| X14 | ARMSX2-NX as a source of ideas | 09:52Z | nothing usable | project says it is not playable | Don't revisit. |
| X15 | OpenGL renderer via devkitPro Mesa | proposed 22:57Z | not attempted | goal set to Vulkan/NVK | — |
| X16 | Using devkitPro's Mesa EGL/glapi with NVK | link stage | clashes | two Mesas | Swap to the SDK's EGL/glapi and drop `drm_nouveau` (CMakeLists). |
| X17 | Bash heredocs to write Python edit scripts on this PC | throughout | corrupted `\n`/`\r` in sources (system.cpp, main.cpp, CMakeLists, gpu.cpp — fixed) | Git Bash heredoc escaping | Write scripts with an editor; check for stray CR/LF after scripted edits. |
| X18 | Calling `wsl ... /opt/...` from Git Bash | throughout | `/opt/...` arguments mangled (recorded in the project memory; exact rewritten form not preserved) | MSYS path conversion | Use PowerShell or `MSYS_NO_PATHCONV=1`. |
| X19 | First `switch_port.py` (`f89a7c6`) without exploring runs / with hard-coded Tekken 4 paths | `f89a7c6` | replaced | Switch cannot compile missed code | Always run `port.sh --native-only` first (or reuse its output with `--skip-port`). |
| X20 | An extra `appletMainLoop()` check in the window loop | `f89a7c6` | could hang on exit | ate SDL's quit | Removed in `601f70c`. |
