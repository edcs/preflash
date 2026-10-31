# Stands in for ESP-IDF's project.cmake, so that a project's own CMake builds
# natively when IDF_PATH points here. It reads the project and writes only to the
# build folder: sdkconfig and managed_components/ must already be in place.

get_filename_component(PREFLASH_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PREFLASH_GEN_LV_CONFIG "${CMAKE_CURRENT_LIST_DIR}/../gen_lv_config.py")
include("${CMAKE_CURRENT_LIST_DIR}/replaced.cmake")

set(ESP_PLATFORM 1)

function(preflash_stop_stale_project problem)
  message(FATAL_ERROR "${problem}\nrun idf.py reconfigure once with ESP-IDF, then try again.")
endfunction()

# Rewriting a file with the same content would rebuild everything that uses it.
function(preflash_write_if_changed path content)
  set(existing "")
  if(EXISTS "${path}")
    file(READ "${path}" existing)
  endif()
  if(NOT existing STREQUAL content)
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

# Writes sdkconfig.h and, as ESP-IDF does, gives components each option as a
# CMake variable. A macro, so those variables land in the directory's scope.
macro(preflash_load_sdkconfig config_dir)
  set(preflash_sdkconfig "${CMAKE_SOURCE_DIR}/sdkconfig")
  if(NOT EXISTS "${preflash_sdkconfig}")
    preflash_stop_stale_project("No sdkconfig in ${CMAKE_SOURCE_DIR}.")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${preflash_sdkconfig}")

  file(STRINGS "${preflash_sdkconfig}" preflash_options REGEX "^CONFIG_[A-Za-z0-9_]+=")
  set(preflash_header "#pragma once\n")
  foreach(preflash_option IN LISTS preflash_options)
    string(REGEX MATCH "^(CONFIG_[A-Za-z0-9_]+)=(.*)$" _ "${preflash_option}")
    set(preflash_key "${CMAKE_MATCH_1}")
    set(preflash_value "${CMAKE_MATCH_2}")
    if(preflash_value STREQUAL "n")
      continue()
    endif()
    if(preflash_value STREQUAL "y")
      string(APPEND preflash_header "#define ${preflash_key} 1\n")
    else()
      string(APPEND preflash_header "#define ${preflash_key} ${preflash_value}\n")
    endif()
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" ${preflash_key} "${preflash_value}")
  endforeach()
  preflash_write_if_changed("${config_dir}/sdkconfig.h" "${preflash_header}")
endmacro()

# True when the manifest lists something the component manager would download,
# which leaves out the idf version and dependencies on local folders.
function(preflash_manifest_downloads manifest out)
  set(${out} FALSE PARENT_SCOPE)
  if(NOT EXISTS "${manifest}")
    return()
  endif()

  file(STRINGS "${manifest}" lines)
  set(in_dependencies FALSE)
  set(indent "")
  set(pending "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^dependencies:")
      set(in_dependencies TRUE)
      continue()
    elseif(line MATCHES "^[^ #]")
      set(in_dependencies FALSE)
    endif()
    if(NOT in_dependencies)
      continue()
    endif()

    if(line MATCHES "^( +)[\"']?([^ #:\"']+)[\"']?:")
      set(key_indent "${CMAKE_MATCH_1}")
      set(key "${CMAKE_MATCH_2}")
      if(NOT indent)
        set(indent "${key_indent}")
      endif()
      if(key_indent STREQUAL indent)
        if(pending)
          set(${out} TRUE PARENT_SCOPE)
          return()
        endif()
        set(pending "${key}")
        if(key STREQUAL "idf")
          set(pending "")
        endif()
      endif()
    endif()
    if(line MATCHES "path:")
      set(pending "")
    endif()
  endforeach()

  if(pending)
    set(${out} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(preflash_check_managed_components component_dirs)
  if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/managed_components")
    return()
  endif()
  foreach(dir IN LISTS component_dirs)
    preflash_manifest_downloads("${dir}/idf_component.yml" downloads)
    if(downloads)
      preflash_stop_stale_project(
        "${dir}/idf_component.yml lists dependencies, but ${CMAKE_SOURCE_DIR} has no managed_components folder.")
    endif()
  endforeach()
endfunction()

# Adds a folder that is a component, or else each component directly inside it.
function(preflash_collect_components dir out)
  set(found "${${out}}")
  if(EXISTS "${dir}/CMakeLists.txt")
    list(APPEND found "${dir}")
  else()
    file(GLOB children "${dir}/*")
    foreach(child IN LISTS children)
      if(EXISTS "${child}/CMakeLists.txt")
        list(APPEND found "${child}")
      endif()
    endforeach()
  endif()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(preflash_is_replaced name out)
  set(${out} FALSE PARENT_SCOPE)
  foreach(pattern IN LISTS PREFLASH_REPLACED_COMPONENTS)
    string(REPLACE "*" ".*" regex "${pattern}")
    if(name MATCHES "^${regex}$")
      set(${out} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# bootloader_components/ stays out, as those run before the app on real hardware
# and ESP-IDF builds them apart from it. The first folder with a name wins.
function(preflash_find_project_components out)
  set(dirs "")
  foreach(entry IN LISTS EXTRA_COMPONENT_DIRS)
    get_filename_component(entry "${entry}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${entry}")
      message(FATAL_ERROR "Folder in EXTRA_COMPONENT_DIRS doesn't exist: ${entry}")
    endif()
    preflash_collect_components("${entry}" dirs)
  endforeach()
  foreach(entry IN ITEMS main components)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${entry}")
      preflash_collect_components("${CMAKE_SOURCE_DIR}/${entry}" dirs)
    endif()
  endforeach()
  preflash_check_managed_components("${dirs}")
  if(EXISTS "${CMAKE_SOURCE_DIR}/managed_components")
    preflash_collect_components("${CMAKE_SOURCE_DIR}/managed_components" dirs)
  endif()

  set(names "")
  set(kept "")
  foreach(dir IN LISTS dirs)
    get_filename_component(name "${dir}" NAME)
    preflash_is_replaced("${name}" replaced)
    if(NOT replaced AND NOT name IN_LIST names)
      list(APPEND names "${name}")
      list(APPEND kept "${dir}")
    endif()
  endforeach()
  set(${out} "${kept}" PARENT_SCOPE)
endfunction()

# The file holds an .incbin, not the bytes, so that large assets stay cheap. The
# literal symbol names match the declarations ESP-IDF's docs tell firmware to use.
function(preflash_embed_file file text out)
  get_filename_component(path "${file}" ABSOLUTE BASE_DIR "${COMPONENT_DIR}")
  get_filename_component(filename "${path}" NAME)
  string(MAKE_C_IDENTIFIER "${filename}" name)

  set(terminator "")
  if(text)
    set(terminator "    \".byte 0\\n\"\n")
  endif()

  # The last byte keeps the end label inside the section, and .alt_entry stops
  # the linker from treating it as a separate block it could move.
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/embed/${name}.c")
  string(CONCAT source
    "__asm__(\n"
    "    \".section __TEXT,__const\\n\"\n"
    "    \".p2align 4\\n\"\n"
    "    \".globl _binary_${name}_start\\n\"\n"
    "    \"_binary_${name}_start:\\n\"\n"
    "    \".incbin \\\"${path}\\\"\\n\"\n"
    "${terminator}"
    "    \".globl _binary_${name}_end\\n\"\n"
    "    \".alt_entry _binary_${name}_end\\n\"\n"
    "    \"_binary_${name}_end:\\n\"\n"
    "    \".byte 0\\n\");\n")
  preflash_write_if_changed("${generated}" "${source}")
  set_property(SOURCE "${generated}" APPEND PROPERTY OBJECT_DEPENDS "${path}")
  set(${out} "${generated}" PARENT_SCOPE)
endfunction()

function(preflash_component_sources out)
  set(sources "")
  if(arg_SRCS)
    foreach(src IN LISTS arg_SRCS)
      get_filename_component(src "${src}" ABSOLUTE BASE_DIR "${COMPONENT_DIR}")
      list(APPEND sources "${src}")
    endforeach()
  else()
    foreach(dir IN LISTS arg_SRC_DIRS)
      get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${COMPONENT_DIR}")
      if(NOT IS_DIRECTORY "${dir}")
        message(FATAL_ERROR "SRC_DIRS entry doesn't exist: ${dir}")
      endif()
      file(GLOB found CONFIGURE_DEPENDS "${dir}/*.c" "${dir}/*.cpp" "${dir}/*.S")
      list(APPEND sources ${found})
    endforeach()
    foreach(src IN LISTS arg_EXCLUDE_SRCS)
      get_filename_component(src "${src}" ABSOLUTE BASE_DIR "${COMPONENT_DIR}")
      list(REMOVE_ITEM sources "${src}")
    endforeach()
  endif()

  foreach(file IN LISTS arg_EMBED_FILES)
    preflash_embed_file("${file}" FALSE generated)
    list(APPEND sources "${generated}")
  endforeach()
  foreach(file IN LISTS arg_EMBED_TXTFILES)
    preflash_embed_file("${file}" TRUE generated)
    list(APPEND sources "${generated}")
  endforeach()

  list(REMOVE_DUPLICATES sources)
  set(${out} "${sources}" PARENT_SCOPE)
endfunction()

# Accepts all of ESP-IDF's arguments, so none of them spills into the list before
# it. REQUIRES and PRIV_REQUIRES have no effect because every component sees every
# include folder; linker fragments, Kconfig files and target lists mean nothing on
# the host.
function(idf_component_register)
  cmake_parse_arguments(arg
    "WHOLE_ARCHIVE"
    "KCONFIG;KCONFIG_PROJBUILD"
    "SRCS;SRC_DIRS;EXCLUDE_SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;LDFRAGMENTS;REQUIRES;PRIV_REQUIRES;REQUIRED_IDF_TARGETS;EMBED_FILES;EMBED_TXTFILES"
    ${ARGN})

  set(lib "${COMPONENT_NAME}")
  set(includes "")
  foreach(dir IN LISTS arg_INCLUDE_DIRS)
    get_filename_component(dir "${dir}" ABSOLUTE BASE_DIR "${COMPONENT_DIR}")
    list(APPEND includes "${dir}")
  endforeach()
  set_property(GLOBAL APPEND PROPERTY PREFLASH_PROJECT_INCLUDES "${includes}")

  preflash_component_sources(sources)
  if(sources)
    add_library(${lib} STATIC ${sources})
    target_include_directories(${lib} PRIVATE ${includes} ${arg_PRIV_INCLUDE_DIRS})
  else()
    add_library(${lib} INTERFACE)
  endif()
  set_property(GLOBAL APPEND PROPERTY PREFLASH_COMPONENT_LIBS "${lib}")

  set(COMPONENT_LIB "${lib}" PARENT_SCOPE)
  set(COMPONENT_TARGET "${lib}" PARENT_SCOPE)
endfunction()

# Built from its sources because its own CMakeLists.txt expects the real ESP-IDF
# build, and with LV_OS_NONE because the host has no FreeRTOS for LVGL to use.
function(preflash_add_lvgl dir config_dir)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PREFLASH_GEN_LV_CONFIG}"
            "${CMAKE_SOURCE_DIR}/sdkconfig" "${config_dir}/preflash_lv_config.h"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Couldn't write the LVGL config from sdkconfig: ${result}")
  endif()

  set(COMPONENT_NAME lvgl__lvgl)
  set(COMPONENT_DIR "${dir}")
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${dir}/src/*.c")
  idf_component_register(SRCS ${sources}
                         INCLUDE_DIRS "${dir}" "${dir}/src" "${dir}/..")
endfunction()

function(preflash_add_components config_dir)
  preflash_find_project_components(component_dirs)
  foreach(dir IN LISTS component_dirs)
    get_filename_component(COMPONENT_NAME "${dir}" NAME)
    set(COMPONENT_DIR "${dir}")
    if(COMPONENT_NAME STREQUAL "lvgl__lvgl")
      preflash_add_lvgl("${dir}" "${config_dir}")
    else()
      add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/components/${COMPONENT_NAME}")
    endif()
  endforeach()
endfunction()

# Runs once every component exists, because a component can include headers from
# one that the scan reaches after it.
function(preflash_link_app config_dir)
  get_property(includes GLOBAL PROPERTY PREFLASH_PROJECT_INCLUDES)
  get_property(libs GLOBAL PROPERTY PREFLASH_COMPONENT_LIBS)
  list(APPEND includes "${PREFLASH_RUNTIME_DIR}/include" "${config_dir}")

  set(definitions ESP_PLATFORM)
  if(TARGET lvgl__lvgl)
    list(APPEND definitions LV_CONF_INCLUDE_SIMPLE
         "LV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"preflash_lv_config.h\"")
  endif()

  set(options "")
  if(NOT CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
    # CMake's release flags would turn off asserts that the firmware's build keeps.
    list(APPEND options -UNDEBUG)
  endif()

  # The target has its own name, so a component can be called app.
  add_executable(preflash_app $<TARGET_OBJECTS:preflash_runtime>)
  set_target_properties(preflash_app PROPERTIES
    OUTPUT_NAME app
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  target_link_libraries(preflash_app PRIVATE preflash_runtime)

  foreach(lib IN LISTS libs ITEMS preflash_runtime)
    get_target_property(type ${lib} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
      target_link_libraries(preflash_app PRIVATE ${lib})
      continue()
    endif()
    target_include_directories(${lib} PRIVATE ${includes})
    target_compile_definitions(${lib} PRIVATE ${definitions})
    target_compile_options(${lib} PRIVATE ${options})
    if(NOT lib STREQUAL "preflash_runtime")
      # Whole-archive, so that app_main and constructors survive. Linking the target
      # as well carries over whatever it links to itself.
      target_link_libraries(preflash_app PRIVATE ${lib})
      target_link_options(preflash_app PRIVATE "LINKER:-force_load,$<TARGET_FILE:${lib}>")
    endif()
  endforeach()
endfunction()

# Replaces CMake's project() as ESP-IDF's does, which is why a project's
# CMakeLists.txt needs nothing after this call to get its app.
macro(project name)
  # CXX_STANDARD 23 below is rejected by CMake older than 3.20, and projects may
  # declare a lower cmake_minimum_required, so stop here with a clear message.
  if(CMAKE_VERSION VERSION_LESS 3.20)
    message(FATAL_ERROR "Preflash needs CMake 3.20 or newer, found ${CMAKE_VERSION}")
  endif()
  set(PREFLASH_CONFIG_DIR "${CMAKE_BINARY_DIR}/config")
  preflash_load_sdkconfig("${PREFLASH_CONFIG_DIR}")

  # The same language levels as ESP-IDF, for code that relies on them.
  set(CMAKE_C_STANDARD 17)
  set(CMAKE_CXX_STANDARD 23)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()

  _project(${name} C CXX ASM)

  preflash_add_components("${PREFLASH_CONFIG_DIR}")
  add_subdirectory("${PREFLASH_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/preflash_runtime")
  preflash_link_app("${PREFLASH_CONFIG_DIR}")
endmacro()
