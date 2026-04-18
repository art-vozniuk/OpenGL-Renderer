#include "common/lighting.glsl"

out vec4 color;

in vec3 v_FragPos;
in vec3 v_Normal;
in vec2 v_TexCoord;
in mat3 v_TBN;

uniform vec3        u_viewPos;
uniform DirLight    u_dirLight;
uniform PointLight  u_pointLights[NR_POINT_LIGHTS];
uniform SpotLight   u_spotLights[NR_SPOT_LIGHTS];
uniform Material    u_material;
uniform int         u_normalMapping;
uniform int         u_specularMapping;
uniform int         u_dbgDisableNormalMapping;
uniform samplerCube u_cubemap;

void main()
{
	// Surface normal: either geometric or sampled from normal map (tangent-space).
	vec3 norm;
	bool nm = bool(u_normalMapping) && !bool(u_dbgDisableNormalMapping);
	if (!nm) {
		norm = normalize(v_Normal);
	} else {
		norm = texture(u_material.normal, v_TexCoord).rgb;
		norm = normalize(norm * 2.0 - 1.0);
		norm = normalize(v_TBN * norm);
	}

	vec3 viewDir = normalize(u_viewPos - v_FragPos);
	vec3 result = vec3(0.0);

	result += CalcDirLight(u_dirLight, norm, viewDir, v_TexCoord, bool(u_specularMapping), u_material);

	for (int i = 0; i < NR_POINT_LIGHTS; i++)
		result += CalcPointLight(u_pointLights[i], norm, v_FragPos, viewDir, v_TexCoord, bool(u_specularMapping), u_material);

	// Spot lights are declared but not summed into the output.

	color = vec4(result, 1.0);
}
