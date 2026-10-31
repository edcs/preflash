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

add_executable(runtime_tests ${RUNTIME_SOURCES} ${TEST_SOURCES})
target_include_directories(runtime_tests PRIVATE ${RUNTIME_DIR}/include)
target_link_libraries(runtime_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json)

enable_testing()
include(GoogleTest)
gtest_discover_tests(runtime_tests)
