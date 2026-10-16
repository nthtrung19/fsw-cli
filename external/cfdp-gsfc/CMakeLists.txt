# GSFC CFDP library, as used by the cFS CF application 2.2.1 (see README.md).
# Built as plain C with warnings off: it is third-party code (like the other
# externals) and must stay unmodified.
add_library(cfdp_gsfc STATIC
    src/aaa.c
    src/callbacks.c
    src/cfdp_lib.c
    src/default_filestore.c
    src/default_indication.c
    src/event.c
    src/machine_list.c
    src/message_class.c
    src/mib.c
    src/misc.c
    src/nak.c
    src/nak_mem.c
    src/pdu.c
    src/pdu_as_string.c
    src/r1.c
    src/r2.c
    src/s1.c
    src/s2.c
    src/timer.c
    src/utils.c)

target_compile_options(cfdp_gsfc PRIVATE -w)
target_include_directories(cfdp_gsfc PRIVATE src)
# config/ first: our cfdp_config.h replaces the flight one.
target_include_directories(cfdp_gsfc SYSTEM PUBLIC config include)
set_target_properties(cfdp_gsfc PROPERTIES POSITION_INDEPENDENT_CODE ON)
