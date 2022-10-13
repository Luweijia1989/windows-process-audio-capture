project(get-wasapi-offsets)

add_executable(get-wasapi-offsets)

target_sources(
  get-wasapi-offsets
  PRIVATE get-wasapi-offsets.cpp
          ../wasapi-hook-info.h)

target_include_directories(get-wasapi-offsets
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
  target_compile_options(get-wasapi-offsets
                         PRIVATE "$<IF:$<CONFIG:Debug>,/MTd,/MT>")
endif()

set_target_properties(get-wasapi-offsets PROPERTIES FOLDER
                                                      "plugins/wasapi-capture")
set_target_properties(
  get-wasapi-offsets
  PROPERTIES OUTPUT_NAME
             "get-wasapi-offsets$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>"
)

set(OBS_PLUGIN_DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/wasapi-capture/")
setup_plugin_target(get-wasapi-offsets)

add_dependencies(wasapi-capture get-wasapi-offsets)

