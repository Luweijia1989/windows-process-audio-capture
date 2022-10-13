project(wasapi-hook)

find_package(Detours REQUIRED)

add_library(wasapi-hook MODULE)
add_library(OBS::wasapi-hook ALIAS wasapi-hook)

target_sources(
  wasapi-hook
  PRIVATE circlebuf.h
          wasapi-hook.h
          wasapi-hook.c
          wasapi_capturer.h
          wasapi_capturer.cpp
          ../wasapi-hook-info.h
          ../../../libobs/util/windows/obfuscate.c
          ../../../libobs/util/windows/obfuscate.h)

target_include_directories(wasapi-hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(wasapi-hook PRIVATE Detours::Detours)
target_link_options(wasapi-hook PRIVATE "LINKER:/IGNORE:4099")

if(MSVC)
  target_compile_options(wasapi-hook PRIVATE "$<IF:$<CONFIG:Debug>,/MTd,/MT>")
endif()

set_target_properties(
  wasapi-hook
  PROPERTIES FOLDER "plugins/wasapi-capture"
             OUTPUT_NAME
             "wasapi-hook$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,64,32>")

set(OBS_PLUGIN_DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/wasapi-capture/")
setup_plugin_target(wasapi-hook)

add_dependencies(wasapi-capture wasapi-hook)
