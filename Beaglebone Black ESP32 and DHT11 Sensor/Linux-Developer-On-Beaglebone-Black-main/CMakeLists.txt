cmake_minimum_required(VERSION 3.10)
project(lvglsim)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -I. -D_GNU_SOURCE")

# 1. Find LVGL Library Sources (Core, Demos, Examples)
file(GLOB_RECURSE LVGL_SOURCES lvgl/src/*.c lvgl/demos/*.c lvgl/examples/*.c)

# 2. Find Driver & Helper Sources (The missing part!)
# This includes driver_backends.c, fbdev.c, evdev.c, etc.
file(GLOB_RECURSE DRIVER_SOURCES src/lib/*.c)

# 3. Add Include Directories
include_directories(
    .
    lvgl
    src/lib
)

# 4. Create Executable
add_executable(lvglsim src/main.c ${LVGL_SOURCES} ${DRIVER_SOURCES})

# 5. Link Libraries
target_link_libraries(lvglsim PRIVATE m pthread)

# 6. Definitions
target_compile_definitions(lvglsim PRIVATE LV_CONF_INCLUDE_SIMPLE)
