# ----------------------------------------------------------------------------
# Test suites (docs/DESIGN.md §7)
#   unit/         GoogleTest, one file per module of fswcli_core
#   oracle/       our packets vs. packets built by cFE's own ccsds.h/ccsds.c
#   integration/  real UDP socket on loopback
#   smoke/        the fswcli executable, run as an operator would
# ----------------------------------------------------------------------------

# GoogleTest v1.17.0 (git submodule)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/googletest
                 ${CMAKE_CURRENT_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
include(GoogleTest)

set(FSWCLI_TEST_DEFINES
    FSWCLI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# ---- unit -------------------------------------------------------------------
add_executable(fswcli_unit_tests
    unit/test_text.cpp
    unit/test_byte_writer.cpp
    unit/test_hex.cpp
    unit/test_options.cpp
    unit/test_catalog.cpp
    unit/test_payload_encoder.cpp
    unit/test_ccsds_v1.cpp
    unit/test_registry_pipeline.cpp
    unit/test_config.cpp
    unit/test_packet_log.cpp
    unit/test_command_service.cpp)
target_include_directories(fswcli_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fswcli_unit_tests PRIVATE fswcli_core GTest::gtest_main)
target_compile_definitions(fswcli_unit_tests PRIVATE ${FSWCLI_TEST_DEFINES})
gtest_discover_tests(fswcli_unit_tests TEST_PREFIX "unit." DISCOVERY_TIMEOUT 30)

# ---- oracle -----------------------------------------------------------------
# cFE 6.7 ccsds.c compiled as C with minimal stand-ins for its cFE includes.
add_library(cfe_ccsds_oracle STATIC oracle/cfe/ccsds.c)
target_include_directories(cfe_ccsds_oracle SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cfe
    ${CMAKE_CURRENT_SOURCE_DIR}/oracle/stubs)

add_executable(fswcli_oracle_tests oracle/test_oracle.cpp)
target_link_libraries(fswcli_oracle_tests PRIVATE fswcli_core cfe_ccsds_oracle GTest::gtest_main)
target_compile_definitions(fswcli_oracle_tests PRIVATE ${FSWCLI_TEST_DEFINES})
gtest_discover_tests(fswcli_oracle_tests TEST_PREFIX "oracle." DISCOVERY_TIMEOUT 30)

# ---- integration ------------------------------------------------------------
add_executable(fswcli_integration_tests integration/test_udp_loopback.cpp)
target_link_libraries(fswcli_integration_tests PRIVATE fswcli_core GTest::gtest_main)
target_compile_definitions(fswcli_integration_tests PRIVATE ${FSWCLI_TEST_DEFINES})
gtest_discover_tests(fswcli_integration_tests TEST_PREFIX "integration." DISCOVERY_TIMEOUT 30)

# ---- smoke ------------------------------------------------------------------
add_subdirectory(smoke)
