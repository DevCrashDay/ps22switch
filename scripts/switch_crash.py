#!/usr/bin/env python3
"""Read the crash lines of a ps2run.log and name the code they point at.

host/switch_crash.cpp prints a CPU fault's registers and call chain as offsets from a function called crash_marker, so the numbers do not
depend on where the console loaded the program. This finds crash_marker in the .elf the .nro was built from, adds each offset back, and
names the function the address falls in (and, when the .elf has debug information, its file and line):

    python scripts/switch_crash.py build/switch/tests/J-deko-render/ps2run.log
    python scripts/switch_crash.py ps2run.log --elf /tmp/switch-build-deko/ps2run.elf

Without --elf it looks for /tmp/switch-build-deko/ps2run.elf in WSL, then for build/switch/*/ps2run.elf here, newest first. The devkitA64
binutils are found on PATH or, when they are not there, through `wsl -d devkita64`.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

MARKER = "crash_marker"
READER = "aarch64-none-elf-addr2line"
LISTER = "aarch64-none-elf-nm"

CRASH = re.compile(r"\[crash\]")
# Both kinds of address in the dump: "pc 0x… (+0x…)" and "   #0 +0x…".
OFFSET = re.compile(r"\+(0x[0-9a-fA-F]+)")
FUNCTIONS = ("t", "w", "T", "W")


def wsl(*args):
    return ["wsl", "-d", "devkita64", "-u", "root", "--", *args]


def linux_path(path):
    """F:/ps22switch/x -> /mnt/f/ps22switch/x, so WSL can open a file given to a Windows shell (and a \\\\wsl$ path as itself)."""
    network = re.fullmatch(r"\\\\wsl\$\\([^\\]+)\\(.*)", path)
    if network:
        return "/" + network.group(2).replace("\\", "/")
    match = re.fullmatch(r"([A-Za-z]):[\\/](.*)", path)
    if not match:
        return path
    return "/mnt/" + match.group(1).lower() + "/" + match.group(2).replace("\\", "/")


def tool(name, args):
    """The output of a devkitA64 tool, run here or through WSL."""
    if _local(name) is not None:
        done = subprocess.run([name, *args], capture_output=True, text=True)
    else:
        arguments = [name, *[linux_path(a) if a.endswith(".elf") else a for a in args]]
        done = subprocess.run(wsl("bash", "-lc", " ".join(arguments)), capture_output=True, text=True)
    return done.stdout if done.returncode == 0 else None


def _local(name):
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        for suffix in (".exe", ""):
            candidate = os.path.join(directory, name + suffix)
            if os.path.isfile(candidate):
                return candidate
    return None


def functions(elf):
    """(name, address) for every function the .elf defines, ascending."""
    listing = tool(LISTER, ["-C", "--defined-only", elf])
    if listing is None:
        return []
    found = []
    for line in listing.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) < 3 or not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        if parts[1] in FUNCTIONS:
            found.append((parts[2], int(parts[0], 16)))
    return sorted(found, key=lambda pair: pair[1])


def marker(elf):
    # It sits in an anonymous namespace, so the name it has here is "(anonymous namespace)::crash_marker()".
    for name, address in functions(elf):
        if MARKER in name:
            return address
    return None


def named(elf, table, address):
    import bisect

    starts = [address for _, address in table]
    index = bisect.bisect_right(starts, address) - 1
    if index < 0:
        return None
    name, start = table[index]
    return f"{name}+0x{address - start:x}"


def lines(elf, addresses):
    """file:line for each address, when the .elf was built with debug information."""
    if not addresses:
        return [None] * len(addresses)
    out = tool(READER, ["-f", "-C", "-e", elf, *[hex(a) for a in addresses]])
    if out is None:
        return [None] * len(addresses)
    parts = [l for l in out.splitlines() if l.strip()]
    result = []
    for index in range(len(addresses)):
        name = parts[index * 2] if index * 2 < len(parts) else "??"
        where = parts[index * 2 + 1] if index * 2 + 1 < len(parts) else "??:0"
        result.append(None if name == "??" or where == "??:0" else f"{name} at {where}")
    return result


def find_elf(given):
    if given:
        return given if os.path.isfile(given) else None
    candidates = sorted(glob.glob(os.path.join("build", "switch", "*", "ps2run.elf")), key=os.path.getmtime, reverse=True)
    if candidates:
        return candidates[0]
    done = subprocess.run(wsl("bash", "-lc", "ls -t /tmp/switch-build-deko/ps2run.elf 2>/dev/null"), capture_output=True, text=True)
    if done.stdout.strip():
        return done.stdout.strip().splitlines()[0]
    return None


def main():
    parser = argparse.ArgumentParser(description="Name the functions a ps2run.log crash points at.")
    parser.add_argument("log", help="the ps2run.log to read")
    parser.add_argument("--elf", help="the .elf the .nro was built from")
    args = parser.parse_args()

    text = open(args.log, "r", encoding="utf-8", errors="replace").read()
    crash_lines = [line for line in text.splitlines() if CRASH.search(line)]
    if not crash_lines:
        print(f"{args.log}: no [crash] lines. Either nothing faulted, or that run was made before host/switch_crash.cpp")
        return 1
    for line in crash_lines:
        print(line)

    elf = find_elf(args.elf)
    table = functions(elf) if elf else []
    base = marker(elf) if elf else None
    if not table or base is None:
        print(f"\n{elf or 'no .elf found'}: no {MARKER} symbol to measure the offsets from: pass --elf, or rebuild")
        return 1

    offsets = [int(o, 16) for line in crash_lines for o in OFFSET.findall(line)]
    if not offsets:
        return 0
    addresses = [base + offset for offset in offsets]
    sources = lines(elf, addresses)
    print(f"\n{elf}: {MARKER} at 0x{base:x}")
    for address, source in zip(addresses, sources):
        name = named(elf, table, address)
        print(f"  0x{address:x}  {name or '?'}" + (f"    [{source}]" if source else ""))
    if not any(named(elf, table, a) for a in addresses):
        print(f"\n{READER} and {LISTER} are needed for the names; they come with devkitA64.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
