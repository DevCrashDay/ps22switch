# Development roadmap

Goal (user): Tekken 4 at full gameplay speed (60 fps in fights) on a real Nintendo Switch 1. Current: fights 22–27 fps.
Expected gains are estimates from the 2026-10-30 profile, not measurements.

## THE SINGLE NEXT TASK

**R1 — Measure what already exists (test builds A–G) and fix the package `launch.txt`.**
Seven commits of GPU work have never run on the console. Every later decision depends on those numbers, and the main package currently
runs the slow renderer (BUG-002). This costs the user ~10 minutes and the developer ~30.

---

## 1. Immediate next steps

### R1 — Measure test builds A–G on the Switch (Priority P0)
- **Why:** BUG-010; decides between texture cache / barrier work / present path.
- **Files:** `build/switch/tests/*/` (+ `README.txt`), `references/important_logs/` for the baseline.
- **Approach:** ask the user to copy each `Tekken-4.nro` + `launch.txt` pair into `sdmc:/switch/ps22switch/Tekken-4/`, play ~1 minute into
  a fight, save `ps2run.log` as `ps2run-A.log` etc. Order: A, B, then G, C, D, E, F. Before handing them out, delete `prod.keys` from the
  package folder (BUG-011).
- **Acceptance:** a table of fight fps / GPU busy / hw_draw / upload / waits per build, compared with the 22–27 fps baseline.
- **Verify:** `[waits]` lines (fps), `[vk-profile]` (B, G-profile), `[parts]`.
- **Risk:** builds were made at 11:28/14:56 from different commits (A–F from `aaed033`-era, G from `8e693c9`); note this when comparing.

### R2 — Make the HW path the Switch default (P0)
- **Why:** BUG-002.
- **Files:** `scripts/switch_port.py` `package()` (add `PS2_GS_HW=1`, maybe `--no-record`), or `runtime/src/gs/gpu/gpu.cpp` `init()`
  (`#ifdef __SWITCH__` default on, `PS2_GS_HW=0` to disable).
- **Acceptance:** a fresh `switch_port.py` package logs `[gpu] hardware rasterizer path on`.
- **Verify:** rebuild, grep `launch.txt`, run on console.

## 2. Critical fixes

### R3 — Remove `prod.keys` from the package folder (P0, user action) — BUG-011.

### R4 — Lines and points on the HW path (P1) — BUG-003
- **Files:** `runtime/src/gs/gpu/hw.wgsl` (`hw_vs` kind ≥ 2), `gpu.cpp` (`hw_emit` prim kinds).
- **Approach:** expand each line to a 1-pixel-wide quad following GS line rules, points to 1x1 quads; or route those batches to the
  compute path.
- **Acceptance/verify:** `cmp.py` against `scratch/wgpu` frames; find a Tekken 4 scene that uses lines (`PS2_TRACE_DRAWS`) to check.

### R5 — Destination alpha test (P1) — BUG-004
- **Approach:** stencil from the target's alpha bit before the draw (PCSX2's DATE approach) or read the colour target in the shader
  (input attachment; needs shim support).
- **Acceptance:** pixel parity with the compute path on frames that use DATE.

### R6 — Depth precision (P2) — BUG-005
- **Approach:** quantise depth to the PS2 Z format (16/24/32-bit) in `hw_fs` when the draw writes depth.
- **Acceptance:** `cmp.py` hwvk vs wgpu: back to 42/42 or documented residual.

## 3. Core feature implementation

### R7 — Direct Vulkan present (P1) — BUG-007
- **Why:** window thread busy 999 ms/s; readback + SDL GL upload each frame.
- **Files:** `wgpu_vk.cpp` (surface/swapchain: `VK_NN_vi_surface` + nwindow from libnx), `gpu.cpp` `present()` path
  (exists for wgpu surfaces), `host/window.cpp` `attach_gpu()`.
- **Risks:** SDL also owns the window/EGL; must not create two presenters on one layer. NetherSX2 does this on NVK.
- **Acceptance:** `[window] presenting through WebGPU` on the Switch; `[waits]` "showing the picture" drops near 0.

### R8 — Make the HW path complete enough to be the reference on Switch (design milestone 4) (P2)
- Pipeline cache on the SD card (`VkPipelineCache`), frame pacing, remove `PS2_GS_HW` requirement.

## 4. Performance and optimisation (order of expected gain per day, from `docs/switch-performance-plan.md`)

### R9 — Texture cache (P1, large)
- **Why:** `hw_fs` decodes every texel from swizzled VRAM (table + word + CLUT, ×4 bilinear).
- **Approach:** decode a texture once into an RGBA8 `VkImage` keyed by (TBP, TBW, PSM, TW/TH, CLUT hash/CBP); invalidate by page when an
  upload, write-back or local copy touches its memory (the page tracking of `g_rendered`/`hw_touch` exists); sample with hardware
  filtering where PS2 rounding allows. Unlike `PS2_HW_DECODE` (per-submit), it must persist across frames.
- **Risk:** CLUT changes (palette animation) and render-to-texture invalidation; correctness vs compute path.
- **Acceptance:** fight `hw_draw` ms/s clearly below the A/B baseline with `cmp.py` parity.

### R10 — Fewer passes and barriers (P1, medium)
- **Approach:** in the shim, skip barriers between dispatches with no data dependency (uploads to disjoint ranges); merge consecutive
  batches on the same target into one render pass (partly done, `a1c327d`).
- **Acceptance:** GPU busy minus covered timestamps (~40 %) shrinks.

### R11 — Upload/CLUT coalescing (P2, medium)
- ~90 upload dispatches per frame from palette upload → CLUT load → next palette. Take the CLUT straight from the uploaded bytes when the
  CLUT load reads exactly what was just uploaded.

### R12 — CPU side once the GPU stops being the limit (P3)
- VU0 macro-mode multiply-add with flags as NEON (~14 % of the emulation thread); more aggressive timer-polling skip (`hw::read32` 13–20 %);
  IPU (MPEG) with NEON (BUG-008).

### R13 — Deko3D backend (P4, large)
- Only if NVK overhead is the limit after R7–R11. Separate backend to maintain.

## 5. Additional functionality

### R14 — Forwarder test on console (P3) — BUG-020.
### R15 — Memory card save/load check on Switch (P3) — F18.
### R16 — Controller mapping/handheld detection (P3) — BUG-014.
### R17 — Log cleanup on Switch (P4) — BUG-013, BUG-015.
### R18 — Exploring-run coverage for the rest of Tekken 4 (P2) — BUG-009: run `port.sh` with more `--rounds/--frames`, play other modes
  on PC, regenerate, rebuild.

## 6. Long-term ideas

- Upscaling on the HW path (design milestone 5; BUG-025).
- Other games (Tekken 5 after upstream fixes its sound-module stop; BUG-021).
- An EE interpreter fallback on the Switch for code missed by exploration.
- Bring the PC toolchain and `bench.sh` into the repo (BUG-024); CI for the Switch build.
- Merge upstream changes from `C:\ps2-native-recomp-main` (dated 2026-11-02) after checking what changed.
- Merge `claude/project-thread-i8q90o` into `main` once the user agrees (D23).
