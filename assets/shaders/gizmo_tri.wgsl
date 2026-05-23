// Flat-shaded world-space triangles for gizmo handles (arrowheads, scale balls).

struct Uniforms {
    viewProj:     mat4x4<f32>,
    viewportSize: vec2<f32>,
    _pad:         vec2<f32>,
};

struct Tri {
    a:     vec4<f32>,
    b:     vec4<f32>,
    c:     vec4<f32>,
    color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> tris: array<Tri>;

struct VsOut {
    @builtin(position) pos:   vec4<f32>,
    @location(0)       color: vec4<f32>,
};

@vertex
fn vs_main(
    @builtin(vertex_index)   vid: u32,
    @builtin(instance_index) iid: u32,
) -> VsOut {
    let t = tris[iid];
    var p: vec3<f32>;
    if      (vid == 0u) { p = t.a.xyz; }
    else if (vid == 1u) { p = t.b.xyz; }
    else                { p = t.c.xyz; }
    var out: VsOut;
    out.pos   = u.viewProj * vec4<f32>(p, 1.0);
    out.color = t.color;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return in.color;
}
