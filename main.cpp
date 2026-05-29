#include <iostream>
#include <fstream>
#include <vector>

// 1. Math Library
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 2. Graphics Infrastructure (Order Matters: GLEW must come before GLFW)
#include <GL/glew.h>
#include <GLFW/glfw3.h>

// 3. Utilities
#include <toml++/toml.hpp>
#include <fmt/core.h>

int main() {
    fmt::print("bh_sim starting\n");
    return 0;
}
