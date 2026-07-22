include(FetchContent)

set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  vst3sdk
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
  GIT_TAG v3.8.0_build_66
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  GIT_SUBMODULES base cmake pluginterfaces public.sdk)

if(EFFETUNE_BUILD_PLUGIN)
  FetchContent_MakeAvailable(vst3sdk)
  # The SDK's plug-in packaging helper reads this in the caller's directory scope.
  set(public_sdk_SOURCE_DIR "${vst3sdk_SOURCE_DIR}/public.sdk")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/choc/choc/gui/choc_WebView.h")
  message(FATAL_ERROR
    "third_party/choc is missing. Run: git submodule update --init --recursive")
endif()

add_library(effetune_choc INTERFACE)
target_include_directories(effetune_choc INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/choc")
