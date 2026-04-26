// Gaussian-splat WGSL render shader, indexed-storage variant.
//
// Splat data lives in five storage buffers (positions, scales, rotations,
// colors, sortedIndices). The `corners` array is a module constant — we
// don't need a vertex buffer for that. Per draw the pipeline emits 6
// vertices x N instances; the vertex shader uses
//   instance_index -> sortedIndices -> raw splat data
// so changing draw order is just rewriting one buffer (sortedIndices)
// from the GPU sort, never touching the bulky position/scale/etc data.

struct Uniforms {
    view:         mat4x4<f32>,
    projection:   mat4x4<f32>,
    viewportSize: vec2<f32>,
    _pad:         vec2<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> positions:     array<vec4<f32>>;
// Pre-computed world-space covariance Σ₃ = R·S²·Rᵀ per splat. Stored
// as 2×vec4 (2 splats per 32-byte block laid out as [Σ₀₀ Σ₀₁ Σ₀₂ Σ₁₁]
// then [Σ₁₂ Σ₂₂ _ _]). Replaces the old scales+rotations buffers —
// we no longer rebuild Σ₃ in the vertex shader every frame.
@group(0) @binding(2) var<storage, read> cov3D:         array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> colors:        array<u32>;        // packed u8x4
@group(0) @binding(4) var<storage, read> sortedIndices: array<u32>;

// 2.5σ quad bound — at 2.5σ a unit-amplitude Gaussian contributes
// exp(−0.5·6.25) ≈ 0.044 of its peak, well below the existing
// `alpha < 1/255` discard threshold for typical splat colours.
// Trimming from 3.0 cuts quad area by ~30% on overdraw-heavy mobile
// scenes (where fragment + tile-bandwidth dominates the GPU budget)
// at no perceptible visual cost.
const SIGMA_BOUND: f32 = 2.5;

struct VsOut {
    @builtin(position) pos:      vec4<f32>,
    @location(0)       color:    vec4<f32>,
    // Vertex-local 2D offset in sigma units (= corner * SIGMA_BOUND).
    // |localPos|² is the Mahalanobis-distance² seen by the fragment.
    @location(1)       localPos: vec2<f32>,
};

// Two triangles per quad: 0,1,2 + 0,2,3 with corners in [-1, 1]^2.
const CORNERS: array<vec2<f32>, 6> = array<vec2<f32>, 6>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 1.0, -1.0),
    vec2<f32>( 1.0,  1.0),
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 1.0,  1.0),
    vec2<f32>(-1.0,  1.0),
);

// ----- Helpers (covariance + eigen) -----------------------------------------

// Reconstruct the symmetric 3×3 covariance from the per-splat 2×vec4
// pre-baked block. CPU-side packing (see GaussianSplatRenderer::Upload):
//   lo = (Σ₀₀, Σ₀₁, Σ₀₂, Σ₁₁)
//   hi = (Σ₁₂, Σ₂₂, _,   _)
fn LoadCov3D(splatIdx: u32) -> mat3x3<f32> {
    let lo = cov3D[splatIdx * 2u + 0u];
    let hi = cov3D[splatIdx * 2u + 1u];
    return mat3x3<f32>(
        vec3<f32>(lo.x, lo.y, lo.z),  // col 0 = (Σ₀₀, Σ₁₀, Σ₂₀)
        vec3<f32>(lo.y, lo.w, hi.x),  // col 1 = (Σ₀₁, Σ₁₁, Σ₂₁)
        vec3<f32>(lo.z, hi.x, hi.y)   // col 2 = (Σ₀₂, Σ₁₂, Σ₂₂)
    );
}

fn Cov2D(viewPos: vec3<f32>, cov3D: mat3x3<f32>, viewRot: mat3x3<f32>,
         fx: f32, fy: f32) -> vec3<f32> {
    let z    = max(-viewPos.z, 0.01);
    let zInv = 1.0 / z;
    let J = mat3x3<f32>(
        vec3<f32>(fx * zInv, 0.0, 0.0),
        vec3<f32>(0.0, fy * zInv, 0.0),
        vec3<f32>(-fx * viewPos.x * zInv * zInv,
                  -fy * viewPos.y * zInv * zInv,
                   0.0),
    );
    let T   = J * viewRot;
    let cov = T * cov3D * transpose(T);
    return vec3<f32>(cov[0][0] + 0.3, cov[0][1], cov[1][1] + 0.3);
}

struct SplatEigen { lambda1: f32, lambda2: f32, majorAxis: vec2<f32> };

fn EvalEigen(cov: vec3<f32>) -> SplatEigen {
    let a = cov.x;
    let b = cov.y;
    let c = cov.z;
    let mid  = 0.5 * (a + c);
    let disc = sqrt(max(0.0, mid * mid - (a * c - b * b)));
    let lambda1 = mid + disc;
    let lambda2 = max(mid - disc, 0.1);

    var axis: vec2<f32>;
    if (abs(b) < 1e-6) {
        if (a >= c) { axis = vec2<f32>(1.0, 0.0); }
        else        { axis = vec2<f32>(0.0, 1.0); }
    } else {
        axis = normalize(vec2<f32>(lambda1 - c, b));
    }

    var e: SplatEigen;
    e.lambda1 = lambda1;
    e.lambda2 = lambda2;
    e.majorAxis = axis;
    return e;
}

// ----- Vertex --------------------------------------------------------------

@vertex
fn vs_main(
    @builtin(vertex_index)   vid: u32,
    @builtin(instance_index) iid: u32,
) -> VsOut {
    let i      = sortedIndices[iid];
    let corner = CORNERS[vid];
    let p4     = positions[i];
    let splatPos   = p4.xyz;
    let splatColor = unpack4x8unorm(colors[i]);

    var out: VsOut;
    out.color = splatColor;
    out.localPos = vec2<f32>(0.0);

    let viewPosH = u.view * vec4<f32>(splatPos, 1.0);
    let viewPos  = viewPosH.xyz;

    if (viewPos.z >= -0.05) {
        out.pos = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        return out;
    }

    let fx = 0.5 * u.viewportSize.x * u.projection[0][0];
    let fy = 0.5 * u.viewportSize.y * u.projection[1][1];

    let cov3   = LoadCov3D(i);
    let viewRot = mat3x3<f32>(u.view[0].xyz, u.view[1].xyz, u.view[2].xyz);
    let cov2   = Cov2D(viewPos, cov3, viewRot, fx, fy);

    let eig = EvalEigen(cov2);
    let majorRadius = SIGMA_BOUND * sqrt(eig.lambda1);
    let minorRadius = SIGMA_BOUND * sqrt(eig.lambda2);

    if (majorRadius > 0.5 * u.viewportSize.y) {
        out.pos = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        return out;
    }

    // Tiny-splat early-out: if the projected major radius is sub-
    // pixel, the rasterised quad covers fewer than ~1 pixel and the
    // existing per-fragment alpha-discard would clip nearly all of
    // it anyway. Discard at vertex stage skips the fragment shader
    // and the alpha-blend bandwidth on a long tail of distant splats
    // — common at the train-scene default spawn where ~half the
    // scene is far enough to project below 1 px.
    if (majorRadius < 0.6) {
        out.pos = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        return out;
    }

    // Frustum cull at the splat centre with a screen-space-radius margin.
    // For a 1M-splat scene with the camera looking at a small region most
    // splats end up off-screen — skipping their quads here saves the
    // fragment shader + alpha-blend overdraw they would otherwise cause.
    // Conservative: test the bounding circle of the splat (radius =
    // max(major, minor) in pixels) against the viewport edges in clip
    // space. The existing 'too big' and 'behind camera' checks above
    // cover the other cases where the projected bound would be wrong.
    let centerClip = u.projection * viewPosH;
    let radiusPx   = max(majorRadius, minorRadius);
    let marginClip = (2.0 * radiusPx / u.viewportSize) * centerClip.w;
    if (abs(centerClip.x) > centerClip.w + marginClip.x
     || abs(centerClip.y) > centerClip.w + marginClip.y) {
        out.pos = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        return out;
    }

    let majorAxis = eig.majorAxis;
    let minorAxis = vec2<f32>(-majorAxis.y, majorAxis.x);
    let offsetPx = majorAxis * (corner.x * majorRadius)
                 + minorAxis * (corner.y * minorRadius);
    let offsetNdc = 2.0 * offsetPx / u.viewportSize;

    var clip = u.projection * viewPosH;
    clip.x = clip.x + offsetNdc.x * clip.w;
    clip.y = clip.y + offsetNdc.y * clip.w;
    out.pos = clip;
    out.localPos = corner * SIGMA_BOUND;
    return out;
}

// ----- Fragment ------------------------------------------------------------

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Mahalanobis-distance² > SIGMA_BOUND² → outside our quad's tight
    // crop. The Gaussian envelope fades fast enough that most of the
    // overdraw is in the corners; this discard keeps the rasteriser
    // honest about it.
    let r2 = dot(in.localPos, in.localPos);
    if (r2 > 6.25) {  // 2.5²
        discard;
    }
    let alpha = exp(-0.5 * r2) * in.color.a;
    if (alpha < 1.0 / 255.0) {
        discard;
    }
    return vec4<f32>(in.color.rgb * alpha, alpha);
}
