// Shared Gaussian-Splat maths used by gsplat_v and gsplat_f.
// Math conventions follow Kerbl et al. 2023 section4 + antimatter15/splat reference.

// Expand a unit quaternion (w, x, y, z) into a 3x3 rotation matrix.
mat3 QuatToMat3(vec4 q)
{
	float w = q.x;
	float x = q.y;
	float y = q.z;
	float z = q.w;

	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, xz = x * z, yz = y * z;
	float wx = w * x, wy = w * y, wz = w * z;

	return mat3(
		vec3(1.0 - 2.0 * (yy + zz),       2.0 * (xy + wz),       2.0 * (xz - wy)),
		vec3(      2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz),       2.0 * (yz + wx)),
		vec3(      2.0 * (xz + wy),       2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy))
	);
}

// Build the 3D covariance matrix for an anisotropic Gaussian:
//   Sigma = R * diag(s)^2 * R^T
// We store the unit rotation quaternion and per-axis sigma on the CPU.
mat3 Cov3D(vec3 scale, vec4 rotation)
{
	mat3 R = QuatToMat3(rotation);
	mat3 S = mat3(0.0);
	S[0][0] = scale.x;
	S[1][1] = scale.y;
	S[2][2] = scale.z;
	mat3 M = R * S;
	return M * transpose(M);
}

// Project a 3D covariance onto the image plane given the camera view matrix
// and focal lengths (fx, fy in pixels). Returns the 2x2 screen-space
// covariance encoded as vec3 (Sigmaxx, Sigmaxy, Sigmayy).
//
// Uses the EWA-style first-order Jacobian from section4 of the reference paper:
//   J * W * Sigma * W^T * J^T
// where W is the view-space rotation (upper-left 3x3 of view matrix)
// and J is the Jacobian of the perspective projection at view-space z.
// A small low-pass term is added on the diagonal to keep single-pixel
// splats numerically well-behaved.
vec3 Cov2D(vec3 viewPos, mat3 cov3D, mat3 viewRot, float fx, float fy)
{
	// Clamp z near the near-plane so the Jacobian stays finite for splats
	// that hug the camera. Matches the reference implementation.
	float z = max(-viewPos.z, 0.01);
	float zInv = 1.0 / z;

	mat3 J = mat3(
		vec3(fx * zInv, 0.0, 0.0),
		vec3(0.0, fy * zInv, 0.0),
		vec3(-fx * viewPos.x * zInv * zInv, -fy * viewPos.y * zInv * zInv, 0.0)
	);

	mat3 T   = J * viewRot;
	mat3 cov = T * cov3D * transpose(T);

	// 0.3 pixel low-pass floor, same as reference.
	return vec3(cov[0][0] + 0.3, cov[0][1], cov[1][1] + 0.3);
}

// Given a 2D covariance (Sigmaxx, Sigmaxy, Sigmayy) return the two eigenvalues and
// eigenvector of the major axis. Used to size the billboard quad on the
// screen so it bounds the ~3sigma ellipse of the Gaussian.
struct SplatEigen {
	float lambda1;  // larger eigenvalue (major axis variance)
	float lambda2;  // smaller eigenvalue (minor axis variance)
	vec2  majorAxis; // unit vector along the major axis
};

SplatEigen EvalEigen(vec3 cov)
{
	float a = cov.x;
	float b = cov.y;
	float c = cov.z;

	float mid = 0.5 * (a + c);
	float disc = sqrt(max(0.0, mid * mid - (a * c - b * b)));
	float lambda1 = mid + disc;
	float lambda2 = max(mid - disc, 0.1);

	// Eigenvector for lambda1. Guard the b ~= 0 case where Sigma is diagonal.
	vec2 axis;
	if (abs(b) < 1e-6) {
		axis = (a >= c) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
	} else {
		axis = normalize(vec2(lambda1 - c, b));
	}

	SplatEigen e;
	e.lambda1 = lambda1;
	e.lambda2 = lambda2;
	e.majorAxis = axis;
	return e;
}
