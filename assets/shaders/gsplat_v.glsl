#include "common/gsplat.glsl"

// The quad corner: 4 vertices with coords in [-1, +1] driven by
// gl_VertexID → the CPU only uploads 4 entries for one TRIANGLE_STRIP.
layout(location = 0) in vec2 a_Corner;

// Per-instance splat attributes (SoA → one buffer per column).
layout(location = 1) in vec3 a_Pos;
layout(location = 2) in vec3 a_Scale;
layout(location = 3) in vec4 a_Rot;    // quaternion (w, x, y, z)
layout(location = 4) in vec4 a_Color;  // rgba in 0..1 (CPU divides 255)

uniform mat4 u_View;
uniform mat4 u_Projection;
uniform vec2 u_ViewportSize;  // pixels — for focal-length reconstruction

out vec4 v_Color;
// Vertex-local 2D offset from the splat's screen-space centre, measured
// in units of the *inverse* 2D covariance — so  r² = dot(v_LocalPos, v_LocalPos)
// is the Mahalanobis distance squared for the fragment shader.
out vec2 v_LocalPos;

// Helper to push a vertex off-screen (clipped). Used both for behind-the-
// camera splats and for outliers whose projected ellipse would be absurdly
// large (a handful of badly-trained splats otherwise drown the viewport
// in a single bright streak).
void cull()
{
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	v_Color = vec4(0.0);
	v_LocalPos = vec2(0.0);
}

void main()
{
	// World → view space centre of the splat.
	vec4 viewPosH = u_View * vec4(a_Pos, 1.0);
	vec3 viewPos  = viewPosH.xyz;

	// Cheap near-plane cull — splats at / behind the camera project with an
	// ill-conditioned Jacobian.
	if (viewPos.z >= -0.05) { cull(); return; }

	// Focal lengths in pixels from the projection matrix.
	float fx = 0.5 * u_ViewportSize.x * u_Projection[0][0];
	float fy = 0.5 * u_ViewportSize.y * u_Projection[1][1];

	// Build 3D covariance then project to 2D screen-space covariance.
	mat3 cov3D  = Cov3D(a_Scale, a_Rot);
	mat3 viewRot = mat3(u_View);
	vec3 cov2   = Cov2D(viewPos, cov3D, viewRot, fx, fy);

	SplatEigen eig = EvalEigen(cov2);
	float majorRadius = 3.0 * sqrt(eig.lambda1);
	float minorRadius = 3.0 * sqrt(eig.lambda2);

	// Drop splats whose projected extent is larger than the viewport — these
	// are the outliers that paint bright streaks across the whole frame. A
	// reasonable 3σ ellipse is at most a few hundred pixels across; anything
	// in four-digit range is a sign of a pathological splat + Jacobian blow-up.
	if (majorRadius > 0.5 * u_ViewportSize.y) { cull(); return; }

	// Orient the billboard along the ellipse axes (in pixel units).
	vec2 majorAxis = eig.majorAxis;
	vec2 minorAxis = vec2(-majorAxis.y, majorAxis.x);

	vec2 offsetPx = majorAxis * (a_Corner.x * majorRadius)
	              + minorAxis * (a_Corner.y * minorRadius);

	// Convert from pixel offset to NDC offset: NDC unit = 2 / viewportPx.
	vec2 offsetNdc = 2.0 * offsetPx / u_ViewportSize;

	vec4 centreClip = u_Projection * viewPosH;
	vec4 clip = centreClip;
	clip.xy += offsetNdc * clip.w;  // preserve perspective divide
	gl_Position = clip;

	// Pass ellipse-local coords (radial, in sqrt(lambda) units) so the
	// fragment shader only needs dot(v_LocalPos, v_LocalPos) * 0.5 to get
	// the Gaussian exponent argument.
	v_LocalPos = a_Corner * 3.0;

	v_Color = a_Color;
}
