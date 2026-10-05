# cFE reference sources (test only)

`ccsds.h` and `ccsds.c` are unmodified copies from Core Flight Executive 6.7
(GSC-18128-1, Copyright (c) 2006-2019 United States Government as represented
by the Administrator of NASA), licensed under the Apache License 2.0 (see the
file headers).

They are compiled only into `fswcli_oracle_tests`, with stand-in headers from
`../stubs/`, to check that fswcli builds byte-identical packets. They are not
part of the fswcli executable.
