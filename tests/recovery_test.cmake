if(NOT DEFINED NPS_CLI OR NOT EXISTS "${NPS_CLI}")
  message(FATAL_ERROR "NPS_CLI must name the built command-line executable")
endif()
if(NOT DEFINED NPS_TEST_ROOT)
  message(FATAL_ERROR "NPS_TEST_ROOT is required")
endif()

cmake_path(ABSOLUTE_PATH NPS_TEST_ROOT NORMALIZE)
if(NOT NPS_TEST_ROOT MATCHES "recovery-test$")
  message(FATAL_ERROR "Refusing to clean an unexpected recovery test path")
endif()

set(project_path "${NPS_TEST_ROOT}/recovery.npsproj")
set(create_retry_path "${NPS_TEST_ROOT}/create-retry.npsproj")
file(REMOVE_RECURSE "${NPS_TEST_ROOT}")
file(MAKE_DIRECTORY "${NPS_TEST_ROOT}")

execute_process(
  COMMAND "${NPS_CLI}" crash-create-after-object "${create_retry_path}"
  RESULT_VARIABLE create_crash_result
  OUTPUT_VARIABLE create_crash_output
  ERROR_VARIABLE create_crash_error
)
if(NOT create_crash_result EQUAL 86)
  message(
    FATAL_ERROR
    "Create crash injection returned ${create_crash_result}, expected 86: "
    "${create_crash_error}"
  )
endif()
if(EXISTS "${create_retry_path}")
  message(FATAL_ERROR "An interrupted create published a partial project")
endif()

execute_process(
  COMMAND "${NPS_CLI}" demo "${create_retry_path}"
  RESULT_VARIABLE create_retry_result
  OUTPUT_VARIABLE create_retry_output
  ERROR_VARIABLE create_retry_error
)
if(NOT create_retry_result EQUAL 0)
  message(FATAL_ERROR "Create retry failed: ${create_retry_error}")
endif()

execute_process(
  COMMAND "${NPS_CLI}" demo "${project_path}"
  RESULT_VARIABLE demo_result
  OUTPUT_VARIABLE demo_output
  ERROR_VARIABLE demo_error
)
if(NOT demo_result EQUAL 0)
  message(FATAL_ERROR "Demo setup failed: ${demo_error}")
endif()

execute_process(
  COMMAND "${NPS_CLI}" crash-commit "${project_path}"
  RESULT_VARIABLE crash_result
  OUTPUT_VARIABLE crash_output
  ERROR_VARIABLE crash_error
)
if(NOT crash_result EQUAL 86)
  message(
    FATAL_ERROR
    "Crash injection returned ${crash_result}, expected 86: ${crash_error}"
  )
endif()

execute_process(
  COMMAND "${NPS_CLI}" verify-recovery "${project_path}"
  RESULT_VARIABLE recovery_result
  OUTPUT_VARIABLE recovery_output
  ERROR_VARIABLE recovery_error
)
if(NOT recovery_result EQUAL 0)
  message(FATAL_ERROR "Recovery verification failed: ${recovery_error}")
endif()
if(NOT recovery_output MATCHES "\"committedTransactionPreserved\"[ \t]*:[ \t]*true")
  message(FATAL_ERROR "Recovery output did not confirm the committed transaction")
endif()

file(REMOVE_RECURSE "${NPS_TEST_ROOT}")
