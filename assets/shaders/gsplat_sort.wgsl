// GPU radix sort over Gaussian-splat indices by view-space depth.
//
// Layout: 4 byte-wide passes of LSD radix. Each pass:
//   1. cs_clear_hist     — zero histogram[256]
//   2. cs_histogram      — atomically increment histogram[digit]
//   3. cs_prefix_sum     — single-thread exclusive scan into offsets[256]
//   4. cs_scatter        — atomicAdd offsets[digit] -> indicesOut[dst] = idx
// Then ping-pong indicesIn <-> indicesOut for the next byte.
//
// Inputs:
//   - positions    (read): unsorted splat centroids in world space
//   - depths       (rw):   per-splat sortable u32 keys (filled by cs_init_depth)
//   - indicesPing  (rw):   one of the two index buffers; meaning depends on pass
//   - indicesPong  (rw):   the other; CPU swaps which is "in" via uniform.
//   - histogram, offsets   (rw, atomic): scratch; reset every pass
//
// One uniform block carries N, the digit shift, and the view's third row
// (so depth = view * pos = vrow . pos). Kept tight — passes are tiny.

struct Uniforms {
    viewRow2:    vec4<f32>,   // (view[0][2], view[1][2], view[2][2], view[3][2])
    N:           u32,
    digitShift:  u32,
    swap:        u32,         // 0 -> read ping write pong, 1 -> swap
    _pad:        u32,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read>            positions: array<vec4<f32>>;  // padded vec3 -> vec4
@group(0) @binding(2) var<storage, read_write>      depths:    array<u32>;
@group(0) @binding(3) var<storage, read_write>      idxPing:   array<u32>;
@group(0) @binding(4) var<storage, read_write>      idxPong:   array<u32>;
@group(0) @binding(5) var<storage, read_write>      histogram: array<atomic<u32>, 256>;
@group(0) @binding(6) var<storage, read_write>      offsets:   array<atomic<u32>, 256>;

const WG: u32 = 256u;


// Read from whichever buffer is currently "in" for this pass.
fn idxIn(i: u32) -> u32 {
    if (u.swap == 0u) { return idxPing[i]; } else { return idxPong[i]; }
}
fn writeIdxOut(i: u32, v: u32) {
    if (u.swap == 0u) { idxPong[i] = v; } else { idxPing[i] = v; }
}


// Pass 0 (run once at start of frame):
//   - depth[i] = bit-cast of view-space z, with sign-bit-flip trick so
//     ascending-uint comparison matches ascending-float.
//   - idxPing[i] = i  (initial identity permutation).
@compute @workgroup_size(WG)
fn cs_init_depth(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= u.N) { return; }
    let p = positions[i].xyz;
    let z = u.viewRow2.x * p.x + u.viewRow2.y * p.y + u.viewRow2.z * p.z + u.viewRow2.w;
    let bits = bitcast<u32>(z);
    var key: u32;
    if ((bits & 0x80000000u) != 0u) {
        key = ~bits;
    } else {
        key = bits ^ 0x80000000u;
    }
    depths[i] = key;
    idxPing[i] = i;
}


@compute @workgroup_size(WG)
fn cs_clear_hist(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i < 256u) {
        atomicStore(&histogram[i], 0u);
    }
}


@compute @workgroup_size(WG)
fn cs_histogram(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= u.N) { return; }
    let idx = idxIn(i);
    let key = depths[idx];
    let digit = (key >> u.digitShift) & 0xFFu;
    atomicAdd(&histogram[digit], 1u);
}


// Single-thread exclusive prefix sum. 256 entries; trivial.
@compute @workgroup_size(1)
fn cs_prefix_sum() {
    var sum: u32 = 0u;
    for (var i: u32 = 0u; i < 256u; i = i + 1u) {
        let c = atomicLoad(&histogram[i]);
        atomicStore(&offsets[i], sum);
        sum = sum + c;
    }
}


@compute @workgroup_size(WG)
fn cs_scatter(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= u.N) { return; }
    let idx = idxIn(i);
    let key = depths[idx];
    let digit = (key >> u.digitShift) & 0xFFu;
    let dst = atomicAdd(&offsets[digit], 1u);
    writeIdxOut(dst, idx);
}
