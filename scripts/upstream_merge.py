#!/usr/bin/env python3
"""Merge the upstream update into this fork, file by file, and report what needs a human.

Our fork's first commit (0660c5e) is a snapshot of the upstream project as it was, our branch is what this fork made of it since, and
C:\\ps2-native-recomp-main is the upstream as it is now. That is exactly a three-way merge: base = the snapshot, ours = our branch,
theirs = the update. This does it per file with git merge-file, which reports a conflict only where both sides changed the same lines.

    python scripts/upstream_merge.py --into C:\\ps22switch-tmp\\merge [--write] [--only PATH ...]

Without --write it only reports. With --write it writes every file that merged cleanly and leaves the conflicts (with their conflict
markers) for a person.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

BASE = Path(r"C:\ps22switch-tmp\import-base")   # the snapshot as it was
UP = Path(r"C:\ps2-native-recomp-main")          # the update
SCOPE = ["app", "cmake", "docs", "host", "recomp", "runtime", "scripts", "tests", "web", ".github"]
ROOT_FILES = ["CMakeLists.txt", "README.md", "Start.cmd", ".gitignore", ".gitattributes", "LICENSE"]
SKIP = {"build", "generated", "games", "isos", "third_party", "tools", ".git", "bench-base", "bench-dec", "scratch"}


def files_of(root: Path):
    found = set()
    for name in ROOT_FILES + SCOPE:
        start = root / name
        if start.is_file():
            found.add(name)
            continue
        for directory, subdirs, names in os.walk(start):
            subdirs[:] = [d for d in subdirs if d not in SKIP]
            for n in names:
                found.add(str((Path(directory) / n).relative_to(root)).replace("\\", "/"))
    return found


def normalized(path: Path) -> bytes:
    """The file with LF endings, which is what the merge has to see: the update's files are LF and a Windows checkout is CRLF, and
    without this every line of every file counts as changed."""
    return path.read_bytes().replace(b"\r\n", b"\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--into", default=r"C:\ps22switch-tmp\merge", help="a worktree of this fork to merge into")
    parser.add_argument("--write", action="store_true", help="write the clean merges and the conflicted files")
    parser.add_argument("--only", nargs="*", default=[], help="limit to these paths")
    args = parser.parse_args()
    into = Path(args.into)
    if not (into / ".git").exists():
        print(f"{into} is not a worktree of this fork (git worktree add {into} -b upstream-merge HEAD)")
        return 1

    base_files, up_files = files_of(BASE), files_of(UP)
    todo = sorted(base_files & up_files)
    if args.only:
        wanted = [p.lower() for p in args.only]
        todo = [p for p in todo if any(w in p.lower() for w in wanted)]

    scratch = into / ".merge-tmp"
    scratch.mkdir(exist_ok=True)
    clean, conflicted, added, failed = [], [], [], []
    for rel in todo:
        ours_path = into / rel
        if not ours_path.is_file():
            failed.append(rel)
            continue
        base, theirs = BASE / rel, UP / rel
        if not base.is_file():
            added.append(rel)  # the update added it: copy
            if args.write:
                ours_path.parent.mkdir(parents=True, exist_ok=True)
                ours_path.write_bytes(theirs.read_bytes())
            continue
        ours_raw = ours_path.read_bytes()
        ours, base_text, theirs_text = normalized(ours_path), normalized(base), normalized(theirs)
        if base_text == theirs_text or ours == theirs_text:
            continue  # the update did not touch it, or we already have exactly that
        ours_tmp, base_tmp, theirs_tmp = scratch / "ours", scratch / "base", scratch / "theirs"
        ours_tmp.write_bytes(ours)
        base_tmp.write_bytes(base_text)
        theirs_tmp.write_bytes(theirs_text)
        done = subprocess.run(["git", "merge-file", "-p", "--diff3", str(ours_tmp), str(base_tmp), str(theirs_tmp)],
                              capture_output=True)
        text = done.stdout
        if b"\r\n" in ours_raw:  # keep the file's own line endings, so the whole file does not look changed
            text = text.replace(b"\n", b"\r\n")
        if done.returncode == 0:
            clean.append(rel)
            if args.write and text != ours_raw:
                ours_path.write_bytes(text)
        elif done.returncode > 0:
            conflicted.append((rel, done.returncode))
            if args.write:
                ours_path.write_bytes(text)
        else:
            failed.append(rel)
    for leftover in scratch.glob("*"):
        leftover.unlink()

    added_existing = sorted(up_files - base_files)
    print(f"{len(clean)} files merge cleanly, {len(conflicted)} conflict, {len(added_existing)} are new")
    if added_existing:
        print("\nNEW (the update adds these; nothing of ours to lose)")
        for rel in added_existing:
            print(f"  {rel}")
    if conflicted:
        print("\nCONFLICT (both sides changed the same lines; the file is left with conflict markers)")
        for rel, count in conflicted:
            print(f"  {rel}  ({count} hunk groups)")
    if failed:
        print("\nMISSING IN THE WORKTREE")
        for rel in failed:
            print(f"  {rel}")
    if not args.write:
        print("\n(dry run: nothing written; add --write)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
