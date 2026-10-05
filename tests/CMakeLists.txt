# ----------------------------------------------------------------------------
# M1 smoke tests: run the real executable in batch mode and check its output.
# Unit tests (golden packet vectors) are added in M2.
# ----------------------------------------------------------------------------

# One-line nested call reaches the ds command through the fsw submenu.
add_test(NAME smoke_inline_submenu
         COMMAND fswcli -c "fsw ds noop")
set_tests_properties(smoke_inline_submenu PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[M1\\] fsw ds noop")

# Arguments are passed through to the handler untouched.
add_test(NAME smoke_args_passthrough
         COMMAND fswcli -c "fsw ds set_app_state enable")
set_tests_properties(smoke_args_passthrough PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[M1\\] fsw ds set_app_state enable")

# Several -c options run in order.
add_test(NAME smoke_multiple_commands
         COMMAND fswcli -c "fsw ds noop" -c "fsw ds reset")
set_tests_properties(smoke_multiple_commands PROPERTIES
    PASS_REGULAR_EXPRESSION "fsw ds noop[^\n]*\n[^\n]*fsw ds reset")

# Unknown command -> non-zero exit code (important for scripts).
add_test(NAME smoke_unknown_command_fails
         COMMAND fswcli -c "fsw ds does_not_exist")
set_tests_properties(smoke_unknown_command_fails PROPERTIES WILL_FAIL TRUE)

# Script file: comments and blank lines are skipped.
add_test(NAME smoke_script_file
         COMMAND fswcli -f ${CMAKE_CURRENT_SOURCE_DIR}/data/smoke_script.txt)
set_tests_properties(smoke_script_file PROPERTIES
    PASS_REGULAR_EXPRESSION "fsw ds noop[^\n]*\n[^\n]*fsw ds set_app_state disable")
