cmake_minimum_required(VERSION 3.20)
project(bh_sim LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")

find_package(raylib REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(bh_sim main.cpp)

target_link_libraries(bh_sim PRIVATE raylib OpenGL::GL)
