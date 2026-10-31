# Static so that a test suite can link the runtime with a main() of its own.
# New sources are picked up by dropping them under src/.
file(GLOB_RECURSE PREFLASH_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
add_library(preflash_runtime STATIC ${PREFLASH_RUNTIME_SOURCES})
