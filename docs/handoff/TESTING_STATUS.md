# Testing status

## Test methods that exist

| Method | What it checks | How | Last run |
|---|---|---|---|
| PC pixel comparison | renderer correctness (shim vs wgpu-native, HW path vs compute) | `scratch/bench.sh` (4 200 frames, scripted presses, frame every 100 → 42 `.ppm`) + `scratch/cmp.py A B` | 2026-10-30 ~14:57 (`hwdec0` vs `hwdec1`: 42/42 identical) |
| Switch hardware run | real speed and behaviour | user runs the .nro, sends `ps2run.log` | 2026-10-30 10:19Z (fights ~22–27 fps) |
| Switch shader stats | NVK code quality | `PS2_VK_STATS=1` → `vk_*.txt` | 2026-10-30 ~09:23Z (`vk_raster.txt` in package folder) |
| Build check | compiles/links for Switch | `switch_port.py ... --skip-port` | 2026-10-30 14:56 (OK) |
| Upstream tests | `tests/*.py`, `tests/stretch_test.cpp`, `scripts/autotests.py` (ps2autotests 54/55 per README) | — | **not run in the Switch work** |

There is **no automated test suite for the Switch build** and no CI.

## Results log (PC, `scratch/` folders, RX 6800 XT)

| Comparison | Result | Meaning |
|---|---|---|
| `wgpu` vs `vk` (compute path, wgpu-native vs shim) | same 42, different 0 (repeated after each shim change) | shim is exact |
| `wgpu` vs `base` (compute vs software renderer, 1 500 frames) | same 5, different 10 (e.g. `frame_01400` 970 px, max 203) | software and GPU renderers differ (upstream; expected) |
| `hw` (HW path, wgpu-native) vs `hwvk` (HW path, shim) | same 42, different 0 | shim graphics support is exact |
| HW path before vs after lazy images (`28bab2c`) | same 17, different 25 (6–80 px, max 11–59) | depth precision (BUG-005) |
| `hwdec0` vs `hwdec1` (`PS2_HW_DECODE` off/on) | same 42, different 0 | decode option is exact |
| Timing `long1` (wgpu) / `long2` (vk), 4 200 frames headless | 37 562 ms / 33 796 ms | shim not slower on PC |

## Results log (Switch 1, real hardware, overclocked unless noted)

| Build (commit) | Menus | Story/FMV | Fights | Evidence |
|---|---|---|---|---|
| `601f70c` software renderer, stock clocks | ~1 | ~1 | ~1 | user message |
| `584dd8e` | crash (OOM) | — | — | reported log |
| `25fb2ba` | frozen first frame, device lost | — | — | reported log |
| `f42606b`/`518f6b6`, stock | ~12 (CPU) | — | 8–10 | reported |
| `9709c64` | crash at start | — | — | user |
| `6dcfc5d` | — | — | emulation waits GPU 70–94 % | reported |
| `1549962` + NAK_DEBUG | crash | — | — | user |
| `526bf32`, stock | 24–34 | ~14 (FMV) | ~9 | reported |
| `8d15359` overclock | 60 | 30–46 | ~7 | reported |
| `4d0bf6c`+`ece356f` HW path | 60 | 40–50 | 12–13 | reported |
| `ce560bf` | — | — | slower than before | user ("now slower") |
| `d9b9eef` | 60 | — | 22–24 | user ("faster") + reported |
| ≈`28bab2c` | 60 | 45–55 | 22–27 | **on disk**: `references/important_logs/switch_ps2run_2026-10-30_hw-path_fight.log` |
| `aaed033` era, test builds A–F | ? | ? | ? | **not run** |
| `8e693c9`, test builds G | ? | ? | ? | **not run** |

## Most recent successful tests
- Switch: the ≈`28bab2c` run above (2026-10-30 10:19Z).
- PC: `PS2_HW_DECODE` parity, 42/42 (2026-10-30 ~14:57 local).
- Build: Switch .nro of `8e693c9` (2026-10-30 14:56).

## What has never been tested
- Test builds A–G on the console; the forwarder NSP; album-mode (non-title-takeover) launch; save/load of the memory card on the
  Switch; docked mode/clock re-apply; long sessions (heat, memory growth); game modes beyond menus/Story Battle; the software renderer
  build since the Vulkan work; upstream tests on this branch; other PC GPUs; Tekken 5 on the Switch.

## How to test a change (recommended routine)
1. PC: rebuild `build/pc-vk` (and `build/pc-wgpu` for HW-path reference), run `bench.sh` into a new folder, `cmp.py` against the last
   good folder (`scratch/hwvk11` / `scratch/hwdec0` for the HW path, `scratch/wgpu` for compute). Expect 42/42 or explain each difference.
2. Switch: `switch_port.py ... --skip-port --no-disc-copy`, add `PS2_GS_HW=1` + `PS2_PRESENT_LOG=1` (+ `PS2_VK_PROFILE=1`,
   `PS2_PARTS=1` for a profile build) to `launch.txt`, hand the user `Tekken-4.nro` + `launch.txt`, ask for ~1 minute into a fight
   and the `ps2run.log`.
3. Compare `[waits]` fps and `[vk-profile]` lines with the table above.
