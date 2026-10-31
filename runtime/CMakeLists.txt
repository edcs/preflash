# Static so that a test suite can link the runtime with a main() of its own.
add_library(preflash_runtime STATIC
    src/host/main.cpp)
