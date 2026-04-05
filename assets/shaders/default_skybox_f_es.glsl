#version 300 es
precision highp float;
precision highp samplerCube;

out vec4 color;

in vec3 v_TexCoord;

uniform samplerCube u_cube;

void main()
{
    color = texture(u_cube, v_TexCoord);
}
