#!/usr/bin/env python3
"""ps22switch: turn a PS2 disc image you own into a Nintendo Switch homebrew (.nro).

Usage:
  python scripts/switch_port.py <game.iso | game_name> [options]

Examples:
  python scripts/switch_port.py "Tekken 4 (USA).iso"
  python scripts/switch_port.py "Tekken 4 (USA).iso" --name Tekken-4 --widescreen
  python scripts/switch_port.py Tekken-4 --skip-port

Steps:
  1. scripts/port.sh --native-only on this PC: extracts the boot program, translates it to C++, runs it headless to find the code
     the static pass missed (the Switch build has no compiler to fall back on, so every entry point has to be found here), and
     compiles the vector unit programs those runs recorded. Its output is generated/<name> and games/<name>.
  2. The same C++ cross-compiled with devkitA64 into an .nro (in the devkitA64 WSL distribution on Windows, directly on Linux).
  3. An SD card folder, build/switch/<name>/package/switch/ps22switch/<name>, with the .nro, the boot program, the disc image and
     launch.txt (the command line the .nro starts the game with, see host/main.cpp).
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
GAMES_DIR = ROOT / "games"
GENERATED_DIR = ROOT / "generated"
SWITCH_OUT_DIR = ROOT / "build" / "switch"
WINDOWS = os.name == "nt"


def run(cmd, capture=False, cwd=None):
    """Run a command, echoing it; stops the script when it fails. With capture, its output is shown and returned too."""
    print(">> " + " ".join(str(c) for c in cmd), flush=True)
    if not capture:
        code = subprocess.run(cmd, cwd=cwd).returncode
        if code != 0:
            sys.exit(f"command failed with exit code {code}")
        return ""
    lines = []
    with subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace") as p:
        for line in p.stdout:
            print(line, end="", flush=True)
            lines.append(line)
    if p.returncode != 0:
        sys.exit(f"command failed with exit code {p.returncode}")
    return "".join(lines)


def find_bash() -> str:
    """The MSYS bash port.sh runs in on Windows (never WSL's bash.exe in System32, which would run it in Linux)."""
    if not WINDOWS:
        return "bash"
    candidates = [ROOT / "tools" / "portable" / "mingit" / "usr" / "bin" / "bash.exe"]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"), os.environ.get("LOCALAPPDATA")):
        if base:
            candidates += [Path(base) / "Git" / "bin" / "bash.exe", Path(base) / "Programs" / "Git" / "bin" / "bash.exe"]
    for c in candidates:
        if c.is_file():
            return str(c)
    found = shutil.which("bash")
    if found and "system32" not in found.lower():
        return found
    sys.exit("Git Bash not found: install Git for Windows (port.sh runs in its bash), or run Start.cmd once to fetch the portable tools")


def posix(path: Path) -> str:
    """C:\\dir\\file as C:/dir/file, the form MSYS bash takes."""
    return str(path.resolve()).replace("\\", "/")


def wsl_path(path: Path) -> str:
    """A Windows path as WSL sees it (/mnt/c/...)."""
    p = str(path.resolve()).replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        return f"/mnt/{p[0].lower()}{p[2:]}"
    return p


def boot_program(game_dir: Path) -> str:
    """The program SYSTEM.CNF boots (BOOT2 = cdrom0:\\SLUS_203.28;1 -> SLUS_203.28), the same way port.sh reads it."""
    cnf = game_dir / "disc" / "SYSTEM.CNF"
    if not cnf.is_file():
        return ""
    for line in cnf.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = re.match(r"\s*BOOT2\s*=\s*(.*)", line)
        if m:
            program = m.group(1).strip()
            program = program.split("\\")[-1].split("/")[-1].split(":")[-1]
            return program.split(";")[0]
    return ""


def port(bash: str, name: str, iso: Path, args) -> list:
    """scripts/port.sh for the native side; returns the arguments of the "Play it natively" line it ends with."""
    cmd = [bash, posix(SCRIPTS / "port.sh"), name, posix(iso), "--native-only"]
    if args.no_explore:
        cmd.append("--no-explore")
    if args.frames:
        cmd += ["--frames", str(args.frames)]
    if args.rounds:
        cmd += ["--rounds", str(args.rounds)]
    for p in args.pnach:
        cmd += ["--pnach", posix(Path(p))]
    for s in args.pnach_section:
        cmd += ["--pnach-section", s]
    if args.widescreen:
        cmd.append("--widescreen")
    out = run(cmd, capture=True, cwd=ROOT)
    for line in out.splitlines():
        if line.startswith("Play it natively:"):
            return shlex.split(line.split(":", 1)[1])[1:]  # without the ps2run path
    return []


def launch_arguments(name: str, game_dir: Path, iso: Path, play: list, widescreen: bool):
    """(program, disc image, patch file, widescreen) for the .nro: from port.sh's play line, else from what is in the game folder."""
    program = disc = patches = None
    wide = widescreen
    if play:
        program = ROOT / play[0]
        it = iter(play[1:])
        for a in it:
            if a == "--disc":
                disc = Path(next(it))
            elif a == "--patches":
                patches = ROOT / next(it)
            elif a == "--widescreen":
                wide = True
        if disc is not None and not disc.is_absolute():
            disc = ROOT / disc
    if program is None or not program.is_file():
        boot = boot_program(game_dir)
        for c in ([game_dir / "patched" / boot] if boot else []) + [game_dir / "core.elf"] + ([game_dir / "disc" / boot] if boot else []):
            if c.is_file():
                program = c
                break
    if program is None or not program.is_file():
        sys.exit(f"no boot program in {game_dir}: run without --skip-port first")
    if disc is None or not disc.is_file():
        disc = iso if iso and iso.is_file() else game_dir / "disc.iso"
    if not disc.is_file():
        sys.exit(f"no disc image for {name}: give the .iso (the game reads its files from it while it runs)")
    if patches is None and (game_dir / "patches.txt").is_file():
        patches = game_dir / "patches.txt"
    return program, disc, patches, wide


def build_nro(name: str, gen_dir: Path, args) -> Path:
    """generated/<name> and the runtime cross-compiled with devkitA64; returns the .nro in build/switch/<name>."""
    out_dir = SWITCH_OUT_DIR / name
    out_dir.mkdir(parents=True, exist_ok=True)
    def defines(to_path):
        d = [f"-DPS2_GENERATED={to_path(gen_dir)}", "-DCMAKE_BUILD_TYPE=Release",
             f"-DSWITCH_TITLE={args.title}", f"-DSWITCH_AUTHOR={args.author}", f"-DSWITCH_VERSION={args.version}"]
        if args.icon:
            d.append(f"-DSWITCH_ICON={to_path(Path(args.icon))}")
        return d

    if WINDOWS:
        # the build folder stays inside WSL: compiling over /mnt/<drive> is many times slower
        build = f"/tmp/switch-build-{re.sub(r'[^A-Za-z0-9_.-]', '_', name).lower()}"
        cfg = ["/usr/local/bin/switch-cmake", "-G", "Ninja", "-B", build, "-S", wsl_path(ROOT)] + defines(wsl_path)
        script = " ".join(shlex.quote(c) for c in cfg)
        script += f" && /usr/local/bin/switch-ninja -C {shlex.quote(build)} ps2run_nro"
        script += f" && cp {shlex.quote(build)}/ps2run.nro {shlex.quote(wsl_path(out_dir))}/{shlex.quote(name)}.nro"
        script += f" && cp {shlex.quote(build)}/ps2run.elf {shlex.quote(wsl_path(out_dir))}/{shlex.quote(name)}.elf"
        run(["wsl", "-d", args.wsl_distro, "-u", "root", "--", "bash", "-c", script])
    else:
        devkitpro = os.environ.get("DEVKITPRO", "/opt/devkitpro")
        build = ROOT / "build" / f"switch-{name}"
        run(["cmake", "-G", "Ninja", "-B", str(build), "-S", str(ROOT), f"-DCMAKE_TOOLCHAIN_FILE={devkitpro}/cmake/Switch.cmake"]
            + defines(lambda p: str(p.resolve())))
        run(["ninja", "-C", str(build), "ps2run_nro"])
        shutil.copy2(build / "ps2run.nro", out_dir / f"{name}.nro")
        shutil.copy2(build / "ps2run.elf", out_dir / f"{name}.elf")
    nro = out_dir / f"{name}.nro"
    if not nro.is_file():
        sys.exit(f"no .nro came out: {nro}")
    print(f">> {nro} ({nro.stat().st_size // (1024 * 1024)} MB)")
    return nro


def package(name: str, nro: Path, game_dir: Path, program: Path, disc: Path, patches, wide: bool, copy_disc: bool) -> Path:
    """sdmc:/switch/ps22switch/<name>/: the .nro, launch.txt and the files it names, with the folder layout launch.txt expects."""
    pkg = SWITCH_OUT_DIR / name / "package" / "switch" / "ps22switch" / name
    pkg.mkdir(parents=True, exist_ok=True)
    shutil.copy2(nro, pkg / nro.name)
    (pkg / "disc").mkdir(exist_ok=True)
    shutil.copy2(program, pkg / "disc" / program.name)
    launch = ["# ps2run's command line, one argument to a line (host/main.cpp); paths are relative to this folder",
              f"disc/{program.name}"]
    if copy_disc:
        if not (pkg / "disc.iso").is_file() or (pkg / "disc.iso").stat().st_size != disc.stat().st_size:
            print(f">> copying the disc image ({disc.stat().st_size // (1024 * 1024)} MB)")
            shutil.copyfile(disc, pkg / "disc.iso")
    else:
        print(f">> put the disc image next to the .nro as disc.iso yourself: {disc}")
    launch += ["--disc", "disc.iso", "--memcard", f"{name}.ps2"]  # without a card file the game's saves would not be kept
    if patches:
        shutil.copy2(patches, pkg / "patches.txt")
        launch += ["--patches", "patches.txt"]
    if wide:
        launch.append("--widescreen")
    (pkg / "launch.txt").write_text("\n".join(launch) + "\n", encoding="utf-8", newline="\n")
    keys = game_dir / "keys.ini"
    if keys.is_file():
        shutil.copy2(keys, pkg / "keys.ini")
    root = SWITCH_OUT_DIR / name / "package"
    print(f">> SD card folder: {pkg}")
    print(f">> copy the 'switch' folder inside {root} to the root of the SD card and start {nro.name} from the homebrew menu")
    print(">> start the homebrew menu by holding R while starting a game (title takeover): the album applet gives homebrew less memory")
    return pkg


def main():
    parser = argparse.ArgumentParser(description="Recompile a PS2 game into Nintendo Switch homebrew")
    parser.add_argument("target", help="the game's disc image (.iso, .zip, .7z), or the name of a game already in games/")
    parser.add_argument("--name", default="", help="folder name under games/ (default: the image's name without (USA) and the like)")
    parser.add_argument("--title", default="", help="name in the homebrew menu")
    parser.add_argument("--author", default="ps22switch", help="author in the homebrew menu")
    parser.add_argument("--version", default="1.0.0", help="version in the homebrew menu")
    parser.add_argument("--icon", default="", help="256x256 JPEG for the homebrew menu")
    parser.add_argument("--skip-port", "--skip-recomp", dest="skip_port", action="store_true",
                        help="use generated/<name> as it is and only build and package for the Switch")
    parser.add_argument("--no-explore", action="store_true", help="port.sh without the exploring runs (not advised: see the top of this file)")
    parser.add_argument("--frames", type=int, default=0, help="frames per exploring run (port.sh's default when left out)")
    parser.add_argument("--rounds", type=int, default=0, help="exploring runs at most (port.sh's default when left out)")
    parser.add_argument("--pnach", action="append", default=[], help="PCSX2 patch file, as for port.sh")
    parser.add_argument("--pnach-section", action="append", default=[], help="patch section to enable, as for port.sh")
    parser.add_argument("--widescreen", action="store_true", help="tell the game the TV is 16:9")
    parser.add_argument("--no-disc-copy", action="store_true", help="do not copy the disc image into the SD card folder")
    parser.add_argument("--wsl-distro", default="devkita64", help="WSL distribution with devkitA64 and switch-cmake (Windows)")
    args = parser.parse_args()

    target = Path(args.target)
    iso = None
    if target.is_file():
        iso = target.resolve()
        stem = iso.stem
        name = args.name or re.sub(r"\s*\([^)]*\)", "", stem).strip().replace(" ", "-") or stem.replace(" ", "-")
        title = args.title or re.sub(r"\s*\([^)]*\)", "", stem).strip() or stem
    else:
        name = args.name or args.target.replace(" ", "-")
        title = args.title or name.replace("-", " ")
    args.title = title
    game_dir = GAMES_DIR / name
    gen_dir = GENERATED_DIR / name

    print("==================================================")
    print(f" ps22switch: {title} for the Nintendo Switch")
    print("==================================================")

    play = []
    if not args.skip_port:
        if iso is None and not (game_dir / "disc.iso").exists():
            sys.exit(f"{args.target}: no such disc image (give the .iso, or --skip-port for a game already ported)")
        play = port(find_bash(), name, iso or game_dir / "disc.iso", args)
    if not list(gen_dir.glob("*.cpp")):
        sys.exit(f"no generated C++ in {gen_dir}: run without --skip-port first")

    program, disc, patches, wide = launch_arguments(name, game_dir, iso, play, args.widescreen)
    nro = build_nro(name, gen_dir, args)
    package(name, nro, game_dir, program, disc, patches, wide, not args.no_disc_copy)
    print(f"\n[DONE] {title} -> {nro.name}")


if __name__ == "__main__":
    main()
