# The chip models with a main of their own, which preflash check builds on first use. The same
# sources as firmware gets, so what's checked is what firmware runs against.
cmake_minimum_required(VERSION 3.20)
project(preflash_replay C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
# main() is here instead. The window needs SDL and the LVGL port and the BSP need a project's LVGL.
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${RUNTIME_DIR}/src/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/src/host/(main|window)\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/src/display/lvgl_port\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/src/bsp/bsp\\.cpp$")

add_executable(replay main.cpp ${RUNTIME_SOURCES})
target_include_directories(replay PRIVATE ${RUNTIME_DIR}/include)
# Firmware gets these from sdkconfig.h, and there's no firmware here to keep to them.
target_compile_definitions(replay PRIVATE CONFIG_FREERTOS_HZ=1000 CONFIG_ESP_TASK_WDT_INIT=1
                           CONFIG_ESP_TASK_WDT_TIMEOUT_S=5)
find_package(CURL REQUIRED)
target_link_libraries(replay PRIVATE CURL::libcurl)

# The same download as the runtime's, from the same cache.
include(FetchContent)
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
target_include_directories(replay SYSTEM PRIVATE ${preflash_json_SOURCE_DIR}/include)
