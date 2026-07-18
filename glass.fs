#version 330
// ─────────────────────────────────────────────────────────────────────────────
// glass.fs — "Liquid Glass" pill. Refracts the backdrop (the black-hole scene)
// behind a rounded-capsule button: the edges bend and magnify what's behind
// them like a real lens, with a bright rim-light and a thin chromatic fringe.
//
// Drawn by covering the button's padded box with a rectangle; this shader
// computes the capsule shape with a signed-distance field and sums:
//   refracted backdrop · dark tint  +  rim-light  +  inner line  +  outer glow
// ─────────────────────────────────────────────────────────────────────────────

in  vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // raylib default (unused)
uniform sampler2D scene;      // the HDR backdrop to refract (hdrBuf)

uniform vec2  uReso;          // framebuffer resolution (px)
uniform vec2  uCenter;        // pill centre, framebuffer px (top-left origin)
uniform vec2  uHalf;          // pill half-extents (px)
uniform float uRadius;        // corner radius (px)
uniform float uHover;         // 0..1 eased hover
uniform float uAppear;        // 0..1 fade-in
uniform vec3  uAccent;        // per-mode accent colour
uniform float uScale;         // framebuffer px per screen unit (DPI scale)

// Signed distance to a rounded rectangle (capsule when radius = min half-extent).
float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec3 aces(vec3 x) {   // tone-map the HDR sample to match the composited screen
    return clamp((x*(2.51*x + 0.03)) / (x*(2.43*x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 samp(vec2 uv) { return aces(texture(scene, clamp(uv, 0.002, 0.998)).rgb); }

void main() {
    float S = uScale;

    // Fragment position in top-left screen space (gl_FragCoord is bottom-left).
    vec2 fragTL = vec2(gl_FragCoord.x, uReso.y - gl_FragCoord.y);
    vec2 p      = fragTL - uCenter;

    float sd = sdRoundRect(p, uHalf, uRadius);
    if (sd > 16.0 * S) discard;                       // beyond glow reach

    float inside = 1.0 - smoothstep(-1.0*S, 1.0*S, sd);      // 1 inside, 0 out (AA)
    float rim    = smoothstep(-22.0*S, 0.0, sd) * inside;    // 0 centre → 1 at edge

    // Backdrop sample coord. NOTE: flip Y to match the on-screen backdrop; if the
    // cosmos seen *through* the glass looks vertically inverted, change to `su.y`.
    vec2 su = fragTL / uReso;
    vec2 sv = vec2(su.x, 1.0 - su.y);

    // Surface normal from the SDF gradient.
    float o  = 1.5 * S;
    float dx = sdRoundRect(p+vec2(o,0), uHalf, uRadius) - sdRoundRect(p-vec2(o,0), uHalf, uRadius);
    float dy = sdRoundRect(p+vec2(0,o), uHalf, uRadius) - sdRoundRect(p-vec2(0,o), uHalf, uRadius);
    vec2  nrm = normalize(vec2(dx, dy) + 1e-6);

    // Refraction: bend outward near the rim (lens bulge) + gentle magnification.
    vec2 centerSv = vec2(uCenter.x/uReso.x, 1.0 - uCenter.y/uReso.y);
    vec2 refr = nrm * (rim*rim) * (30.0*S) / uReso;
    vec2 mag  = (centerSv - sv) * 0.06 * inside;
    vec2 base = sv + refr + mag;

    // Chromatic fringe along the refraction direction, strongest at the rim.
    vec2 ca = nrm * (rim*rim) * (7.0*S) / uReso;
    vec3 refracted = vec3(samp(base+ca).r, samp(base).g, samp(base-ca).b);

    // Dark cosmic glass tint, slightly darker in the centre so the label reads.
    vec3 tint = mix(vec3(0.30, 0.36, 0.50), uAccent*0.6 + 0.25, 0.15);
    vec3 frost = vec3(0.05, 0.06, 0.10);
    vec3 col   = mix(frost, refracted * tint, 0.72) * (0.85 + 0.35*uHover);
    col *= (0.72 + 0.28*rim);

    // Bright rim-light (the glassy edge catch).
    float edge = 1.0 - smoothstep(0.0, 2.0*S, abs(sd));
    col += mix(vec3(1.0), uAccent, 0.35) * edge * (0.55 + 0.6*uHover);

    // Faint inner accent line for depth.
    float inner = 1.0 - smoothstep(0.0, 3.0*S, abs(sd + 6.0*S));
    col += uAccent * inner * (0.10 + 0.15*uHover);

    // Soft outer accent glow beyond the edge (blooms on hover).
    float glow = (1.0 - smoothstep(0.0, 16.0*S, sd)) * (1.0 - inside);
    col += uAccent * glow * (0.12 + 0.5*uHover);

    float alpha = max(inside, glow * (0.25 + 0.5*uHover));
    finalColor = vec4(col, alpha * uAppear);
}