#version 330
// ─────────────────────────────────────────────────────────────────────────────
// volume.fs — lensed, beamed rendering of the SPH particle field.
//
// This is the physics-sim counterpart to lens.fs. It fires the SAME backward
// Schwarzschild geodesic per pixel — but instead of asking a procedural
// diskDensity() "is there matter here?", it samples three 3D textures built
// from the actual simulated particles this frame:
//
//   uDensity  (sampler3D, R)    — mass density splatted from the SPH kernels
//   uVelTemp  (sampler3D, RGBA) — RGB = momentum-weighted velocity, A = temperature
//   uOccupy   (sampler3D, R)    — coarse 1/0 occupancy: is any gas near this cell?
//
// The occupancy texture is the whole optimisation: where it reads empty, the ray
// FAST-FORWARDS with a big step (skipping only SAMPLING, never BENDING), so ~95%
// of the volume — pure vacuum — costs almost nothing. Where it reads occupied,
// the ray drops to fine steps and does real emission/absorption.
//
// Because sampling happens along the bent geodesic, the far side of the disk
// lenses over the top for free — same reason the cinema disk does. And because
// uVelTemp carries the REAL local flow, Doppler beaming uses the actual velocity
// dotted with the actual bent ray direction: more correct than cinema's assumed
// circular orbit.
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
uniform float rs;              // Schwarzschild radius, render units
uniform float cRender;        // speed of light, render units/sec (for beaming)

// The particle field. gridMin/gridSize map WORLD space → texture [0,1]³.
uniform sampler3D uDensity;
uniform sampler3D uVelTemp;
uniform sampler3D uOccupy;
uniform vec3      gridMin;     // world-space corner of the grid
uniform float     gridSize;    // world-space edge length of the (cubic) grid
uniform float     voxelN;      // voxels per axis — must match VoxelField::N

uniform float densGain;        // absorption / brightness master
uniform float emitGain;        // emission master
uniform float nebulaStrength;

const int   STEPS   = 200;
const float BIG_STEP = 0.0;    // set from gridSize at runtime? no — computed below

// ── world → grid texture coordinate ───────────────────────────────────────────
vec3 toGrid(vec3 world) { return (world - gridMin) / gridSize; }

bool inGrid(vec3 uvw) {
    return all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)));
}

// ── cheap starfield backdrop (matches the physics-mode sky) ───────────────────
float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p){
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm(vec2 p){ float s=0.0,a=0.5; for(int i=0;i<5;i++){s+=a*noise(p);p*=2.0;a*=0.5;} return s; }
vec3 skyColor(vec3 dir){
    float az=atan(dir.z,dir.x), el=asin(clamp(dir.y,-1.0,1.0));
    vec2 g=vec2(az,el)*40.0; vec2 c=floor(g), f=fract(g);
    float star=step(0.985,hash(c))*smoothstep(0.25,0.0,length(f-0.5));
    float tw=0.6+0.4*sin(time*2.0+hash(c)*6.2831);
    vec2 sky=vec2(az,el);
    float d=fbm(sky*2.5);
    float cloud=smoothstep(0.5,0.92,d)*(0.4+0.6*fbm(sky*6.0+3.7));
    vec3 neb=mix(vec3(0.09,0.12,0.30),vec3(0.40,0.18,0.48),smoothstep(0.4,0.7,d))*cloud;
    return star*tw*vec3(0.8,0.9,1.0) + neb*nebulaStrength;
}

// ── temperature → colour (blackbody-ish ramp, matches the sim's particleColor) ─
vec3 tempColor(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.5) return mix(vec3(0.95,0.30,0.06), vec3(1.0,0.78,0.28), t*2.0);
    return               mix(vec3(1.0,0.78,0.28), vec3(0.75,0.88,1.0), (t-0.5)*2.0);
}

void main() {
    vec2  uv   = (gl_FragCoord.xy / resolution) * 2.0 - 1.0;
    uv.x      *= resolution.x / resolution.y;
    float fovT = tan(fovY * 0.5);
    vec3  v    = normalize(camForward + uv.x*fovT*camRight + uv.y*fovT*camUp);
    vec3  p    = camPos;

    float tCA = dot(bhPos - camPos, v);
    if (tCA <= 0.0) { finalColor = vec4(skyColor(v), 1.0); return; }

    // Conserved angular momentum² — identical to lens.fs.
    vec3  L  = cross(p - bhPos, v);
    float h2 = dot(L, L);

    // Jump to just outside the grid so no steps are wasted getting there.
    float reach = gridSize * 0.9;
    p = camPos + v * max(0.0, tCA - reach);

    vec3  col      = vec3(0.0);
    float transmit = 1.0;

    // Step sizes in world units. BIG skips vacuum; FINE resolves the gas. FINE
    // must be < one voxel so a thin sheet is never stepped over.
    float voxel   = gridSize / voxelN;
    float fineDL  = voxel * 0.5;
    float bigDL   = voxel * 2.0;

    for (int i = 0; i < STEPS; i++) {
        vec3  rel = p - bhPos;
        float r2  = dot(rel, rel);
        float r   = sqrt(r2);

        // Captured at the photon sphere (shadow), or escaped outward → done.
        if (r2 < 2.25 * rs * rs && dot(rel, v) < 0.0) { transmit = 0.0; break; }
        if (r2 > (reach*1.4)*(reach*1.4) && dot(rel, v) > 0.0) break;

        // Decide the step size FIRST (occupancy-based fast-forward), then bend
        // by that same distance. Above/below the disk plane the ray is almost
        // always in vacuum, so it takes bigDL hops nearly all the way to the
        // photon sphere — bending had been scaled by a fixed fineDL regardless,
        // under-applying curvature by 4x on exactly those rays, so they never
        // looped far enough to pick up the backside disk and the ring never closed.
        vec3  uvw = toGrid(p);
        float dl  = bigDL;
        bool  occupied = false;
        if (inGrid(uvw)) {
            // Occupancy: coarse map, conservative (dilated on the CPU). Empty →
            // fast-forward; occupied → fine march + sample.
            float occ = texture(uOccupy, uvw).r;
            if (occ > 0.5) { dl = fineDL; occupied = true; }
        }
        // The deflection term blows up as 1/r^5 near the photon sphere. A bigDL
        // step taken that close (still common in vacuum, off the disk plane)
        // would either wildly under-bend (if scaled by a fixed small dl) or
        // overshoot/destabilise (if simply scaled by the large dl). Clamping the
        // step to a fraction of the distance-to-hole keeps the bend-per-step
        // accurate AND stable right where the photon ring actually forms.
        dl = min(dl, max(fineDL, r * 0.12));

        // ALWAYS bend the ray — the geodesic is integrated whether or not we
        // sample here. Skipping bending would break lensing. Scaled by the
        // ACTUAL step just chosen, so vacuum fast-forward hops bend correctly
        // instead of under-bending.
        v += (-1.5 * rs * h2 * rel / pow(r2, 2.5)) * dl;

        if (occupied) {
                float dens = texture(uDensity, uvw).r * densGain;
                if (dens > 0.001) {
                    vec4  vt   = texture(uVelTemp, uvw);
                    vec3  flow = vt.xyz;               // momentum-weighted velocity
                    float temp = vt.w;

                    // Relativistic Doppler beaming from the REAL local flow, dotted
                    // with the REAL bent ray direction at this point.
                    float speed = length(flow);
                    float D = 1.0;
                    if (speed > 1e-3) {
                        float beta = min(speed / cRender, 0.95);
                        float gam  = 1.0 / sqrt(1.0 - beta*beta);
                        // `v` is the ray marching CAMERA→scene — the opposite of the
                        // photon's actual travel direction (scene→camera). Beaming
                        // needs the angle to the direction light actually travels
                        // toward the observer, i.e. -v, or approaching/receding flips.
                        float cosT = -dot(flow / speed, normalize(v));
                        D = clamp(1.0 / (gam * (1.0 - beta*cosT)), 0.25, 4.0);
                    }

                    // tempColor() alone only shifts HUE with temperature; without an
                    // intensity term coupled to temp too, cold and hot gas emit at the
                    // same brightness and the disk reads as one flat wash. Matching the
                    // point-render's (0.22 + 1.15*heat) keeps cold gas dim and dark red,
                    // hot gas blazing, so the ramp actually shows on screen.
                    vec3  emit = tempColor(temp) * emitGain * (0.22 + 1.15 * temp);
                    emit *= pow(D, 3.0);                // relativistic flux boost
                    emit.b *= pow(D,  0.4);             // approaching side bluer
                    emit.r *= pow(D, -0.3);             // receding side redder

                    // Beer–Lambert: emit, then attenuate what's behind.
                    float a = 1.0 - exp(-dens * dl * 0.01);
                    col      += transmit * emit * a;
                    transmit *= (1.0 - a);
                }
        }

        p += v * dl;
        if (transmit < 0.01) break;
    }

    col += transmit * skyColor(normalize(v));
    finalColor = vec4(col, 1.0);
}