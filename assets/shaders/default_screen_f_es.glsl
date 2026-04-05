#version 300 es
precision highp float;
precision highp sampler2D;

out vec4 color;

struct Material {
    sampler2D diffuse;
};

in vec2 v_TexCoord;

uniform Material u_material;
uniform float u_dbgPPOffset;
uniform int   u_dbgPPEffect;

void main()
{
    float offset = u_dbgPPOffset;
    vec2 offsets[9];
    offsets[0] = vec2(-offset,  offset);
    offsets[1] = vec2( 0.0,     offset);
    offsets[2] = vec2( offset,  offset);
    offsets[3] = vec2(-offset,  0.0);
    offsets[4] = vec2( 0.0,     0.0);
    offsets[5] = vec2( offset,  0.0);
    offsets[6] = vec2(-offset, -offset);
    offsets[7] = vec2( 0.0,    -offset);
    offsets[8] = vec2( offset, -offset);

    // GLSL ES 3.0 requires constant expressions for array init,
    // so we fill kernels via if-chains below.

    if (u_dbgPPEffect == 0) {
        color = texture(u_material.diffuse, v_TexCoord);
        return;
    }

    vec3 sampleTex[9];
    for (int i = 0; i < 9; i++) {
        sampleTex[i] = vec3(texture(u_material.diffuse, v_TexCoord.st + offsets[i]));
    }

    float k[9];
    if (u_dbgPPEffect == 1) {
        // Sharpen
        k[0]=-1.0; k[1]=-1.0; k[2]=-1.0;
        k[3]=-1.0; k[4]= 9.0; k[5]=-1.0;
        k[6]=-1.0; k[7]=-1.0; k[8]=-1.0;
    } else if (u_dbgPPEffect == 2) {
        // Gaussian blur
        k[0]=1.0/16.0; k[1]=2.0/16.0; k[2]=1.0/16.0;
        k[3]=2.0/16.0; k[4]=4.0/16.0; k[5]=2.0/16.0;
        k[6]=1.0/16.0; k[7]=2.0/16.0; k[8]=1.0/16.0;
    } else {
        // Edge detection
        k[0]=1.0; k[1]= 1.0; k[2]=1.0;
        k[3]=1.0; k[4]=-8.0; k[5]=1.0;
        k[6]=1.0; k[7]= 1.0; k[8]=1.0;
    }

    vec3 col = vec3(0.0);
    for (int i = 0; i < 9; i++)
        col += sampleTex[i] * k[i];

    color = vec4(col, 1.0);
}
