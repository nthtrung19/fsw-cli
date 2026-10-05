# fswcli

Command-line tool for sending commands to cFS flight software.

```
fswcli> fsw ds set_app_state enable
```

The prototype targets the DS (Data Storage) app over CCSDS v1 / UDP. The design
keeps apps, protocols and transports pluggable so `ci`, `to`, `fm`, CSP, etc.
can be added without changing existing logic.

## Status

| Milestone | Content | State |
|---|---|---|
| M1 | Skeleton, `cli` submodule, menu tree, interactive/batch modes | **done** |
| M2 | Byte writer, CCSDS v1 packet format, golden-vector tests | next |
| M3 | Command catalog, payload encoder, DS commands, dry-run | |
| M4 | UDP transport, target config, sending to cFS | |
| M5 | Remaining DS commands | |
| M6 | JSON catalog (optional), more apps | |

## Build

Requirements: CMake >= 3.16, a C++17 compiler (tested with GCC 13), Linux.

```sh
git clone --recurse-submodules <repo-url> fsw-cli
cd fsw-cli
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If you cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

## Usage

```sh
./build/src/fswcli                          # interactive session
./build/src/fswcli -c "fsw ds noop"         # one command, then exit
./build/src/fswcli -c "fsw ds noop" -c "fsw ds reset"
./build/src/fswcli -f commands.txt          # script: one command per line, # comments
```

In batch modes the exit code is non-zero if any command failed.

Interactive navigation: commands can be typed in one line (`fsw ds noop`) or
by entering menus (`fsw`, then `ds`, then `noop`); `..` goes up, `help` lists
commands, `exit` quits.

## Layout

```
external/cli/   daniele77/cli, git submodule pinned to v2.2.0 (do not edit)
src/app/        main, command-line options
src/ui/         menu tree; the ONLY module that includes <cli/...>
tests/          tests (smoke tests now; unit tests from M2)
```

Planned (added milestone by milestone): `src/core`, `src/catalog`,
`src/encode`, `src/protocol`, `src/transport`, `src/service`.

## Third-party

- [daniele77/cli](https://github.com/daniele77/cli), Boost Software License 1.0.
