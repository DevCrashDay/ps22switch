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
2. **Shaders**: `hw.wgsl` (and `blit.wgsl`) have to become DKSH. naga turns WGSL into GLSL; the console toolchain needs uam
   (GLSL → the console's shader bytecode) and deko3d's DKSH container. The uam *headers* come with libnx; a `libuam.a` was not found in
   `/opt/devkitpro/libnx/lib`, so this is the first thing to test before stage 1 is worth writing.
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

What it does not do: draw anything from the PS2, read a texture, keep a pipeline. It is a display-path test, not a renderer.

If it works, `ps2run.log` holds `[deko] device open`, `[deko] 3 back buffers of 1280x720`, `[deko] swap chain made`, five
`[deko] N frames in ... s` lines at about 60 frames a second, and `[deko] stage 0 done`, with colours on the screen in between. If it
fails, the last `[deko]` line names the step, and the device's own complaints are logged by the debug callback.
