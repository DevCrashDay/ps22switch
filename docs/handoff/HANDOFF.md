# HANDOFF — ps22switch (PS2 → Nintendo Switch 1 static recompilation, Tekken 4)

Written 2026-11-04 from the filesystem, git, build outputs, logs, the project's discussion thread and the previous Claude Code session.
Project last worked on: **2026-10-30 ~15:00 CET**. HEAD: `8e693c9` on branch `claude/project-thread-i8q90o` (pushed).
Where evidence was missing this package says "UNKNOWN — not verified from available evidence."

---

## READ THIS BEFORE MAKING ANY CODE CHANGES

**State in one paragraph.** Tekken 4 runs on a real Switch 1 — menus 60 fps, story/FMV 45–55 fps, **fights 22–27 fps** — with the
Vulkan (Mesa NVK) GPU renderer and the new hardware-rasterizer path (`PS2_GS_HW=1`) plus a built-in overclock. The goal (60 fps in
fights) is not reached; the GPU and the thread that feeds it are the limit. Seven optimisation commits after the last measurement and
eight test builds (`build/switch/tests/A..G`) have never been run on the console.

**Most sensitive components (break these and nothing works or results silently change):**
1. `runtime/src/gs/gpu/vk/wgpu_vk.cpp` — the WebGPU-on-Vulkan shim. Keeps WebGPU ordering (barriers, staging, maps). Every change must
   keep `scratch/cmp.py wgpu vk` at 42/42.
2. `runtime/src/gs/gpu/gs.wgsl`, `hw.wgsl` ↔ `runtime/src/gs/gpu/vk/spirv_table.cpp` — after **any** WGSL edit run
   `python3 scripts/wgsl_spirv.py --naga /root/naga29/bin/naga` in WSL (naga **29**, not 30). CMake refuses to configure otherwise.
3. `runtime/src/gs/gpu/gpu.cpp` HW path (`hw_*`, lazy images, CLUT owner table, reorder, merge) — correctness vs the compute reference.
4. `CMakeLists.txt` Switch section: `-ffp-contract=off` (PS2 float exactness), NVK link group with `-Wl,-u,vk_icd*`, SDL2 EGL/glapi swap.
5. `host/main.cpp` Switch code: `__libnx_initheap` GPU reserve (512/128 MB), `__nx_nv_transfermem_size`, `launch.txt` parsing before
   static init, log redirection.
6. `host/thread.hpp` 16 MiB stacks and `runtime/src/system.cpp` core placement — removing them crashes or serialises the game.

**Known pitfalls:**
- `scripts/switch_port.py` rewrites the package `launch.txt` **without `PS2_GS_HW=1`** → the slow compute renderer runs (BUG-002). Add
  the line after every build (or fix the script).
- `build/switch/Tekken-4/package/.../prod.keys` is the user's console key file: never copy it to the SD card, never zip, upload or commit it.
- Never set `NAK_DEBUG=print` on the Switch (crash). Use `PS2_VK_STATS=1`.
- Never turn robust buffer access back on for speed tests (NVK makes it a branch per access). `PS2_VK_ROBUST=1` only for debugging.
- Switch threads must be created with explicit stack sizes (`HostThread`, or `pthread_attr_setstacksize`).
- Parallel GPU algorithms that rescan history are O(n²) on real Tekken 4 batches (CLUT regression `ce560bf`).
- Git Bash mangles `/opt/...` paths passed to `wsl`; heredocs mangle `\n` in Python edit scripts. Use PowerShell / script files.
- The PC toolchain, the disc image and the bench inputs live in `C:\ps2-native-recomp-main` (outside the repo).
- No run-time code translation on the Switch: code the PC exploring runs missed stops the game.

**Instruction:** read [ARCHITECTURE.md](ARCHITECTURE.md), [BUGS_AND_LIMITATIONS.md](BUGS_AND_LIMITATIONS.md) and
[TECHNICAL_DECISIONS.md](TECHNICAL_DECISIONS.md) (especially "approaches that failed") before changing code; verify every renderer
change on the PC with `scratch/bench.sh` + `scratch/cmp.py` before building for the Switch.

---

## 1. Project name and purpose
**ps22switch** — recompile PS2 games the user owns into native Nintendo Switch 1 homebrew. Fork of
[exoteric33/ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp). Details: [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md).

## 2. What it does
On a Windows PC it translates the game's MIPS R5900 code to C++ (`generated/<game>`), records and translates VU1 programs, then
cross-compiles everything with devkitA64 (in WSL) together with an emulated-PS2 runtime into `<game>.nro`, and lays out an SD-card folder
(`.nro`, boot ELF, disc image, `launch.txt`). On the Switch the .nro reads the disc image from the SD card while it runs.

## 3. Current status
| Area | Status |
|---|---|
| Boot / gameplay / audio / 3D | working on real hardware (Tekken 4) |
| Speed | menus 60, story 45–55, fights 22–27 fps (target 60) |
| Renderer | Vulkan via NVK; compute path (reference) + hardware-rasterizer path (`PS2_GS_HW=1`, missing lines/points/DATE) |
| Pending | measure test builds A–G; make HW path default; texture cache, fewer barriers, direct present |
| Completion estimate | see the paragraph below the table |

**Completion estimate (methodology):** of the user's goal "Tekken 4 at full speed on Switch 1", functional parts are done (boots, plays,
correct output) and the frame rate is 60/60 in menus and ~25/60 in fights. A percentage would be invented; the honest measure is
**fights at ~40 % of the target frame rate**, with the remaining gap believed to be GPU work (see BUG-001).

- **Development phase:** performance optimisation of the hardware-rasterizer GS path.
- **Last completed task:** `PS2_HW_DECODE` (commit `8e693c9`) verified 42/42 on PC; test builds G-decode / G-decode-profile made.
- **Current active task:** none (stopped 2026-10-30); next is measuring A–G.
- **Last known working build on hardware:** the package build of ≈ commit `28bab2c` (2026-10-30 ~11:13 CET); its log is
  `references/important_logs/switch_ps2run_2026-10-30_hw-path_fight.log`.
- **Last known working commit on PC:** `8e693c9`.
- **Blockers:** none technical; waiting for console measurements (user action).

## 4. Current architecture (short)
```
PC: disc.iso → port.sh (eerecomp, exploring runs, vurecomp) → generated/Tekken-4/*.cpp
    switch_port.py → WSL devkitA64 + NVK SDK → Tekken-4.nro → SD package
Switch: emulation thread (core 1, translated EE code, IOP, SPU2, IPU)
        VU thread (core 2, VIF1/VU1)          ─┐ GS queue
        window thread (core 0, SDL, gpu.cpp) ◄─┘ → wgpu_vk.cpp → NVK → Tegra X1; picture read back → SDL present
```
Full description: [ARCHITECTURE.md](ARCHITECTURE.md).

## 5. Development environment
Windows 10 Pro 19044 PC (`CHRIS`), project on `F:\ps22switch`; WSL **1** distro `devkita64` (Debian 12) with devkitA64 r29.2 (GCC 15.2.0),
libnx 4.12.0, switch-sdl2 2.28.5, CMake 3.31.6, Ninja 1.11.1; NVK SDK in `tools/nvk-sdk`; naga 29.0.4 in WSL; PC toolchain llvm-mingw
clang 23.1.1 / CMake 4.4.4 in `C:\ps2-native-recomp-main\tools\portable`; Python 3.11.7 + Pillow 10.2.0; AMD RX 6800 XT for PC checks;
real Switch 1 with hbmenu. Details: [BUILD_AND_SETUP.md](BUILD_AND_SETUP.md).

## 6. Important tools
devkitA64/libnx/switch-sdl2 (WSL), Mesa NVK SDK (OpenTrickyNX release), naga-cli 29, llvm-mingw (PC), `scripts/switch_port.py`,
`scripts/wgsl_spirv.py`, `scratch/bench.sh` + `scratch/cmp.py`, the built-in diagnostics `PS2_PRESENT_LOG`, `PS2_VK_PROFILE`, `PS2_PARTS`,
`PS2_VK_STATS`. All 60: [TOOL_INVENTORY.md](TOOL_INVENTORY.md).

## 7. Documentation index
| File | Content |
|---|---|
| [HANDOFF.md](HANDOFF.md) | this entry point |
| [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) | purpose, goal, status in brief |
| [PROJECT_HISTORY.md](PROJECT_HISTORY.md) | chronological timeline M01–M22, measured progress, lessons |
| [ARCHITECTURE.md](ARCHITECTURE.md) | components, execution flow, threads, renderer, shim, memory, interfaces |
| [FILE_INVENTORY.md](FILE_INVENTORY.md) | every tracked file; untracked files by family; missing files; large binaries |
| [FILETREE.txt](FILETREE.txt) | complete recursive tree with sizes and tags (1 992 files) |
| [TOOL_INVENTORY.md](TOOL_INVENTORY.md) | 60 tools/approaches with versions and status |
| [BUILD_AND_SETUP.md](BUILD_AND_SETUP.md) | setup, build, run, test, debug, clean commands (verified/untested labels) |
| [WORKING_FEATURES.md](WORKING_FEATURES.md) | 28 features with verification class |
| [BUGS_AND_LIMITATIONS.md](BUGS_AND_LIMITATIONS.md) | 27 issues with severity, evidence, next steps |
| [TECHNICAL_DECISIONS.md](TECHNICAL_DECISIONS.md) | 23 decisions + 20 failed approaches |
| [DEVELOPMENT_ROADMAP.md](DEVELOPMENT_ROADMAP.md) | prioritised tasks R1–R18 + long-term ideas |
| [GIT_STATE.md](GIT_STATE.md) | branches, commits, essential vs experimental, diffs preserved |
| [TESTING_STATUS.md](TESTING_STATUS.md) | test methods, all PC/Switch results, untested areas |
| [AUDIT_REPORT.md](AUDIT_REPORT.md) | documentation audit and remaining gaps |
| `references/important_logs/` | the last Switch log, PC logs (software, HW path decode off/on), profiles, build tail |
| `references/important_diffs/` | all Switch changes vs upstream, focused diffs, full git log |
| `references/additional_documentation/` | design + performance docs, test-build README and every test `launch.txt`, package `launch.txt`/`app.json`, `bench.sh`, `cmp.py`, example `keys.ini` |

## 8. Complete filetree location
[FILETREE.txt](FILETREE.txt) (generated 2026-11-04 from `F:\ps22switch`, excluding `.git/` and `handoff/`).

## 9. Working functionality (summary)
Boot to fights with correct audio/3D on hardware; Vulkan renderer (compute) and HW-rasterizer path; overclock from `launch.txt`;
`launch.txt` arguments + environment; 16 MiB stacks / core placement / futex; GPU memory reserve; diagnostics; Switch build pipeline;
PC pixel-comparison harness; forwarder NSP (built, untested). Full list: [WORKING_FEATURES.md](WORKING_FEATURES.md).

## 10. Known problems (top)
BUG-001 fights 22–27 fps · BUG-002 package `launch.txt` lacks `PS2_GS_HW=1` · BUG-009 no live translation on Switch · BUG-003/004 HW
path lacks lines/points and DATE · BUG-005 depth precision · BUG-007 picture readback through SDL · BUG-010 builds unmeasured ·
BUG-011 `prod.keys` in package. Full register: [BUGS_AND_LIMITATIONS.md](BUGS_AND_LIMITATIONS.md).

## 11. Important technical decisions
Static recompilation (no JIT) · Tekken 4 only · GPU renderer instead of tuning software · Vulkan via NVK (not OpenGL, Deko3D later) ·
WebGPU-on-Vulkan shim to keep the upstream renderer · offline SPIR-V with naga 29 · robust access off / shader clamping · HW rasterizer
with VRAM as the truth and lazy images · overclock 1785/921.6/1600. See [TECHNICAL_DECISIONS.md](TECHNICAL_DECISIONS.md).

## 12. Build instructions (verified)
```bash
python scripts/switch_port.py "C:/ps2-native-recomp-main/games/Tekken-4/disc.iso" --name Tekken-4 --title "Tekken 4" --skip-port --no-disc-copy
```
```bash
printf 'PS2_GS_HW=1\nPS2_PRESENT_LOG=1\n' >> build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/launch.txt
```
Output: `build/switch/Tekken-4/Tekken-4.nro` and `build/switch/Tekken-4/package/switch/` (copy to the SD root, without `prod.keys`).
After a WGSL change, first (in WSL): `cd /mnt/f/ps22switch && python3 scripts/wgsl_spirv.py --naga /root/naga29/bin/naga`.

## 13. Testing instructions
PC (before any Switch build):
```bash
P=/c/ps2-native-recomp-main/tools/portable; export PATH="$P/llvm-mingw/bin:$P/cmake/bin:$P/ninja:$PATH"; ninja -C build/pc-vk ps2run
```
```bash
PS2_GS_HW=1 GS=gpu bash scratch/bench.sh build/pc-vk/ps2run.exe /f/ps22switch/scratch/hwvk12 4200 && cd scratch && python cmp.py hwdec0 hwvk12
```
Switch: copy `.nro` + `launch.txt`, play ~1 minute into a Story Battle fight, read `ps2run.log` (`[waits]`, `[vk-profile]`, `[parts]`).
Details: [TESTING_STATUS.md](TESTING_STATUS.md).

## 14. Immediate next task
**R1:** have the user run test builds A, B, then G, C, D, E, F from `build/switch/tests/` (README there) and return the logs; at the
same time fix BUG-002 (put `PS2_GS_HW=1` into what `switch_port.py` writes) and remove `prod.keys` from the package folder. Then choose
between texture cache (R9), fewer barriers (R10) and direct present (R7) from the numbers. See
[DEVELOPMENT_ROADMAP.md](DEVELOPMENT_ROADMAP.md).

## 15. Critical warnings — do not change without a reason and a PC check
- `-ffp-contract=off`; the 16 MiB stacks; the core placement; the futex code; the heap reserve.
- The shim's barriers/ordering (relaxing them is a roadmap item; do it with pixel checks).
- `spirv_table.cpp` by hand (regenerate instead); naga version (29).
- `.gitignore` rules (no game data, generated code, binaries, keys, saves, screenshots in git).
- The compute renderer: it is the reference for every HW-path comparison.
- `scratch/` reference frame folders (`wgpu`, `hwdec0`, `hwvk11`, ...): deleting them loses the baselines.

## 16. Instructions for the next AI coding agent
1. Read this file, then ARCHITECTURE, BUGS_AND_LIMITATIONS, TECHNICAL_DECISIONS (failed approaches), DEVELOPMENT_ROADMAP.
2. Confirm the environment: `git status` (clean except `handoff/`, `handoff.zip`), `wsl -l -v` (devkita64), `/root/naga29/bin/naga --version`
   (29.0.4), `tools/nvk-sdk/portlibs/switch/lib/libvulkan.a` exists, `C:\ps2-native-recomp-main\games\Tekken-4\disc.iso` exists.
3. Work on branch `claude/project-thread-i8q90o` unless told otherwise; commit with clear messages; do not open a PR unless asked.
4. For each renderer change: PC build → `bench.sh` → `cmp.py` (state the result) → Switch build → add `PS2_GS_HW=1` → hand the user
   the `.nro` + `launch.txt` and ask for the log. Never claim a Switch speed-up without a Switch log.
5. Ask before downloads or installs (the user approved the NVK SDK and naga explicitly). Never read, print, copy or move `prod.keys`
   yourself; ask the user to move it.
6. Write edit scripts with a file editor, not bash heredocs; run WSL commands from PowerShell or a script file.
7. Keep measurements in the docs (`docs/switch-performance-plan.md`) up to date; mark untested work as untested.
