cmake_minimum_required(VERSION 3.20)
project(bh_sim LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew on Apple Silicon
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")

# --- Dependencies ---
find_package(glm          REQUIRED)
find_package(glfw3        REQUIRED)
find_package(GLEW         REQUIRED)
find_package(fmt          REQUIRED)
find_package(tomlplusplus REQUIRED)

# --- Executable ---
add_executable(bh_sim main.cpp)

target_link_libraries(bh_sim PRIVATE
    GLEW::GLEW
    glfw
    glm::glm
    fmt::fmt
    tomlplusplus::tomlplusplus
    "-framework OpenGL"
)
