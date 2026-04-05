#version 300 es
precision highp float;

layout(location = 0) in vec3 a_Position;

uniform mat4 u_ViewProjection;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 v_TexCoord;

void main()
{
	vec4 pos = u_projection * mat4(mat3(u_view)) * vec4(a_Position, 1.0);
	pos = pos.xyww;
	pos.z -= 0.000001;
	gl_Position = pos;

	v_TexCoord = a_Position;
}
