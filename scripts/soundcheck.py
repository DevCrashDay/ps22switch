#!/usr/bin/env python
"""Runs God of War II in a window from a saved state with the fight pattern pressed and keeps what the sound did.

    python scripts/soundcheck.py --out games/gow2/sound/arena-on --frames 1200                      # the arena, stretcher on
    python scripts/soundcheck.py --out games/gow2/sound/arena-off --frames 1200 --env PS2_AUDIO_STRETCH=0
    python scripts/soundcheck.py --state gow2_f052625.state --start 52625 --out ... --env PS2_GPU_SKIP=raster

Writes into --out: run.log (the runner's output), device.wav (PS2_AUDIO_DUMP: the sound as it went to the device), heard.wav
(PS2_AUDIO_HEARD: what the speaker played, silence included), chip.wav (PS2_DUMP_AUDIO: what the sound chip made) and prints the
[audio] lines of the log. Needs the game built (scripts/port.sh) and the saved states of docs/gow2-play.md in games/gow2/play.
On the shared laptop start it through wt/slot.py: python wt/slot.py run <lane> -- python scripts/soundcheck.py ...
docs/sound.md says what the numbers mean.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import play  # noqa: E402  (the move syntax)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True, help="folder for the log and the sound files")
    ap.add_argument("--state", default="gow2_f055800.state", help="saved state in games/gow2/play (default: the arena); 'none' starts the game from power-on")
    ap.add_argument("--start", type=int, default=55800, help="the frame the state starts at (the name says it)")
    ap.add_argument("--frames", type=int, default=1200, help="frames to run after the state")
    ap.add_argument("--moves", default="@fight.txt", help="buttons to press (scripts/play.py syntax; @file is read from games/gow2/play)")
    ap.add_argument("--exe", type=Path, help="runner (default: build/native-gow2/ps2run.exe of this folder)")
    ap.add_argument("--play", type=Path, help="folder with the states (default: games/gow2/play here, else the main checkout's)")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the runner")
    ap.add_argument("--no-dumps", action="store_true", help="do not write the wav files")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    exe = args.exe or ROOT / "build" / "native-gow2" / "ps2run.exe"
    playdir = args.play
    if not playdir:
        playdir = ROOT / "games" / "gow2" / "play"
        if not (playdir / args.state).exists():
            playdir = ROOT.parent.parent / "games" / "gow2" / "play"  # the main checkout, read only
    state = None if args.state == "none" else playdir / args.state
    if not exe.exists() or (state and not state.exists()):
        sys.exit(f"missing {exe if not exe.exists() else state}")
    if state is None and args.start == 55800:
        args.start = 0
    program = next(iter(sorted((ROOT / "games" / "gow2" / "disc").glob("[A-Z][A-Z][A-Z][A-Z]_[0-9][0-9][0-9].[0-9][0-9]"))))
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    tokens = [("@" + str(playdir / t[1:])) if t.startswith("@") else t for t in args.moves.split()]
    presses = play.parse_moves(" ".join(play.read_moves(" ".join(tokens))), args.start)
    cmd = [str(exe), str(program), "--disc", str(ROOT / "games" / "gow2" / "disc.iso"), "--window", "--memcard", str(out / "card.ps2"),
           "--quick-dir", str(out / "quick")] + (["--load-state", str(state)] if state else []) + presses + ["--frames", str(args.start + args.frames)]
    env = dict(os.environ)
    if not args.no_dumps:
        env["PS2_AUDIO_DUMP"] = str(out / "device.wav")
        env["PS2_AUDIO_HEARD"] = str(out / "heard.wav")
        env["PS2_DUMP_AUDIO"] = str(out / "chip.wav")
    for pair in args.env:
        key, _, value = pair.partition("=")
        env[key] = value
    started = time.time()
    with open(out / "run.log", "wb") as log:
        code = subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout).returncode
    wall = time.time() - started
    print(f"ran {args.frames} frames in {wall:.1f} s ({args.frames / wall:.1f} fps), exit code {code}")
    for line in (out / "run.log").read_text(errors="replace").splitlines():
        if line.startswith("[audio]") and "ran dry at frame" not in line or line.startswith("[spu2] wrote"):
            print(line)


if __name__ == "__main__":
    main()
