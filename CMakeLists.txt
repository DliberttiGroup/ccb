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
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
