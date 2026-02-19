cmake_minimum_required(VERSION 3.16)
project(ab3d1 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------
# Data: ensure data/pal exists and copy from amiga/pal if present
# -----------------------------------------------------------------------
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/pal")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/amiga/pal")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/amiga/pal" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()

# -----------------------------------------------------------------------
# SDL2 (auto-download via FetchContent)
# -----------------------------------------------------------------------
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.11
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL2)

# All source files
set(SOURCES
    src/main.c
    src/game_state.c
    src/game_data.c
    src/math_tables.c
    src/ai.c
    src/movement.c
    src/level.c
    src/objects.c
    src/visibility.c
    src/control_loop.c
    src/game_loop.c
    src/player.c
    src/renderer.c
    src/stub_display.c
    src/stub_input.c
    src/stub_audio.c
    src/stub_io.c
    src/sb_decompress.c
    src/sprite_palettes.c
)

add_executable(ab3d1 ${SOURCES})

target_include_directories(ab3d1 PRIVATE src)

# Generate sprite_palettes_data.h from data/pal/*.pal (no runtime .pal load)
set(PAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/pal")
set(PAL_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/sprite_palettes_data.h")
if(EXISTS "${PAL_DIR}")
  find_package(Python3 QUIET)
  if(Python3_FOUND)
    execute_process(
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pal_to_header.py"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE PAL_RESULT
      OUTPUT_VARIABLE PAL_OUT
      ERROR_VARIABLE PAL_ERR
    )
    if(PAL_RESULT EQUAL 0)
      message(STATUS "Sprite palettes: ${PAL_OUT}")
    else()
      message(WARNING "Sprite palette generation failed: ${PAL_ERR}")
    endif()
  endif()
endif()
if(NOT EXISTS "${PAL_HEADER}")
  message(FATAL_ERROR "sprite_palettes_data.h not found. Put .pal files in data/pal and run: python tools/pal_to_header.py")
endif()

# Link SDL2 (no SDL2main - we use CONSOLE subsystem for printf output)
target_link_libraries(ab3d1 PRIVATE SDL2-static)

# Platform-specific settings
if(WIN32)
    # Windows: link winmm for SDL2
    target_link_libraries(ab3d1 PRIVATE winmm imm32 version setupapi)
elseif(UNIX)
    target_link_libraries(ab3d1 PRIVATE m)
endif()

# Debug build by default for development
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Force CONSOLE subsystem on Windows (override SDL2's WINDOWS subsystem)
if(MSVC)
    target_link_options(ab3d1 PRIVATE /SUBSYSTEM:CONSOLE)
endif()

# Warnings
if(MSVC)
    target_compile_options(ab3d1 PRIVATE /W4)
    target_compile_definitions(ab3d1 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ab3d1 PRIVATE -Wall -Wextra -Wpedantic)
endif()
