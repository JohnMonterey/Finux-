# The shell's guts live in a library so that tests can render widgets to an
# offscreen surface without an X server or a main().
add_library(finux_core STATIC
  gfx/image.cpp
  gfx/canvas.cpp
  gfx/effects.cpp
  text/font.cpp
  text/text_renderer.cpp
  ui/widget.cpp
  ui/glyphs.cpp
  shell/taskbar.cpp
  shell/startmenu.cpp
  shell/wallpaper.cpp
  platform/x11/x11_platform.cpp
)

target_include_directories(finux_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(finux_core PUBLIC
  blend2d::blend2d
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::FONTCONFIG
  PkgConfig::XCB
)

target_compile_options(finux_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(finux-shell shell/main.cpp)
target_link_libraries(finux-shell PRIVATE finux_core)
target_compile_options(finux-shell PRIVATE -Wall -Wextra -Wpedantic)
