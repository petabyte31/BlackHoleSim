#version 330
// ─────────────────────────────────────────────────────────────────────────────
// sky.fs — the galactic backdrop for Physics Sim mode.
//
// Same starfield + nebula as lens.fs, but with NO ray marching: physics mode
// doesn't lens, so each pixel just reconstructs its camera ray and samples the
// sky directly. One cheap fullscreen pass. Its job is ambience and, more
// importantly, CONTRAST — without it the black hole is black on black.
// ─────────────────────────────────────────────────────────────────────────────

in  vec2 fragTexCoord;
out vec4 finalColor;

uniform vec2  resolution;
uniform float time;
uniform vec3  camForward;
uniform vec3  camRight;
uniform vec3  camUp;
uniform float fovY;
uniform float nebulaStrength;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),              hash(i + vec2(1, 0)), f.x),
               mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

float fbm2(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 5; i++) { sum += amp * noise(p); p *= 2.0; amp *= 0.5; }
    return sum;
}

vec3 starColor(vec3 dir) {
    float az = atan(dir.z, dir.x);
    float el = asin(clamp(dir.y, -1.0, 1.0));
    vec2  grid = vec2(az, el) * 40.0;
    vec2  cell = floor(grid), frac = fract(grid);
    float isStar  = step(0.985, hash(cell));
    float glow    = smoothstep(0.25, 0.0, length(frac - 0.5));
    float twinkle = 0.6 + 0.4 * sin(time * 2.0 + hash(cell) * 6.2831);
    return (isStar * glow * twinkle) * vec3(0.8, 0.9, 1.0);
}

vec3 nebulaColor(vec3 dir) {
    float az = atan(dir.z, dir.x);
    float el = asin(clamp(dir.y, -1.0, 1.0));
    vec2  sky = vec2(az, el);
    float density = fbm2(sky * 2.5);
    float detail  = fbm2(sky * 6.0 + 3.7);
    float cloud   = smoothstep(0.50, 0.92, density) * (0.4 + 0.6 * detail);
    vec3  c = mix(vec3(0.09, 0.12, 0.30), vec3(0.40, 0.18, 0.48), smoothstep(0.4, 0.7, density));
    c       = mix(c,                      vec3(0.80, 0.42, 0.32), smoothstep(0.7, 1.0, density));
    return c * cloud;
}

void main() {
    vec2  uv   = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    uv.x      *= resolution.x / resolution.y;
    float fovT = tan(fovY * 0.5);
    vec3  dir  = normalize(camForward + uv.x * fovT * camRight + uv.y * fovT * camUp);
    finalColor = vec4(starColor(dir) + nebulaColor(dir) * nebulaStrength, 1.0);
}