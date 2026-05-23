// Editor gizmo renderer — thick screen-space line segments with
// distance-field AA edges. Each line instance = 6 vertices (2 tris)
// expanded to a perpendicular quad in screen space. The fragment
// shader smooths the edge over ~1px so the lines look soft instead of
// stair-stepped.

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
    // rgba.
    color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> lines: array<Line>;

struct VsOut {
    @builtin(position) pos:      vec4<f32>,
    @location(0)       color:    vec4<f32>,
    // Signed normalized distance from line center across the quad's
    // short axis. -1 on one edge, +1 on the other, 0 down the middle.
    @location(1)       sideN:    f32,
    // Half-thickness in pixels — fragment shader uses it to size the
    // 1-pixel AA feather correctly regardless of how thick the line is.
    @location(2)       halfPx:   f32,
};

@vertex
fn vs_main(
    @builtin(vertex_index)   vid: u32,
    @builtin(instance_index) iid: u32,
) -> VsOut {
    let ln = lines[iid];

    let aClip = u.viewProj * vec4<f32>(ln.a.xyz, 1.0);
    let bClip = u.viewProj * vec4<f32>(ln.b.xyz, 1.0);

    let aNdc = aClip.xy / aClip.w;
    let bNdc = bClip.xy / bClip.w;

    // Direction in pixel space → thickness is measured in pixels.
    let dirPx = (bNdc - aNdc) * u.viewportSize * 0.5;
    let dirL  = max(length(dirPx), 1e-4);
    let dirN  = dirPx / dirL;
    let perpN = vec2<f32>(-dirN.y, dirN.x);

    // Pad an extra half-pixel for the AA feather so anti-aliasing has
    // room to fade. Otherwise the smoothstep would clip at the geometry
    // edge and we'd get a stair-stepped outline.
    let halfPx     = ln.a.w * 0.5;
    let halfPxAA   = halfPx + 1.0;
    let perpPx     = perpN * halfPxAA;
    let perpNdc    = perpPx * 2.0 / u.viewportSize;

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

    var out: VsOut;
    out.pos = vec4<f32>(
        base.x + perpNdc.x * side * base.w,
        base.y + perpNdc.y * side * base.w,
        base.z,
        base.w
    );
    out.color  = ln.color;
    // Side coord is ±1 at the *padded* edge; the visible-line edge is
    // at ±halfPx/halfPxAA. fs_main rescales using halfPx.
    out.sideN  = side * (halfPxAA / max(halfPx, 0.0001));
    out.halfPx = halfPx;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // 1-pixel feather measured in normalized side units. Lines thicker
    // than ~3px get a near-binary edge with a single-pixel soft border;
    // thin lines fade smoothly across most of their width.
    let feather = 1.0 / max(in.halfPx, 1.0);
    let coverage = 1.0 - smoothstep(1.0 - feather, 1.0, abs(in.sideN));
    if (coverage <= 0.001) {
        discard;
    }
    return vec4<f32>(in.color.rgb, in.color.a * coverage);
}
