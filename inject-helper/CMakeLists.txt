project(wasapi-inject-helper)

add_executable(wasapi-inject-helper)

target_sources(
  wasapi-inject-helper
  PRIVATE inject-helper.c
          ../../win-capture/inject-library.c
          ../../win-capture/inject-library.h
          ../../../libobs/util/windows/obfuscate.c
          ../../../libobs/util/windows/obfuscate.h)

target_include_directories(wasapi-inject-helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
  target_compile_options(wasapi-inject-helper PRIVATE "$<IF:$<CONFIG:Debug>,/MTd,/MT>")
endif()

set_target_properties(
  wasapi-inject-helper
  PROPERTIES FOLDER "plugins/wasapi-capture"
             OUTPUT_NAME
             "wasapi-inject-helper$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>")

set(OBS_PLUGIN_DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/wasapi-capture/")
setup_plugin_target(wasapi-inject-helper)

add_dependencies(wasapi-capture wasapi-inject-helper)
