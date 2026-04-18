// Common Blinn-Phong-ish lighting utilities shared across fragment shaders.
// Expects the caller to provide sampled tex coords and a Material uniform.

#define NR_POINT_LIGHTS 1
#define NR_SPOT_LIGHTS  1

struct Material {
	sampler2D diffuse;
	sampler2D specular;
	sampler2D normal;
	sampler2D reflection;
	float shininess;
};

struct DirLight {
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
};

struct PointLight {
	vec3 position;
	float constant;
	float linear;
	float quadratic;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
};

struct SpotLight {
	vec3 position;
	vec3 direction;
	float cutOff;
	float outerCutOff;
	float constant;
	float linear;
	float quadratic;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
};

// Generic Phong term used by all light types.
// Returns ambient + diffuse + specular for the given surface/light directions
// and material. Attenuation / cone shaping is applied by the caller.
vec3 PhongShade(vec3 lightAmbient, vec3 lightDiffuse, vec3 lightSpecular,
                vec3 lightDir, vec3 normal, vec3 viewDir,
                vec2 uv, bool specEnabled, Material mat)
{
	float diff = max(dot(normal, lightDir), 0.0);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = specEnabled ? pow(max(dot(viewDir, reflectDir), 0.0), mat.shininess) : 0.0;

	vec3 baseDiffuse = vec3(texture(mat.diffuse, uv));
	vec3 baseSpec    = vec3(texture(mat.specular, uv)).rrr;

	vec3 ambient  = lightAmbient  * baseDiffuse;
	vec3 diffuse  = lightDiffuse  * diff * baseDiffuse;
	vec3 specular = lightSpecular * spec * baseSpec;
	return ambient + diffuse + specular;
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir,
                  vec2 uv, bool specEnabled, Material mat)
{
	vec3 lightDir = normalize(-light.direction);
	return PhongShade(light.ambient, light.diffuse, light.specular,
	                  lightDir, normal, viewDir, uv, specEnabled, mat);
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir,
                    vec2 uv, bool specEnabled, Material mat)
{
	vec3 lightDir = normalize(light.position - fragPos);
	float dist = length(light.position - fragPos);
	float atten = 1.0 / (light.constant + light.linear * dist + light.quadratic * (dist * dist));

	vec3 shaded = PhongShade(light.ambient, light.diffuse, light.specular,
	                         lightDir, normal, viewDir, uv, specEnabled, mat);
	return shaded * atten;
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir,
                   vec2 uv, bool specEnabled, Material mat)
{
	vec3 lightDir = normalize(light.position - fragPos);
	float dist = length(light.position - fragPos);
	float atten = 1.0 / (light.constant + light.linear * dist + light.quadratic * (dist * dist));

	float theta    = dot(lightDir, normalize(-light.direction));
	float epsilon  = light.cutOff - light.outerCutOff;
	float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);

	vec3 shaded = PhongShade(light.ambient, light.diffuse, light.specular,
	                         lightDir, normal, viewDir, uv, specEnabled, mat);
	return shaded * atten * intensity;
}
