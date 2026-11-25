# The device window, as a process of its own so that it outlives the app's restarts. It's built
# beside the app, and links nothing of the runtime but the two winlink sources it shares.
add_executable(preflash_window
    main.cpp
    chrome.cpp
    layout.cpp
    link.cpp
    pad.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/winlink/frame_buffer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/winlink/protocol.cpp)
set_target_properties(preflash_window PROPERTIES
  OUTPUT_NAME preflash-window
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(preflash_window PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
target_include_directories(preflash_window SYSTEM PRIVATE ${preflash_json_SOURCE_DIR}/include)
target_link_libraries(preflash_window PRIVATE SDL2::SDL2)
