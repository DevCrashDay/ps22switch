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
   the result.
1. **The calls `gpu.cpp` makes**: `wgpu_deko.cpp` in the same folder, the same file-and-a-half the Vulkan shim is
   (`gpu/vk/wgpu_vk.cpp`), implementing what the hardware path actually uses — about a dozen entry points (device, queue, buffer and
   texture creation and writes, render pass, pipeline, bind groups, draw, submit, present). Until it exists, `PS2_DEKO3D` changes
   nothing about how the game is drawn.
2. **Shaders**: `hw.wgsl` (and `blit.wgsl`) have to become DKSH, deko3d's shader container. naga (already here, for SPIR-V) turns WGSL
   into GLSL; the console toolchain then needs **uam**, the GLSL → NVIDIA shader compiler extracted from Mesa's GLSL and nv50 back
   ends. There is no prebuilt uam on this machine (`libnx/lib` has `libdeko3d.a` and nothing else, and no `uam` headers). Another port
   does it by building uam from source into the app — see *Prior art* below — so this stage is a known quantity, not a research risk.
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

If it works, `ps2run.log` holds `[deko] device open`, `[deko] 3 back buffers of 1280x720`, `[deko] swap chain made`, five
`[deko] N frames in ... s` lines at about 60 frames a second, and `[deko] stage 0 done`, with colours on the screen in between. If it
fails, the last `[deko]` line names the step, and the device's own complaints are logged by the debug callback.

Two things the Vulkan path learned the hard way apply here as well: the display offers exactly three buffers and only mailbox and
immediate present modes, and the window has to be ours alone. Stage 0 is run before SDL exists for that reason; once stage 1 draws the
game, SDL's window and EGL surface have to go, not sit beside deko3d.

## Prior art

`ref-nether/` (NetherSX2's source, on this machine as a reference, not built) already has a complete deko3d backend —
`ref-nether/source/deko3d/`, 3 826 lines including the uam build recipe: device and swap chain (`dk_gpu.cpp`), presentation
(`dk_presentation.cpp`), image transfers (`dk_texture.cpp`), render state, the UI, and a shader path (`compiler/Uam.cmake`,
`compiler/UamBridge.cpp`, `dk_shader_source.cpp`). What it settles:

- **Shaders**: uam is built from source into the app (`FetchContent` of `github.com/PalindromicBreadLoaf/uam`, pinned commit, needs
  bison, flex and Mako at configure time). `UamCompileGlsl(glsl, stage, &size, &log)` returns a ready DKSH blob — the bridge carries
  deko3d's own `dksh.h`/`nv_shader_header.h` writer, so nothing else has to be written for stage 2 beyond turning `hw.wgsl` into GLSL.
- **Symbols**: uam imports a large part of Mesa (the GLSL compiler, nv50_ir), which clashes with the Mesa that NVK links statically. The
  fix is in `Uam.cmake`: link uam's objects into one relocatable object and strip every global symbol but `UamCompileGlsl`
  (`objcopy --keep-global-symbol=UamCompileGlsl`). Worth copying for stage 3, where NVK is still linked.
- **Shaders are unsigned**: DKSH goes through `dkShaderInitialize` with no signature, so this only ever runs on a console with the
  signature patches homebrew already needs. Same as any deko3d homebrew.
- **Licences**: uam is Mesa-derived (MIT for those parts); the Dolphin-derived bridge files are GPL-2.0-or-later, and this project's
  licence is not GPL. That has to be settled before stage 2's code is added, not after.

