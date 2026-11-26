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

# Static release binary: cmake --preset release (or -DCCB_STATIC_RELEASE=ON).
# Links only the ccb executable with -static and strips it. Dev and
# sanitizer builds stay dynamic and unstripped. ASan cannot link
# statically, so the two options are mutually exclusive.
option(CCB_STATIC_RELEASE "Link ccb statically and strip it (release only)" OFF)
if(CCB_STATIC_RELEASE AND CCB_SANITIZE)
  message(FATAL_ERROR "CCB_STATIC_RELEASE and CCB_SANITIZE are mutually exclusive: ASan does not work with a static binary")
endif()

# Core library: every module except the entry point.
file(GLOB CCB_CORE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM CCB_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c)
add_library(ccb_core STATIC ${CCB_CORE_SOURCES})
target_include_directories(ccb_core PUBLIC src)

add_executable(ccb src/main.c)
target_link_libraries(ccb PRIVATE ccb_core)
if(CCB_STATIC_RELEASE)
  target_link_options(ccb PRIVATE -static)
  add_custom_command(TARGET ccb POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:ccb>
    COMMENT "Stripping release binary")
endif()

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
  elseif(test_name STREQUAL "test_dex_contents")
    # Runs two real `ccb build`s (kotlinc + d8 on the stdlib).
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 600)
  elseif(test_name STREQUAL "test_dexcheck")
    # Runs three real `ccb build`s (hello, hello --aab, negative)
    # plus kotlinc/d8 subprocesses.
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 900)
  elseif(test_name STREQUAL "test_r8_minify")
    # Runs two real `ccb build`s (debug + release R8 on the stdlib).
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 600)
  elseif(test_name STREQUAL "test_kotlinc_daemon")
    # Runs four real `ccb build`s (cold + warm daemon builds, stale
    # daemon rebuild, clean) plus a stop poll bounded by the 180 s
    # idle timeout; the live half takes a few minutes, longer under
    # ASan (the post-kotlinc pipeline alone is ~95 s there).
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 900)
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

# Test labels: speed (fast/slow) + area. Every test gets exactly one of
# each. `ctest -L fast` is the quick gate (no real Android build, seconds);
# `ctest -L <area>` runs one area; plain `ctest` runs the full suite.
# fast counts: config 2, setup 2, pipeline 5, preview 29, misc 5 (= 43).
# slow counts: pipeline 3, dexcheck 1, daemon 1, maven 1, modules 1 (= 7).
set_tests_properties(test_config test_toml
  PROPERTIES LABELS "fast;config")
set_tests_properties(test_sdk_tools test_setup
  PROPERTIES LABELS "fast;setup")
set_tests_properties(test_bundle test_cache test_d8_heap test_manifest_merge
  test_probes
  PROPERTIES LABELS "fast;pipeline")
set_tests_properties(test_preview test_preview_cli_gate_diff
  test_preview_correctness_a test_preview_correctness_b
  test_preview_correctness_c test_preview_css test_preview_diff
  test_preview_fidelity test_preview_flags test_preview_flow test_preview_gate
  test_preview_handlers test_preview_lint test_preview_lists test_preview_lsp
  test_preview_phase2 test_preview_resources test_preview_scenario
  test_preview_scope test_preview_slices23 test_preview_storyboard
  test_preview_trace test_preview_tree test_preview_walk test_preview_watch
  test_preview_watch_loop test_preview_width fuzz_preview_css fuzz_preview_flow
  PROPERTIES LABELS "fast;preview")
set_tests_properties(test_hash test_json test_proc test_str test_zip
  PROPERTIES LABELS "fast;misc")
set_tests_properties(test_aab test_dex_contents test_r8_minify
  PROPERTIES LABELS "slow;pipeline")
set_tests_properties(test_dexcheck PROPERTIES LABELS "slow;dexcheck")
set_tests_properties(test_kotlinc_daemon PROPERTIES LABELS "slow;daemon")
set_tests_properties(test_maven PROPERTIES LABELS "slow;maven")
set_tests_properties(test_modules PROPERTIES LABELS "slow;modules")
