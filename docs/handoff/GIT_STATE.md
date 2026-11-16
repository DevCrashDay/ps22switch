# Git state (snapshot 2026-11-04, before the handoff folder was added)

Nothing in git was changed for this handoff: no commit, stash, reset, checkout or push. The only new things in the working tree are
`handoff/` and `handoff.zip` (untracked).

## Repository

| Item | Value |
|---|---|
| Path | `F:\ps22switch` |
| Remote `origin` | `https://github.com/DevCrashDay/ps22switch` (fetch and push) |
| Current branch | `claude/project-thread-i8q90o` (tracks `origin/claude/project-thread-i8q90o`, in sync) |
| HEAD | `8e693c9394b08912b30220ebc1f2450361c40fc5` — "Hardware path: PS2_HW_DECODE pre-decodes textures once per submit (off by default, untested)", xcrashdayx, 2026-10-30 11:34:34 +0100 |
| Local `main` | `f89a7c66c08149e2db1d9c57d71acf8252c1a935` — **1 commit ahead of `origin/main`, never pushed** |
| `origin/main` | `ba3d4712a9291abe1767c8aa0aa8dc4eeda1db71` (upstream "Show the key card on a thread of its own on Linux") |
| Tags | none |
| Stashes | none |
| Commits on HEAD | 60 (30 by xcrashdayx = the Switch work, 27 exoteric33 + 3 exoteric = upstream) |
| Configured identity | `xcrashdayx` (email removed) |
| Uncommitted modifications to tracked files | **none** (`git status` clean before `handoff/` was created) |
| Untracked (not ignored) | none before the handoff; now `handoff/` (`handoff.zip` is covered by the `*.zip` ignore rule) |
| Ignored (present on disk) | `build/`, `build-switch/`, `games/`, `generated/`, `scratch/`, `scripts/__pycache__/`, `tools/` |

## Branch topology

```
* 8e693c9 (HEAD, origin/claude/project-thread-i8q90o)  ... 29 Switch commits ...
* 601f70c Switch port: fix thread stacks, cores, wake-ups, FMA, packaging and launch
*   e1b87f6 Merge upstream exoteric33/ps2-native-recomp main (8 commits)
|\
| * 23f6332 ... 3dc39c4   (upstream, 8 commits)
* | f89a7c6 (main)  Implement Nintendo Switch (Switch 1) recompilation pipeline and runtime port
|/
* ba3d471 (origin/main) ... 0660c5e Import ps2-native-recomp
```

## Important commits (Switch work, oldest first)

| Commit | Essential / experimental | Summary |
|---|---|---|
| `f89a7c6` | essential (base) | first Switch pipeline (switch_port.py, NRO, Switch host code, NEON in vu_ops, live builds off) |
| `e1b87f6` | essential | merge of upstream `23f6332` |
| `601f70c` | essential | 16 MiB stacks, cores, futex, `-ffp-contract=off`, `launch.txt`, port.sh in switch_port |
| `584dd8e` | essential | WebGPU-on-Vulkan shim, naga SPIR-V, NVK linking |
| `25fb2ba` | essential | shared staging ring (fixed OOM) |
| `f42606b` | essential | in-flight cap 8 (fixed device lost), `PS2_VK_PROFILE`, env lines in launch.txt, naga 29 |
| `518f6b6` | neutral | memory-type choice (no effect on Switch), tile variants |
| `9709c64`, `6dcfc5d` | diagnostic | `PS2_PARTS` (+ crash fix) |
| `1549962` | diagnostic | stdout/fd redirect (from the failed NAK_DEBUG attempt; harmless) |
| `95decfe` | diagnostic | `PS2_VK_STATS` |
| `526bf32` | essential | robust access off, shader clamping |
| `8d15359` | essential | overclock from launch.txt |
| `c191089` | doc | HW renderer design |
| `6210d1a`, `4d0bf6c` | essential | HW rasterizer path + shim graphics |
| `ece356f` | essential | GPU heap reserve / transfer memory |
| `ce560bf` | essential (with `d9b9eef`) | no decode on HW path; parallel CLUT (regressed) |
| `d9b9eef` | essential | CLUT owner table (fixed regression) |
| `8b45d6f`, `28bab2c` | essential (measured) | flat setup interpolants; lazy images |
| `9475d4f`, `bd001fd`, `1103fd1`, `a1c327d`, `aaed033` | **unmeasured on Switch** | read-only VRAM, no CPU binning, reorder, pass merge, FIX split |
| `b6e27c3`, `5307556` | diagnostic | `PS2_SWITCH_CORES`, present timing |
| `569b216` | doc | performance plan |
| `8e693c9` | **experimental**, off by default | `PS2_HW_DECODE` |

## Last known working commit
- **Measured on hardware:** ≈ `28bab2c` (the last on-disk Switch log; reconstructed from timestamps — the agent built and handed out a
  package at ≈10:13Z right after `28bab2c`, and the user's log arrived 10:19Z without the `5307556` log field).
- **Builds and runs on PC (pixel-checked):** `8e693c9` (HEAD).
- **Built for Switch:** `8e693c9` (`build/switch/Tekken-4/Tekken-4.nro`, 2026-10-30 14:56; `/tmp/switch-build-tekken-4/ps2run.nro` same size).

## Diffs preserved in this package
`references/important_diffs/`:
- `switch_port_all_changes_vs_upstream_23f6332.diff` — every Switch change vs upstream, excluding the generated `spirv_table.cpp` and the
  vendored wgpu headers (256 KB).
- `switch_port_diffstat.txt` — `git diff --stat 23f6332 HEAD` (37 files, +24 186 / −92).
- `f89a7c6_initial_switch_pipeline_vs_import.diff` — `git diff 0660c5e f89a7c6` (includes upstream changes between them).
- `wgpu_vk_shim.diff`, `gs_gpu_renderer_and_hw_path.diff`, `host_switch_changes.diff` — focused diffs.
- `git_log_full_with_stat.txt` — full log with messages and file stats.

## Files changed by the Switch work (vs upstream `23f6332`)
New: `Switch_Build.cmd`, `docs/switch-hw-renderer.md`, `docs/switch-performance-plan.md`, `host/switch_clock.cpp`, `host/switch_clock.hpp`,
`host/thread.hpp`, `runtime/include/ps2/part.hpp`, `runtime/src/part.cpp`, `runtime/src/gs/gpu/hw.wgsl`, `runtime/src/gs/gpu/vk/*`
(`wgpu_vk.cpp`, `spirv.hpp`, `spirv_table.cpp`, `include/webgpu/webgpu.h`, `include/webgpu/wgpu.h`), `scripts/switch_port.py`,
`scripts/wgsl_spirv.py`.
Modified: `.gitignore`, `CMakeLists.txt`, `README.md`, `host/main.cpp`, `host/window.cpp`, `runtime/include/ps2/vu_ops.hpp`,
`runtime/src/dmac.cpp`, `ee_live.cpp`, `gs/gif.cpp`, `gs/gpu/gpu.cpp`, `gs/gpu/gs.wgsl`, `gs/gs.cpp`, `gs/queue.cpp`, `hw.cpp`,
`ipu/ipu.cpp`, `live_build.cpp`, `live_build.hpp`, `load_stats.hpp`, `system.cpp`, `vif.cpp`, `vu_live.cpp`.

## Open git questions for the user
1. Push local `main` (`f89a7c6`) to `origin/main`? (It is the base of the work branch.)
2. Open a PR / merge `claude/project-thread-i8q90o` into `main`? No PR exists; none was requested.
