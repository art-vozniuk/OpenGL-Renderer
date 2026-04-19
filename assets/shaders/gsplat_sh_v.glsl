// Gaussian-splat vertex shader with view-dependent SH colour (bands 1..3).
// A near-copy of gsplat_v.glsl — same quad sizing + EWA projection maths —
// but reads an RGB texture of SH coefs per splat and adds the evaluated
// view-dependent term to the DC colour uploaded through a_Color.

#include "common/gsplat.glsl"
#include "common/sh.glsl"

layout(location = 0) in vec2 a_Corner;
layout(location = 1) in vec3 a_Pos;
layout(location = 2) in vec3 a_Scale;
layout(location = 3) in vec4 a_Rot;
layout(location = 4) in vec4 a_Color;
// Original splat index (pre-sort). The renderer's back-to-front sort
// reshuffles the other per-instance attributes each time the camera stops
// moving, but the SH texture stays in file order — so we carry a reshuffled
// index per instance to look up the correct row in the SH texture.
layout(location = 5) in uint a_OrigIdx;

uniform mat4 u_View;
uniform mat4 u_Projection;
uniform vec2 u_ViewportSize;

// View-dependent colour inputs
uniform sampler2D u_ShTex;           // RGBA32F, tiled: splatsPerRow per row
uniform vec3      u_CameraPos;       // world-space camera position (Y-up frame)
uniform int       u_ShCoefCount;     // coefs per channel (15 for degree 3)
uniform int       u_ShSplatsPerRow;  // splats packed side-by-side per row

out vec4 v_Color;
out vec2 v_LocalPos;

void cull()
{
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	v_Color = vec4(0.0);
	v_LocalPos = vec2(0.0);
}

void main()
{
	vec4 viewPosH = u_View * vec4(a_Pos, 1.0);
	vec3 viewPos  = viewPosH.xyz;

	if (viewPos.z >= -0.05) { cull(); return; }

	float fx = 0.5 * u_ViewportSize.x * u_Projection[0][0];
	float fy = 0.5 * u_ViewportSize.y * u_Projection[1][1];

	mat3 cov3D  = Cov3D(a_Scale, a_Rot);
	mat3 viewRot = mat3(u_View);
	vec3 cov2   = Cov2D(viewPos, cov3D, viewRot, fx, fy);

	SplatEigen eig = EvalEigen(cov2);
	float majorRadius = 3.0 * sqrt(eig.lambda1);
	float minorRadius = 3.0 * sqrt(eig.lambda2);

	if (majorRadius > 0.5 * u_ViewportSize.y) { cull(); return; }

	vec2 majorAxis = eig.majorAxis;
	vec2 minorAxis = vec2(-majorAxis.y, majorAxis.x);
	vec2 offsetPx  = majorAxis * (a_Corner.x * majorRadius)
	               + minorAxis * (a_Corner.y * minorRadius);
	vec2 offsetNdc = 2.0 * offsetPx / u_ViewportSize;

	vec4 centreClip = u_Projection * viewPosH;
	vec4 clip = centreClip;
	clip.xy += offsetNdc * clip.w;
	gl_Position = clip;

	v_LocalPos = a_Corner * 3.0;

	// View direction from camera to splat, in the PLY's native Y-down frame.
	// Loader flipped (y, z) → to feed SH basis we must flip back.
	vec3 dirYup   = normalize(a_Pos - u_CameraPos);
	vec3 dirPly   = vec3(dirYup.x, -dirYup.y, -dirYup.z);

	// Full SH evaluation in PLY's native Y-down frame. We use a_OrigIdx (not
	// gl_InstanceID) because the draw order is post-sort and doesn't match
	// the SH texture's original-order layout.
	vec3 shSum = EvalShFull(dirPly, u_ShTex, int(a_OrigIdx),
	                        u_ShCoefCount, u_ShSplatsPerRow);
	vec3 rgb   = clamp(vec3(0.5) + shSum, vec3(0.0), vec3(1.0));

	// Opacity still comes from the VBO (sigmoid of the PLY's logit-opacity,
	// baked to uint8 by the loader). Alpha precision at 1/255 is plenty.
	v_Color = vec4(rgb, a_Color.a);
}
