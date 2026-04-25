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
@group(0) @binding(2) var<storage, read> scales:        array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> rotations:     array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> colors:        array<u32>;        // packed u8x4
@group(0) @binding(5) var<storage, read> sortedIndices: array<u32>;

struct VsOut {
    @builtin(position) pos:      vec4<f32>,
    @location(0)       color:    vec4<f32>,
    // Vertex-local 2D offset in sigma units (= corner * 3.0). |localPos|^2
    // is the Mahalanobis distance squared seen by the fragment shader.
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

fn QuatToMat3(q: vec4<f32>) -> mat3x3<f32> {
    // Convention: q.x=w, q.y=x, q.z=y, q.w=z (matches SplatLoader).
    let w = q.x;
    let x = q.y;
    let y = q.z;
    let z = q.w;
    let xx = x * x; let yy = y * y; let zz = z * z;
    let xy = x * y; let xz = x * z; let yz = y * z;
    let wx = w * x; let wy = w * y; let wz = w * z;
    return mat3x3<f32>(
        vec3<f32>(1.0 - 2.0 * (yy + zz),       2.0 * (xy + wz),       2.0 * (xz - wy)),
        vec3<f32>(      2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz),       2.0 * (yz + wx)),
        vec3<f32>(      2.0 * (xz + wy),       2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy))
    );
}

fn Cov3D(scale: vec3<f32>, rotation: vec4<f32>) -> mat3x3<f32> {
    let R = QuatToMat3(rotation);
    let S = mat3x3<f32>(
        vec3<f32>(scale.x, 0.0, 0.0),
        vec3<f32>(0.0, scale.y, 0.0),
        vec3<f32>(0.0, 0.0, scale.z),
    );
    let M = R * S;
    return M * transpose(M);
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
    let splatScale = scales[i].xyz;
    let splatRot   = rotations[i];
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

    let cov3D   = Cov3D(splatScale, splatRot);
    let viewRot = mat3x3<f32>(u.view[0].xyz, u.view[1].xyz, u.view[2].xyz);
    let cov2    = Cov2D(viewPos, cov3D, viewRot, fx, fy);

    let eig = EvalEigen(cov2);
    let majorRadius = 3.0 * sqrt(eig.lambda1);
    let minorRadius = 3.0 * sqrt(eig.lambda2);

    if (majorRadius > 0.5 * u.viewportSize.y) {
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
    out.localPos = corner * 3.0;
    return out;
}

// ----- Fragment ------------------------------------------------------------

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let r2 = dot(in.localPos, in.localPos);
    if (r2 > 9.0) {
        discard;
    }
    let alpha = exp(-0.5 * r2) * in.color.a;
    if (alpha < 1.0 / 255.0) {
        discard;
    }
    return vec4<f32>(in.color.rgb * alpha, alpha);
}
