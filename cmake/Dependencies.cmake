include(FetchContent)

set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  vst3sdk
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
  GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96 # v3.8.1_build_84
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
if(WIN32 AND EFFETUNE_BUILD_PLUGIN)
  find_program(EFFETUNE_NODE_EXECUTABLE node REQUIRED)
  set(effetune_webview_include "${CMAKE_BINARY_DIR}/webview-include")
  set(effetune_choc_webview
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/choc/choc/gui/choc_WebView.h")
  set(effetune_webview_header_generator
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-choc-webview-header.mjs")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${effetune_choc_webview}" "${effetune_webview_header_generator}")
  execute_process(
    COMMAND "${EFFETUNE_NODE_EXECUTABLE}" "${effetune_webview_header_generator}"
            --source "${effetune_choc_webview}"
            --out "${effetune_webview_include}/choc/gui/choc_WebView.h"
    RESULT_VARIABLE effetune_webview_header_result
    ERROR_VARIABLE effetune_webview_header_error)
  if(NOT effetune_webview_header_result EQUAL 0)
    message(FATAL_ERROR "Preparing CHOC WebView failed: ${effetune_webview_header_error}")
  endif()
  target_include_directories(effetune_choc INTERFACE "${effetune_webview_include}")
endif()
target_include_directories(effetune_choc INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/choc")
