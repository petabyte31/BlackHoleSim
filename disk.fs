#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform float time;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1, 0));
    float c = hash(i + vec2(0, 1));
    float d = hash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Flowing matter: turbulence that orbits DIFFERENTIALLY (inner faster) but
// never winds. Two time-staggered layers, each advected then reset every
// `life` seconds, cross-faded so the reset is invisible (finite lifetime).
// Noise is sampled on a ring (cos/sin) so it's seamless around the disk.
float flowNoise(float rNorm, float theta, float time) {
    float orbit = 0.7 / (rNorm + 0.3);     // differential drift, inner faster
    float life  = 6.0;                      // feature lifetime (seconds)
    float ph    = time / life;

    float fA = fract(ph);
    float fB = fract(ph + 0.5);             // half a lifetime out of phase

    float angA = theta - orbit * fA * life;
    float angB = theta - orbit * fB * life;

    float ringR = 2.0 + rNorm * 4.0;        // feature count around the ring
    vec2  baseA = vec2(cos(angA), sin(angA)) * ringR + vec2(rNorm * 5.0, 0.0);
    vec2  baseB = vec2(cos(angB), sin(angB)) * ringR + vec2(rNorm * 5.0, 0.0);

    float nA = noise(baseA) * 0.6 + noise(baseA * 2.0) * 0.4;
    float nB = noise(baseB) * 0.6 + noise(baseB * 2.0) * 0.4;

    float wA = 1.0 - abs(2.0 * fA - 1.0);   // 0 at A's reset, 1 mid-life
    return mix(nB, nA, wA);
}

void main() {
    vec2  p = fragTexCoord - 0.5;
    float r = length(p) * 2.0;
    if (r < 0.2 || r > 0.95) discard;

    float rNorm = (r - 0.2) / 0.75;
    float theta = atan(p.y, p.x);

    // Subtle rigid spiral density wave (large-scale structure, never winds).
    const float PATTERN_SPEED = 0.25;
    const float ARMS = 2.0;
    const float WIND = 3.0;
    float patternAngle = mod(PATTERN_SPEED * time, 6.28318530718);
    float spiral = cos(ARMS * (theta - patternAngle) - WIND * log(r));
    float arms   = smoothstep(-0.3, 0.9, spiral);

    // Flowing matter (differential, finite-lifetime).
    float flow = flowNoise(rNorm, theta, time);

    // ---- Warm temperature palette (saturated, no grey wash) ----
    float temp = 1.0 - rNorm;                 // 0 outer, 1 inner
    vec3 cOuter = vec3(0.5, 0.08, 0.02);      // deep ember red
    vec3 cMid   = vec3(1.0, 0.45, 0.12);      // orange
    vec3 cInner = vec3(1.0, 0.80, 0.40);      // golden
    vec3 color;
    if (temp < 0.5) color = mix(cOuter, cMid, temp / 0.5);
    else            color = mix(cMid, cInner, (temp - 0.5) / 0.5);
    // white-hot ONLY at the very inner lip
    color = mix(color, vec3(1.0, 0.95, 0.85), smoothstep(0.88, 1.0, temp));

    // Brightness: flowing matter, with a gentle arm-density boost.
    float brightness = (0.45 + 0.55 * flow) * (0.8 + 0.2 * arms);
    color *= brightness;

    float edgeFade = smoothstep(0.20, 0.28, r) * smoothstep(0.95, 0.80, r);
    finalColor = vec4(color * edgeFade, edgeFade);
    vec3 p = camPos;
vec3 v = rayDir;

vec3  L  = cross(p - bhPos, v);
float h2 = dot(L, L);                 // conserved, computed once

vec3  color   = vec3(0.0);            // accumulated light
float transmit = 1.0;                 // how much is still see-through

const int   STEPS = 200;              // budget (tune for FPS vs quality)
const float DL    = 2.0;              // step length in render units (tune)

for (int i = 0; i < STEPS; i++) {
    vec3  rel = p - bhPos;
    float r2  = dot(rel, rel);

    // 1. captured by the hole -> stop, stays black
    if (r2 < rs * rs) { transmit = 0.0; break; }

    // 2. bend the velocity (the geodesic step)
    vec3 accel = -1.5 * rs * h2 * rel / pow(r2, 2.5);
    v += accel * DL;

    // 3. remember old position, then move forward
    vec3 pPrev = p;
    p += v * DL;

    // 4. did we cross the disk plane (y = 0) this step?
    if (pPrev.y * p.y < 0.0) {                 // sign flip = crossed
        float t   = pPrev.y / (pPrev.y - p.y); // where between the two
        vec3  hit = mix(pPrev, p, t);          // exact crossing point
        float rad = length(hit.xz - bhPos.xz); // radius in the disk plane
        if (rad > diskInner && rad < diskOuter) {
            vec4 d = diskColor(hit);           // your disk math, as a function
            color   += transmit * d.rgb * d.a; // add its glow
            transmit *= (1.0 - d.a);           // disk is semi-transparent
        }
    }

    // 5. escaped to infinity -> sample the stars in the current direction
    if (r2 > escapeR * escapeR) break;
}

color += transmit * starColor(v);    // whatever light still gets through
finalColor = vec4(color, 1.0);
}