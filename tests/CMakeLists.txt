add_executable(effetune_vst_unit_tests unit/engine_tests.cpp)
target_link_libraries(effetune_vst_unit_tests PRIVATE effetune_engine)
target_compile_features(effetune_vst_unit_tests PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(effetune_vst_unit_tests PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(effetune_vst_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME effetune_vst_unit_tests COMMAND effetune_vst_unit_tests)

add_executable(effetune_resampler_tests audio/resampler_tests.cpp)
target_link_libraries(effetune_resampler_tests PRIVATE effetune_engine)
target_compile_features(effetune_resampler_tests PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(effetune_resampler_tests PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(effetune_resampler_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME effetune_resampler_tests COMMAND effetune_resampler_tests)

add_executable(effetune_file_exchange_tests
  unit/file_exchange_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/plugin/native_file_dialog.cpp
  ${PROJECT_SOURCE_DIR}/src/plugin/native_file_dialog.h)
target_include_directories(effetune_file_exchange_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(effetune_file_exchange_tests PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(effetune_file_exchange_tests PRIVATE /W4 /WX /utf-8)
  target_link_libraries(effetune_file_exchange_tests PRIVATE ole32)
else()
  target_compile_options(effetune_file_exchange_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(APPLE)
    target_link_libraries(effetune_file_exchange_tests PRIVATE "-framework Cocoa")
  endif()
endif()
add_test(NAME effetune_file_exchange_tests COMMAND effetune_file_exchange_tests)

if(WIN32 AND TARGET effetune_plugin)
  add_executable(effetune_plugin_view_scaling_tests
    unit/plugin_view_scaling_tests.cpp)
  target_link_libraries(effetune_plugin_view_scaling_tests PRIVATE sdk)
  target_include_directories(effetune_plugin_view_scaling_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src)
  target_compile_features(effetune_plugin_view_scaling_tests PRIVATE cxx_std_20)
  target_compile_options(effetune_plugin_view_scaling_tests PRIVATE /W4 /WX /utf-8)
  add_dependencies(effetune_plugin_view_scaling_tests effetune_plugin)
  add_test(NAME effetune_plugin_view_scaling_tests
    COMMAND effetune_plugin_view_scaling_tests $<TARGET_FILE:effetune_plugin>)
endif()

if(WIN32 AND TARGET effetune_ui_assets)
  add_executable(effetune_webview_smoke
    webview/webview_smoke.cpp
    ${PROJECT_SOURCE_DIR}/src/bridge/webview_host.cpp
    ${PROJECT_SOURCE_DIR}/src/bridge/webview_host.h)
  target_link_libraries(effetune_webview_smoke PRIVATE effetune_choc user32 ole32 shell32 shlwapi)
  target_include_directories(effetune_webview_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_features(effetune_webview_smoke PRIVATE cxx_std_20)
  target_compile_definitions(effetune_webview_smoke PRIVATE
    EFFETUNE_WEBVIEW_ASSET_DIR="${CMAKE_BINARY_DIR}/webview-assets")
  target_compile_options(effetune_webview_smoke PRIVATE /W4 /WX /utf-8)
  add_dependencies(effetune_webview_smoke effetune_ui_assets)
  add_test(NAME effetune_webview_smoke COMMAND effetune_webview_smoke)
  set_tests_properties(effetune_webview_smoke PROPERTIES TIMEOUT 30)
endif()

if(TARGET effetune_ui_assets)
  add_test(NAME effetune_vst_audio_manager_tests
    COMMAND "${EFFETUNE_NODE_EXECUTABLE}" --test
            "${PROJECT_SOURCE_DIR}/tests/esm/vst-audio-manager.test.mjs")
  add_test(NAME effetune_ui_asset_contract
    COMMAND "${EFFETUNE_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/verify-ui-assets.mjs"
            --assets "${CMAKE_BINARY_DIR}/webview-assets")
endif()

if(TARGET effetune_plugin_bundle_assets)
  add_test(NAME effetune_vst3_ui_asset_contract
    COMMAND "${EFFETUNE_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/verify-ui-assets.mjs"
            --assets
            "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/${EFFETUNE_PLUGIN_PACKAGE_NAME}.vst3/Contents/Resources/webview")
endif()

if(WIN32 AND TARGET validator AND TARGET effetune_plugin)
  add_test(NAME effetune_vst3_validator
    COMMAND $<TARGET_FILE:validator>
            "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/${EFFETUNE_PLUGIN_PACKAGE_NAME}.vst3")
  set_tests_properties(effetune_vst3_validator PROPERTIES TIMEOUT 120)
elseif(APPLE AND TARGET validator AND TARGET effetune_plugin)
  add_test(NAME effetune_vst3_validator
    COMMAND $<TARGET_FILE:validator> $<TARGET_BUNDLE_DIR:effetune_plugin>)
  set_tests_properties(effetune_vst3_validator PROPERTIES TIMEOUT 120)
endif()
