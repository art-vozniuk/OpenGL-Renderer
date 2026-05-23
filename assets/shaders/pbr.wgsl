// Minimal PBR — single directional light, GGX BRDF, constant ambient.
// No IBL, no shadows, no emissive. Tangent-space normal mapping.

struct Uniforms {
    view:         mat4x4<f32>,
    projection:   mat4x4<f32>,
    model:        mat4x4<f32>,
    normalMat:    mat4x4<f32>,    // inverse-transpose of model (mat3 padded)
    lightDir:     vec4<f32>,      // xyz = world-space dir TO light (normalized), w unused
    lightColor:   vec4<f32>,      // rgb = color * intensity, w unused
    ambient:      vec4<f32>,      // rgb = ambient term, w unused
    baseColorFac: vec4<f32>,      // rgba
    mrFac:        vec4<f32>,      // r=metallic, g=roughness
    flags:        vec4<f32>,      // x=hasBaseColorTex, y=hasNormalTex, z=hasMRTex, w=unused
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var baseColorTex: texture_2d<f32>;
@group(0) @binding(3) var normalTex: texture_2d<f32>;
@group(0) @binding(4) var mrTex: texture_2d<f32>;

struct VsIn {
    @location(0) pos:     vec3<f32>,
    @location(1) normal:  vec3<f32>,
    @location(2) uv:      vec2<f32>,
    @location(3) tangent: vec4<f32>,  // xyz = tangent, w = handedness sign
};

struct VsOut {
    @builtin(position) clipPos: vec4<f32>,
    @location(0) worldPos: vec3<f32>,
    @location(1) normal:   vec3<f32>,
    @location(2) tangent:  vec3<f32>,
    @location(3) bitangent: vec3<f32>,
    @location(4) uv:       vec2<f32>,
};

@vertex
fn vs_main(in: VsIn) -> VsOut {
    let world4 = u.model * vec4<f32>(in.pos, 1.0);
    let nrm3 = mat3x3<f32>(u.normalMat[0].xyz, u.normalMat[1].xyz, u.normalMat[2].xyz);
    let n = normalize(nrm3 * in.normal);
    let t = normalize(nrm3 * in.tangent.xyz);
    let b = normalize(cross(n, t) * in.tangent.w);

    var out: VsOut;
    out.clipPos  = u.projection * u.view * world4;
    out.worldPos = world4.xyz;
    out.normal   = n;
    out.tangent  = t;
    out.bitangent = b;
    out.uv       = in.uv;
    return out;
}

const PI: f32 = 3.14159265359;

fn distributionGGX(N: vec3<f32>, H: vec3<f32>, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    let NdotH2 = NdotH * NdotH;
    let denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 1e-6);
}

fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let r = roughness + 1.0;
    let k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k + 1e-6);
}

fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (vec3<f32>(1.0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    // Base color (sRGB → linear: pow 2.2 approx).
    var baseColor = u.baseColorFac;
    if (u.flags.x > 0.5) {
        let tex = textureSample(baseColorTex, samp, in.uv);
        baseColor = baseColor * vec4<f32>(pow(tex.rgb, vec3<f32>(2.2)), tex.a);
    }

    // Metallic / roughness (glTF: G=roughness, B=metallic).
    var metallic  = u.mrFac.x;
    var roughness = u.mrFac.y;
    if (u.flags.z > 0.5) {
        let mr = textureSample(mrTex, samp, in.uv);
        roughness = roughness * mr.g;
        metallic  = metallic  * mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    // Normal mapping.
    var N = normalize(in.normal);
    if (u.flags.y > 0.5) {
        let nm = textureSample(normalTex, samp, in.uv).xyz * 2.0 - 1.0;
        let T = normalize(in.tangent);
        let B = normalize(in.bitangent);
        N = normalize(nm.x * T + nm.y * B + nm.z * N);
    }

    // View direction. Camera world position from inverse-view (row 3 of inv(view)).
    // We pass inv(view) implicitly via lightDir already in world space;
    // for V, derive from worldPos and the view matrix's inverse — easier:
    // recompute camera world pos from view = inv(camMat), so camMat * (0,0,0,1).
    // Cheaper: pass camPos in a uniform. We currently use the position
    // delta from the world origin to a fictional camera by inverting view
    // on the CPU side and packing into normalMat[3] — instead we approximate
    // V via the view matrix.
    // Camera position: view is world→view, so cam world = -view^-1 * t.
    // For simplicity here, reconstruct from inverse-view via view matrix
    // columns: cam world = -transpose(view3) * view.t.
    let view3 = mat3x3<f32>(u.view[0].xyz, u.view[1].xyz, u.view[2].xyz);
    // WGSL has no unary `-` for matrix; move the negation to the vector.
    let camPos = transpose(view3) * -u.view[3].xyz;
    let V = normalize(camPos - in.worldPos);

    let L = normalize(u.lightDir.xyz);
    let H = normalize(V + L);

    let F0 = mix(vec3<f32>(0.04), baseColor.rgb, metallic);

    let NDF = distributionGGX(N, H, roughness);
    let G   = geometrySmith(N, V, L, roughness);
    let F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

    let numerator = NDF * G * F;
    let denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4;
    let specular = numerator / denominator;

    let kS = F;
    let kD = (vec3<f32>(1.0) - kS) * (1.0 - metallic);

    let NdotL = max(dot(N, L), 0.0);
    let radiance = u.lightColor.rgb;
    let direct = (kD * baseColor.rgb / PI + specular) * radiance * NdotL;

    let ambient = u.ambient.rgb * baseColor.rgb;
    var color = ambient + direct;

    // Linear → sRGB approx for non-sRGB targets.
    color = pow(color, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(color, baseColor.a);
}
