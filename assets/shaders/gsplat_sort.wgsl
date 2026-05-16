// GPU radix sort over Gaussian-splat indices by view-space depth.
//
// Stable LSD radix, 4 byte-wide passes:
//
//   1. cs_clear_wg_hist  — zero the per-workgroup histogram block
//   2. cs_wg_hist        — every workgroup builds its own [256] histogram
//                          via shared-memory atomicAdd, then writes to
//                          `wgHist[digit][workgroup]` (column-major: digits
//                          stride by num_wg, so per-digit values are
//                          contiguous for the next phase's prefix scan).
//   3. cs_global_scan    — single-threaded scan over `wgHist`. Produces:
//                          - `wgOffset[d][w]` — exclusive prefix of
//                            wgHist along the workgroup axis for each digit
//                            (i.e. how many same-digit items come from
//                            earlier workgroups).
//                          - `globalDigitOffset[d]` — start of digit d's
//                            global bucket, exclusive prefix of total
//                            counts across digits.
//                          Single-thread is fine here: 256 * num_wg
//                          additions, ~5 ms for 1M splats; small share of
//                          a 16 ms frame.
//   4. cs_stable_scatter  — every thread reads its digit, computes a
//                          local rank within its workgroup (count of
//                          earlier same-digit threads via shared-memory
//                          loop), and writes to:
//                            dst = globalDigitOffset[d]
//                                + wgOffset[d][wgid]
//                                + localRank
//                          Stable: lower thread/workgroup ids always land
//                          in lower destinations within their digit class.
//                          That stability across 4 passes is what keeps
//                          LSD radix correct (and the frame stable).
//
// Why not parallel atomic scatter? `atomicAdd` returns unique slots but
// in non-deterministic order, so after multiple LSD passes same-byte
// pairs land in random positions, lower-byte ordering is lost, and the
// final order flickers frame to frame even on a static camera.

struct Uniforms {
    viewRow2:    vec4<f32>,   // (view[0][2], view[1][2], view[2][2], view[3][2])
    N:           u32,
    digitShift:  u32,
    swap:        u32,         // 0 -> read ping write pong, 1 -> swap
    numWg:       u32,         // ceil(N / WG)
    // World-space frustum planes (Gribb-Hartmann from view-projection).
    // Convention: dot(plane.xyz, p) + plane.w > 0  ->  inside half-space.
    // Order: left, right, bottom, top, near, far. Used by cs_init_depth
    // to cull off-screen splats out of the render before sort runs.
    frustum:     array<vec4<f32>, 6>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read>            positions:        array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write>      depths:           array<u32>;
@group(0) @binding(3) var<storage, read_write>      idxPing:          array<u32>;
@group(0) @binding(4) var<storage, read_write>      idxPong:          array<u32>;
// Per-workgroup histogram. Layout: wgHist[d * numWg + w].
@group(0) @binding(5) var<storage, read_write>      wgHist:           array<atomic<u32>>;
// Same shape, holds the per-(digit, workgroup) prefix-sum (start position
// of this workgroup's items inside the digit's bucket).
@group(0) @binding(6) var<storage, read_write>      wgOffset:         array<u32>;
// Per-digit global bucket start. 256 entries.
@group(0) @binding(7) var<storage, read_write>      globalDigitOffset: array<u32, 256>;
// Per-digit total count (one scan output, fed into the digit-offset
// scan). 256 entries.
@group(0) @binding(8) var<storage, read_write>      digitTotals:       array<u32, 256>;
// Per-splat scales (vec4 padded). Read by cs_init_depth for the
// conservative bounding-sphere radius used in the frustum test.
@group(0) @binding(9) var<storage, read>            scales:           array<vec4<f32>>;
// Indirect-draw arguments written by the cull pass + used by the
// render's DrawIndirect. Layout matches GPUDrawIndirectArgs:
//   [0] vertexCount   = 6   (set once per frame by cs_clear_indirect)
//   [1] instanceCount = visible-splat count (atomicAdd in cs_init_depth)
//   [2] firstVertex   = 0
//   [3] firstInstance = 0
// Combined indirect-args block. Single binding to keep the BGL under
// the maxStorageBuffersPerShaderStage = 10 lower bound that mobile
// browsers honour. Layout (offsets in 4-byte u32):
//   [0..3] = DrawIndirect args  (vertexCount, instanceCount, firstVertex, firstInstance)
//   [4..6] = DispatchIndirect args  (wgX, wgY, wgZ)  — used by cs_wg_hist + cs_stable_scatter
// `indirectArgs[1]` doubles as the visible-splat counter that gates
// the dynamic bounds check inside every sort kernel.
@group(0) @binding(10) var<storage, read_write>     indirectArgs:     array<atomic<u32>, 7>;

const WG: u32 = 256u;

fn idxIn(i: u32) -> u32 {
    if (u.swap == 0u) { return idxPing[i]; } else { return idxPong[i]; }
}
fn writeIdxOut(i: u32, v: u32) {
    if (u.swap == 0u) { idxPong[i] = v; } else { idxPing[i] = v; }
}


// Reset the indirect-draw argument buffer. Run as a single thread
// every frame before cs_init_depth so the atomic counter starts at 0
// and the constant fields are correct.
@compute @workgroup_size(1)
fn cs_clear_indirect(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x != 0u) { return; }
    atomicStore(&indirectArgs[0], 6u);   // vertexCount   (two triangles)
    atomicStore(&indirectArgs[1], 0u);   // instanceCount (atomicAdd target)
    atomicStore(&indirectArgs[2], 0u);   // firstVertex
    atomicStore(&indirectArgs[3], 0u);   // firstInstance
    atomicStore(&indirectArgs[4], 0u);   // dispatch wgX (cs_finalize_args fills)
    atomicStore(&indirectArgs[5], 1u);   // dispatch wgY
    atomicStore(&indirectArgs[6], 1u);   // dispatch wgZ
}


// True if the sphere at `c` with radius `r` lies entirely outside the
// frustum half-spaces. Conservative — false-positives only when the
// sphere actually does intersect, never the other way round.
fn sphere_outside_frustum(c: vec3<f32>, r: f32) -> bool {
    for (var k: u32 = 0u; k < 6u; k = k + 1u) {
        let p = u.frustum[k];
        if (dot(p.xyz, c) + p.w < -r) { return true; }
    }
    return false;
}


// Pre-sort compaction: for every visible splat we claim a contiguous
// slot in idxPing via atomicAdd, write the original index there, and
// store the depth key at the splat's NATIVE slot in `depths`. The
// sort then runs over only [0, visibleCount) — invisible splats never
// participate in any byte pass.
//
// Memory model:
//   depths[orig_i]      = sort key, native-indexed (stable across frames
//                         for visible orig_i; stale for currently-culled
//                         orig_i but never read since they're not in
//                         idxPing's compacted range).
//   idxPing[slot]       = orig_i for visible splats only, slot in
//                         [0, visibleCount).
@compute @workgroup_size(WG)
fn cs_init_depth(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= u.N) { return; }

    let p = positions[i].xyz;
    // 3-sigma conservative radius. Matches the quad size the render
    // shader emits, so anything that would draw nothing through the
    // existing clip-space test will also fail the frustum test here.
    let s = scales[i].xyz;
    let r = max(s.x, max(s.y, s.z)) * 3.0;
    if (sphere_outside_frustum(p, r)) { return; }

    // Visible: claim a slot, compute key.
    let slot = atomicAdd(&indirectArgs[1], 1u);
    idxPing[slot] = i;

    let z = u.viewRow2.x * p.x + u.viewRow2.y * p.y + u.viewRow2.z * p.z + u.viewRow2.w;
    let bits = bitcast<u32>(z);
    var key: u32;
    if ((bits & 0x80000000u) != 0u) {
        key = ~bits;
    } else {
        key = bits ^ 0x80000000u;
    }
    depths[i] = key;
}


// Translate the visible counter into compute-dispatch-indirect args
// for the rest of the sort pipeline. Single thread; called once per
// frame between cs_init_depth and the byte passes.
@compute @workgroup_size(1)
fn cs_finalize_args(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x != 0u) { return; }
    let visible = atomicLoad(&indirectArgs[1]);
    let wgX = (visible + WG - 1u) / WG;
    atomicStore(&indirectArgs[4], wgX);
    // y and z were set to 1 in cs_clear_indirect — leave alone.
}


// Sort-on-stop / mobile path: identity-permutation init.
// Writes idxPing[i] = i + depths[i] = key for ALL N splats (no
// frustum cull, no atomic compaction). Thread 0 also seeds the
// indirect-args block with instanceCount = N + dispatch wgX = numWg
// so the byte passes operate over the full range.
//
// Why no compaction here: with sort-on-stop the byte passes only
// run on the moving→idle transition. During the moving frames we
// skip EncodeSort entirely and let the render reuse the last
// sorted idxPing as-is. If init_depth had compacted the array each
// frame, that previous ordering would be blown away by the atomic
// adds, and rendering would see garbage-ordered splats. Keeping
// identity + all-N preserves the last sort across frames; the
// vertex shader's existing frustum / behind-camera / sub-pixel
// early-outs handle off-screen splats per-vertex.
@compute @workgroup_size(WG)
fn cs_init_identity(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i == 0u) {
        atomicStore(&indirectArgs[0], 6u);     // vertexCount
        atomicStore(&indirectArgs[1], u.N);    // instanceCount = all splats
        atomicStore(&indirectArgs[2], 0u);     // firstVertex
        atomicStore(&indirectArgs[3], 0u);     // firstInstance
        atomicStore(&indirectArgs[4], u.numWg); // dispatch wgX
        atomicStore(&indirectArgs[5], 1u);     // wgY
        atomicStore(&indirectArgs[6], 1u);     // wgZ
    }
    if (i >= u.N) { return; }

    idxPing[i] = i;

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
}


// Zero the [num_wg * 256] per-workgroup histogram array between byte passes.
@compute @workgroup_size(WG)
fn cs_clear_wg_hist(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    let total = u.numWg * 256u;
    if (i < total) {
        atomicStore(&wgHist[i], 0u);
    }
}


// Per-workgroup histogram. Each workgroup builds a 256-bucket count of its
// 256 elements via shared-memory atomicAdd, then leader threads flush
// those counts to wgHist[digit][workgroup_id].
var<workgroup> sLocalHist: array<atomic<u32>, 256>;

@compute @workgroup_size(WG)
fn cs_wg_hist(@builtin(global_invocation_id) gid: vec3<u32>,
              @builtin(workgroup_id) wgid: vec3<u32>,
              @builtin(local_invocation_index) lid: u32) {
    if (lid < 256u) { atomicStore(&sLocalHist[lid], 0u); }
    workgroupBarrier();

    // visibleCount lives in indirectArgs[1] — bounds the compacted
    // sort. Threads beyond it are tail of the last workgroup; they
    // still hit the barriers, just skip the work.
    let visibleCount = atomicLoad(&indirectArgs[1]);
    let i = gid.x;
    if (i < visibleCount) {
        let idx = idxIn(i);
        let key = depths[idx];
        let digit = (key >> u.digitShift) & 0xFFu;
        atomicAdd(&sLocalHist[digit], 1u);
    }
    workgroupBarrier();

    // Each thread (lid 0..255) writes one digit's count for this wg.
    if (lid < 256u) {
        let cnt = atomicLoad(&sLocalHist[lid]);
        // Column-major layout: same-digit values across workgroups are
        // contiguous => the global scan can read `wgHist[d * numWg + w]`
        // sequentially per digit.
        atomicStore(&wgHist[lid * u.numWg + wgid.x], cnt);
    }
}


// Per-digit column scan + digit-total emit.
//
// Dispatch geometry: 256 workgroups (one per digit), 256 threads each.
// Each thread covers `chunk = ceil(numWg / 256)` entries of its digit's
// column. Within a workgroup we do a per-thread local exclusive prefix
// over the chunk, then a workgroup Hillis-Steele scan over thread totals,
// then add the thread base to the per-thread chunk and write back.
//
// Last thread emits the digit total to digitTotals[d] for the next step.
//
// Replaces the previous single-thread O(numWg * 256) scan that was the
// frame budget killer (sat at ~400 ms / frame for 1M splats).
const SCAN_WG: u32 = 256u;
// Per-thread chunk capacity for the column scan. With SCAN_WG=256
// this caps numWg at 256 * MAX_CHUNK = 16384 workgroups (~4M splats).
// Overflowing it silently corrupts wgOffset and the stable-scatter
// writes collide non-deterministically — flicker on a fixed view.
const MAX_CHUNK: u32 = 64u;
var<workgroup> sScan: array<u32, SCAN_WG>;

@compute @workgroup_size(SCAN_WG)
fn cs_column_scan(@builtin(workgroup_id) wgid: vec3<u32>,
                  @builtin(local_invocation_index) lid: u32) {
    let d    = wgid.x;
    let base = d * u.numWg;
    let chunk = (u.numWg + SCAN_WG - 1u) / SCAN_WG;

    // Read chunk entries, build per-thread exclusive prefix into `local`.
    var prefix: u32 = 0u;
    var local: array<u32, MAX_CHUNK>;
    for (var k: u32 = 0u; k < chunk; k = k + 1u) {
        let i = lid * chunk + k;
        var v: u32 = 0u;
        if (i < u.numWg) { v = atomicLoad(&wgHist[base + i]); }
        local[k] = prefix;
        prefix = prefix + v;
    }
    sScan[lid] = prefix;
    workgroupBarrier();

    // Hillis-Steele inclusive scan over the 256 thread totals.
    var step: u32 = 1u;
    while (step < SCAN_WG) {
        var add: u32 = 0u;
        if (lid >= step) { add = sScan[lid - step]; }
        workgroupBarrier();
        sScan[lid] = sScan[lid] + add;
        workgroupBarrier();
        step = step << 1u;
    }

    // Convert inclusive -> exclusive base for this thread, then add to
    // the per-thread local prefixes and write back.
    var threadBase: u32 = 0u;
    if (lid > 0u) { threadBase = sScan[lid - 1u]; }
    for (var k: u32 = 0u; k < chunk; k = k + 1u) {
        let i = lid * chunk + k;
        if (i < u.numWg) {
            wgOffset[base + i] = threadBase + local[k];
        }
    }
    if (lid == SCAN_WG - 1u) {
        digitTotals[d] = sScan[SCAN_WG - 1u];
    }
}


// Tiny second scan: 256 digit totals -> 256 globalDigitOffset entries.
// Single-threaded is fine — 256 ops.
@compute @workgroup_size(1)
fn cs_digit_offset_scan() {
    var sum: u32 = 0u;
    for (var d: u32 = 0u; d < 256u; d = d + 1u) {
        globalDigitOffset[d] = sum;
        sum = sum + digitTotals[d];
    }
}


// Stable scatter. Each thread:
//   1. Reads its digit.
//   2. Counts how many earlier threads in the same workgroup share that
//      digit (`localRank`) — gives a stable position within the workgroup
//      for that digit class.
//   3. Writes idx to:
//        globalDigitOffset[d] + wgOffset[d * numWg + wgid] + localRank
var<workgroup> sDigit: array<u32, WG>;

@compute @workgroup_size(WG)
fn cs_stable_scatter(@builtin(global_invocation_id) gid: vec3<u32>,
                     @builtin(workgroup_id) wgid: vec3<u32>,
                     @builtin(local_invocation_index) lid: u32) {
    // Same dynamic-bound trick as cs_wg_hist — gate on visibleCount,
    // not the static u.N. Sentinel digit keeps tail threads in the
    // shared-mem rank loop without writing.
    let visibleCount = atomicLoad(&indirectArgs[1]);
    let i = gid.x;
    var digit: u32 = 256u;  // sentinel meaning "no element"
    var idx: u32 = 0u;
    if (i < visibleCount) {
        idx = idxIn(i);
        let key = depths[idx];
        digit = (key >> u.digitShift) & 0xFFu;
    }
    sDigit[lid] = digit;
    workgroupBarrier();

    if (digit < 256u) {
        var localRank: u32 = 0u;
        for (var j: u32 = 0u; j < lid; j = j + 1u) {
            if (sDigit[j] == digit) { localRank = localRank + 1u; }
        }
        let dst = globalDigitOffset[digit] + wgOffset[digit * u.numWg + wgid.x] + localRank;
        writeIdxOut(dst, idx);
    }
}
