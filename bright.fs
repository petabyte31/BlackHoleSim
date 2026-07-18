#version 330
in  vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // full HDR render
uniform float threshold;      // luminance cut-off (e.g. 0.65)

void main() {
    vec3  col  = texture(texture0, fragTexCoord).rgb;
    // Perceptual luminance — keeps colour ratios intact while selecting bright pixels
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    // Smooth knee: pixels well above threshold contribute fully,
    // pixels near the threshold blend in gradually, darkness stays dark.
    col *= smoothstep(threshold, threshold + 0.3, luma);
    finalColor = vec4(col, 1.0);
}
