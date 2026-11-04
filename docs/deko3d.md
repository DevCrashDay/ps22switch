# deko3d: the console's own GPU API for the drawing thread

## Why

The drawing thread is the wall on the Switch. In a steady fight (12 299 shapes, 33.1 ms a frame, 30.2 fps) about 21 ms of it is the
drawing thread: present 4.4 ms, the driver 2.6 ms, our own emit ~1.5 ms, and the rest is graphics and compute work going through NVK.
The GPU itself measures 19.5 ms a frame and another ~15 ms a frame is never attributed.

Every call in that path crosses three layers we do not own: `gpu.cpp` → the WebGPU shim (`gpu/vk/wgpu_vk.cpp`) → NVK → the Tegra
driver. NVK's price on this console is known from another port: in Ultra Street Fighter IV the render thread cost **18.6 ms** under
NVK and **2.8 ms** with the same drawing moved to deko3d (GPU 9.3 → 8.4 ms). That is the same order as our 21 ms, so the drawing
thread's overhead is a plausible place for most of the missing frame time.

deko3d is the homebrew-side API of the console's own GPU driver: no Vulkan, no ICD, no SPIR-V, no EGL surface, no SDL in the path.
Command buffers, pipelines and descriptor sets are ours, recorded once and submitted directly.

## Stages

Each stage is meant to be runnable on the console on its own, so a failure says which layer failed.

0. **Bring-up** (done, `runtime/src/gs/gpu/deko/deko_stage0.cpp`): a device, a swap chain on the console's default window, a clear.
   No SDL, no Vulkan, no game. Proves the device opens, the window can be ours, a command list is submitted, and the compositor shows
   the result. `PS2_DEKO3D_DRAW=1` adds a triangle drawn with a shader uam compiled: the whole shader path on its own.
1. **The calls `gpu.cpp` makes**: `wgpu_deko.cpp` in the same folder, the same file-and-a-half the Vulkan shim is
   (`gpu/vk/wgpu_vk.cpp`), implementing what the hardware path actually uses — about a dozen entry points (device, queue, buffer and
   texture creation and writes, render pass, pipeline, bind groups, draw, submit, present). Until it exists, `PS2_DEKO3D` changes
   nothing about how the game is drawn.
2. **Shaders** (done for everything the renderer draws with): `hw.wgsl`, `blit.wgsl` and the bring-up triangle become deko3d shader
   modules with `scripts/wgsl_dksh.py` — naga writes GLSL 450 per entry point, uam compiles that to the Tegra X1's shader bytecode,
   and the script writes `runtime/src/gs/gpu/deko/dksh_table.cpp`, the counterpart of the SPIR-V table. All of it happens on the PC
   before the build, so no compiler and no GPL code goes into the `.nro`. See *Shaders* below for the sizes, the one program that does
   not translate, and how to run the script.
3. **Switch the hardware path over** and drop NVK from the link; the compute renderer stays as the reference on the PC.
4. **One command buffer a frame** with explicit barriers, instead of the per-pass submit the Vulkan shim does, and present from the
   same queue.

Expected: reaching the 42–45 fps the drawing thread's cost allows, with the texture cache (decoding) still needed for a steady 60. No
number is promised until it runs.

## Stage 0 as built

- `CMakeLists.txt`: `PS2_DEKO3D` (on for the Switch, off elsewhere; elsewhere it is a lethal error, since deko3d is the console's
  API). It links `$DEVKITPRO/libnx/lib/libdeko3d.a`, which comes with libnx, and adds `-DPS2_DEKO3D` for the host program.
- `runtime/include/ps2/deko_stage0.h`, `runtime/src/gs/gpu/deko/deko_stage0.cpp`: the test itself. Three `RGBA8_Unorm` back buffers
  the size the window reports, `DkImageFlags_UsageRender | DkImageFlags_UsagePresent`, a swap chain over them on
  `nwindowGetDefault()`, and a frame loop: wait for the fence of the frame before, acquire a back buffer, bind it, clear it to a colour,
  signal, submit, present. The colour turns through the hue wheel once a second, so a screen that is being presented again and again
  looks different from a stuck one. Every 60 frames it logs frames a second; at the end, `[deko] stage 0 done`.
- `host/main.cpp`: `PS2_DEKO3D=<frames>` runs this instead of the game, **before `open_window()`** — SDL takes the console's window for
  its EGL surface, which is exactly why the Vulkan present path (`PS2_VK_PRESENT=1`) could not create a swap chain on it: 36
  combinations of format, present mode, transform and image count all came back `VK_ERROR_INITIALIZATION_FAILED`.
- `build/switch/tests/H-deko3d/`: the .nro and the `launch.txt` that runs it (`PS2_DEKO3D=300`, five seconds).

Being part of every Switch build costs 77 824 bytes of `.nro` and nothing else: without `PS2_DEKO3D` set, no deko3d call is ever made.

### Measured on the console (2026-11-04, `build/switch/tests/H-deko3d`)

```
[deko] window 1280x720
[deko] device open
[deko] 3 back buffers of 1280x720
[deko] swap chain made
[deko] 60 frames in 0.9 s (63.8 frames a second)
[deko] 300 frames in 4.9 s (60.7 frames a second)
[deko] stage 0 done: 300 frames presented in 4956 ms (60.5 frames a second)
```

60.5 frames a second, steady (63.8 at the start, 60.7 at the end), at the same clocks the game runs with (CPU 1785, GPU 921.6, memory
1600 MHz). The colours were seen on the screen, which is the part a log cannot prove.

This is the step the Vulkan path never got past: a swap chain on this console's window (36 combinations of format, present mode,
transform and image count, all `VK_ERROR_INITIALIZATION_FAILED`), and then present at refresh rate with three buffers in flight.

What it does not do: draw anything from the PS2, read a texture, keep a pipeline. It is a display-path test, not a renderer.

`PS2_DEKO3D_DRAW=1` adds the triangle of `deko/stage_draw.wgsl` over the colour of each frame: two programs uam compiled, loaded as
deko3d shader modules into a `DkMemBlockFlags_Code` memory block, a pipeline, a viewport, a scissor and one `dkCmdBufDraw`. It costs
about 130 lines and proves the shader path on the console before the renderer's own 18 KB programs are involved.

Measured (2026-11-04 22:51, same clocks): `[deko] shader modules loaded: 1536 + 1280 bytes of code` — the space reserved for each
module, which is its code section plus the part of a page deko3d cannot use, on a `DK_SHADER_CODE_ALIGNMENT` boundary; the module
files themselves are 768 and 512 bytes, the rest of which is the control block that stays in main memory. deko3d accepted both modules
(no complaint from the device) and the loop ran 300 frames in 4963 ms, 60.4 a second.

**Nothing was drawn in that run**, and neither deko3d nor the shader compiler said a word about it. Two causes, both now fixed, both
worth knowing before stage 1:

- naga's GLSL writer adjusts the vertex position for *Vulkan* unless told not to: it negates `gl_Position.y` and rewrites `z` as
  `z * 2 - w`, to move WebGPU's 0..1 depth into Vulkan's -1..1. deko3d's depth is 0..1, so every vertex landed at z = -1, behind the
  near plane, and the triangle was clipped away. `scripts/wgsl_dksh.py` now passes `--keep-coordinate-space`; the Y axis is the
  device's business (`DkDeviceFlags_OriginLowerLeft`), not the shader's.
- deko3d's defaults are not "draw everything": `dkRasterizerStateDefaults` culls back faces, and `dkDepthStencilStateDefaults` turns the
  depth test *and* depth writes on. A one-winding triangle with no depth target can disappear into either. The test now culls nothing
  and has depth off; the renderer's own draws must map the PS2's winding and depth state onto these two deliberately.

The frame rate was unaffected by either, which is the other lesson: a draw that produces nothing costs as much as one that draws.

Two things the Vulkan path learned the hard way apply here as well: the display offers exactly three buffers and only mailbox and
immediate present modes, and the window has to be ours alone. Stage 0 is run before SDL exists for that reason; once stage 1 draws the
game, SDL's window and EGL surface have to go, not sit beside deko3d.

## Shaders

`scripts/wgsl_dksh.py` turns the renderer's WGSL into deko3d shader modules, entirely on the PC, before the build:

```
naga --profile core450 --entry-point hw_vs hw.wgsl hw_vs.vert     (naga-cli; the same program wgsl_spirv.py uses)
uam hw_vs.vert -s vert -o hw_vs.dksh                            (uam, built from source: meson setup build && ninja -C build)
```

uam wants a `layout(binding = N)` on every block, sampler and image, and that number *is* the deko3d binding ID. naga writes no
bindings, but names each global after the one it came from (`_group_0_binding_3_vs`), so the script puts the number back; a global it
cannot name is a hard error rather than a guess. The table is checked against the three `.wgsl` files by sha256 at configure time, as
`spirv_table.cpp` is against `gs.wgsl`, so a stale table cannot be built by accident.

What it produced (2026-11-04, naga 29, uam 1.1.0):

| program | stage | bytes |
| --- | --- | --- |
| `hw_vs` | vertex | 3 840 |
| `hw_full_vs` | vertex | 768 |
| `hw_fs` (dual-source blending) | fragment | 18 176 |
| `hw_fs_single` | fragment | 18 176 |
| `hw_load_depth` | fragment (writes `gl_FragDepth`) | 2 816 |
| `hw_load_color` | compute (storage image) | 3 328 |
| `blit` `vs` / `fs` | vertex / fragment | 768 / 37 376 |
| `triangle_vs` / `triangle_fs` | vertex / fragment | 768 / 512 |

`hw_writeback` is the one that does not translate: it reads VRAM back through a `textureLoad` of a *depth* texture, and naga cannot
write that as GLSL at all ("`textureLoad` from depth textures is not supported in GLSL"). Nothing is lost that a deko3d renderer needs:
it does not have to keep depth in a depth image to read it back, so that program becomes one reading a colour image (`R32F`), which is a
different program rather than a translation of this one.

To rebuild the table: `python scripts/wgsl_dksh.py --naga <naga> --uam <uam build/uam>`. Neither program is needed to build the
`.nro` — only to change a shader.

## Stage 1: the shim (in progress)

`runtime/src/gs/gpu/deko/wgpu_deko.cpp` answers the same 63 WebGPU calls `gpu.cpp` makes, on deko3d, and `-DPS2_DEKO3D_RENDER=ON`
swaps it in for `gpu/vk/wgpu_vk.cpp`. That build **links**: 133 targets, a 36.7 MB `.nro` with 67 `wgpu*` symbols and no Vulkan shim
(NVK is still linked until stage 3). `gpu.cpp` itself has not changed.

What the deko3d version does differently, and why it should be cheaper than the Vulkan one:

- **Buffers bind by address, not by descriptor set.** deko3d takes a GPU address and size per slot (`dkCmdBufBindStorageBuffers`), so a
  bind group is just a list of (slot, address, size) that a pass binds when it is used — dynamic offset included. No descriptor memory,
  no layout matching, no `vkAllocateDescriptorSets`. Vulkan's equivalent was ~2.4–2.9 µs per draw on the A57.
- **A queue write is a memcpy.** Buffers are CPU-uncached/deko3d-visible memory, so `wgpuQueueWriteBuffer` writes the bytes and the next
  command list opens with `DkBarrier_Full, DkInvalidateFlags_L2Cache` (the Tegra's L2 is not coherent with the CPU). The Vulkan version
  copied through a staging buffer every time.
- **A readback is a barrier and a wait**, not a staging copy plus a fence.
- **Shaders are looked up, not compiled**: the pipeline's entry point name finds the DKSH module `uam` compiled, so there is no SPIR-V,
  no NAK, no shader compilation on the console at all.
- **Command memory belongs to the encoder**, and `wgpuQueueSubmit` replays the encoders' lists into one device list
  (`dkCmdBufCallList`) and submits that; the memory is retired when the fence says the GPU is done with it.

Implemented in this slice: instance, adapter (limits, dual-source blending advertised, timestamp queries not), device and queue, buffers
(created zeroed, writes, mapping), textures and views, the surface (three swap-chain back buffers on the console's window, acquire,
present), command encoders and submission, and a render pass that binds its colour target, clears it, and takes viewport, scissor and
blend constant.

Not in it yet: bind groups bind nothing, pipelines create no deko3d state, and `draw`/`dispatch` only log once that they are the next
slice. So `PS2_DEKO3D_RENDER=ON` boots and configures but draws nothing — it is not a playable build yet.

Two things to settle in the next slice: the front-face winding and depth convention (`DkDeviceFlags_OriginLowerLeft`, `DkFrontFace`,
and the PS2's own cull/ZTST state — the triangle test already showed how silently a wrong choice draws nothing), and `gl_FragCoord`'s
origin in `hw_fs`, which must match what the Vulkan path produced. Both are cheapest to check with a screenshot comparison against the
Vulkan build.

One finding for stage 3: **NVK cannot simply be dropped from the link yet.** Trying it fails in devkitPro's own SDL2, whose static
`libEGL.a` (from the NVK SDK) contains Mesa's zink, and zink calls `nvk_loaderless_GetInstanceProcAddr` and the `vk_*` helpers. So the
stage that removes NVK is the same one that stops using SDL's renderer and takes the display with deko3d — the two are one change.

## Prior art

`ref-nether/` (NetherSX2's source, on this machine as a reference, not built) already has a complete deko3d backend —
`ref-nether/source/deko3d/`, 3 826 lines including the uam build recipe: device and swap chain (`dk_gpu.cpp`), presentation
(`dk_presentation.cpp`), image transfers (`dk_texture.cpp`), render state, the UI, and a shader path (`compiler/Uam.cmake`,
`compiler/UamBridge.cpp`, `dk_shader_source.cpp`). What it settles:

- **How a DKSH module is loaded** (`dk_shader.cpp`, worth following exactly): the module is a control block plus a code section; the
  code goes into a `DkMemBlockFlags_Code` block at a `DK_SHADER_CODE_ALIGNMENT` offset, `dkShaderMakerDefaults(&maker, block, offset)`
  takes that address, and `maker.control` points at the control block, which stays in main memory.
- **Shaders are unsigned**: DKSH goes through `dkShaderInitialize` with no signature, so this only ever runs on a console with the
  signature patches homebrew already needs. Same as any deko3d homebrew.
- **Licences**: uam's own files are zlib (fincs), `mesa-imported/` is Mesa's MIT, and uam ships a command-line compiler — so the
  offline path above uses no GPL code at all. NetherSX2's route (compiling uam into the app through a Dolphin-derived bridge,
  GPL-2.0-or-later, and isolating its symbols from NVK's Mesa with `objcopy --keep-global-symbol=UamCompileGlsl`) is only needed if a
  shader has to be compiled on the console. Ours do not: uam produces bytecode for the Tegra X1 on any x86 PC, which is also why the
  `.nro` carries no compiler.

