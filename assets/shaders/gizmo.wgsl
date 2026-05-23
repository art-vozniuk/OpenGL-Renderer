// Editor gizmo renderer — thick screen-space line segments.
//
// One draw call per frame: N line instances, each = 6 vertices (2 tris).
// The CPU side rebuilds the line buffer every frame from the editor's
// current tool / selection / hover state. Lines approximate everything
// the editor needs: translate arrows (line + V tip), rotate rings (many
// short chords), scale handles (line + wireframe cube).

struct Uniforms {
    viewProj:     mat4x4<f32>,
    viewportSize: vec2<f32>,
    _pad:         vec2<f32>,
};

struct Line {
    // xyz = start world pos, w = thickness in pixels.
    a:     vec4<f32>,
    // xyz = end   world pos, w = unused.
    b:     vec4<f32>,
    // rgba; a < 1 for dimmed (unselected) handles, hover state sets a = 1.
    color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> lines: array<Line>;

struct VsOut {
    @builtin(position) pos:   vec4<f32>,
    @location(0)       color: vec4<f32>,
};

@vertex
fn vs_main(
    @builtin(vertex_index)   vid: u32,
    @builtin(instance_index) iid: u32,
) -> VsOut {
    let ln = lines[iid];

    let aClip = u.viewProj * vec4<f32>(ln.a.xyz, 1.0);
    let bClip = u.viewProj * vec4<f32>(ln.b.xyz, 1.0);

    // NDC positions of endpoints (post-w divide).
    let aNdc = aClip.xy / aClip.w;
    let bNdc = bClip.xy / bClip.w;

    // Direction in PIXEL space (so the thickness measure is in pixels
    // regardless of aspect ratio).
    let dirPx = (bNdc - aNdc) * u.viewportSize * 0.5;
    let dirL  = max(length(dirPx), 1e-4);
    let dirN  = dirPx / dirL;
    let perpN = vec2<f32>(-dirN.y, dirN.x);

    // Pixel offset perpendicular to the line, then convert back to NDC.
    let halfPx     = ln.a.w * 0.5;
    let perpPx     = perpN * halfPx;
    let perpNdc    = perpPx * 2.0 / u.viewportSize;

    // Pick endpoint + side per vertex. Quad layout:
    //   tri 0: (a,-) (b,-) (b,+)
    //   tri 1: (a,-) (b,+) (a,+)
    var which: u32 = 0u;
    var side:  f32 = -1.0;
    if      (vid == 0u) { which = 0u; side = -1.0; }
    else if (vid == 1u) { which = 1u; side = -1.0; }
    else if (vid == 2u) { which = 1u; side =  1.0; }
    else if (vid == 3u) { which = 0u; side = -1.0; }
    else if (vid == 4u) { which = 1u; side =  1.0; }
    else                { which = 0u; side =  1.0; }

    var base: vec4<f32>;
    if (which == 0u) { base = aClip; } else { base = bClip; }

    // Offset is applied pre-perspective-divide, so multiply by base.w
    // to compensate (so that after the GPU divides by w, the offset is
    // still `perpNdc * side` in NDC == `perpPx * side` pixels).
    var out: VsOut;
    out.pos = vec4<f32>(
        base.x + perpNdc.x * side * base.w,
        base.y + perpNdc.y * side * base.w,
        base.z,
        base.w
    );
    out.color = ln.color;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return in.color;
}
