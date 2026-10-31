cmake_minimum_required(VERSION 3.20)
project(preflash_runtime_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest json)

get_filename_component(RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime ABSOLUTE)

# A new runtime source or test file is picked up by dropping it in place.
# main() lives in host/main.cpp, which the suite replaces with gtest_main.
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${RUNTIME_DIR}/src/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/src/host/main\\.cpp$")
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)

# The runtime is compiled once and shared by the tests and the restart helper.
# Firmware gets its tick rate from sdkconfig.h; the suite has none, so it says so here.
add_library(runtime_objs OBJECT ${RUNTIME_SOURCES})
target_include_directories(runtime_objs PUBLIC ${RUNTIME_DIR}/include)
target_compile_definitions(runtime_objs PUBLIC CONFIG_FREERTOS_HZ=1000)
target_link_libraries(runtime_objs PUBLIC nlohmann_json::nlohmann_json)

# Restarting re-executes the binary, so the test needs one it can afford to lose.
add_executable(restart_helper helpers/restart_helper.cpp)
target_link_libraries(restart_helper PRIVATE runtime_objs)

add_executable(runtime_tests ${TEST_SOURCES})
target_link_libraries(runtime_tests PRIVATE runtime_objs GTest::gtest_main)
target_compile_definitions(runtime_tests PRIVATE RESTART_HELPER="$<TARGET_FILE:restart_helper>")
add_dependencies(runtime_tests restart_helper)

enable_testing()
include(GoogleTest)
gtest_discover_tests(runtime_tests)
