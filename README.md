# ps22switch: Recompiling PS2 Games for Nintendo Switch

Turns the disc image of a PS2 game you own into a native, standalone homebrew executable (`.nro`) for the **Nintendo Switch (Switch 1)**, or into a native program for Windows / Linux.

Based on [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp) and [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) by Samir Nuri.

---

## How It Works on Nintendo Switch

1. **CPU Static Recompilation (Emotion Engine MIPS R5900):**
   The game's MIPS R5900 machine code is analyzed and statically translated once into portable modern C++20 units via `eerecomp`. These units are cross-compiled directly for the Nintendo Switch ARMv8-A Cortex-A57 64-bit CPU cores using `devkitA64` (`aarch64-none-elf-gcc`). There is **zero CPU emulation overhead** at runtime.

2. **Vector Units (VU0 / VU1):**
   The VU1 programs the exploring runs on the PC recorded are statically translated to C++ (vector code that GCC turns into ARM NEON). A program the build does not know is interpreted: the Switch has no compiler to translate it while the game runs, so the more of the game the PC runs saw, the faster it plays.

3. **Graphics Synthesizer (GS):**
   Drawn on the Switch's GPU with Vulkan through Mesa's NVK driver (the same driver build OpenTrickyNX uses). The renderer is
   the PC's own WebGPU drawing program: `runtime/src/gs/gpu/vk` implements the WebGPU calls it makes on Vulkan, and
   `scripts/wgsl_spirv.py` turns its shader into SPIR-V with naga (run it after changing `gs.wgsl`). On the PC the same
   layer draws Tekken 4 pixel for pixel like wgpu-native (`-DPS2_VULKAN=ON -DPS2_VULKAN_INCLUDE=<folder with vulkan/vulkan.h>`).
   The finished picture is shown through SDL2. `--software` builds with the old software renderer instead (far too slow).

4. **Audio (SPU2):**
   The PS2 SPU2 synthesizer streams 48,000 Hz stereo audio with dynamic time stretching directly into the Nintendo Switch SDL2 audio driver.

5. **Controls:**
   Native support for Joy-Cons and Nintendo Switch Pro Controllers through the libnx SDL2 GameController API.

---

## Quick Start for Nintendo Switch

### Prerequisites
- Windows 10/11 with WSL2 and a distribution named `devkita64` holding devkitPro's devkitA64, libnx and `switch-sdl2`,
  with `switch-cmake` / `switch-ninja` wrappers in `/usr/local/bin` (`--wsl-distro` picks another name). On Linux,
  devkitPro installed directly (`$DEVKITPRO`) is used instead.
- The Mesa NVK SDK for the Switch: `mesa-switch-nvk-sdk.zip` from the `nvk-sdk` release of
  [OpenTrickyNX](https://github.com/DevCrashDay/OpenTrickyNX/releases/tag/nvk-sdk), unzipped into `tools/nvk-sdk`
  (or `--nvk-sdk <its portlibs/switch folder>`).
- What the native build needs on the PC (Git Bash and the translator tools: run `Start.cmd` once, or `scripts/setup.sh --native-only`).
- Python 3

### Building with One Command
1. Place your PS2 game ISO (e.g. `Tekken 4 (USA).iso`) in the project directory.
2. Run `Switch_Build.cmd` or invoke `scripts/switch_port.py`:
   ```cmd
   Switch_Build.cmd "Tekken 4 (USA).iso"
   ```
   Or via Python:
   ```cmd
   python scripts/switch_port.py "Tekken 4 (USA).iso" --title "Tekken 4"
   ```
3. The build pipeline automatically:
   - Runs `scripts/port.sh --native-only` on the PC: extracts the boot program, translates its MIPS R5900 code to C++,
     runs the game headless to find the code the static pass missed and compiles the VU1 programs those runs recorded.
     This step matters more than on the PC: the Switch build cannot compile code it meets while the game runs, so code
     the PC runs never reached stops the game there. `--pnach`, `--widescreen`, `--frames` and `--rounds` are passed on.
   - Cross-compiles `ps2runtime` and the game's C++ for AArch64 with devkitA64 (`-ffp-contract=off`, so the PS2's
     floating point results come out the same as on the PC).
   - Packages the `.nro` with its NACP title and metadata.
   - Creates a ready-to-use SD card folder under `build/switch/<GameName>/package/`, with the disc image copied in
     (`--no-disc-copy` leaves that to you) and `launch.txt`, the command line the `.nro` starts the game with.
   - `--skip-port` rebuilds the `.nro` from what an earlier run left in `generated/<GameName>`.

### Installing to your Switch
Copy the `switch` folder from `build/switch/<GameName>/package/` to the root of your Switch SD card:
```
sdmc:/
  └── switch/
      └── ps22switch/
          └── <GameName>/
              ├── <GameName>.nro
              ├── launch.txt       the program, disc image and options, one argument to a line
              ├── disc/<boot program>
              ├── disc.iso         the game reads its files from the disc image while it runs
              ├── patches.txt      when PCSX2 patches were applied
              └── keys.ini
```
Launch the game from the **Homebrew Menu (hbmenu)**, preferably started by holding R while starting a game (title
takeover gives homebrew all of the memory and processor time; the album applet gives less). The game runs on three of
the Switch's cores (emulation, vector units, drawing), its log goes to `ps2run.log` and its saves to `<GameName>.ps2`,
both next to the `.nro`. Without `launch.txt` the `.nro` looks for a boot program and an `.iso` in its folder and in `disc/`.

---

- **Windows:** the recompiler, the runtime and the game build with llvm-mingw and draw through Direct3D 12 or Vulkan.
- **Faster drawing:** the graphics chip's drawing program does about a fifth less work per frame, with the same pictures
  pixel for pixel. On Windows it is compiled with Microsoft's newer shader compiler and drawn through Direct3D 12 by
  default, and the processor side (game, vector units, window) no longer holds the game back.
- **A clearer picture:** a new scaling step with light sharpening keeps text and the HUD crisp, slanted edges are
  smoothed, shadows are solid instead of dotted, and faces move as they should (Silent Hill 2).
- **An app for players:** choose a disc image, press *Build*, press *Play*. It downloads its own build tools, so nothing
  has to be installed.
- **Code translated while you play:** game code and graphics programs the first build did not see are translated in the
  background while the game runs, and kept for the next start.
- **More games:** God of War, Gran Turismo 4 (US), Silent Hill 2, Resident Evil 4 and Shadow of the Colossus run too,
  with fixes in the runtime where they needed them.
- **Playing:** keyboard and mouse controls with a key card, quick saves, sound that is stretched instead of chopped when
  the game runs below full speed, picture styles to switch between, and PCSX2's community patches (widescreen and more)
  to pick in the app.

## What runs

Tried with real discs (mine are US releases), as far as each was played (no known bugs):

| Game | How far | Tested by |
|---|---|---|
| God of War II | **played from start to end**, with a game controller; keyboard and mouse work too, and so do quick saves and the game's own save | petarsti |
| God of War | plays well, not played through yet: from power-on through the menus and the opening film into the first fight on the ship, also in 16:9 | me |
| Gran Turismo 4 | plays well, not played through yet: from power-on through the film and the menus, and a whole race played | me |
| Resident Evil 4 | plays well, not played through yet: the menus, the opening, Leon in the forest | me |
| Silent Hill 2 | plays well, not played through yet: the opening at the rest stop and about 20 minutes of play from there but works fine| me |
| Shadow of the Colossus | plays well, not played through yet: played like 30 minutes that went very well | me |
| The Super Dimension Fortress Macross | plays well, not played through yet: menus and gameplay at about 60 fps (Windows 11, a laptop with Intel graphics, a game controller; [#3](https://github.com/exoteric33/ps2-native-recomp/issues/3)) | omegakatana92 |
| The King of Fighters XI | plays well, not played through yet: works perfectly | TsimpleD (Reddit) |
| God Hand | plays well, not played through yet: as far as the casino hub, no problems so far | theraineydaze (Reddit) |
| Tekken 4 | plays well, not played through yet: fights in Story Battle at 60 fps, only a short stutter when a fight starts (Windows 11, a Radeon RX 6800 XT, keyboard and mouse; [#8](https://github.com/exoteric33/ps2-native-recomp/issues/8)) | DevCrashDay |
| Armored Trooper VOTOMS (Japan) | plays well, not played through yet: the tutorial and the first mission at about 60 fps, and saves work; it slowed down once, the first time a new part loaded (Windows 11, a laptop with Intel graphics, a game controller; [#5](https://github.com/exoteric33/ps2-native-recomp/issues/5)) | omegakatana92 |
| Ben 10: Alien Force | plays well, not played through yet: works very well, no problems so far | Travani |

Known not to work yet (reported by playtesters, on my list to fix):

| Game | What happens | Reported by |
|---|---|---|
| Ben 10: Alien Force - Vilgax Attacks | stops at "Checking memory card" right after the start and goes no further | Travani |

54 of the 55 programs of the free [ps2autotests](https://github.com/unknownbrackets/ps2autotests) suite print exactly
what the console prints (`python scripts/autotests.py`).

Not checked yet: any other game from start to end, macOS since my changes, Windows 10, and every other game.

## Playtesters wanted

I'm looking for people who try this with their own PS2 discs and tell me how it went: which games start, how far they
get, how fast they run, and what looks or sounds wrong. Every report helps, "it just worked" too, and so does every PC I
don't have, from weak laptops to big graphics cards.

1. Build a game you own (see *Get started* below) and play it.
2. When something looks, sounds or behaves wrong, press **F9** right there. It notes the spot, and the game goes on.
3. Open a [playtest report](https://github.com/exoteric33/ps2-native-recomp/issues/new?template=playtest.yml): the game,
   your PC, how far you got, what went wrong, and the text of `log.txt` and `info.txt` from the newest folder in
   `reports\`. If you'd rather not use GitHub, write to me at exoteric54@gmail.com.

Please don't attach disc images, save states or other files from the game; if I need more, I'll ask. Questions, ideas
and fixes are welcome too, as issues, pull requests or by e-mail.

## Get started on Windows

**With the app** (nothing to install): on this page press *Code*, then *Download ZIP*. Unpack all of it into a short
folder such as `C:\PS2Native` and double-click `Start.cmd` (Windows asks once whether to run it: press *Run*). Press
*Add a game*, choose the disc image (an `.iso`, or a `.zip` or `.7z` that holds one), press *Build*, then *Play*. The
first build downloads the build tools it needs (about 312 MB, from their official release pages) into that folder;
building a game takes several minutes. *Make desktop shortcut* puts the game on the desktop.

**From the command line** (for developers):

1. Install llvm-mingw (UCRT), CMake, Ninja, Python 3 and Git for Windows. `scripts\bootstrap.ps1` installs them with
   winget. No Visual Studio is needed.
2. In Git Bash: `bash scripts/setup.sh --native-only`
3. `bash scripts/port.sh mygame "/c/Games/My Game.iso" --native-only`
4. Run the line `port.sh` prints at the end, for example
   `build/native-mygame/ps2run.exe games/mygame/disc/BOOT.ELF --disc games/mygame/disc.iso --window --memcard mygame.ps2`

## Get started on Linux

You need git, clang, CMake, Ninja, Python 3, the SDL2 development files and a Vulkan driver. Then
`bash scripts/setup.sh --native-only` and `bash scripts/port.sh mygame "My Game.iso" --native-only`, and run the line it
prints. Checked on CachyOS with an NVIDIA graphics card.

On macOS the original project builds and runs (on Metal); my changes have not been tried on a Mac yet.

## Playing

- **F1** shows all keys. They can be changed in `keys.ini` (the F1 card has a button for it).
- Keyboard and mouse: **W A S D** is the left stick, the mouse buttons, **Space**, **E**, **Q**, **F** and **Shift** are the
  game's buttons (the F1 card shows which is which), **Esc** is Start.
- A game controller works too (any controller SDL knows).
- **F5** quick save, **F8** quick load, **F11** full screen, **F2** switches between picture styles.
- Twice the resolution: set `PS2_SCALE=2` before starting the game (for a strong graphics card).
- Widescreen and other patches: in the app, *Choose extras...* looks up PCSX2's community patches for the game when you
  ask it to. On the command line: `port.sh --pnach FILE --pnach-section "Widescreen 16:9"`.
- Parts of a game that the first build did not see are translated while you play and kept for the next start. The first
  time, they can run slowly for a few seconds.

## Your own disc

Everyone uses a disc image of their own disc. This repo contains no game code or data and never will: no disc images,
extracted files, generated C++, built game programs, screenshots, recordings or saves. The generated C++ and the built
programs are translations of the game's own code and belong to the game's owners: keep them on your own machine and do
not share them. Please do not ask for disc images here. There is no decryption or copy-protection code in this repo
(PS2 discs need none).

## Credits and license

MIT, see [LICENSE](LICENSE): the original copyright is Samir Nuri's, and my changes are under the same license. The VLC
tables of the MPEG decoder are adapted from [Play!](https://github.com/jpd002/Play-) (BSD 2-clause, notice in
`runtime/src/ipu/NOTICE`). wgpu-native, SDL2 and the build tools are downloaded from their own release pages, not shipped.
PCSX2's patch files are downloaded to your PC when you ask for them, not shipped.

I wrote much of the code with the help of an AI coding assistant (Claude Code) and checked it with the test suite and my
own discs.

PlayStation and PS2 are trademarks of Sony Interactive Entertainment. All game names are trademarks of their owners. This
project is not affiliated with or endorsed by Sony or any game publisher.
