#!/usr/bin/env python3
"""What a repeatable Switch run did, from the PS2_FRAME_LOG file (PS2_FRAME_LOG=frames.csv in launch.txt).

  python scripts/frame_stats.py frames.csv [from_frame] [to_frame]

The benchmark in build/switch/tests/Z-diagnose/launch-bench.txt plays a fixed script and stops at frame 2600, so the numbers from
two builds can be compared directly - which the [waits] lines cannot do, because the scene drifts inside one run.

Columns in the file: frame, wall_ms, cpu_ms, wait_window_ms, wait_sound_ms, shapes, interpreted, audio_ms.
A frame's wall_ms is real time, so the frame rate is 1000 / mean(wall_ms); the "busy" columns say what the time went into.
"""
import statistics
import sys


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline()
        if not header.startswith("frame"):
            raise SystemExit(f"{path}: not a PS2_FRAME_LOG file (first line: {header.strip()[:60]})")
        for line in f:
            parts = line.split()
            if len(parts) < 8:
                continue
            try:
                rows.append([float(x) for x in parts[:8]])
            except ValueError:
                continue
    return rows


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    rows = load(sys.argv[1])
    if len(rows) < 10:
        raise SystemExit(f"{sys.argv[1]}: only {len(rows)} frames")
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else float("inf")
    rows = [r for r in rows if lo <= r[0] <= hi]
    if len(rows) < 10:
        raise SystemExit(f"only {len(rows)} frames in that range")
    wall = [r[1] for r in rows]
    mean = statistics.mean(wall)
    print(f"frames {int(rows[0][0])}-{int(rows[-1][0])}: {len(rows)} frames")
    print(f"  frame time  mean {mean:6.2f} ms   median {statistics.median(wall):6.2f}   p95 {sorted(wall)[int(len(wall) * 0.95)]:6.2f}   max {max(wall):7.2f}")
    print(f"  frame rate  mean {1000.0 / mean:6.2f} fps  (from the mean frame time, not from the per-second lines)")
    for name, index in (("cpu", 2), ("waiting for the window", 3), ("waiting for the sound", 4)):
        values = [r[index] for r in rows]
        print(f"  {name:22} mean {statistics.mean(values):6.2f} ms  ({100.0 * statistics.mean(values) / mean:4.1f} % of the frame)")
    print(f"  shapes per frame     mean {statistics.mean([r[5] for r in rows]):8.0f}   (a check that both runs drew the same scene)")
    print(f"  slowest 5 frames     {[round(w, 1) for w in sorted(wall)[-5:]]}")


if __name__ == "__main__":
    main()
