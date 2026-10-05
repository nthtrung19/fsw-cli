cmake_minimum_required(VERSION 3.16)

project(fswcli
    VERSION 0.1.0
    DESCRIPTION "Command-line tool for commanding cFS flight software"
    LANGUAGES CXX)

# ----------------------------------------------------------------------------
# Global settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(FSWCLI_BUILD_TESTS "Build unit tests" ON)

# Warning flags applied to our own targets only (never to external/).
add_library(fswcli_warnings INTERFACE)
target_compile_options(fswcli_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ----------------------------------------------------------------------------
# Third-party: daniele77/cli (header-only, git submodule pinned to v2.2.0)
# No Asio: the built-in LoopScheduler is enough for a command-only tool.
# ----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/cli/CMakeLists.txt")
    message(FATAL_ERROR
        "external/cli is missing. Run: git submodule update --init --recursive")
endif()
add_subdirectory(external/cli EXCLUDE_FROM_ALL)
# Treat cli headers as system headers so our strict warnings don't fire on them.
get_target_property(_cli_inc cli INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(cli PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cli_inc}")

# ----------------------------------------------------------------------------
# Our code
# ----------------------------------------------------------------------------
add_subdirectory(src)

if(FSWCLI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
