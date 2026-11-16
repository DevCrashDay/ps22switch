# Build environment and reproducibility

Labels: **[VERIFIED]** = this exact command ran successfully during the project (seen in the session transcript or its output is on
disk); **[UNTESTED]** = written from the scripts/README, not observed running.

## 1. Reference machine (as of 2026-11-04)

| Item | Value |
|---|---|
| Host OS | Windows 10 Pro 10.0.19044 (machine name `CHRIS`), x86-64 |
| Project path | `F:\ps22switch` (git repo, branch `claude/project-thread-i8q90o`) |
| Upstream copy (holds the PC toolchain, the disc image, recorded inputs) | `C:\ps2-native-recomp-main` (not a git repo) |
| WSL | distro `devkita64`, **WSL 1**, Debian 12, kernel string `4.4.0-19041-Microsoft` |
| Switch toolchain | devkitPro in WSL `/opt/devkitpro`: devkitA64 r29.2-1 (GCC 15.2.0), libnx 4.12.0, switch-sdl2 2.28.5, switch-tools 1.13.1, CMake 3.31.6, Ninja 1.11.1 |
| Vulkan driver for Switch | Mesa NVK SDK `F:\ps22switch\tools\nvk-sdk\portlibs\switch` (mesa-switch rev 0ff52079…) |
| Shader converter | naga-cli 29.0.4 at WSL `/root/naga29/bin/naga` (naga 30.0.1 also at `/root/.cargo/bin/naga`, not used anymore) |
| PC compiler | llvm-mingw clang++ 23.1.1, CMake 4.4.4, Ninja: `C:\ps2-native-recomp-main\tools\portable\{llvm-mingw,cmake,ninja}` |
| PC GPU | AMD Radeon RX 6800 XT (Vulkan 1.4.315) |
| Python | Windows 3.11.7 + Pillow 10.2.0; WSL 3.11.2 |
| Target | Nintendo Switch 1 (Tegra X1), custom firmware with hbmenu (version UNKNOWN) |

## 2. Required directory structure

```
F:\ps22switch\
  generated\Tekken-4\     translated C++ (git-ignored; produced by port.sh; currently copied from C:\ps2-native-recomp-main\generated\Tekken-4)
  games\Tekken-4\          disc\SYSTEM.CNF, disc\SLUS_203.28, vu1\*.bin, keys.ini, app.json (git-ignored)
  tools\nvk-sdk\portlibs\switch\   NVK SDK (git-ignored; download)
  tools\forwarder\         only for the NSP forwarder (git-ignored)
  build\                   outputs (git-ignored)
C:\ps2-native-recomp-main\games\Tekken-4\disc.iso   the Tekken 4 (USA) disc image, 2 698 346 496 bytes (user-supplied, never commit)
```

## 3. Setting up from scratch [UNTESTED as a whole; individual steps marked]

1. **Windows tools for the recompiler (PC side).** Either run upstream `Start.cmd` once (downloads portable tools into
   `tools\portable`) or `scripts\bootstrap.ps1` (winget: llvm-mingw UCRT, CMake, Ninja, Python 3, Git), then in Git Bash
   `bash scripts/setup.sh --native-only` (builds `build/tools/eerecomp.exe`, `vurecomp.exe`) [UNTESTED here: the existing
   `build/tools` was built 2026-10-29 with the C: portable tools].
   On the reference PC the tools live in `C:\ps2-native-recomp-main\tools\portable`; put them on the Git Bash PATH:
   ```bash
   P=/c/ps2-native-recomp-main/tools/portable; export PATH="$P/llvm-mingw/bin:$P/cmake/bin:$P/ninja:$PATH"
   ```
   [VERIFIED]
2. **WSL distro with devkitPro.** Create a Debian WSL distro named `devkita64`, install devkitPro pacman, then
   `dkp-pacman -S switch-dev switch-sdl2` (package names per devkitPro docs) and `apt install cmake ninja-build python3`
   [UNTESTED — the distro already existed; how it was set up is UNKNOWN].
   Create the two wrappers (exact contents as found on the reference machine) [VERIFIED present]:
   ```bash
   # /usr/local/bin/switch-cmake
   #!/bin/bash
   export DEVKITPRO=/opt/devkitpro
   export DEVKITA64=/opt/devkitpro/devkitA64
   export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin
   exec /usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake "$@"
   ```
   ```bash
   # /usr/local/bin/switch-ninja
   #!/bin/bash
   export DEVKITPRO=/opt/devkitpro
   export DEVKITA64=/opt/devkitpro/devkitA64
   export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin
   exec /usr/bin/ninja "$@"
   ```
   (`chmod +x` both.)
3. **NVK SDK** [VERIFIED 2026-10-29]:
   ```bash
   mkdir -p /f/ps22switch/tools/nvk-sdk && cd /f/ps22switch/tools/nvk-sdk && curl -sL -o mesa-switch-nvk-sdk.zip https://github.com/DevCrashDay/OpenTrickyNX/releases/download/nvk-sdk/mesa-switch-nvk-sdk.zip && unzip -q -o mesa-switch-nvk-sdk.zip && cat portlibs/switch/MESA_REVISION
   ```
4. **naga 29 in WSL** (only needed when `gs.wgsl` or `hw.wgsl` change) [VERIFIED]:
   ```bash
   curl https://sh.rustup.rs -sSf | sh -s -- -y        # [UNTESTED wording; rustup was installed this way or similar]
   cargo install naga-cli --version '^29' --root /root/naga29
   /root/naga29/bin/naga --version                      # 29.0.4
   ```
5. **Python packages (Windows):** `pip install pillow` (for `cmp.py` and the forwarder) [UNTESTED as a command; Pillow 10.2.0 present].

## 4. Producing the translated game code (PC)

Tekken 4's `generated/Tekken-4` (75 units + table + 3 VU1 files) was produced by upstream's pipeline on 2026-10-29 (17:09 for EE
units, 22:53 for VU1 files) and is present. To regenerate (also runs exploring runs; takes "several minutes") [UNTESTED in this repo]:
```bash
bash scripts/port.sh Tekken-4 "/c/ps2-native-recomp-main/games/Tekken-4/disc.iso" --native-only
```
`switch_port.py` without `--skip-port` does this step itself.

## 5. Building the Switch .nro

The command used for every Switch build in the project [VERIFIED, Git Bash or cmd in `F:\ps22switch`]:
```bash
python scripts/switch_port.py "C:/ps2-native-recomp-main/games/Tekken-4/disc.iso" --name Tekken-4 --title "Tekken 4" --skip-port --no-disc-copy
```
What it does (`scripts/switch_port.py` `build_nro`): in WSL as root,
```bash
/usr/local/bin/switch-cmake -G Ninja -B /tmp/switch-build-tekken-4 -S /mnt/f/ps22switch \
  -DPS2_GENERATED=/mnt/f/ps22switch/generated/Tekken-4 -DCMAKE_BUILD_TYPE=Release \
  -DSWITCH_TITLE="Tekken 4" -DSWITCH_AUTHOR=ps22switch -DSWITCH_VERSION=1.0.0 \
  -DPS2_VULKAN=ON -DNVK_SDK=/mnt/f/ps22switch/tools/nvk-sdk/portlibs/switch
/usr/local/bin/switch-ninja -C /tmp/switch-build-tekken-4 ps2run_nro
cp /tmp/switch-build-tekken-4/ps2run.nro /mnt/f/ps22switch/build/switch/Tekken-4/Tekken-4.nro
cp /tmp/switch-build-tekken-4/ps2run.elf /mnt/f/ps22switch/build/switch/Tekken-4/Tekken-4.elf
```
then packages `build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/` (copies .nro, `disc/SLUS_203.28`, writes `launch.txt`,
copies `games/Tekken-4/keys.ini`). With `--no-disc-copy` you place `disc.iso` yourself (already present in the package folder).

Effective compiler flags (from `/tmp/switch-build-tekken-4/CMakeCache.txt`) [VERIFIED]:
`-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__`,
Release `-g -O2 -DNDEBUG`, plus `-ffp-contract=off` (CMakeLists) and `-std=c++20`. Link: NVK libs in a `--start-group`
(`libvulkan.a`, the SDK's `expat`, `zstd`, `z`), `nx stdc++ m -pthread`, `-Wl,-u,vk_icdGetInstanceProcAddr
-Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion`; SDL2's `EGL`/`glapi` replaced by the SDK's, `drm_nouveau` removed.

Options of `switch_port.py`: `--name`, `--title`, `--author` (ps22switch), `--version` (1.0.0), `--icon`, `--skip-port`,
`--no-explore`, `--frames`, `--rounds`, `--pnach`, `--pnach-section`, `--widescreen`, `--no-disc-copy`, `--nvk-sdk`,
`--software` (software renderer, `-DPS2_VULKAN=OFF`), `--wsl-distro` (devkita64).

**After every `switch_port.py` run, re-add the renderer switches to the package `launch.txt`** (the script writes only the
clock lines; without `PS2_GS_HW=1` the slow compute renderer is used — BUG-002) [VERIFIED practice in the project]:
```bash
L=build/switch/Tekken-4/package/switch/ps22switch/Tekken-4/launch.txt; printf 'PS2_GS_HW=1\nPS2_PRESENT_LOG=1\n' >> $L
```

Incremental rebuild only (no re-configure) [VERIFIED pattern]:
```powershell
wsl -d devkita64 -u root -- bash -c "/usr/local/bin/switch-ninja -C /tmp/switch-build-tekken-4 ps2run_nro"
```
Note: Git Bash may rewrite `/usr/...` or `/opt/...` arguments when calling `wsl`; run WSL commands from PowerShell, or put them in a
`.sh` file and run `wsl -d devkita64 -u root -- bash <wslpath of file>`, or prefix `MSYS_NO_PATHCONV=1`.

## 6. Regenerating SPIR-V after a shader change [VERIFIED]

In WSL (CMake refuses to configure otherwise: "spirv_table.cpp was not made from this gs.wgsl"):
```bash
cd /mnt/f/ps22switch && python3 scripts/wgsl_spirv.py --naga /root/naga29/bin/naga
```

## 7. PC builds for verification (Windows, Git Bash)

Three configured build folders exist [VERIFIED configured; reconfigure commands below are UNTESTED reconstructions from the caches]:

| Folder | Renderer | Key cache values |
|---|---|---|
| `build/pc-vk` | the Vulkan shim on the PC GPU | `PS2_VULKAN=ON`, `PS2_VULKAN_INCLUDE=F:/ps22switch/tools/nvk-sdk/portlibs/switch/include`, `PS2_GENERATED=F:/ps22switch/generated/Tekken-4`, Release, clang++ from C: portable |
| `build/pc-wgpu` | wgpu-native (reference) | `PS2_VULKAN=OFF`, `PS2_WGPU_DIR=C:/ps2-native-recomp-main/third_party/wgpu-native` |
| `build/pc-t4` | software/plain build of 2026-10-29 | `PS2_WGPU_DIR=F:/ps22switch/third_party/wgpu-native` (missing folder → no GPU renderer) |

Rebuild [VERIFIED]:
```bash
P=/c/ps2-native-recomp-main/tools/portable; export PATH="$P/llvm-mingw/bin:$P/cmake/bin:$P/ninja:$PATH"; ninja -C build/pc-vk ps2run
```
Re-configure from scratch [UNTESTED]:
```bash
cmake -G Ninja -B build/pc-vk -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/ps2-native-recomp-main/tools/portable/llvm-mingw/bin/clang++.exe -DPS2_GENERATED=F:/ps22switch/generated/Tekken-4 -DPS2_VULKAN=ON -DPS2_VULKAN_INCLUDE=F:/ps22switch/tools/nvk-sdk/portlibs/switch/include
```

## 8. Running

**On the Switch** [VERIFIED by the user]: copy `build/switch/Tekken-4/package/switch/` to the SD card root (do **not** copy
`prod.keys`; delete it from the package first), add `PS2_GS_HW=1` to `launch.txt`, start hbmenu by holding R while launching a
game (title takeover), start `Tekken-4.nro`. Logs: `sdmc:/switch/ps22switch/Tekken-4/ps2run.log`.

`launch.txt` format (host/main.cpp): one argument per line, paths relative to the folder, `#` comments, `NAME=value` lines become
environment variables (applied before static init). Current package file:
```
# ps2run's command line, one argument to a line (host/main.cpp); paths are relative to this folder
disc/SLUS_203.28
--disc
disc.iso
--memcard
Tekken-4.ps2
# clocks while the game runs, in MHz (0 keeps the system's; sys-clk's profile wins unless PS2_CLOCK_FORCE=1)
PS2_CPU_MHZ=1785
PS2_GPU_MHZ=921.6
PS2_MEM_MHZ=1600
#PS2_CLOCK_FORCE=1
```
Useful lines: `PS2_GS_HW=1` (hardware renderer — needed for speed), `PS2_PRESENT_LOG=1`, `PS2_VK_PROFILE=1`, `PS2_PARTS=1`,
`PS2_VK_STATS=1`, `PS2_SWITCH_CORES=e,v,w`, `PS2_VK_IN_FLIGHT=n`, `PS2_PRE_REORDER=0`, `PS2_HW_MERGE=0`, `PS2_HW_DECODE=1`,
`PS2_VK_ROBUST=1`, `PS2_CLOCK_FORCE=1`. Never `NAK_DEBUG=...`.

**Ready-made test packages:** `build/switch/tests/<A..G>/` each hold `Tekken-4.nro` + `launch.txt`; copy both over the SD files
(see `references/additional_documentation/switch_test_builds/README.txt`).

**On the PC (scripted Tekken 4 run)** [VERIFIED]:
```bash
GS=gpu bash scratch/bench.sh build/pc-wgpu/ps2run.exe /f/ps22switch/scratch/wgpu 4200             # compute reference (wgpu-native)
PS2_GS_HW=1 GS=gpu bash scratch/bench.sh build/pc-vk/ps2run.exe /f/ps22switch/scratch/hwvk 4200    # HW path through the shim
PS2_GS_HW=1 PS2_GPU_BACKEND=vulkan GS=gpu bash scratch/bench.sh build/pc-wgpu/ps2run.exe /f/ps22switch/scratch/hw 4200  # HW path on wgpu-native
cd scratch && python cmp.py wgpu vk      # prints "same N different M"
```
`bench.sh` writes `log.txt` and 42 `.ppm` frames per folder (every 100 frames), using inputs/card from
`C:\ps2-native-recomp-main\games\Tekken-4\states\run_2026-10-29_22-54-13_f000000.inputs.ps2` and `--rtc 1793310853`.
Add `PS2_VK_PROFILE=1` for GPU timing, `PS2_VK_STATS=1` for shader stats.

PC run of the game by hand (upstream style) [UNTESTED with these exact paths]:
```bash
build/pc-vk/ps2run.exe games/Tekken-4/disc/SLUS_203.28 --disc /c/ps2-native-recomp-main/games/Tekken-4/disc.iso --window --memcard tekken4.ps2
```

## 9. Tests

- Pixel comparison: section 8 (`bench.sh` + `cmp.py`). This is the project's regression test.
- Upstream unit-ish tests: `tests/*.py`, `tests/stretch_test.cpp`, `python scripts/autotests.py` (needs ps2autotests) — not run
  during the Switch work [UNTESTED].

## 10. Debugging

- Switch: read `ps2run.log`; enable `PS2_PRESENT_LOG`, `PS2_VK_PROFILE`, `PS2_PARTS`, `PS2_VK_STATS` (writes `vk_*.txt` next to the
  .nro). No debugger/GDB stub was used (devkitA64-gdb is installed; nxlink/GDB over network: UNTESTED). Crash dumps: Atmosphère
  crash reports on the SD (location/format not used in project — UNKNOWN).
- PC: `PS2_PROFILE` sampling profiler (Windows), `PS2_GPU_PROFILE=start,end,file`, `PS2_VK_VALIDATE` (shim validation layers;
  UNTESTED), `PS2_GS_COMPARE`.

## 11. Cleaning

```bash
rm -rf build/pc-vk build/pc-wgpu build/pc-t4      # PC outputs (reconfigure afterwards)           [UNTESTED]
wsl -d devkita64 -u root -- rm -rf /tmp/switch-build-tekken-4                                       # [UNTESTED]
rm -rf build-switch                                # stale, unused folder                           [UNTESTED]
```
Do **not** delete `generated/`, `games/`, `tools/` or `build/switch/tests` unless you can regenerate them; `scratch/` holds the PC
reference frames used for comparisons.
