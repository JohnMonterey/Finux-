# Development aids, not part of the shipped shell.
add_executable(finux-testwindow testwindow.cpp)
target_link_libraries(finux-testwindow PRIVATE PkgConfig::XCB)
target_compile_options(finux-testwindow PRIVATE -Wall -Wextra -Wpedantic)
