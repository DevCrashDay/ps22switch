#!/usr/bin/env python3
"""Play a game in short stretches: run some frames with scripted buttons, look at the pictures, decide, continue.

  python scripts/play.py --frames 600 --moves "300:start 600:cross"            # from the power-on, 600 frames
  python scripts/play.py --from games/gow2/play/gow2_f000600.state --frames 900 --moves "0:lup:200 250:square:3@12*20"

Every stretch ends with a save state named after the frame it stopped at (games/<game>/play/<game>_f<frame>.state, give
the next stretch --from that file) and a handful of small pictures (PNG) to look at. The buttons are written relative to
the stretch start, so the same moves work from any state. A move is
    WHEN:BUTTONS[:LENGTH][@PERIOD*COUNT]
WHEN and LENGTH are frames (default length 8; a menu missed presses of 6 frames), BUTTONS joins ps2run's names with +: cross, circle, square, triangle, start,
select, up, down, left, right, l1, l2, r1, r2, l3, r3 and the left stick as lup, ldown, lleft, lright (full tilt; lup+lright is a
diagonal) or ls<degrees> / rs<degrees> for the right stick (full tilt towards an angle: ls0 up, ls90 right, ls180 down, ls270 left, ls45 up-right). A short
length is a tap of the stick. `@12*20` repeats the move 20 times, 12 frames apart (button mashing). Moves are separated by spaces or new lines, `#` starts a comment.
Every stretch is appended to games/<game>/play/log.txt. More: docs/gow2-play.md.
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOVE = re.compile(r"^(\d+):([a-z0-9+]+)(?::(\d+))?(?:@(\d+)\*(\d+))?$")
# what is worth a line in the summary; everything else stays in the full log next to the pictures
INTERESTING = re.compile(r"unhandled|missing|halted|cannot|error|fault|warn|\[mc\]|\[disc\]|\[state\]|\[ee\] stopped|\[gpu\]|unknown|unsupported", re.I)


def stick_axes(name):
    """The left stick of one button name as {axis: value} (0x80 is the rest position), or None for an ordinary button.
    lup, ldown, lleft, lright are full tilt; ls<degrees> tilts the stick fully towards that angle, 0 up, 90 right, 180 down,
    270 left (ls45 is the same as lup+lright)."""
    digital = {"lup": ("ly", 0x00), "ldown": ("ly", 0xFF), "lleft": ("lx", 0x00), "lright": ("lx", 0xFF)}
    if name in digital:
        axis, value = digital[name]
        return {axis: value}
    m = re.fullmatch(r"([lr])s(\d{1,3})", name)  # ls = left stick, rs = right stick (dodging in God of War II)
    if not m:
        return None
    x, y = math.sin(math.radians(int(m.group(2)))), -math.cos(math.radians(int(m.group(2))))
    scale = 1 / max(abs(x), abs(y))  # the stick's corners, like the digital diagonal
    side = m.group(1)
    return {side + "x": max(0, min(255, round(127.5 + 127.5 * x * scale))), side + "y": max(0, min(255, round(127.5 + 127.5 * y * scale)))}


def parse_moves(text, start):
    """The moves as the arguments for ps2run: ["--press", "F:buttons:len", "--axis", "F:lx:value:len", ...]."""
    args = []
    for line in text.splitlines():
        for token in line.split("#")[0].split():
            m = MOVE.match(token)
            if not m:
                sys.exit(f"bad move '{token}' (expected WHEN:BUTTONS[:LENGTH][@PERIOD*COUNT])")
            when, buttons, length, period, count = m.groups()
            keys, axes = [], {}
            for name in buttons.split("+"):
                tilt = stick_axes(name)
                if tilt is None:
                    keys.append(name)
                else:
                    axes.update(tilt)
            for i in range(int(count) if count else 1):
                at = start + int(when) + i * int(period or 0)
                if keys:
                    args += ["--press", f"{at}:{'+'.join(keys)}:{length or 8}"]
                for axis, value in axes.items():
                    args += ["--axis", f"{at}:{axis}:{value}:{length or 8}"]
    return args


def read_moves(text):
    """The moves as a list of tokens: comments dropped, @file replaced by what the file holds."""
    tokens = []
    for line in text.splitlines():
        for token in line.split("#")[0].split():
            tokens += read_moves(Path(token[1:]).read_text()) if token.startswith("@") else [token]
    return tokens


def find_program(game):
    disc = ROOT / "games" / game / "disc"
    for path in sorted(disc.iterdir()) if disc.is_dir() else []:
        if re.fullmatch(r"[A-Z]{4}_\d{3}\.\d{2}", path.name) or path.name == "core.elf":
            return path
    sys.exit(f"no game program in {disc}: build the game first (scripts/port.sh)")


def contact_sheet(pngs, sheet, columns):
    from PIL import Image, ImageDraw
    tiles = [Image.open(p) for p in pngs]
    w, h = tiles[0].size
    rows = -(-len(tiles) // columns)
    image = Image.new("RGB", (w * columns, h * rows))
    for i, tile in enumerate(tiles):
        image.paste(tile.convert("RGB"), (i % columns * w, i // columns * h))
        ImageDraw.Draw(image).text((i % columns * w + 4, i // columns * h + 4), pngs[i].stem.split("_")[-1], fill=(255, 255, 0))
    image.save(sheet)


def to_png(ppm, png, width):
    from PIL import Image  # pip install pillow
    image = Image.open(ppm)
    if image.width > width:
        image = image.resize((width, round(image.height * width / image.width)), Image.LANCZOS)
    image.save(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog="Extra ps2run options go after --.")
    ap.add_argument("--game", default="gow2")
    ap.add_argument("--from", dest="source", help="state file to continue from (default: power-on)")
    ap.add_argument("--frames", type=int, required=True, help="length of this stretch in frames")
    ap.add_argument("--moves", default="", help="buttons, see above; @file inside it is replaced by that file's moves")
    ap.add_argument("--shots", type=int, default=6, help="pictures to take, spread over the stretch (default 6)")
    ap.add_argument("--width", type=int, default=320, help="width of one picture in pixels (default 320)")
    ap.add_argument("--cols", type=int, default=3, help="pictures per row in the all-in-one sheet (default 3)")
    ap.add_argument("--memcard", help="memory card file (default: play/<game>.ps2)")
    ap.add_argument("--headless", action="store_true", help="no window (one thread, slower; vector programs are not recorded)")
    ap.add_argument("--note", default="", help="a line for the log")
    ap.add_argument("extra", nargs="*", help="more ps2run options")
    a = ap.parse_args()

    play = ROOT / "games" / a.game / "play"
    play.mkdir(parents=True, exist_ok=True)
    exe = ROOT / "build" / f"native-{a.game}" / ("ps2run.exe" if os.name == "nt" else "ps2run")
    program = find_program(a.game)
    disc = ROOT / "games" / a.game / "disc.iso"
    start = 0
    if a.source:
        m = re.search(r"_f(\d+)", Path(a.source).name)
        if not m:
            sys.exit("the state file name must contain _f<frame>, like the ones this script writes")
        start = int(m.group(1))
    every = max(1, a.frames // max(1, a.shots))
    end = -(-(start + a.frames) // every) * every  # the last picture is of the last frame
    save = play / f"{a.game}_f{end:06d}.state"  # the name carries the frame: the next stretch needs it
    moves = " ".join(read_moves(a.moves))
    pictures = play / "ppm"
    pictures.mkdir(exist_ok=True)
    for old in pictures.glob("*.ppm"):
        old.unlink()

    cmd = [str(exe), str(program), "--disc", str(disc), "--memcard", a.memcard or str(play / f"{a.game}.ps2"),
           "--frames", str(end + 1), "--frames-dir", str(pictures), "--frame-every", str(every), "--save-state", f"{end}:{save}"]
    if a.source:
        cmd += ["--load-state", a.source]
    if not a.headless:
        cmd.append("--window")
    cmd += parse_moves(moves, start)
    cmd += a.extra
    log = play / "last-run.log"
    t0 = time.time()
    with open(log, "wb") as out:
        code = subprocess.run(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT).returncode
    seconds = time.time() - t0
    lines = log.read_text(errors="replace").splitlines()
    seen, summary = set(), []
    for line in lines:
        key = re.sub(r"\d+", "#", line)  # the same complaint over and over is shown once
        if INTERESTING.search(line) and key not in seen:
            seen.add(key)
            summary.append(line)
    for line in summary[:40]:
        print(line)
    if len(summary) > 40:
        print(f"... {len(summary) - 40} more different lines in {log}")
    shots = []
    for ppm in sorted(pictures.glob("frame_*.ppm")):
        png = play / f"{a.game}_{ppm.stem[len('frame_'):]}.png"
        to_png(ppm, png, a.width)
        ppm.unlink()
        shots.append(png)
    for old in sorted(play.glob(f"{a.game}_?????.png"), key=os.path.getmtime)[:-100]:  # keep the newest hundred
        if old not in shots:
            old.unlink()
    verdict = "ok" if code == 0 and save.exists() else f"EXIT {code}, state {'written' if save.exists() else 'NOT written'}"
    print(f"frames {start} -> {end} in {seconds:.0f} s ({(end - start) / max(seconds, 1):.1f} fps, rough): {verdict}")
    print(f"state: {save}")
    if shots:
        sheet = play / "sheet.png"
        contact_sheet(shots, sheet, a.cols)
        print(f"pictures (frame numbers on them), all in one: {sheet}")
        print("single pictures: " + ", ".join(png.name for png in shots) + f" in {play}")
    with open(play / "log.txt", "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M')} frames {start}->{end} {verdict} state={save.name} from={Path(a.source).name if a.source else 'power-on'}"
                f" moves=[{moves[:200]}] {a.note}\n")
    return 0 if verdict == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
