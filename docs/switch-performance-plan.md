# Tekken 4 on the Switch: where the time goes and what to try next

State of 2026-10-30 (overclocked to CPU 1785 / GPU 921 / memory 1600 MHz, hardware renderer `PS2_GS_HW=1`):

| Scene | Before the work (software renderer) | Compute renderer on Vulkan | Hardware renderer, latest log |
|---|---|---|---|
| Menus | 1 fps | 24 to 34 fps, 60 overclocked | 60 fps |
| Story, FMV | 1 fps | 30 to 46 fps | 40 to 50 fps (FMV: video decoding on the CPU) |
| Fights | 1 fps | 7 fps | 22 to 24 fps |

Builds after that log add: read-only video memory for the pixel shader (no cache-bypassing atomic loads), the setup carried by the
vertices, images kept between batches, no CPU tile binning, CLUT loads looked up from a table. None of these has been measured on the
console yet; the test builds in `build/switch/tests` are for that.

## Where a fight frame goes (last log, 22 fps)

GPU, per second: drawing 310 ms, uploads 85 ms, fill and write-back 100 ms (now much less), CLUT loads 55 ms, plus about 40 % of
the GPU's time that the timestamps do not cover (barriers, the copies of queue writes, gaps). Emulation thread: game code 39 %, waiting
for the window thread 46 %. So the GPU (and the window thread that feeds it) is still the limit in fights; menus are paced by the game.

## Approaches, roughly in order of expected gain per day of work

1. **Texture cache** (GPU, large). The pixel shader decodes every texel from PS2 memory: the swizzle table, the word, the CLUT, four
   times for bilinear. Decoding each texture once into an RGBA image when it is first used (keyed by address, format, size and CLUT;
   dropped when an upload, write-back or copy touches its memory) turns that into one image read, and lets the GPU's texture units do
   the filtering where the PS2's rounding allows. This is how PCSX2's hardware renderer works. Expected: the largest single cut in
   drawing time.
2. **Fewer passes and barriers** (GPU, medium). Every batch is a render pass with a full barrier on each side, every dispatch has a
   barrier after it. On Tegra one barrier costs tens of microseconds. Consecutive batches on the same buffers can share one render pass;
   uploads that do not overlap need no barrier between them. The uncovered 40 % of GPU time is mostly this.
3. **Uploads** (GPU, medium). About 90 upload dispatches per frame, mostly because the game uploads a palette, loads the CLUT from it,
   uploads the next palette to the same place, and so on. Taking the CLUT straight from the uploaded bytes when the CLUT load reads
   exactly what was just uploaded would remove most of them.
4. **Present without the readback** (CPU and GPU, medium). Each frame is read back from the GPU, copied, and uploaded again into
   SDL's OpenGL texture on the window thread, the same thread that records the GPU work. Presenting straight from Vulkan (NVK's
   `VK_NN_vi_surface`, as NetherSX2 does) removes the copy and frees the window thread.
5. **Depth precision** (correctness). Depth now stays a float between batches; PS2 depth is integer. Writing the depth rounded in the
   shader brings back exact ties (decals, shadows) at some cost; or keep the images and round only on write-back as now.
6. **CPU side, for when the GPU stops being the limit**: the vector unit 0 instructions the game code calls (multiply-add with flags,
   about 14 % of the emulation thread) as ARM NEON; timer-polling loops (`hw::read32`, 13 to 20 %) skipped more aggressively; MPEG
   video decoding (IPU, 50 % in FMVs) with NEON.
7. **Deko3D backend** (large). NetherSX2 has a native Deko3D renderer next to NVK. Deko3D avoids Mesa entirely and is the fastest way
   to the Switch's GPU, but it is a separate backend to write and maintain; worth it only if NVK's overhead turns out to be the limit
   after 1 to 4.

## Measuring on the console

`launch.txt` lines: `PS2_PRESENT_LOG=1` (frames per second, waits), `PS2_VK_PROFILE=1` (GPU time per program; costs some time
itself), `PS2_PARTS=1` (emulation thread breakdown), `PS2_VK_STATS=1` (shader statistics and machine code), `PS2_GS_HW=0` (the
compute renderer), `PS2_SWITCH_CORES=e,v,w`, `PS2_VK_IN_FLIGHT=n`, `PS2_PRE_REORDER=0`.
