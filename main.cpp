// ─────────────────────────────────────────────────────────────────────────────
// Black Hole Simulator — Cinema renderer
//
// Real-time gravitational lensing (lens.fs) finished with HDR bloom. A cosmic
// start menu renders a live, orbiting black hole behind Liquid-Glass buttons
// that refract the backdrop through them, plus a white-star cursor trail. Two
// presets share the engine — a quiet Black Hole and an active Quasar.
//
// Render pipeline (every frame — behind the menu AND in a mode):
//   Pass 1  lens.fs       ray-march the metric              → hdrBuf   (full res)
//   Pass 2  bright.fs      keep pixels above a luma cutoff    → bloomA   (half res)
//   Pass 3  blur.fs  ×2    separable Gaussian, ping-pong      bloomA ↔ bloomB
//   Pass 4  composite.fs   add bloom, ACES tone-map           → screen
//   Menu    glass.fs pills refract hdrBuf; UI + trail on top
//
// Controls (simulation): Space+drag orbit · scroll/↑↓ zoom · ←→ rotate ·
//                        Q toggle Black Hole/Quasar · H return to menu
//
// Optional font: assets/Orbitron-Bold.ttf (free, Google Fonts). Falls back to
// the built-in font if absent.
// ─────────────────────────────────────────────────────────────────────────────

#include <raylib.h>
#include <raymath.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <rlgl.h>
#include "voxel_field.h"
// An SPH particle is NOT a ball of matter — it is a moving sample point of a
// continuous fluid field. Its properties are interpolated from its neighbours
// through a smoothing kernel, which is why this reads as liquid rather than sand.
struct Particle {
    Vector3 pos, vel;
    Vector3 accSPH;    // pressure + viscosity acceleration (recomputed per frame)
    float   rho;       // density,  interpolated from neighbours
    float   P;         // pressure, from the equation of state
    float   u;         // specific internal energy → temperature → colour
    float   dudt;      // energy rate: compression + shock heating − radiation
    float   heat;      // u mapped to 0..1 for colour
    float   bright;    // per-particle luminosity, varied at spawn
    bool    alive;

    int     clumpId     = -1;    // -1 = not part of a clump burst (stream mode)
    bool    clumpBroken = false; // once tidal shear beats cohesion, stays true forever
};

// Physics-sim tuning. File scope so respawn() can see them.
// SPH costs ~100x more per particle than ballistic gravity (every particle must
// find its neighbours twice per frame), so the budget drops accordingly. This is
// the single most multithreadable loop in the project — prime M5 Pro territory.
const int   MAX_PARTICLES   = 3000;   // fixed pool — never grows, never allocates
const int   SPAWN_PER_FRAME = 120;    // ramp-in cap; steady state self-balances
const float EMIT_SPREAD     = 14.0f;  // nozzle radius, render units
const float EMIT_SPEED      = 0.95f;  // launch speed, × circular velocity at the nozzle
const float ARROW_LEN       = 78.0f;  // aim arrow length, render units
const float CLUMP_INTERVAL  = 1.40f;  // seconds between blobs in clump mode
const int   CLUMP_SIZE      = 180;    // particles per blob
// A clump's cohesion is a stand-in for self-gravity/pressure holding a real body
// together — NOT atomic or nuclear force, which is irrelevant at this scale. It
// competes against the hole's TIDAL SHEAR (Roche-limit disruption): while shear
// stays below CLUMP_BIND_STRENGTH the blob holds together; once it wins, cohesion
// permanently switches off for that clump and it shreds via ordinary Keplerian
// shear from then on. Both are "look dials" — tune by watching CLUMPS mode (C).
const float CLUMP_BIND_STRENGTH = 1500.0f; // tidal shear (render accel) a clump can resist
const float CLUMP_COHESION_K    = 8.0f;    // spring accel per unit distance from clump COM
const float VISCOSITY       = 0.035f; // Shakura–Sunyaev α-viscosity stand-in. Real
                                      // disks accrete via MRI turbulence, which SPH
                                      // cannot resolve; gravity alone conserves
                                      // angular momentum, so without this the disk
                                      // orbits forever and never feeds.

// ── SPH fluid parameters ──────────────────────────────────────────────────────
const float SPH_H      = 38.0f;    // smoothing length; support radius is 2h.
                                   // Sized for ~53 neighbours — the SPH sweet spot.
                                   // 42 gave 71: same physics, 30% more work.
const float SPH_MASS   = 1.0f;     // mass per sample point
const float SPH_GAMMA  = 1.6667f;  // ideal monatomic gas (hydrogen), 5/3
const float SPH_ALPHA  = 1.0f;     // Monaghan artificial viscosity — bulk
const float SPH_BETA   = 2.0f;     // Monaghan artificial viscosity — quadratic
const float SPH_U0     = 26000.0f; // internal energy at birth. Sets the SOUND SPEED
                                   // via c_s=√(γ(γ−1)u), and a thin disk obeys
                                   // H/R = c_s/v_orbit. At 26000, c_s≈167 against a
                                   // 1882 orbit → Mach 11, H/R≈0.09: a real thin disk.
                                   // At 260 it was Mach 110 — pressureless dust.
const float SPH_UMIN   = 2500.0f;  // floor, so pressure never collapses to zero
const float SPH_UHOT   = 150000.f; // u that maps to white-hot in the colour ramp
const float SPH_TCOOL  = 0.06f;    // radiative cooling time (β-cooling). Without
                                   // this the disk heats forever and puffs up.
const float RHO_MIN    = 1e-6f;    // guard: isolated particles must not divide by 0

// Uniform grid for neighbour finding. Naive SPH is O(N²) — 3000 particles would
// be 9M pair tests per pass. The grid makes it O(N·k) with k≈300 candidates.
const float GRID_HALF  = 2100.0f;          // domain half-extent, render units
const float GRID_CELL  = 2.0f * SPH_H;     // MUST equal the support radius, so a
                                           // 3×3×3 cell search provably finds every
                                           // neighbour within 2h and no more.
const int   GRID_N     = (int)(2.0f * GRID_HALF / GRID_CELL);   // cells per axis
const int   GRID_CELLS = GRID_N * GRID_N * GRID_N;
const float PART_SIZE       = 76.0f;  // = 2·SPH_H, the kernel SUPPORT DIAMETER.
                                      // (DrawBillboard's size is full width, so the
                                      // drawn radius is h — exactly the footprint the
                                      // SPH interpolation already assumes.) Particle
                                      // spacing is ~33u, so ~13 kernels overlap per
                                      // pixel and the swarm RECONSTRUCTS the density
                                      // field on screen instead of reading as sand.
const float PART_GLOW       = 0.12f;  // per-kernel contribution. Deliberately LOW —
                                      // the SUM over ~13 overlapping kernels is what
                                      // sets surface brightness. THE brightness dial.

struct ModeParams {
    float diskInnerMul, diskOuterMul, diskBoost, quasarMode, jetStrength, nebulaStrength;
};

enum Screen { SCREEN_MENU, SCREEN_BLACKHOLE, SCREEN_QUASAR, SCREEN_PHYSICS };

// ── White-star cursor trail ────────────────────────────────────────────────────
struct TrailParticle { Vector2 pos, vel; float life, maxLife, size; };

static void updateDrawTrail(TrailParticle* ps, int n, Vector2 mouse, bool spawn, float dt) {
    if (spawn) {
        int made = 0;
        for (int i = 0; i < n && made < 2; i++) {
            if (ps[i].life > 0.f) continue;
            float ang = GetRandomValue(0, 628) / 100.f;
            float spd = (float)GetRandomValue(12, 55);
            ps[i].pos     = { mouse.x + GetRandomValue(-3, 3), mouse.y + GetRandomValue(-3, 3) };
            ps[i].vel     = { cosf(ang) * spd, sinf(ang) * spd };
            ps[i].maxLife = 0.5f + GetRandomValue(0, 45) / 100.f;
            ps[i].life    = ps[i].maxLife;
            ps[i].size    = 1.0f + GetRandomValue(0, 14) / 10.f;
            made++;
        }
    }
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < n; i++) {
        if (ps[i].life <= 0.f) continue;
        // Gentle pull toward the cursor → curved star trail.
        Vector2 toM = { mouse.x - ps[i].pos.x, mouse.y - ps[i].pos.y };
        float   d   = sqrtf(toM.x*toM.x + toM.y*toM.y) + 24.f;
        float   g   = 1200.f / d;
        ps[i].vel.x += toM.x / d * g * dt;
        ps[i].vel.y += toM.y / d * g * dt;
        ps[i].vel.x *= 0.95f; ps[i].vel.y *= 0.95f;
        ps[i].pos.x += ps[i].vel.x * dt;
        ps[i].pos.y += ps[i].vel.y * dt;
        ps[i].life  -= dt;

        float a  = ps[i].life / ps[i].maxLife;
        float sz = ps[i].size * (0.4f + 0.6f * a);
        DrawCircleV(ps[i].pos, sz * 2.4f, Fade(WHITE, 0.09f * a));
        DrawCircleV(ps[i].pos, sz * 1.4f, Fade(WHITE, 0.20f * a));
        DrawCircleV(ps[i].pos, sz,        Fade(WHITE, 0.75f * a));
    }
    EndBlendMode();
}

// ── Glass pill ──────────────────────────────────────────────────────────────────
struct GlassCtx {
    Shader sh; Texture2D scene;
    int reso, center, half, radius, hover, appear, accent, scale, sceneLoc;
};
// ── SPH kernel ────────────────────────────────────────────────────────────────
// Cubic spline (M4). Every fluid quantity at a point is the kernel-weighted sum
// of its neighbours' — that weighting IS the "smoothed" in SPH.

static inline float kernelW(float r, float h) {
    float q = r / h;
    float k = 1.0f / (PI * h * h * h);
    if (q < 1.0f) return k * (1.0f - 1.5f * q * q + 0.75f * q * q * q);
    if (q < 2.0f) { float a = 2.0f - q; return k * 0.25f * a * a * a; }
    return 0.0f;
}

// dW/dr — scalar; multiply by the unit vector r̂ to get ∇W. Always ≤ 0: the
// kernel falls off outward, which is what makes pressure push neighbours apart.
static inline float kernelDW(float r, float h) {
    float q = r / h;
    float k = 1.0f / (PI * h * h * h * h);
    if (q < 1.0f) return k * (-3.0f * q + 2.25f * q * q);
    if (q < 2.0f) { float a = 2.0f - q; return k * (-0.75f * a * a); }
    return 0.0f;
}

// ── Uniform neighbour grid ────────────────────────────────────────────────────
struct SPHGrid {
    std::vector<int> start;   // GRID_CELLS+1 — prefix-summed bucket offsets
    std::vector<int> cursor;  // scratch for the scatter pass
    std::vector<int> items;   // particle indices, sorted by cell
    std::vector<int> cellOf;  // per-particle cell, or -1 if outside the domain
};

static inline int cellIndexOf(Vector3 p) {
    int x = (int)((p.x + GRID_HALF) / GRID_CELL);
    int y = (int)((p.y + GRID_HALF) / GRID_CELL);
    int z = (int)((p.z + GRID_HALF) / GRID_CELL);
    if (x < 0 || y < 0 || z < 0 || x >= GRID_N || y >= GRID_N || z >= GRID_N) return -1;
    return (z * GRID_N + y) * GRID_N + x;
}

// Counting sort into cells: O(N) and cache-friendly. No hashing, so no collisions
// and no risk of visiting the same particle twice from two different cells.
static void gridBuild(SPHGrid& g, const std::vector<Particle>& ps) {
    std::fill(g.start.begin(), g.start.end(), 0);
    for (size_t i = 0; i < ps.size(); i++) {
        int c = ps[i].alive ? cellIndexOf(ps[i].pos) : -1;
        g.cellOf[i] = c;
        if (c >= 0) g.start[c + 1]++;
    }
    for (int c = 0; c < GRID_CELLS; c++) g.start[c + 1] += g.start[c];
    g.cursor = g.start;
    for (size_t i = 0; i < ps.size(); i++)
        if (g.cellOf[i] >= 0) g.items[g.cursor[g.cellOf[i]]++] = (int)i;
}

// ── SPH pass 1: density and pressure ──────────────────────────────────────────
static void sphDensity(std::vector<Particle>& ps, const SPHGrid& g) {
    const float H2 = 2.0f * SPH_H;
    for (size_t i = 0; i < ps.size(); i++) {
        Particle& pi = ps[i];
        if (!pi.alive) continue;
        if (g.cellOf[i] < 0) { pi.rho = RHO_MIN; pi.P = 0.f; continue; }

        int cx = (int)((pi.pos.x + GRID_HALF) / GRID_CELL);
        int cy = (int)((pi.pos.y + GRID_HALF) / GRID_CELL);
        int cz = (int)((pi.pos.z + GRID_HALF) / GRID_CELL);

        float rho = 0.f;
        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = cx+dx, ny = cy+dy, nz = cz+dz;
            if (nx<0||ny<0||nz<0||nx>=GRID_N||ny>=GRID_N||nz>=GRID_N) continue;
            int c = (nz*GRID_N + ny)*GRID_N + nx;
            for (int k = g.start[c]; k < g.start[c+1]; k++) {
                const Particle& pj = ps[g.items[k]];
                float r = Vector3Distance(pi.pos, pj.pos);
                if (r < H2) rho += SPH_MASS * kernelW(r, SPH_H);
            }
        }
        pi.rho = fmaxf(rho, RHO_MIN);
        // Ideal gas: P = (γ−1)ρu. Pressure rises with BOTH compression and heat,
        // which is what lets the disk push back and settle to a real thickness.
        pi.P = (SPH_GAMMA - 1.f) * pi.rho * pi.u;
    }
}

// ── SPH pass 2: pressure force, shock viscosity, heating ──────────────────────
static void sphForces(std::vector<Particle>& ps, const SPHGrid& g) {
    const float H2 = 2.0f * SPH_H;
    for (size_t i = 0; i < ps.size(); i++) {
        Particle& pi = ps[i];
        if (!pi.alive) continue;
        pi.accSPH = { 0, 0, 0 };
        pi.dudt   = 0.f;
        if (g.cellOf[i] < 0) continue;

        int cx = (int)((pi.pos.x + GRID_HALF) / GRID_CELL);
        int cy = (int)((pi.pos.y + GRID_HALF) / GRID_CELL);
        int cz = (int)((pi.pos.z + GRID_HALF) / GRID_CELL);

        float ci = sqrtf(SPH_GAMMA * (SPH_GAMMA - 1.f) * pi.u);   // sound speed
        float Pi_over = pi.P / (pi.rho * pi.rho);

        Vector3 acc = { 0, 0, 0 };
        float   dudt = 0.f;

        for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = cx+dx, ny = cy+dy, nz = cz+dz;
            if (nx<0||ny<0||nz<0||nx>=GRID_N||ny>=GRID_N||nz>=GRID_N) continue;
            int c = (nz*GRID_N + ny)*GRID_N + nx;
            for (int k = g.start[c]; k < g.start[c+1]; k++) {
                int j = g.items[k];
                if (j == (int)i) continue;
                const Particle& pj = ps[j];

                Vector3 rij = Vector3Subtract(pi.pos, pj.pos);
                float   r   = Vector3Length(rij);
                if (r >= H2 || r < 1e-4f) continue;

                Vector3 vij  = Vector3Subtract(pi.vel, pj.vel);
                float   dw   = kernelDW(r, SPH_H);
                Vector3 gradW = Vector3Scale(rij, dw / r);       // r̂ · dW/dr

                // Monaghan artificial viscosity — fires only on APPROACH
                // (v·r < 0), i.e. inside a shock. It converts kinetic energy of
                // compression into heat, which is what makes gas behave as gas.
                float visc = 0.f;
                float vr   = Vector3DotProduct(vij, rij);
                if (vr < 0.f) {
                    float cj    = sqrtf(SPH_GAMMA * (SPH_GAMMA - 1.f) * pj.u);
                    float mu    = SPH_H * vr / (r * r + 0.01f * SPH_H * SPH_H);
                    float cbar  = 0.5f * (ci + cj);
                    float rbar  = 0.5f * (pi.rho + pj.rho);
                    visc = (-SPH_ALPHA * cbar * mu + SPH_BETA * mu * mu) / rbar;
                    visc = fminf(visc, 20.f * cbar);  // cap: viscous pressure ≤ 20× sound speed
                }

                float term = Pi_over + pj.P / (pj.rho * pj.rho) + visc;
                acc  = Vector3Subtract(acc, Vector3Scale(gradW, SPH_MASS * term));
                // Energy equation: work done against pressure + shock dissipation.
                dudt += 0.5f * SPH_MASS * term * Vector3DotProduct(vij, gradW);
            }
        }
        pi.accSPH = acc;
        pi.dudt   = dudt;
    }
}

static Vector3 pwAccel(Vector3 p, float rsR, float GM) {
    Vector3 d = Vector3Negate(p);                     // hole sits at the origin
    float   r = Vector3Length(d);
    if (r < 1e-3f) return { 0, 0, 0 };
    float   denom = fmaxf(r - rsR, rsR * 0.15f);      // guard the singularity
    return Vector3Scale(Vector3Normalize(d), GM / (denom * denom));
}
// Circular-orbit speed for the PW potential: v = √(GM·r) / (r − rs)
static float pwCircularV(float r, float rsR, float GM) {
    return sqrtf(GM * r) / (r - rsR);
}

// Tidal shear at radius r: the DIFFERENCE in the hole's pull across a body of
// characteristic size `span` — d(pwAccel)/dr · span. This is the actual physics
// of tidal disruption (the Roche limit): a body survives while its own binding
// beats this gradient, and gets torn apart the moment the gradient wins.
static float tidalShear(Vector3 pos, float rsR, float GM, float span) {
    float r     = Vector3Length(pos);
    float denom = fmaxf(r - rsR, rsR * 0.15f);        // same guard as pwAccel
    return 2.f * GM * span / (denom * denom * denom);
}

// Clump cohesion: a cheap stand-in for a body's self-gravity/pressure, pulling
// every member of a clump toward that clump's own live centre of mass. While the
// hole's tidal shear at the COM stays below CLUMP_BIND_STRENGTH the blob holds
// together (adds a spring-like restoring accel into accSPH); the moment shear
// wins, cohesion permanently switches off for every member of that clump — it
// then shreds via the ordinary Keplerian shear already in stepParticles.
static void applyClumpCohesion(std::vector<Particle>& ps, float rsR, float GM) {
    struct Accum { Vector3 comSum{0,0,0}; int count = 0; bool brokenAlready = false; };
    std::unordered_map<int, Accum> clumps;

    for (auto& p : ps) {
        if (!p.alive || p.clumpId < 0) continue;
        Accum& c = clumps[p.clumpId];
        c.comSum = Vector3Add(c.comSum, p.pos);
        c.count++;
        if (p.clumpBroken) c.brokenAlready = true;
    }
    if (clumps.empty()) return;

    for (auto& [id, c] : clumps) {
        Vector3 com = Vector3Scale(c.comSum, 1.f / (float)c.count);
        float   shear = tidalShear(com, rsR, GM, EMIT_SPREAD);
        c.brokenAlready = c.brokenAlready || (shear > CLUMP_BIND_STRENGTH);
    }

    for (auto& p : ps) {
        if (!p.alive || p.clumpId < 0) continue;
        const Accum& c = clumps[p.clumpId];
        if (c.brokenAlready) { p.clumpBroken = true; continue; }
        Vector3 com   = Vector3Scale(c.comSum, 1.f / (float)c.count);
        Vector3 toCom = Vector3Subtract(com, p.pos);
        p.accSPH = Vector3Add(p.accSPH, Vector3Scale(toCom, CLUMP_COHESION_K));
    }
}
// Gravity substeps (it varies violently near the hole); the SPH acceleration is
// held fixed across them. That is an operator split: SPH forces change on the
// SOUND-crossing time, which is far longer than the orbital time near the ISCO,
// so resolving them 6× per frame would buy nothing for 6× the cost.
static void stepParticles(std::vector<Particle>& ps, float dt,
                          float rsR, float GM) {
    const int SUB = 6;
    float h = dt / SUB;
    for (int s = 0; s < SUB; s++) {
        for (auto& p : ps) {
            if (!p.alive) continue;

            Vector3 a = Vector3Add(pwAccel(p.pos, rsR, GM), p.accSPH);
            p.vel = Vector3Add(p.vel, Vector3Scale(a, h));   // velocity first

            // α-viscosity: MRI turbulence, unresolvable at this scale, standing in
            // for the angular-momentum transport that lets matter fall inward.
            // Friction doesn't destroy energy — it converts KE into heat. That
            // conversion IS the accretion disk's luminosity: v² peaks at the ISCO,
            // so the inner disk heats itself with no heat = f(r) anywhere in code.
            float ke0 = Vector3LengthSqr(p.vel);
            p.vel = Vector3Scale(p.vel, 1.f - VISCOSITY * h);
            p.u  += 0.5f * (ke0 - Vector3LengthSqr(p.vel));

            p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, h));

            // Heating from compression/shocks, minus β-cooling. The energy leaving
            // through that cooling term IS the light the disk radiates — which is
            // why temperature can drive colour honestly here.
            //
            // This is integrated EXACTLY (not explicit Euler) because Euler on a
            // decay term is only stable while h << SPH_TCOOL — push TCOOL down (to
            // compress the disk) or let the frame rate dip (bigger h) and Euler
            // overshoots: u oscillates/flips sign, pressure spikes, and the SPH
            // force blows particles apart instead of confining them. The closed-form
            // update below (exact for dudt held constant across the substep, same
            // assumption already used elsewhere) is stable for any h/TCOOL ratio.
            float uEq   = p.dudt * SPH_TCOOL;           // equilibrium: heating = cooling
            float decay = expf(-h / SPH_TCOOL);
            p.u = uEq + (p.u - uEq) * decay;
            p.u  = fmaxf(p.u, SPH_UMIN);
            p.u  = fminf(p.u, SPH_UHOT * 5.f);   // hard ceiling: prevents thermal runaway

            float r = Vector3Length(p.pos);
            if (r < rsR * 2.6f)      { p.alive = false; continue; }  // eaten
            if (r > GRID_HALF*0.95f) { p.alive = false; continue; }  // left the domain
            // Kill runaway particles — anything moving faster than ~2× ISCO escape velocity
            // is a sign of an unphysical SPH kick, not real orbital mechanics.
            if (Vector3LengthSqr(p.vel) > 2.25e8f) { p.alive = false; continue; }
        }
    }
    // Temperature → colour. sqrt() softens the ramp so the whole disk isn't a
    // two-tone step between "cold" and "blown out".
    for (auto& p : ps)
        if (p.alive) p.heat = fminf(1.f, sqrtf(p.u / SPH_UHOT));
}

// The prograde tangent at a point — the aim that produces a near-circular orbit,
// which viscosity then grinds into a disk. T snaps to this.
static Vector3 progradeAim(Vector3 pos) {
    Vector3 t = Vector3CrossProduct({ 0.f, 1.f, 0.f }, pos);
    if (Vector3Length(t) < 1e-3f) t = { 1.f, 0.f, 0.f };   // guard the poles
    return Vector3Normalize(t);
}

// Refill one dead slot from the nozzle. This is the whole emitter: particles die
// at the shadow, slots free, this refills them. Death rate IS spawn rate — the
// population self-regulates and the system runs forever.
static void respawn(Particle& p, Vector3 origin, Vector3 aimDir, float rsR, float GM,
                     int clumpId = -1) {
    // Spawn flat: keep y-spread to ~10% of the horizontal spread so particles
    // immediately form a thin sheet. Gravity will then confine them to the disk
    // plane; hot-start pressure would puff it back into a sphere.
    p.pos = Vector3Add(origin, {
        GetRandomValue(-100, 100) / 100.f * EMIT_SPREAD,
        GetRandomValue(-100, 100) / 100.f * EMIT_SPREAD * 0.10f,
        GetRandomValue(-100, 100) / 100.f * EMIT_SPREAD });

    // Speed is scaled to the LOCAL circular velocity, so the aim angle is the
    // only thing you're really choosing — the nozzle stays sane at any radius.
    float r  = fmaxf(Vector3Length(p.pos), rsR * 4.f);
    float vc = pwCircularV(r, rsR, GM);
    p.vel = Vector3Scale(aimDir, vc * EMIT_SPEED);

    // Tiny jitter — enough to break symmetry, not enough to scatter out of the disk.
    p.vel = Vector3Add(p.vel, {
        GetRandomValue(-100, 100) / 100.f * vc * 0.02f,
        GetRandomValue(-100, 100) / 100.f * vc * 0.005f,
        GetRandomValue(-100, 100) / 100.f * vc * 0.02f });

    // Cold start: particles begin below the sound speed of the existing disk so
    // pressure doesn't explode the clump before gravity has time to organise it.
    // Viscous dissipation heats them naturally as they fall inward.
    p.u      = SPH_UMIN * 4.f;
    p.rho    = RHO_MIN;
    p.P      = 0.f;
    p.dudt   = 0.f;
    p.accSPH = { 0, 0, 0 };
    p.heat   = 0.f;
    p.bright = 1.0f + GetRandomValue(0, 50) / 100.f;
    p.alive  = true;

    p.clumpId     = clumpId;
    p.clumpBroken = false;
}

static Color particleColor(const Particle& p, Vector3 camPos, float cR) {
    Vector3 c = (p.heat < 0.5f)
        ? Vector3Lerp({0.95f,0.30f,0.06f}, {1.0f,0.78f,0.28f}, p.heat * 2.f)
        : Vector3Lerp({1.0f,0.78f,0.28f},  {0.75f,0.88f,1.0f}, (p.heat-0.5f) * 2.f);
    float b = p.bright * PART_GLOW * (0.22f + 1.15f * p.heat);

    // Relativistic Doppler beaming — D = 1/(γ(1 − β·cosθ)).
    // Matter rushing toward the camera brightens (D>1) and blueshifts;
    // matter receding dims (D<1) and redshifts. Near the ISCO β≈0.66, so
    // the approaching side is ~15× brighter than the receding side. That
    // contrast is most of what makes this read as a black-hole accretion disk.
    Vector3 toCam = Vector3Subtract(camPos, p.pos);
    float   dl    = Vector3Length(toCam);
    float   v     = Vector3Length(p.vel);
    if (dl > 1e-3f && v > 1e-3f) {
        toCam      = Vector3Scale(toCam, 1.f / dl);
        float beta = fminf(v / cR, 0.95f);
        float gam  = 1.f / sqrtf(1.f - beta * beta);
        float cosT = Vector3DotProduct(Vector3Scale(p.vel, 1.f / v), toCam);
        float D    = fmaxf(0.2f, fminf(1.f / (gam * (1.f - beta * cosT)), 5.f));
        b   *= powf(D, 3.0f);    // flux ∝ D³
        c.z *= powf(D,  0.4f);   // approaching: bluer
        c.x *= powf(D, -0.3f);   // receding:    redder
    }

    return { (unsigned char)fminf(255.f, c.x * 255.f * b),
             (unsigned char)fminf(255.f, c.y * 255.f * b),
             (unsigned char)fminf(255.f, c.z * 255.f * b), 255 };
}

static void drawGlassPill(const GlassCtx& G, Rectangle r, Color accent,
                          float hover, float appear, float scale) {
    if (appear <= 0.001f) return;
    float reso[2] = { (float)GetRenderWidth(), (float)GetRenderHeight() };
    float c[2]    = { (r.x + r.width*0.5f) * scale, (r.y + r.height*0.5f) * scale };
    float h[2]    = { r.width*0.5f * scale, r.height*0.5f * scale };
    float rad     = r.height*0.5f * scale;
    float acc[3]  = { accent.r/255.f, accent.g/255.f, accent.b/255.f };

    BeginShaderMode(G.sh);
        SetShaderValue(G.sh, G.reso,   reso, SHADER_UNIFORM_VEC2);
        SetShaderValue(G.sh, G.center, c,    SHADER_UNIFORM_VEC2);
        SetShaderValue(G.sh, G.half,   h,    SHADER_UNIFORM_VEC2);
        SetShaderValue(G.sh, G.radius, &rad,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(G.sh, G.hover,  &hover,  SHADER_UNIFORM_FLOAT);
        SetShaderValue(G.sh, G.appear, &appear, SHADER_UNIFORM_FLOAT);
        SetShaderValue(G.sh, G.accent, acc,  SHADER_UNIFORM_VEC3);
        SetShaderValue(G.sh, G.scale,  &scale,  SHADER_UNIFORM_FLOAT);
        SetShaderValueTexture(G.sh, G.sceneLoc, G.scene);
        DrawRectangleRec({ r.x - 18, r.y - 18, r.width + 36, r.height + 36 }, WHITE);
    EndShaderMode();
}

// ── Menu ──────────────────────────────────────────────────────────────────────
static void drawMenuUI(Screen& screen, ModeParams& M, const ModeParams& bh,
                       const ModeParams& quasar, Font font, float menuAge,
                       const GlassCtx& G, float scale) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, Color{ 0, 0, 8, 100 });
    DrawRectangleGradientV(0, 0, sw, 130, Color{ 0, 0, 10, 165 }, Color{ 0, 0, 10, 0 });
    DrawRectangleGradientV(0, sh - 150, sw, 150, Color{ 0, 0, 10, 0 }, Color{ 0, 0, 10, 190 });

    float tA = fminf(1.f, menuAge / 0.5f);
    float ty = sh * 0.16f - (1.f - tA) * 18.f;
    const char* title = "BLACK HOLE SIMULATOR";
    float tfs = 40, tsp = 5;
    Vector2 tsz = MeasureTextEx(font, title, tfs, tsp);
    float tx = (sw - tsz.x) * 0.5f;
    DrawTextEx(font, title, { tx, ty }, tfs, tsp, Fade(Color{ 120, 150, 255, 255 }, 0.15f * tA));
    DrawTextEx(font, title, { tx, ty }, tfs, tsp, Fade(RAYWHITE, tA));

    const char* sub = "SELECT A MODE";
    float sfs = 15, ssp = 5;
    Vector2 ssz = MeasureTextEx(font, sub, sfs, ssp);
    DrawTextEx(font, sub, { (sw - ssz.x)*0.5f, ty + tsz.y + 16 }, sfs, ssp,
               Fade(Color{ 140, 150, 180, 255 }, tA));

    static float hov[3] = { 0, 0, 0 };
    float bw = 300, bht = 56, gap = 20;
    float bx = (sw - bw) * 0.5f, by = sh * 0.46f;
    Rectangle base[3] = {
        { bx, by,                bw, bht },
        { bx, by + (bht+gap),    bw, bht },
        { bx, by + 2*(bht+gap),  bw, bht },
    };
    const char* labels[3]  = { "BLACK HOLE", "QUASAR", "PHYSICS SIM" };
    Color       accents[3] = { { 255,140,60,255 }, { 90,150,255,255 }, { 80,220,150,255 } };
    bool        enabled[3] = { true, true, true };
    Screen      targets[3] = { SCREEN_BLACKHOLE, SCREEN_QUASAR, SCREEN_PHYSICS };
    float       dt = GetFrameTime();

    for (int i = 0; i < 3; i++) {
        float appear = fminf(1.f, fmaxf(0.f, (menuAge - 0.15f - i*0.09f) / 0.4f));
        if (appear <= 0.001f) continue;

        bool hovering = enabled[i] && CheckCollisionPointRec(GetMousePosition(), base[i]);
        hov[i] += ((hovering ? 1.f : 0.f) - hov[i]) * fminf(1.f, dt * 12.f);

        float yoff = (1.f - appear) * 16.f - hov[i] * 2.f;
        Rectangle r = { base[i].x, base[i].y + yoff, base[i].width, base[i].height };

        drawGlassPill(G, r, enabled[i] ? accents[i] : Color{ 90, 92, 105, 255 },
                      enabled[i] ? hov[i] : 0.f, appear, scale);

        float   lfs = 21, lsp = 3;
        Vector2 lsz = MeasureTextEx(font, labels[i], lfs, lsp);
        Vector2 lp  = { r.x + (r.width - lsz.x)*0.5f, r.y + (r.height - lsz.y)*0.5f - 1 };
        Color   lcol = enabled[i]
            ? Color{ (unsigned char)(210 + 45*hov[i]), (unsigned char)(214 + 41*hov[i]),
                     (unsigned char)(224 + 31*hov[i]), 255 }
            : Color{ 135, 135, 145, 255 };
        DrawTextEx(font, labels[i], { lp.x + 1, lp.y + 1 }, lfs, lsp, Fade(BLACK, 0.55f * appear));
        DrawTextEx(font, labels[i], lp, lfs, lsp, Fade(lcol, appear));

        if (!enabled[i]) {
            float   zfs = 11, zsp = 2;
            Vector2 zsz = MeasureTextEx(font, "SOON", zfs, zsp);
            DrawTextEx(font, "SOON", { r.x + r.width - zsz.x - 18, r.y + (r.height - zsz.y)*0.5f },
                       zfs, zsp, Fade(Color{ 135, 135, 145, 255 }, appear));
                       
        }
        

        if (appear > 0.9f && enabled[i] && hovering && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            M = (targets[i] == SCREEN_QUASAR) ? quasar : bh;
            screen = targets[i];
        }
    }

    DrawTextEx(font, "REAL-TIME GRAVITATIONAL LENSING", { 16, (float)sh - 30 }, 13, 3,
               Fade(Color{ 110, 115, 140, 255 }, tA));
}

// ── Entry point ───────────────────────────────────────────────────────────────
int main() {
    const int screenWidth  = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "Black Hole Simulator");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    // raylib's default 3D clip planes are near=0.01, far=1000 — sized for scenes
    // where 1 unit ≈ 1 metre. Our world is thousands of units across, so anything
    // beyond 1000 units FROM THE CAMERA was being clipped: that is the "horizon",
    // and it was also slicing the back off the shadow sphere.
    rlSetClipPlanes(1.0, 20000.0);
    bool fontLoaded = FileExists("assets/Orbitron-Bold.ttf");
    Font uiFont = fontLoaded ? LoadFontEx("assets/Orbitron-Bold.ttf", 96, 0, 0) : GetFontDefault();
    if (fontLoaded) SetTextureFilter(uiFont.texture, TEXTURE_FILTER_BILINEAR);

    
    const float C_RENDER  = 10000.0f;
    const float G_       = 6.67430e-11f;
    const float C_       = 299792458.0f;
    const float BH_MASS  = 8e30f;
    const float rs_SI    = 2.f * G_ * BH_MASS / (C_ * C_);
    const float RENDER_SCALE = (screenWidth * 0.045f) / rs_SI;
    const float rsRender = rs_SI * RENDER_SCALE;
    const float GM_RENDER = rsRender * C_RENDER * C_RENDER * 0.5f;
    // Fixed pool: sized once, all slots start dead and the nozzle fills them.
    std::vector<Particle> particles(MAX_PARTICLES);

    // Neighbour grid. ~1 MB of index arrays, allocated once — never per frame.
    SPHGrid sph;
    sph.start.assign(GRID_CELLS + 1, 0);
    sph.cursor.assign(GRID_CELLS + 1, 0);
    sph.items.assign(MAX_PARTICLES, 0);
    sph.cellOf.assign(MAX_PARTICLES, -1);
    for (auto& p : particles) p.alive = false;
    Vector3 emitPos = { rsRender * 16.f, rsRender * 0.1f, 0.f };
    float   emitYaw = 0.f, emitPitch = 0.f;   // aim, spherical — set on mode entry

    //                              inner  outer   boost  quasar  jets  nebula
    const ModeParams BH_PRESET     = { 3.0f, 14.0f, 1.3f, 0.0f, 0.0f, 0.15f };
    const ModeParams QUASAR_PRESET = { 3.0f, 45.0f, 1.2f, 1.0f, 1.0f, 0.60f };
    const ModeParams MENU_PRESET   = { 3.0f, 14.0f, 1.0f, 0.0f, 0.0f, 0.25f };

    float bloomStrength = 1.0f;
    ModeParams M      = BH_PRESET;
    Screen     screen = SCREEN_MENU;

    Camera3D camera   = {};
    camera.target     = { 0.f, 0.f, 0.f };
    camera.up         = { 0.f, 1.f, 0.f };
    camera.fovy       = 50.f;
    camera.projection = CAMERA_PERSPECTIVE;
    float camAzimuth = 0.6f, camElevation = 0.4f, camDistance = 2800.f;

    int rW = GetRenderWidth(), rH = GetRenderHeight();
    float dpiScale = (float)rW / (float)screenWidth;   // framebuffer px per screen unit

    RenderTexture2D hdrBuf = LoadRenderTexture(rW,   rH);
    RenderTexture2D bloomA = LoadRenderTexture(rW/2, rH/2);
    RenderTexture2D bloomB = LoadRenderTexture(rW/2, rH/2);

    Shader lens     = LoadShader(0, "lens.fs");
    Shader brightSh = LoadShader(0, "bright.fs");
    Shader blurSh   = LoadShader(0, "blur.fs");
    Shader compSh   = LoadShader(0, "composite.fs");
    Shader glassSh  = LoadShader(0, "glass.fs");
    Shader skySh    = LoadShader(0, "sky.fs");

    VoxelField field;
    // Must cover the whole SPH domain (GRID_HALF, same bound stepParticles kills
    // particles at) — a smaller cube here doesn't stop the physics from happening
    // out there, it just makes volume.fs's inGrid() reject it: gas that's real and
    // alive renders as nothing because it's outside the box.
    field.init(-GRID_HALF, GRID_HALF * 2.0f);

    Shader volSh = LoadShader(0, "volume.fs");
    int volResLoc   = GetShaderLocation(volSh, "resolution");
    int volTimeLoc  = GetShaderLocation(volSh, "time");
    int volFwdLoc   = GetShaderLocation(volSh, "camForward");
    int volRightLoc = GetShaderLocation(volSh, "camRight");
    int volUpLoc    = GetShaderLocation(volSh, "camUp");
    int volFovLoc   = GetShaderLocation(volSh, "fovY");
    int volCamLoc   = GetShaderLocation(volSh, "camPos");
    int volBhLoc    = GetShaderLocation(volSh, "bhPos");
    int volRsLoc    = GetShaderLocation(volSh, "rs");
    int volCLoc     = GetShaderLocation(volSh, "cRender");
    int volGMinLoc  = GetShaderLocation(volSh, "gridMin");
    int volGSzLoc   = GetShaderLocation(volSh, "gridSize");
    int volVoxelNLoc = GetShaderLocation(volSh, "voxelN");
    int volDensLoc  = GetShaderLocation(volSh, "uDensity");
    int volVelLoc   = GetShaderLocation(volSh, "uVelTemp");
    int volOccLoc   = GetShaderLocation(volSh, "uOccupy");
    int volDGainLoc = GetShaderLocation(volSh, "densGain");
    int volEGainLoc = GetShaderLocation(volSh, "emitGain");
    int volNebLoc   = GetShaderLocation(volSh, "nebulaStrength");

    // Soft radial dot for particle billboards. DrawPoint3D draws a 0.1-unit LINE,
    // which is sub-pixel at this scale — that is why nothing rendered.
    // Particle sprite: a 64x64 GAUSSIAN, not raylib's linear cone. Two reasons —
    // it magnifies to ~65px without banding, and a Gaussian is the honest shape:
    // it mirrors the SPH smoothing kernel each particle already represents.
    Image dotImg = GenImageColor(64, 64, BLANK);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x - 31.5f) / 31.5f, dy = (y - 31.5f) / 31.5f;
            float rr = sqrtf(dx*dx + dy*dy);
            float a  = (rr > 1.f) ? 0.f : expf(-rr * rr * 4.2f) * (1.f - rr * rr);
            ImageDrawPixel(&dotImg, x, y,
                           Color{ 255, 255, 255, (unsigned char)(a * 255.f) });
        }
    Texture2D dotTex = LoadTextureFromImage(dotImg);
    UnloadImage(dotImg);
    SetTextureFilter(dotTex, TEXTURE_FILTER_BILINEAR);

    // Photon-ring texture: a thin bright annulus with a soft Gaussian profile, so
    // the bloom pass turns it into a glowing halo rather than a hard wire.
    const float RING_AT = 0.72f, RING_W = 0.026f;
    Image ringImg = GenImageColor(256, 256, BLANK);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            float dx = (x - 127.5f) / 127.5f, dy = (y - 127.5f) / 127.5f;
            float rr = sqrtf(dx*dx + dy*dy);
            float a  = expf(-powf((rr - RING_AT) / RING_W, 2.f));
            ImageDrawPixel(&ringImg, x, y,
                           Color{ 255, 244, 228, (unsigned char)(a * 255.f) });
        }
    Texture2D ringTex = LoadTextureFromImage(ringImg);
    UnloadImage(ringImg);
    SetTextureFilter(ringTex, TEXTURE_FILTER_BILINEAR);
    int skyResLoc   = GetShaderLocation(skySh, "resolution");
    int skyTimeLoc  = GetShaderLocation(skySh, "time");
    int skyFwdLoc   = GetShaderLocation(skySh, "camForward");
    int skyRightLoc = GetShaderLocation(skySh, "camRight");
    int skyUpLoc    = GetShaderLocation(skySh, "camUp");
    int skyFovLoc   = GetShaderLocation(skySh, "fovY");
    int skyNebLoc   = GetShaderLocation(skySh, "nebulaStrength");

    int lensResLoc       = GetShaderLocation(lens, "resolution");
    int lensTimeLoc      = GetShaderLocation(lens, "time");
    int lensFwdLoc       = GetShaderLocation(lens, "camForward");
    int lensRightLoc     = GetShaderLocation(lens, "camRight");
    int lensUpLoc        = GetShaderLocation(lens, "camUp");
    int lensFovLoc       = GetShaderLocation(lens, "fovY");
    int lensCamPosLoc    = GetShaderLocation(lens, "camPos");
    int lensBhPosLoc     = GetShaderLocation(lens, "bhPos");
    int lensRsLoc        = GetShaderLocation(lens, "rs");
    int lensDiskInnerLoc = GetShaderLocation(lens, "diskInner");
    int lensDiskOuterLoc = GetShaderLocation(lens, "diskOuter");
    int lensDiskBoostLoc = GetShaderLocation(lens, "diskBoost");
    int lensQuasarLoc    = GetShaderLocation(lens, "quasarMode");
    int lensJetLoc       = GetShaderLocation(lens, "jetStrength");
    int lensNebulaLoc    = GetShaderLocation(lens, "nebulaStrength");

    Vector3 bhPos = { 0.f, 0.f, 0.f };
    SetShaderValue(lens, lensBhPosLoc, &bhPos,    SHADER_UNIFORM_VEC3);
    SetShaderValue(lens, lensRsLoc,    &rsRender, SHADER_UNIFORM_FLOAT);

    int   brightThreshLoc = GetShaderLocation(brightSh, "threshold");
    float threshold = 0.55f;
    SetShaderValue(brightSh, brightThreshLoc, &threshold, SHADER_UNIFORM_FLOAT);

    int   blurResLoc = GetShaderLocation(blurSh, "resolution");
    int   blurHorizLoc = GetShaderLocation(blurSh, "horizontal");
    float blurRes[2] = { (float)(rW/2), (float)(rH/2) };
    SetShaderValue(blurSh, blurResLoc, blurRes, SHADER_UNIFORM_VEC2);

    int   compBloomLoc = GetShaderLocation(compSh, "bloom");
    int   compStrengthLoc = GetShaderLocation(compSh, "bloomStrength");
    SetShaderValue(compSh, compStrengthLoc, &bloomStrength, SHADER_UNIFORM_FLOAT);

    // Glass context — sampler + uniform locations, scene texture is hdrBuf.
    GlassCtx glass = {
        glassSh, hdrBuf.texture,
        GetShaderLocation(glassSh, "uReso"),   GetShaderLocation(glassSh, "uCenter"),
        GetShaderLocation(glassSh, "uHalf"),   GetShaderLocation(glassSh, "uRadius"),
        GetShaderLocation(glassSh, "uHover"),  GetShaderLocation(glassSh, "uAppear"),
        GetShaderLocation(glassSh, "uAccent"), GetShaderLocation(glassSh, "uScale"),
        GetShaderLocation(glassSh, "scene"),
    };

    const Rectangle srcHdr   = { 0, 0, (float)rW,     -(float)rH     };
    const Rectangle srcBloom = { 0, 0, (float)(rW/2), -(float)(rH/2) };
    const Rectangle dstBloom = { 0, 0, (float)(rW/2),  (float)(rH/2) };
    const Rectangle dstScr   = { 0, 0, (float)screenWidth, (float)screenHeight };

    float   menuEnterTime = (float)GetTime();
    bool    prevMenu = true;
    Vector2 prevMouse = GetMousePosition();
    Screen  prevScreen = screen;
    const int TRAIL_N = 140;
    TrailParticle trail[TRAIL_N] = {};

    while (!WindowShouldClose()) {
        bool    inMenu = (screen == SCREEN_MENU);
        float   dt     = GetFrameTime();
        Vector2 mouse  = GetMousePosition();

        if (inMenu && !prevMenu) menuEnterTime = (float)GetTime();
        float menuAge = (float)GetTime() - menuEnterTime;

        // ── Physics screen — fully self-contained, skips the shader pipeline ──────
        if (screen == SCREEN_PHYSICS) {
            if (IsKeyPressed(KEY_H)) { screen = SCREEN_MENU; prevMenu = false; continue; }
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && IsKeyDown(KEY_SPACE)) {
                Vector2 d = GetMouseDelta();
                camAzimuth   -= d.x * 0.005f;
                camElevation += d.y * 0.005f;
                camElevation  = fmaxf(-1.5f, fminf(1.5f, camElevation));
            }
            camDistance = fmaxf(200.f, fminf(3000.f, camDistance - GetMouseWheelMove() * 30.f));
            if (IsKeyDown(KEY_UP))    camDistance = fmaxf(200.f,  camDistance - 5.f);
            if (IsKeyDown(KEY_DOWN))  camDistance = fminf(3000.f, camDistance + 5.f);
            if (IsKeyDown(KEY_LEFT))  camAzimuth -= 0.01f;
            if (IsKeyDown(KEY_RIGHT)) camAzimuth += 0.01f;

            camera.position = {
                camDistance * cosf(camElevation) * sinf(camAzimuth),
                camDistance * sinf(camElevation),
                camDistance * cosf(camElevation) * cosf(camAzimuth)
            };

            if (prevScreen != SCREEN_PHYSICS) {
                for (auto& p : particles) p.alive = false;   // nozzle refills them
                emitPos     = { rsRender * 16.f, rsRender * 0.1f, 0.f };
                camDistance = 900.f;                          // cinema's 2800 is far too far
                Vector3 a0  = progradeAim(emitPos);           // start aimed for a disk
                emitYaw     = atan2f(a0.x, a0.z);
                emitPitch   = asinf(fmaxf(-1.f, fminf(1.f, a0.y)));
            }

            // Nozzle controls. D+drag moves it across the camera's view plane,
            // R+drag rotates its aim, Z/X push it along the view axis, T re-aims
            // it prograde (the angle that builds a disk).
            {
                Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                Vector3 rgt = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
                Vector3 upv = Vector3CrossProduct(rgt, fwd);

                if (IsKeyDown(KEY_D) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
                    Vector2 md = GetMouseDelta();
                    float   k  = camDistance * 0.0016f;       // same feel at any zoom
                    emitPos = Vector3Add(emitPos, Vector3Scale(rgt,  md.x * k));
                    emitPos = Vector3Add(emitPos, Vector3Scale(upv, -md.y * k));
                }
                // T grabs the arrow TIP and swings it: horizontal drag sweeps the
                // yaw through its radians, vertical drag tilts the pitch.
                if (IsKeyDown(KEY_T) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
                    Vector2 md = GetMouseDelta();
                    emitYaw   -= md.x * 0.007f;
                    emitPitch  = fmaxf(-1.45f, fminf(1.45f, emitPitch - md.y * 0.007f));
                }
                if (IsKeyDown(KEY_Z)) emitPos = Vector3Add(emitPos, Vector3Scale(fwd,  6.f));
                if (IsKeyDown(KEY_X)) emitPos = Vector3Add(emitPos, Vector3Scale(fwd, -6.f));

                // Nothing clamped emitPos before this: drag it past ~0.95*GRID_HALF
                // (stepParticles' "left the domain" kill radius) and everything
                // spawned there — a whole 180-particle clump at once — dies on the
                // very next physics step, one frame after appearing. That reads as
                // "spawned, then gone / never really there," not a visible clump.
                float er = Vector3Length(emitPos);
                if (er > GRID_HALF * 0.8f) emitPos = Vector3Scale(emitPos, (GRID_HALF * 0.8f) / er);
            }

            // Aim direction from the two angles — this is what the arrow shows and
            // what every new particle is fired along.
            Vector3 aimDir = { cosf(emitPitch) * sinf(emitYaw),
                               sinf(emitPitch),
                               cosf(emitPitch) * cosf(emitYaw) };

            // SPH, once per frame, in this exact order: you cannot compute a
            // pressure gradient before every particle knows its own density.
            gridBuild(sph, particles);     // sort particles into cells   O(N)
            sphDensity(particles, sph);    // ρ from neighbours, then P    O(N·k)
            sphForces(particles, sph);     // ∇P + shock viscosity + du/dt O(N·k)
            applyClumpCohesion(particles, rsRender, GM_RENDER);  // Roche-limit tidal disruption

            stepParticles(particles, dt, rsRender, GM_RENDER);
            field.build(particles, SPH_UHOT);      // splat particles into the 3D textures

            // Emission mode. A steady stream fills a disk. Discrete CLUMPS spawn as a
            // bound blob (see applyClumpCohesion) that holds together under its own
            // cohesion until the hole's tidal shear exceeds CLUMP_BIND_STRENGTH — then
            // it shreds, same as a real Roche-limit tidal disruption.
            static bool  clumpMode   = false;
            static float clumpTimer  = 0.f;
            static int   nextClumpId = 0;
            if (IsKeyPressed(KEY_C)) clumpMode = !clumpMode;

            int budget = clumpMode ? 0 : SPAWN_PER_FRAME;
            int burstClumpId = -1;
            if (clumpMode) {
                clumpTimer += dt;
                if (clumpTimer >= CLUMP_INTERVAL) {
                    clumpTimer = 0.f;
                    budget = CLUMP_SIZE;
                    burstClumpId = nextClumpId++;   // whole burst shares one id
                }
            }

            int spawned = 0, aliveCount = 0;
            for (int i = 0; i < (int)particles.size(); i++) {
                if (!particles[i].alive && spawned < budget) {
                    respawn(particles[i], emitPos, aimDir, rsRender, GM_RENDER, burstClumpId);
                    spawned++;
                }
                if (particles[i].alive) aliveCount++;
            }

            // Build camera basis for sky shader.
            float   physRes[2]  = { (float)rW, (float)rH };
            float   physNow     = (float)GetTime();
            float   physFov     = camera.fovy * DEG2RAD;
            Vector3 physFwd     = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            Vector3 physRight   = Vector3Normalize(Vector3CrossProduct(physFwd, camera.up));
            Vector3 physUp      = Vector3CrossProduct(physRight, physFwd);
            float   physNebStr  = 0.6f;
            SetShaderValue(skySh, skyResLoc,   physRes,     SHADER_UNIFORM_VEC2);
            SetShaderValue(skySh, skyTimeLoc,  &physNow,    SHADER_UNIFORM_FLOAT);
            SetShaderValue(skySh, skyFwdLoc,   &physFwd,    SHADER_UNIFORM_VEC3);
            SetShaderValue(skySh, skyRightLoc, &physRight,  SHADER_UNIFORM_VEC3);
            SetShaderValue(skySh, skyUpLoc,    &physUp,     SHADER_UNIFORM_VEC3);
            SetShaderValue(skySh, skyFovLoc,   &physFov,    SHADER_UNIFORM_FLOAT);
            SetShaderValue(skySh, skyNebLoc,   &physNebStr, SHADER_UNIFORM_FLOAT);

            static bool volumetric = true;
            if (IsKeyPressed(KEY_V)) volumetric = !volumetric;

            if (volumetric) {
                // Lensed volumetric render: ray-march the same geodesic as lens.fs,
                // but sample the SPH field's own density/velocity/temperature textures
                // instead of a procedural disk — so the far side lenses over the top
                // and Doppler beaming follows the particles' real velocities.
                Vector3 volGmin = { field.gridMin[0], field.gridMin[1], field.gridMin[2] };
                float   dG = 6.0f, eG = 1.4f, neb = 0.4f;     // the look dials (Step 6)

                SetShaderValue(volSh, volResLoc,   physRes,          SHADER_UNIFORM_VEC2);
                SetShaderValue(volSh, volTimeLoc,  &physNow,         SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volFwdLoc,   &physFwd,         SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volRightLoc, &physRight,       SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volUpLoc,    &physUp,          SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volFovLoc,   &physFov,         SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volCamLoc,   &camera.position, SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volBhLoc,    &bhPos,           SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volRsLoc,    &rsRender,        SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volCLoc,     &C_RENDER,        SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volGMinLoc,  &volGmin,         SHADER_UNIFORM_VEC3);
                SetShaderValue(volSh, volGSzLoc,   &field.gridSize,  SHADER_UNIFORM_FLOAT);
                float voxelN = (float)VoxelField::N;   // was hardcoded in the shader before
                                                        // and silently went stale the last
                                                        // time N changed — now derived live.
                SetShaderValue(volSh, volVoxelNLoc, &voxelN,         SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volDGainLoc, &dG,              SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volEGainLoc, &eG,              SHADER_UNIFORM_FLOAT);
                SetShaderValue(volSh, volNebLoc,   &neb,             SHADER_UNIFORM_FLOAT);

                BeginTextureMode(hdrBuf);
                    ClearBackground(BLACK);
                    BeginShaderMode(volSh);
                        field.bind(volSh, volDensLoc, volVelLoc, volOccLoc);
                        DrawRectangle(0, 0, rW, rH, WHITE);
                    EndShaderMode();

                    // The shader quad has no notion of the nozzle — draw it as a
                    // normal 3D overlay on top, same as the point-render path, so
                    // you can still see where you're aiming while volumetric.
                    BeginMode3D(camera);
                        DrawSphereWires(emitPos, EMIT_SPREAD, 8, 8, Fade(SKYBLUE, 0.45f));
                        {
                            bool    held = IsKeyDown(KEY_T);
                            Color   ac   = held ? Fade(WHITE, 0.95f) : Fade(SKYBLUE, 0.95f);
                            Vector3 tip  = Vector3Add(emitPos, Vector3Scale(aimDir, ARROW_LEN));
                            Vector3 neck = Vector3Add(emitPos, Vector3Scale(aimDir, ARROW_LEN * 0.72f));
                            DrawCylinderEx(emitPos, neck, 1.8f, 1.8f, 8, ac);
                            DrawCylinderEx(neck, tip,    6.0f, 0.0f, 10, ac);
                            DrawSphere(tip, held ? 8.f : 5.f, ac);
                        }
                    EndMode3D();
                EndTextureMode();
            } else {
            // Sky (fullscreen quad, no depth write) then 3D scene on top.
            BeginTextureMode(hdrBuf);
                ClearBackground(BLACK);
                rlDisableDepthMask();
                BeginShaderMode(skySh);
                    DrawRectangle(0, 0, rW, rH, WHITE);
                EndShaderMode();
                rlEnableDepthMask();
                BeginMode3D(camera);
                    // Shadow first, opaque, so it writes depth and occludes matter behind it.
                    DrawSphere({ 0, 0, 0 }, rsRender * 2.6f, BLACK);

                    // Photon-sphere corona glow — the faint orange halo that makes
                    // the silhouette visible against the sky background.
                    BeginBlendMode(BLEND_ADDITIVE);
                    rlDisableDepthMask();
                    DrawSphere({0,0,0}, rsRender * 3.6f, Color{ 55, 20, 6, 28 });
                    DrawSphere({0,0,0}, rsRender * 3.0f, Color{ 80, 32, 8, 45 });
                    rlEnableDepthMask();
                    EndBlendMode();

                    // ISCO orbit ring — vivid so you can see where disk terminates.
                    DrawCircle3D({ 0,0,0 }, rsRender * 3.0f, { 1,0,0 }, 90.f,
                                 Color{ 100, 180, 255, 210 });

                    // Photon ring — the thin bright annulus at the shadow edge.
                    // Photon ring. It is the shadow's SILHOUETTE — light that grazed
                    // the hole and escaped — so it is a circle perpendicular to the
                    // LINE OF SIGHT, identical from every angle. A billboard is the
                    // exact representation; a fixed-plane circle would lie flat in
                    // the disk and vanish among the particles.
                    BeginBlendMode(BLEND_ADDITIVE);
                    rlDisableDepthMask();
                    DrawBillboard(camera, ringTex, { 0, 0, 0 },
                                  rsRender * 2.62f * 2.f / RING_AT,
                                  Color{ 255, 244, 228, 255 });
                    rlDrawRenderBatchActive();   // flush NOW — see note below
                    rlEnableDepthMask();
                    EndBlendMode();

                    // Depth TEST on (the shadow still hides matter), depth WRITE off
                    // (so additive glow accumulates regardless of draw order).
                    //
                    // CRITICAL: rlDisableDepthMask() is an immediate GL call, but
                    // DrawBillboard only QUEUES vertices into raylib's batch. Without
                    // an explicit flush, the batch draws at EndMode3D — by which time
                    // the mask is back ON, every quad writes depth across its whole
                    // square (transparent corners included), and the billboards
                    // depth-reject each other into visible flat squares.
                    BeginBlendMode(BLEND_ADDITIVE);
                    rlDisableDepthMask();
                    for (const auto& p : particles) {
                        if (!p.alive) continue;
                        float s = PART_SIZE * (0.9f + 0.25f * p.heat);  // hotter = slightly bigger
                        DrawBillboard(camera, dotTex, p.pos, s, particleColor(p, camera.position, C_RENDER));
                    }
                    rlDrawRenderBatchActive();   // draw them while depth-write is OFF
                    rlEnableDepthMask();
                    EndBlendMode();

                    // The nozzle you're holding, and the arrow showing where it fires.
                    DrawSphereWires(emitPos, EMIT_SPREAD, 8, 8, Fade(SKYBLUE, 0.45f));
                    {
                        bool    held = IsKeyDown(KEY_T);
                        Color   ac   = held ? Fade(WHITE, 0.95f) : Fade(SKYBLUE, 0.95f);
                        Vector3 tip  = Vector3Add(emitPos, Vector3Scale(aimDir, ARROW_LEN));
                        Vector3 neck = Vector3Add(emitPos, Vector3Scale(aimDir, ARROW_LEN * 0.72f));
                        DrawCylinderEx(emitPos, neck, 1.8f, 1.8f, 8, ac);
                        DrawCylinderEx(neck, tip,    6.0f, 0.0f, 10, ac);
                        // The grab handle — lights up while T is held.
                        DrawSphere(tip, held ? 8.f : 5.f, ac);
                    }
                EndMode3D();
            EndTextureMode();
            }

            // Bloom passes (particles bloom via additive).
            BeginTextureMode(bloomA);
                ClearBackground(BLACK);
                BeginShaderMode(brightSh);
                    DrawTexturePro(hdrBuf.texture, srcHdr, dstBloom, {0,0}, 0.f, WHITE);
                EndShaderMode();
            EndTextureMode();
            for (int bi = 0; bi < 2; bi++) {
                int bh = 1;
                SetShaderValue(blurSh, blurHorizLoc, &bh, SHADER_UNIFORM_INT);
                BeginTextureMode(bloomB);
                    ClearBackground(BLACK);
                    BeginShaderMode(blurSh);
                        DrawTexturePro(bloomA.texture, srcBloom, dstBloom, {0,0}, 0.f, WHITE);
                    EndShaderMode();
                EndTextureMode();
                int bv = 0;
                SetShaderValue(blurSh, blurHorizLoc, &bv, SHADER_UNIFORM_INT);
                BeginTextureMode(bloomA);
                    ClearBackground(BLACK);
                    BeginShaderMode(blurSh);
                        DrawTexturePro(bloomB.texture, srcBloom, dstBloom, {0,0}, 0.f, WHITE);
                    EndShaderMode();
                EndTextureMode();
            }

            BeginDrawing();
                ClearBackground(BLACK);
                BeginShaderMode(compSh);
                    SetShaderValueTexture(compSh, compBloomLoc, bloomA.texture);
                    DrawTexturePro(hdrBuf.texture, srcHdr, dstScr, {0,0}, 0.f, WHITE);
                EndShaderMode();
                DrawTextEx(uiFont, "PHYSICS SIM", { 16, 16 }, 22, 3, Color{ 90, 220, 150, 255 });
                DrawTextEx(uiFont, TextFormat("%d / %d particles", aliveCount, MAX_PARTICLES),
                           { 16, 44 }, 14, 2, Color{ 130, 135, 155, 255 });
                DrawTextEx(uiFont, "D+drag move  T+drag aim  Z/X depth  C clumps  H home",
                           { 16, 64 }, 12, 2, Color{ 110, 115, 135, 255 });
                DrawTextEx(uiFont, TextFormat("mode: %s   yaw %.0f°   pitch %.0f°",
                                              clumpMode ? "CLUMPS" : "stream",
                                              emitYaw * RAD2DEG, emitPitch * RAD2DEG),
                           { 16, 82 }, 12, 2,
                           clumpMode ? Color{120,220,130,255} : Color{110,115,135,255});
                DrawFPS(screenWidth - 90, 12);
            EndDrawing();

            prevMenu   = false;
            prevMouse  = mouse;
            prevScreen = screen;
            continue;
        }
        // ─────────────────────────────────────────────────────────────────────────

        if (!inMenu) {
            if (IsKeyPressed(KEY_H)) screen = SCREEN_MENU;
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && IsKeyDown(KEY_SPACE)) {
                Vector2 d = GetMouseDelta();
                camAzimuth   -= d.x * 0.005f;
                camElevation += d.y * 0.005f;
                camElevation  = fmaxf(-1.5f, fminf(1.5f, camElevation));
            }
            camDistance = fmaxf(200.f, fminf(3000.f, camDistance - GetMouseWheelMove() * 30.f));
            if (IsKeyDown(KEY_UP))    camDistance = fmaxf(200.f,  camDistance - 5.f);
            if (IsKeyDown(KEY_DOWN))  camDistance = fminf(3000.f, camDistance + 5.f);
            if (IsKeyDown(KEY_LEFT))  camAzimuth -= 0.01f;
            if (IsKeyDown(KEY_RIGHT)) camAzimuth += 0.01f;
            if (IsKeyPressed(KEY_Q)) {
                bool toQuasar = (M.quasarMode < 0.5f);
                M      = toQuasar ? QUASAR_PRESET : BH_PRESET;
                screen = toQuasar ? SCREEN_QUASAR : SCREEN_BLACKHOLE;
            }
        }

        if (inMenu) {
            float t = (float)GetTime();
            float az = t * 0.06f, el = 0.34f + 0.05f * sinf(t * 0.25f), dst = 820.f;
            camera.position = { dst*cosf(el)*sinf(az), dst*sinf(el), dst*cosf(el)*cosf(az) };
        } else {
            camera.position = {
                camDistance * cosf(camElevation) * sinf(camAzimuth),
                camDistance * sinf(camElevation),
                camDistance * cosf(camElevation) * cosf(camAzimuth)
            };
        }

        const ModeParams& A = inMenu ? MENU_PRESET : M;
        float diskInner = rsRender * A.diskInnerMul;
        float diskOuter = rsRender * A.diskOuterMul;

        float   now      = (float)GetTime();
        float   res[2]   = { (float)rW, (float)rH };
        Vector3 camFwd   = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 camRight = Vector3Normalize(Vector3CrossProduct(camFwd, camera.up));
        Vector3 camUp    = Vector3CrossProduct(camRight, camFwd);
        float   fovYrad  = camera.fovy * DEG2RAD;

        SetShaderValue(lens, lensResLoc,       res,               SHADER_UNIFORM_VEC2);
        SetShaderValue(lens, lensTimeLoc,      &now,              SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensFwdLoc,       &camFwd,           SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensRightLoc,     &camRight,         SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensUpLoc,        &camUp,            SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensFovLoc,       &fovYrad,          SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensCamPosLoc,    &camera.position,  SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensDiskInnerLoc, &diskInner,        SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensDiskOuterLoc, &diskOuter,        SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensDiskBoostLoc, &A.diskBoost,      SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensQuasarLoc,    &A.quasarMode,     SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensJetLoc,       &A.jetStrength,    SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensNebulaLoc,    &A.nebulaStrength, SHADER_UNIFORM_FLOAT);

        BeginTextureMode(hdrBuf);
            ClearBackground(BLACK);
            BeginShaderMode(lens);
                DrawRectangle(0, 0, rW, rH, WHITE);
            EndShaderMode();
        EndTextureMode();

        BeginTextureMode(bloomA);
            ClearBackground(BLACK);
            BeginShaderMode(brightSh);
                DrawTexturePro(hdrBuf.texture, srcHdr, dstBloom, {0,0}, 0.f, WHITE);
            EndShaderMode();
        EndTextureMode();

        for (int i = 0; i < 2; i++) {
            int h = 1;
            SetShaderValue(blurSh, blurHorizLoc, &h, SHADER_UNIFORM_INT);
            BeginTextureMode(bloomB);
                ClearBackground(BLACK);
                BeginShaderMode(blurSh);
                    DrawTexturePro(bloomA.texture, srcBloom, dstBloom, {0,0}, 0.f, WHITE);
                EndShaderMode();
            EndTextureMode();
            int v = 0;
            SetShaderValue(blurSh, blurHorizLoc, &v, SHADER_UNIFORM_INT);
            BeginTextureMode(bloomA);
                ClearBackground(BLACK);
                BeginShaderMode(blurSh);
                    DrawTexturePro(bloomB.texture, srcBloom, dstBloom, {0,0}, 0.f, WHITE);
                EndShaderMode();
            EndTextureMode();
        }

        BeginDrawing();
            ClearBackground(BLACK);
            BeginShaderMode(compSh);
                SetShaderValueTexture(compSh, compBloomLoc, bloomA.texture);
                DrawTexturePro(hdrBuf.texture, srcHdr, dstScr, {0,0}, 0.f, WHITE);
            EndShaderMode();

            if (inMenu) {
                drawMenuUI(screen, M, BH_PRESET, QUASAR_PRESET, uiFont, menuAge, glass, dpiScale);
                bool moved = Vector2Distance(mouse, prevMouse) > 1.5f;
                updateDrawTrail(trail, TRAIL_N, mouse, moved, dt);
            } else {
                DrawTextEx(uiFont, (screen == SCREEN_QUASAR) ? "QUASAR" : "BLACK HOLE",
                           { 16, 16 }, 22, 3, Color{ 255, 170, 90, 255 });
                DrawTextEx(uiFont, "Q  switch     H  home", { 16, 44 }, 14, 2,
                           Color{ 130, 135, 155, 255 });
                DrawFPS(screenWidth - 90, 12);
            }
        EndDrawing();

        prevMenu   = inMenu;
        prevMouse  = mouse;
        prevScreen = screen;
    }

    UnloadTexture(dotTex);
    UnloadTexture(ringTex);
    if (fontLoaded) UnloadFont(uiFont);
    UnloadShader(lens);   UnloadShader(brightSh); UnloadShader(blurSh);
    UnloadShader(compSh); UnloadShader(glassSh);  UnloadShader(skySh);
    field.unload();
    UnloadShader(volSh);
    UnloadRenderTexture(hdrBuf); UnloadRenderTexture(bloomA); UnloadRenderTexture(bloomB);
    CloseWindow();
    return 0;
}