// needs WebGPU's dual-source-blending feature for hw_fs (hw_fs_single without it)
enable dual_source_blending;

// The hardware-rasterizer path of the GS renderer (docs/switch-hw-renderer.md): the shapes of a batch are drawn as triangles by the
// GPU's rasterizer into a colour and a depth image, which stand for the batch's frame and depth buffers. The images are filled from
// PS2 video memory before the batch (hw_load_color, hw_load_depth) and written back to it after (hw_writeback), inside the batch's
// bounding box, so video memory stays the truth for everything else (uploads, CLUTs, copies, the display, textures).
//
// Done: Gouraud and flat colour, textures read from video memory (every format, CLUTs, nearest and bilinear, the wrap modes), the
// texture functions, fog, the alpha test (as far as discarding goes) and the depth test. To come: blending, the destination alpha
// test, lines and points.

struct Params {
    v: array<vec4<u32>, 16>,
}

@group(0) @binding(0) var<storage, read_write> vram: array<atomic<u32>>;
@group(0) @binding(1) var<storage, read> tables: array<u32>;
@group(0) @binding(2) var<uniform> params: Params;
@group(0) @binding(3) var<storage, read> data: array<u32>;
@group(0) @binding(4) var<storage, read> clut: array<u32>;

@group(1) @binding(0) var color_out: texture_storage_2d<rgba8unorm, write>;
@group(2) @binding(0) var color_in: texture_2d<f32>;
@group(2) @binding(1) var depth_in: texture_depth_2d;

const PRIM_WORDS: u32 = 28u;
const SETUP_WORDS: u32 = 40u;

const T_OFF32: u32 = 0u;
const T_OFFZ32: u32 = 2048u;
const T_OFF16: u32 = 4096u;
const T_OFF16S: u32 = 8192u;
const T_OFFZ16: u32 = 12288u;
const T_OFFZ16S: u32 = 16384u;
const T_OFF8: u32 = 20480u;
const T_OFF4: u32 = 28672u;

fn prm(i: u32) -> u32 {
    return params.v[i / 4u][i % 4u];
}

struct Loc {
    word: u32,
    shift: u32,
    mask: u32,
}

fn page_addr(bp: u32, bw: u32, x: u32, y: u32, pw: u32, ph: u32) -> u32 {
    let per_row = max(1u, bw * 64u / pw);
    return bp * 256u + ((x / pw) + (y / ph) * per_row) * 8192u;
}

// The frame and depth formats only (the texture formats come with milestone 2)
fn locate(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> Loc {
    var a = 0u;
    var shift = 0u;
    var mask = 0xFFFFFFFFu;
    switch psm {
        case 0x30u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFFZ32 + (y & 31u) * 64u + (x & 63u)];
        }
        case 0x31u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFFZ32 + (y & 31u) * 64u + (x & 63u)];
            mask = 0xFFFFFFu;
        }
        case 0x01u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
            mask = 0xFFFFFFu;
        }
        case 0x02u {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFF16 + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x0Au {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFF16S + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x32u {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFFZ16 + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x3Au {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFFZ16S + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        default {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
        }
    }
    return Loc((a & 0x3FFFFFu) >> 2u, shift, mask);
}

fn read_pixel(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> u32 {
    let l = locate(psm, bp, bw, x, y);
    return (atomicLoad(&vram[l.word]) >> l.shift) & l.mask;
}

// value under keep (bits set: left as they are)
fn write_masked(psm: u32, bp: u32, bw: u32, x: u32, y: u32, value: u32, keep: u32) {
    let l = locate(psm, bp, bw, x, y);
    let m = (l.mask & ~keep) << l.shift;
    if (m == 0xFFFFFFFFu) {
        atomicStore(&vram[l.word], value);
        return;
    }
    if (m == 0u) {
        return;
    }
    atomicAnd(&vram[l.word], ~m);
    atomicOr(&vram[l.word], (value << l.shift) & m);
}

fn is16(psm: u32) -> bool {
    return psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au;
}

// A frame-buffer word as RGBA8 (24-bit: alpha 0x80; 16-bit: 5:5:5:1 with alpha 0x80 or 0)
fn frame_rgba(psm: u32, raw: u32) -> u32 {
    if (psm == 0x01u) {
        return raw | 0x80000000u;
    }
    if (is16(psm)) {
        var a = 0u;
        if ((raw & 0x8000u) != 0u) {
            a = 0x80u;
        }
        return ((raw & 0x1Fu) << 3u) | (((raw >> 5u) & 0x1Fu) << 11u) | (((raw >> 10u) & 0x1Fu) << 19u) | (a << 24u);
    }
    return raw;
}

fn rgba_frame(psm: u32, c: u32) -> u32 {
    if (is16(psm)) {
        var a = 0u;
        if ((c & 0x80000000u) != 0u) {
            a = 0x8000u;
        }
        return ((c >> 3u) & 0x1Fu) | (((c >> 11u) & 0x1Fu) << 5u) | (((c >> 19u) & 0x1Fu) << 10u) | a;
    }
    return c;
}

fn unpack(c: u32) -> vec4<f32> {
    return vec4<f32>(f32(c & 0xFFu), f32((c >> 8u) & 0xFFu), f32((c >> 16u) & 0xFFu), f32(c >> 24u)) / 255.0;
}

fn pack(c: vec4<f32>) -> u32 {
    let b = vec4<u32>(clamp(round(c * 255.0), vec4<f32>(0.0), vec4<f32>(255.0)));
    return b.x | (b.y << 8u) | (b.z << 16u) | (b.w << 24u);
}

// --- filling the images from video memory and writing them back -----------------------------------------------------------------
// params: 0 fbp, 1 fbw, 2 fpsm, 3 zbp, 4 zpsm, 5 box x0 | y0 << 16, 6 box x1 | y1 << 16, 7 zmax, 8 frame mask (FBMSK), 9 flags
// (1: write the depth back), 10 target width, 11 target height

fn box_pixel(gid: vec3<u32>, xy: ptr<function, vec2<u32>>) -> bool {
    let x0 = prm(5u) & 0xFFFFu;
    let y0 = prm(5u) >> 16u;
    let x1 = prm(6u) & 0xFFFFu;
    let y1 = prm(6u) >> 16u;
    let x = x0 + gid.x;
    let y = y0 + gid.y;
    *xy = vec2<u32>(x, y);
    return x <= x1 && y <= y1;
}

@compute @workgroup_size(8, 8)
fn hw_load_color(@builtin(global_invocation_id) gid: vec3<u32>) {
    var xy: vec2<u32>;
    if (!box_pixel(gid, &xy)) {
        return;
    }
    let psm = prm(2u);
    let raw = read_pixel(psm, prm(0u), prm(1u), xy.x, xy.y);
    textureStore(color_out, vec2<i32>(xy), unpack(frame_rgba(psm, raw)));
}

@compute @workgroup_size(8, 8)
fn hw_writeback(@builtin(global_invocation_id) gid: vec3<u32>) {
    var xy: vec2<u32>;
    if (!box_pixel(gid, &xy)) {
        return;
    }
    let psm = prm(2u);
    let c = pack(textureLoad(color_in, vec2<i32>(xy), 0));
    var keep = prm(8u);
    if (psm == 0x01u) {
        keep = keep | 0xFF000000u;  // a 24-bit frame keeps bits 24 to 31 of the word
    }
    if (is16(psm)) {
        keep = rgba_frame(psm, keep) | select(0u, 0x8000u, (keep & 0x80000000u) != 0u);
    }
    write_masked(psm, prm(0u), prm(1u), xy.x, xy.y, rgba_frame(psm, c), keep);
    if ((prm(9u) & 1u) != 0u) {
        let zmax = prm(7u);
        let d = textureLoad(depth_in, vec2<i32>(xy), 0);
        let z = u32(clamp(round(d * f32(zmax)), 0.0, f32(zmax)));
        write_masked(prm(4u), prm(3u), prm(1u), xy.x, xy.y, z, 0u);
    }
}

// The depth image is filled by a full-screen triangle that writes the depth (the box is the scissor rectangle)
struct FullOut {
    @builtin(position) position: vec4<f32>,
}

@vertex
fn hw_full_vs(@builtin(vertex_index) i: u32) -> FullOut {
    var out: FullOut;
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    out.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

@fragment
fn hw_load_depth(in: FullOut) -> @builtin(frag_depth) f32 {
    let x = u32(in.position.x);
    let y = u32(in.position.y);
    let zmax = prm(7u);
    let z = read_pixel(prm(4u), prm(3u), prm(1u), x, y) & zmax;
    return f32(z) / f32(zmax);
}

// --- textures, as gs.wgsl reads them --------------------------------------------------------------------------------------------

const CLUT_ENTRIES: u32 = 512u;
const F_IIP: u32 = 1u;
const F_TME: u32 = 2u;
const F_FGE: u32 = 4u;
const F_ABE: u32 = 8u;
const F_FST: u32 = 16u;
const F_ATE: u32 = 64u;
const F_FBA: u32 = 4096u;
const F_TCC: u32 = 8192u;
const F_BILINEAR: u32 = 16384u;
const F_AEM: u32 = 32768u;

struct Fmt {
    base: u32,
    per_row: u32,
    table: u32,
    bits: u32,
    mask: u32,
}

struct Tex {
    flags: u32,
    modes: u32,
    aref: u32,
    tw: u32,
    th: u32,
    cpsm: u32,
    csa: u32,
    minu: u32,
    maxu: u32,
    minv: u32,
    maxv: u32,
    ta0: u32,
    ta1: u32,
    fogcol: u32,
    clut: u32,
    tpsm: u32,
    tf: Fmt,
}

fn load_tex(at: u32) -> Tex {
    var t: Tex;
    t.flags = data[at];
    t.modes = data[at + 7u];
    t.aref = data[at + 8u];
    t.tpsm = data[at + 14u];
    t.tw = data[at + 15u] & 0xFFu;
    t.th = data[at + 15u] >> 8u;
    t.cpsm = data[at + 16u];
    t.csa = data[at + 17u];
    t.minu = data[at + 18u] & 0xFFFFu;
    t.maxu = data[at + 18u] >> 16u;
    t.minv = data[at + 19u] & 0xFFFFu;
    t.maxv = data[at + 19u] >> 16u;
    t.ta0 = data[at + 20u] & 0xFFu;
    t.ta1 = (data[at + 20u] >> 8u) & 0xFFu;
    t.fogcol = data[at + 21u];
    t.clut = data[at + 22u];
    t.tf = Fmt(data[at + 25u], data[at + 26u], data[at + 27u], data[at + 28u], data[at + 29u]);
    return t;
}

fn has(t: Tex, f: u32) -> bool {
    return (t.flags & f) != 0u;
}

fn mode(t: Tex, shift: u32, bits: u32) -> u32 {
    return (t.modes >> shift) & ((1u << bits) - 1u);
}

fn addr(f: Fmt, x: u32, y: u32) -> Loc {
    let pws = f.bits & 15u;
    let phs = (f.bits >> 4u) & 15u;
    let amask = (f.bits >> 8u) & 3u;
    let nbit = (f.bits >> 10u) & 1u;
    let n = tables[f.table + ((y & ((1u << phs) - 1u)) << pws) + (x & ((1u << pws) - 1u))];
    let a = f.base + ((x >> pws) + (y >> phs) * f.per_row) * 8192u + (n >> nbit);
    return Loc((a & 0x3FFFFFu) >> 2u, (f.bits >> 11u) + (a & amask) * 8u + (n & nbit) * 4u, f.mask);
}

fn expand16(c: u32, t: Tex) -> u32 {
    let r = (c & 0x1Fu) << 3u;
    let g = ((c >> 5u) & 0x1Fu) << 3u;
    let b = ((c >> 10u) & 0x1Fu) << 3u;
    var a = t.ta0;
    if ((c & 0x8000u) != 0u) {
        a = t.ta1;
    } else if (has(t, F_AEM) && (c & 0x7FFFu) == 0u) {
        a = 0u;
    }
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

fn convert(t: Tex, raw: u32) -> u32 {
    switch t.tpsm {
        case 0x00u, 0x30u {
            return raw;
        }
        case 0x01u, 0x31u {
            var a = t.ta0;
            if (has(t, F_AEM) && (raw & 0xFFFFFFu) == 0u) {
                a = 0u;
            }
            return (raw & 0xFFFFFFu) | (a << 24u);
        }
        case 0x02u, 0x0Au, 0x32u, 0x3Au {
            return expand16(raw, t);
        }
        case 0x13u, 0x1Bu {
            let e = clut[t.clut * CLUT_ENTRIES + (raw & 255u)];
            if (t.cpsm == 0x00u || t.cpsm == 0x01u) {
                return e;
            }
            return expand16(e, t);
        }
        case 0x14u, 0x24u, 0x2Cu {
            let e = clut[t.clut * CLUT_ENTRIES + (((t.csa & 15u) * 16u + (raw & 15u)) & 511u)];
            if (t.cpsm == 0x00u || t.cpsm == 0x01u) {
                return e;
            }
            return expand16(e, t);
        }
        default {
            return raw;
        }
    }
}

fn wrap_coord(v: i32, size: i32, m: u32, mn: u32, mx: u32) -> i32 {
    switch m {
        case 0u {
            return v & (size - 1);
        }
        case 1u {
            return clamp(v, 0, size - 1);
        }
        case 2u {
            return clamp(v, i32(mn), i32(mx));
        }
        default {
            return i32((u32(v) & mn) | mx);
        }
    }
}

fn fetch(t: Tex, xi: i32, yi: i32) -> u32 {
    let x = u32(wrap_coord(xi, 1 << t.tw, mode(t, 24u, 2u), t.minu, t.maxu));
    let y = u32(wrap_coord(yi, 1 << t.th, mode(t, 26u, 2u), t.minv, t.maxv));
    let l = addr(t.tf, x, y);
    return convert(t, (atomicLoad(&vram[l.word]) >> l.shift) & l.mask);
}

fn rgba(c: u32) -> vec4<f32> {
    return vec4<f32>(vec4<u32>(c & 0xFFu, (c >> 8u) & 0xFFu, (c >> 16u) & 0xFFu, c >> 24u));
}

fn sample(t: Tex, uin: f32, vin: f32) -> vec4<i32> {
    if (!has(t, F_BILINEAR)) {
        return vec4<i32>(rgba(fetch(t, i32(floor(uin)), i32(floor(vin)))));
    }
    let u = uin - 0.5;
    let v = vin - 0.5;
    let x0 = i32(floor(u));
    let y0 = i32(floor(v));
    let fx = u - f32(x0);
    let fy = v - f32(y0);
    let top = mix(rgba(fetch(t, x0, y0)), rgba(fetch(t, x0 + 1, y0)), fx);
    let bottom = mix(rgba(fetch(t, x0, y0 + 1)), rgba(fetch(t, x0 + 1, y0 + 1)), fx);
    return vec4<i32>(clamp(mix(top, bottom, fy) + vec4<f32>(0.5), vec4<f32>(0.0), vec4<f32>(255.0)));
}

// --- drawing ----------------------------------------------------------------------------------------------------------------------
// params: 0 first shape (word offset in data), 1 setups (word offset), 2 target width, 3 target height

// The setup's fields the fragment shader needs travel with the vertices (flat): read once per vertex instead of once per pixel
struct DrawOut {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,       // 0 to 255
    @location(1) stqf: vec4<f32>,        // S, T, Q (or U, V in 16ths of a texel, and 1), fog
    @location(2) @interpolate(flat) t0: vec4<u32>,
    @location(3) @interpolate(flat) t1: vec4<u32>,
    @location(4) @interpolate(flat) t2: vec4<u32>,
    @location(5) @interpolate(flat) t3: vec4<u32>,
}

fn pack_tex(at: u32, out: ptr<function, DrawOut>) {
    (*out).t0 = vec4<u32>(data[at], data[at + 7u], data[at + 8u], data[at + 15u]);
    (*out).t1 = vec4<u32>(data[at + 16u] | (data[at + 17u] << 8u), data[at + 18u], data[at + 19u], data[at + 20u]);
    (*out).t2 = vec4<u32>(data[at + 21u], data[at + 22u], data[at + 14u], data[at + 25u]);
    (*out).t3 = vec4<u32>(data[at + 26u], data[at + 27u], data[at + 28u], data[at + 29u]);
}

fn unpack_tex(in: DrawOut) -> Tex {
    var t: Tex;
    t.flags = in.t0.x;
    t.modes = in.t0.y;
    t.aref = in.t0.z;
    t.tw = in.t0.w & 0xFFu;
    t.th = in.t0.w >> 8u;
    t.cpsm = in.t1.x & 0xFFu;
    t.csa = in.t1.x >> 8u;
    t.minu = in.t1.y & 0xFFFFu;
    t.maxu = in.t1.y >> 16u;
    t.minv = in.t1.z & 0xFFFFu;
    t.maxv = in.t1.z >> 16u;
    t.ta0 = in.t1.w & 0xFFu;
    t.ta1 = (in.t1.w >> 8u) & 0xFFu;
    t.fogcol = in.t2.x;
    t.clut = in.t2.y;
    t.tpsm = in.t2.z;
    t.tf = Fmt(in.t2.w, in.t3.x, in.t3.y, in.t3.z, in.t3.w);
    return t;
}

fn f(i: u32) -> f32 {
    return bitcast<f32>(data[i]);
}

fn color_of(c: u32) -> vec4<f32> {
    return rgba(c);
}

fn stqf(p: u32, n: u32) -> vec4<f32> {
    return vec4<f32>(f(p + 15u + n * 3u), f(p + 16u + n * 3u), f(p + 17u + n * 3u), f32((data[p + 11u] >> (n * 8u)) & 0xFFu));
}

// Every shape is six vertices (two triangles); a triangle uses the first three and leaves the other three on one point (no area).
@vertex
fn hw_vs(@builtin(vertex_index) vi: u32) -> DrawOut {
    let shape = vi / 6u;
    let corner = vi % 6u;
    let p = prm(0u) + shape * PRIM_WORDS;
    let setup = prm(1u) + data[p] * SETUP_WORDS;
    let kind = data[p + 1u];
    let zmax = f32(data[setup + 23u]);
    var x = 0.0;
    var y = 0.0;
    var z = 0.0;
    var out: DrawOut;
    pack_tex(setup, &out);
    if (kind == 0u) {
        let n = min(corner, 2u);
        x = f(p + 4u + n * 2u);
        y = f(p + 5u + n * 2u);
        z = f32(data[p + 24u]);
        if (n > 0u) {
            z = z + f(p + 24u + n);
        }
        out.color = color_of(data[p + 12u + n]);
        out.stqf = stqf(p, n);
    } else if (kind == 1u) {
        // sprite: x0, y0, width, height; corners 0 1 2 / 2 1 3 of (x0, y0) (x1, y0) (x0, y1) (x1, y1)
        let k = array<u32, 6>(0u, 1u, 2u, 2u, 1u, 3u)[corner];
        x = f(p + 4u) + select(0.0, f(p + 6u), (k & 1u) != 0u);
        y = f(p + 5u) + select(0.0, f(p + 7u), (k & 2u) != 0u);
        z = f32(data[p + 24u]);
        out.color = color_of(data[p + 13u]);
        let a = stqf(p, 0u);
        let b = stqf(p, 1u);
        out.stqf = vec4<f32>(select(a.x, b.x, (k & 1u) != 0u), select(a.y, b.y, (k & 2u) != 0u), b.z, b.w);
    } else {
        // lines and points: to come
        x = -1e9;
        y = -1e9;
        out.color = vec4<f32>(0.0);
        out.stqf = vec4<f32>(0.0);
    }
    let w = f32(prm(2u));
    let h = f32(prm(3u));
    out.position = vec4<f32>((x + 0.5) / w * 2.0 - 1.0, 1.0 - (y + 0.5) / h * 2.0, clamp(z / zmax, 0.0, 1.0), 1.0);
    return out;
}

struct Shaded {
    color: vec4<f32>,
    factor: f32,  // C / 128 when C is the source alpha
}

struct BlendOut {
    @location(0) @blend_src(0) color: vec4<f32>,
    @location(0) @blend_src(1) factor: vec4<f32>,
}

// With dual-source blending: the colour and, as the second source, the blend factor
@fragment
fn hw_fs(in: DrawOut) -> BlendOut {
    let s = shade(in);
    var out: BlendOut;
    out.color = s.color;
    out.factor = vec4<f32>(s.factor);
    return out;
}

// Without it: the source alpha is the colour's alpha (the frame's alpha comes out wrong where blending uses the source alpha)
@fragment
fn hw_fs_single(in: DrawOut) -> @location(0) vec4<f32> {
    return shade(in).color;
}

fn shade(in: DrawOut) -> Shaded {
    let t = unpack_tex(in);
    var c = vec4<i32>(in.color);
    if (has(t, F_TME)) {
        var u: f32;
        var v: f32;
        if (has(t, 16u)) {
            u = in.stqf.x / 16.0;
            v = in.stqf.y / 16.0;
        } else {
            var q = in.stqf.z;
            if (q == 0.0) {
                q = 1.0;
            }
            u = in.stqf.x / q * f32(1u << t.tw);
            v = in.stqf.y / q * f32(1u << t.th);
        }
        let x = sample(t, u, v);
        let tcc = has(t, F_TCC);
        switch mode(t, 20u, 2u) {
            case 0u {
                c = vec4<i32>(clamp((x.xyz * c.xyz) >> vec3<u32>(7u), vec3<i32>(0), vec3<i32>(255)), select(c.w, clamp((x.w * c.w) >> 7u, 0, 255), tcc));
            }
            case 1u {
                c = vec4<i32>(x.xyz, select(c.w, x.w, tcc));
            }
            case 2u {
                c = vec4<i32>(clamp(((x.xyz * c.xyz) >> vec3<u32>(7u)) + vec3<i32>(c.w), vec3<i32>(0), vec3<i32>(255)), select(c.w, clamp(x.w + c.w, 0, 255), tcc));
            }
            default {
                c = vec4<i32>(clamp(((x.xyz * c.xyz) >> vec3<u32>(7u)) + vec3<i32>(c.w), vec3<i32>(0), vec3<i32>(255)), select(c.w, x.w, tcc));
            }
        }
    }
    if (has(t, F_FGE)) {
        let fo = i32(in.stqf.w);
        let fc = vec3<i32>(i32(t.fogcol & 0xFFu), i32((t.fogcol >> 8u) & 0xFFu), i32((t.fogcol >> 16u) & 0xFFu));
        c = vec4<i32>((fo * c.xyz + (255 - fo) * fc) >> vec3<u32>(8u), c.w);
    }
    if (has(t, F_ATE)) {
        let av = u32(c.w);
        var ok = false;
        switch mode(t, 0u, 3u) {
            case 0u {
                ok = false;
            }
            case 1u {
                ok = true;
            }
            case 2u {
                ok = av < t.aref;
            }
            case 3u {
                ok = av <= t.aref;
            }
            case 4u {
                ok = av == t.aref;
            }
            case 5u {
                ok = av >= t.aref;
            }
            case 6u {
                ok = av > t.aref;
            }
            default {
                ok = av != t.aref;
            }
        }
        // a failed test that keeps only the frame (1) or only RGB (3) still draws here; keeping only the depth (2) or nothing (0) does not
        let fail = mode(t, 4u, 2u);
        if (!ok && (fail == 0u || fail == 2u)) {
            discard;
        }
    }
    var a = u32(c.w) & 0xFFu;
    if (has(t, F_FBA)) {
        a = a | 0x80u;
    }
    var out: Shaded;
    out.color = vec4<f32>(vec3<f32>(clamp(c.xyz, vec3<i32>(0), vec3<i32>(255))), f32(a)) / 255.0;
    out.factor = f32(c.w & 0xFF) / 128.0;
    return out;
}
