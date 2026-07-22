add_executable(effetune-headless headless_host.cpp)

target_link_libraries(effetune-headless PRIVATE effetune_engine)
target_include_directories(
  effetune-headless
  PRIVATE "${PROJECT_SOURCE_DIR}/external/effetune/dsp/generated/cpp")
target_compile_features(effetune-headless PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(effetune-headless PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(
    effetune-headless PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(effetune-resampler-bench resampler_bench.cpp)
target_link_libraries(effetune-resampler-bench PRIVATE effetune_engine)
target_compile_features(effetune-resampler-bench PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(effetune-resampler-bench PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(
    effetune-resampler-bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_test(NAME effetune_headless_help COMMAND effetune-headless --help)
endif()
