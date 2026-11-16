# File inventory

Snapshot 2026-11-04. The complete file-by-file tree with sizes and tags is **[FILETREE.txt](FILETREE.txt)** (1 992 files, 206
directories, ~4.5 GB, excluding `.git/` and `handoff/`). This file explains every tracked file and every untracked file or file family.

Origin codes: **UP** = upstream ps2-native-recomp, untouched by Switch work · **UP+M** = upstream, modified for the Switch ·
**NEW** = added by the Switch work · **GEN** = generated · **DL** = downloaded · **EXP** = experimental/scratch · **OUT** = build output ·
**USER** = supplied by the user. "Used" = used by the Switch build/run path.

## 1. Tracked files (169, in git at HEAD `8e693c9`)

### Root
| Path | Bytes | Type | Origin | Used | Purpose / notes |
|---|---|---|---|---|---|
| `.gitignore` | 1 194 | text | UP+M | yes | excludes games/, generated/, tools/, build/, build-switch/, scratch/, disc images, binaries, saves, images; +`/build-switch/` |
| `CMakeLists.txt` | 19 189 | CMake | UP+M | yes | targets eerecomp/vurecomp/vujit_dump (host only), `ps2runtime`, `ps2game`, `ps2run`, `ps2web`; Switch: `-ffp-contract=off`, `PS2_VULKAN`/`NVK_SDK`, SPIR-V sha256 guard, NVK link group, SDL2 EGL/glapi swap, NACP/NRO |
| `LICENSE` | 1 119 | text | UP | — | MIT (Samir Nuri; changes under same licence) |
| `README.md` | 14 428 | Markdown | UP+M | doc | Switch quick start + upstream PC README; some overstatements (BUG-019) |
| `Start.cmd` | 693 | batch | UP | no | starts the Windows app (`app/app.ps1`) |
| `Switch_Build.cmd` | 1 459 | batch | NEW | not used | drop an ISO → `scripts\switch_port.py`; uses broken `py` on this PC (BUG-016) |

### `.github/`
| `.github/ISSUE_TEMPLATE/playtest.yml` | 2 289 | YAML | UP | no | upstream playtest issue form |

### `app/` (Windows GUI, upstream, not used for Switch)
| Path | Bytes | Notes |
|---|---|---|
| `app/app.ps1` | 142 026 | PowerShell WinForms app: add game, build, play, extras (pnach), update |
| `app/get-python.ps1` | 1 600 | downloads embeddable Python into tools\portable |
| `app/ui.cs` | 25 242 | GDI+ look of the app |

### `cmake/`
| `cmake/embed.cmake` | 135 | CMake script | UP | yes | embeds a text file (WGSL, headers) as a C string `R"WGSL(...)WGSL"` |

### `docs/`
| `docs/switch-hw-renderer.md` | 4 988 | NEW | doc | HW-rasterizer design + milestones (copy in references/additional_documentation) |
| `docs/switch-performance-plan.md` | 4 163 | NEW | doc | where Tekken 4's time goes on Switch (2026-10-30) + 7 approaches (copy in references) |

### `host/` (the runner program `ps2run`)
| Path | Bytes | Origin | Used | Purpose |
|---|---|---|---|---|
| `host/main.cpp` | 44 618 | UP+M | yes | CLI parsing, log copy, crash handlers; **Switch:** `__libnx_initheap` GPU reserve, `__nx_nv_transfermem_size`, `switch_early_environment` (constructor 101), `switch_arguments` (launch.txt), `switch_error`, chdir + log redirection, `switch_clock_start` |
| `host/window.cpp` | 72 172 | UP+M | yes | SDL2 window/audio/pads, render loop, `show_pixels`, `[waits]` log (+present time), HostThread use, `switch_clock_restore` |
| `host/window.hpp` | 333 | UP | yes | |
| `host/keymap.cpp`, `keymap.hpp` | 18 343 / 3 223 | UP | yes | keyboard/mouse mapping, `keys.ini`, key card |
| `host/stretch.cpp`, `stretch.hpp` | 18 884 / 9 058 | UP | yes | audio time-stretching |
| `host/thread.hpp` | 1 814 | NEW | yes | `HostThread`: 16 MiB pthread stacks on Switch |
| `host/switch_clock.cpp`, `.hpp` | 6 317 / 324 | NEW | yes | clocks via clkrst/pcv from `PS2_CPU_MHZ` etc. |
| `host/profile.cpp`, `profile.hpp` | 3 623 / 124 | UP | PC (Linux x86) | sampling profiler |
| `host/profile_win.cpp` | 10 723 | UP | PC (Windows) | `PS2_PROFILE` sampling profiler |
| `host/pad_test.cpp` | 14 196 | UP | no | `ps2padtest` target (SDL virtual controllers) |
| `host/web.cpp` | 20 897 | UP | no | browser host (Emscripten) |

### `recomp/` (translators, run on the PC; unchanged by Switch work)
| Path | Bytes | Purpose |
|---|---|---|
| `recomp/main.cpp` | 15 484 | `eerecomp` CLI |
| `recomp/r5900.cpp/.hpp` | 9 825 / 4 988 | R5900 instruction decoder |
| `recomp/elf.cpp/.hpp` | 4 906 / 1 214 | ELF loader |
| `recomp/analysis.cpp/.hpp` | 12 610 / 1 424 | function / jump-table discovery |
| `recomp/codegen.cpp/.hpp` | 58 405 / 1 824 | C++ emitter |
| `recomp/eerecomp_gen.cpp/.hpp` | 12 485 / 6 195 | unit/table generation (also linked into PC runtime for live translation) |
| `recomp/snr2.cpp/.hpp` | 5 591 / 995 | SN Systems relocatable-module support (inferred from name; UNKNOWN in detail) |
| `recomp/xff2.hpp` | 10 687 | XFF2 (IOP/EE relocatable) format support (inferred from name) |
| `recomp/vurecomp.cpp`, `vurecomp_gen.cpp/.hpp` | 2 099 / 40 827 / 2 207 | VU1 microprogram → C++ |
| `recomp/vujit_dump.cpp` | 1 298 | dump tool for the VU JIT |

### `runtime/include/ps2/` (public runtime headers; embedded into PC live builds)
| Path | Bytes | Origin | Notes |
|---|---|---|---|
| `cpu.hpp` 2 040, `fpu.hpp` 12 437, `mmi.hpp` 14 510, `memory.hpp` 5 990, `generated.hpp` 3 268, `ee_live.hpp` 3 168, `ee_live_abi.hpp` 6 932, `vu.hpp` 8 728, `vu_live.hpp` 1 930, `disc.hpp` 869, `os.hpp` 1 934, `state.hpp` 5 000, `state_request.hpp` 368, `system.hpp` 9 608, `video.hpp` 483, `hitch.hpp` 3 063 | UP | CPU state, FPU/MMI semantics, memory, generated-code ABI, VU, OS helpers (`os::env`), save states, system API, hitch log |
| `vu_ops.hpp` | 45 686 | UP+M | VU vector ops; Switch: NEON `vminq_u32`/`vmaxvq_u32` paths in `vfix_generic`/`vitof` |
| `part.hpp` | 1 376 | NEW | `PS2_PARTS` scopes/tags |

### `runtime/src/` (emulated PS2)
| Path | Bytes | Origin | Notes |
|---|---|---|---|
| `system.cpp` | 43 660 | UP+M | threads, frame loop; Switch `thread_tune` (svcSetThreadCoreMask, `PS2_SWITCH_CORES`), `part::start` |
| `memory.cpp` 9 551, `dispatch.cpp` 11 752, `kernel.cpp` 35 593 (+`.hpp` 600), `cop.cpp` 6 156, `disc.cpp` 8 125, `vu_micro.cpp` 54 994 (+`.hpp`), `vu_jit.cpp` 63 881 (+`.hpp`), `vu_jit_web.cpp` 3 157 (+`.hpp`), `quick_keys.inc` 53 235 | — | UP | memory map, address resolve, EE kernel HLE, COP0, disc, VU interpreter/JIT, quick-save keys |
| `hw.cpp` 14 741 (+`.hpp`), `dmac.cpp` 20 822 (+`.hpp`), `vif.cpp` 18 577 (+`.hpp`), `ipu/ipu.cpp` 26 252 | — | UP+M | only `PS2_PARTS` scope marks added |
| `ee_live.cpp` 60 872, `vu_live.cpp` 14 865, `live_build.cpp` 19 978, `live_build.hpp` 4 754 | — | UP+M | live translation; **disabled on `__SWITCH__`** |
| `load_stats.hpp` | 1 255 | UP+M | `PS2_IOP_LOAD`; tagged for PS2_PARTS |
| `part.cpp` | 2 207 | NEW | `PS2_PARTS` sampler (256 KB pthread on Switch) |
| `ipu/ipu.hpp` 383, `ipu/tables.hpp` 9 124, `ipu/NOTICE` 1 453 | UP | MPEG tables (from Play!, BSD-2) |
| `iop/*.cpp/.hpp` (cdvd 10 577, cdvdfsv 7 545, cpu 10 065, hw 12 536, io 6 723, kernel 31 652, libs 56 329, sif 45 032, sio2 25 351, spu2 38 920, iop.hpp 2 474 + headers) | UP | IOP interpreter + HLE modules, SPU2, pads/memcard (SIO2), CD |
| `gs/gs.cpp` 69 891, `gs/gs.hpp` 3 534 | UP+M | GS registers/primitives, backend choice (`PS2_GS`) |
| `gs/gif.cpp` 12 157 (+`.hpp`) | UP+M | GIF paths (+PS2_PARTS) |
| `gs/queue.cpp` 10 546 (+`.hpp`) | UP+M | render queue ring; Switch futex |
| `gs/raster.cpp` 22 733 (+`.hpp`) | UP | software reference rasterizer (`--software`) |
| `gs/gpu/gpu.cpp` | 167 960 | UP+M | **GPU GS renderer** (compute + HW path `hw_*`, CLUT owner table, reorder, merge, HW_DECODE) |
| `gs/gpu/gpu.hpp` | 1 750 | UP | renderer interface |
| `gs/gpu/gs.wgsl` | 38 209 | UP+M | compute shaders: upload, copy_read/write, load_clut (parallel + owner table), display, decode, snapshot, resolve, raster |
| `gs/gpu/hw.wgsl` | 20 690 | NEW | HW path: hw_load_color, hw_writeback, hw_full_vs, hw_load_depth, hw_vs, hw_fs (dual-source), hw_fs_single |
| `gs/gpu/blit.wgsl` | 10 107 | UP | present/scale shader (PC surfaces) |
| `gs/gpu/vk/wgpu_vk.cpp` | 85 414 | NEW | **WebGPU-on-Vulkan shim** |
| `gs/gpu/vk/spirv.hpp` | 713 | NEW | SPIR-V table interface |
| `gs/gpu/vk/spirv_table.cpp` | 1 666 282 | NEW, GEN (by `scripts/wgsl_spirv.py`, naga 29) | SPIR-V words per shader variant + `// gs.wgsl sha256 ...` / `// hw.wgsl sha256 ...` lines. Never edit by hand |
| `gs/gpu/vk/include/webgpu/webgpu.h`, `wgpu.h` | 266 335 / 57 477 | NEW (vendored wgpu-native v29 headers, BSD-3) | API declarations for the shim |

### `scripts/`
| Path | Bytes | Origin | Used for Switch | Purpose |
|---|---|---|---|---|
| `switch_port.py` | 14 807 | NEW | **yes** | Switch build + package |
| `wgsl_spirv.py` | 5 510 | NEW | **yes** (shader changes) | WGSL → SPIR-V table |
| `port.sh` | 10 918 | UP | yes (indirect) | full pipeline per game |
| `setup.sh` 1 914, `cxx.sh` 5 734, `runtime.sh` 2 607, `bootstrap.ps1` 3 302, `portable.ps1` 19 512, `pack_compiler.ps1` 8 061 | UP | PC setup | toolchain setup / shared runtime builds |
| `fetch_wgpu.sh` 888, `fetch_sdl2.sh` 795, `fetch_dxc.sh` 2 100 | UP | PC | download third-party libs into `third_party/` |
| `disc.py` 6 338, `mkiso.py` 3 744, `windows.py` 8 840, `ps2tools.py` 9 752, `pnach.py` 34 233, `mcformat.py` 29 202 | UP | port.sh helpers | disc extraction, ISO, code windows, shared helpers, PCSX2 patches, memory cards |
| `autotests.py` 20 279, `test_memcard.py` 21 662, `test_vu_codegen.py` 36 992 | UP | no | tests |
| `speedcheck.py` 51 404, `webspeed.mjs` 14 288, `report.py` 56 667, `replay.py` 11 672, `play.py` 10 051, `hitches.py` 11 492, `soundcheck.py` 4 692, `wavcheck.py` 17 821, `gpu_profile.py` 1 767, `gt4_core.py` 3 074, `gow2_health.py` 1 254, `serve.py` 7 908, `build_web.sh` 1 758 | UP | no | PC/web measurement & debugging tools |

### `tests/` (upstream; not run in Switch work)
| `tests/analysis_tailcall.py` 2 057, `codegen_fallthrough.py` 1 844, `elf_bss.py` 1 510, `fpu_half_damage.py` 3 438, `stretch_test.cpp` 28 118 | UP | recompiler regressions, FPU half-float test, audio stretch test |

### `web/`
| `web/shell.html` | 54 022 | UP | no | Emscripten page for the browser build |

## 2. Untracked / ignored content (by pattern)

### `generated/` — GEN, 114 files, ~29 MB (never commit: translation of game code)
| Pattern | Count | Notes |
|---|---|---|
| `generated/Tekken-4/unit_0000.cpp … unit_004A.cpp` | 75 | EE units (`gen::unit_XXXX`), dated 2026-10-29 17:09 (same as the C: upstream copy → copied from there) |
| `generated/Tekken-4/table.cpp`, `units.hpp` | 2 | dispatch table / declarations |
| `generated/Tekken-4/vu1_000.cpp … vu1_002.cpp`, `vu1_table.cpp` | 4 | 13 VU1 programs as C++ (2026-10-29 22:53) |
| `generated/Tekken-5/m_004A8000_7382A761_unit_0000.cpp …` | 33 | Tekken 5 overlay translation (upstream); unused (BUG-021) |

### `games/Tekken-4/` — game data (USER-derived; never commit), 18 files
| File | Notes |
|---|---|
| `disc/SLUS_203.28` (1 956 544 B) | boot ELF extracted from the disc |
| `disc/SYSTEM.CNF` (57 B) | `BOOT2 = cdrom0:\SLUS_203.28;1`, `VER = 1.00`, `VMODE = NTSC` |
| `vu1/<hash>_<addr>.bin` ×13 (16 384 B each) | VU1 microprogram dumps recorded by exploring runs (input of vurecomp) |
| `keys.ini` (3 095 B) | key mapping (copied into the package) |
| `app.json` (137 B) | `{"source": "F:\\ps22switch\\Tekken 4 (USA).iso", "name": "Tekken-4", "elf": "disc/SLUS_203.28", "iso": "disc.iso"}` — the source ISO path does **not** exist on F: (the disc used is `C:\ps2-native-recomp-main\games\Tekken-4\disc.iso`) |
| `entries.txt` (0 B) | extra entry points (empty) |
| missing but expected by port.sh: `disc.iso` | not present here (only in C: and in the package folder) |

### `build/` — OUT, 546 files, ~3.2 GB
| Path | Notes |
|---|---|
| `build/tools/` (27 files) | `eerecomp.exe` (2.4 MB), `vurecomp.exe` (2.1 MB), CMake/Ninja state (CMake 4.4.4, llvm-mingw) |
| `build/pc-t4/` (156 files, 61 MB) | 2026-10-29 PC build (no GPU lib found → software); `ps2run.exe` 17.7 MB, `libps2game.a`, `libps2runtime.a`, embedded headers |
| `build/pc-wgpu/` (164 files, 76 MB) | PC build with wgpu-native from C:; reference renderer; `ps2run.exe` 30.9 MB (2026-10-30 11:14) |
| `build/pc-vk/` (166 files, 64 MB) | PC build with the Vulkan shim; `ps2run.exe` 18.6 MB (2026-10-30 14:56) |
| `build/switch/Tekken-4/Tekken-4.nro` (37 087 429 B), `Tekken-4.elf` (137 955 144 B, debug info) | Switch build of `8e693c9`, 2026-10-30 14:56 |
| `build/switch/Tekken-4/Tekken 4.nsp` (181 472 B) | forwarder NSP, untested |
| `build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/` | SD folder: `Tekken-4.nro`, `launch.txt` (no `PS2_GS_HW`, BUG-002), `app.json`, `keys.ini`, `disc/SLUS_203.28`, `disc/SYSTEM.CNF`, **`disc.iso` (2 698 346 496 B, the user's disc image)**, `cover.jpg` (63 146 B, user's box art for the forwarder), **`prod.keys` (14 612 B, SENSITIVE console keys, BUG-011)**, `ps2run.log` (latest Switch log, copied back by the user), `vk_raster.txt` (1.29 MB), `vk_resolve.txt`, `vk_snapshot.txt` (Switch `PS2_VK_STATS` output) |
| `build/switch/tests/{A-fast,B-profile,C-cores,D-inflight3,E-noreorder,F-nomerge,G-decode,G-decode-profile}/` | each `Tekken-4.nro` + `launch.txt`; `README.txt` explains; never run |

### `build-switch/` — stale OUT
| `CMakeCache.txt`, `CMakeFiles/` (2026-10-29 22:40) | abandoned configure with `CMAKE_TOOLCHAIN_FILE=/cmake/Switch.cmake`; unused (BUG-018) |

### `tools/` — DL / external dependencies, 114 files, ~287 MB
| Path | Notes |
|---|---|
| `tools/nvk-sdk/mesa-switch-nvk-sdk.zip` | 52 797 811 B download |
| `tools/nvk-sdk/portlibs/switch/include/{vulkan,vk_video,EGL,GL,GLES,GLES2,GLES3,KHR}/` | Vulkan/EGL/GL headers (PC builds also use the Vulkan headers) |
| `tools/nvk-sdk/portlibs/switch/lib/*.a` (28 libs) | `libvulkan.a` 85.7 MB, `libEGL.a` 42.7 MB, `libnak_rs.a`, `libnil.a`, `libnir.a`, `libnvk.a`, `libnouveau_*`, `libmesa_util*`, `libzstd.a`, `libexpat.a`, `libz.a`, `libglapi.a`, `libGL*.a`, `libvtn.a`, `libcompiler*.a`, `libblake3.a`, `libxmlconfig.a`, ... |
| `tools/nvk-sdk/portlibs/switch/lib/{cmake,pkgconfig}/`, `share/{drirc.d,licenses,vulkan/icd.d}/`, `MESA_REVISION` | config, licences, Mesa revision |
| `tools/forwarder/src/` (`source/main.c` 18 315 B, `source/trampoline.s`, `Makefile`, `forwarder.json`, `LICENSE.md`) | loader source from OpenTrickyNX (`DEFAULT_NRO` changed) |
| `tools/forwarder/forwarder.nso` (39 844 B), `forwarder.npdm` (1 016 B), `make_forwarder.py` (15 933 B, GPL-3.0) | built loader + packer |

### `scratch/` — EXP, 1 028 files, ~1 GB (PC test data; keep: it holds the reference frames)
| Pattern | Notes |
|---|---|
| `scratch/bench.sh`, `cmp.py` | test harness (copies in references) |
| `scratch/<run>/frame_NNNNN.ppm` (959 files) + `log.txt` + `card.ps2` per run folder | runs: `base` (software), `wgpu`, `wgpu2`, `wgpu_c` (wgpu-native compute), `vk`, `vk2`, `vk8` (shim compute), `hw`, `hw2`, `hw5` (HW path on wgpu-native), `hwvk`, `hwvk2`…`hwvk11`, `hwvk6p` (HW path through the shim, one per optimisation step), `hwdec0`/`hwdec1` (`PS2_HW_DECODE` off/on), `prof` (`p.txt` profile) |
| `scratch/vk_<program>.txt` ×9 (2026-10-30 10:09) | PC (AMD) `PS2_VK_STATS` dumps |
| `scratch/wprof.txt` (844 KB), `p6.txt` | CPU profile (software renderer) / GPU dispatch log |
| `scratch/long1.txt`, `long2.txt`, `sw.log` | timing of 4 200-frame runs; tail of the last Switch build |
| `scratch/contact.png`, `diff.png` (2026-10-30 10:46) | visual comparison sheets of HW vs compute frames |
| `scratch/*/card.ps2` (25) | memory card copies used by bench runs (contain game saves; never commit) |

### `scripts/__pycache__/` — Python bytecode (OUT), ignored.

## 3. Outside the project folder (referenced)
| Path | Role |
|---|---|
| `C:\ps2-native-recomp-main\` | upstream copy (not git; updated 2026-11-02); `tools\portable\` PC toolchain; `third_party\{wgpu-native,sdl2,dxc}`; `games\Tekken-4\disc.iso` (the disc), `states\run_2026-10-29_22-54-13_f000000.inputs(.ps2)` (bench inputs/card); `generated\Tekken-4` (source of F:'s copy) |
| WSL `devkita64`: `/opt/devkitpro`, `/usr/local/bin/switch-{cmake,ninja}`, `/root/naga29/bin/naga`, `/root/.cargo/bin/naga`, `/tmp/switch-build-tekken-4/` (build dir), `/tmp/fw/` (forwarder build), `/tmp/table30.cpp` (spirv table made with naga 30, backup) | Switch toolchain |
| `C:\Users\Christopher Schimpf\.claude\projects\F--ps22switch\` | Claude Code transcripts and project memory (local only) |

## 4. Missing but expected
- `third_party/` in F: (referenced by `PS2_WGPU_DIR` default and two build caches).
- `docs/*.md` referenced from code comments (BUG-019).
- `games/Tekken-4/disc.iso` (port.sh default location) — disc is on C: and in the package.
- `F:\ps22switch\Tekken 4 (USA).iso` (named in `app.json`).

## 5. Large binaries (≥ 10 MB)
`disc.iso` 2.5 GB (package) · `Tekken-4.elf` 131.6 MB · 9× `Tekken-4.nro` 35.4 MB · NVK libs (`libvulkan.a` 85.7 MB, `libEGL.a` 42.7 MB,
`libnak_rs.a` 30.4 MB, `libnil.a` 23.9 MB, `libzstd.a` 12.7 MB, `libnir.a` 11.1 MB) · `mesa-switch-nvk-sdk.zip` 50.4 MB · PC `ps2run.exe`
16.9–30.9 MB · `libps2game.a` 16.5 MB ×3. See FILETREE.txt tag `LARGE`.

Counts: 169 tracked files described individually or in grouped rows; 1 823 untracked files described by family (FILETREE.txt lists
each). Total inventoried: **1 992 files**.
