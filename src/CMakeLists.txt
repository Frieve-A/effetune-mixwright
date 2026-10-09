# Share the worklet's display policy without maintaining a second type registry.
set(effetune_processor_source "${EFFETUNE_UPSTREAM_SOURCE_DIR}/plugins/audio-processor.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${effetune_processor_source}")
file(READ "${effetune_processor_source}" effetune_processor_text)
string(REGEX MATCH "const DISPLAY_ONLY_DSP_TYPES = new Set\\(\\[([^]]*)\\]\\);"
  effetune_display_declaration "${effetune_processor_text}")
if(NOT effetune_display_declaration)
  message(FATAL_ERROR "Unable to read upstream display-only DSP policy")
endif()
set(effetune_display_types "${CMAKE_MATCH_1}")
string(REGEX REPLACE "'[A-Za-z][A-Za-z0-9]*'|[, \t\r\n]" ""
  effetune_display_remainder "${effetune_display_types}")
if(NOT effetune_display_remainder STREQUAL "")
  message(FATAL_ERROR "Unsupported upstream display-only DSP policy syntax")
endif()
string(REPLACE "'" "\"" effetune_display_types "${effetune_display_types}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/DisplayDspPolicy.h" CONTENT
"// Generated from upstream audio-processor.js. Do not edit.
#pragma once
#include <string_view>
namespace effetune::vst {
inline constexpr std::string_view kPureDisplayDspTypes[] = {@effetune_display_types@};
}
" @ONLY)

add_library(effetune_engine STATIC
  engine/automation_catalog.cpp
  engine/automation_binding.cpp
  engine/automation_scheduler.cpp
  engine/dry_delay.cpp
  engine/engine_host.cpp
  engine/latency.cpp
  engine/output_transition.cpp
  engine/output_analyzers.cpp
  engine/spectrum_capture.cpp
  engine/pipeline_model.cpp
  engine/resampler.cpp
  bridge/sfz_library.cpp
  bridge/config_store.cpp
  bridge/backup_export.cpp
  bridge/message_router.cpp
  bridge/preset_store.cpp
  bridge/state_codec.cpp)

target_include_directories(effetune_engine
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${EFFETUNE_UPSTREAM_SOURCE_DIR}/dsp/core"
    "${EFFETUNE_UPSTREAM_SOURCE_DIR}/dsp/generated/cpp")
target_link_libraries(effetune_engine PUBLIC effetune_dsp_core effetune_choc)
target_compile_features(effetune_engine PUBLIC cxx_std_20)
if(WIN32)
  target_link_libraries(effetune_engine PUBLIC bcrypt)
endif()

if(EXISTS "${EFFETUNE_UPSTREAM_SOURCE_DIR}/dsp/include/effetune/spectrum_tap.h")
  target_sources(effetune_engine PRIVATE engine/native_spectrum_capture.cpp)
  target_compile_definitions(effetune_engine PUBLIC EFFETUNE_HAS_NATIVE_SPECTRUM_TAP=1)
endif()

if(MSVC)
  target_compile_options(effetune_engine PRIVATE /W4 /WX /utf-8)
  if(EFFETUNE_ENABLE_AVX2)
    target_compile_options(effetune_engine PRIVATE /arch:AVX2)
  endif()
else()
  target_compile_options(effetune_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(EFFETUNE_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()

if(EFFETUNE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
