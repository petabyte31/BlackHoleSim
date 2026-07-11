#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform vec2  resolution;
uniform float time;
uniform vec3  camForward;
uniform vec3  camRight;
uniform vec3  camUp;
uniform float fovY;
uniform vec3  camPos;
uniform vec3  bhPos;
uniform float rs;
uniform float diskInner;   // inner edge of disk, render units
uniform float diskOuter;   // outer edge of disk, render units
uniform float diskBoost;   // accretion-flare brightness multiplier (1 = normal)

// ── noise ────────────────────────────────────────────────────────────────────

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),           hash(i + vec2(1, 0)), f.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
}

float flowNoise(float rNorm, float theta, float t) {
    float orbit = 0.7 / (rNorm + 0.3);
    float life  = 6.0;
    float ph    = t / life;
    float fA    = fract(ph);
    float fB    = fract(ph + 0.5);
    float angA  = theta - orbit * fA * life;
    float angB  = theta - orbit * fB * life;
    float ringR = 2.0 + rNorm * 4.0;
    vec2 baseA  = vec2(cos(angA), sin(angA)) * ringR + vec2(rNorm * 5.0, 0.0);
    vec2 baseB  = vec2(cos(angB), sin(angB)) * ringR + vec2(rNorm * 5.0, 0.0);
    float nA    = noise(baseA) * 0.6 + noise(baseA * 2.0) * 0.4;
    float nB    = noise(baseB) * 0.6 + noise(baseB * 2.0) * 0.4;
    float wA    = 1.0 - abs(2.0 * fA - 1.0);
    return mix(nB, nA, wA);
}

// ── accretion disk ───────────────────────────────────────────────────────────
// Sampled at any 3D hit point in the disk plane (y == bhPos.y).
// Returns (rgb, alpha) ready for Porter-Duff compositing.

vec4 diskColor(vec3 hit) {
    vec2  hitXZ = hit.xz - bhPos.xz;
    float rad   = length(hitXZ);
    if (rad < diskInner || rad > diskOuter) return vec4(0.0);

    float rNorm = clamp((rad - diskInner) / (diskOuter - diskInner), 0.0, 1.0);
    float theta = atan(hit.z - bhPos.z, hit.x - bhPos.x);


    // Spiral density wave (rotates slowly, never winds up)
    const float PATTERN_SPEED = 0.25;
    const float ARMS = 2.0;
    const float WIND = 3.0;
    float patternAngle = mod(PATTERN_SPEED * time, 6.28318530718);
    float spiral = cos(ARMS * (theta - patternAngle) - WIND * log(rNorm + 0.1));
    float arms   = smoothstep(-0.3, 0.9, spiral);

    // Fine radial banding (compressed toward inner edge — more structure where it's hotter)
    float bands = 0.70 + 0.30 * sin(pow(1.0 - rNorm, 0.6) * 28.0 - time * 0.3);

    // Flowing turbulence (differential rotation, seamless reset)
    float flow = flowNoise(rNorm, theta, time);

    // Temperature palette: deep crimson → red-orange → hot orange → white-hot inner lip
    float temp  = 1.0 - rNorm;
    vec3 cOuter = vec3(0.30, 0.01, 0.0);    // deep crimson
    vec3 cMid   = vec3(0.85, 0.12, 0.01);   // vivid red
    vec3 cInner = vec3(1.0,  0.50, 0.08);   // hot orange
    vec3 col;
    if (temp < 0.5) col = mix(cOuter, cMid,   temp * 2.0);
    else            col = mix(cMid,   cInner, (temp - 0.5) * 2.0);
    col = mix(col, vec3(1.0, 0.88, 0.60), smoothstep(0.85, 1.0, temp));  // white-hot core

    // Combine: flow noise, spiral arms, fine bands, inner glow
    float innerGlow  = 1.0 + 1.2 * pow(temp, 3.0);   // gentle inner brightening
    float brightness = (0.35 + 0.65 * flow) * (0.75 + 0.25 * arms) * bands * innerGlow;
    col *= brightness;

    // Soft fade at inner and outer edges
    float edgeFade = smoothstep(0.0, 0.06, rNorm) * smoothstep(1.0, 0.88, rNorm);
    return vec4(col * edgeFade * diskBoost, edgeFade);
}

// ── stars ─────────────────────────────────────────────────────────────────────
// Pass a geodesic-bent direction here to get gravitational lensing of the sky.

vec3 starColor(vec3 dir) {
    float az = atan(dir.z, dir.x);
    float el = asin(clamp(dir.y, -1.0, 1.0));
    vec2 grid = vec2(az, el) * 40.0;
    vec2 cell = floor(grid);
    vec2 frac = fract(grid);
    float rnd     = hash(cell);
    float isStar  = step(0.985, rnd);
    float d       = length(frac - 0.5);
    float glow    = smoothstep(0.25, 0.0, d);
    float twinkle = 0.6 + 0.4 * sin(time * 2.0 + rnd * 6.2831);
    return (isStar * glow * twinkle) * vec3(0.8, 0.9, 1.0);
}

// ── main ─────────────────────────────────────────────────────────────────────

void main() {
    // Reconstruct world-space ray direction for this pixel
    vec2 uv    = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    uv.x      *= resolution.x / resolution.y;
    float fovT = tan(fovY * 0.5);
    vec3 v     = normalize(camForward + uv.x * fovT * camRight + uv.y * fovT * camUp);
    vec3 p     = camPos;

    // BH behind the camera: no bending, sample stars directly
    float tCA = dot(bhPos - camPos, v);
    if (tCA <= 0.0) {
        finalColor = vec4(starColor(v), 1.0);
        return;
    }

    // Angular momentum squared — conserved along the null geodesic, computed once
    vec3  L  = cross(p - bhPos, v);
    float h2 = dot(L, L);

    // Jump ahead to the interesting zone, skip the empty space between camera and BH
    const int   STEPS  = 200;
    const float HALF_M = 400.0;   // start this many render units before closest approach
    p = camPos + v * max(0.0, tCA - HALF_M);

    // Ray march: Schwarzschild null geodesic + disk plane intersection
    vec3  col     = vec3(0.0);
    float transmit = 1.0;

    for (int i = 0; i < STEPS; i++) {
        vec3  rel = p - bhPos;
        float r2  = dot(rel, rel);

        // Captured inside the photon sphere — any inward-bound ray here falls to the singularity
        if (r2 < 2.25 * rs * rs && dot(rel, v) < 0.0) { transmit = 0.0; break; }

        // Adaptive step: fine near the hole where bending is strong, coarse in empty space
        float dl = clamp(0.05 * sqrt(r2), 0.5, 8.0);

        // Geodesic deflection (Schwarzschild null-geodesic approximation)
        vec3 accel = -1.5 * rs * h2 * rel / pow(r2, 2.5);
        v += accel * dl;

        vec3 pPrev = p;
        p += v * dl;

        // Did the ray cross the disk plane (y = bhPos.y) this step?
        float yPrev = pPrev.y - bhPos.y;
        float yNext = p.y    - bhPos.y;
        if (yPrev * yNext < 0.0) {
            float frac = yPrev / (yPrev - yNext);   // where between pPrev and p
            vec3  hit  = mix(pPrev, p, frac);
            vec4  d    = diskColor(hit);
            col      += transmit * d.rgb * d.a;     // add disk glow
            transmit *= (1.0 - d.a);                // disk attenuates what passes through
        }
    }

    // Remaining transmittance reaches the sky — sample stars in bent direction
    col += transmit * starColor(normalize(v));

    finalColor = vec4(col, 1.0);
}
