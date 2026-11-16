upstream 2026-11: the part that merges cleanly, and what is left

**Superseded (2026-11-16):** upstream is now merged with git (`git merge` of github.com/exoteric33/ps2-native-recomp main, up to
a50471a "Merge pull request #109 from Stilgear/pr/hd-textures"), so its history is part of this fork's and the next update is an ordinary
`git merge upstream/main`. 20 files conflicted; how each was settled is in the merge commit's message. The notes below describe the
earlier file-by-file attempt (branch upstream-2026-11) and are kept for its reasoning.

The upstream project (C:\ps2-native-recomp-main) is not a git repository: it is a folder that is replaced when it updates. This fork
started from a snapshot of it (0660c5e "Import ps2-native-recomp"), so the update is a three-way merge: base = that snapshot, ours =
this branch, theirs = the folder.

scripts/upstream_merge.py does it per file with git merge-file (after normalising line endings, without which every file looks
conflicted). The result:

    49 files merge cleanly    (this commit)
    29 files are new          (this commit)
    26 files conflict         (not in this commit: reverted to ours, listed below)

The clean part is real work, not just plumbing: vu_jit.cpp, vu_micro.cpp, recomp/vurecomp_gen.cpp (the VU0 AOT generator), gs.cpp,
queue.cpp, gif.cpp, disc.cpp, iop/{io,hw,sio2,cdvdfsv}.cpp, kernel.cpp, fpu.hpp, os.hpp, vu_ops.hpp, plus the tooling and tests. It does
not compile on its own yet, because the 26 remaining files carry the interfaces those need.

What conflicts, and why: both sides changed the same lines. Ours are the Switch and deko3d changes (the Vi surface target, the deko3d
shim, the SDL-less window, the Switch clock and crash report); theirs are the update's new features. The first seam the compiler found:

    runtime/src/gs/gs.cpp: too many initializers for 'const ps2::gs::gpu::DisplaySource'

because the update's DisplaySource carries a DisplayOverlay (the quick menu's panel) and ours does not, and it made mode() an inline over
a global instead of a call. That is the shape of all 26: small interface drift, not a rewrite.

The 26, with the size of each conflict (lines inside the conflict regions):

    runtime/src/ee_live.cpp             1400     runtime/src/iop/sif.cpp               290
    app/app.ps1                          611     README.md                             288
    host/window.cpp                      321     runtime/src/gs/gpu/gpu.cpp            206
    runtime/src/system.cpp               176     CMakeLists.txt                        136
    runtime/src/dispatch.cpp             137     recomp/eerecomp_gen.cpp               130
    runtime/src/iop/cdvd.cpp              90     runtime/src/memory.cpp                 81
    recomp/analysis.cpp                   67     host/main.cpp                          64
    runtime/include/ps2/ee_live.hpp       45     runtime/src/vif.cpp                    12
    runtime/src/iop/spu2.cpp              35     recomp/analysis.hpp                    33
    recomp/eerecomp_gen.hpp               31     runtime/src/dmac.cpp                   27
    runtime/src/iop/libs.cpp              15     runtime/src/iop/kernel.cpp             12
    runtime/src/ipu/ipu.cpp                8     runtime/src/gs/gpu/gpu.hpp              6
    runtime/include/ps2/system.hpp         6     runtime/src/hw.cpp                      7

The order that costs least: gpu.hpp (6 lines, the interface above) with the overlay fields ours does not use yet, then gpu.cpp (206), then
the emulator core (ee_live, dispatch, system, memory, vif, dmac), then the IOP group (sif, cdvd, spu2, libs, kernel, ipu), then the host
(window, main), then the tools and the app. Building after each group is what says whether a seam is really closed.

Nothing here is committed to the working branch: it is branch upstream-2026-11, checked out at C:\ps22switch-tmp\merge.
