
set(content "")
file(REMOVE "${CTEST_FILE}")
if(NOT DEFINED TEST_DISCOVERY_TIMEOUT OR TEST_DISCOVERY_TIMEOUT STREQUAL "")
  set(TEST_DISCOVERY_TIMEOUT 30)
endif()

execute_process(
  COMMAND "${TEST_EXECUTABLE}" --list-test-cases
  OUTPUT_VARIABLE case_list
  RESULT_VARIABLE exit_code
  ERROR_VARIABLE stderr_text
  TIMEOUT "${TEST_DISCOVERY_TIMEOUT}")

if(NOT exit_code STREQUAL "0")
  string(STRIP "${stderr_text}" stderr_text)
  message(FATAL_ERROR
    "nxtest: '${TEST_EXECUTABLE} --list-test-cases' failed (${exit_code}): ${stderr_text}")
else()
  string(LENGTH "${case_list}" total)
  set(cursor 0)
  while(cursor LESS total)
    string(SUBSTRING "${case_list}" ${cursor} -1 remainder)
    string(FIND "${remainder}" "\n" newline)
    if(newline EQUAL -1)
      set(case_name "${remainder}")
      set(cursor ${total})
    else()
      string(SUBSTRING "${remainder}" 0 ${newline} case_name)
      math(EXPR cursor "${cursor} + ${newline} + 1")
    endif()

    string(REGEX REPLACE "[\r\n]+$" "" case_name "${case_name}")
    if(case_name STREQUAL "")
      continue()
    endif()

    string(APPEND content
      "add_test(\"${TEST_PREFIX}${case_name}\" \"${TEST_EXECUTABLE}\" \"--test-case=${case_name}\")\n")
    string(APPEND content
      "set_tests_properties(\"${TEST_PREFIX}${case_name}\" PROPERTIES LABELS \"${TEST_LABELS}\" TIMEOUT ${TEST_TIMEOUT} SKIP_RETURN_CODE 77)\n")
  endwhile()
endif()

file(WRITE "${CTEST_FILE}" "${content}")
