#version 330
in  vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;     // full-res HDR scene (from lens.fs)
uniform sampler2D bloom;        // blurred bright pixels (half-res, upsampled on read)
uniform float     bloomStrength; // additive weight of the bloom layer

// ACES filmic tone map — keeps saturated colours and avoids the grey wash of Reinhard
vec3 acesFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr  = texture(texture0, fragTexCoord).rgb;
    // Bloom was rendered to a half-res texture; GPU bilinear interpolation handles
    // the upscale automatically when we sample it at full-res UVs.
    vec3 glow = texture(bloom, fragTexCoord).rgb;

    vec3 col = hdr + glow * bloomStrength;
    col = acesFilm(col);

    finalColor = vec4(col, 1.0);
}
