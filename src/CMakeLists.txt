add_library(effetune_engine STATIC
  engine/block_adapter.cpp
  engine/dry_delay.cpp
  engine/engine_host.cpp
  engine/latency.cpp
  engine/output_transition.cpp
  engine/pipeline_model.cpp
  engine/resampler.cpp
  bridge/config_store.cpp
  bridge/message_router.cpp
  bridge/preset_store.cpp
  bridge/state_codec.cpp)

target_include_directories(effetune_engine
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${PROJECT_SOURCE_DIR}/external/effetune/dsp/core")
target_link_libraries(effetune_engine PUBLIC effetune_dsp_core effetune_choc)
target_compile_features(effetune_engine PUBLIC cxx_std_20)

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
