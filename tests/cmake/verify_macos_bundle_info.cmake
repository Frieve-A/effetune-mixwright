if(NOT DEFINED BUNDLE_PATH OR NOT DEFINED EXPECTED_EXECUTABLE OR
   NOT DEFINED EXPECTED_IDENTIFIER OR NOT DEFINED EXPECTED_VERSION)
  message(FATAL_ERROR "Bundle verification arguments are incomplete")
endif()

set(plist_path "${BUNDLE_PATH}/Contents/Info.plist")
set(executable_path "${BUNDLE_PATH}/Contents/MacOS/${EXPECTED_EXECUTABLE}")

if(NOT EXISTS "${plist_path}")
  message(FATAL_ERROR "Bundle Info.plist is missing: ${plist_path}")
endif()
if(NOT EXISTS "${executable_path}")
  message(FATAL_ERROR "Bundle executable is missing: ${executable_path}")
endif()

find_program(PLUTIL_EXECUTABLE plutil REQUIRED)
execute_process(
  COMMAND "${PLUTIL_EXECUTABLE}" -lint "${plist_path}"
  RESULT_VARIABLE lint_result
  OUTPUT_VARIABLE lint_output
  ERROR_VARIABLE lint_error)
if(NOT lint_result EQUAL 0)
  message(FATAL_ERROR "Invalid bundle Info.plist: ${lint_output}${lint_error}")
endif()

function(expect_plist_value key expected)
  execute_process(
    COMMAND "${PLUTIL_EXECUTABLE}" -extract "${key}" raw -o - "${plist_path}"
    RESULT_VARIABLE extract_result
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE extract_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "Unable to read ${key}: ${extract_error}")
  endif()
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "Unexpected ${key}: expected '${expected}', got '${actual}'")
  endif()
endfunction()

expect_plist_value(CFBundlePackageType "BNDL")
expect_plist_value(CFBundleExecutable "${EXPECTED_EXECUTABLE}")
expect_plist_value(CFBundleName "${EXPECTED_EXECUTABLE}")
expect_plist_value(CFBundleIdentifier "${EXPECTED_IDENTIFIER}")
expect_plist_value(CFBundleVersion "${EXPECTED_VERSION}")
expect_plist_value(CFBundleShortVersionString "${EXPECTED_VERSION}")
