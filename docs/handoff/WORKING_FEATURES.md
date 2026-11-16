# Working features (verified functionality)

Status classes: **VERIFIED** (fully implemented and observed working), **PARTIAL** (implemented, only partly tested),
**CONDITIONAL** (works under specific conditions), **EXPERIMENTAL** (implemented, not verified on target), **REGRESSED**
(worked earlier, now broken). Test environments: **SW** = real Nintendo Switch 1 run by the user (Ash) with logs;
**PC** = Windows 10, RX 6800 XT, scripted run `scratch/bench.sh`.

Nothing is listed as working only because code exists.

| ID | Feature | Class | Confidence |
|---|---|---|---|
| F01 | Tekken 4 EE code statically recompiled and running on the Switch | VERIFIED | high |
| F02 | Precompiled VU1 programs for Tekken 4 | VERIFIED | high |
| F03 | Switch build pipeline (`switch_port.py --skip-port`) | VERIFIED | high |
| F04 | SD-card package + `launch.txt` (arguments and `NAME=value` env) | VERIFIED | high |
| F05 | Boot → menus → story → fights on hardware with correct sound and 3D | VERIFIED | high |
| F06 | 16 MiB thread stacks and per-core thread placement | VERIFIED (indirect) | medium-high |
| F07 | Futex hand-over on Horizon | VERIFIED (indirect) | medium |
| F08 | Vulkan (NVK) GPU renderer, compute path | VERIFIED | high |
| F09 | Hardware-rasterizer path `PS2_GS_HW=1` | VERIFIED (with known gaps) | medium-high |
| F10 | Built-in overclock from `launch.txt` | VERIFIED | high |
| F11 | GPU heap reserve + NV transfer memory | PARTIAL | medium |
| F12 | `PS2_VK_PROFILE` GPU timing | VERIFIED | high |
| F13 | `PS2_PARTS` emulation-thread breakdown | VERIFIED | high |
| F14 | `PS2_PRESENT_LOG` fps/waits | VERIFIED | high |
| F15 | `PS2_VK_STATS` shader statistics | VERIFIED | high |
| F16 | Logging to `ps2run.log` / `ps2run.stdout.log` | VERIFIED | high |
| F17 | On-screen error when no program is found | EXPERIMENTAL | low |
| F18 | Memory card file `Tekken-4.ps2` | PARTIAL | low-medium |
| F19 | Controllers (Joy-Con / Pro via SDL) | PARTIAL | medium |
| F20 | Audio (SPU2 → SDL, time-stretch when slow) | VERIFIED (at 1 fps and later) | medium-high |
| F21 | Forwarder NSP | EXPERIMENTAL | low |
| F22 | PC pixel-comparison harness | VERIFIED | high |
| F23 | Vulkan shim on the PC GPU (`PS2_VULKAN=ON` on Windows) | VERIFIED | high |
| F24 | `PS2_HW_DECODE` texture pre-decode | EXPERIMENTAL | low |
| F25 | Tuning switches (`PS2_HW_MERGE`, `PS2_PRE_REORDER`, `PS2_SWITCH_CORES`, `PS2_VK_IN_FLIGHT`, `PS2_TILE`, `PS2_VK_ROBUST`, `PS2_VK_FMA`) | EXPERIMENTAL | low |
| F26 | Software renderer build (`--software`) | REGRESSED? / UNTESTED now | low |
| F27 | CMake guard: SPIR-V must match the WGSL | VERIFIED | high |
| F28 | Upstream PC (Windows) build of Tekken 4 | VERIFIED upstream; PC builds of this branch run | medium |

Total: **28 features**.

---

### F01 — Tekken 4 EE code statically recompiled and running on the Switch
- **Description:** MIPS R5900 code of `SLUS_203.28` translated to C++ (`generated/Tekken-4/unit_0000..004A.cpp`, `table.cpp`), compiled
  for AArch64 into the .nro.
- **Files:** `recomp/*`, `generated/Tekken-4/*`, `runtime/src/dispatch.cpp`.
- **Use:** part of every Switch build (`-DPS2_GENERATED=...`).
- **Tested:** SW, user played into fights (2026-10-29 22:53Z and every later test).
- **Limits:** no live translation on the Switch: code the PC exploring runs never reached would stop the game (BUG-009). Not observed for
  the parts of Tekken 4 played (menus, Story Battle, fights).

### F02 — Precompiled VU1 programs
- 13 VU1 program dumps in `games/Tekken-4/vu1/*.bin` (16 384 bytes each) → `generated/Tekken-4/vu1_000..002.cpp` + `vu1_table.cpp`.
  Unknown programs fall back to the interpreter (`vu_micro.cpp`). Verified by gameplay; how many programs ran interpreted on the
  Switch: UNKNOWN (no log line for it was captured).

### F03 — Switch build pipeline
- `scripts/switch_port.py` (+ WSL wrappers). Command and flags in BUILD_AND_SETUP.md §5. Last verified build 2026-10-30 14:56
  (`build/switch/Tekken-4/Tekken-4.nro`, 37 087 429 bytes; `scratch/sw.log` shows `[4/4] Building NRO executable target ps2run_nro`).

### F04 — SD-card package and `launch.txt`
- `host/main.cpp` `switch_early_environment()` (constructor 101), `switch_arguments()`; `switch_port.py` `package()`.
- Verified SW: log lines `[switch] arguments from ./launch.txt`, `[switch] PS2_CPU_MHZ=1785`, etc.
- Limitation: `switch_port.py` overwrites `launch.txt` and does not add `PS2_GS_HW=1` (BUG-002).

### F05 — Boots and plays on real hardware
- Expected: Tekken 4 from power-on through menus into fights. Observed (user, 2026-10-29): "everything boots, i can get into a fight
  ... sound and 3d rendering looks correct, no artifacts" (at 1 fps, software renderer). Later builds: menus, story, fights confirmed by
  logs. Full playthrough, other modes (Tekken Force, practice, saving): UNKNOWN.

### F06 — Thread stacks and core placement
- `host/thread.hpp` (16 MiB pthread stacks), `runtime/src/system.cpp` `thread_tune` (cores {1,2,0}).
- Verified indirectly: no stack-overflow crash since `601f70c`; no `[switch] cannot move a thread` line in the Switch log.

### F07 — Futex hand-over
- `runtime/src/gs/queue.cpp` (`svcWaitForAddress`/`svcSignalToAddress`). Verified indirectly (game runs; window thread "no work"
  time reported normally). Not micro-benchmarked.

### F08 — Vulkan renderer, compute path
- `gpu.cpp` + `gs.wgsl` through `vk/wgpu_vk.cpp` on NVK. Default GPU path when `PS2_GS_HW` is unset.
- Verified PC: 42/42 frames identical to wgpu-native. Verified SW: menus 60 fps (overclocked), fights ~7–9 fps.
- Log signature: `[gpu] NVIDIA Tegra X1 (GM20B) (NVK GM20B) (Vulkan 1.4.354, driver 109060100) on Vulkan`, `[gpu] 9 shaders compiled`.

### F09 — Hardware-rasterizer path (`PS2_GS_HW=1`)
- `hw.wgsl`, `gpu.cpp` `hw_*`, shim graphics support. Must be enabled in `launch.txt`.
- Verified SW (log on disk, build ≈ `28bab2c`): menus 60, story 45–55, fights 22–27 fps. Verified PC: identical through the shim
  vs wgpu-native's HW path; vs the compute reference, menus/story identical, fights differ in details; after lazy images 17/42 frames
  identical, others differ by 6–80 pixels.
- Gaps: lines and points not drawn (BUG-003), no destination alpha test (BUG-004), depth precision (BUG-005).
- Log signature: `[gpu] hardware rasterizer path on (PS2_GS_HW=1)`.

### F10 — Overclock
- `host/switch_clock.cpp`; `PS2_CPU_MHZ=1785`, `PS2_GPU_MHZ=921.6`, `PS2_MEM_MHZ=1600`; `PS2_CLOCK_FORCE=1` overrides sys-clk.
- Verified SW: `[clock] CPU 1020 -> 1785 MHz`, `[clock] GPU 307 -> 921 MHz`, `[clock] memory already at 1600 MHz`.
- Restore at exit and re-apply after docking: implemented, not explicitly confirmed by the user.

### F11 — GPU heap reserve / transfer memory
- `host/main.cpp` `__libnx_initheap` (512 MB / 128 MB), `__nx_nv_transfermem_size = 16 MB`. No out-of-memory since; benefit not
  isolated. Album-mode behaviour: UNTESTED.

### F12–F15 — Diagnostics (`PS2_VK_PROFILE`, `PS2_PARTS`, `PS2_PRESENT_LOG`, `PS2_VK_STATS`)
- All verified on the Switch (see `references/important_logs/switch_ps2run_2026-10-30_hw-path_fight.log` for the first three;
  `vk_raster.txt` in the package folder for the fourth). `PS2_VK_PROFILE` and `PS2_PARTS` cost some speed.

### F16 — Logs
- `ps2run.log` (stderr, line-buffered) and `ps2run.stdout.log` next to the .nro. Verified.

### F17 — On-screen error screen
- `switch_error()` in `host/main.cpp` (console, waits for +). Never triggered in tests: UNTESTED.

### F18 — Memory card
- `--memcard Tekken-4.ps2`; log: `[mc] card in slot 1: Tekken-4.ps2 (new, unformatted)`. Whether a save was written and reloaded on the
  Switch: UNKNOWN.

### F19 — Controllers
- SDL GameController (libnx). The user navigated menus and fought, so input works. The log shows two `Switch Controller (generic)` in
  ports 1 and 2 and repeated "is not used: both ports have a controller" (BUG-014). Button mapping quality: UNKNOWN.

### F20 — Audio
- SPU2 at 48 kHz stereo through SDL; `stretch.cpp` slows playback with the same pitch when the game runs slow. User: sound correct
  (2026-10-29). Sound in fights at ~25 fps: not commented on by the user.

### F21 — Forwarder NSP
- `build/switch/Tekken-4/Tekken 4.nsp` (181 472 bytes, title ID `0500544B4E340000`), starts `sdmc:/switch/ps22switch/Tekken-4/Tekken-4.nro`.
  Built 2026-10-30 10:02; never installed/launched (BUG-020).

### F22 — PC pixel-comparison harness
- `scratch/bench.sh`, `scratch/cmp.py`; reference frame folders in `scratch/` (`wgpu`, `vk`, `hw`, `hwvk*`, `hwdec0/1`, ...). Verified many
  times (outputs "same 42 different 0").

### F23 — Vulkan shim on PC
- `build/pc-vk/ps2run.exe` (`PS2_VULKAN=ON`, Vulkan headers from the NVK SDK, system loader at run time). Verified identical output.

### F24 — `PS2_HW_DECODE=1`
- `8e693c9`. PC: 42/42 identical, slight slowdown. Switch: test builds `G-decode`, `G-decode-profile` exist, **never run**.

### F25 — Tuning switches
- Defaults verified implicitly (they are what ran). Non-default values only exist in test builds C/D/E/F: **never run**.

### F26 — Software renderer
- `--software` → `-DPS2_VULKAN=OFF`; also the fallback "if the GPU path can't start". Worked at `601f70c` (1 fps). Not rebuilt or run since
  the Vulkan work: UNTESTED.

### F27 — SPIR-V/WGSL consistency guard
- `CMakeLists.txt` 133–148 compares sha256 of `gs.wgsl`/`hw.wgsl` with `spirv_table.cpp` and stops with a message. Verified (it is how
  stale tables were caught during development, per the transcript).

### F28 — Upstream PC build
- Upstream README: Tekken 4 at 60 fps on Windows 11 / RX 6800 XT (tested by DevCrashDay). This branch's PC builds (`pc-vk`, `pc-wgpu`)
  run the scripted 4 200-frame sequence (`exit 0, ~34–38 s, 42 frames`, `scratch/long1.txt`, `long2.txt`).
