#!/usr/bin/env python3
"""Turns the graphics chip's drawing program (runtime/src/gs/gpu/gs.wgsl) into SPIR-V for the Vulkan renderer
(runtime/src/gs/gpu/vk/wgpu_vk.cpp) and writes runtime/src/gs/gpu/vk/spirv_table.cpp, which the build compiles in.

Run it after every change to gs.wgsl (the build stops with a message when the table is older than the shader):
  python scripts/wgsl_spirv.py [--naga path/to/naga]

It needs naga (cargo install naga-cli). WGSL "override" constants become plain constants, one SPIR-V module per set of values
(VARIANTS below), because SPIR-V has no overrides; at run time the pipeline's constants pick the module.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WGSL = ROOT / "runtime" / "src" / "gs" / "gpu" / "gs.wgsl"
HW_WGSL = ROOT / "runtime" / "src" / "gs" / "gpu" / "hw.wgsl"
OUT = ROOT / "runtime" / "src" / "gs" / "gpu" / "vk" / "spirv_table.cpp"

# (SCALE, TILE, EXPLICIT_FMA): the tile sizes gpu.cpp takes (PS2_TILE=4, 8 or 16), with and without the sums spelled out as fma()
VARIANTS = [(1, tile, fma) for tile in (4, 8, 16) for fma in (False, True)]


def wgsl_hash(text: str) -> int:
    h = 1469598103934665603
    for b in text.replace("\r", "").encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def specialise(text: str, scale: int, tile: int, fma: bool) -> str:
    values = {"SCALE": f"{scale}u", "TILE": f"{tile}u", "EXPLICIT_FMA": "true" if fma else "false"}
    seen = set()

    def repl(m):
        name, ty = m.group(1), m.group(2)
        if name not in values:
            sys.exit(f"gs.wgsl has an override this script does not know: {name}")
        seen.add(name)
        return f"const {name}: {ty} = {values[name]};"

    out = re.sub(r"override\s+(\w+)\s*:\s*(\w+)\s*=\s*[^;]+;", repl, text)
    missing = set(values) - seen
    if missing:
        sys.exit(f"gs.wgsl no longer declares {', '.join(sorted(missing))}: update VARIANTS in this script")
    return out


def compile_wgsl(naga, text, name, tmp, extra=()):
    src = Path(tmp) / f"{name}.wgsl"
    spv = src.with_suffix(".spv")
    src.write_text(text, encoding="utf-8", newline="\n")
    # every index clamped into its array (branch-free), as WebGPU allows: the driver needs no robust buffer access, which NVK does
    # with a branch around every memory access
    r = subprocess.run([naga, "--index-bounds-check-policy", "Restrict", "--buffer-bounds-check-policy", "Restrict", *extra, str(src), str(spv)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not spv.is_file():
        sys.exit(f"naga failed on {name}:\n{r.stdout}{r.stderr}")
    data = spv.read_bytes()
    if len(data) % 4 or int.from_bytes(data[:4], "little") != 0x07230203:
        sys.exit("naga's output is not SPIR-V")
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--naga", default=os.environ.get("NAGA", "naga"), help="the naga program (naga-cli)")
    args = parser.parse_args()
    naga = shutil.which(args.naga) or args.naga
    text = WGSL.read_text(encoding="utf-8")
    hw_text = HW_WGSL.read_text(encoding="utf-8")
    h = wgsl_hash(text)
    hw_h = wgsl_hash(hw_text)
    modules = []  # (hash, scale, tile, fma, words); tile 0: a program without overrides, any constants
    with tempfile.TemporaryDirectory() as tmp:
        for scale, tile, fma in VARIANTS:
            words = compile_wgsl(naga, specialise(text, scale, tile, fma), f"gs_{scale}_{tile}_{int(fma)}", tmp)
            modules.append((h, scale, tile, fma, words))
            print(f"gs.wgsl SCALE={scale} TILE={tile} EXPLICIT_FMA={int(fma)}: {len(words)} words")
        words = compile_wgsl(naga, hw_text, "hw", tmp, ("--capabilities", "dual_source_blending"))
        modules.append((hw_h, 0, 0, False, words))
        print(f"hw.wgsl: {len(words)} words")

    sha = lambda t: hashlib.sha256(t.replace(chr(13), '').encode('utf-8')).hexdigest()
    lines = [
        "// Made by scripts/wgsl_spirv.py from runtime/src/gs/gpu/gs.wgsl and hw.wgsl: do not edit, run the script again after changing them.",
        f"// gs.wgsl hash {h:016x}",
        f"// gs.wgsl sha256 {sha(text)}",
        f"// hw.wgsl sha256 {sha(hw_text)}",
        '#include "spirv.hpp"',
        "",
        "namespace ps2::gs::gpu::vk {",
        "",
        "namespace {",
    ]
    for n, (_, scale, tile, fma, words) in enumerate(modules):
        lines.append(f"const std::uint32_t kWords{n}[] = {{")
        for i in range(0, len(words), 12):
            lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 12]) + ",")
        lines.append("};")
    lines += ["}  // namespace", "", "const SpirvVariant kSpirvVariants[] = {"]
    for n, (mh, scale, tile, fma, words) in enumerate(modules):
        lines.append(f"    {{0x{mh:016x}ull, {scale}, {tile}, {'true' if fma else 'false'}, kWords{n}, sizeof kWords{n} / 4}},")
    lines += ["};", "const std::size_t kSpirvVariantCount = sizeof kSpirvVariants / sizeof kSpirvVariants[0];", "", "}  // namespace ps2::gs::gpu::vk", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
