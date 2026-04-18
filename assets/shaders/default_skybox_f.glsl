out vec4 color;

in vec3 v_TexCoord;

uniform samplerCube u_cube;

void main()
{
	color = texture(u_cube, v_TexCoord);
}
