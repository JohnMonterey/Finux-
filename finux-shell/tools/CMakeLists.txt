# Development aids, not part of the shipped shell.
add_executable(finux-testwindow testwindow.cpp)
target_link_libraries(finux-testwindow PRIVATE PkgConfig::XCB)
target_compile_options(finux-testwindow PRIVATE -Wall -Wextra -Wpedantic)

# Renders the full shell to a PNG for side-by-side comparison with a reference
# screenshot. Deterministic: the clock is injected, not read.
add_executable(finux-render-desktop render_desktop.cpp)
target_link_libraries(finux-render-desktop PRIVATE finux_core)
target_compile_options(finux-render-desktop PRIVATE -Wall -Wextra -Wpedantic)
