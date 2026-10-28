#!/usr/bin/env python3
"""Show Kratos' health and magic as saved in God of War II save states (USA disc, SCUS_974.81).

  python scripts/gow2_health.py games/gow2/play/gow2_f055800.state games/gow2/play/gow2_f059400.state

The game keeps a copy of the numbers the HUD draws at four floats in main memory (health, health maximum, magic, magic
maximum, 0x002D8748). A save state holds the main memory from byte 1476 on (docs/gow2-play.md). The copy is one
frame behind the real value, which lives in Kratos' actor record; for this lane's tests that does not matter.
"""
import struct
import sys

RAM_AT = 1476          # byte of a state file where the 32 MiB of main memory start
HUD = 0x002D8748       # health, health maximum, magic, magic maximum (USA disc)


def read_hud(path):
    with open(path, "rb") as f:
        f.seek(RAM_AT + HUD)
        return struct.unpack("<4f", f.read(16))


def main(paths):
    if not paths:
        sys.exit(__doc__)
    for path in paths:
        health, health_max, magic, magic_max = read_hud(path)
        print(f"{path}: health {health:g} of {health_max:g} ({100 * health / health_max:.1f} %), magic {magic:g} of {magic_max:g}")


if __name__ == "__main__":
    main(sys.argv[1:])
