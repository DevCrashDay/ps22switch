# Project history: ps22switch (PS2 static recompilation for Nintendo Switch 1)

Handoff written 2026-11-04. All times are local (CET, UTC+1) unless marked `Z` (UTC). Git commit dates are local (+0100); project
thread timestamps are UTC.

## Sources used and their limits

| Source | What it gave | Reliability |
|---|---|---|
| `git log` of `F:\ps22switch` (60 commits, branches `main`, `claude/project-thread-i8q90o`) | exact changes, commit messages with measurements | verified |
| Project thread "Get Tekken 4 running on Switch" (92 replies, 2026-10-29 22:26Z to 2026-10-30 14:58Z) | user reports from the real Switch, decisions, approvals | verified (read in full) |
| Project chat (coordinator) | goal statement, approvals relayed | verified |
| Previous Claude Code session transcript (`C:\Users\Christopher Schimpf\.claude\projects\F--ps22switch\0a411a9d-....jsonl`, 15 MB) and its compaction summary | commands actually run, failed attempts, tool installs | verified by reading; it was **not** copied into this package (local-only, personal transcript) |
| Claude project memory `switch-port.md` | build setup notes | verified, summarised here |
| Logs on disk: `build/switch/Tekken-4/package/.../ps2run.log` (Switch), `scratch/*/log.txt` (PC) | measured fps / GPU time | verified |
| User's Switch logs sent as thread attachments (about 12 logs) | most of the per-step fps numbers | **only the last one is on disk**; the others are known only through the agent's replies quoting them. Numbers from those are marked "(reported)" |

What is not available: the attachments of earlier Switch logs, `vk_*.txt` files from the Switch other than the ones in the package
folder, and any record of how commit `f89a7c6` (the first Switch commit, made before the project thread started) was written.

## Chronological timeline (milestones)

Each milestone: what, why, files, tools, outcome, lessons, next step.

### M01 — 2026-10-28: Upstream fork imported (`0660c5e "Import ps2-native-recomp"`)
- **What:** The repo `DevCrashDay/ps22switch` starts as an import of exoteric33's `ps2-native-recomp` (itself based on Samir Nuri's
  `ps2-web-recomp`): a static recompiler that turns a PS2 game's MIPS R5900 code into C++ and runs it with an emulated PS2 runtime on
  Windows/Linux/macOS/web.
- **Files:** everything except the Switch-specific files listed in M03+ (152 files, 47 366 lines).
- **Outcome:** verified (git). Upstream README claims Tekken 4 plays at 60 fps on Windows (tested by DevCrashDay, upstream issue #8).

### M02 — 2026-10-28/29: Upstream development continues (27 commits by exoteric33/exoteric up to `ba3d471`, then 8 more up to `23f6332`)
- README game table, GTA III fix, live translation in background, PS2_IOP_LOAD, VIF1 state fix, waiting-loop skip (`30d1d2d`,
  `PS2_NO_SPIN_SKIP`), Tekken 5 black-screen fix (`23f6332`: ELF loader zeroes segments first, live translation of code loaded at
  run time, IOP SIF calls). Not Switch work; listed so it is not mistaken for it.

### M03 — 2026-10-29 23:05: First Switch commit (`f89a7c6`, author xcrashdayx, before the project thread)
- **What:** "Implement Nintendo Switch (Switch 1) recompilation pipeline and runtime port": `scripts/switch_port.py` (250 lines),
  `Switch_Build.cmd`, `NINTENDO_SWITCH` blocks in `CMakeLists.txt` (NRO/NACP generation), Switch code in `host/main.cpp` and
  `host/window.cpp`, ARM NEON alternatives in `runtime/include/ps2/vu_ops.hpp`, live translation disabled on Switch
  (`ee_live.cpp`, `vu_live.cpp`, `live_build.*`), README rewritten for the Switch.
- **How it was made:** UNKNOWN — not verified from available evidence (no transcript; the thread started 22:26Z = 23:26 local).
- **Outcome:** compiled; it drew with the reference **software** GS renderer only. Had hard-coded Tekken 4 paths, 128 KiB thread
  stacks, all threads on core 0 (see M05).

### M04 — 2026-10-29 22:26Z: Project thread starts ("look through the current implementation and fix bugs ... implement a ps2 to switch 1 version")
- Agent confirms a devkitA64 build works in WSL distro `devkita64`; notices `C:\ps2-native-recomp-main` is newer than the import.
- User (22:35Z): **the first target is Tekken 4, not Tekken 5** ("tekken 4s recomp to windows was successful").

### M05 — 2026-10-29 23:31 / 23:44: Upstream merge and Switch port fixes (`e1b87f6`, `601f70c`)
- **Why:** code review found crash/stall risks on real hardware.
- **Changes (601f70c):**
  - `host/thread.hpp` (new): `HostThread` — pthread with a **16 MiB stack** on Switch (libnx default is 128 KiB, too small for the
    translated code's deep call chains).
  - `runtime/src/system.cpp` `thread_tune`: `svcSetThreadCoreMask`, emulation core 1, VU core 2, window core 0 (Horizon keeps every
    thread on core 0 otherwise).
  - `runtime/src/gs/queue.cpp`: futex via `svcWaitForAddress(&word, 2 /*WaitIfEqual*/, seen, ns)` and
    `svcSignalToAddress(&word, 0, 0, -1)` instead of 1 ms sleeps.
  - `CMakeLists.txt`: `-ffp-contract=off` for Switch (GCC would fuse multiply-add and change PS2 FPU/VU results).
  - `host/window.cpp`: removed an extra `appletMainLoop()` check that ate SDL's quit.
  - `host/main.cpp`: `launch.txt` next to the .nro (one argument per line, `NAME=value` lines become environment variables), chdir to
    the .nro folder, `ps2run.log`, on-screen error screen.
  - `scripts/switch_port.py`: runs upstream `scripts/port.sh --native-only` (exploring runs + VU1 compile) before cross-compiling,
    then packages `disc/<ELF>`, `disc.iso`, `launch.txt`, memory card name, `keys.ini`.
- **Tools:** devkitA64 15.2.0 in WSL `devkita64`, `/usr/local/bin/switch-cmake`, `switch-ninja`.
- **Outcome (verified by user on a real Switch 1, 22:53Z):** "everything boots, i can get into a fight, but everything runs in
  **1fps**, BUT sound and 3d rendering looks correct, no artifacts".

### M06 — 2026-10-29 22:55Z–23:00Z: Diagnosis of 1 fps and the renderer decision
- PC profile (software renderer): rasterisation ~87 % of time; whole game ~6x slower than with the GPU renderer.
- Options offered: (1) tune the software renderer (est. 1.5–3x), (2) a real Switch GPU renderer.
- **User decision:** "there is no point in 1 if we dont reach full gameplay speed, so do 2 instead" (real Switch 1, not emulator).
- Coordinator relays the goal: Vulkan via the approach of `github.com/DevCrashDay/OpenTrickyNX` (Mesa NVK on libnx), not OpenGL.

### M07 — 2026-10-29 23:14Z–23:46Z: Toolchain for Vulkan
- OpenTrickyNX cloned to a temp folder and studied (NVK linking, SDL2 EGL/glapi swap).
- User approved ("Ok", 23:46Z): download `mesa-switch-nvk-sdk.zip` (52 797 811 bytes) from the OpenTrickyNX `nvk-sdk` release
  into `tools/nvk-sdk` (Mesa revision `danfromtico/mesa-switch 0ff52079ca9e9e837ed27eb95a8d239f439a1864`), and install Rust +
  `naga-cli` in WSL (`cargo install naga-cli` → naga 30.0.1 at `/root/.cargo/bin/naga`; later `cargo install naga-cli --version '^29'
  --root /root/naga29` to match wgpu-native 29).

### M08 — 2026-10-30 00:52: WebGPU-on-Vulkan shim (`584dd8e`)
- **What:** `runtime/src/gs/gpu/vk/wgpu_vk.cpp` implements the ~66 WebGPU C functions `gpu.cpp` calls, on Vulkan; shaders become
  SPIR-V offline via `scripts/wgsl_spirv.py` → `spirv_table.cpp`; CMake `PS2_VULKAN`/`NVK_SDK`; vendored wgpu-native v29 headers.
- **Why:** reuse the PC's exact GPU renderer (compute shader GS) unchanged instead of writing a new renderer.
- **Verified on PC (RX 6800 XT):** 42 of 42 frames of the scripted Tekken 4 run (power-on → fight) pixel-identical to wgpu-native
  (`scratch/bench.sh`, `scratch/cmp.py`, folders `scratch/wgpu` vs `scratch/vk`).

### M09 — Switch test 1 (≈23:59Z): crash at start → staging fix (`25fb2ba`)
- Log (reported): Vulkan started, 9 shaders compiled, then out of memory: every submission took its own 16 MiB staging block.
- Fix: one shared staging ring reused once the GPU passes its last reader. PC: 2–3 blocks, still pixel-identical.

### M10 — Switch test 2 (00:06Z): first frame frozen, audio continues → in-flight cap (`f42606b`)
- Log (reported): GPU fell 230+ submissions behind; NVK reported **device lost**.
- Fix: CPU waits when 8 submissions ahead (`PS2_VK_IN_FLIGHT`), `PS2_VK_PROFILE=1` GPU timestamps, `launch.txt` `NAME=value` env
  lines, SPIR-V regenerated with naga 29.

### M11 — Switch test 3 (00:19Z): "still slow, just as the sw renderer before"
- Raster shader ~10 ms per batch on Switch vs ~0.01 ms on PC.
- **Failed hypothesis:** GPU memory type with CPU coherency (`518f6b6` picks the type with fewest extra flags). The Switch exposes
  only two memory types, both the same RAM → **no effect**. Also added SPIR-V tile variants 4/8/16 (`PS2_TILE`).
- Another log (00:27Z, reported): menus ~12 fps with emulation thread ~80 ms/frame; fights GPU-limited 8–10 fps. Suggestion to
  overclock.

### M12 — 2026-10-30 01:31 → 08:55Z: PS2_PARTS emulation-thread sampler (`9709c64`) — crashed → fixed (`6dcfc5d`)
- No sampling profiler on Switch, so the emulation thread tags which subsystem it is in; a helper thread samples it.
- User: "it crashed immediatly" → sampler thread now created with an explicit 256 KB stack and logs `[parts] sampling...` first.
- Result (reported, 09:03Z): in fights the emulation thread waits for the GPU 70–94 %; game code 5–35 %. **GPU is the bottleneck.**

### M13 — 09:00Z–09:02Z: NSP forwarder ("fake nsp that points to the nro with a tekken 4 cover")
- `tools/forwarder/` copied from OpenTrickyNX (`port/switch/forwarder`, GPL-3.0 `make_forwarder.py`), `DEFAULT_NRO` changed to
  `sdmc:/switch/ps22switch/Tekken-4/Tekken-4.nro`, title ID `0x0500544B4E340000`, built in WSL (`make -C /tmp/fw RELEASE=1`).
- Packed with the user's `prod.keys` and `cover.jpg` (paths given by the user) →
  `build/switch/Tekken-4/Tekken 4.nsp` (181 472 bytes). **Never installed or launched on a console (untested).**
- Warning given to the user: move `prod.keys` out of the SD-card package folder. As of 2026-11-04 it is **still there**.

### M14 — 09:03Z–09:09Z: Shader-compiler dump attempts
- `NAK_DEBUG=print` + `MESA_SHADER_CACHE_DISABLE=true` in launch.txt → **immediate crash** (Mesa's Rust NAK writes to stdout).
- `1549962`: stdout → `ps2run.stdout.log`, dup2 onto fds 1/2 → **still crashed** (Rust std I/O behaves like Linux; not usable on
  Horizon). Approach abandoned.
- Replacement `95decfe` `PS2_VK_STATS=1`: `VK_KHR_pipeline_executable_properties` stats and machine code to `vk_<program>.txt`.
  Worked on the Switch (`vk_raster.txt`, `vk_resolve.txt`, `vk_snapshot.txt` are in the package folder, 10:23 local).

### M15 — 09:19Z–09:24Z: Robust buffer access removed (`526bf32`)
- Finding from Switch `vk_raster.txt`: NVK implements robust buffer access as a branch around each of ~150 memory reads (~480
  branches in the raster program).
- Fix: naga `--index-bounds-check-policy Restrict --buffer-bounds-check-policy Restrict`; robust access off unless `PS2_VK_ROBUST=1`.
- Result (reported): menus 19–26 → 24–34 fps, raster instructions −20 %, fights still ~9 fps.
- Conclusion: the compute-raster design (one workgroup per 8x8 tile walking every primitive, 104 registers/thread on Tegra → 16
  warps) is near its limit on this GPU.

### M16 — 09:24Z–09:34Z: Decision card → hardware rasterizer + built-in overclock (`8d15359`)
- Options card: (a) hardware-rasterizer GS renderer [recommended], (b) squeeze the compute renderer (1.5–2x), (c) CPU-side wins.
- User: "go ahead, also make the sys clk already active for the nro through the launch.txt file like you did for opentricky".
- `host/switch_clock.cpp`: `PS2_CPU_MHZ=1785`, `PS2_GPU_MHZ=921.6`, `PS2_MEM_MHZ=1600` via clkrst (≥8.0.0) or pcv, keeper thread
  re-applies after docking/home menu, restores at exit; sys-clk/Horizon OC profile wins unless `PS2_CLOCK_FORCE=1`.
- Result (verified in log, reported fps): `[clock] CPU 1020 -> 1785 MHz`, GPU 307 → 921; menus solid **60 fps**, story/FMV 30–46,
  fights still **~7 fps** → purely GPU-bound.

### M17 — 09:35Z–09:51Z: Hardware-rasterizer GS path (`c191089` design, `6210d1a`, `4d0bf6c`)
- Design doc `docs/switch-hw-renderer.md` (milestones 1–5).
- `6210d1a` (`PS2_GS_HW=1`): new `hw.wgsl`; batches drawn by the GPU rasterizer into 2048x2048 colour (RGBA8) + depth (D32F) images,
  filled from VRAM inside the batch box before and written back after; PS2 blend as fixed function (dual-source alpha, FIX as
  blend constant). Developed in WebGPU on the PC first.
- `4d0bf6c`: shim gains 2D textures (GENERAL layout), views, render pipelines, dual-source blending, Vulkan 1.3 dynamic rendering.
- PC results: through the shim pixel-identical to wgpu-native's HW path; HW vs compute: menus/story identical, fights differ in
  shading details (floor, effects). **Lines, points, destination alpha test not implemented.**
- User asked to study NetherSX2_nx and ARMSX2-NX. NetherSX2 runs PCSX2's HW Vulkan renderer on NVK and has a Deko3D backend;
  ARMSX2-NX states it is not playable. Copied (re-written) NetherSX2's memory settings → `ece356f`: `__libnx_initheap` leaves 512 MB
  (128 MB in album mode) unmapped for nvmap; `__nx_nv_transfermem_size = 16 MB`.
- First Switch HW test (reported, 10:00Z): fights **7 → 12–13 fps**, menus 60, story 40–50.

### M18 — 10:00Z–10:08Z: Remove decode on HW path; CLUT loads (`ce560bf` → regression → `d9b9eef`)
- Log showed `decode` ~170 ms/s and `load_clut` ~160 ms/s in fights.
- `ce560bf`: HW path skips texture pre-decode; CLUT loads in parallel (each entry walks back over earlier loads).
- User: "new log. now slower" → the parallel walk is O(loads²) with hundreds of loads per batch (0.58 s of GPU per second).
- `d9b9eef`: CPU builds an owner table (which load last wrote each of a slot's 32 16-entry blocks); shader looks it up. ~20 µs per
  batch on PC. User: "faster" → fights **22–24 fps**.

### M19 — 10:08Z–10:29Z (user away "a few hours", asked to iterate and prepare test .nro files)
- `8b45d6f` setup texture fields as flat vertex outputs; `28bab2c` keep colour/depth images between batches (lazy write-back;
  fills/write-backs 1400 → 600 per second on PC; **depth stays float → 25 of 42 PC frames differ by 6–80 pixels**);
  `9475d4f` VRAM bound read-only (binding 5) instead of atomic loads (NVK strong loads skip L1); `bd001fd` no CPU tile binning on HW
  path; `1103fd1` uploads before CLUT loads when order allows (`PS2_PRE_REORDER=0` off); `b6e27c3` `PS2_SWITCH_CORES`;
  `569b216` `docs/switch-performance-plan.md`; `5307556` present timing in `[waits]`; `a1c327d` merge render passes with only CLUT
  loads between (`PS2_HW_MERGE=0` off); `aaed033` FIX splits draw runs only when the blend uses it.
- Test builds A–F in `build/switch/tests` (README.txt).
- **Last Switch log on disk** (user, 10:19Z; build of ≈10:13Z, i.e. commit `28bab2c` — reconstructed from timing, the log lacks the
  "showing the picture" field added in `5307556`): menus 60, story 45–55, **fights ~22–27 fps**; GPU busy ~600 ms/s (hw_draw ~340,
  upload ~100 over ~2 500 dispatches, load_clut ~48, load_color ~43, writeback ~52); window thread 999 ms/s busy; emulation thread
  waits for the window ~44 %.

### M20 — 10:31Z–13:58Z: `PS2_HW_DECODE` (`8e693c9`) and test builds G
- Pre-decode each submit's textures once into a buffer (cached for the frame), pixel shader reads one word. Off by default.
- Usage limit hit at 10:34Z (commit pushed, PC check pending). Resumed 13:56Z: PC check **42/42 identical** (`scratch/hwdec0` vs
  `hwdec1`), but on PC a small loss (~4 700 decode dispatches/s). Builds `G-decode`, `G-decode-profile` made (14:56 local).
- Final state: all work pushed to `origin/claude/project-thread-i8q90o` (HEAD `8e693c9`). Thread went offline 14:58Z.
  **No Switch logs for test builds A–G were ever returned.**

### M21 — 2026-11-02: Upstream copy at `C:\ps2-native-recomp-main` changed (observation only)
- Files there are dated 2026-11-02 20:18 (README 17 284 bytes vs 14 428 here, CMakeLists 14 560 bytes) and a `FIFA Street 2` game
  was added. It is not a git checkout. Not merged; whether it holds new upstream fixes: UNKNOWN — not verified from available
  evidence.

### M22 — 2026-11-04: This handoff package.

## Measured progress summary (Tekken 4 on a real Switch 1)

| Stage | Menus | Story/FMV | Fights | Source |
|---|---|---|---|---|
| Software renderer (`601f70c`) | ~1 fps | ~1 fps | ~1 fps | user report |
| Compute renderer on Vulkan, stock clocks (`526bf32`) | 24–34 | 12–14 (FMV) | ~9 | reported |
| + overclock 1785/921/1600 (`8d15359`) | 60 | 30–46 | ~7 | reported |
| HW path first (`4d0bf6c`+`ece356f`) | 60 | 40–50 | 12–13 | reported |
| + CLUT owner table (`d9b9eef`) | 60 | 40–50 | 22–24 | reported |
| + flat setup, lazy images (`28bab2c`, reconstructed) | 60 | 45–55 | 22–27 | `ps2run.log` on disk |
| Commits after `28bab2c` (read-only VRAM, no binning, reorder, merge, FIX split, HW_DECODE) | ? | ? | ? | **never measured on Switch** |

## Lessons learned (do not relearn)
1. On the Switch the GPU, not the CPU, limits fights once the clocks are raised.
2. NVK's robust buffer access is software branches: keep it off and clamp in the shader.
3. NVK atomic (strong) loads bypass L1: bind read-only buffers for reads.
4. Mesa debug printing (`NAK_DEBUG=print`) crashes on Horizon; use `PS2_VK_STATS=1`.
5. A per-entry parallel algorithm that rescans history is O(n²) on real batches (hundreds of CLUT loads): precompute on the CPU.
6. Memory-type choice does nothing on the Switch (unified memory, two types).
7. Always rebuild the package `launch.txt`-aware: `switch_port.py` regenerates `launch.txt` **without** `PS2_GS_HW=1` (see BUG-002).
8. Always check on PC with `scratch/bench.sh` + `scratch/cmp.py` before sending a build to the console.
