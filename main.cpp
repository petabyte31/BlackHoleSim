// Black Hole Simulation using raylib — Stage 6a (lensing background)
//
// Frame structure: each loop iteration is three phases, strictly ordered:
//   1. UPDATE  — input, camera state, spawning, dragging
//   2. PHYSICS — step(), trails, horizon removal
//   3. DRAW    — exactly one BeginDrawing()/EndDrawing()

#include <raylib.h>
#include <rlgl.h>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <raymath.h>

const int   TRAIL_MAX   = 120;
const float MASS_SMALL  = 1e24f;
const float MASS_MEDIUM = 1e28f;
const float MASS_LARGE  = 2e29f;

const float EPSILON_BH   = 0.1f;
const float EPSILON_BODY = 5e5f;

struct BlackHole {
    float   mass;
    float   gravityConstant;
    float   radius;
    float   eventHorizonRadius;
    Vector3 position;
};

struct Body {
    Vector3              position;
    Vector3              velocity;
    float                mass;
    std::vector<Vector3> trail;
    bool    isDebris = false;   // already-shredded fragment
    float   heat     = 0.0f;    // 0 cool .. 1 white-hot (for rendering)
};

void computeAccelerations(const std::vector<Body>& bodies,
                          const BlackHole&         bh,
                          std::vector<Vector3>&    accel)
{
    size_t n = bodies.size();
    std::fill(accel.begin(), accel.end(), Vector3{0.0f, 0.0f, 0.0f});

    for (size_t i = 0; i < n; i++) {
        float dx  = bh.position.x - bodies[i].position.x;
        float dy  = bh.position.y - bodies[i].position.y;
        float dz  = bh.position.z - bodies[i].position.z;
        float r   = sqrtf(dx*dx + dy*dy + dz*dz);
        // Paczyński–Wiita pseudo-potential: GM/(r−Rs)² instead of GM/r²
        // Reproduces the GR ISCO at 3Rs — bodies inside that naturally spiral in.
        float rPW = fmaxf(r - bh.radius, 0.5f * bh.radius);
        float a   = bh.gravityConstant * bh.mass / (rPW * rPW);
        accel[i].x += a * (dx / r);
        accel[i].y += a * (dy / r);
        accel[i].z += a * (dz / r);
    }

    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            float dx    = bodies[j].position.x - bodies[i].position.x;
            float dy    = bodies[j].position.y - bodies[i].position.y;
            float dz    = bodies[j].position.z - bodies[i].position.z;
            float dist  = sqrtf(dx*dx + dy*dy + dz*dz + EPSILON_BODY*EPSILON_BODY);
            float invR3 = 1.0f / (dist * dist * dist);
            float ax    = bh.gravityConstant * invR3 * dx;
            float ay    = bh.gravityConstant * invR3 * dy;
            float az    = bh.gravityConstant * invR3 * dz;
            accel[i].x += ax * bodies[j].mass;
            accel[i].y += ay * bodies[j].mass;
            accel[i].z += az * bodies[j].mass;
            accel[j].x -= ax * bodies[i].mass;
            accel[j].y -= ay * bodies[i].mass;
            accel[j].z -= az * bodies[i].mass;
        }
    }
}

void step(std::vector<Body>& bodies, const BlackHole& bh, float dt)
{
    size_t n = bodies.size();
    std::vector<Vector3> accel(n);

    computeAccelerations(bodies, bh, accel);
    for (size_t i = 0; i < n; i++) {
        bodies[i].velocity.x += accel[i].x * (0.5f * dt);
        bodies[i].velocity.y += accel[i].y * (0.5f * dt);
        bodies[i].velocity.z += accel[i].z * (0.5f * dt);
    }
    for (size_t i = 0; i < n; i++) {
        bodies[i].position.x += bodies[i].velocity.x * dt;
        bodies[i].position.y += bodies[i].velocity.y * dt;
        bodies[i].position.z += bodies[i].velocity.z * dt;
    }
    computeAccelerations(bodies, bh, accel);
    for (size_t i = 0; i < n; i++) {
        bodies[i].velocity.x += accel[i].x * (0.5f * dt);
        bodies[i].velocity.y += accel[i].y * (0.5f * dt);
        bodies[i].velocity.z += accel[i].z * (0.5f * dt);
    }
}

void spawnBody(std::vector<Body>& bodies,
               float orbitRadius, float orbitVelocity, float mass,
               float inclination = 0.0f)
{
    float theta = ((float)rand() / RAND_MAX) * 2.0f * (float)M_PI;
    float ci    = cosf(inclination);
    float si    = sinf(inclination);

    float px = orbitRadius * cosf(theta);
    float pz = orbitRadius * sinf(theta);
    float vx = -orbitVelocity * sinf(theta);
    float vz =  orbitVelocity * cosf(theta);

    Body b;
    b.mass     = mass;
    b.position = { px, -pz * si, pz * ci };
    b.velocity = { vx, -vz * si, vz * ci };
    bodies.push_back(b);
}

static inline Vector3 toRender(Vector3 sim, float rs) {
    return { sim.x * rs, sim.y * rs, sim.z * rs };
}

static inline Vector3 renderToSim(Vector3 r, float rs) {
    return { r.x / rs, r.y / rs, r.z / rs };
}

Vector3 mouseHitWorldPlane(Camera3D camera, float planeY = 0.0f) {
    Ray ray = GetMouseRay(GetMousePosition(), camera);
    if (fabsf(ray.direction.y) < 1e-6f) return camera.target;
    float t = (planeY - ray.position.y) / ray.direction.y;
    if (t < 0.0f) return camera.target;
    return {
        ray.position.x + t * ray.direction.x,
        planeY,
        ray.position.z + t * ray.direction.z
    };
}

int main() {
    const int screenWidth  = 800;
    const int screenHeight = 450;

    // -------- Black hole --------
    BlackHole blackHole;
    blackHole.mass               = 8e30f;
    blackHole.gravityConstant    = 6.67430e-11f;
    const float speedOfLight     = 299792458.0f;
    blackHole.radius             = 2.0f * (blackHole.gravityConstant * blackHole.mass
                                           / (speedOfLight * speedOfLight));
    blackHole.eventHorizonRadius = blackHole.radius;
    blackHole.position           = { 0.0f, 0.0f, 0.0f };

    // -------- Scales and tuning --------
    const float TARGET_FRACTION    = 0.15f;
    const float RENDER_SCALE       = (screenWidth * TARGET_FRACTION) / blackHole.radius;
    const float BODY_RENDER_RADIUS = 5.0f;

    float orbitRadius   = 0.9f * (screenWidth * 0.5f) / RENDER_SCALE;
    float orbitVelocity = sqrtf(blackHole.gravityConstant
                                * (blackHole.mass + MASS_SMALL) / orbitRadius);

    const float TARGET_ORBIT_SECONDS = 30.0f;
    const float MIN_ORBIT_SECONDS    = 10.0f;
    const float MAX_ORBIT_SECONDS    = 1000.0f;
    float orbitalPeriod  = 2.0f * (float)M_PI * orbitRadius / orbitVelocity;
    float targetSeconds  = fmaxf(MIN_ORBIT_SECONDS, fminf(MAX_ORBIT_SECONDS, TARGET_ORBIT_SECONDS));
    const float TIME_SCALE = orbitalPeriod / targetSeconds;
    const float DRAG_SCALE = 3.0f * orbitVelocity / (orbitRadius * TIME_SCALE);

    // -------- Camera --------
    Camera3D camera   = { 0 };
    camera.target     = { 0.0f, 0.0f, 0.0f };
    camera.up         = { 0.0f, 1.0f, 0.0f };
    camera.fovy       = 50.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    float camAzimuth   = 0.6f;
    float camElevation = 0.4f;
    float camDistance  = 780.0f;

    // -------- Bodies + interaction state --------
    std::vector<Body> bodies;
    spawnBody(bodies, orbitRadius, orbitVelocity, MASS_SMALL);

    bool    dragging        = false;
    Vector3 dragStartWorld  = { 0.0f, 0.0f, 0.0f };
    Vector2 dragStartScreen = { 0.0f, 0.0f };
    int     swallowedCount  = 0;

    // -------- Window + GL setup --------
    InitWindow(screenWidth, screenHeight, "raylib - blackholesim 3D");
    rlSetClipPlanes(0.1, 100000.0);
    SetTargetFPS(60);

    // -------- Lensing shader (fullscreen background: stars + shadow + bending) --------
    Shader lens        = LoadShader(0, "lens.fs");
    int lensResLoc     = GetShaderLocation(lens, "resolution");
    int lensTimeLoc    = GetShaderLocation(lens, "time");
    int lensFwdLoc     = GetShaderLocation(lens, "camForward");
    int lensRightLoc   = GetShaderLocation(lens, "camRight");
    int lensUpLoc      = GetShaderLocation(lens, "camUp");
    int lensFovLoc     = GetShaderLocation(lens, "fovY");
    int lensCamPosLoc  = GetShaderLocation(lens, "camPos");
    int lensBhPosLoc   = GetShaderLocation(lens, "bhPos");
    int lensRsLoc        = GetShaderLocation(lens, "rs");
    int lensDiskInnerLoc  = GetShaderLocation(lens, "diskInner");
    int lensDiskOuterLoc  = GetShaderLocation(lens, "diskOuter");
    int lensDiskBoostLoc  = GetShaderLocation(lens, "diskBoost");

    // Constant uniforms — set once (these never change while running).
    float   rsRender      = blackHole.radius * RENDER_SCALE;   // Rs in render units
    float   diskInner     = rsRender * 1.5f;   // inside shadow boundary so shadow defines the visible inner edge
    float   diskOuter     = rsRender * 4.5f;   // comfortably inside camDistance (780)
    Vector3 bhP           = blackHole.position;
    SetShaderValue(lens, lensRsLoc,       &rsRender,  SHADER_UNIFORM_FLOAT);
    SetShaderValue(lens, lensBhPosLoc,    &bhP,       SHADER_UNIFORM_VEC3);
    SetShaderValue(lens, lensDiskInnerLoc, &diskInner, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lens, lensDiskOuterLoc, &diskOuter, SHADER_UNIFORM_FLOAT);

    float       accretionFlare = 0.0f;
    const float TIDAL_RADIUS   = 3.5f * blackHole.radius;   // SIM units — stays below orbit radius (~3Rs) so bodies shred only during infall

    while (!WindowShouldClose()) {

        // ================== PHASE 1: UPDATE ==================

        // Clear all bodies and reset the counter
        if (IsKeyPressed(KEY_C)) {
            bodies.clear();
            swallowedCount = 0;
        }

        // Keyboard zoom
        if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)      || IsKeyDown(KEY_UP))   camDistance *= 0.98f;
        if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT) || IsKeyDown(KEY_DOWN)) camDistance *= 1.02f;

        // Spawn keys
        if (IsKeyPressed(KEY_ONE))   spawnBody(bodies, orbitRadius, orbitVelocity, MASS_SMALL);
        if (IsKeyPressed(KEY_TWO))   spawnBody(bodies, orbitRadius, orbitVelocity, MASS_MEDIUM);
        if (IsKeyPressed(KEY_THREE)) spawnBody(bodies, orbitRadius, orbitVelocity, MASS_LARGE);
        if (IsKeyPressed(KEY_I)) {
            float inclination = ((float)rand() / RAND_MAX - 0.5f) * (float)M_PI * 0.5f;
            spawnBody(bodies, orbitRadius, orbitVelocity, MASS_MEDIUM, inclination);
        }
        if (IsKeyPressed(KEY_FOUR)) {
            // Spawn inside the ISCO at 2 Rs — sub-circular velocity so it spirals in and gets swallowed
            float tightR = 2.0f * blackHole.radius;
            float tightV = sqrtf(blackHole.gravityConstant * blackHole.mass / tightR) * 0.9f;
            spawnBody(bodies, tightR, tightV, MASS_SMALL);
        }

        // Camera orbit (Space + left-drag)
        if (IsKeyDown(KEY_SPACE) && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && !dragging) {
            Vector2 delta = GetMouseDelta();
            camAzimuth   -= delta.x * 0.005f;
            camElevation -= delta.y * 0.005f;
            camElevation = fmaxf(-1.5f, fminf(1.5f, camElevation));
        }

        // Scroll zoom
        float scroll = GetMouseWheelMove();
        camDistance *= powf(0.9f, scroll);
        camDistance  = fmaxf(50.0f, fminf(camDistance, 10000.0f));

        // Recompute camera position from spherical coords (before any drawing uses it)
        camera.position = {
            camera.target.x + camDistance * cosf(camElevation) * sinf(camAzimuth),
            camera.target.y + camDistance * sinf(camElevation),
            camera.target.z + camDistance * cosf(camElevation) * cosf(camAzimuth)
        };

        // Drag-to-launch: press
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !IsKeyDown(KEY_SPACE)) {
            Vector3 hit     = mouseHitWorldPlane(camera, 0.0f);
            dragStartWorld  = renderToSim(hit, RENDER_SCALE);
            dragStartScreen = GetMousePosition();
            dragging        = true;
        }
        // Drag-to-launch: release
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON) && dragging) {
            Vector3 hitEnd       = mouseHitWorldPlane(camera, 0.0f);
            Vector3 dragEndWorld = renderToSim(hitEnd, RENDER_SCALE);

            float launchMass = MASS_SMALL;
            if (IsKeyDown(KEY_TWO))   launchMass = MASS_MEDIUM;
            if (IsKeyDown(KEY_THREE)) launchMass = MASS_LARGE;

            Body b;
            b.mass     = launchMass;
            b.position = dragStartWorld;
            b.velocity = {
                (dragEndWorld.x - dragStartWorld.x) * DRAG_SCALE * TIME_SCALE,
                (dragEndWorld.y - dragStartWorld.y) * DRAG_SCALE * TIME_SCALE,
                (dragEndWorld.z - dragStartWorld.z) * DRAG_SCALE * TIME_SCALE
            };
            bodies.push_back(b);
            dragging = false;
        }

        // ================== PHASE 2: PHYSICS ==================

        // Pre-step horizon check: remove bodies already inside Rs before step() runs.
        // Without this, the PW force (GM/(r-Rs)²) gives a body right at the horizon a
        // huge kick in one Verlet step, causing it to shoot back out with enormous velocity.
        for (size_t i = 0; i < bodies.size(); ) {
            float dx = bodies[i].position.x - blackHole.position.x;
            float dy = bodies[i].position.y - blackHole.position.y;
            float dz = bodies[i].position.z - blackHole.position.z;
            if (sqrtf(dx*dx + dy*dy + dz*dz) < blackHole.eventHorizonRadius) {
                accretionFlare += bodies[i].mass / MASS_SMALL;
                accretionFlare  = fminf(accretionFlare, 3.0f);
                swallowedCount++;
                bodies.erase(bodies.begin() + i);
            } else { ++i; }
        }

        float dt = GetFrameTime() * TIME_SCALE;
        step(bodies, blackHole, dt);

        // Trail push — debris gets a shorter tail so it doesn't clutter
        for (Body& b : bodies) {
            b.trail.push_back(b.position);
            int maxTrail = b.isDebris ? 30 : TRAIL_MAX;
            if ((int)b.trail.size() > maxTrail) b.trail.erase(b.trail.begin());
        }

        // Tidal disruption, heat update, horizon removal
        {
            std::vector<Body> fresh;
            for (size_t i = 0; i < bodies.size(); ) {
                Body& b   = bodies[i];
                float dx   = b.position.x - blackHole.position.x;
                float dy   = b.position.y - blackHole.position.y;
                float dz   = b.position.z - blackHole.position.z;
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);

                // Swallowed
                if (dist < blackHole.eventHorizonRadius) {
                    accretionFlare += b.mass / MASS_SMALL;
                    accretionFlare  = fminf(accretionFlare, 3.0f);
                    swallowedCount++;
                    bodies.erase(bodies.begin() + i);
                    continue;
                }

                // Tidal shredding (non-debris only)
                if (!b.isDebris && dist < TIDAL_RADIUS) {
                    float invDist = 1.0f / dist;
                    float rx = dx*invDist, ry = dy*invDist, rz = dz*invDist;  // radial unit vec

                    // Tangent = velocity minus its radial component
                    float vDotR = b.velocity.x*rx + b.velocity.y*ry + b.velocity.z*rz;
                    float tx = b.velocity.x - vDotR*rx;
                    float ty = b.velocity.y - vDotR*ry;
                    float tz = b.velocity.z - vDotR*rz;
                    float tLen = sqrtf(tx*tx + ty*ty + tz*tz);
                    if (tLen < 1.0f) {           // velocity nearly radial — pick any perp
                        tx = -rz; ty = 0.0f; tz = rx;
                        tLen = sqrtf(tx*tx + tz*tz);
                        if (tLen < 1e-6f) { tx = 0; ty = 1; tz = 0; tLen = 1.0f; }
                    }
                    tx /= tLen; ty /= tLen; tz /= tLen;

                    float speed      = sqrtf(b.velocity.x*b.velocity.x + b.velocity.y*b.velocity.y + b.velocity.z*b.velocity.z);
                    float streamLen  = dist * 0.4f;
                    float shearSpeed = speed * 0.25f;
                    float ejectSpeed = speed * 0.08f;

                    const int N = 18;
                    for (int k = 0; k < N; k++) {
                        float along = (float)k / (N - 1) - 0.5f;
                        float r01   = (float)rand() / RAND_MAX;
                        Body f;
                        f.isDebris = true;
                        f.mass     = b.mass / N;
                        f.position = { b.position.x + tx*along*streamLen,
                                       b.position.y + ty*along*streamLen,
                                       b.position.z + tz*along*streamLen };
                        f.velocity = { b.velocity.x + tx*along*shearSpeed + rx*(r01-0.5f)*ejectSpeed,
                                       b.velocity.y + ty*along*shearSpeed + ry*(r01-0.5f)*ejectSpeed,
                                       b.velocity.z + tz*along*shearSpeed + rz*(r01-0.5f)*ejectSpeed };
                        fresh.push_back(f);
                    }
                    bodies.erase(bodies.begin() + i);
                    continue;
                }

                // Heat: rises from 3Rs (orbit) to TIDAL_RADIUS (2Rs), matching disintegration
                b.heat = (!b.isDebris && dist < 3.0f * blackHole.radius)
                    ? fmaxf(0.0f, fminf(1.0f, (3.0f * blackHole.radius - dist) / blackHole.radius))
                    : 0.0f;

                ++i;
            }
            for (auto& f : fresh) bodies.push_back(f);
        }

        accretionFlare *= 0.94f;   // flare decays ~1 second half-life at 60 fps

        // ================== PHASE 3: DRAW ==================

        float now = (float)GetTime();

        BeginDrawing();
        ClearBackground(BLACK);

        // Lensing background — camera basis + per-frame uniforms, then fullscreen draw.
        Vector3 camFwd    = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 camRightV = Vector3Normalize(Vector3CrossProduct(camFwd, camera.up));
        Vector3 camUpV    = Vector3CrossProduct(camRightV, camFwd);
        float   fovYrad   = camera.fovy * DEG2RAD;

        float resolution[2] = { (float)GetRenderWidth(), (float)GetRenderHeight() };
        SetShaderValue(lens, lensResLoc,    resolution,       SHADER_UNIFORM_VEC2);
        SetShaderValue(lens, lensTimeLoc,   &now,             SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensFwdLoc,    &camFwd,          SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensRightLoc,  &camRightV,       SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensUpLoc,     &camUpV,          SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensFovLoc,    &fovYrad,         SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensCamPosLoc, &camera.position, SHADER_UNIFORM_VEC3);

        float diskBoost = 1.0f + accretionFlare;
        SetShaderValue(lens, lensDiskBoostLoc, &diskBoost, SHADER_UNIFORM_FLOAT);

        BeginShaderMode(lens);
            DrawRectangle(0, 0, screenWidth, screenHeight, WHITE);
        EndShaderMode();

        // 3D scene — bodies and trails drawn on top of the lens background
        BeginBlendMode(BLEND_ALPHA);
        BeginMode3D(camera);

        // Invisible depth-only sphere at the photon-sphere shadow radius.
        // Writes to the depth buffer without touching the color buffer (BLANK = alpha 0),
        // so the 2D lens-shader disk remains fully visible while 3D bodies behind the
        // shadow are depth-culled and don't show through the BH.

        for (const Body& b : bodies) {
            float dx   = b.position.x - blackHole.position.x;
            float dy   = b.position.y - blackHole.position.y;
            float dz   = b.position.z - blackHole.position.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);

            float shadowSim = 2.6f * blackHole.radius;
            if (dist < shadowSim) continue;                 // never draw inside the shadow
            float fadeStart  = shadowSim * 1.6f;
            float fadeFactor = fmaxf(0.0f, fminf(1.0f, (dist - shadowSim) / (fadeStart - shadowSim)));
            float aFade      = fadeFactor;

            unsigned char rG = (unsigned char)(fadeFactor * 255);
            unsigned char rA = (unsigned char)(aFade * 255);

            // Trail
            for (int i = 1; i < (int)b.trail.size(); i++) {
                float         t     = (float)i / (float)b.trail.size();
                unsigned char alpha = (unsigned char)(t * aFade * 255);
                Color         c     = { 255, rG, 0, alpha };
                DrawLine3D(toRender(b.trail[i-1], RENDER_SCALE),
                           toRender(b.trail[i],   RENDER_SCALE), c);
            }

            // Base color
            Color bodyColor;
            if      (b.isDebris)            bodyColor = { 255, 160,  60, rA };
            else if (b.mass >= MASS_LARGE)  bodyColor = { 255, 140,   0, rA };
            else if (b.mass >= MASS_MEDIUM) bodyColor = { 255, 255, 255, rA };
            else                            bodyColor = {   0, 120, 255, rA };

            // Heat tint: lerp toward white-hot orange as body falls in
            if (b.heat > 0.0f) {
                bodyColor.r = (unsigned char)(bodyColor.r + (255 - bodyColor.r) * b.heat);
                bodyColor.g = (unsigned char)(bodyColor.g + (180 - bodyColor.g) * b.heat);
                bodyColor.b = (unsigned char)(bodyColor.b + ( 50 - bodyColor.b) * b.heat);
            }

            float scaleFactor = b.isDebris
                ? 0.35f
                : fmaxf(0.5f, 1.0f + 0.3f * log10f(b.mass / MASS_SMALL));
            float bodyR = BODY_RENDER_RADIUS * scaleFactor;

            // Disintegration: body heats and breaks into particles along its orbit.
            // approach = 0 at 3Rs (stable orbit/ISCO), = 1 at 2Rs (tidal shredding).
            float approach = (!b.isDebris && dist < 3.0f * blackHole.radius)
                ? fmaxf(0.0f, fminf(1.0f, (3.0f * blackHole.radius - dist) / blackHole.radius))
                : 0.0f;

            Vector3 renderPos = toRender(b.position, RENDER_SCALE);

            if (approach > 0.01f) {
                // Orbital velocity direction = spaghettification axis
                float vLen = sqrtf(b.velocity.x*b.velocity.x + b.velocity.y*b.velocity.y + b.velocity.z*b.velocity.z);
                float svx = vLen > 0.1f ? b.velocity.x/vLen : dx/dist;
                float svy = vLen > 0.1f ? b.velocity.y/vLen : dy/dist;
                float svz = vLen > 0.1f ? b.velocity.z/vLen : dz/dist;

                // Main body shrinks as particles peel off — gone by approach = 1
                float mainScale = fmaxf(0.05f, 1.0f - approach);
                DrawSphere(renderPos, bodyR * mainScale, bodyColor);

                // 0 → 100 particles peel off and stretch along the orbital direction
                int   nParts  = (int)(approach * 100.0f);
                float spread  = bodyR * (1.0f + 6.0f * approach);  // stream half-length
                float scatter = bodyR * 0.5f * (1.0f - approach);  // cloud collapses to line

                for (int k = 0; k < nParts; k++) {
                    float fi    = (float)k / 100.0f;
                    float along = (fi - 0.5f) * 2.0f * spread;

                    // Transverse scatter — deterministic per particle, collapses to zero
                    float ang = fi * 94.25f;                          // 15 cycles across stream
                    float sx  = sinf(ang)               * scatter;
                    float sy  = cosf(ang * 0.71f)       * scatter;
                    float sz  = sinf(ang * 1.31f + 2.1f)* scatter;
                    float sdotv = sx*svx + sy*svy + sz*svz;          // remove along-velocity component
                    sx -= sdotv*svx; sy -= sdotv*svy; sz -= sdotv*svz;

                    Vector3 partPos = {
                        renderPos.x + svx*along + sx,
                        renderPos.y + svy*along + sy,
                        renderPos.z + svz*along + sz
                    };

                    // Particles taper in size and opacity toward the tips of the stream
                    float fromCenter = fabsf(fi - 0.5f) * 2.0f;
                    float partR  = bodyR * 0.18f * (1.0f - 0.5f * fromCenter);
                    unsigned char partA = (unsigned char)(rA * (1.0f - 0.5f * fromCenter));
                    Color partColor = { bodyColor.r, bodyColor.g, bodyColor.b, partA };
                    DrawSphereEx(partPos, partR, 4, 4, partColor);
                }
            } else {
                DrawSphere(renderPos, bodyR, bodyColor);
            }
        }

        EndMode3D();
        EndBlendMode();

        // 2D overlays
        if (dragging) {
            DrawLineV(dragStartScreen, GetMousePosition(), WHITE);
            DrawCircleV(dragStartScreen, 4.0f, WHITE);
        }

        DrawFPS(10, 30);
        DrawText(TextFormat("BH: %.2e m  |  Dist: %.0f  |  Bodies: %d  |  Swallowed: %d",
                            blackHole.radius, camDistance, (int)bodies.size(), swallowedCount),
                 10, 10, 12, RAYWHITE);
        DrawText("LMB drag: launch  |  SPACE+drag: orbit  |  scroll/+/-: zoom  |  1/2/3: spawn  |  4: plunge  |  I: inclined  |  C: clear",
                 10, screenHeight - 20, 10, GRAY);
        EndDrawing();
    }

    UnloadShader(lens);
    CloseWindow();
    return 0;
}