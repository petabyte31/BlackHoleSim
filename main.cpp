// Black Hole Cinema — pure gravitational lensing mode.
// No physics bodies. Frame loop: UPDATE camera → DRAW fullscreen shader.

#include <raylib.h>
#include <raymath.h>
#include <cmath>

int main() {
    const int screenWidth  = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "Black Hole Cinema");
    SetTargetFPS(60);

    // ── Black hole (SI units) ─────────────────────────────────────────────────
    const float G       = 6.67430e-11f;
    const float C       = 299792458.0f;
    const float BH_MASS = 8e30f;
    const float rs_SI   = 2.0f * G * BH_MASS / (C * C);   // Schwarzschild radius, metres

    // Map rs to 15 % of screenWidth for a nicely sized shadow
    const float RENDER_SCALE = (screenWidth * 0.15f) / rs_SI;
    const float rsRender     = rs_SI   * RENDER_SCALE;
    const float diskInner    = rsRender * 1.5f;
    const float diskOuter    = rsRender * 4.5f;

    // ── Camera (spherical) ────────────────────────────────────────────────────
    Camera3D camera   = {};
    camera.target     = { 0.0f, 0.0f, 0.0f };
    camera.up         = { 0.0f, 1.0f, 0.0f };
    camera.fovy       = 50.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    float camAzimuth   =  0.6f;
    float camElevation =  0.4f;
    float camDistance  = 780.0f;

    // ── Lens shader ───────────────────────────────────────────────────────────
    Shader lens = LoadShader(0, "lens.fs");

    int lensResLoc      = GetShaderLocation(lens, "resolution");
    int lensTimeLoc     = GetShaderLocation(lens, "time");
    int lensFwdLoc      = GetShaderLocation(lens, "camForward");
    int lensRightLoc    = GetShaderLocation(lens, "camRight");
    int lensUpLoc       = GetShaderLocation(lens, "camUp");
    int lensFovLoc      = GetShaderLocation(lens, "fovY");
    int lensCamPosLoc   = GetShaderLocation(lens, "camPos");
    int lensBhPosLoc    = GetShaderLocation(lens, "bhPos");
    int lensRsLoc       = GetShaderLocation(lens, "rs");
    int lensDiskInnerLoc= GetShaderLocation(lens, "diskInner");
    int lensDiskOuterLoc= GetShaderLocation(lens, "diskOuter");
    int lensDiskBoostLoc= GetShaderLocation(lens, "diskBoost");

    // These never change while running
    Vector3 bhPos    = { 0.0f, 0.0f, 0.0f };
    float   boost    = 1.0f;
    SetShaderValue(lens, lensBhPosLoc,    &bhPos,     SHADER_UNIFORM_VEC3);
    SetShaderValue(lens, lensRsLoc,       &rsRender,  SHADER_UNIFORM_FLOAT);
    SetShaderValue(lens, lensDiskInnerLoc, &diskInner, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lens, lensDiskOuterLoc, &diskOuter, SHADER_UNIFORM_FLOAT);
    SetShaderValue(lens, lensDiskBoostLoc, &boost,     SHADER_UNIFORM_FLOAT);

    while (!WindowShouldClose()) {

        // ── UPDATE — camera orbit ─────────────────────────────────────────────
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && IsKeyDown(KEY_SPACE)) {
            Vector2 delta  = GetMouseDelta();
            camAzimuth    -= delta.x * 0.005f;
            camElevation  += delta.y * 0.005f;
            camElevation   = fmaxf(-1.5f, fminf(1.5f, camElevation));
        }
        float scroll = GetMouseWheelMove();
        camDistance  = fmaxf(200.0f, fminf(3000.0f, camDistance - scroll * 30.0f));
        if (IsKeyDown(KEY_UP))    camDistance = fmaxf(200.0f, camDistance - 5.0f);
        if (IsKeyDown(KEY_DOWN))  camDistance = fminf(3000.0f, camDistance + 5.0f);
        if (IsKeyDown(KEY_LEFT))  camAzimuth -= 0.01f;
        if (IsKeyDown(KEY_RIGHT)) camAzimuth += 0.01f;

        camera.position = {
            camDistance * cosf(camElevation) * sinf(camAzimuth),
            camDistance * sinf(camElevation),
            camDistance * cosf(camElevation) * cosf(camAzimuth)
        };

        // ── DRAW — fullscreen lens shader only ────────────────────────────────
        float now = (float)GetTime();

        Vector3 camFwd    = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 camRight  = Vector3Normalize(Vector3CrossProduct(camFwd, camera.up));
        Vector3 camUp     = Vector3CrossProduct(camRight, camFwd);
        float   fovYrad   = camera.fovy * DEG2RAD;

        float resolution[2] = { (float)GetRenderWidth(), (float)GetRenderHeight() };

        SetShaderValue(lens, lensResLoc,    resolution,       SHADER_UNIFORM_VEC2);
        SetShaderValue(lens, lensTimeLoc,   &now,             SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensFwdLoc,    &camFwd,          SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensRightLoc,  &camRight,        SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensUpLoc,     &camUp,           SHADER_UNIFORM_VEC3);
        SetShaderValue(lens, lensFovLoc,    &fovYrad,         SHADER_UNIFORM_FLOAT);
        SetShaderValue(lens, lensCamPosLoc, &camera.position, SHADER_UNIFORM_VEC3);

        BeginDrawing();
            BeginShaderMode(lens);
                DrawRectangle(0, 0, GetRenderWidth(), GetRenderHeight(), WHITE);
            EndShaderMode();

            DrawFPS(10, 30);
        EndDrawing();
    }

    UnloadShader(lens);
    CloseWindow();
    return 0;
}
