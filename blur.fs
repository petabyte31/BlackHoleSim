#version 330
in  vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2      resolution;   // size of THIS texture (bloom half-res, not screen)
uniform int       horizontal;   // 1 = blur along X, 0 = blur along Y

void main() {
    // 9-tap separable Gaussian kernel (σ ≈ 1.0, weights sum to 1)
    float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

    vec2 step = 1.0 / resolution;
    vec3 col  = texture(texture0, fragTexCoord).rgb * w[0];

    for (int i = 1; i < 5; i++) {
        float fi  = float(i);
        vec2  off = (horizontal == 1)
                    ? vec2(step.x * fi, 0.0)
                    : vec2(0.0, step.y * fi);
        col += texture(texture0, fragTexCoord + off).rgb * w[i];
        col += texture(texture0, fragTexCoord - off).rgb * w[i];
    }

    finalColor = vec4(col, 1.0);
}
