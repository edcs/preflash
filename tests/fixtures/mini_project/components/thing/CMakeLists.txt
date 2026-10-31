if(ESP_PLATFORM)
    idf_component_register(SRCS "thing.c" "startup.c"
                           INCLUDE_DIRS "include"
                           REQUIRES esp_timer
                           EMBED_FILES "../../assets/hello.txt"
                           EMBED_TXTFILES "../../assets/notes.txt")
    target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
else()
    message(FATAL_ERROR "ESP_PLATFORM is not set")
endif()
