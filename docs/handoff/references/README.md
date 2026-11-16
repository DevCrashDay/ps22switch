# references/

Supporting material copied (not moved) from the project on 2026-11-04. The project folder stays the authoritative source.

## important_logs/
| File | Source | Why it matters |
|---|---|---|
| `switch_ps2run_2026-10-30_hw-path_fight.log` | `build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/ps2run.log` | the only Switch log on disk: HW path, overclock, menus 60 / fights ~22–27 fps, GPU profile and emulation breakdown |
| `switch_build_tail_sw.log` | `scratch/sw.log` | tail of the last Switch build (14:56, commit `8e693c9`) |
| `pc_software_renderer_base_log.txt` | `scratch/base/log.txt` | software-renderer run of the scripted sequence (baseline log format) |
| `pc_hw_path_decode_off_hwdec0_log.txt`, `pc_hw_path_decode_on_hwdec1_log.txt` | `scratch/hwdec0|1/log.txt` | latest PC HW-path runs with GPU profile (RX 6800 XT) |
| `pc_bench_long1_wgpu.txt`, `pc_bench_long2_vk.txt` | `scratch/long1|2.txt` | 4 200-frame timings, wgpu-native vs shim |
| `pc_gpu_profile_p6_head.txt` | first 60 lines of `scratch/p6.txt` | per-dispatch GPU log format |
| `pc_cpu_profile_wprof_head.txt` | first 80 lines of `scratch/wprof.txt` | CPU profile of the software-renderer era |

## important_diffs/
See GIT_STATE.md. `switch_port_all_changes_vs_upstream_23f6332.diff` is the complete Switch work minus generated SPIR-V and vendored headers.

## additional_documentation/
Copies of `docs/switch-hw-renderer.md`, `docs/switch-performance-plan.md`, `scratch/bench.sh`, `scratch/cmp.py`, the package
`launch.txt` and `app.json`, `games/Tekken-4/keys.ini` (as `keys.ini.example`), and `switch_test_builds/` (README + every test
`launch.txt`).
