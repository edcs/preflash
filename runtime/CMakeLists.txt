# Static so that a test suite can link the runtime with a main() of its own.
# Each folder under src/ is listed once its sources build in a firmware build.
file(GLOB PREFLASH_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/os/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/storage/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/console/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/display/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/bsp/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/bus/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/chips/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/host/*.cpp)

# A project without LVGL still gets the panel. The port and the BSP compile against
# the project's own LVGL, so they need one.
get_property(PREFLASH_HAS_LVGL GLOBAL PROPERTY PREFLASH_HAS_LVGL)
if(NOT PREFLASH_HAS_LVGL)
  list(REMOVE_ITEM PREFLASH_RUNTIME_SOURCES
      ${CMAKE_CURRENT_LIST_DIR}/src/display/lvgl_port.cpp
      ${CMAKE_CURRENT_LIST_DIR}/src/bsp/bsp.cpp)
endif()

add_library(preflash_runtime STATIC ${PREFLASH_RUNTIME_SOURCES})

find_package(SDL2 CONFIG REQUIRED)
# The app that links the runtime is defined a directory up, and must see the target too.
set_target_properties(SDL2::SDL2 PROPERTIES IMPORTED_GLOBAL TRUE)
target_link_libraries(preflash_runtime PRIVATE SDL2::SDL2)

# Only the headers are used, so the folder named here doesn't exist, which leaves
# its build out of it. The cache spares a download for every clean build folder.
include(FetchContent)
# CMake 3.24 warns unless the extract timestamp is chosen, and older ones don't know the option.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(preflash_extract_timestamp DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
FetchContent_Declare(preflash_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_DIR "$ENV{HOME}/Library/Caches/preflash"
    ${preflash_extract_timestamp}
    SOURCE_SUBDIR no-cmake-build)
FetchContent_MakeAvailable(preflash_json)
target_include_directories(preflash_runtime SYSTEM PRIVATE ${preflash_json_SOURCE_DIR}/include)
