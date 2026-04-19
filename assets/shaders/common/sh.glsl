// Spherical-harmonic basis for 3DGS view-dependent colour.
//
// The training pipeline (graphdeco-inria/gaussian-splatting) stores 16 SH
// coefs per channel, layout: 1 DC + 15 (bands 1..3). DC is already folded
// into per-splat RGB; this file only evaluates the 15-coef rest using the
// standard real-spherical-harmonics basis with the same constants as the
// reference renderer.
//
// `dir` must be a unit vector pointing from the camera to the splat,
// expressed in the PLY's native frame (COLMAP Y-down). Callers that keep
// scene data in +Y-up must flip (y, z) of the direction before calling.
//
// The SH texture is tiled: `splatsPerRow` splats are packed side-by-side
// on each row, so the row is always smaller than GL_MAX_TEXTURE_SIZE.
// A single splat occupies `coefCount` contiguous texels. `texelFetch`
// is driven by an integer coef index and the splat's gl_InstanceID.

// Constants precomputed from sqrt((2l+1) / (4pi)) * associated-Legendre prefactors.
// Values match the reference 3DGS CUDA rasterizer.
const float SH_C0   = 0.28209479177387814;
const float SH_C1_  = 0.4886025119029199;
const float SH_C2_0 = 1.0925484305920792;
const float SH_C2_1 = -1.0925484305920792;
const float SH_C2_2 = 0.31539156525252005;
const float SH_C2_3 = -1.0925484305920792;
const float SH_C2_4 = 0.5462742152960396;
const float SH_C3_0 = -0.5900435899266435;
const float SH_C3_1 = 2.890611442640554;
const float SH_C3_2 = -0.4570457994644658;
const float SH_C3_3 = 0.3731763325901154;
const float SH_C3_4 = -0.4570457994644658;
const float SH_C3_5 = 1.445305721320277;
const float SH_C3_6 = -0.5900435899266435;

// Fetch one RGB SH coef for splat `splatId`. The texture is tiled wide:
// row = splatId / splatsPerRow; col = (splatId % splatsPerRow) * coefCount + coefIdx.
vec3 FetchShCoef(sampler2D shTex, int splatId, int coefIdx,
                 int coefCount, int splatsPerRow)
{
	int row = splatId / splatsPerRow;
	int col = (splatId - row * splatsPerRow) * coefCount + coefIdx;
	return texelFetch(shTex, ivec2(col, row), 0).rgb;
}

// Evaluate the full SH expansion (DC + bands 1..3) for one splat. `coefCount`
// is the total number of coef texels per splat: 1 (DC only) up to 16 (deg 3).
// `dir` is a unit direction from camera -> splat in the PLY's native frame.
//
// Matches the reference 3DGS CUDA rasterizer's `computeColorFromSH` bit-for-
// bit: the caller must add 0.5 and clamp to [0, inf) before using the result
// as a linear colour.
vec3 EvalShFull(vec3 dir, sampler2D shTex, int splatId,
                int coefCount, int splatsPerRow)
{
	// Slot 0: DC. We apply SH_C0 here so the caller doesn't have to know
	// about band-0 vs bands-1..3 -- it's one sum.
	vec3 c = SH_C0 * FetchShCoef(shTex, splatId, 0, coefCount, splatsPerRow);
	if (coefCount < 2) return c;

	float x = dir.x, y = dir.y, z = dir.z;

	// Band 1 (slots 1..3)
	if (coefCount >= 2) c += SH_C1_ * (-y) * FetchShCoef(shTex, splatId, 1, coefCount, splatsPerRow);
	if (coefCount >= 3) c += SH_C1_ * ( z) * FetchShCoef(shTex, splatId, 2, coefCount, splatsPerRow);
	if (coefCount >= 4) c += SH_C1_ * (-x) * FetchShCoef(shTex, splatId, 3, coefCount, splatsPerRow);
	if (coefCount < 5) return c;

	// Band 2 (slots 4..8)
	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, yz = y * z, xz = x * z;
	if (coefCount >= 5) c += SH_C2_0 * xy                   * FetchShCoef(shTex, splatId, 4, coefCount, splatsPerRow);
	if (coefCount >= 6) c += SH_C2_1 * yz                   * FetchShCoef(shTex, splatId, 5, coefCount, splatsPerRow);
	if (coefCount >= 7) c += SH_C2_2 * (2.0 * zz - xx - yy) * FetchShCoef(shTex, splatId, 6, coefCount, splatsPerRow);
	if (coefCount >= 8) c += SH_C2_3 * xz                   * FetchShCoef(shTex, splatId, 7, coefCount, splatsPerRow);
	if (coefCount >= 9) c += SH_C2_4 * (xx - yy)            * FetchShCoef(shTex, splatId, 8, coefCount, splatsPerRow);
	if (coefCount < 10) return c;

	// Band 3 (slots 9..15)
	if (coefCount >= 10) c += SH_C3_0 * y * (3.0 * xx - yy)            * FetchShCoef(shTex, splatId,  9, coefCount, splatsPerRow);
	if (coefCount >= 11) c += SH_C3_1 * xy * z                         * FetchShCoef(shTex, splatId, 10, coefCount, splatsPerRow);
	if (coefCount >= 12) c += SH_C3_2 * y * (4.0 * zz - xx - yy)       * FetchShCoef(shTex, splatId, 11, coefCount, splatsPerRow);
	if (coefCount >= 13) c += SH_C3_3 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * FetchShCoef(shTex, splatId, 12, coefCount, splatsPerRow);
	if (coefCount >= 14) c += SH_C3_4 * x * (4.0 * zz - xx - yy)       * FetchShCoef(shTex, splatId, 13, coefCount, splatsPerRow);
	if (coefCount >= 15) c += SH_C3_5 * z * (xx - yy)                  * FetchShCoef(shTex, splatId, 14, coefCount, splatsPerRow);
	if (coefCount >= 16) c += SH_C3_6 * x * (xx - 3.0 * yy)            * FetchShCoef(shTex, splatId, 15, coefCount, splatsPerRow);

	return c;
}


// Legacy: evaluate only bands 1..3 (no DC) -- kept in case anything wants to
// mix a VBO-stored DC with texture-stored rest. `coefCount` is the number
// of rest coefs (up to 15).
vec3 EvalShRest(vec3 dir, sampler2D shTex, int splatId,
                int coefCount, int splatsPerRow)
{
	float x = dir.x, y = dir.y, z = dir.z;
	vec3 c = vec3(0.0);

	// Band 1 (l=1, 3 coefs)
	if (coefCount >= 1) c += SH_C1_ * (-y) * FetchShCoef(shTex, splatId, 0, coefCount, splatsPerRow);
	if (coefCount >= 2) c += SH_C1_ * ( z) * FetchShCoef(shTex, splatId, 1, coefCount, splatsPerRow);
	if (coefCount >= 3) c += SH_C1_ * (-x) * FetchShCoef(shTex, splatId, 2, coefCount, splatsPerRow);

	if (coefCount < 4) return c;

	// Band 2 (l=2, 5 coefs)
	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, yz = y * z, xz = x * z;

	if (coefCount >= 4) c += SH_C2_0 * xy                   * FetchShCoef(shTex, splatId, 3, coefCount, splatsPerRow);
	if (coefCount >= 5) c += SH_C2_1 * yz                   * FetchShCoef(shTex, splatId, 4, coefCount, splatsPerRow);
	if (coefCount >= 6) c += SH_C2_2 * (2.0 * zz - xx - yy) * FetchShCoef(shTex, splatId, 5, coefCount, splatsPerRow);
	if (coefCount >= 7) c += SH_C2_3 * xz                   * FetchShCoef(shTex, splatId, 6, coefCount, splatsPerRow);
	if (coefCount >= 8) c += SH_C2_4 * (xx - yy)            * FetchShCoef(shTex, splatId, 7, coefCount, splatsPerRow);

	if (coefCount < 9) return c;

	// Band 3 (l=3, 7 coefs)
	if (coefCount >= 9)  c += SH_C3_0 * y * (3.0 * xx - yy)            * FetchShCoef(shTex, splatId,  8, coefCount, splatsPerRow);
	if (coefCount >= 10) c += SH_C3_1 * xy * z                         * FetchShCoef(shTex, splatId,  9, coefCount, splatsPerRow);
	if (coefCount >= 11) c += SH_C3_2 * y * (4.0 * zz - xx - yy)       * FetchShCoef(shTex, splatId, 10, coefCount, splatsPerRow);
	if (coefCount >= 12) c += SH_C3_3 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * FetchShCoef(shTex, splatId, 11, coefCount, splatsPerRow);
	if (coefCount >= 13) c += SH_C3_4 * x * (4.0 * zz - xx - yy)       * FetchShCoef(shTex, splatId, 12, coefCount, splatsPerRow);
	if (coefCount >= 14) c += SH_C3_5 * z * (xx - yy)                  * FetchShCoef(shTex, splatId, 13, coefCount, splatsPerRow);
	if (coefCount >= 15) c += SH_C3_6 * x * (xx - 3.0 * yy)            * FetchShCoef(shTex, splatId, 14, coefCount, splatsPerRow);

	return c;
}
