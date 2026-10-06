cmake_minimum_required(VERSION 3.16)

project(mcs
    VERSION 0.2.0
    DESCRIPTION "Command-line tool for commanding cFS flight software"
    LANGUAGES C CXX)

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

option(mcs_BUILD_TESTS "Build unit, oracle, integration and smoke tests" ON)

# Strict warnings, applied to our own targets only (never to external/).
add_library(mcs_warnings INTERFACE)
target_compile_options(mcs_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion>)

# ----------------------------------------------------------------------------
# Third-party code: git submodules in external/, pinned to release tags.
# ----------------------------------------------------------------------------
foreach(_dep cli json googletest)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/${_dep}/CMakeLists.txt")
        message(FATAL_ERROR
            "external/${_dep} is missing. Run: git submodule update --init --recursive")
    endif()
endforeach()

# Mark an INTERFACE library's include dirs as system headers so our strict
# warnings don't fire inside third-party headers.
function(mcs_as_system_headers target)
    get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
endfunction()

# daniele77/cli v2.2.0 (header-only). No Asio: the built-in LoopScheduler is enough.
add_subdirectory(external/cli EXCLUDE_FROM_ALL)
mcs_as_system_headers(cli)

# nlohmann/json v3.12.0 (header-only). Used by src/config only.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
add_subdirectory(external/json EXCLUDE_FROM_ALL)

# ----------------------------------------------------------------------------
# Our code
# ----------------------------------------------------------------------------
add_subdirectory(src)

if(mcs_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
