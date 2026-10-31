# Static so that a test suite can link the runtime with a main() of its own.
# Each folder under src/ is listed once its sources build in a firmware build;
# src/host holds more than main(), which needs libraries a project doesn't have.
file(GLOB PREFLASH_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/os/*.cpp)
add_library(preflash_runtime STATIC src/host/main.cpp ${PREFLASH_RUNTIME_SOURCES})
