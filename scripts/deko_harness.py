#!/usr/bin/env python3
"""Builds and runs the deko3d shim's harness on the PC: the real `wgpu_deko.cpp` and `deko_gpu.cpp` against a stub deko3d
(runtime/src/gs/gpu/deko/stub), driven through the WebGPU calls gpu.cpp makes - the same buffer sizes, layouts, entry points, passes,
submit and present.

Why: a mistake in the shim used to cost a console round trip (copy to the SD card, start the game, read the log). This finds the same
class of mistake in a second: it runs the whole API surface the renderer uses, and the stub checks what deko3d itself would reject (a
4 MiB buffer bound as a uniform, a module loaded twice into the code block, an image larger than its memory, a shader for the wrong
stage, a draw with no primitive...).

    python scripts/deko_harness.py [--wsl devkita64] [--verbose]

Needs a C++20 compiler. On Windows the compiler comes from the wsl distribution that also has devkitPro's headers (libnx's deko3d.h);
elsewhere a native g++ with those headers on the include path is used.
"""

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEKO = ROOT / "runtime" / "src" / "gs" / "gpu" / "deko"

SOURCES = [DEKO / "stub" / "deko3d_stub.cpp", DEKO / "deko_gpu.cpp", DEKO / "wgpu_deko.cpp",
           DEKO / "dksh_table.cpp", DEKO / "test" / "deko_harness.cpp"]


def wsl_path(p: Path) -> str:
    """F:\\ps22switch\\x -> /mnt/f/ps22switch/x"""
    s = str(p).replace("\\", "/")
    if len(s) > 1 and s[1] == ":":
        return f"/mnt/{s[0].lower()}{s[2:]}"
    return s


def run_wsl(distro: str, verbose: bool) -> int:
    out = "/tmp/deko_harness"
    args = [
        "g++", "-std=c++20", "-O1", "-g", "-o", out,
        "-I", wsl_path(DEKO / "stub"),           # the fake switch.h, before the real one
        "-I", wsl_path(DEKO),
        "-I", wsl_path(DEKO.parent / "vk" / "include"),
        "-I", "/opt/devkitpro/libnx/include",     # deko3d.h and deko3d.hpp
        "-pthread",
    ] + [wsl_path(s) for s in SOURCES]
    script = " ".join(shlex.quote(a) for a in args) + f" && {out}"
    if verbose:
        print(script)
    r = subprocess.run(["wsl", "-d", distro, "-u", "root", "--", "bash", "-lc", script], capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    if r.stdout:
        sys.stdout.write(r.stdout)
    return r.returncode


def run_native(verbose: bool) -> int:
    out = "/tmp/deko_harness"
    args = [
        "g++", "-std=c++20", "-O1", "-g", "-o", out,
        "-I", str(DEKO / "stub"), "-I", str(DEKO), "-I", str(DEKO.parent / "vk" / "include"),
        "-I", "/opt/devkitpro/libnx/include", "-pthread",
    ] + [str(s) for s in SOURCES]
    if verbose:
        print(" ".join(args))
    return subprocess.run(args).returncode and subprocess.run([out]).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wsl", default="" if not sys.platform.startswith("win") else "devkita64",
                        help="WSL distribution with a compiler and devkitPro's headers (default: devkita64 on Windows)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if args.wsl:
        return run_wsl(args.wsl, args.verbose)
    return run_native(args.verbose)


if __name__ == "__main__":
    sys.exit(main())
