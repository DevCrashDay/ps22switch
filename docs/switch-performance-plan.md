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

## Re-reading the last log (2026-11-04): the window thread, not the GPU, is the limit

The steady part of the last fight log (`references/important_logs/switch_ps2run_2026-10-30_hw-path_fight.log`, lines 130 to 170) says
something the earlier summary above hides. Per second, at 25 to 26 fps:

| Measured | Value | Per frame |
|---|---|---|
| Window thread busy (recording all GPU work **and** presenting) | 999 to 1000 ms | **38 ms** |
| GPU busy (sum of the shim's per-dispatch timestamps) | 597 to 600 ms | 23 ms |
| Of the window thread: blocked because the GPU is 9 submissions behind | 337 to 392 ms | **14 ms** |
| Emulation thread waiting for the window thread | 10 to 12 ms | 10 to 12 ms |
| Submissions | 78 to 80 | 3 |
| Draws (render passes) / uploads / CLUT loads | 500 / 2500 / 500 | 19 / 95 / 19 |

The GPU is **only ~60 % busy** while the window thread is **100 % busy**. So the frame rate is not set by how much the GPU has to do
(it has 40 % of its time free); it is set by how long the window thread needs to (a) do its own work and (b) wait for the GPU. That
thread costs 38 ms a frame and 14 ms of it is the wait above, so about **24 ms a frame is its own CPU work**. The previous session read
the same log as "GPU bound" because the GPU's timestamps cover 60 % of the second; it is the other 40 % that decides.

This matters because it changes what to write next:

* if most of the 24 ms is **presenting** (SDL: read back the picture, upload it into an SDL texture, `SDL_RenderPresent` including any
  wait for the screen's refresh), the fix is to present from Vulkan directly (R7) - it removes work *and* a possible vsync wait, and
  nothing on the GPU side needs to change;
* if most of it is **recording** (95 upload dispatches and 19 render passes a frame, each a handful of driver calls), the fix is fewer
  commands (R10, R11) - worth 30 to 40 % of the thread's time, and it also lowers the GPU's 23 ms;
* only if the GPU were nearly 100 % busy would the texture cache (R9) be the first thing to do, and it is the largest piece of work.

A fight frame needs 16.6 ms for 60 fps. Both the window thread (38 ms) and the GPU (23 ms) are above it, so 60 fps needs roughly
2.3x on that thread and 1.4x on the GPU - the GPU part is the easy half. Nobody should promise 60 fps from a single change.

### The one measurement that separates those cases

The .nro already on the SD card (commit `8e693c9`) prints everything needed; only `launch.txt` has to change - see
`build/switch/tests/Z-diagnose/`:

| Line | Field | Meaning |
|---|---|---|
| `[waits]` | `showing the picture M ms` | SDL upload and present, including a possible wait for the screen |
| `[vk-profile]` | `CPU waited X ms` | blocked because 9 submissions were already queued |
| `[hitch]` | `picture ...: N ms busy ... gif A ms; present B ms;` | the same split per picture, by kind of work |
| `[waits]` | `rendering R ms` | the whole thread; `R - M - X` is its own CPU work |

That run decides between R7 (present), R10/R11 (fewer commands) and R9 (texture cache). The A to G test builds answer a different
question - which of the seven unmeasured commits helped - and should be measured after, not before.

## 2026-11-04: profiling the drawing thread on the PC, and the one quadratic loop it found

The console log above cannot say where the drawing thread's own CPU time goes, so the sampling profiler was pointed at that thread
instead of the emulation thread (`PS2_PROFILE_RENDER=<file>`, new, works on a `PS2_THREADS=1` PC run) and `PS2_EMIT_LOG=1` (new) was
added to time the three parts of the drawing path. Fight, PC, hardware path, before the change:

| Part of the drawing thread | Share |
|---|---|
| `emit_pre()` - turning the queued uploads and CLUT loads into dispatches | **19.8 %** (232 ms of every second) |
| `gpu::draw()`, `gif::render()`, `gs::write_packed()`, `gs::kick()`, `gs::write()`, `gs::vertex()`, the queue ring | 22 % |
| inside the Vulkan driver and the C library (`memcpy`, `memcmp`) | 14 % |
| waiting for the emulation thread (PC only: there the emulation thread is the limit) | 42 % |

`emit_pre` asked "does a CLUT load read memory that a later upload writes?" with two nested loops over every queued operation, and a
Tekken 4 fight queues thousands of them per frame: **343 million overlap tests a second**. One forward pass over a few merged
intervals answers the same question exactly; `emit_pre` fell from 232.6 to 22.9 ms/s (10x) and the 42 comparison frames stayed
identical (`cmp.py`: same 42, different 0). The PC's frame rate did not move, because on the PC the emulation thread is the limit, not
the drawing thread - on the Switch it is the other way round, which is why this was worth doing.

After the fix the same profile says the drawing thread's own work is now: `gpu::draw` 10.8 %, `gif::render` 6.7 %, `gs::write_packed`
4.3 %, `gs::kick` 4.0 %, the queue ring 4.2 %, `emit_pre` 1.5 %, `flush_batch` 0.7 %. `submit()` - the driver calls themselves - is
93.6 ms/s, so on the Switch, where every driver call is several times dearer, that is the part to watch next.

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

`launch.txt` lines: `PS2_PRESENT_LOG=1` (frames per second, waits, how much of the window thread goes into showing the picture),
`PS2_VK_PROFILE=1` (GPU time per program, and the CPU time spent waiting for the GPU; costs some time itself), `PS2_PARTS=1`
(emulation thread breakdown), `PS2_EMIT_LOG=1` (every 2 s: `emit_pre`, `flush_batch` and the driver calls of a `submit`, in ms/s),
`PS2_HITCH_LOG=<ms>` (one line per picture over that many ms, with the window thread's time per kind of work),
`PS2_VK_STATS=1` (shader statistics and machine code), `PS2_GS_HW=0` (the compute renderer), `PS2_SWITCH_CORES=e,v,w`,
`PS2_VK_IN_FLIGHT=n`, `PS2_PRE_REORDER=0`, `PS2_HW_MERGE=0`, `PS2_HW_DECODE=1`.

On the PC: `PS2_THREADS=1` runs the window run's three threads without a window, `PS2_PROFILE=<file>` samples the emulation thread,
`PS2_PROFILE_RENDER=<file>` samples the drawing thread (both with `PS2_PROFILE_FROM=<frame>`), `PS2_EMIT_LOG=1` as above.
