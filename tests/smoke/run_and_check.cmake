# Runs a command and checks its exit code and output.
#
#   cmake -DEXPECT_EXIT=0 [-DEXPECT_REGEX=...] [-DREJECT_REGEX=...]
#         [-DEXPECT_FILE_GLOB=...] -P run_and_check.cmake -- prog arg1 arg2 ...
#
# The command comes after "--" (read from CMAKE_ARGV*), so arguments with
# spaces or semicolons need no escaping. stdout and stderr are combined.

set(CMD "")
set(collect FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
    if(collect)
        list(APPEND CMD "${CMAKE_ARGV${i}}")
    elseif("${CMAKE_ARGV${i}}" STREQUAL "--")
        set(collect TRUE)
    endif()
endforeach()
if(NOT CMD)
    message(FATAL_ERROR "no command given after --")
endif()

execute_process(
    COMMAND ${CMD}
    INPUT_FILE /dev/null
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err)
set(all "${out}${err}")

if(NOT "${exit_code}" STREQUAL "${EXPECT_EXIT}")
    message(FATAL_ERROR "exit code ${exit_code}, expected ${EXPECT_EXIT}\n--- output ---\n${all}")
endif()

if(DEFINED EXPECT_REGEX AND NOT all MATCHES "${EXPECT_REGEX}")
    message(FATAL_ERROR "output does not match: ${EXPECT_REGEX}\n--- output ---\n${all}")
endif()

if(DEFINED REJECT_REGEX AND all MATCHES "${REJECT_REGEX}")
    message(FATAL_ERROR "output must not match: ${REJECT_REGEX}\n--- output ---\n${all}")
endif()

if(DEFINED EXPECT_FILE_GLOB)
    file(GLOB found "${EXPECT_FILE_GLOB}")
    if(NOT found)
        message(FATAL_ERROR "no file matches ${EXPECT_FILE_GLOB}")
    endif()
endif()

message(STATUS "ok\n${all}")
