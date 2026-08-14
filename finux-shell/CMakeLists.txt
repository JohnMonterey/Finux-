cmake_minimum_required(VERSION 3.20)
project(finux-shell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FINUX_BUILD_TESTS "Build the pixel-diff and unit tests" ON)

# Blend2D's asmjit JIT backend is OFF by default, and that is not arbitrary:
# on CMake 3.28 configuring asmjit together with a JIT-enabled Blend2D sends
# CheckCXXCompilerFlag into unbounded recursion (raising
# CMAKE_MAXIMUM_RECURSION_DEPTH just turns the error into a segfault). Either
# half configures fine alone, so this is an upstream interaction bug.
# The portable reference pipeline is comfortably fast enough for shell
# chrome. Flip this on once upstream is fixed and measure before caring.
option(FINUX_BLEND2D_JIT "Use Blend2D's asmjit JIT pipeline (see comment)" OFF)

# ---------------------------------------------------------------------------
# Rasterizer: Blend2D.
#
# Deliberately not GTK/Qt. The whole fidelity argument rests on owning every
# pixel we put on screen; a theme engine that owns widget metrics cannot be
# talked into Windows 10's geometry. See docs/DESIGN.md.
#
# Pinned to an exact commit, not a branch: Blend2D has no release tags and
# master recently renamed its entire public API from camelCase to snake_case
# (writeToFile -> write_to_file, ...). Tracking master would break this tree
# under anyone who cloned it on a different day.
# ---------------------------------------------------------------------------
set(FINUX_BLEND2D_COMMIT "6dbc2cefbc996379e07104e34519a440b49b15d7"
    CACHE STRING "Blend2D commit to build against")

include(FetchContent)
set(BLEND2D_STATIC TRUE CACHE BOOL "" FORCE)
if(FINUX_BLEND2D_JIT)
  set(ASMJIT_STATIC TRUE CACHE BOOL "" FORCE)
  FetchContent_Declare(asmjit
    GIT_REPOSITORY https://github.com/asmjit/asmjit.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(asmjit)
else()
  set(BLEND2D_NO_JIT TRUE CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(blend2d
  GIT_REPOSITORY https://github.com/blend2d/blend2d.git
  GIT_TAG        ${FINUX_BLEND2D_COMMIT})
FetchContent_MakeAvailable(blend2d)

# Blend2D's public headers use anonymous structs and other extensions that
# -Wpedantic objects to. Consume them as system headers so our own warnings
# stay readable instead of being buried under hundreds of upstream ones.
if(TARGET blend2d)
  get_target_property(_blend2d_includes blend2d INTERFACE_INCLUDE_DIRECTORIES)
  if(_blend2d_includes)
    set_target_properties(blend2d PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_blend2d_includes}")
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE  REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ  REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET
  xcb xcb-ewmh xcb-icccm xcb-image xcb-shm xcb-randr xcb-composite)

add_subdirectory(src)
add_subdirectory(tools)
if(FINUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "finux-shell ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  blend2d JIT: ${FINUX_BLEND2D_JIT}")
message(STATUS "  tests      : ${FINUX_BUILD_TESTS}")
message(STATUS "")
