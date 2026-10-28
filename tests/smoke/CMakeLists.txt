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
    ARGS ${BASE} --target fsw-dry -c "fsw ds set_app_state disable"
    EXPECT "19 4B C0 00 00 05 6A 02 00 00 00 00")

mcs_smoke(NAME dryrun_ci_noop EXIT 0
    ARGS ${BASE} -n -c "fsw ci noop"
    EXPECT "\\[dry-run\\] fsw ci noop  \\(8 bytes, apid=0x134 seq=0\\)\n  19 34 C0 00 00 01 13 00")

mcs_smoke(NAME ci_enable_to_defaults EXIT 0
    ARGS ${BASE} -n -c "fsw ci enable_to"
    EXPECT "\\(26 bytes, apid=0x134 seq=0\\)\n  19 34 C0 00 00 13 A8 02 31 32 37 2E 30 2E 30 2E 31 00 00 00 00 00 00 00 93 13")

mcs_smoke(NAME dryrun_ci_hk EXIT 0
    ARGS ${BASE} -n -c "fsw ci hk"
    EXPECT "\\[dry-run\\] fsw ci hk  \\(8 bytes, apid=0x135 seq=0\\)\n  19 35 C0 00 00 01 12 00")

mcs_smoke(NAME dryrun_aocs_hk EXIT 0
    ARGS ${BASE} -n -c "fsw aocs hk"
    EXPECT "\\[dry-run\\] fsw aocs hk  \\(8 bytes, apid=0x112 seq=0\\)\n  19 12 C0 00 00 01 35 00")

mcs_smoke(NAME dryrun_sc_hk EXIT 0
    ARGS ${BASE} -n -c "fsw sc hk"
    EXPECT "\\[dry-run\\] fsw sc hk  \\(8 bytes, apid=0x18A seq=0\\)\n  19 8A C0 00 00 01 AD 00")

mcs_smoke(NAME dryrun_fdir_hk EXIT 0
    ARGS ${BASE} -n -c "fsw fdir hk"
    EXPECT "\\[dry-run\\] fsw fdir hk  \\(8 bytes, apid=0x084 seq=0\\)\n  18 84 C0 00 00 01 A2 00")

mcs_smoke(NAME dryrun_main_hk EXIT 0
    ARGS ${BASE} -n -c "fsw main hk"
    EXPECT "\\[dry-run\\] fsw main hk  \\(8 bytes, apid=0x106 seq=0\\)\n  19 06 C0 00 00 01 21 00")

mcs_smoke(NAME dryrun_ds_hk EXIT 0
    ARGS ${BASE} -n -c "fsw ds hk"
    EXPECT "\\[dry-run\\] fsw ds hk  \\(8 bytes, apid=0x14C seq=0\\)\n  19 4C C0 00 00 01 6B 00")

mcs_smoke(NAME sequence_increments EXIT 0
    ARGS ${BASE} -n -c "fsw ds noop" -c "fsw ds noop"
    EXPECT "seq=0.*seq=1")

mcs_smoke(NAME dryrun_evs_enable_app_event_type EXIT 0
    ARGS ${BASE} -n -c "fsw evs enable_app_event_type DS debug"
    EXPECT "\\(30 bytes, apid=0x001 seq=0\\)\n  18 01 C0 00 00 17 22 05 44 53 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00")

# ---- validation: nothing sent, exit code 1 ---------------------------------------
mcs_smoke(NAME missing_argument EXIT 1
    ARGS ${BASE} -n -c "fsw ds set_app_state"
    EXPECT "expected 1 argument, got 0" REJECT "dry-run")

mcs_smoke(NAME invalid_enum EXIT 1
    ARGS ${BASE} -n -c "fsw ds set_app_state maybe"
    EXPECT "invalid value 'maybe'" REJECT "dry-run")

mcs_smoke(NAME ci_ip_too_long EXIT 1
    ARGS ${BASE} -n -c "fsw ci enable_to 1234567890123456 5011"
    EXPECT "too long" REJECT "dry-run")

mcs_smoke(NAME aocs_time_zero_rejected EXIT 1
    ARGS ${BASE} -n -c "fsw aocs nadir_pointing 0"
    EXPECT "below minimum 1" REJECT "dry-run")

mcs_smoke(NAME sc_rts_out_of_range EXIT 1
    ARGS ${BASE} -n -c "fsw sc start_rts 65"
    EXPECT "above maximum 64" REJECT "dry-run")

mcs_smoke(NAME fdir_app_name_too_long EXIT 1
    ARGS ${BASE} -n -c "fsw fdir trigger_event_watchpoint ABCDEFGHIJKLMNOPQRSTU 1 on"
    EXPECT "too long" REJECT "dry-run")

mcs_smoke(NAME main_switch_mask_out_of_range EXIT 1
    ARGS ${BASE} -n -c "fsw main set_switch_state 0x400"
    EXPECT "above maximum 1023" REJECT "dry-run")

# ---- critical commands: '!' in front, in any menu ----------------------------------
mcs_smoke(NAME critical_refused EXIT 1
    ARGS ${BASE} -n -c "fsw fm delete_dir /cf/x"
    EXPECT "'delete_dir' is a critical command: type '!' before it" REJECT "dry-run")

mcs_smoke(NAME confirm_in_app_menu EXIT 0
    ARGS ${BASE} -n -c "fsw" -c "fm" -c "! delete_dir /cf/x"
    EXPECT "\\[dry-run\\] fsw fm delete_dir /cf/x")

mcs_smoke(NAME confirm_without_space EXIT 0
    ARGS ${BASE} -n -c "fsw" -c "fm" -c "!delete_dir /cf/x"
    EXPECT "\\[dry-run\\] fsw fm delete_dir /cf/x")

mcs_smoke(NAME confirm_in_fsw_menu EXIT 0
    ARGS ${BASE} -n -c "fsw" -c "! fm delete_dir /cf/x"
    EXPECT "\\[dry-run\\] fsw fm delete_dir /cf/x")

mcs_smoke(NAME confirm_top_level EXIT 0
    ARGS ${BASE} -n -c "! fsw fm delete_dir /cf/x"
    EXPECT "\\[dry-run\\] fsw fm delete_dir /cf/x")

mcs_smoke(NAME confirm_covers_one_line EXIT 1
    ARGS ${BASE} -n -c "! fsw fm delete_dir /cf/a" -c "fsw fm delete_dir /cf/b"
    EXPECT "fsw fm delete_dir /cf/a.*'delete_dir' is a critical command")

mcs_smoke(NAME bang_alone EXIT 1
    ARGS ${BASE} -n -c "!"
    EXPECT "usage: ! fsw <app> <command> \\[args...\\]")

mcs_smoke(NAME bang_history_index_gone EXIT 1
    ARGS ${BASE} -n -c "fsw ds noop" -c "! 0"
    EXPECT "usage: ! fsw <app> <command>" REJECT "seq=1")

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
    EXPECT "unknown target 'nope'.*available: fsw, fsw-dry")
mcs_smoke(NAME missing_config EXIT 3
    ARGS --config ${DATA}/does_not_exist.json -c "fsw ds noop"
    EXPECT "config file not found")
mcs_smoke(NAME bad_catalog EXIT 3
    ARGS --config ${DATA}/bad_targets.json --no-log -c "fsw ds noop"
    EXPECT "bad_catalog.json.*command code 0 is already used")
mcs_smoke(NAME list_targets EXIT 0
    ARGS --config ${SHIPPED_CONFIG} --list-targets
    EXPECT "\\* fsw +ccsds_v1 -> udp.*fsw-dry +ccsds_v1 -> dryrun")
mcs_smoke(NAME help EXIT 0 ARGS --help EXPECT "Exit codes: 0 ok")
mcs_smoke(NAME version EXIT 0 ARGS --version EXPECT "mcs [0-9]+\\.[0-9]+\\.[0-9]+")

# ---- session commands, scripts, verbose, log ---------------------------------------
mcs_smoke(NAME script_file EXIT 0
    ARGS ${BASE} -n -f ${DATA}/script.txt
    EXPECT "fsw ds noop.*fsw ds set_app_state enable.*fsw ds reset")

mcs_smoke(NAME target_command EXIT 0
    ARGS ${BASE} -n -c "target"
    EXPECT "target: +fsw\npipeline: ccsds_v1 \\(little-endian\\) -> dryrun.*ds +mid=0x194B")

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

# ---- script checks ----------------------------------------------------------------------
mcs_smoke(NAME check_good_script EXIT 0
    ARGS ${BASE} --check -f ${DATA}/checks_ok.mcs
    EXPECT "checks_ok\\.mcs: ok, 10 line\\(s\\) checked, nothing sent"
    REJECT "dry-run|sent fsw")

# A script is checked as a whole first: line 2 is valid but is not sent either.
mcs_smoke(NAME script_with_typo_is_not_run EXIT 1
    ARGS ${BASE} -n -f ${DATA}/checks_typo.mcs
    EXPECT "checks_typo\\.mcs:5: error: unknown command: fsw ds nop\n.*checks_typo\\.mcs: not run: 1 error"
    REJECT "\\[dry-run\\]")

# The shipped test scripts still match the catalogs.
mcs_smoke(NAME check_script_ds_full EXIT 0
    ARGS ${BASE} --check -f ${PROJECT_SOURCE_DIR}/scripts/ds_full.mcs
    EXPECT "ds_full\\.mcs: ok, [0-9]+ line\\(s\\) checked")
mcs_smoke(NAME check_script_ds_add_mid EXIT 0
    ARGS ${BASE} --check -f ${PROJECT_SOURCE_DIR}/scripts/ds_add_mid.mcs
    EXPECT "ds_add_mid\\.mcs: ok, [0-9]+ line\\(s\\) checked")
mcs_smoke(NAME check_script_ds_smoke EXIT 0
    ARGS ${BASE} --check -f ${PROJECT_SOURCE_DIR}/scripts/ds_smoke.mcs
    EXPECT "ds_smoke\\.mcs: ok, [0-9]+ line\\(s\\) checked")

mcs_smoke(NAME check_command_lines EXIT 1
    ARGS ${BASE} --check -c "fsw ds noop" -c "expect ds.hk cmd_acepted_counter == 1"
    EXPECT "error: expect: ds.hk has no field 'cmd_acepted_counter' \\(did you mean cmd_accepted_counter\\?\\)"
    REJECT "\\[dry-run\\]")

mcs_smoke(NAME check_needs_lines EXIT 2 ARGS ${BASE} --check EXPECT "--check needs -f FILE or -c CMD")

# No FSW: the check fails at its deadline (a private telemetry port avoids clashes).
mcs_smoke(NAME expect_without_telemetry_fails EXIT 1
    ARGS ${BASE} -n --tlm-port 45999 -c "expect ds.hk within 0.2"
    EXPECT "FAIL expect ds.hk within 0\\.2 s  \\(no ds\\.hk packet since the start\\)\nresult: FAIL  checks: 0 passed, 1 failed")

mcs_smoke(NAME undefined_variable EXIT 1
    ARGS ${BASE} -n --tlm-port 45998 -c "fsw ds noop" -c "fsw ds set_app_state $s"
    EXPECT "\\[dry-run\\] fsw ds noop.*error: undefined variable \\$s")

# ---- file transfer (CFDP) ------------------------------------------------------------
# A dry-run class 1 put: the directive PDUs are shown as CF_INCOMING_PDU_MID
# (0x192D) command packets, and the transfer completes.
mcs_smoke(NAME file_put_dryrun EXIT 0
    ARGS ${BASE} -n -c "file put ${DATA}/script.txt /cf/script.txt"
    EXPECT "started 0\\.23_[0-9]+ .*\\[dry-run\\] cfdp MD 0\\.23_[0-9]+ -> 0\\.24  19 2D C0 00 .*\\[dry-run\\] cfdp EOF 0\\.23_[0-9]+ -> 0\\.24  19 2D .*done: /cf/script\\.txt")

mcs_smoke(NAME file_put_missing_file EXIT 1
    ARGS ${BASE} -n -c "file put no-such-file.bin /cf/x.bin"
    EXPECT "error: file put: no such file: no-such-file\\.bin"
    REJECT "cfdp MD")

mcs_smoke(NAME file_put_bad_remote_path EXIT 1
    ARGS ${BASE} -n -c "file put ${DATA}/script.txt cf/x.bin"
    EXPECT "remote path must start with '/'"
    REJECT "cfdp MD")

mcs_smoke(NAME file_get_needs_a_live_target EXIT 1
    ARGS ${BASE} -n -c "file get /cf/x.bin"
    EXPECT "error: file get: needs a live target"
    REJECT "playback_file")

mcs_smoke(NAME file_put_class2_needs_a_live_target EXIT 1
    ARGS ${BASE} -n -c "file put ${DATA}/script.txt /cf/x.bin class2"
    EXPECT "class 2 needs a live target")

mcs_smoke(NAME file_list_empty EXIT 0
    ARGS ${BASE} -n -c "file list"
    EXPECT "no transfers yet")
