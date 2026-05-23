// Infinite-grid floor, screen-space technique.
//
// Vertex stage emits a single fullscreen triangle (no vertex buffer). For
// each pixel, the fragment stage reconstructs the world-space ray from
// the inverse view-projection, intersects it with the y=0 plane, and
// shades a grid pattern at integer X / Z values (with a coarser "major"
// pattern every 10 units and an axis highlight at x=0 / z=0).
//
// Drawing this BEFORE the splat pass keeps the splats' alpha blend math
// unchanged — the splat shader still assumes whatever's already in the
// colour attachment is the background.

struct Uniforms {
    invViewProj: mat4x4<f32>,
    // xyz = camera world pos, w = aspect (unused, kept for 16-byte align).
    cameraWorld: vec4<f32>,
    // x = grid spacing (minor), y = major-line stride (multiples of x),
    // z = fade distance (world units before grid disappears),
    // w = line thickness in world units at unit distance.
    params:      vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VsOut {
    @builtin(position) clipPos: vec4<f32>,
    @location(0)       ndc:     vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    // Fullscreen triangle: (-1,-1), (3,-1), (-1,3). NDC coverage is the
    // [-1,1]^2 square — the third vertex is past the viewport so the
    // single triangle blankets every pixel without seams.
    var p = vec2<f32>(-1.0, -1.0);
    if      (vid == 1u) { p = vec2<f32>( 3.0, -1.0); }
    else if (vid == 2u) { p = vec2<f32>(-1.0,  3.0); }

    var out: VsOut;
    out.clipPos = vec4<f32>(p, 0.0, 1.0);
    out.ndc     = p;
    return out;
}

// Reconstruct world position of the point on the ray-vs-y0-plane
// intersection. Returns (worldPos, t) where t < 0 means "ray points away
// from plane — discard".
fn unproject(ndc: vec2<f32>, depth: f32) -> vec3<f32> {
    let clip = vec4<f32>(ndc.x, ndc.y, depth, 1.0);
    let w    = u.invViewProj * clip;
    return w.xyz / w.w;
}

fn grid_factor(coord: f32, spacing: f32, thickness: f32) -> f32 {
    // Distance to the nearest grid line, normalised by `thickness` so the
    // line is "1 thickness wide". fwidth gives us a pixel-footprint
    // estimate that lets us anti-alias.
    let g     = coord / spacing;
    let f     = abs(fract(g - 0.5) - 0.5) / fwidth(g);
    let line  = 1.0 - min(f / thickness, 1.0);
    return line;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Two world-space points along the ray from this pixel — z=0 (near)
    // and z=1 (far) in NDC. Their interpolation gives every other
    // world-space point along the ray.
    let near = unproject(in.ndc, 0.0);
    let far  = unproject(in.ndc, 1.0);

    let rayDir = normalize(far - near);
    let camPos = u.cameraWorld.xyz;

    // Intersect with y = 0. If the ray is roughly parallel to the
    // floor, or hits behind the camera, discard.
    if (abs(rayDir.y) < 1e-4) {
        discard;
    }
    let t = -camPos.y / rayDir.y;
    if (t <= 0.0) {
        discard;
    }
    let hit = camPos + rayDir * t;

    let minorSpacing = u.params.x;
    let majorStride  = u.params.y;
    let fadeDist     = u.params.z;
    let thickness    = max(u.params.w, 0.5);

    // Two pattern layers: 1-unit minor lines and 10-unit major lines.
    let minor = max(grid_factor(hit.x, minorSpacing, thickness),
                    grid_factor(hit.z, minorSpacing, thickness));
    let major = max(grid_factor(hit.x, minorSpacing * majorStride, thickness),
                    grid_factor(hit.z, minorSpacing * majorStride, thickness));

    // Axis highlights: X axis (red-ish) at z=0, Z axis (blue-ish) at x=0.
    let axisX = 1.0 - min(abs(hit.z) / (minorSpacing * 0.05 * thickness), 1.0);
    let axisZ = 1.0 - min(abs(hit.x) / (minorSpacing * 0.05 * thickness), 1.0);

    let minorColor = vec3<f32>(0.30, 0.30, 0.34);
    let majorColor = vec3<f32>(0.55, 0.55, 0.60);
    let axisXCol   = vec3<f32>(0.85, 0.30, 0.30);
    let axisZCol   = vec3<f32>(0.30, 0.45, 0.85);

    // Compose layers (axes > major > minor).
    var color = minorColor * minor;
    color = mix(color, majorColor, major);
    color = mix(color, axisXCol,   axisX);
    color = mix(color, axisZCol,   axisZ);

    var alpha = max(max(minor, major), max(axisX, axisZ));
    if (alpha <= 0.001) {
        discard;
    }

    // Distance fade — pure horizontal distance (ignore y so a low-flying
    // camera doesn't lose the whole floor). Saturate to [0, 1].
    let distXZ = length(hit.xz - camPos.xz);
    let fade   = clamp(1.0 - distXZ / fadeDist, 0.0, 1.0);
    alpha *= fade;

    return vec4<f32>(color, alpha);
}
