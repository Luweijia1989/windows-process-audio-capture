project(wasapi-capture)

add_library(wasapi-capture MODULE)
add_library(OBS::wasapi-capture ALIAS wasapi-capture)

target_sources(
  wasapi-capture
  PRIVATE plugin-main.c
          nt-stuff.h
	      hook-helpers.h
	      wasapi-hook-info.h
	      app-helpers.h
	      app-helpers.c
	      audio-channel.h
	      audio-channel.c
	      wasapi-capture.h
	      wasapi-capture.c
          windows-helpers.cpp
          ../../libobs/util/windows/obfuscate.c
          ../../libobs/util/windows/obfuscate.h)

target_link_libraries(wasapi-capture PRIVATE OBS::libobs)

set_target_properties(wasapi-capture PROPERTIES FOLDER "plugins/wasapi-capture")

if(MSVC)
  target_link_libraries(wasapi-capture PRIVATE OBS::w32-pthreads)
endif()

target_compile_definitions(
  wasapi-capture PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
                      _CRT_NONSTDC_NO_WARNINGS)

set_property(GLOBAL APPEND PROPERTY OBS_MODULE_LIST "wasapi-capture")

setup_plugin_target(wasapi-capture)

add_subdirectory(wasapi-hook)
add_subdirectory(get-wasapi-offsets)
add_subdirectory(inject-helper)
