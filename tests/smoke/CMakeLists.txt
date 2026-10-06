# ----------------------------------------------------------------------------
# Smoke tests: run the mcs executable as an operator would and check exit
# codes and output. They use the shipped config/ files.
# ----------------------------------------------------------------------------

set(SHIPPED_CONFIG ${PROJECT_SOURCE_DIR}/config/targets.json)
set(DATA ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(RUNNER ${CMAKE_CURRENT_SOURCE_DIR}/run_and_check.cmake)

# mcs_smoke(NAME <name> EXIT <code> ARGS <args...> [EXPECT <regex>] [REJECT <regex>] [FILE <glob>])
function(mcs_smoke)
    cmake_parse_arguments(S "" "NAME;EXIT;EXPECT;REJECT;FILE" "ARGS" ${ARGN})
    set(defs -DEXPECT_EXIT=${S_EXIT})
    if(DEFINED S_EXPECT)
        list(APPEND defs "-DEXPECT_REGEX=${S_EXPECT}")
    endif()
    if(DEFINED S_REJECT)
        list(APPEND defs "-DREJECT_REGEX=${S_REJECT}")
    endif()
    if(DEFINED S_FILE)
        list(APPEND defs "-DEXPECT_FILE_GLOB=${S_FILE}")
    endif()
    add_test(NAME smoke.${S_NAME}
             COMMAND ${CMAKE_COMMAND} ${defs} -P ${RUNNER} -- $<TARGET_FILE:mcs> ${S_ARGS})
    # Never wait for input: a mis-parsed command line would otherwise start an
    # interactive session and hang.
    set_tests_properties(smoke.${S_NAME} PROPERTIES TIMEOUT 20)
endfunction()

set(BASE --config ${SHIPPED_CONFIG} --no-log)

# ---- golden packets through the whole executable (dry-run) ---------------------
mcs_smoke(NAME dryrun_noop EXIT 0
    ARGS ${BASE} --dry-run -c "fsw ds noop"
    EXPECT "\\[dry-run\\] fsw ds noop  \\(8 bytes, apid=0x14B seq=0\\)\n  19 4B C0 00 00 01 6C 00")

mcs_smoke(NAME dryrun_reset EXIT 0
    ARGS ${BASE} -n -c "fsw ds reset"
    EXPECT "19 4B C0 00 00 01 6D 01")

mcs_smoke(NAME dryrun_set_app_state_enable EXIT 0
    ARGS ${BASE} -n -c "fsw ds set_app_state enable"
    EXPECT "19 4B C0 00 00 05 6B 02 01 00 00 00")

mcs_smoke(NAME dryrun_target_profile EXIT 0
    ARGS ${BASE} --target sil-dry -c "fsw ds set_app_state disable"
    EXPECT "19 4B C0 00 00 05 6A 02 00 00 00 00")

mcs_smoke(NAME sequence_increments EXIT 0
    ARGS ${BASE} -n -c "fsw ds noop" -c "fsw ds noop"
    EXPECT "seq=0.*seq=1")

# ---- validation: nothing sent, exit code 1 ---------------------------------------
mcs_smoke(NAME missing_argument EXIT 1
    ARGS ${BASE} -n -c "fsw ds set_app_state"
    EXPECT "expected 1 argument, got 0" REJECT "dry-run")

mcs_smoke(NAME invalid_enum EXIT 1
    ARGS ${BASE} -n -c "fsw ds set_app_state maybe"
    EXPECT "invalid value 'maybe'" REJECT "dry-run")

mcs_smoke(NAME unknown_command EXIT 1
    ARGS ${BASE} -n -c "fsw ds nope"
    EXPECT "unknown command: fsw ds nope")

mcs_smoke(NAME one_failure_fails_batch EXIT 1
    ARGS ${BASE} -n -c "fsw ds noop" -c "fsw ds set_app_state 7" -c "fsw ds reset"
    EXPECT "fsw ds noop.*error:.*fsw ds reset")

mcs_smoke(NAME bad_session_command EXIT 1
    ARGS ${BASE} -n -c "verbose maybe"
    EXPECT "usage: verbose")

# ---- options and configuration ---------------------------------------------------
mcs_smoke(NAME bad_option EXIT 2 ARGS --bogus EXPECT "unknown option: --bogus")
mcs_smoke(NAME c_and_f_conflict EXIT 2 ARGS -c x -f y EXPECT "cannot be combined")
mcs_smoke(NAME unknown_target EXIT 3
    ARGS ${BASE} --target nope -c "fsw ds noop"
    EXPECT "unknown target 'nope'.*available: sil, sil-dry")
mcs_smoke(NAME missing_config EXIT 3
    ARGS --config ${DATA}/does_not_exist.json -c "fsw ds noop"
    EXPECT "config file not found")
mcs_smoke(NAME bad_catalog EXIT 3
    ARGS --config ${DATA}/bad_targets.json --no-log -c "fsw ds noop"
    EXPECT "bad_catalog.json.*command code 0 is already used")
mcs_smoke(NAME list_targets EXIT 0
    ARGS --config ${SHIPPED_CONFIG} --list-targets
    EXPECT "\\* sil +ccsds_v1 -> udp.*sil-dry +ccsds_v1 -> dryrun")
mcs_smoke(NAME help EXIT 0 ARGS --help EXPECT "Exit codes: 0 ok")
mcs_smoke(NAME version EXIT 0 ARGS --version EXPECT "mcs [0-9]+\\.[0-9]+\\.[0-9]+")

# ---- session commands, scripts, verbose, log ---------------------------------------
mcs_smoke(NAME script_file EXIT 0
    ARGS ${BASE} -n -f ${DATA}/script.txt
    EXPECT "fsw ds noop.*fsw ds set_app_state enable.*fsw ds reset")

mcs_smoke(NAME target_command EXIT 0
    ARGS ${BASE} -n -c "target"
    EXPECT "target: +sil\npipeline: ccsds_v1 \\(little-endian\\) -> dryrun.*ds +mid=0x194B")

mcs_smoke(NAME verbose_option EXIT 0
    ARGS ${BASE} -n -v -c "fsw ds noop"
    EXPECT "19 4B C0 00 00 01 6C 00")

mcs_smoke(NAME raw_command EXIT 0
    ARGS ${BASE} -n -c "raw 19 4B C0 00 00 01 6C 00"
    EXPECT "\\[dry-run\\] raw 19 4B C0 00 00 01 6C 00")

mcs_smoke(NAME menu_navigation EXIT 0
    ARGS ${BASE} -n -c "fsw" -c "ds" -c "noop"
    EXPECT "\\[dry-run\\] fsw ds noop")

mcs_smoke(NAME packet_log_written EXIT 0
    ARGS --config ${SHIPPED_CONFIG} -n --log-dir ${CMAKE_CURRENT_BINARY_DIR}/logs -c "fsw ds noop"
    FILE ${CMAKE_CURRENT_BINARY_DIR}/logs/mcs-*.log)
