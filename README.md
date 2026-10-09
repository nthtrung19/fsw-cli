# mcs

Command-line tool for sending commands to cFS flight software.

```
$ mcs
mcs 0.2.0  target 'sil': ccsds_v1 (little-endian) -> udp://127.0.0.1:1234
mcs> fsw ds set_app_state enable
sent fsw ds set_app_state enable -> udp://127.0.0.1:1234  (12 bytes, apid=0x14B seq=0)
```

Arguments are validated before anything is sent; packets are byte-identical to
what cFE 6.7 builds (verified against cFE's own `ccsds.c` in the test suite).
Apps, commands and targets are data files; protocols and transports are
plug-ins. See [docs/DESIGN.md](docs/DESIGN.md) for the full design.

## Build

Requirements: Linux, CMake ≥ 3.16, a C++17 compiler (tested with GCC 13).

```sh
git clone --recurse-submodules <repo-url> mcs      # or extract the archive
cd mcs
cmake -S . -B build
cmake --build build -j
ctest --test-dir build                                  # 126 tests
```

Missing submodules: `git submodule update --init --recursive`.
Moved or copied the project? Delete `build/` and configure again (CMake caches
absolute paths).

Third-party code lives in `external/` as git submodules pinned to release tags,
so the build needs no network once the repository is checked out. The only local
change is `external/patches/cli-tab-completion.patch` (tab completion lists just
the word being completed), which CMake applies to `external/cli` when
configuring:

| Library | Version | Used by |
|---|---|---|
| [daniele77/cli](https://github.com/daniele77/cli) | v2.2.0 | interactive menu (`src/ui`, `src/app`) |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | config files (`src/config` only) |
| [googletest](https://github.com/google/googletest) | v1.17.0 | tests only |

## Usage

```sh
./build/src/mcs                                   # interactive session
./build/src/mcs -c "fsw ds noop"                  # one command, then exit
./build/src/mcs -n -c "fsw ds set_app_state enable"   # dry run: print, don't send
./build/src/mcs -f commands.txt                   # script, one command per line
./build/src/mcs --target sil-dry                  # pick a target profile
./build/src/mcs --list-targets
./build/src/mcs --help
```

Exit codes: `0` ok, `1` a command failed, `2` bad options, `3` configuration error.

### Commands in a session

| Command | Effect |
|---|---|
| `fsw <app> <cmd> [args]` | Send a command, e.g. `fsw ds set_app_state disable` |
| `fsw`, `ds`, `..` | Navigate menus; inside `ds>` type just `noop` |
| `help` | List commands of the current menu, with parameters |
| `target` | Show the active target, protocol stack and catalog |
| `verbose on\|off` | Hexdump every sent packet |
| `raw <hex bytes>` | Send pre-built bytes, e.g. `raw 19 4B C0 00 00 01 6C 00` |
| `arm` | Allow the next command marked `critical` |
| `history`, `exit` | Library built-ins |

Session commands (`target`, `verbose`, `raw`, `arm`) are typed at the top level
(`mcs>`).

Every sent packet is appended to `mcs-YYYYMMDD.log` in the current directory
(`--log-dir DIR` to change, `--no-log` to disable).

### Configuration

`config/targets.json` holds target profiles (byte order, catalog files,
packet format, optional wrapping layers, transport). The file is found via
`--config FILE`, then `$mcs_CONFIG`, then `./config/targets.json`, then the
`config/` directory next to the build or install tree.

`config/catalog/<app>.json` defines an app's commands, grouped by the MID they
are sent on: `"mids": [ { "mid": ..., "commands": [...] }, ... ]` (an app with a
single MID may write `"mid"` + `"commands"` instead). `fsw ds noop` and
`fsw ds hk` can thus go to different MIDs. Command codes are unique per MID,
command names per app, and a MID belongs to one app. Supported field
types: `u8..u64`, `i8..i64`, `f32`, `f64`, `string` (with `size`), `padding`
(with `size`); integer fields may have `enum`, `min`, `max`. Enum fields accept
only their listed values, by name or number.

## Extending

| To add | Do this | Rebuild? |
|---|---|---|
| A command | Add an entry to `config/catalog/<app>.json` | No |
| An app (`ci`, `to`, `fm`, ...) | New `config/catalog/<app>.json`, list it in the target's `catalog` | No |
| A target | New entry in `config/targets.json` | No |
| A packet format, wrapping layer (e.g. CSP) or transport | Implement `IPacketFormat` / `IFramingLayer` / `ITransport`, add one line in `src/registry/builtin_plugins.cpp` | Yes |

## Layout

```
config/         targets.json, catalog/*.json (shipped configuration)
docs/DESIGN.md  design specification
external/       third-party submodules (do not edit; local fixes go in external/patches/)
src/core        bytes, byte order, hex, plug-in options, errors
src/catalog     app/command/field definitions + validation
src/encode      arguments -> payload bytes
src/protocol    IPacketFormat, IFramingLayer, CcsdsV1Format
src/transport   ITransport, UdpTransport, DryRunTransport
src/registry    plug-in names -> factories (builtin_plugins.cpp)
src/pipeline    format -> layers -> transport
src/config      JSON files -> the objects above (only JSON-aware module)
src/log         packet audit log
src/service     CommandService: one command end to end
src/ui          menu tree from the catalog (only module using the cli library)
src/app         main, command-line options
tests/unit        GoogleTest per module
tests/oracle      mcs packets vs. cFE 6.7 ccsds.h/ccsds.c
tests/integration real UDP on loopback
tests/smoke       the executable, checked for output and exit codes
```

## Testing against cFS

With cFS running and `ci_lab` listening on 127.0.0.1:1234:

1. `mcs -c "fsw ds noop"` → DS no-op event message; DS command counter +1.
2. `mcs -c "fsw ds set_app_state disable"` then `enable` → DS state changes in events / housekeeping.
3. `mcs -c "fsw ds set_app_state maybe"` → rejected locally, nothing sent (exit 1).

UDP is fire-and-forget: "sent" means the datagram left this machine. Confirm
reception in the cFS event log.
