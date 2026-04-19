in vec4 v_Color;
// Ellipse-local position in units of sqrt(lambda) -- same scale as the
// vertex-side `a_Corner * 3.0`, so |v_LocalPos|^2 is the Mahalanobis
// distance squared between this fragment and the splat centre.
in vec2 v_LocalPos;

out vec4 fragColor;

void main()
{
	// 2D Gaussian weight. exp(-0.5 * r^2) with r = |LocalPos|.
	float r2 = dot(v_LocalPos, v_LocalPos);
	// Hard 3sigma cutoff. Using the same constant the quad was sized with
	// avoids a soft rectangular seam around the ellipse.
	if (r2 > 9.0) discard;

	float alpha = exp(-0.5 * r2) * v_Color.a;
	if (alpha < 1.0 / 255.0) discard;

	// Output premultiplied alpha so the OIT-ish front-to-back / back-to-
	// front blend can work with `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
	fragColor = vec4(v_Color.rgb * alpha, alpha);
}
