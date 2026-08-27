cmake_minimum_required(VERSION 3.24)

project(EffeTunePlugin VERSION 0.6.0 LANGUAGES C CXX)

# CMake 4.1.0-rc1 can decode the Japanese MSVC /showIncludes prefix as
# mojibake, which leaves Ninja with no header dependencies. Repair only that
# known-bad detected value; other compiler locales remain untouched.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja" AND
   CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^繝")
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "メモ: インクルード ファイル:  ")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Apply the shipping optimization profile explicitly instead of relying only on
# CMake's cached CMAKE_<LANG>_FLAGS_RELEASE defaults. A configure attempted
# before the compiler environment is ready can leave those defaults empty in a
# reusable build tree, producing an unoptimized plug-in despite Release being
# selected.
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/O2>
                      $<$<CONFIG:Release>:/Ob2>)
else()
  add_compile_options($<$<CONFIG:Release>:-O3>)
endif()
add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)

option(EFFETUNE_BUILD_PLUGIN "Build the VST3 plug-in" ON)
option(EFFETUNE_BUILD_TESTS "Build EffeTune VST unit tests" ON)
option(EFFETUNE_BUILD_TOOLS "Build EffeTune VST command-line tools" ON)
option(EFFETUNE_ENABLE_AVX2 "Use AVX2/FMA for the Windows x64 resampler" ON)
option(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE
  "Build the non-shipping automation registration host-gate fixture" OFF)

if(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
  if(NOT EFFETUNE_BUILD_PLUGIN OR NOT EFFETUNE_BUILD_TESTS)
    message(FATAL_ERROR
      "The automation host-gate fixture requires plug-in and test targets")
  endif()
  set(EFFETUNE_PLUGIN_PACKAGE_NAME "EffeTune Mixwright Automation Host Gate")
  add_compile_definitions(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE=1)
  message(WARNING
    "Building the non-shipping automation host-gate fixture with an isolated VST identity")
else()
  set(EFFETUNE_PLUGIN_PACKAGE_NAME "EffeTune Mixwright")
endif()

if(EFFETUNE_BUILD_TESTS)
  include(CTest)
endif()

include(cmake/Dependencies.cmake)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/effetune/dsp/CMakeLists.txt")
  message(FATAL_ERROR
    "external/effetune is missing. Run: git submodule update --init --recursive")
endif()

if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif()

add_subdirectory(external/effetune/dsp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

if(EFFETUNE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
