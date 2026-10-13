# mcs

Command-line tool for sending commands to cFS flight software and watching
its telemetry.

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
ctest --test-dir build                                  # 184 tests
```

Missing submodules: `git submodule update --init --recursive`.
Moved or copied the project? Delete `build/` and configure again (CMake caches
absolute paths).

Third-party code lives in `external/` as git submodules pinned to release tags,
so the build needs no network once the repository is checked out. The only local
change is `external/patches/cli-tab-completion.patch` (tab completion lists just
the word being completed, one per line, with its description), which CMake applies to `external/cli` when
configuring:

| Library | Version | Used by |
|---|---|---|
| [daniele77/cli](https://github.com/daniele77/cli) | v2.2.0 | interactive menu (`src/ui`, `src/app`) |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | config files (`src/config` only) |
| [googletest](https://github.com/google/googletest) | v1.17.0 | tests only |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | v7.0.3 | full-screen session (`src/ui/tui_session.cpp`) |

## Usage

```sh
./build/src/mcs                                   # interactive session (full screen)
./build/src/mcs --plain                           # interactive, line mode
./build/src/mcs --tlm-port 5012                   # receive telemetry on another port
./build/src/mcs -c "fsw ds noop"                  # one command, then exit
./build/src/mcs -n -c "fsw ds set_app_state enable"   # dry run: print, don't send
./build/src/mcs -f commands.txt                   # script, one command per line
./build/src/mcs --target sil-dry                  # pick a target profile
./build/src/mcs --list-targets
./build/src/mcs --help
```

Exit codes: `0` ok, `1` a command failed, `2` bad options, `3` configuration error.

### Full-screen session

On a terminal, the interactive session fills the screen. The **top** part shows
telemetry received from the flight software; the **bottom** part is for
commanding: the output of your commands, the `mcs>` input line and key hints.

```
┌ TLM udp://0.0.0.0:5011 ● 1532 pkts · 3 unknown · last 0.2 s ago   F1 dash  F2 monitor  F4 stats ⠙ ┐
│╭ packets (F6) ──────╮╭ ds.hk 0x08B8 seq 412 · ×412 · 0.4 s ┐╭ ? 0x0899 seq 9 · 42 B · ×9 ──────╮ │
││ [x] ds.hk        ● ││ cmd_accepted                12 ▲   ││ 0000: 08 99 C0 09 00 23 ...      │ │
││ [ ] evs.hk       ● ││ app_state               enable     ││ no definition: bytes 0-11 header │ │
││ [x] ? 0x0899     ● │╰────────────────────────────────────╯╰──────────────────────────────────╯ │
│╰────────────────────╯                                                                             │
│─ events ──────────────────────────────────────────────────────────────────────────────────────────│
│ 11:23:26.081  INFO   DS     12  No-op command, Version 2.5.0.0                                    │
╞═══════════════════════════════════════════════════════════════════════════════════════════════════╡
│ > fsw ds noop                                                                                     │
│ sent fsw ds noop -> udp://127.0.0.1:1234  (8 bytes, apid=0x14B seq=0)                             │
│ mcs> █                                                                                            │
└ Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · F1/F2/F4 views · F5 pause ────────────┘
```

| Key | Effect |
|---|---|
| F1 | Dashboard: packet checklist on the left (every catalog packet, ● received / ○ not yet, then MIDs received without a definition); the ticked packets on the right (latest values, changes highlighted with ▲/▼, age turns yellow/red when stale; unknown MIDs as a hexdump); EVS event log below. Nothing is ticked at start. |
| F6 | Move the keys to the checklist: ↑↓ move, Space/Enter tick, PgUp/PgDn page; Esc or F6 returns to the command line, and so does typing any character |
| F2 | Packet monitor: every received and sent packet, with sequence-gap, length, unknown-MID and malformed checks |
| F4 | Statistics per MID: count, rate, gaps, length errors, last seen |
| F5 | Pause/resume the display (receiving continues) |
| Ctrl+↑/↓ (or Alt+↑/↓) | Scroll the ticked packets / packet monitor |
| PgUp/PgDn | Scroll the command output |
| Ctrl-C | Clear the input line; on an empty line, quit (as do Ctrl-D and `exit`) |

Telemetry is configured per target (`"telemetry"` in `config/targets.json`): a
receiver (UDP port 5011 by default, TO's default destination) and telemetry
definition files in `config/telemetry/`. Start the stream from the session, e.g.
`fsw to enable_output 127.0.0.1 5011`. Packets without a definition still show
up (unknown MIDs, monitor, statistics). When stdin or stdout is not a terminal,
or with `--plain`, the session is the line-mode prompt, without telemetry.

In an interactive session TAB completes command names (listed with their
descriptions) and enum arguments (`fsw ds set_app_state <TAB>` → `disable`,
`enable`); for other arguments it shows what is expected, e.g.
`<file_table_index: u16 ..15>   FileTableIndex  (argument 1 of 2)`.

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
| `tlm view dash\|monitor\|stats` | Switch the telemetry view (same as F1/F2/F4) |
| `tlm show <packet>\|<mid>\|all`, `tlm hide ...` | Tick/untick packets in the dashboard checklist, e.g. `tlm show ds.hk`, `tlm show 0x0899` |
| `tlm monitor <app>\|<mid>\|all` | Packet monitor: only one app (sent and received) or MID, e.g. `tlm monitor 0x0808` |
| `tlm pause [on\|off]`, `tlm clear`, `tlm status` | Freeze the display; forget values and statistics; show receiver and counters |
| `history`, `exit` | Library built-ins |

Session commands (`target`, `verbose`, `raw`, `arm`, `tlm ...`) are typed at the
top level (`mcs>`). The `tlm` commands exist in the full-screen session only.

Every sent packet is appended to `mcs-YYYYMMDD.log` in the current directory
(`--log-dir DIR` to change, `--no-log` to disable).

### Configuration

`config/targets.json` holds target profiles (byte order, catalog files,
packet format, optional wrapping layers, transport). The file is found via
`--config FILE`, then `$mcs_CONFIG`, then `./config/targets.json`, then the
`config/` directory next to the build or install tree.

`config/telecommand/<app>.json` defines an app's commands, grouped by the MID they
are sent on: `"mids": [ { "mid": ..., "commands": [...] }, ... ]` (an app with a
single MID may write `"mid"` + `"commands"` instead). `fsw ds noop` and
`fsw ds hk` can thus go to different MIDs. Command codes are unique per MID,
command names per app, and a MID belongs to one app. Supported field
types: `u8..u64`, `i8..i64`, `f32`, `f64`, `string` (with `size`), `padding`
(with `size`); integer fields may have `enum`, `min`, `max`. Enum fields accept
only their listed values, by name or number. Any non-padding field may have a
`default` (string or number), used when its argument is omitted; only trailing
arguments can be omitted, and defaults are validated at start-up.

`config/telemetry/<app>.json` defines an app's telemetry packets, one per MID,
with fields in payload order after the 12-byte telemetry header (same field
types; `enum`, `units`, `hex` for display). `"event": true` marks EVS event
packets, which go to the event log. `evs.json` (cFE 6.7 long/short event
messages) is shipped; housekeeping definitions are written from each app's
`*_msg.h`, like the command catalogs.

## Extending

| To add | Do this | Rebuild? |
|---|---|---|
| A command | Add an entry to `config/telecommand/<app>.json` | No |
| An app (`ci`, `to`, `fm`, ...) | New `config/telecommand/<app>.json`, list it in the target's `catalog` | No |
| A target | New entry in `config/targets.json` | No |
| A telemetry packet | Add it to `config/telemetry/<app>.json`, list the file in the target's `telemetry.catalog` | No |
| A packet format, wrapping layer (e.g. CSP), transport or telemetry receiver | Implement `IPacketFormat` / `IFramingLayer` / `ITransport` / `IReceiver`, add one line in `src/registry/builtin_plugins.cpp` | Yes |

## Layout

```
config/         targets.json, telecommand/*.json, telemetry/*.json (shipped configuration)
docs/DESIGN.md  design specification
external/       third-party submodules (do not edit; local fixes go in external/patches/)
src/core        bytes, byte order, hex, plug-in options, errors
src/catalog     app/command/field definitions + validation
src/encode      arguments -> payload bytes
src/protocol    IPacketFormat, IFramingLayer, CcsdsV1Format
src/transport   ITransport, UdpTransport, DryRunTransport; IReceiver, UdpReceiver
src/registry    plug-in names -> factories (builtin_plugins.cpp)
src/pipeline    format -> layers -> transport
src/config      JSON files -> the objects above (only JSON-aware module)
src/log         packet audit log
src/service     CommandService: one command end to end
src/telemetry   telemetry catalog, CCSDS telemetry parser, decoder, TlmStore, receiver thread
src/ui          menu tree, full-screen session (only modules using the cli and FTXUI libraries)
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
4. In `mcs` (full screen): `fsw to enable_output 127.0.0.1 5011`, then `fsw ds noop` →
   the DS no-op event appears in the event log; the packet monitor (F2) shows
   the TX command and the received packets.

UDP is fire-and-forget: "sent" means the datagram left this machine. Confirm
reception in the cFS event log.
