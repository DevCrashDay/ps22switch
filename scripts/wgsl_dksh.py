#!/usr/bin/env python3
"""Turns the hardware renderer's programs (runtime/src/gs/gpu/hw.wgsl, blit.wgsl) into deko3d shaders (.dksh) and writes
runtime/src/gs/gpu/deko/dksh_table.cpp, which the build compiles in. The deko3d counterpart of scripts/wgsl_spirv.py.

  python scripts/wgsl_dksh.py [--naga path/to/naga] [--uam path/to/uam]

Two programs are needed, and neither speaks WGSL:

  naga  WGSL -> GLSL 450, one entry point at a time (naga-cli; the same program wgsl_spirv.py uses)
  uam   GLSL -> deko3d shader module, for the Tegra X1 (github.com/PalindromicBreadLoaf/uam, zlib licence; meson build:
        meson setup build && ninja -C build, then build/uam)

uam wants every uniform block, storage block, sampler and image to say its binding (`layout(binding = N)`), because that number is
the deko3d binding ID. naga does not write bindings, but it does name each block after the binding it came from
(`_group_0_binding_3_vs`), so this script puts the number back. deko3d allows 16 UBOs, 16 SSBOs, 32 samplers and 8 images per stage,
which is what the WebGPU bindings of these shaders already fit into.

naga is told `--keep-coordinate-space`. Without it, its GLSL writer adds the adjustment a *Vulkan* driver needs — it negates
gl_Position.y and rewrites z as `z * 2 - w`, so that WebGPU's 0..1 depth lands in Vulkan's -1..1. deko3d's depth is 0..1 like WebGPU's,
so that rewrite puts every vertex behind the near plane and nothing is drawn, with no error from anywhere: the first console run of the
triangle showed exactly that. The Y axis is the device's business instead (`DkDeviceFlags_OriginLowerLeft`), not the shader's.
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
DEKO_DIR = ROOT / "runtime" / "src" / "gs" / "gpu" / "deko"
SHADERS = [ROOT / "runtime" / "src" / "gs" / "gpu" / "hw.wgsl", ROOT / "runtime" / "src" / "gs" / "gpu" / "gs.wgsl",
           ROOT / "runtime" / "src" / "gs" / "gpu" / "blit.wgsl",
           DEKO_DIR / "stage_draw.wgsl"]
OUT = DEKO_DIR / "dksh_table.cpp"

# entry point -> naga's extension (which picks the GLSL stage) and uam's name for the same stage
STAGES = {"vert": ("vert", "vert"), "frag": ("frag", "frag"), "comp": ("comp", "comp")}
# What the renderer draws with, and the memory moves it can do on the GPU, per shader file. hw_writeback_depth is the exception: it
# reads back through a textureLoad of a depth texture, which naga cannot write as GLSL at all ("not supported in GLSL"). The colour
# writeback next to it is a separate program and compiles, which is what matters: only the depth half of the writeback is missing.
WANTED = {
    "hw.wgsl": ("hw_vs", "hw_full_vs", "hw_fs", "hw_fs_single", "hw_load_depth", "hw_load_color", "hw_writeback"),
    # the hardware path still uses these for video memory: uploads, the two halves of a local copy, CLUT loads, texture decoding, the
    # display (which is where the picture on the screen comes from), the snapshot and the resolve. `raster` is not needed: hw.wgsl draws
    # the triangles on this path.
    "gs.wgsl": ("upload", "copy_read", "copy_write", "load_clut", "display", "decode", "snapshot", "resolve"),
    "blit.wgsl": ("vs", "fs"),
    "stage_draw.wgsl": ("triangle_vs", "triangle_fs"),
}
NOT_YET = ("hw_writeback_depth",)




def wgsl_hash(text: str) -> int:
    h = 1469598103934665603
    for b in text.replace("\r", "").encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def entry_points(text: str) -> dict:
    """{name: stage} for every @vertex/@fragment/@compute function in a WGSL module."""
    found = {}
    for stage, name in re.findall(r"@(vertex|fragment|compute)\b[^\n]*\n\s*fn\s+(\w+)\s*\(", text):
        found[name] = {"vertex": "vert", "fragment": "frag", "compute": "comp"}[stage]
    return found


def bindings(text: str) -> dict:
    """{name: (group, binding)} for every @group(G) @binding(N) global in a WGSL module."""
    return {name: (int(g), int(n)) for g, n, name in re.findall(r"@group\((\d+)\)\s*@binding\((\d+)\)\s*var(?:<[^>]*>)?\s+(\w+)", text)}


def add_bindings(glsl: str, wgsl: str) -> str:
    """naga leaves the binding off each global; uam needs it, and it is the deko3d binding ID the pipeline must match."""
    out = []
    for line in glsl.splitlines():
        layout = re.match(r"\s*layout\(([^)]*)\)", line)
        # uniform and storage blocks, samplers and images: any of them, naga writes no binding:
        #   layout(std140) uniform Name { ... }   layout(std430) readonly buffer Name { ... }
        #   layout(rgba8) writeonly uniform image2D Name;   layout(...) uniform sampler2D Name;
        if layout and "binding" not in layout.group(1) and re.search(r"\b(uniform|buffer)\s", line):
            found = re.search(r"_group_\d+_binding_(\d+)", line)
            if not found:
                sys.exit(f"naga wrote a global this script cannot give a binding to:\n{line}")
            line = line[:layout.start(1)] + f"{layout.group(1)}, binding = {found.group(1)}" + line[layout.end(1):]
        elif re.match(r"\s*uniform\s+\w*(sampler|image)\w*", line) and "layout(" not in line:
            # a sampler or image, which naga writes with no layout at all: uam still needs the binding, and it is the same number
            found = re.search(r"_group_\d+_binding_(\d+)", line)
            if not found:
                sys.exit(f"naga wrote a sampler or image this script cannot give a binding to:\n{line}")
            line = f"layout(binding = {found.group(1)}) " + line.lstrip()
        elif re.search(r"\b(sampler\w*|image\w*)\b", line) and "binding" not in line:
            # a global of a kind this script does not know how to bind: guessing would be worse than stopping
            sys.exit(f"naga wrote a global this script cannot give a binding to:\n{line}")
        out.append(line)
    used = set(re.findall(r"_group_\d+_binding_(\d+)", glsl))
    known = {str(n) for _, n in bindings(wgsl).values()}
    if used - known:
        sys.exit(f"naga used bindings this shader does not declare: {', '.join(sorted(used - known))}")
    return "\n".join(out) + "\n"


def compile_program(naga, uam, wgsl_path: Path, wgsl_text: str, entry: str, stage: str, tmp: str):
    ext, uam_stage = STAGES[stage]
    glsl = Path(tmp) / f"{entry}.{ext}"
    dksh = Path(tmp) / f"{entry}.dksh"
    r = subprocess.run([naga, "--profile", "core450", "--keep-coordinate-space", "--entry-point", entry, str(wgsl_path), str(glsl)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not glsl.is_file():
        sys.exit(f"naga failed on {wgsl_path.name}:{entry}:\n{r.stdout}{r.stderr}")
    glsl.write_text(add_bindings(glsl.read_text(encoding="utf-8"), wgsl_text), encoding="utf-8", newline="\n")
    r = subprocess.run([uam, str(glsl), "-s", uam_stage, "-o", str(dksh)], capture_output=True, text=True)
    if r.returncode != 0 or not dksh.is_file():
        sys.exit(f"uam failed on {wgsl_path.name}:{entry}:\n{r.stdout}{r.stderr}")
    data = dksh.read_bytes()
    if len(data) < 16 or len(data) % 4:
        sys.exit(f"uam's output for {entry} is not a deko3d shader module ({len(data)} bytes)")
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--naga", default=os.environ.get("NAGA", "naga"), help="the naga program (naga-cli)")
    parser.add_argument("--uam", default=os.environ.get("UAM", "uam"), help="the uam program (deko3d's shader compiler)")
    args = parser.parse_args()
    naga = shutil.which(args.naga) or args.naga
    uam = shutil.which(args.uam) or args.uam

    programs = []  # (name, stage, words)
    hashes = []
    with tempfile.TemporaryDirectory() as tmp:
        for path in SHADERS:
            text = path.read_text(encoding="utf-8")
            hashes.append((path, hashlib.sha256(text.replace("\r", "").encode("utf-8")).hexdigest()))
            stages = entry_points(text)
            for entry in WANTED[path.name]:
                if entry not in stages:
                    sys.exit(f"{path.name} no longer has {entry}: update WANTED in this script")
                words = compile_program(naga, uam, path, text, entry, stages[entry], tmp)
                programs.append((entry, stages[entry], words))
                print(f"{path.name}:{entry} ({stages[entry]}): {len(words) * 4} bytes")
            for entry in sorted(stages):
                if entry not in WANTED[path.name]:
                    print(f"{path.name}:{entry} ({stages[entry]}): not compiled"
                          f"{' (see NOT_YET)' if entry in NOT_YET else ''}")

    lines = [
        "// Made by scripts/wgsl_dksh.py from " + ", ".join(p.name for p in SHADERS) +
        ": do not edit, run the script again after changing them.",
        *(f"// {path.name} sha256 {sha}" for path, sha in hashes),
        '#include "dksh.hpp"',
        "",
        "#include <cstring>",
        "",
        "namespace ps2::gs::gpu::deko {",
        "",
        "namespace {",
    ]
    for n, (_, _, words) in enumerate(programs):
        # deko3d reads a module's control block from this address: keep it aligned like the file format's own sections
        lines.append(f"alignas(256) const std::uint32_t kProgram{n}[] = {{")
        for i in range(0, len(words), 12):
            lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 12]) + ",")
        lines.append("};")
    lines += ["}  // namespace", "", "const DkshProgram kDkshPrograms[] = {"]
    for n, (name, stage, words) in enumerate(programs):
        lines.append(f'    {{"{name}", DkshStage_{stage.capitalize()}, kProgram{n}, sizeof kProgram{n} / 4}},')
    lines += ["};", "const std::size_t kDkshProgramCount = sizeof kDkshPrograms / sizeof kDkshPrograms[0];", "",
              "const DkshProgram *dksh_program(const char *name) {",
              "    for (std::size_t i = 0; i < kDkshProgramCount; ++i)",
              "        if (std::strcmp(kDkshPrograms[i].name, name) == 0) return &kDkshPrograms[i];",
              "    return nullptr;",
              "}",
              "", "}  // namespace ps2::gs::gpu::deko", ""]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
