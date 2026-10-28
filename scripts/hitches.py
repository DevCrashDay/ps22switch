#!/usr/bin/env python3
"""Replay a recorded play session in a real window, with real timing, and list the frames that take much longer than their neighbours.

  python scripts/hitches.py games/gow2/play/states/run_..._f055800.inputs --state games/gow2/play/gow2_f055800.state
  python scripts/hitches.py <inputs> --state <state> --threshold 50 --log hitch.log --frames hitch-frames.txt
  python scripts/hitches.py <inputs> --state <state> --pictures pictures.txt   (also what the window thread did for every picture)
  python scripts/hitches.py --analyse hitch-frames.txt --log hitch.log --pictures pictures.txt   (look at an earlier run again)

Why a window: the headless replay (scripts/replay.py) has no graphics chip, no sound device and no window thread, so it says whether the
game went the same way, but nothing about speed. Here the same recording runs the way the player's run did: three threads, the graphics
chip, the sound device (the sound paces the game, so you hear the replay), and PS2_FRAME_LOG writes one line per frame. Nothing else on the
laptop should run meanwhile (a measurement with another game running means nothing).

The answer: how many frames were slow (default: over 50 ms), each of them with what the program measured in that frame (CPU time of all
threads, waiting for the window thread, waiting for the sound, shapes drawn, vector programs run by the slow interpreter, sound waiting in
the queue), the "[audio] the sound ran dry" lines, the stretching summary and whether the replay said "same to the end".
Extra switches for the run go with --env NAME=VALUE (PS2_HITCH_LOG=1 prints what the program did in every slow frame). --pictures FILE
switches on PS2_HITCH_PICTURES: one line per picture with the milliseconds the window thread spent on the graphics-chip commands (gif),
waiting for the next screen picture (acquire: the chip is still busy with earlier pictures), handing the work over (submit); the summary
says which of them the slow stretches are made of. The console stays quiet unless PS2_HITCH_LOG is given (docs/hitches.md).
"""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read_header(path):
    head = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if re.match(r"(p\d|h|e) \d", line):
                break
            key, _, value = line.rstrip("\n").partition(" ")
            head[key] = value
    return head


def run(args):
    inputs = Path(args.recording).resolve()
    head = read_header(inputs)
    game = args.game
    program = args.program or head.get("program", "")
    if not game:
        m = re.search(r"games[/\\]([^/\\]+)[/\\]", program)
        game = m.group(1) if m else "gow2"
    program_path = Path(program) if Path(program).is_absolute() else ROOT / program.replace("\\", "/")
    if not program_path.exists():
        program_path = ROOT / "games" / game / "disc" / Path(program.replace("\\", "/")).name
    disc = Path(args.disc) if args.disc else ROOT / "games" / game / "disc.iso"
    exe = Path(args.exe) if args.exe else ROOT / "build" / f"native-{game}" / "ps2run.exe"
    state = Path(args.state).resolve() if args.state else None
    work = Path(tempfile.mkdtemp(prefix="hitches_"))
    frames_path = Path(args.frames).resolve() if args.frames else work / "frames.txt"
    log_path = Path(args.log).resolve() if args.log else work / "run.log"
    try:
        cmd = [str(exe), str(program_path), "--disc", str(disc), "--replay", str(inputs), "--quick-dir", str(work)]
        if not args.headless:
            cmd.append("--window")
        if state:
            cmd += ["--load-state", str(state)]
            card = Path(str(state) + ".ps2")
            if card.exists():
                shutil.copyfile(card, work / "card.ps2")
            # No card beside the state: a card that is not there yet, which the game sees as a new, unformatted card. That is what the
            # player had (the .cmd files give --memcard my-save.ps2 before the first save); without any card the game goes another way
            # at once (frame 55875 of the arena session).
            cmd += ["--memcard", str(work / "card.ps2")]
        env = dict(os.environ)
        env["PS2_FRAME_LOG"] = str(frames_path)
        if args.pictures:
            env["PS2_HITCH_LOG"] = "100000"  # timing on, nothing printed (an --env PS2_HITCH_LOG=... below replaces it)
            env["PS2_HITCH_PICTURES"] = str(Path(args.pictures).resolve())
        for item in args.env:
            k, _, v = item.partition("=")
            env[k] = v
        print("running:", " ".join(cmd), flush=True)
        with open(log_path, "wb") as log:
            code = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
        print(f"ps2run ended with code {code}")
    finally:
        shutil.rmtree(work, ignore_errors=True) if not (args.frames and args.log) else None
    return frames_path, log_path, code


def stretches(rows, threshold, median):
    """Slow frames that lie within 8 frames of each other make one stretch; the same stretches show up in every run of one recording."""
    out = []
    for r in rows:
        if r[1] > threshold:
            if out and r[0] - out[-1][-1][0] <= 8:
                out[-1].append(r)
            else:
                out.append([r])
    print(f"slow stretches (slow frames within 8 frames of each other): {len(out)}")
    print("  first-last frame   slow  longest_ms  lost_s  of the slow frames' time: waiting for window thread / for sound")
    for c in out:
        total = sum(r[1] for r in c)
        print(f"  {c[0][0]:6d}-{c[-1][0]:6d} {len(c):6d} {max(r[1] for r in c):10.1f} {sum(r[1] - median for r in c) / 1000:7.2f}"
              f"  {100 * sum(r[3] for r in c) / total:3.0f} % / {100 * sum(r[4] for r in c) / total:3.0f} %")


def picture_summary(path):
    """Mean milliseconds per picture of the window thread, for all pictures and by how long the picture took."""
    rows = []
    with open(path, encoding="utf-8") as f:
        next(f)
        for line in f:
            p = line.split()
            if len(p) == 9:
                rows.append([float(x) for x in p])
    if len(rows) < 3:
        return
    rows = rows[1:]
    print("window thread, mean ms per picture (gap = time since the previous picture; gif = graphics-chip commands built on the processor;")
    print("  acquire = waiting for the chip; submit = handing the work over; wait = the rest, i.e. waiting for the game):")
    for name, lo, hi in (("all", 0, 1e9), ("gap up to 25 ms", 0, 25), ("gap 25 to 40", 25, 40), ("gap 40 to 60", 40, 60), ("gap over 60", 60, 1e9)):
        sel = [r for r in rows if lo < r[1] <= hi] if name != "all" else rows
        if not sel:
            continue
        m = lambda k: sum(r[k] for r in sel) / len(sel)
        wait = m(1) - m(3) - m(4) - m(5) - m(6) - m(7) - m(8)
        print(f"  {name:16s} {len(sel):6d} pictures: gap {m(1):5.1f}  gif {m(3):5.1f}  acquire {m(6):5.1f}  submit {m(7):5.1f}  wait {wait:5.1f}")


def analyse(frames_path, log_path, threshold, pictures=None):
    rows = []
    with open(frames_path, encoding="utf-8") as f:
        next(f)
        for line in f:
            p = line.split()
            if len(p) == 8:
                rows.append((int(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4]), int(p[5]), int(p[6]), float(p[7])))
    if not rows:
        print("no frames logged")
        return
    wall = [r[1] for r in rows]
    srt = sorted(wall)
    pct = lambda q: srt[min(len(srt) - 1, int(len(srt) * q))]
    total = sum(wall) / 1000
    print(f"{len(rows)} frames in {total:.1f} s (frames {rows[0][0]} to {rows[-1][0]}): mean {statistics.mean(wall):.2f} ms, median {pct(.5):.2f}, "
          f"95% {pct(.95):.2f}, 99% {pct(.99):.2f}, 99.9% {pct(.999):.2f}, longest {srt[-1]:.1f} ms")
    for t in (30, 40, 50, 75, 100, 200, 500):
        print(f"  frames over {t:3d} ms: {sum(1 for w in wall if w > t)}")
    slow = [r for r in rows if r[1] > threshold]
    over_33 = [r for r in rows if r[1] > 33.4]
    print(f"frames slower than 30 fps (over 33.4 ms): {len(over_33)}, {sum(r[1] - 16.7 for r in over_33) / 1000:.1f} s beyond a 60 fps frame; of that the game waited "
          f"{sum(min(r[3], r[1] - 16.7) for r in over_33) / 1000:.1f} s for the window thread and {sum(min(r[4], r[1] - 16.7) for r in over_33) / 1000:.1f} s for the sound")
    print(f"slow frames (over {threshold} ms): {len(slow)}; time lost to them beyond a normal frame: {sum(r[1] - pct(.5) for r in slow) / 1000:.2f} s")
    stretches(rows, threshold, pct(.5))
    if pictures and Path(pictures).exists():
        picture_summary(pictures)
    print("frame   at_s  wall_ms  cpu_ms  wait_win  wait_snd  shapes  interp  audio_ms | previous frame wall")
    elapsed = 0.0
    prev = 0.0
    seen = {r[0]: i for i, r in enumerate(rows)}
    clock = {}
    for r in rows:
        elapsed += r[1] / 1000
        clock[r[0]] = elapsed
    for r in slow:
        i = seen[r[0]]
        print(f"{r[0]:6d} {clock[r[0]]:6.1f} {r[1]:8.1f} {r[2]:7.1f} {r[3]:9.1f} {r[4]:9.1f} {r[5]:7d} {r[6]:7d} {r[7]:9.0f} | {rows[i-1][1] if i else 0:.1f}")
    text = Path(log_path).read_text(encoding="utf-8", errors="replace") if log_path and Path(log_path).exists() else ""
    dry = re.findall(r"the sound ran dry at frame (\d+)", text)
    print(f"[audio] the sound ran dry lines: {len(dry)}" + (" at frames " + ", ".join(dry) if dry else ""))
    for l in text.splitlines():
        if l.startswith("[audio] the device got") or l.startswith("[audio] stretching") or l.startswith("[replay]"):
            print(l[:230])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", nargs="?", help="the .inputs recording")
    ap.add_argument("--state", help="the state the recording began from")
    ap.add_argument("--game")
    ap.add_argument("--program")
    ap.add_argument("--disc")
    ap.add_argument("--exe")
    ap.add_argument("--headless", action="store_true", help="no window (then there is no graphics and no sound: only for comparing)")
    ap.add_argument("--threshold", type=float, default=50.0, help="a frame is slow when it took longer than this many ms")
    ap.add_argument("--log", help="keep the run's log in this file")
    ap.add_argument("--frames", help="keep the per-frame log (PS2_FRAME_LOG) in this file")
    ap.add_argument("--pictures", help="keep the window thread's table of every picture (PS2_HITCH_PICTURES) in this file")
    ap.add_argument("--analyse", help="only look at this per-frame log again (with --log for the run's log)")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the run")
    args = ap.parse_args()
    if args.analyse:
        analyse(args.analyse, args.log, args.threshold, args.pictures)
        return 0
    if not args.recording:
        ap.error("give a recording")
    frames_path, log_path, code = run(args)
    analyse(frames_path, log_path, args.threshold, args.pictures)
    return code


if __name__ == "__main__":
    sys.exit(main())
