Tekken 4 test builds, 2026-10-30

Each folder holds Tekken-4.nro and launch.txt. To try one, copy BOTH files over the ones in
sdmc:/switch/ps22switch/Tekken-4/ (the forwarder and the homebrew menu start that folder), play about a minute
into a fight, and keep ps2run.log (rename it to the folder's letter, e.g. ps2run-A.log).

All builds have: the hardware renderer, the overclock (CPU 1785 / GPU 921 / memory 1600), read-only video memory for
drawing, images kept between batches, no CPU tile binning, the fast CLUT loads.

A-fast        The plain build, only the frame-rate line in the log. This is the real speed (profiling costs time).
B-profile     Same build with GPU timing and the CPU breakdown, to see where the time goes now.
C-cores       Threads on other cores: window/GPU thread alone on core 2, vector units on core 0.
D-inflight3   The CPU may run only 3 GPU submissions ahead instead of 8 (less latency, maybe less stutter).
E-noreorder   Without the upload/palette reordering, to see what it brings.

Most useful order if time is short: A, then B, then the others.
F-nomerge     Without merging render passes that only have palette loads between them, to see what that brings.
G-decode      Textures decoded once ahead of drawing (PS2_HW_DECODE=1). Same picture as A on PC (42 of 42 frames identical);
              on PC it was a small loss (decoding cost more than drawing saved), so it only helps if texture reads are
              much dearer on the Switch. Compare its fps with A.
G-decode-profile  The same with the GPU timing, like B: compare decode + hw_draw with B's hw_draw.
