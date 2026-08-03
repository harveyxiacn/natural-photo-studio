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
set(migration_source "${NPS_TEST_ROOT}/migration-source.npsproj")
set(migration_target "${NPS_TEST_ROOT}/migration-target.npsproj")
set(export_target "${NPS_TEST_ROOT}/recovered-export.ppm")
set(v1_export_target "${NPS_TEST_ROOT}/must-not-export-v1.ppm")
set(forbidden_export_target "${migration_target}/must-not-export-here.ppm")
set(published_source "${NPS_TEST_ROOT}/published-source.npsproj")
set(published_target "${NPS_TEST_ROOT}/published-target.npsproj")
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

execute_process(
  COMMAND "${NPS_CLI}" demo "${migration_source}"
  RESULT_VARIABLE migration_setup_result
  ERROR_VARIABLE migration_setup_error
)
if(NOT migration_setup_result EQUAL 0)
  message(FATAL_ERROR "Migration source setup failed: ${migration_setup_error}")
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" crash-migrate-staged
    "${migration_source}" "${migration_target}"
  RESULT_VARIABLE migration_stage_crash_result
  ERROR_VARIABLE migration_stage_crash_error
)
if(NOT migration_stage_crash_result EQUAL 86)
  message(
    FATAL_ERROR
    "Migration staged crash returned ${migration_stage_crash_result}, "
    "expected 86: ${migration_stage_crash_error}"
  )
endif()
if(EXISTS "${migration_target}")
  message(FATAL_ERROR "A pre-publication migration crash exposed a target")
endif()

execute_process(
  COMMAND "${NPS_CLI}" verify "${migration_source}"
  RESULT_VARIABLE source_verify_result
  ERROR_VARIABLE source_verify_error
)
if(NOT source_verify_result EQUAL 0)
  message(
    FATAL_ERROR
    "The v1 source was damaged by staged migration: ${source_verify_error}"
  )
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" migrate-v1-v2
    "${migration_source}" "${migration_target}"
  RESULT_VARIABLE migration_retry_result
  OUTPUT_VARIABLE migration_retry_output
  ERROR_VARIABLE migration_retry_error
)
if(NOT migration_retry_result EQUAL 0)
  message(FATAL_ERROR "Migration retry failed: ${migration_retry_error}")
endif()
if(NOT migration_retry_output MATCHES "\"projectFormat\"[ \t]*:[ \t]*\"nps.project/v2\"")
  message(FATAL_ERROR "Migration retry did not publish a verified v2 project")
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" export-demo
    "${migration_source}" "${v1_export_target}"
  RESULT_VARIABLE v1_export_result
  ERROR_VARIABLE v1_export_error
)
if(NOT v1_export_result EQUAL 2)
  message(
    FATAL_ERROR
    "A v1 export returned ${v1_export_result}, expected migration-required "
    "exit 2: ${v1_export_error}"
  )
endif()
if(NOT v1_export_error MATCHES "IO_PROJECT_MIGRATION_REQUIRED")
  message(FATAL_ERROR "A v1 export did not report explicit migration")
endif()
if(EXISTS "${v1_export_target}")
  message(FATAL_ERROR "A v1 project unexpectedly published an export")
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" export-demo
    "${migration_target}" "${forbidden_export_target}"
  RESULT_VARIABLE forbidden_export_result
  ERROR_VARIABLE forbidden_export_error
)
if(NOT forbidden_export_result EQUAL 3)
  message(
    FATAL_ERROR
    "An export into the project returned ${forbidden_export_result}, "
    "expected export-safety exit 3: ${forbidden_export_error}"
  )
endif()
if(NOT forbidden_export_error MATCHES "EXPORT_forbidden_destination")
  message(FATAL_ERROR "An in-project export did not report a forbidden path")
endif()
if(EXISTS "${forbidden_export_target}")
  message(FATAL_ERROR "An export wrote inside the protected project root")
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" crash-export-after-flush
    "${migration_target}" "${export_target}"
  RESULT_VARIABLE export_crash_result
  ERROR_VARIABLE export_crash_error
)
if(NOT export_crash_result EQUAL 86)
  message(
    FATAL_ERROR
    "Export crash returned ${export_crash_result}, expected 86: "
    "${export_crash_error}"
  )
endif()
if(EXISTS "${export_target}")
  message(FATAL_ERROR "A pre-publication export crash exposed a target file")
endif()
file(GLOB private_export_temps "${NPS_TEST_ROOT}/.nps-export-*.tmp")
list(LENGTH private_export_temps private_export_temp_count)
if(NOT private_export_temp_count EQUAL 1)
  message(
    FATAL_ERROR
    "The export crash must leave exactly one private, non-product temp file"
  )
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" export-demo
    "${migration_target}" "${export_target}"
  RESULT_VARIABLE export_retry_result
  OUTPUT_VARIABLE export_retry_output
  ERROR_VARIABLE export_retry_error
)
if(NOT export_retry_result EQUAL 0)
  message(FATAL_ERROR "Export retry failed: ${export_retry_error}")
endif()
if(NOT EXISTS "${export_target}")
  message(FATAL_ERROR "Export retry did not publish the complete target")
endif()
if(NOT export_retry_output MATCHES "\"status\"[ \t]*:[ \t]*\"exported\"")
  message(FATAL_ERROR "Export retry did not report a verified publication")
endif()
file(READ "${export_target}" export_magic OFFSET 0 LIMIT 2 HEX)
if(NOT export_magic STREQUAL "5036")
  message(FATAL_ERROR "Export retry did not publish a binary P6 PPM")
endif()
file(SIZE "${export_target}" export_size)
if(export_size LESS 13)
  message(FATAL_ERROR "Export retry published an implausibly short file")
endif()

execute_process(
  COMMAND "${NPS_CLI}" demo "${published_source}"
  RESULT_VARIABLE published_setup_result
  ERROR_VARIABLE published_setup_error
)
if(NOT published_setup_result EQUAL 0)
  message(
    FATAL_ERROR
    "Published-fault migration source setup failed: ${published_setup_error}"
  )
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" crash-migrate-published
    "${published_source}" "${published_target}"
  RESULT_VARIABLE migration_publish_crash_result
  ERROR_VARIABLE migration_publish_crash_error
)
if(NOT migration_publish_crash_result EQUAL 86)
  message(
    FATAL_ERROR
    "Migration published crash returned ${migration_publish_crash_result}, "
    "expected 86: ${migration_publish_crash_error}"
  )
endif()
if(NOT EXISTS "${published_target}/project.db")
  message(FATAL_ERROR "The complete published v2 target is missing")
endif()

execute_process(
  COMMAND
    "${NPS_CLI}" migrate-v1-v2
    "${published_source}" "${published_target}"
  RESULT_VARIABLE migration_publish_retry_result
  OUTPUT_VARIABLE migration_publish_retry_output
  ERROR_VARIABLE migration_publish_retry_error
)
if(NOT migration_publish_retry_result EQUAL 0)
  message(
    FATAL_ERROR
    "Published migration retry failed: ${migration_publish_retry_error}"
  )
endif()
if(EXISTS "${published_target}/.nps-migrating")
  message(FATAL_ERROR "The recovered migration marker was not finalized")
endif()

file(REMOVE_RECURSE "${NPS_TEST_ROOT}")
