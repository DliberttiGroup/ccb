cmake_minimum_required(VERSION 3.16)
project(ccb C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to an optimized build when the caller does not pick one.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Werror -Wpedantic)

# Sanitizers for the test build: cmake -S . -B build -DCCB_SANITIZE=ON
option(CCB_SANITIZE "Build with ASan+UBSan+LSan" OFF)
if(CCB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Core library: every module except the entry point.
file(GLOB CCB_CORE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM CCB_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c)
add_library(ccb_core STATIC ${CCB_CORE_SOURCES})
target_include_directories(ccb_core PUBLIC src)

add_executable(ccb src/main.c)
target_link_libraries(ccb PRIVATE ccb_core)

enable_testing()
file(GLOB CCB_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.c)
foreach(test_src ${CCB_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE ccb_core)
  # The watch-loop integration test spawns the real ccb binary: pass
  # its path as argv[1], following the fuzz harnesses' convention of
  # taking paths on argv. The test also falls back to a sibling ccb
  # next to the test executable, so a manual run still works. The
  # ctest timeout is a backstop only; the test's own waits are bounded.
  if(test_name STREQUAL "test_preview_watch_loop")
    add_test(NAME ${test_name} COMMAND ${test_name} $<TARGET_FILE:ccb>)
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
  else()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endif()
endforeach()

# --- BEGIN fuzz harnesses (worker W-B; append-only, do not edit above) ---
# Standalone fuzz harnesses for the preview parsers (tests/fuzz_preview_*.c).
# They are named fuzz_* on purpose so the test_* GLOB above does not pick
# them up twice. Each harness takes file or directory paths on argv (a
# directory contributes only its matching extension) and exits nonzero
# only on a crash/sanitizer abort; parser diagnostics are normal output.
# The ctest entries run each harness over the existing never-crash corpus.
add_executable(fuzz_preview_css tests/fuzz_preview_css.c)
target_link_libraries(fuzz_preview_css PRIVATE ccb_core)
add_test(NAME fuzz_preview_css
  COMMAND fuzz_preview_css ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/preview)

add_executable(fuzz_preview_flow tests/fuzz_preview_flow.c)
target_link_libraries(fuzz_preview_flow PRIVATE ccb_core)
add_test(NAME fuzz_preview_flow
  COMMAND fuzz_preview_flow ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/preview)
# --- END fuzz harnesses (worker W-B) ---
