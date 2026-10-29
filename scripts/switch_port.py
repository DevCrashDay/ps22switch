#!/usr/bin/env python3
"""ps22switch: Recompile PS2 games from PS2 ISO/ELF to native Nintendo Switch (.nro) homebrew.

Usage:
  python scripts/switch_port.py <game.iso | game_name> [options]

Examples:
  python scripts/switch_port.py "Tekken 4 (USA).iso"
  python scripts/switch_port.py Tekken-4 --title "Tekken 4"
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
BUILD_DIR = ROOT / "build"
GAMES_DIR = ROOT / "games"
GENERATED_DIR = ROOT / "generated"
SWITCH_OUT_DIR = ROOT / "build" / "switch"


def win_to_wsl_path(path: Path) -> str:
    """Convert a Windows Path to a WSL mount path (/mnt/c/...)."""
    p = str(path.resolve()).replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        drive = p[0].lower()
        return f"/mnt/{drive}{p[2:]}"
    return p


def run_cmd(cmd, check=True, cwd=None, env=None):
    """Run a command displaying its execution."""
    print(f">> Running: {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd, env=env, shell=isinstance(cmd, str))
    if check and res.returncode != 0:
        sys.exit(f"Command failed with exit code {res.returncode}")
    return res.returncode


def get_eerecomp_path() -> Path:
    """Find eerecomp executable on Windows or Linux."""
    candidates = [
        BUILD_DIR / "tools" / "eerecomp.exe",
        BUILD_DIR / "tools" / "eerecomp",
        ROOT / "build" / "native" / "eerecomp.exe",
        ROOT / "build" / "native" / "eerecomp",
    ]
    for c in candidates:
        if c.is_file():
            return c
    sys.exit("eerecomp tool not found! Please build tools first.")


def extract_disc(iso_path: Path, game_dir: Path):
    """Extract SYSTEM.CNF and boot program from PS2 disc ISO."""
    disc_dir = game_dir / "disc"
    disc_dir.mkdir(parents=True, exist_ok=True)
    
    # 1. Extract SYSTEM.CNF
    run_cmd([sys.executable, str(SCRIPTS / "disc.py"), "extract", str(iso_path), str(disc_dir), "SYSTEM.CNF"])
    system_cnf = disc_dir / "SYSTEM.CNF"
    if not system_cnf.is_file():
        sys.exit(f"Failed to extract SYSTEM.CNF from {iso_path}")
    
    # 2. Parse BOOT2
    boot_line = ""
    with open(system_cnf, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if line.strip().startswith("BOOT2"):
                boot_line = line.strip()
                break
    if not boot_line:
        sys.exit("SYSTEM.CNF does not contain a BOOT2 line!")
    
    # Example: BOOT2 = cdrom0:\SLUS_203.28;1
    m = re.search(r"=\s*cdrom0:[\\/]([^;]+)", boot_line, re.IGNORECASE)
    if not m:
        m = re.search(r"=\s*([^\s;]+)", boot_line)
    if not m:
        sys.exit(f"Cannot parse boot program from BOOT2 line: {boot_line}")
    
    boot_prog = m.group(1).replace("\\", "/").split("/")[-1]
    print(f">> Identified PS2 boot executable: {boot_prog}")
    
    # 3. Extract boot executable
    run_cmd([sys.executable, str(SCRIPTS / "disc.py"), "extract", str(iso_path), str(disc_dir), boot_prog])
    boot_elf = disc_dir / boot_prog
    if not boot_elf.is_file():
        sys.exit(f"Failed to extract boot ELF: {boot_elf}")
    
    return boot_elf


def recompile_mips(boot_elf: Path, gen_dir: Path, game_dir: Path):
    """Recompile MIPS R5900 Emotion Engine code to C++ units."""
    eerecomp = get_eerecomp_path()
    gen_dir.mkdir(parents=True, exist_ok=True)
    
    entries_file = game_dir / "entries.txt"
    if not entries_file.exists():
        entries_file.write_text("", encoding="utf-8")
        
    cmd = [
        str(eerecomp),
        str(boot_elf),
        "--entries", str(entries_file),
        "--out", str(gen_dir)
    ]
    print(f">> Translating MIPS R5900 machine code to C++...")
    run_cmd(cmd)


def build_switch_nro(game_name: str, gen_dir: Path, title: str, author: str, version: str):
    """Cross-compile ps2runtime + game units into a Nintendo Switch .nro binary."""
    wsl_src = win_to_wsl_path(ROOT)
    wsl_gen = win_to_wsl_path(gen_dir)
    wsl_build = f"/tmp/switch-build-{game_name.lower()}"
    
    # Configure with CMake
    cmake_cmd = (
        f"/usr/local/bin/switch-cmake -G Ninja -B {wsl_build} -S {wsl_src} "
        f"-DPS2_GENERATED={wsl_gen} "
        f'-DSWITCH_TITLE="{title}" '
        f'-DSWITCH_AUTHOR="{author}" '
        f'-DSWITCH_VERSION="{version}"'
    )
    print(">> Configuring Switch build via devkitA64...")
    run_cmd(["wsl", "-d", "devkita64", "-u", "root", "bash", "-c", cmake_cmd])
    
    # Build with Ninja
    ninja_cmd = f"/usr/local/bin/switch-ninja -C {wsl_build} ps2run_nro"
    print(">> Compiling native Nintendo Switch homebrew binary...")
    run_cmd(["wsl", "-d", "devkita64", "-u", "root", "bash", "-c", ninja_cmd])
    
    # Copy output files back to Windows workspace
    game_out_dir = SWITCH_OUT_DIR / game_name
    game_out_dir.mkdir(parents=True, exist_ok=True)
    
    wsl_out = win_to_wsl_path(game_out_dir)
    copy_cmd = (
        f"cp {wsl_build}/ps2run.nro {wsl_out}/{game_name}.nro && "
        f"cp {wsl_build}/ps2run.elf {wsl_out}/{game_name}.elf"
    )
    run_cmd(["wsl", "-d", "devkita64", "-u", "root", "bash", "-c", copy_cmd])
    
    nro_path = game_out_dir / f"{game_name}.nro"
    if not nro_path.is_file():
        sys.exit(f"Failed to produce Switch NRO: {nro_path}")
        
    print(f">> Successfully built Nintendo Switch homebrew: {nro_path} ({nro_path.stat().st_size // (1024*1024)} MB)")
    return nro_path


def package_for_sd(game_name: str, nro_path: Path, game_dir: Path, iso_path: Path = None):
    """Create ready-to-use SD card directory structure (sdmc:/switch/ps22switch/<game>/)."""
    pkg_dir = SWITCH_OUT_DIR / game_name / "package" / "switch" / "ps22switch" / game_name
    pkg_dir.mkdir(parents=True, exist_ok=True)
    
    # Copy NRO
    shutil.copy2(nro_path, pkg_dir / f"{game_name}.nro")
    
    # Copy disc/ folder (extracted ELF)
    disc_src = game_dir / "disc"
    if disc_src.is_dir():
        disc_dst = pkg_dir / "disc"
        if disc_dst.exists():
            shutil.rmtree(disc_dst)
        shutil.copytree(disc_src, disc_dst)
        
    # Copy app.json and keys.ini if present
    for fname in ["app.json", "keys.ini"]:
        fpath = game_dir / fname
        if fpath.is_file():
            shutil.copy2(fpath, pkg_dir / fname)
            
    print(f">> SD Card package created at: {pkg_dir}")
    print(f">> Copy the 'switch' folder inside {SWITCH_OUT_DIR / game_name / 'package'} to your Switch SD root!")


def main():
    parser = argparse.ArgumentParser(description="Recompile PS2 game to Nintendo Switch homebrew")
    parser.add_argument("target", help="Path to game ISO (e.g. Tekken 4.iso) or game name in games/")
    parser.add_argument("--title", default="", help="NACP Title Name for Switch homebrew menu")
    parser.add_argument("--author", default="DevCrashDay", help="NACP Author name")
    parser.add_argument("--version", default="1.0.0", help="NACP Version")
    parser.add_argument("--skip-recomp", action="store_true", help="Skip MIPS translation and only build Switch NRO")
    args = parser.parse_args()
    
    target_path = Path(args.target)
    if target_path.is_file() and target_path.suffix.lower() == ".iso":
        iso_path = target_path.resolve()
        game_name = iso_path.stem.replace(" ", "-")
        # Strip common region suffixes like (USA)
        clean_name = re.sub(r"\s*\([^)]*\)", "", iso_path.stem).strip().replace(" ", "-")
        if clean_name:
            game_name = clean_name
        title = args.title or re.sub(r"\s*\([^)]*\)", "", iso_path.stem).strip()
    else:
        game_name = args.target.replace(" ", "-")
        iso_path = None
        title = args.title or game_name.replace("-", " ")
        
    game_dir = GAMES_DIR / game_name
    gen_dir = GENERATED_DIR / game_name
    game_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"==================================================")
    print(f" ps22switch: Recompiling {title} for Nintendo Switch")
    print(f"==================================================")
    
    # Step 1 & 2: Extract & Recompile if needed
    if not args.skip_recomp:
        if iso_path and iso_path.is_file():
            print(f">> Processing ISO: {iso_path}")
            boot_elf = extract_disc(iso_path, game_dir)
            recompile_mips(boot_elf, gen_dir, game_dir)
            
            # Create/update app.json
            app_json = {
                "source": str(iso_path),
                "name": game_name,
                "elf": f"disc/{boot_elf.name}",
                "iso": "disc.iso"
            }
            with open(game_dir / "app.json", "w", encoding="utf-8") as f:
                json.dump(app_json, f, indent=4)
        else:
            # Check if generated files already exist
            units = list(gen_dir.glob("*.cpp"))
            if not units:
                sys.exit(f"No generated C++ units found in {gen_dir}! Please supply an ISO to recompile.")
            print(f">> Using existing generated C++ units in {gen_dir} ({len(units)} files)")
            
    # Step 3: Build Switch NRO
    nro_path = build_switch_nro(game_name, gen_dir, title, args.author, args.version)
    
    # Step 4: Package for Switch SD card
    package_for_sd(game_name, nro_path, game_dir, iso_path)
    print(f"\n[DONE] ps22switch successfully recompiled {title} -> {nro_path.name} for Nintendo Switch!")


if __name__ == "__main__":
    main()
