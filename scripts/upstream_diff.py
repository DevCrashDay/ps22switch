#!/usr/bin/env python3
"""What changed in the upstream snapshot since the one this fork started from.

The upstream project (C:\\ps2-native-recomp-main) is not a git repository: it is a folder that gets replaced when it is updated. This
fork's first commit (0660c5e "Import ps2-native-recomp") is the snapshot it started from, so checking that commit out next to the folder
and comparing the two says exactly what the update contains and what it costs to take.

    python scripts/upstream_diff.py --base C:\\ps22switch-tmp\\import-base --upstream C:\\ps2-native-recomp-main [--git]

With --git it also asks this repository whether each changed file is one this fork has edited, which is what decides between "take
theirs" and "merge".
"""
import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

# Where the update can matter: the emulator, the recompiler, the renderer, the host layer, the tools and the docs. Not game data, not
# build output, not third party drops.
SCOPE = ["app", "cmake", "docs", "host", "recomp", "runtime", "scripts", "tests", "web", ".github"]
ROOT_FILES = ["CMakeLists.txt", "README.md", "Start.cmd", ".gitignore", ".gitattributes", "LICENSE"]
SKIP_DIRS = {"build", "generated", "games", "isos", "third_party", "tools", ".git", "node_modules", "bench-base", "bench-dec", "scratch"}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def files_of(root: Path, scope):
    found = {}
    for name in scope:
        start = root / name
        if start.is_file():
            found[name] = start
            continue
        if not start.is_dir():
            continue
        for directory, subdirs, names in os.walk(start):
            subdirs[:] = [d for d in subdirs if d not in SKIP_DIRS]
            for name2 in names:
                path = Path(directory) / name2
                found[str(path.relative_to(root)).replace("\\", "/")] = path
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default=r"C:\ps22switch-tmp\import-base", help="a checkout of the import commit")
    parser.add_argument("--upstream", default=r"C:\ps2-native-recomp-main", help="the updated upstream folder")
    parser.add_argument("--git", action="store_true", help="also report whether this fork edited the same file")
    args = parser.parse_args()

    base_root, up_root = Path(args.base), Path(args.upstream)
    if not base_root.is_dir() or not up_root.is_dir():
        print(f"need both trees: {base_root} and {up_root}")
        return 1

    scope = ROOT_FILES + SCOPE
    base = files_of(base_root, scope)
    up = files_of(up_root, scope)

    added = sorted(set(up) - set(base))
    removed = sorted(set(base) - set(up))
    changed = []
    for name in sorted(set(base) & set(up)):
        if digest(base[name]) != digest(up[name]):
            changed.append(name)

    print(f"== {len(up)} files in the update's scope, {len(base)} in the snapshot it replaced")
    print(f"== {len(added)} added, {len(removed)} removed, {len(changed)} changed\n")

    def ours(name):
        done = subprocess.run(["git", "-C", str(Path(__file__).resolve().parent.parent), "diff", "--quiet", "0660c5e", "HEAD", "--", name])
        return done.returncode != 0

    if added:
        print("ADDED")
        for name in added:
            print(f"  {name}")
        print()
    if removed:
        print("REMOVED")
        for name in removed:
            print(f"  {name}")
        print()
    if changed:
        print("CHANGED" + ("   (ours = this fork also edited it)" if args.git else ""))
        for name in changed:
            mark = ""
            if args.git:
                mark = "   ours" if ours(name) else ""
            print(f"  {name}{mark}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
