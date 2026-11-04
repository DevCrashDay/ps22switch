// The smallest pair of programs deko3d can draw with: a centred triangle and one flat colour. Nothing of the game uses it. It is what
// PS2_DEKO3D_DRAW=1 draws, so that a console run can prove the shader path on its own — naga wrote the GLSL, uam compiled it into a
// deko3d shader module, deko3d loaded it, the pipeline was made, the viewport and the scissor were right, and the GPU drew it — before
// any of the renderer's own programs (18 KB each) are involved.

struct Position {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn triangle_vs(@builtin(vertex_index) i: u32) -> Position {
    // the corners of one triangle, in clip space: left bottom, right bottom, top middle; small enough to leave the clear colour
    // visible around it, so a still picture cannot be mistaken for a working one
    var corners = array<vec2<f32>, 3>(vec2<f32>(-0.9, -0.9), vec2<f32>(0.9, -0.9), vec2<f32>(0.0, 0.9));
    var out: Position;
    out.position = vec4<f32>(corners[i], 0.0, 1.0);
    return out;
}

@fragment
fn triangle_fs() -> @location(0) vec4<f32> {
    return vec4<f32>(0.9, 0.15, 0.6, 1.0);
}
