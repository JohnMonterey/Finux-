add_library(finux_pixdiff STATIC pixdiff/pixdiff.cpp)
target_include_directories(finux_pixdiff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(finux_pixdiff PUBLIC finux_core)

set(FINUX_REFERENCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pixdiff/references")

function(finux_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE finux_core finux_pixdiff)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE
    FINUX_REFERENCE_DIR="${FINUX_REFERENCE_DIR}")
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${name} COMMAND ${name})
  # A test that cannot run (no fonts, no display) reports as skipped rather
  # than passing, so an under-provisioned CI box cannot show all green.
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

finux_add_test(test_geometry)
finux_add_test(test_pixdiff)
finux_add_test(test_canvas_interleave)
finux_add_test(test_text)
finux_add_test(test_taskbar_render)

# End-to-end check against a real window manager. Covers strut reservation and
# window placement, which offscreen rendering tests cannot see.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME x11_smoke
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration/x11_smoke.py
            $<TARGET_FILE:finux-shell>
            $<TARGET_FILE:finux-testwindow>)
  set_tests_properties(x11_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
