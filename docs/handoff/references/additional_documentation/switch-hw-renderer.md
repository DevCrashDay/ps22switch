# A hardware-rasterizer GS renderer for the Switch

## Why

On the Switch (Tegra X1, NVK) Tekken 4 reaches 60 fps in menus with the built-in overclock, but fights run at about 7 fps, and the
GPU is the limit there (logs of 2026-10-30). The renderer the PC uses (`runtime/src/gs/gpu`, through the WebGPU-on-Vulkan layer in
`gpu/vk`) does the whole PS2 pixel pipeline in one compute program: one workgroup per 8x8 screen tile walks every primitive that
touches the tile, in order. That is exact and fast on a desktop GPU. On the Tegra it is not:

- the raster program needs 104 registers per thread, so only 16 warps fit on the SM (a quarter of what it can run);
- every pixel walks every primitive of its tile, with the edge tests, the setup loads and the texture decode in the shader, instead of
  the fixed-function rasterizer, depth test and blender the GPU has;
- one CLUT load is one workgroup looping over the loads in turn (2.9 ms each in a fight).

The fix is the approach of PCSX2's hardware renderer: draw the PS2's triangles and sprites as real triangles into real colour and
depth images with vertex and fragment shaders, and keep the 4 MB of PS2 video memory only for what has to be bit-exact (uploads,
CLUTs, local copies, the display, downloads).

## Shape

`gpu_hw.cpp` implements the same interface as `gpu.cpp` (`gpu.hpp`: `draw`, `image_begin`/`image_data`, `local_copy`, `load_clut`,
`flush`, `display`, `download`, `read_state`/`write_state`), directly on Vulkan. The emulation side does not change. It is chosen
with `PS2_GS=hw` and becomes the Switch default once it draws Tekken 4 correctly; the compute renderer stays as the reference.

### Video memory and targets

- VRAM stays a storage buffer, swizzled exactly as on the PS2. The compute programs of `gs.wgsl` that only move memory (upload,
  copy_read, copy_write, load_clut, display) keep running on it.
- A *target* is a colour image (`R8G8B8A8_UNORM`, 1x scale first) or a depth image (`D32_SFLOAT`) standing for one framebuffer:
  key (base pointer, buffer width, format), size from the scissor/drawn area.
- Draws go into targets. A target is *loaded* from VRAM (unswizzle, a compute pass) when VRAM was written behind its back (an upload,
  a local copy, a CPU write), and *written back* (swizzle) to VRAM when VRAM is read: a texture from that memory, the display, a
  download, a local copy from it, or a save state. Both are tracked per page (8 KB), as `gpu.cpp`'s `g_rendered` already does.

### Drawing

- One graphics pipeline per combination of the state that cannot be dynamic (blend equation, depth compare and write, colour mask,
  topology), cached by key.
- Vertex shader: the GS vertex (XYZ fixed point minus the offset, Z as 32-bit integer, colour, STQ or UV, fog) to clip space;
  `flat` interpolation when IIP is off; sprites become two triangles on the CPU.
- Fragment shader: texture fetch from the VRAM buffer (the `gs.wgsl` swizzle tables and format decode, CLUT from the clut buffer,
  nearest or bilinear, wrap modes), TFX (modulate, decal, highlight 1/2), fog, alpha test (`discard`, or splitting colour and depth
  writes for the FAIL modes), FBA, and the destination-alpha test by reading the target as an input attachment.
- Depth: Z (up to 32 bits) mapped to `D32_SFLOAT` as `z / 2^32`, the comparison as GEQUAL/GREATER/ALWAYS/NEVER.
- Blending: the PS2 equation `((A - B) * C >> 7) + D` maps to fixed-function blending for the common cases (alpha blend `Cs, Cd, As,
  Cd`, additive `Cs, 0, As, Cd`, subtractive `0, Cs, As, Cd`, copy), with `As * 2` as a blend constant trick for the `>> 7`. The
  rest (C = Ad, colour clamp off) reads the target as an input attachment and blends in the shader, one primitive per draw where
  primitives overlap.

### What stays exact

Uploads, CLUT loads, local copies, the display and downloads stay the compute programs they are now, on the same memory. Pictures
drawn by the hardware path will differ from the compute renderer in the last bit here and there (interpolation, blending rounding);
`scratch/cmp.py`-style comparisons against the compute renderer on the PC measure how much.

## Milestones

1. **Targets and flat geometry**: target cache, load/write back, untextured Gouraud triangles and sprites with depth, the display
   through write-back. Check on PC against the compute renderer (Tekken 4 menus).
2. **Textures**: VRAM-sampled textures in the fragment shader (all PSMs, CLUTs, bilinear, wrap/clamp/region), TFX, alpha test, fog.
   Tekken 4 fights on PC.
3. **Blending**: fixed-function modes, the shader fallback, FBA, DATE, colour masks. Tekken 4 pixel comparison within tolerance.
4. **Switch**: pipeline cache on the SD card, frame pacing, the Switch default. Measure fights.
5. **Effects**: render-to-texture (a target sampled as a texture without a round trip through VRAM), half-pixel offsets, upscaling.

Milestones 1 to 3 are where most of the work is; each is checked on the PC first, where both renderers run.
