cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

project(
  natural_photo_studio
  VERSION 0.2.0
  DESCRIPTION "Local-first non-destructive professional photo editor"
  LANGUAGES CXX
)

include(GNUInstallDirs)

option(NPS_BUILD_TESTS "Build the Natural Photo Studio test suite" ON)
option(NPS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NpsWarnings)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(
  nps_imaging
  STATIC
    src/color/color_encoding.cpp
    src/imaging/image_f32.cpp
    src/imaging/image16.cpp
    src/imaging/mask16.cpp
    src/imaging/operations.cpp
    src/imaging/ppm16.cpp
    src/imaging/synthetic.cpp
)
add_library(nps::imaging ALIAS nps_imaging)
target_compile_features(nps_imaging PUBLIC cxx_std_23)
target_include_directories(
  nps_imaging
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(nps_imaging PRIVATE OpenSSL::Crypto)
nps_enable_warnings(nps_imaging)

add_library(
  nps_document
  STATIC
    src/document/edit_graph.cpp
)
add_library(nps::document ALIAS nps_document)
target_compile_features(nps_document PUBLIC cxx_std_23)
target_include_directories(
  nps_document
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
  nps_document
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    OpenSSL::Crypto
)
nps_enable_warnings(nps_document)

add_library(
  nps_render
  STATIC
    src/render/atomic_ppm_export.cpp
    src/render/cpu_renderer.cpp
)
add_library(nps::render ALIAS nps_render)
target_compile_features(nps_render PUBLIC cxx_std_23)
target_include_directories(
  nps_render
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
  nps_render
  PUBLIC
    nps::document
    nps::imaging
    Threads::Threads
  PRIVATE
    OpenSSL::Crypto
)
nps_enable_warnings(nps_render)

add_library(
  nps_core
  STATIC
    src/core/command.cpp
    src/core/command_bus.cpp
    src/core/project_store.cpp
)
add_library(nps::core ALIAS nps_core)
target_compile_features(nps_core PUBLIC cxx_std_23)
target_include_directories(
  nps_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
  nps_core
  PUBLIC
    nps::document
    nlohmann_json::nlohmann_json
  PRIVATE
    OpenSSL::Crypto
    unofficial::sqlite3::sqlite3
)
nps_enable_warnings(nps_core)

add_executable(nps_cli apps/cli/main.cpp)
set_target_properties(nps_cli PROPERTIES OUTPUT_NAME "nps-cli")
target_compile_features(nps_cli PRIVATE cxx_std_23)
target_link_libraries(nps_cli PRIVATE nps::core nps::imaging nps::render)
nps_enable_warnings(nps_cli)

if(NPS_BUILD_TESTS)
  include(CTest)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(
    nps_tests
      tests/atomic_ppm_export_test.cpp
      tests/command_bus_test.cpp
      tests/command_protocol_test.cpp
      tests/cpu_renderer_test.cpp
      tests/edit_graph_test.cpp
      tests/imaging_test.cpp
      tests/pixel_contract_test.cpp
      tests/project_store_test.cpp
  )
  target_compile_features(nps_tests PRIVATE cxx_std_23)
  target_link_libraries(
    nps_tests
    PRIVATE
      nps::core
      nps::document
      nps::imaging
      nps::render
      Catch2::Catch2WithMain
      unofficial::sqlite3::sqlite3
  )
  nps_enable_warnings(nps_tests)

  include(Catch)
  catch_discover_tests(
    nps_tests
    PROPERTIES
      LABELS unit
      TIMEOUT 30
  )

  add_test(
    NAME nps.recovery.committed-transaction
    COMMAND
      "${CMAKE_COMMAND}"
      "-DNPS_CLI=$<TARGET_FILE:nps_cli>"
      "-DNPS_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/recovery-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/recovery_test.cmake"
  )
  set_tests_properties(
    nps.recovery.committed-transaction
    PROPERTIES
      LABELS "process;recovery;migration;export"
      RUN_SERIAL TRUE
      TIMEOUT 120
  )
endif()

# vcpkg's FindOpenSSL wrapper represents the Windows import library as an
# UNKNOWN imported target, so target-graph DLL enumeration misses libcrypto.
# Inspect the built executable instead and omit Windows system libraries. The
# default Linux and macOS triplets link these dependencies statically.
if(WIN32)
  install(
    TARGETS nps_cli
    RUNTIME_DEPENDENCIES
      PRE_EXCLUDE_REGEXES
        "api-ms-win-.*"
        "ext-ms-win-.*"
      POST_EXCLUDE_REGEXES
        ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
    RUNTIME
      DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT Runtime
  )
else()
  install(
    TARGETS nps_cli
    RUNTIME
      DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT Runtime
  )
endif()

install(
  FILES
    LICENSE
    NOTICE
    THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/natural-photo-studio"
  COMPONENT Runtime
)
