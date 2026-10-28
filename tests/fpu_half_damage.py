#!/usr/bin/env python3
"""God of War II stores an attack's damage as a 16-bit half float and turns it into a float with the Emotion Engine's FPU:
the 15 bits of exponent and fraction are shifted up by 13, the sign by 16, and the result is divided by 2^-112 (0x07800000).
Then the game multiplies it by the difficulty factor (Mortal 0.5, Spartan 1, God 2.5, Titan 5) before it subtracts it from
Kratos' health. This test runs that chain through the runtime's FPU (runtime/include/ps2/fpu.hpp) for all 65536 half
floats and checks every result against exact arithmetic, so a rounding or flushing mistake could not silently shrink or
remove damage. A half with exponent 0 (zero or denormal) must give 0, the EE FPU has no denormals.

  python tests/fpu_half_damage.py [c++ compiler]      (from any directory; needs no game data)
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CXX = sys.argv[1] if len(sys.argv) > 1 else "clang++"

SOURCE = r'''
#include "ps2/fpu.hpp"
#include <cmath>
#include <cstdio>

using namespace ps2::fp;

static double as_double(std::uint32_t x) { return std::bit_cast<float>(x); }

int main() {
    int bad = 0, checked = 0;
    const std::uint32_t kScale = 0x07800000u;  // 2^-112
    const float factors[4] = {0.5f, 1.0f, 2.5f, 5.0f};  // Mortal, Spartan, God, Titan (enemy damage, from the game's table)
    for (std::uint32_t h = 0; h < 0x10000u; ++h) {
        std::uint32_t fcr = 0;
        const std::uint32_t bits = ((h & 0x8000u) << 16) | ((h & 0x7FFFu) << 13);
        const std::uint32_t damage = ee::div(fcr, bits, kScale);
        const unsigned e = (h >> 10) & 31u, m = h & 1023u;
        double want = e ? std::ldexp(1.0 + m / 1024.0, static_cast<int>(e) - 15) : 0.0;
        if (h & 0x8000u) want = -want;
        ++checked;
        if (as_double(damage) != want || (fcr & 0xFFFFu) != 0) {
            if (bad++ < 10) std::printf("half %04X: got %08X (%g), wanted %g, fcr %08X\n", h, damage, as_double(damage), want, fcr);
            continue;
        }
        for (float f : factors) {
            std::uint32_t fc = 0;
            const std::uint32_t scaled = ee::mul(fc, damage, std::bit_cast<std::uint32_t>(f));
            const double exact = want * f;  // every product here is exact in a float: the half has 11 significant bits
            ++checked;
            if (as_double(scaled) != exact) {
                if (bad++ < 10) std::printf("half %04X x %g: got %08X (%g), wanted %g\n", h, f, scaled, as_double(scaled), exact);
            }
        }
    }
    std::printf("%d checks, %d wrong\n", checked, bad);
    return bad != 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "half.cpp"
        exe = pathlib.Path(tmp) / "half.exe"
        src.write_text(SOURCE)
        build = subprocess.run([CXX, "-std=c++20", "-O1", "-I", str(ROOT / "runtime" / "include"), str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            sys.exit(build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout.strip())
        if run.returncode:
            sys.exit("FAIL: the FPU chain gives a wrong damage value")
        print("PASS")


if __name__ == "__main__":
    main()
