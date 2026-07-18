#version 330
// ─────────────────────────────────────────────────────────────────────────────
// lens.fs — the whole scene, computed per pixel.
//
// A light ray is fired from the camera and marched BACKWARDS through the
// Schwarzschild metric, accumulating whatever it meets along the bent path:
//   · the accretion disk  — THIN plane for the quiet black hole (low accretion
//                           rate), VOLUMETRIC slim disk + dusty feeding torus
//                           for the quasar (near-Eddington, puffs up to H/R≈0.3)
//   · the jets            — plasma launched from the axial funnel, collimated
//                           to a waist, then diverging conically
//   · the sky             — stars + nebula in the ray's final direction
// Everything is sampled at the BENT position, so it all lenses for free.
//
// Disk temperature follows Shakura–Sunyaev: T ∝ r^(-3/4)·(1−√(r_isco/r))^(1/4),
// so the blue/white UV core is compact near the ISCO and the sprawl is orange.
//
// All march constants scale off `diskOuter`/`rs` — changing the render scale
// can't break the step budget.
// ─────────────────────────────────────────────────────────────────────────────

in  vec2 fragTexCoord;
out vec4 finalColor;

uniform vec2  resolution;
uniform float time;

uniform vec3  camForward;
uniform vec3  camRight;
uniform vec3  camUp;
uniform float fovY;
uniform vec3  camPos;

uniform vec3  bhPos;
uniform float rs;             // Schwarzschild radius, render units
uniform float diskInner;      // inner edge (ISCO), render units
uniform float diskOuter;      // outer edge, render units
uniform float diskBoost;      // disk luminosity multiplier

uniform float quasarMode;     // 0 = thin warm black hole, 1 = volumetric quasar
uniform float jetStrength;
uniform float nebulaStrength;

// ── Tuning ────────────────────────────────────────────────────────────────────
const int   STEPS      = 150;
const float H_DISK     = 0.10;   // slim-disk aspect ratio at the vortex throat
const float V_FLARE    = 0.15;   // mouth opening — keep LOW or it becomes a cup
const float V_POWER    = 1.5;    // funnel profile: >1 = paraboloid trumpet
const float V_TAPER    = 0.50;   // rNorm where the mouth starts closing again —
                                 // makes the profile a LENS, not a trumpet+wall
const float V_FALLOFF  = 0.46;   // radius (× diskOuter) where the gas has thinned
const float V_TAIL     = 1.25;   // falloff sharpness: LOWER = fatter dust tail
                                 // reaching further out before it dies
const float V_RAGGED   = 0.45;   // warps the fade radius itself, so the boundary
                                 // is torn rather than a perfect circle
const float SHRED_FROM = 0.70;   // rNorm past which the gas is violently shredded
const float GAS_FREQ   = 0.40;   // clump size: LOWER = bigger, smoother blobs
const float GAS_PUFF   = 0.50;   // frequency multiplier at the rim: <1 grows the
                                 // outer clumps into big soft puffs
const float GAS_LO     = 0.30;   // clump shoulder — start of the soft ramp
const float GAS_HI     = 0.74;   // clump shoulder — end of the soft ramp
const float GAS_FLOOR  = 0.20;   // density in the voids between clumps
const float GAS_PEAK   = 2.00;   // density at the heart of a clump
const float CLUMP_FROM = 0.30;   // rNorm where fragmentation begins — inside this
                                 // the disk is smooth plasma, outside it breaks up
const float TORUS_FROM = 0.45;   // rNorm where the disk gives way to dusty inflow
const float TORUS_DENS = 0.34;   // torus density vs disk — diffuse gas, not a wall
const float TORUS_EMIS = 0.13;   // torus emissivity — dust radiates in the INFRARED,
                                 // so it mostly obscures; a little keeps the tail visible
const float DISK_KAPPA = 1.2;    // ↑ = more opaque, ↓ = more over-the-top wrap
const float JET_GAIN   = 1.6;    // jet luminosity
const float T_PEAK     = 2.05;   // normalises the S–S profile to peak ≈ 1

// ── Noise ─────────────────────────────────────────────────────────────────────

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),              hash(i + vec2(1, 0)), f.x),
               mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

// Cheap sin-free hash (fract/mul only) — 8 of these beat 2 sin() calls.
float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Proper trilinear 3D value noise. The old version hashed a *continuous*
// coordinate, which returns white noise — and fBm of white noise averages into
// featureless grey fog. That is why the gas never clumped, no matter the tuning.
float vnoise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i + vec3(0,0,0)), hash3(i + vec3(1,0,0)), f.x),
                   mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), f.x),
                   mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm3(vec3 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 3; i++) { sum += amp * vnoise3(p); p *= 2.1; amp *= 0.5; }
    return sum * 1.45;                       // renormalise to ~0..1
}

// Gas clumps: only two octaves, deliberately coarse. Fine octaves are worse than
// useless here — they are smaller than the march step, so they alias into grain
// instead of resolving as structure. Big soft blobs are both truer to gas at
// this scale and cheaper (16 hashes vs 48).
float gasNoise(vec3 p) {
    return vnoise3(p) * 0.66 + vnoise3(p * 2.05 + 7.0) * 0.34;
}

float fbm2(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 5; i++) { sum += amp * noise(p); p *= 2.0; amp *= 0.5; }
    return sum;
}

// Differentially-orbiting turbulence with finite feature lifetime, so it flows
// with the disk instead of winding into thin filaments over time.
float flowNoise(float rNorm, float theta, float t) {
    float orbit = 0.7 / (rNorm + 0.3);
    float life  = 6.0;
    float ph    = t / life;
    float fA    = fract(ph);
    float fB    = fract(ph + 0.5);
    float angA  = theta - orbit * fA * life;
    float angB  = theta - orbit * fB * life;
    float ringR = 2.0 + rNorm * 4.0;
    vec2  baseA = vec2(cos(angA), sin(angA)) * ringR + vec2(rNorm * 5.0, 0.0);
    vec2  baseB = vec2(cos(angB), sin(angB)) * ringR + vec2(rNorm * 5.0, 0.0);
    float nA    = noise(baseA) * 0.6 + noise(baseA * 2.0) * 0.4;
    float nB    = noise(baseB) * 0.6 + noise(baseB * 2.0) * 0.4;
    float wA    = 1.0 - abs(2.0 * fA - 1.0);
    return mix(nB, nA, wA);
}

// ── Disk geometry (quasar only) ────────────────────────────────────────────────

// The whole structure is one VORTEX: a circular paraboloid whose tip is the
// black hole and whose mouth opens outward and dissolves into space. A slim
// throat near the ISCO (H ∝ r) plus a trumpet that flares as r^V_POWER.
float diskHalfHeight(float rad) {
    float rN     = clamp(rad / diskOuter, 0.0, 1.0);
    float throat = H_DISK * rad;                                    // slim inner disk
    float funnel = V_FLARE * diskOuter * pow(rN, V_POWER);          // paraboloid walls

    // ...then close the mouth again. A funnel that only ever widens has to END
    // somewhere, and that end IS the wall. Tapering it back down makes the gas
    // thin out as it dissipates — the profile becomes a lens that feathers into
    // wisps at the rim instead of a cylinder that stops.
    funnel *= 1.0 - smoothstep(V_TAPER, 1.0, rN);
    return throat + funnel;
}

// Density of disk/torus material at a world point. Cheap culls before any noise.
float diskDensity(vec3 pos) {
    vec3  rel = pos - bhPos;
    float rad = length(rel.xz);
    if (rad < diskInner) return 0.0;

    float rSph = length(rel);                                 // SPHERICAL radius
    if (rSph > diskOuter * 1.15) return 0.0;                  // cull, far past fade

    float H  = diskHalfHeight(rad);
    float z  = abs(rel.y);
    float zn = z / max(H, 1.0);
    if (zn > 2.2) return 0.0;                                 // cull, far past fade

    float rN = clamp((rad - diskInner) / (diskOuter - diskInner), 0.0, 1.0);

    // Vertical: Gaussian against the vortex wall — no surface, no face.
    float vert = exp(-zn * zn * 1.7);

    // Radial dilution. Crucially the fade radius is WARPED by low-frequency
    // noise first: a perfectly radial falloff is a perfect circle no matter how
    // clumpy its contents, so the envelope itself has to be torn.
    float eN    = gasNoise(rel * (0.20 / rs) + 31.0);
    float rWarp = rSph * (1.0 + (eN - 0.5) * V_RAGGED);

    // V_TAIL < 2 gives a long, fat tail — thin dust reaching well past the ring
    // and dissolving into the cosmos.
    float dilute = exp(-pow(rWarp / (diskOuter * V_FALLOFF), V_TAIL));
    // A fat tail is still non-zero at the cull, which would show as a rim. Ease
    // to zero on the WARPED radius so the guarantee is ragged too.
    dilute *= 1.0 - smoothstep(0.70, 1.05, rWarp / diskOuter);
    float inner  = smoothstep(diskInner, diskInner * 1.4, rad);

    // Gas is random but not granular. Low-frequency noise for big blobs, shaped
    // by a smoothstep so each clump has SOFT shoulders — a gradient between knot
    // and void, exactly like the colour ramp. pow() thresholds; smoothstep eases.
    // Clump SCALE grows outward: tight structure in the disk, big soft puffs in
    // the outer dust. Same noise, coarser sampling — puffiness for free.
    float freq   = mix(GAS_FREQ, GAS_FREQ * GAS_PUFF, smoothstep(TORUS_FROM, 1.0, rN));
    vec3  q      = vec3(rel.xz * (freq / rs), rel.y * (freq / rs) + time * 0.06);
    float clumpy = GAS_FLOOR + GAS_PEAK * smoothstep(GAS_LO, GAS_HI, gasNoise(q));

    // Fragmentation ramps in with radius. The inner disk is hot, ionized,
    // continuous plasma — it must stay SMOOTH. Only past the self-gravity/dust
    // regime does the flow break into discrete knots, maximal at the rim.
    float clumpAmt = smoothstep(CLUMP_FROM, 1.0, rN);
    float turb     = mix(1.0, clumpy, clumpAmt);

    // Degradation escalates outward: the outermost gas is the most shredded, so
    // a second finer break-up bites only at the rim — entropy is maximal where
    // the structure is dying, not uniform across it.
    float shred = smoothstep(SHRED_FROM, 1.0, rN);
    if (shred > 0.0) {
        float fine = smoothstep(0.30, 0.80, gasNoise(q * 2.6 + 51.0));
        turb *= mix(1.0, 0.08 + 1.55 * fine, shred);
    }

    // Spiral streams winding down the funnel into the throat.
    float theta  = atan(rel.z, rel.x);
    float stream = 0.45 + 0.55 * cos(3.0 * theta
                                   - 5.0 * log(max(rad / diskInner, 1.0))
                                   + time * 0.25);
    float tor  = smoothstep(TORUS_FROM, 0.95, rN);
    float dens = mix(1.0, stream, tor * 0.85);
    float thin = mix(1.0, TORUS_DENS, tor);

    return vert * dilute * inner * turb * dens * thin;
}

// Soft radial edges for the THIN (black-hole) disk path.
float diskEdgeFade(float rad) {
    float rN = clamp((rad - diskInner) / (diskOuter - diskInner), 0.0, 1.0);
    return smoothstep(0.0, 0.06, rN) * smoothstep(1.0, 0.88, rN);
}

// ── Disk colour ────────────────────────────────────────────────────────────────

// Quiet black hole: cooler, burnt-orange throughout.
vec3 bhRamp(float t) {
    vec3 c = (t < 0.5) ? mix(vec3(0.22, 0.015, 0.0),  vec3(0.70, 0.10, 0.008), t * 2.0)
                       : mix(vec3(0.70, 0.10, 0.008), vec3(0.95, 0.42, 0.06), (t - 0.5) * 2.0);
    return mix(c, vec3(0.98, 0.80, 0.52), smoothstep(0.85, 1.0, t));
}

// Quasar: dusty torus → red → yellow-white → "Big Blue Bump" UV → white-hot ISCO.
vec3 quasarRamp(float t) {
    if (t < 0.25) return mix(vec3(0.20, 0.05, 0.02), vec3(0.85, 0.28, 0.04),  t / 0.25);
    if (t < 0.50) return mix(vec3(0.85, 0.28, 0.04), vec3(1.00, 0.72, 0.32), (t - 0.25) / 0.25);
    if (t < 0.75) return mix(vec3(1.00, 0.72, 0.32), vec3(0.55, 0.85, 1.45), (t - 0.50) / 0.25);
    return                 mix(vec3(0.55, 0.85, 1.45), vec3(1.05, 1.10, 1.30), (t - 0.75) / 0.25);
}

// Shakura–Sunyaev radial temperature profile, normalised to peak ≈ 1.
// Zero-torque inner boundary → T → 0 at the ISCO, peaks near 1.36·r_isco, then
// falls as r^(-3/4). This is what keeps the blue UV core COMPACT.
float diskTemp(float rad) {
    float u = diskInner / max(rad, diskInner);
    float f = max(1.0 - sqrt(u), 0.0);
    return clamp(T_PEAK * pow(u, 0.75) * pow(f, 0.25), 0.0, 1.0);
}

vec3 diskColor(vec3 pos) {
    vec3  rel   = pos - bhPos;
    float rad   = max(length(rel.xz), diskInner);
    float rNorm = clamp((rad - diskInner) / (diskOuter - diskInner), 0.0, 1.0);
    float theta = atan(rel.z, rel.x);
    float temp  = diskTemp(rad);

    vec3 col = mix(bhRamp(temp), quasarRamp(temp), quasarMode);

    // Spiral density wave (rigid pattern rotation — never winds up)
    float patternAngle = mod(0.25 * time, 6.28318530718);
    float spiral = cos(2.0 * (theta - patternAngle) - 3.0 * log(rNorm + 0.1));
    float arms   = smoothstep(-0.3, 0.9, spiral);
    float bands  = 0.70 + 0.30 * sin(pow(1.0 - rNorm, 0.6) * 28.0 - time * 0.3);
    float flow   = flowNoise(rNorm, theta, time);
    float glow   = 1.0 + 1.5 * pow(temp, 3.0);
    col *= (0.40 + 0.60 * flow) * (0.75 + 0.25 * arms) * bands * glow;

    // Relativistic Doppler beaming: the side rotating toward the camera is
    // brighter and bluer; the receding side dims and reddens.
    float beta     = clamp(sqrt(0.5 * rs / rad), 0.0, 0.9);
    float gamma_r  = 1.0 / sqrt(1.0 - beta * beta);
    vec3  tangent  = normalize(vec3(-rel.z, 0.0, rel.x));
    vec3  toCamera = normalize(camPos - pos);
    float cosTheta = dot(tangent, toCamera);
    float D = clamp(1.0 / (gamma_r * (1.0 - beta * cosTheta)), 0.1, 6.0);
    col   *= pow(D, 3.5);
    col.r *= pow(D, -0.5);
    col.b *= pow(D,  0.5);

    // The dusty torus radiates in the INFRARED — essentially invisible optically.
    // It should obscure and silhouette, not glow. This is what stops the outer
    // region reading as a solid orange body.
    col *= mix(1.0, TORUS_EMIS, smoothstep(TORUS_FROM, 0.95, rNorm));

    return col * diskBoost;
}

// ── Jets ───────────────────────────────────────────────────────────────────────
// Blandford–Payne: plasma flung centrifugally off the inner disk along magnetic
// field lines, collimated by the field's hoop stress. Wide annular base at the
// ISCO → pinches to a waist → DIVERGES conically once the field stops winning.

float jetDensity(vec3 pos) {
    vec3  rel = pos - bhPos;
    float h   = abs(rel.y);
    float rho = length(rel.xz);

    float jetLength = diskOuter * 1.6;
    if (h < rs * 0.4 || h > jetLength) return 0.0;            // cull 1: height

    // Waist is deliberately ~1·rs wide: any thinner and the march step is larger
    // than the beam, which aliases the bright core into a string of beads.
    float collim  = smoothstep(0.0, diskInner * 5.0, h);
    float waist   = mix(diskInner * 0.80, rs * 0.95, collim);
    float diverge = pow(h / (diskInner * 8.0), 1.25) * rs * 0.8;
    float coneR   = waist + diverge;
    if (rho > coneR) return 0.0;                              // cull 2: radial

    // Helical twist from the wound-up field, flowing outward with time.
    float ang  = atan(rel.z, rel.x) + h * (0.9 / diskInner) - time * 0.5;
    vec2  xs   = vec2(cos(ang), sin(ang)) * rho;
    float flow = h * (1.2 / rs) - time * 2.0;
    float turb = fbm3(vec3(xs * (1.0 / rs), flow));

    float core   = smoothstep(coneR * 0.70, 0.0, rho);
    float sheath = smoothstep(coneR, coneR * 0.25, rho);
    float radial = core + sheath * turb * 1.1;

    // Travelling knots: brightness ripples propelled outward. Shallow on purpose
    // — deep modulation chops the beam into gaps; this keeps it CONTINUOUS with
    // motion running through it. Long wavelength = propulsion, short = beads.
    float knot = 0.84 + 0.16 * sin(h * (6.2831 / (diskInner * 5.0)) - time * 3.0);
    knot *= 0.80 + 0.20 * fbm3(vec3(rho * (3.0 / rs), h * (0.5 / rs) - time * 2.5, 0.0));

    float axial = smoothstep(jetLength, jetLength * 0.10, h)
                * smoothstep(rs * 0.4, rs * 3.0, h);
    return radial * axial * knot;
}

// ── Sky ────────────────────────────────────────────────────────────────────────

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

vec3 skyColor(vec3 dir) {
    return starColor(dir) + nebulaColor(dir) * nebulaStrength;
}

// ── Main ───────────────────────────────────────────────────────────────────────

void main() {
    vec2  uv   = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    uv.x      *= resolution.x / resolution.y;
    float fovT = tan(fovY * 0.5);
    vec3  v    = normalize(camForward + uv.x * fovT * camRight + uv.y * fovT * camUp);
    vec3  p    = camPos;

    // Hole behind the camera → no bending, sample the sky directly.
    float tCA = dot(bhPos - camPos, v);
    if (tCA <= 0.0) { finalColor = vec4(skyColor(v), 1.0); return; }

    vec3  L  = cross(p - bhPos, v);
    float h2 = dot(L, L);                       // conserved angular momentum²

    // HALF_M > diskOuter, so the jump always lands outside the disk — nothing
    // can be skipped over, and no pre-jump crossing check is needed.
    float HALF_M  = diskOuter * 1.25;
    float escapeR = diskOuter * 2.4;
    p = camPos + v * max(0.0, tCA - HALF_M);

    vec3  col      = vec3(0.0);
    float transmit = 1.0;
    bool  thick    = (quasarMode > 0.5);

    for (int i = 0; i < STEPS; i++) {
        vec3  rel = p - bhPos;
        float r2  = dot(rel, rel);

        if (r2 < 2.25 * rs * rs && dot(rel, v) < 0.0) { transmit = 0.0; break; }
        if (r2 > escapeR * escapeR && dot(rel, v) > 0.0) break;

        float r   = sqrt(r2);
        float rad = length(rel.xz);
        float dl  = 0.05 * r;

        // Only the volumetric path needs fine steps near the slab; the thin
        // path interpolates its plane crossing exactly and doesn't care.
        if (thick && length(rel) < diskOuter * 1.1) {
            float H   = diskHalfHeight(clamp(rad, diskInner, diskOuter));
            float gap = abs(rel.y) - H;
            dl = min(dl, (gap > 0.0) ? max(gap * 0.6, 0.30 * H) : 0.30 * H);
        }
        // Refine along the jet funnel — the beam is ~1·rs wide, so a coarse step
        // walks straight through it and the core aliases into dots.
        if (jetStrength > 0.0 && rad < diskInner * 2.0)
            dl = min(dl, rs * 0.5);
        dl = clamp(dl, 0.4, diskOuter * 0.025);

        v += (-1.5 * rs * h2 * rel / pow(r2, 2.5)) * dl;
        vec3 pPrev = p;
        p += v * dl;

        if (thick) {
            // QUASAR — volumetric emission + absorption (Beer–Lambert), so the
            // near side occludes the far side and it reads as a solid body.
            float dd = diskDensity(p);
            if (dd > 0.0) {
                float a = 1.0 - exp(-dd * dl * (DISK_KAPPA / rs));
                col      += transmit * diskColor(p) * a;
                transmit *= (1.0 - a);
            }
        } else {
            // BLACK HOLE — exact thin-plane crossing, as before.
            float yPrev = pPrev.y - bhPos.y;
            float yNext = p.y     - bhPos.y;
            if (yPrev * yNext < 0.0) {
                vec3  hit = mix(pPrev, p, yPrev / (yPrev - yNext));
                float hr  = length(hit.xz - bhPos.xz);
                if (hr > diskInner && hr < diskOuter) {
                    float a = diskEdgeFade(hr);
                    col      += transmit * diskColor(hit) * a * a;
                    transmit *= (1.0 - a);
                }
            }
        }

        float jd = jetDensity(p) * jetStrength;
        if (jd > 0.0) {
            float up = smoothstep(diskOuter * 1.6, rs, abs(p.y - bhPos.y));
            vec3  jc = mix(vec3(0.60, 0.55, 1.00), vec3(0.88, 0.95, 1.0), up);
            col += transmit * jc * jd * jd * dl * (JET_GAIN / rs);
        }

        if (transmit < 0.01) break;
    }

    col += transmit * skyColor(normalize(v));
    finalColor = vec4(col, 1.0);
}