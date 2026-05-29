#include <raylib.h>
struct BlackHole {
    float mass;
    float gravityConstant;
    float radius;
    float eventHorizonRadius;
    Vector2 position; // It's great to store its location here too!
};

int main() {
    const int  screencenterX = GetScreenWidth() / 2.0f;
    const int screencenterY = GetScreenHeight() / 2.0f;
    const int screenWidth = 800;
    const int screenHeight = 450;
    const float WORLD_WIDTH = 100.0f; 
    const float pixelsperunit = (float)screenWidth / WORLD_WIDTH ; // Scale factor for rendering
    BlackHole blackHole;
    blackHole.mass = 5.0e15f; // Arbitrary mass
    blackHole.gravityConstant = 6.67430e-11f; // Gravitational constant
    float speedOfLight = 299792458.0f;
     blackHole.radius = 2.0f * (blackHole.gravityConstant * blackHole.mass / (speedOfLight * speedOfLight)); // Schwarzschild radius
    blackHole.eventHorizonRadius = blackHole.radius; // For a non-rotating
    InitWindow(screenWidth, screenHeight, "raylib - blackholesim");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(BLACK);
        DrawText("Hello,Black Hole!", 190, 200, 20, WHITE);
        EndDrawing();
    }

    CloseWindow();

    return 0;
}
