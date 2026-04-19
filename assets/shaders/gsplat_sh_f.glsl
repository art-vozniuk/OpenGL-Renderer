// Gaussian-splat fragment shader, SH variant. Identical to gsplat_f.glsl —
// colour assembly happens entirely in the vertex shader (SH evaluation is
// per-splat, not per-pixel), so the fragment path only needs the Mahalanobis
// weight and premultiplied-alpha output.

in vec4 v_Color;
in vec2 v_LocalPos;

out vec4 fragColor;

void main()
{
	float r2 = dot(v_LocalPos, v_LocalPos);
	if (r2 > 9.0) discard;

	float alpha = exp(-0.5 * r2) * v_Color.a;
	if (alpha < 1.0 / 255.0) discard;

	fragColor = vec4(v_Color.rgb * alpha, alpha);
}
