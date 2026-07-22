set(effetune_plugin_sources
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
  external_url.cpp
  external_url.h
  factory.cpp
  native_file_dialog.cpp
  native_file_dialog.h
  editor_size.h
  plugin_view.cpp
  plugin_view.h
  plugin_processor.cpp
  plugin_processor.h
  plugin_ids.h
  ../bridge/webview_host.cpp
  ../bridge/webview_host.h
  version.h)

smtg_add_vst3plugin(effetune_plugin
  PACKAGE_NAME "${EFFETUNE_PLUGIN_PACKAGE_NAME}"
  SOURCES_LIST ${effetune_plugin_sources})

find_program(EFFETUNE_NODE_EXECUTABLE node REQUIRED)
set(effetune_webview_assets "${CMAKE_BINARY_DIR}/webview-assets")
file(GLOB_RECURSE effetune_ui_source_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${PROJECT_SOURCE_DIR}/external/effetune/effetune.html"
  "${PROJECT_SOURCE_DIR}/external/effetune/effetune.css"
  "${PROJECT_SOURCE_DIR}/external/effetune/js/*"
  "${PROJECT_SOURCE_DIR}/external/effetune/plugins/*"
  "${PROJECT_SOURCE_DIR}/external/effetune/presets/*"
  "${PROJECT_SOURCE_DIR}/external/effetune/images/*")
add_custom_command(
  OUTPUT "${effetune_webview_assets}/.stamp"
  COMMAND "${EFFETUNE_NODE_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tools/build-ui-assets.mjs"
          --source "${PROJECT_SOURCE_DIR}/external/effetune"
          --out "${effetune_webview_assets}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/tools/build-ui-assets.mjs"
    "${PROJECT_SOURCE_DIR}/ui-shim/vst-bootstrap.js"
    "${PROJECT_SOURCE_DIR}/ui-shim/vst-audio-manager.js"
    "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
    ${effetune_ui_source_files}
  COMMENT "Preparing EffeTune WebView assets"
  VERBATIM)
add_custom_target(effetune_ui_assets DEPENDS "${effetune_webview_assets}/.stamp")
add_dependencies(effetune_plugin effetune_ui_assets)

target_link_libraries(effetune_plugin PRIVATE sdk effetune_engine)
target_compile_features(effetune_plugin PRIVATE cxx_std_20)
set_target_properties(effetune_plugin PROPERTIES OUTPUT_NAME "${EFFETUNE_PLUGIN_PACKAGE_NAME}")
smtg_target_configure_version_file(effetune_plugin)

set(effetune_bundle_resources
    "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/${EFFETUNE_PLUGIN_PACKAGE_NAME}.vst3/Contents/Resources")
set(effetune_bundle_asset_stamp "${effetune_bundle_resources}/.webview-assets.stamp")
add_custom_command(OUTPUT "${effetune_bundle_asset_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${effetune_bundle_resources}/webview"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${effetune_bundle_resources}/webview"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${effetune_webview_assets}"
          "${effetune_bundle_resources}/webview"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
          "${effetune_bundle_resources}/THIRD-PARTY-NOTICES.txt"
  COMMAND "${CMAKE_COMMAND}" -E touch "${effetune_bundle_asset_stamp}"
  DEPENDS
    effetune_plugin
    "${effetune_webview_assets}/.stamp"
    "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
  COMMENT "Copying EffeTune WebView assets into the VST3 bundle")
add_custom_target(effetune_plugin_bundle_assets ALL DEPENDS "${effetune_bundle_asset_stamp}")
add_dependencies(effetune_plugin_bundle_assets effetune_plugin)

# VST3 SDK 3.8 only assigns per-config bundle output paths while iterating
# CMAKE_CONFIGURATION_TYPES. Ninja is a single-config generator, so provide the
# equivalent path explicitly to keep the DLL inside the .vst3 bundle.
if(WIN32 AND NOT CMAKE_CONFIGURATION_TYPES AND SMTG_CREATE_BUNDLE_FOR_WINDOWS)
  get_target_property(effetune_win_arch effetune_plugin SMTG_WIN_ARCHITECTURE_NAME)
  get_target_property(effetune_bundle_contents effetune_plugin SMTG_PLUGIN_PACKAGE_CONTENTS)
  set_target_properties(effetune_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY
      "${CMAKE_BINARY_DIR}/VST3/${CMAKE_BUILD_TYPE}/${EFFETUNE_PLUGIN_PACKAGE_NAME}.vst3/${effetune_bundle_contents}/${effetune_win_arch}")
endif()

if(MSVC)
  target_compile_options(effetune_plugin PRIVATE /W4 /WX /utf-8)
endif()

if(WIN32)
  target_link_libraries(effetune_plugin PRIVATE user32 ole32 shell32 shlwapi)
elseif(APPLE)
  target_link_libraries(effetune_plugin PRIVATE "-framework Cocoa" "-framework WebKit")
endif()

if(SMTG_MAC)
  smtg_target_set_bundle(effetune_plugin
    BUNDLE_IDENTIFIER "com.frieve.effetune.plugin"
    COMPANY_NAME "Frieve")
  set_target_properties(effetune_plugin PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.frieve.effetune.plugin"
    MACOSX_BUNDLE_BUNDLE_NAME "${EFFETUNE_PLUGIN_PACKAGE_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 Frieve.")
endif()
