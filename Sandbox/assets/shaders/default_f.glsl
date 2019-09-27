#version 330 core
out vec4 color;

struct Material {
    sampler2D diffuse;
    sampler2D specular;
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

#define NR_POINT_LIGHTS 1
#define NR_SPOT_LIGHTS 1

in vec3 v_FragPos;
in vec3 v_Normal;
in vec2 v_TexCoord;

uniform vec3		u_viewPos;
uniform DirLight	u_dirLight;
uniform PointLight	u_pointLights[NR_POINT_LIGHTS];
uniform SpotLight	u_spotLights[NR_SPOT_LIGHTS];
uniform Material	u_material;

vec3 CalcDirLight(DirLight light, vec3 v_Normal, vec3 viewDir);
vec3 CalcPointLight(PointLight light, vec3 v_Normal, vec3 v_FragPos, vec3 viewDir);
vec3 CalcSpotLight(SpotLight light, vec3 v_Normal, vec3 v_FragPos, vec3 viewDir);

void main()
{    
    // properties
    vec3 norm = normalize(v_Normal);
    vec3 viewDir = normalize(u_viewPos - v_FragPos);

	vec3 result = vec3(0.f, 0.f, 0.f);
    result += CalcDirLight(u_dirLight, norm, viewDir);

    for(int i = 0; i < NR_POINT_LIGHTS; i++)
        result += CalcPointLight(u_pointLights[i], norm, v_FragPos, viewDir); 

    for(int i = 0; i < NR_SPOT_LIGHTS; i++)
		result += CalcSpotLight(u_spotLights[i], norm, v_FragPos, viewDir);    
    
    color = vec4(result, 1.0);
}

// calculates the color when using a directional light.
vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir)
{
    vec3 lightDir = normalize(-light.direction);
    // diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    // specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_material.shininess);
	// combine
    vec3 ambient = light.ambient * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 diffuse = light.diffuse * diff * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 specular = light.specular * spec * vec3(texture(u_material.specular, v_TexCoord));
    return (ambient + diffuse + specular);
}

// calculates the color when using a point light.
vec3 CalcPointLight(PointLight light, vec3 normal, vec3 v_FragPos, vec3 viewDir)
{
    vec3 lightDir = normalize(light.position - v_FragPos);
    // diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    // specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_material.shininess);
    // attenuation
    float distance = length(light.position - v_FragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));    
    // combine
    vec3 ambient = light.ambient * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 diffuse = light.diffuse * diff * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 specular = light.specular * spec * vec3(texture(u_material.specular, v_TexCoord));
    ambient *= attenuation;
    diffuse *= attenuation;
    specular *= attenuation;
    return (ambient + diffuse + specular);
}

// calculates the color when using a spot light.
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 v_FragPos, vec3 viewDir)
{
    vec3 lightDir = normalize(light.position - v_FragPos);
    // diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    // specular
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), u_material.shininess);
    // attenuation
    float distance = length(light.position - v_FragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));    
    // intensity
    float theta = dot(lightDir, normalize(-light.direction)); 
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
    // combine
    vec3 ambient = light.ambient * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 diffuse = light.diffuse * diff * vec3(texture(u_material.diffuse, v_TexCoord));
    vec3 specular = light.specular * spec * vec3(texture(u_material.specular, v_TexCoord));
    ambient *= attenuation * intensity;
    diffuse *= attenuation * intensity;
    specular *= attenuation * intensity;
    return (ambient + diffuse + specular);
}