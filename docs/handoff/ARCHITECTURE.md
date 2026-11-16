# Architecture

## 1. What the system is

ps22switch is a **static recompiler + emulated-hardware runtime**. It never emulates the PS2's main CPU instruction by instruction
at run time (when all code was found ahead of time); instead it translates the game's MIPS R5900 (Emotion Engine, "EE") machine
code into C++ once, on a PC, and compiles that C++ for the target (Windows/Linux/macOS/web upstream; **Nintendo Switch 1 /
AArch64 here**). Everything else of the PS2 (GS graphics chip, VU0/VU1 vector units, VIF/GIF/DMA, IOP sound/IO processor, SPU2,
IPU MPEG decoder, CD/DVD, memory card, pads) is emulated by `runtime/`, a C++20 static library.

```
                 ON THE PC (Windows, Git Bash + WSL)                                   ON THE SWITCH
 ┌──────────────────────────────────────────────────────────────┐        ┌─────────────────────────────────────────────┐
 │ disc.iso ──► scripts/port.sh --native-only                    │        │ sdmc:/switch/ps22switch/Tekken-4/           │
 │   1. extract SYSTEM.CNF + boot ELF (SLUS_203.28) → games/<n>/ │        │   Tekken-4.nro  (ps2run + game + runtime)   │
 │   2. eerecomp: ELF → generated/<n>/unit_XXXX.cpp, table.cpp   │        │   launch.txt    (argv lines + NAME=value)   │
 │   3. exploring runs (ps2run headless) find missed code        │        │   disc/SLUS_203.28, disc.iso, keys.ini      │
 │   4. vurecomp: recorded VU1 programs → vu1_NNN.cpp            │        │   Tekken-4.ps2 (memory card, created)       │
 │                                                               │        │   ps2run.log / ps2run.stdout.log            │
 │ scripts/switch_port.py                                        │  copy  │                                             │
 │   5. WSL devkitA64: CMake(Switch.cmake)+Ninja → ps2run.elf    │ ─────► │ hbmenu (title takeover) or forwarder NSP    │
 │      → nx_create_nro → build/switch/<n>/<n>.nro               │        │   → main() in host/main.cpp                 │
 │   6. package → build/switch/<n>/package/switch/ps22switch/<n> │        │                                             │
 └──────────────────────────────────────────────────────────────┘        └─────────────────────────────────────────────┘
```

## 2. Components and files

| Component | Files | Role |
|---|---|---|
| EE recompiler | `recomp/main.cpp`, `r5900.*` (decoder), `elf.*`, `analysis.*` (function/jump-table discovery), `codegen.*` (C++ emitter), `eerecomp_gen.*`, `snr2.*`, `xff2.hpp` | `eerecomp` tool: ELF → C++ units + dispatch table |
| VU recompiler | `recomp/vurecomp.cpp`, `vurecomp_gen.*`, `vujit_dump.cpp` | VU1 microprograms → C++ (`vu1_*.cpp`) |
| Generated game code | `generated/Tekken-4/unit_0000.cpp … unit_004A.cpp` (75 units), `table.cpp`, `units.hpp`, `vu1_000..002.cpp`, `vu1_table.cpp` | functions `gen::unit_XXXX(ps2::Cpu&, ...)`, registered with `register_generated_code()` / `register_generated_vu()` |
| Runtime core | `runtime/src/system.cpp` (threads, frame loop, `thread_tune`), `memory.cpp`, `dispatch.cpp` (`resolve`), `kernel.cpp` (EE BIOS/syscalls HLE), `cop.cpp`, `hw.cpp` (EE hardware registers, timers), `dmac.cpp`, `vif.cpp`, `vu_micro.cpp` (VU interpreter), `vu_jit*.cpp`, `part.cpp` (PS2_PARTS) | the emulated PS2 |
| Live translation | `ee_live.cpp`, `vu_live.cpp`, `live_build.*` | On PC: compile unknown code at run time with the host compiler. **Stubbed on Switch** (`#if defined(__EMSCRIPTEN__) || defined(__SWITCH__)`) |
| IOP | `runtime/src/iop/*.cpp` (cpu interpreter, kernel, libs HLE, sif, sio2 pads/memcard, spu2, cdvd, cdvdfsv, io) | sound, pads, memory card, disc |
| IPU | `runtime/src/ipu/ipu.cpp`, `tables.hpp` | MPEG-2 video (FMVs), runs on the emulation thread |
| GS front end | `runtime/src/gs/gs.cpp` (registers, primitives, `PS2_GS` backend choice), `gif.cpp`, `queue.cpp` (ring between emulation/VU and window threads; futex), `raster.cpp` (software reference renderer) | |
| GPU GS renderer | `runtime/src/gs/gpu/gpu.cpp` (3 300 lines; batches, compute dispatches, HW path), `gs.wgsl` (compute renderer), `hw.wgsl` (hardware-rasterizer path), `blit.wgsl` | Written against the **WebGPU C API** |
| WebGPU-on-Vulkan shim | `runtime/src/gs/gpu/vk/wgpu_vk.cpp`, `spirv.hpp`, `spirv_table.cpp` (generated SPIR-V), `include/webgpu/{webgpu.h,wgpu.h}` (vendored wgpu-native v29 headers) | Lets `gpu.cpp` run unchanged on NVK |
| Host | `host/main.cpp` (argument parsing, Switch launch/heap/logging), `window.cpp` (SDL2 window, audio pacing, present, `[waits]`), `keymap.*`, `stretch.*` (audio time-stretch), `thread.hpp` (HostThread), `switch_clock.*` (overclock), `profile*.cpp` (PC only), `web.cpp` (browser) | |
| Build | `CMakeLists.txt`, `cmake/embed.cmake`, `scripts/switch_port.py`, `scripts/wgsl_spirv.py`, `scripts/port.sh`, `Switch_Build.cmd` | |

## 3. Execution flow on the Switch

1. **Static init** — `__libnx_initheap` (host/main.cpp ~line 495): heap = all memory minus 2 MB, then minus a **512 MB** (title
   takeover) / **128 MB** (album) GPU reserve left unmapped for NVK's nvmap allocations; `__nx_nv_transfermem_size = 16 MB`.
   A `constructor(101)` (`switch_early_environment`) reads `launch.txt` via the weak `__system_argv` so `NAME=value` lines are
   environment variables before other static objects initialise.
2. **`main()`** — `romfsInit`; `chdir` to the .nro folder (argv[0]); `stderr` → `ps2run.log`, `stdout` → `ps2run.stdout.log`,
   `dup2` onto fds 1/2; if no CLI args, `switch_arguments()` reads `launch.txt` (`./` or `romfs:/`) — non-`NAME=value`, non-`#` lines
   are argv; if nothing is found `switch_error()` shows a console screen. `switch_clock_start()` applies `PS2_CPU_MHZ`/`GPU`/`MEM`.
   Forced `window = fullscreen = true`.
3. **Runtime start** — `run_window()` (host/window.cpp): `ps2::init_graphics()` → `gs::gpu::init()` (Vulkan device via shim; logs
   `[gpu] NVIDIA Tegra X1 (GM20B) (NVK GM20B) ... on Vulkan`, `[gpu] 9 shaders compiled`, and with `PS2_GS_HW=1` `hw_init()`).
   Starts the VU thread (`HostThread`) and the emulation thread (`HostThread`, 16 MiB stack). The calling thread is the
   **window thread**.
4. **Emulation thread** (core 1): runs translated EE code (`gen::unit_*`), dispatches unknown addresses via `resolve`, services
   hardware events, IOP (interpreted), SPU2, IPU, VIF0/VU0, DMA, GIF; pushes GS work into the queue ring; paced by audio.
5. **VU thread** (core 2): VIF1 + VU1 programs (precompiled C++ or interpreter), emits GS primitives.
6. **Window thread** (core 0): SDL events/pads, takes queued GS work and calls `gpu.cpp` (records and submits Vulkan work), reads
   back the displayed picture (`display_async`) and shows it via SDL's renderer (`show_pixels`: texture upload + `SDL_RenderPresent`).
7. **Exit** — SDL_QUIT (HOME/close): emulation joined, memory card flushed (`atexit`), `switch_clock_restore()`, `_Exit`.

### Threads and concurrency

| Thread | Core (default) | Override | Stack | Hand-off |
|---|---|---|---|---|
| Emulation | 1 | `PS2_SWITCH_CORES=e,v,w` | 16 MiB (`host/thread.hpp`) | GS queue ring (`gs/queue.cpp`), futex = `svcWaitForAddress`/`svcSignalToAddress` |
| VU | 2 | same | 16 MiB | VIF1 work queue, GS queue |
| Window / GPU submit | 0 | same | process main | waits on render work (`wait_render_work`) |
| Clock keeper | — | — | libnx thread | re-applies clocks |
| PS2_PARTS sampler (optional) | — | — | 256 KB pthread | samples a tag every 250 µs |

Horizon never migrates threads; without `svcSetThreadCoreMask` all would share core 0 (fixed in `601f70c`). The CPU may run at most
`PS2_VK_IN_FLIGHT` (default 8) GPU submissions ahead.

## 4. The GS renderer on the Switch (critical path)

```
 emulation/VU thread                   window thread                                   GPU (NVK, Tegra X1)
 ───────────────────                   ─────────────                                   ───────────────────
 GIF packets ─► gs.cpp (State,Vertex) ─► queue ring ─► gpu::draw / image_data / load_clut / local_copy
                                                      │  batches primitives (Batch: prims 28 words, setups 40 words)
                                                      ├─ flush_batch():
                                                      │    compute path: CPU tile binning → raster dispatch (gs.wgsl)
                                                      │    HW path (PS2_GS_HW=1): hw_emit runs → render pass (hw.wgsl)
                                                      ├─ emit_pre(): uploads / CLUT loads / copies (compute, gs.wgsl)
                                                      └─ submit() ─► wgpu* calls ─► wgpu_vk.cpp ─► vkQueueSubmit
                                                         display_async() readback ─► SDL texture ─► SDL_RenderPresent
```

- **VRAM model:** the 4 MB PS2 video memory is a GPU storage buffer, swizzled exactly as on the PS2 (swizzle tables in `gs.wgsl`).
- **Compute path** (`gs.wgsl` `raster`): one workgroup per 8x8 tile walks every primitive touching the tile; bit-exact; reference
  renderer; on Tegra uses 104 registers/thread → 16 warps; ~7–9 fps in fights.
- **Hardware path** (`hw.wgsl`, `gpu.cpp` `hw_*`, env `PS2_GS_HW=1`):
  - A 2048x2048 RGBA8 colour image and D32F depth image hold the current target (`kHwSize = 2048`, 1x scale).
  - Each shape = 6 vertices pulled in `hw_vs` from the prim records; sprites → 2 triangles; **lines/points produce no geometry
    ("to come")**.
  - `hw_fs` decodes textures from VRAM (`vram_ro`, binding 5, read-only; all PSMs, CLUT, bilinear, wrap modes), TFX, fog,
    alpha test (discard), outputs dual-source colour for the PS2 blend; FIX via blend constant.
  - Pipelines cached by key = depth mode + blend<<4 (`hw_pipeline`), FIX only splits runs when the blend uses it (`aaed033`).
  - **Lazy images** (`28bab2c`): images keep the last target; written back (`hw_writeback`, compute) only when VRAM is read
    (texture over it, CLUT load, display, download, copy, save state) and reloaded (`hw_load_color`, `hw_load_depth`) only when VRAM
    changed underneath. Depth stays float between batches (small differences vs the compute reference).
  - Render passes with only CLUT loads between them merge (`a1c327d`, `PS2_HW_MERGE=0` off).
  - Optional `PS2_HW_DECODE=1`: textures pre-decoded per submit into a buffer (experimental).
- **CLUT loads:** `load_clut` compute, 2 workgroups per load, each entry looks up its source load in a CPU-built owner table
  (`d9b9eef`).
- **Uploads before CLUT loads** when no dependency (`1103fd1`, `PS2_PRE_REORDER=0` off).

### WebGPU-on-Vulkan shim (`runtime/src/gs/gpu/vk/wgpu_vk.cpp`)

- Implements ~66 `wgpu*` entry points: compute pipelines, storage/uniform buffers with dynamic offsets, queue writes (staging ring
  shared by all submissions, `25fb2ba`), copies, map-for-read (timeline semaphore), device poll, 2D textures in `VK_IMAGE_LAYOUT_GENERAL`,
  views, render pipelines (dual-source blend, dynamic viewport/scissor/blend constant), render passes via Vulkan 1.3 dynamic rendering.
- Ordering: a full memory barrier after every dispatch/copy and at the start of each command buffer (WebGPU semantics). On Tegra
  each barrier costs tens of µs — a known cost (~40 % of GPU time not covered by timestamps).
- Not implemented: surfaces (no swapchain), WebGPU timestamp queries (the shim times dispatches itself when `PS2_VK_PROFILE=1`).
- Device features: timeline semaphores, dynamic rendering, dual-source blending; `robustBufferAccess` **off** unless
  `PS2_VK_ROBUST=1`. Memory type: fewest extra flags (`518f6b6`). Off-Switch it loads the system Vulkan loader dynamically
  (`VK_NO_PROTOTYPES`), on Switch NVK is linked statically (`-Wl,-u,vk_icdGetInstanceProcAddr`).
- WGSL is not compiled at run time: `spirv_table.cpp` holds SPIR-V per variant (gs.wgsl: TILE 4/8/16 × explicit-FMA on/off;
  hw.wgsl: tile=0 "any"); WGSL text is matched by hash. CMake refuses to configure if the sha256 of `gs.wgsl`/`hw.wgsl` does not
  match the `// gs.wgsl sha256` / `// hw.wgsl sha256` lines in `spirv_table.cpp`.

## 5. Memory

- Newlib heap = process memory − 2 MB − GPU reserve (512/128 MB). PS2 RAM (32 MB EE + IOP 2 MB + scratch etc.) lives in the heap.
- GPU: NVK allocates through nvmap from the reserved region; staging blocks of 16 MiB in a ring; VRAM buffer 4 MB; HW images
  2048² RGBA8 + D32F (≈32 MB).
- Switch memory types reported by NVK: type 0 `device-local host-visible cached`, type 1 `device-local host-visible coherent`
  (heap 2391 MB).

## 6. External interfaces

| Interface | Used for |
|---|---|
| libnx svc: `svcSetThreadCoreMask`, `svcWaitForAddress`, `svcSignalToAddress`, `svcGetInfo`, `svcSetHeapSize` | cores, futex, heap |
| libnx services: clkrst (≥8.0.0) / pcv: `PcvModule_CpuBus`, `PcvModule_GPU`, `PcvModule_EMC` | clocks (caps 1785 MHz, 921.6 MHz, 1600 MHz) |
| SDL2 (libnx port): video (GL renderer via EGL from NVK SDK), audio 48 kHz stereo, GameController | window, sound, pads |
| Vulkan 1.3/1.4 (NVK) + `VK_KHR_pipeline_executable_properties` | rendering, shader stats |
| SD card files (see `launch.txt` format in BUILD_AND_SETUP.md) | program, disc image (read while running), card, logs |

## 7. Data that must stay consistent

- `gs.wgsl`/`hw.wgsl` ↔ `spirv_table.cpp` (regenerate with naga 29 in WSL).
- `generated/<game>` ↔ the boot ELF it was translated from (Tekken 4: `SLUS_203.28`, 1 956 544 bytes).
- `launch.txt` ↔ package folder layout (`disc/<ELF>`, `disc.iso`).
- Forwarder `DEFAULT_NRO` ↔ SD path `sdmc:/switch/ps22switch/Tekken-4/Tekken-4.nro`; title ID in `forwarder.json` and
  `make_forwarder.py` (`0x0500544B4E340000`).

## 8. Build and deployment architecture

See BUILD_AND_SETUP.md. Summary: PC tools in `C:\ps2-native-recomp-main\tools\portable` + Git Bash produce `generated/`; WSL1
`devkita64` cross-compiles into `/tmp/switch-build-<name>` and copies `.nro`/`.elf` to `F:\ps22switch\build\switch\<name>\`;
the package folder is copied by hand to the SD card.
