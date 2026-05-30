#include <raylib.h>
#include <cmath>
struct BlackHole {
    float mass;
    float gravityConstant;
    float radius;
    float eventHorizonRadius;
    Vector2 position;
};

int main() {
    const int screenWidth = 800;
    const int screenHeight = 450;
    const int screencenterX = screenWidth / 2;
    const int screencenterY = screenHeight / 2;

    BlackHole blackHole;
    blackHole.mass = 2e30f;
    blackHole.gravityConstant = 6.67430e-11f;
    float speedOfLight = 299792458.0f;
    blackHole.radius = 2.0f * (blackHole.gravityConstant * blackHole.mass / (speedOfLight * speedOfLight));
    blackHole.eventHorizonRadius = blackHole.radius;
    blackHole.position = { (float)screencenterX, (float)screencenterY };

    // Auto-fit: event horizon starts at 15% of screen width
    const float TARGET_FRACTION = 0.15f;
    const float basePixelsPerUnit = (screenWidth * TARGET_FRACTION) / blackHole.radius;
    float zoom = 1.0f;

    InitWindow(screenWidth, screenHeight, "raylib - blackholesim");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        // Zoom: up arrow, scroll up → zoom in; down arrow, scroll down → zoom out
        float scroll = GetMouseWheelMove();
        if (IsKeyDown(KEY_UP)    || scroll > 0.0f) zoom *= 1.02f;
        if (IsKeyDown(KEY_DOWN)  || scroll < 0.0f) zoom *= 0.98f;
        // +/- as fallback
        if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))      zoom *= 1.02f;
        if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) zoom *= 0.98f;

        zoom = fminf(fmaxf(zoom, 0.01f), 1000.0f);

        float ppu = basePixelsPerUnit * zoom;

        BeginDrawing();
        ClearBackground(BLACK);
        DrawCircleLines(screencenterX, screencenterY, blackHole.eventHorizonRadius * ppu, DARKGRAY);
        DrawCircle(screencenterX, screencenterY, blackHole.radius * ppu, BLACK);
        DrawText(TextFormat("Radius: %.3e m  |  Zoom: %.2fx", blackHole.radius, zoom), 10, 10, 12, RAYWHITE);
        DrawText("Zoom: UP / DOWN arrows  |  Scroll wheel  |  + / -", 10, screenHeight - 20, 10, GRAY);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
