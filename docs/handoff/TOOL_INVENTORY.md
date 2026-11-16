# Tool inventory

Versions were read on 2026-11-04 from the machine `CHRIS` (Windows 10 Pro 10.0.19044) unless noted. "Status" uses:
**USED-OK** (used, worked), **USED-PARTIAL**, **FAILED** (tried, did not work), **INVESTIGATED** (looked at, not used),
**AVAILABLE** (present, not exercised in the Switch work). Locations are absolute.

## A. Switch cross-compilation (WSL)

| ID | Tool | Version | Location | Purpose / tasks | Status | Still required | Notes |
|---|---|---|---|---|---|---|---|
| T01 | WSL distribution `devkita64` | WSL **1** (`wsl -l -v` → VERSION 1; kernel `4.4.0-19041-Microsoft`), Debian 12 bookworm | Windows WSL | Hosts devkitPro; every Switch build runs here | USED-OK | yes | `switch_port.py --wsl-distro` picks another name. Build dir must stay inside WSL (`/tmp/switch-build-<name>`): compiling over `/mnt/f` is "many times slower" (comment in `switch_port.py`). |
| T02 | devkitA64 | `devkitA64 r29.2-1`; `aarch64-none-elf-gcc (devkitA64) 15.2.0` (`devkita64-gcc 15.2.0-7`), `devkita64-binutils 2.45.1-2`, `devkita64-newlib 4.6.0.20260123-4`, `devkitA64-gdb 14.1-1` | `/opt/devkitpro/devkitA64` | C++20 cross-compiler for Cortex-A57 (`-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft`) | USED-OK | yes | `-ffp-contract=off` is mandatory (CMakeLists.txt). gdb never used. |
| T03 | libnx | 4.12.0-1 | `/opt/devkitpro/libnx` | Horizon OS API: threads, svc, clkrst/pcv, hid, romfs, console | USED-OK | yes | Weak overrides used: `__libnx_initheap`, `__nx_nv_transfermem_size`. |
| T04 | switch-sdl2 | 2.28.5-4 (also sdl2_gfx 1.0.4-2, sdl2_image 2.0.4-1, sdl2_mixer 2.0.4-2, sdl2_net 2.0.1-3, sdl2_ttf 2.22.0-2 installed, unused) | `/opt/devkitpro/portlibs/switch` | window, audio, game controllers; picture shown through SDL's GL renderer | USED-OK | yes | Its `EGL`/`glapi` link deps are swapped for the NVK SDK's and `drm_nouveau` dropped (CMakeLists.txt 263–278), as OpenTrickyNX does. |
| T05 | switch-mesa / switch-glad | 20.1.0-5 / 0.1.27-1 | portlibs | devkitPro's own Mesa (pulled in by SDL2) | USED (replaced at link) | indirectly | Clashes with NVK's Mesa; must not be linked. |
| T06 | switch-tools | 1.13.1-1 | `/opt/devkitpro/tools/bin` | `elf2nro`, `nacptool` behind `nx_create_nro` / `nx_generate_nacp` | USED-OK | yes | |
| T07 | devkitPro CMake support | `switch-cmake 1.5.2-1`, `devkita64-cmake 1.1.3-1`, `dkp-cmake-common-utils 1.5.3-1`, toolchain `/opt/devkitpro/cmake/Switch.cmake` | WSL | `NINTENDO_SWITCH`, `nx_*` CMake functions | USED-OK | yes | |
| T08 | Wrapper scripts | created 2026-10-29 22:50 | `/usr/local/bin/switch-cmake`, `/usr/local/bin/switch-ninja` | set `DEVKITPRO`, `DEVKITA64`, `PATH`, call `/usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake` / `/usr/bin/ninja` | USED-OK | yes | Exact contents in BUILD_AND_SETUP.md. Who created them: UNKNOWN — not verified from available evidence (they existed when the thread began). |
| T09 | CMake (WSL) | 3.31.6 | `/usr/bin/cmake` | configure Switch build | USED-OK | yes | |
| T10 | Ninja (WSL) | 1.11.1 | `/usr/bin/ninja` | build | USED-OK | yes | |
| T11 | other devkitPro packages | `general-tools 1.4.4-1`, `switch-freetype 2.13.3-1`, `switch-zlib 1.3.1-1`, `switch-pkg-config 0.28-4`, `devkita64-rules 1.1.1-1` | WSL | zlib linked by NVK if the SDK lacks one | AVAILABLE | zlib yes | |
| T12 | Python 3 (WSL) | 3.11.2 | `/usr/bin/python3` | runs `scripts/wgsl_spirv.py` next to naga | USED-OK | yes (for shader changes) | |

## B. Vulkan / GPU on the Switch

| ID | Tool | Version | Location | Purpose / tasks | Status | Still required | Notes |
|---|---|---|---|---|---|---|---|
| T13 | Mesa NVK SDK for Switch (`mesa-switch-nvk-sdk.zip`) | 52 797 811 bytes, downloaded 2026-10-29 23:46Z from `github.com/DevCrashDay/OpenTrickyNX/releases/tag/nvk-sdk`; `MESA_REVISION`: `https://github.com/danfromtico/mesa-switch.git 0ff52079ca9e9e837ed27eb95a8d239f439a1864`. Reports `NVK GM20B`, Vulkan 1.4.354, driver 109060100 at run time | `F:\ps22switch\tools\nvk-sdk\` (zip + unpacked `portlibs\switch`) | statically linked Vulkan driver (`libvulkan.a` 85.7 MB, `libnvk.a`, `libnak_rs.a`, `libnir.a`, `libEGL.a`, `libglapi.a`, zstd, expat, z ...) | USED-OK | yes | Not a conformant implementation (log: "NVK is not a conformant Vulkan implementation"). Software robust-access and strong atomic loads are slow (see TECHNICAL_DECISIONS). `tools/` is git-ignored: a fresh clone must download it again. |
| T14 | Rust toolchain (rustup) | `rustc 1.99.1 (58cf8f9c3 2026-10-13)` | WSL `/root/.cargo/bin` | builds naga-cli | USED-OK | only to reinstall naga | Installed with user approval 2026-10-29 23:46Z. |
| T15 | naga-cli 30 | 30.0.1 | WSL `/root/.cargo/bin/naga` | first SPIR-V conversion (`584dd8e`) | USED, superseded | no | Replaced by naga 29 to match wgpu-native 29's WGSL dialect (`f42606b`). |
| T16 | naga-cli 29 | 29.0.4 | WSL `/root/naga29/bin/naga` (`cargo install naga-cli --version '^29' --root /root/naga29`) | `python3 scripts/wgsl_spirv.py --naga /root/naga29/bin/naga` → `runtime/src/gs/gpu/vk/spirv_table.cpp` | USED-OK | yes (after any `gs.wgsl`/`hw.wgsl` change) | Flags used by the script: `--index-bounds-check-policy Restrict --buffer-bounds-check-policy Restrict`; hw.wgsl also needs the dual-source-blending capability. |
| T17 | `PS2_VK_STATS=1` (VK_KHR_pipeline_executable_properties) | built in (`95decfe`) | runtime | register/spill stats + NIR/machine code to `vk_<program>.txt` | USED-OK on Switch | diagnostic | Replaces T18. |
| T18 | `NAK_DEBUG=print` + `MESA_SHADER_CACHE_DISABLE=true` | Mesa env vars | launch.txt | dump NAK shader code | **FAILED** (crash at start, also after redirecting stdout in `1549962`) | **never use** | Mesa's Rust std I/O does not work on Horizon. |
| T19 | `PS2_VK_PROFILE=1` | built in (`f42606b`) | runtime | GPU time per program per second via timestamps | USED-OK | diagnostic | Costs some time itself. |
| T20 | `PS2_PARTS=1` | built in (`9709c64`, fixed `6dcfc5d`) | runtime | emulation-thread time split every 2 s | USED-OK (after fix) | diagnostic | First version crashed on Switch. |
| T21 | `PS2_PRESENT_LOG=1` | built in (upstream, extended `5307556`) | host/window.cpp | fps + waits per second | USED-OK | diagnostic | |

## C. Windows / PC side (recompiler, PC test builds)

| ID | Tool | Version | Location | Purpose / tasks | Status | Still required | Notes |
|---|---|---|---|---|---|---|---|
| T22 | llvm-mingw clang++ | 23.1.1 | `C:\ps2-native-recomp-main\tools\portable\llvm-mingw\bin\clang++.exe` | builds `eerecomp`, `vurecomp`, PC `ps2run.exe` (pc-t4, pc-vk, pc-wgpu) | USED-OK | yes | Lives in the **upstream** folder on C:, not in this repo. |
| T23 | CMake (portable) | 4.4.4 (from `build/*/CMakeFiles/4.4.4`) | `C:\ps2-native-recomp-main\tools\portable\cmake` | PC configure | USED-OK | yes | Not on the Git Bash PATH by default: `export PATH="$P/llvm-mingw/bin:$P/cmake/bin:$P/ninja:$PATH"` with `P=/c/ps2-native-recomp-main/tools/portable`. |
| T24 | Ninja (portable) | version UNKNOWN — not verified from available evidence | `C:\ps2-native-recomp-main\tools\portable\ninja\ninja.exe` | PC build | USED-OK | yes | |
| T25 | Git for Windows / Git Bash (MSYS) | `git version 2.55.0.windows.5` | system | runs `scripts/port.sh`, `scratch/bench.sh`; git | USED-OK | yes | **Pitfall:** Git Bash rewrites `/opt/...` style arguments passed to `wsl`; run WSL scripts from a file through PowerShell (`wsl -d devkita64 -u root -- bash <wslpath>`), or `MSYS_NO_PATHCONV=1`. Bash heredocs mangled `\n` in Python edit scripts during the project: write such scripts with an editor/Write tool. |
| T26 | Python 3 (Windows) | 3.11.7 (`python`); `py` launcher at `C:\Windows\py.exe` is **broken** (points at missing `C:\Python313\python.exe`) | PATH | `switch_port.py`, `make_forwarder.py`, `cmp.py` | USED-OK | yes | `Switch_Build.cmd` prefers `py -3` when `py` exists → it will fail on this PC (BUG-016). |
| T27 | Pillow | 10.2.0 | Windows Python | `scratch/cmp.py` frame comparison; `make_forwarder.py` icon | USED-OK | yes | |
| T28 | wgpu-native | v29 headers vendored (`runtime/src/gs/gpu/vk/include/webgpu`); release binary version: UNKNOWN beyond "29" | `C:\ps2-native-recomp-main\third_party\wgpu-native` | PC reference renderer (`build/pc-wgpu`) to compare the shim and HW path; `PS2_GPU_BACKEND=vulkan` makes it use Vulkan | USED-OK | for PC comparisons | `F:\ps22switch\third_party\` does **not exist**; `build/pc-t4` and `build/pc-vk` caches point at it (harmless for those builds). |
| T29 | SDL2 (Windows) | UNKNOWN | `C:\ps2-native-recomp-main\third_party\sdl2` | PC window | USED-OK | PC only | |
| T30 | DXC | UNKNOWN | `C:\ps2-native-recomp-main\third_party\dxc` | D3D12 shader compiler (PC default backend) | AVAILABLE | PC only | PC logs show FXC was used. |
| T31 | Windows sampling profiler `PS2_PROFILE` (`host/profile_win.cpp`) | upstream | runtime | `scratch/wprof.txt` (CPU profile of software renderer run) | USED-OK | diagnostic | |
| T32 | `PS2_GPU_PROFILE=start,end,file` | upstream | runtime | `scratch/p6.txt` per-dispatch GPU log | USED-OK | diagnostic | |
| T33 | Test GPU: AMD Radeon RX 6800 XT | driver 26.10.1 / Vulkan 1.4.315 | the PC | all PC pixel comparisons | USED-OK | yes | Results on other GPUs: UNKNOWN. |

## D. Project scripts and executables

| ID | Tool | Location | Purpose | Status | Notes |
|---|---|---|---|---|---|
| T34 | `eerecomp.exe`, `vurecomp.exe` (+ `vujit_dump`) | `build/tools/` (built 2026-10-29) | MIPS R5900 → C++ and VU1 → C++ translators (upstream `recomp/`) | USED-OK (by port.sh, for the generated Tekken 4 code) | Built by `scripts/setup.sh --native-only`. |
| T35 | `scripts/port.sh` | repo | upstream pipeline: extract disc, translate, exploring runs, VU1 compile, native build | USED-OK (indirectly; Tekken 4's generated code dates 2026-10-29 17:09, made in the upstream C: folder and copied) | How `generated/Tekken-4` reached F: — copy from `C:\ps2-native-recomp-main\generated\Tekken-4` (same timestamps); exact command UNKNOWN. |
| T36 | `scripts/switch_port.py` | repo | Switch build + SD package | USED-OK | Verified command: `python scripts/switch_port.py "C:/ps2-native-recomp-main/games/Tekken-4/disc.iso" --name Tekken-4 --title "Tekken 4" --skip-port --no-disc-copy`. |
| T37 | `Switch_Build.cmd` | repo | drag-and-drop wrapper for switch_port.py | NOT USED in the project | Uses `py -3` when available (broken here). |
| T38 | `scripts/wgsl_spirv.py` | repo | WGSL → SPIR-V table | USED-OK (in WSL) | |
| T39 | `scratch/bench.sh` | scratch (git-ignored) | scripted Tekken 4 run (power-on → fight, 4200 frames, a frame every 100) with a copied memory card and fixed RTC 1793310853 | USED-OK | Reads the disc and card from `C:\ps2-native-recomp-main\games\Tekken-4`. Copy in `references/additional_documentation/`. |
| T40 | `scratch/cmp.py` | scratch | compare two folders of `.ppm` frames pixel-wise | USED-OK | prints `same N different M`. |
| T41 | Forwarder (`tools/forwarder/`: `src/` nx-hbloader-based loader, `forwarder.nso`, `forwarder.npdm`, `make_forwarder.py` GPL-3.0) | `F:\ps22switch\tools\forwarder` (git-ignored) | Home-menu `.nsp` that starts `sdmc:/switch/ps22switch/Tekken-4/Tekken-4.nro`; title ID `0x0500544B4E340000` | USED (built `Tekken 4.nsp`), **untested on console** | Built in WSL: `make -C /tmp/fw RELEASE=1`. Needs the user's `prod.keys` (Lockpick_RCM dump). Pure-Python AES. |
| T42 | Upstream app (`Start.cmd`, `app/app.ps1`, `app/ui.cs`, `app/get-python.ps1`) | repo | Windows GUI for PC builds | AVAILABLE, not used for Switch | |
| T43 | `scripts/autotests.py` (ps2autotests) | repo | upstream conformance suite (README: 54/55 pass) | NOT RUN during Switch work | Status on the current branch: UNKNOWN. |
| T44 | Other upstream scripts (`speedcheck.py`, `report.py`, `replay.py`, `play.py`, `pnach.py`, `mcformat.py`, ...) | repo | PC tooling | AVAILABLE | See FILE_INVENTORY. |

## E. Hardware, console software, services

| ID | Tool | Purpose | Status | Notes |
|---|---|---|---|---|
| T45 | Nintendo Switch 1 (Tegra X1, GPU GM20B), real hardware | all Switch tests | USED-OK | Firmware version, CFW (Atmosphère?) version: UNKNOWN — not verified. Homebrew requires CFW. |
| T46 | Homebrew Menu (hbmenu), title takeover (hold R on a game) | starts the .nro with full memory | USED-OK | Album applet gives less memory (heap reserve drops to 128 MB). |
| T47 | sys-clk / Horizon OC | external clock managers | INVESTIGATED (interplay coded) | If running, their profile wins unless `PS2_CLOCK_FORCE=1`. Whether the user has it: UNKNOWN. |
| T48 | DBI / Tinfoil | install the forwarder NSP (needs signature patches) | SUGGESTED, not confirmed | |
| T49 | Lockpick_RCM | dumps `prod.keys` | user's tool (input only) | `prod.keys` is SENSITIVE. |
| T50 | GitHub (repos, releases API, `curl`, `git clone --depth 1`) | download NVK SDK, read OpenTrickyNX / NetherSX2_nx / ARMSX2-NX, push branch | USED-OK | Remote `https://github.com/DevCrashDay/ps22switch`. |
| T51 | Claude Code (agent) + Claude project threads | did the development; logs exchanged as thread attachments | USED | Transcript stays on the PC (`C:\Users\Christopher Schimpf\.claude\projects\F--ps22switch\`). |
| T52 | PowerShell 5.1 | runs WSL scripts without MSYS path mangling | USED-OK | |

## F. Reference projects and approaches

| ID | Project / approach | What was taken | Status |
|---|---|---|---|
| T53 | OpenTrickyNX (`github.com/DevCrashDay/OpenTrickyNX`) | NVK SDK release, linking recipe (SDL2 EGL/glapi swap), clock method, forwarder | USED (ideas + SDK + forwarder code) |
| T54 | NetherSX2_nx (`github.com/NaGaa95/NetherSX2_nx`) | GPU heap reserve 512 MB, `__nx_nv_transfermem_size` 16 MB (re-written); confirms PCSX2-style HW renderer on NVK works; has Deko3D backend | USED (settings), INVESTIGATED (Deko3D) |
| T55 | ARMSX2-NX (`github.com/HayatoG/ARMSX2-NX`) | nothing (README says not playable) | INVESTIGATED, rejected |
| T56 | Deko3D | possible future backend bypassing Mesa | INVESTIGATED, not started |
| T57 | OpenGL via devkitPro Mesa | first proposed for a GPU renderer | REJECTED in favour of Vulkan/NVK (coordinator/user goal) |
| T58 | PCSX2 hardware renderer | design model for `hw.wgsl` (targets, fixed-function blending, texture cache idea) | DESIGN REFERENCE (no code copied) |
| T59 | Play! emulator | VLC tables of the MPEG decoder (upstream, `runtime/src/ipu/NOTICE`, BSD-2) | upstream dependency |
| T60 | Emulators (Ryujinx/yuzu) | not used; user confirmed tests are on real hardware | NOT USED |

Total documented: **60 tools/approaches** (T01–T60).
