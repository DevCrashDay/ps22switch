# Bugs, limitations and unfinished work

Severity: **Critical** (project cannot function), **High** (major feature unavailable), **Medium** (degraded), **Low** (minor or
cosmetic), **Unknown** (insufficient information). "Reported" numbers come from the user's Switch logs as quoted in the project thread;
only the last log is on disk (`references/important_logs/switch_ps2run_2026-10-30_hw-path_fight.log`).

| ID | Title | Severity | Status |
|---|---|---|---|
| BUG-001 | Fights run at ~22–27 fps, not 60 (GPU + window thread bound) | High | open, main goal |
| BUG-002 | Package `launch.txt` lacks `PS2_GS_HW=1` → slow compute renderer | High | open (easy fix) |
| BUG-003 | Hardware path does not draw lines or points | Medium | open |
| BUG-004 | Hardware path has no destination alpha test (DATE) | Medium | open |
| BUG-005 | Depth stays float between batches (lazy images) → small pixel differences | Medium | accepted, open |
| BUG-006 | HW path differs from compute reference in fights (floor shading, effects) | Medium | open |
| BUG-007 | Picture read back and re-uploaded through SDL every frame | Medium | open |
| BUG-008 | FMV (IPU) decoding on the CPU caps story/FMV at ~40–55 fps | Medium | open |
| BUG-009 | No run-time translation on the Switch (EE and VU live builds stubbed) | High (latent) | by design, open |
| BUG-010 | Commits after `28bab2c` and test builds A–G never measured on the Switch | Unknown | open |
| BUG-011 | `prod.keys` sits inside the SD-card package folder | Medium (security) | open |
| BUG-012 | `NAK_DEBUG=print` crashes the program on the Switch | Low (workaround exists) | won't fix |
| BUG-013 | Misleading log lines on the Switch (D3D12/FXC, "unknown processor: 0 logical processors") | Low | open |
| BUG-014 | Two "Switch Controller (generic)" pads; repeated "is not used" lines | Unknown | open |
| BUG-015 | Pad recording always on; console clock date in file names | Low | open |
| BUG-016 | `Switch_Build.cmd` prefers the broken `py` launcher on this PC | Low | open |
| BUG-017 | IPU "bad code in command 0x10010000" messages | Low | open (upstream) |
| BUG-018 | Stale/broken build folders and caches (`build-switch/`, missing `third_party/`) | Low | open |
| BUG-019 | Documentation drift: missing docs referenced by code; stale comments; README overstatements | Low | open |
| BUG-020 | Forwarder NSP never tested on a console | Unknown | open |
| BUG-021 | Tekken 5 does not work (upstream) and is untested on the Switch | Medium (not a target) | open |
| BUG-022 | Too many GPU dispatches/barriers per frame (uploads ~2 500/s; ~40 % of GPU time between timestamps) | Medium | open (part of BUG-001) |
| BUG-023 | `PS2_HW_DECODE` is slower on PC; unknown on Switch | Low | experimental |
| BUG-024 | Reproducibility depends on files outside the repo (C: toolchain, disc image, generated code, NVK SDK) | Medium | open |
| BUG-025 | HW path is 1x only (2048x2048 targets), no upscaling | Low | limitation |
| BUG-026 | Audio behaviour below full speed in fights not assessed | Unknown | open |
| BUG-027 | Software-renderer build path not re-tested since the Vulkan work | Low | open |

Total: **27 issues**.

---

### BUG-001 — Fights at ~22–27 fps (High)
- **Expected:** 60 fps in Tekken 4 fights on Switch 1 (user's goal: "full gameplay speed").
- **Actual:** menus 60, story/FMV 45–55, fights 22–27 fps (overclocked 1785/921.6/1600, `PS2_GS_HW=1`).
- **Reproduce:** package + `PS2_GS_HW=1`, `PS2_PRESENT_LOG=1`, `PS2_VK_PROFILE=1`, `PS2_PARTS=1`; start a Story Battle fight; read the
  `[waits]`/`[vk-profile]` lines.
- **Evidence (fight, per second):** `GPU busy ~600 ms`, `hw_draw ~340 ms (~500 passes, ~0.7 ms/pass)`, `upload ~95–105 ms (~2 500
  dispatches)`, `load_clut ~48 ms`, `hw_load_color ~43 ms`, `hw_writeback ~52 ms`, `most in flight 9`, `CPU waited ~350 ms`;
  `[waits] window thread rendering 999 ms, no work 0 ms`; `[parts] game code ~45 %, waiting for the window ~45 %`.
- **Root cause (confirmed in part):** GPU time and the window thread (which records all GPU work and presents) are saturated. Within
  GPU time: per-pixel texture decode from swizzled VRAM in `hw_fs` (swizzle table + word + CLUT, ×4 for bilinear), many small
  dispatches and full barriers, readback/upload of the picture.
- **Fix history:** compute renderer 7 fps → HW path 12–13 → no decode on HW path + CLUT owner table 22–24 → flat setup + lazy images
  22–27. Later commits (read-only VRAM, no CPU binning, reorder, merged passes, FIX split, HW_DECODE) unmeasured (BUG-010).
- **Next steps:** measure test builds A/B first; then texture cache (decode once into RGBA images, sample with HW filtering where
  rounding allows), fewer passes/barriers, upload/CLUT coalescing, direct Vulkan present (`VK_NN_vi_surface`); see
  DEVELOPMENT_ROADMAP.md.

### BUG-002 — Package `launch.txt` lacks `PS2_GS_HW=1` (High)
- **Expected:** the default package runs the fastest renderer.
- **Actual:** `build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/launch.txt` (written 2026-10-30 14:56 by `switch_port.py`)
  contains only the argv and clock lines. `gpu.cpp:2215` enables the HW path only when `PS2_GS_HW` starts with `1` → the package uses
  the compute path (~7 fps in fights). During the project the agent appended the lines by hand after each build
  (`printf 'PS2_GS_HW=1\n...' >> launch.txt`); the 14:56 rebuild for test build G overwrote that.
- **Files:** `scripts/switch_port.py` `package()` (line ~220), `runtime/src/gs/gpu/gpu.cpp` `init()`.
- **Fix:** add `PS2_GS_HW=1` (and optionally `PS2_PRESENT_LOG=1`) to the lines `package()` writes, or make the HW path the Switch default
  in `gpu.cpp` (design doc milestone 4). Until then: add the line by hand. The test builds in `build/switch/tests/*` already contain it.

### BUG-003 — Lines and points not drawn on the HW path (Medium)
- `hw.wgsl` `hw_vs`: `// lines and points: to come` → vertices at `-1e9` (no area). Header comment lists them as "to come".
- Effect in Tekken 4: UNKNOWN (no visible defect reported; the user did not compare).
- **Next:** expand lines to quads (PS2 line rasterisation rules) and points to 1-pixel quads in `hw_vs`; or route batches with lines
  or points to the compute path.

### BUG-004 — No destination alpha test (DATE) on the HW path (Medium)
- Design doc: "the destination-alpha test by reading the target as an input attachment" — not implemented (`6210d1a` message:
  "Lines, points and the destination alpha test are still missing"). Batches using DATE are drawn without it.
- **Next:** implement via input attachment / framebuffer fetch, or stencil pre-pass (PCSX2 style), or fall back to compute for such batches.

### BUG-005 — Depth precision with lazy images (Medium, accepted)
- Since `28bab2c` depth stays `D32_SFLOAT` (`z / 2^32`) between batches instead of rounding through 32-bit integer VRAM. PC: 17/42 frames
  identical, others 6–80 differing pixels (max channel diff 11–59; `frame_03900`…`frame_04100`). Possible flicker in decals/shadows.
  User was told; no report back.
- **Next:** round depth to the PS2 format in the shader on write (cost), or round only on write-back (current).

### BUG-006 — HW vs compute renderer differences in fights (Medium)
- PC through wgpu-native (`6210d1a` message): "fights differ in shading details (floor, effects)". Interpolation/blend rounding.
  Quantified per frame: only partly (see BUG-005 numbers after later commits).
- **Next:** `scratch/cmp.py` between `scratch/wgpu` (compute) and `scratch/hwvk*`; inspect differing frames (`scratch/diff.png`,
  `scratch/contact.png` exist from 2026-10-30 10:46).

### BUG-007 — Picture readback + SDL upload (Medium)
- `gpu.cpp` `display_async` reads the frame back; `window.cpp` `show_pixels` uploads to an SDL texture (GL via EGL) and presents, on
  the same window thread that records GPU work. The shim has no surface support.
- **Next:** present from Vulkan directly (NVK's `VK_NN_vi_surface`, as NetherSX2 does); requires surface/swapchain support in the shim.

### BUG-008 — FMV decoding cost (Medium)
- `[parts]`: IPU 17–29 % of the emulation thread in FMVs (reported up to ~50 % at stock clocks); story/FMV 40–55 fps overclocked.
- **Next:** NEON for IPU IDCT/colour conversion (`runtime/src/ipu/ipu.cpp`).

### BUG-009 — No live translation on the Switch (High, latent)
- `ee_live.cpp` and `vu_live.cpp` compile to stubs on `__SWITCH__` ("code the build does not know still stops the game there"); unknown
  VU1 programs run in the interpreter.
- **Risk:** any Tekken 4 code path not reached by the PC exploring runs halts the game on the Switch; unknown VU1 programs are slow.
  `games/Tekken-4/entries.txt` is empty (0 bytes) in this repo.
- **Next:** run more/longer exploring runs on PC (`port.sh --frames/--rounds`), play more modes on PC with `PS2_VU_RECORD`/live builds
  and bring recorded programs/entries back into `generated/`; or add an EE interpreter fallback.

### BUG-010 — Unmeasured builds (Unknown)
- Commits `9475d4f`, `bd001fd`, `1103fd1`, `b6e27c3`, `5307556`, `a1c327d`, `aaed033`, `8e693c9` were verified on PC only.
  `build/switch/tests/A-fast … G-decode-profile` were never run; no logs returned.
- **Next:** run A then B, then the others (README.txt in `build/switch/tests`), keep logs named by letter.

### BUG-011 — `prod.keys` in the package folder (Medium, security)
- `build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/prod.keys` (14 612 bytes, dated 2026-10-24) — the user's console key file,
  placed there by the user to build the forwarder. Copying the package to the SD card copies the keys too. The user was warned
  (2026-10-30 09:02Z). It is still there. It is git-ignored (`build/`) and **was not copied into this handoff**.
- **Fix (user):** move it out of the package folder; never commit, upload or zip it.

### BUG-012 — `NAK_DEBUG=print` crash (Low)
- Adding `NAK_DEBUG=print` + `MESA_SHADER_CACHE_DISABLE=true` crashed at start; still crashed after `1549962` redirected stdout. Mesa's
  Rust code uses Linux-like std I/O. Use `PS2_VK_STATS=1` instead.

### BUG-013 — Misleading log lines on the Switch (Low)
- `[gpu] Direct3D 12 shader compiler: FXC (PS2_DX12_COMPILER=fxc|dxc picks)` is printed on the Switch; `[cpu] unknown processor: 0
  logical processors` (`std::thread::hardware_concurrency()` returns 0 on Horizon).
- **Fix:** guard the D3D12 line by platform; print "Nintendo Switch, 3 cores for the game" on `__SWITCH__`.

### BUG-014 — Controller detection lines (Unknown)
- Log: `[pad] Switch Controller (generic) in port 1`, `... in port 2`, then many `[pad] Switch Controller is not used: both ports have a
  controller`. Possibly handheld Joy-Cons and/or dock controllers each enumerated. Whether player 2 or a third pad matters: UNKNOWN.
- **Next:** log SDL controller names/GUIDs on the Switch; decide mapping (handheld as player 1).

### BUG-015 — Pad recording always on (Low)
- `[replay] recording the pad from frame 0 into sdmc:/switch/ps22switch/Tekken-4/states/run_2025-01-16_04-05-37_f000000.inputs` — the
  console date was 2025-01-16, and every run writes a recording to the SD card (I/O cost unknown). `--no-record` exists in the usage line.
- **Next:** add `--no-record` to `launch.txt` for performance tests, or default it off on the Switch.

### BUG-016 — `Switch_Build.cmd` and `py` (Low)
- The script uses `py -3` when `py` exists; on this PC `C:\Windows\py.exe` points at a missing `C:\Python313\python.exe`. The project
  always ran `python scripts/switch_port.py ...` directly.

### BUG-017 — IPU "bad code" messages (Low)
- `[ipu] bad code in command 0x10010000 (frame 680..687 ...)` on PC and `frame 395..402` on the Switch, in the same place of the intro
  film. Present in the PC reference too; visible effect: UNKNOWN. Upstream behaviour.

### BUG-018 — Stale build state (Low)
- `build-switch/CMakeCache.txt` (2026-10-29 22:40) has `CMAKE_TOOLCHAIN_FILE=/cmake/Switch.cmake` (wrong path) and no build files: an
  abandoned early configure. `build/pc-t4`, `build/pc-vk` caches set `PS2_WGPU_DIR=F:/ps22switch/third_party/wgpu-native`, which does
  not exist. WSL `/tmp` also holds early probe folders (`test-build`, `test_build`, `tekken4-build`, `sdl_build`, `pt`, `fw`).

### BUG-019 — Documentation drift (Low)
- Code comments reference upstream docs not in this repo: `docs/vu-live.md`, `docs/ee-live.md`, `docs/speed.md`, `docs/hitches.md`,
  `docs/replay.md`, `docs/linux.md`, `docs/sound.md`, `docs/keys.md`, `docs/gvz.md`, `docs/gow2*.md`, and others (the upstream copy on
  C: has only `docs/aot-window-capture.md`).
- `hw.wgsl` header says "To come: blending" although blending is implemented.
- README "There is **zero CPU emulation overhead** at runtime" is too strong (IOP is interpreted, unknown VU1 programs are interpreted).
- README "Graphics" section omits that the HW path must be enabled with `PS2_GS_HW=1`.

### BUG-020 — Forwarder untested (Unknown)
- `build/switch/Tekken-4/Tekken 4.nsp` not installed. Needs signature patches; the title ID `0500544B4E340000` must not clash with
  installed titles.

### BUG-021 — Tekken 5 (Medium, not a target)
- `generated/Tekken-5/` (33 files `m_004A8000_7382A761_unit_00NN.cpp`, 8 MB) exists from the upstream copy. Upstream `23f6332`: Tekken 5
  now runs its loading mini-game, then stops waiting for its sound module. Never built for or run on the Switch. User: Tekken 4 first.

### BUG-022 — Dispatch/barrier overhead (Medium)
- Every dispatch is followed by a full barrier (WebGPU semantics in the shim); every batch is a render pass. ~90 upload dispatches per
  frame (palette upload → CLUT load → next palette pattern). About 40 % of GPU busy time is not covered by per-dispatch timestamps.
- **Next:** see DEVELOPMENT_ROADMAP items P2–P4.

### BUG-023 — `PS2_HW_DECODE` (Low, experimental)
- On PC ~4 700 decode dispatches/s cost more than drawing saved. Only useful if texture reads are much dearer on the Switch.
  Off by default.

### BUG-024 — Reproducibility outside the repo (Medium)
- PC compiler/CMake/Ninja/wgpu-native/SDL2 come from `C:\ps2-native-recomp-main` (not a git repo; changed on 2026-11-02); the disc
  image and the recorded inputs used by `bench.sh` too; `generated/`, `games/`, `tools/`, `scratch/` are git-ignored. A fresh clone
  cannot build Tekken 4 without them.
- **Next:** document/automate (script that fetches the NVK SDK and checks for naga 29; keep `bench.sh` in the repo with paths as arguments).

### BUG-025 — 1x only (Low)
- HW targets are fixed 2048x2048 at native resolution; no upscaling (`PS2_SCALE` affects the compute path only). Design doc milestone 5.

### BUG-026 — Audio below full speed (Unknown)
- The emulation thread is paced by audio; below 60 fps the sound is stretched ("played slower, with the same pitch"). Quality in fights
  at ~25 fps was not reported.

### BUG-027 — Software renderer path (Low)
- `--software` and the automatic fallback were not built/run after `584dd8e`. Worked before (1 fps).
