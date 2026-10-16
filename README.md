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
ctest --test-dir build                                  # 288 tests
```

Missing submodules: `git submodule update --init --recursive`.
Moved or copied the project? Delete `build/` and configure again (CMake caches
absolute paths).

Third-party code lives in `external/` as git submodules pinned to release tags,
so the build needs no network once the repository is checked out. `external/cli`
points to a fork (github.com/nthtrung19/cli) that carries the mcs changes:
completions with descriptions and hints, and structured entries for the
two-column `help`:

| Library | Version | Used by |
|---|---|---|
| [daniele77/cli](https://github.com/daniele77/cli) (fork) | v2.2.0 + mcs changes | interactive menu (`src/ui`, `src/app`) |
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

On a terminal, the interactive session fills the screen and has two screens.
A header line (target, screen tabs, telemetry link status) and a key-hint line
are always visible.

**F1 Console**: flight software events on top, the output of your commands
below, then the `mcs>` input line and the Tab-completion list.

```
 mcs · sil   F1 Console   F2 Telemetry          TLM udp://0.0.0.0:5011 ● 1532 pkts · last 0.2 s ⠙
╭ EVENTS 37 ─────────────────────────────────────────────────────────────────────────────────────╮
│ time          type   app         id    message                                                 │
│ 09:00:19.414  INFO   TO          3     ENABLE OUTPUT CMD Succesful for Routes:0x0001           │
│ 09:00:20.002  ERROR  DS          7     Invalid command length: expected 8, got 12              │
╰────────────────────────────────────────────────────────────────────────────────────────────────╯
╭ COMMAND OUTPUT ────────────────────────────────────────────────────────────────────────────────╮
│ > fsw ds noop                                                                                  │
│ sent fsw ds noop -> udp://127.0.0.1:1234  (8 bytes, apid=0x14B seq=0)                          │
╰────────────────────────────────────────────────────────────────────────────────────────────────╯
 mcs> █
 Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ events · F2 telemetry · F5 pause
```

**F2 Telemetry**: the packet checklist on the left (every catalog packet,
● received / ○ not yet, then MIDs received without a definition) and a card for
each ticked packet on the right. Nothing is ticked at start.

```
╭ PACKETS 2/5 ──────────────╮ ╭ DS  hk 0x08B8  ⚠ 1 gap   seq 412 · ×412 · 0.4 s ╮ ╭ ?  unknown 0x0899  seq 9 · 42 B ╮
│ ◉ ds.hk                 ● │ │ cmd_accepted ······················· 12 ▲     │ │ 0000  08 99 C0 09 00 23 …  ...#  │
│ ○ evs.hk                ● │ │ app_state ························ enable       │ ╰──────────────────────────────────╯
│ ◉ ? 0x0899              ● │ ╰───────────────────────────────────────────────╯
╰───────────────────────────╯
 ↑↓ move · Space tick · PgUp/PgDn scroll cards · F5 pause · F1 console (or just type) · Ctrl-C quit
```

Cards have a border in the app's colour (grey when the packet is stale, > 30 s),
the packet's sequence count, number received and age, and ⚠ markers for sequence
gaps and length errors. Values that changed in the last second are highlighted
with ▲/▼; enum values are coloured (enabled green, disabled grey, errors red).
Large packets spread over several columns. A MID without a definition is shown
as a hexdump (header bytes in cyan, with an ASCII column).

| Key | Effect |
|---|---|
| F1 / F2 | Console / Telemetry screen |
| F5 | Pause/resume the display (receiving continues) |
| Ctrl-C | Clear the input line; on an empty line, quit (as do Ctrl-D and `exit`) |
| **Console** | |
| PgUp/PgDn | Scroll the command output |
| Ctrl+↑/↓ (or Alt+↑/↓) | Scroll the events |
| **Telemetry** | |
| ↑/↓, Home/End | Move in the checklist |
| Space or Enter | Tick/untick the packet under the cursor |
| PgUp/PgDn, Ctrl+↑/↓ | Scroll the cards |
| Esc, or typing any character | Back to the console (the character is typed into the input line) |

Telemetry is configured per target (`"telemetry"` in `config/targets.json`): a
receiver (UDP port 5011 by default, TO's default destination) and telemetry
definition files in `config/telemetry/`. Start the stream from the session, e.g.
`fsw to enable_output 127.0.0.1 5011`. Packets without a definition still show
up in the checklist and the header counters. When stdin or stdout is not a terminal,
or with `--plain`, the session is the line-mode prompt, without telemetry.

In an interactive session TAB completes command names and enum arguments
(`fsw ds set_app_state <TAB>` → `disable`, `enable`); in the full-screen console
the candidates are listed by name in a grid of columns. For other arguments it
shows what is expected, e.g.
`<file_table_index: u16 ..15>   FileTableIndex  (argument 1 of 2)`.
`help` lists the commands of the current menu with their descriptions in two
columns (one on narrow terminals), names in colour; long descriptions wrap under
their column.

### Commands in a session

| Command | Effect |
|---|---|
| `fsw <app> <cmd> [args]` | Send a command, e.g. `fsw ds set_app_state disable` |
| `fsw`, `ds`, `..` | Navigate menus; inside `ds>` type just `noop` |
| `help` | List commands of the current menu with their descriptions (parameters: TAB after the command) |
| `target` | Show the active target, protocol stack and catalog |
| `verbose on\|off` | Hexdump every sent packet |
| `raw <hex bytes>` | Send pre-built bytes, e.g. `raw 19 4B C0 00 00 01 6C 00` |
| `arm` | Allow the next command marked `critical` |
| `tlm view console\|telemetry` | Switch screen (same as F1/F2) |
| `tlm show <packet>\|<mid>\|all`, `tlm hide ...` | Tick/untick packets in the telemetry checklist, e.g. `tlm show ds.hk`, `tlm show 0x0899` |
| `tlm pause [on\|off]`, `tlm clear`, `tlm status` | Freeze the display; forget values, events and counters; show receiver and counters |
| `file put <local> <remote> [class1\|class2]` | Send a ground file to the FSW, e.g. `file put test.bin /cf/test.bin class2` |
| `file get <remote> [local] [class1\|class2]` | Ask CF to send a file and receive it, e.g. `file get /cf/log/hk_0001.dat` |
| `file list`, `file status <id>` | Active and recent transfers; details of one (`0.24_7`) |
| `file cancel\|abandon <id>\|all`, `file suspend\|resume <id>` | Control a transfer |
| `history`, `exit` | Library built-ins |

Session commands (`target`, `verbose`, `raw`, `arm`, `tlm ...`, `file ...`) are typed at the
top level (`mcs>`). The `tlm` commands exist in the full-screen session only; the
`file` commands exist when the target has a `cfdp` block.

### File transfer (CFDP)

mcs is the ground CFDP entity (`0.23`) for CF (`0.24`). It runs the same CFDP
engine as CF 2.2.1 (the GSFC library, `external/cfdp-gsfc`), so both sides speak
exactly the same protocol.

- **`file put`**: the file goes up as CFDP PDUs, each sent as a command on
  `0x192D` (CF_INCOMING_PDU_MID) through the normal pipeline, paced to
  `pdus_per_second`. Class 2: CF acknowledges and asks again for lost data.
- **`file get`**: mcs sends `fsw cf playback_file <class> 0 0 keep 0.23 <remote> <remote>`;
  CF's PDUs come back as telemetry on `0x092D` and the file is stored as
  `<downlink_dir>/<remote path>` (e.g. `downlink/cf/log/hk_0001.dat`), or moved
  to `[local]`. Files CF sends without a `file get` (polling directories,
  playback commands typed by hand) are stored too.
- **Batch mode** (`-c`, `-f`): `file put/get` wait until the transfer ends and
  print progress; the exit code is 1 if it failed. In a session they return
  at once.
- **Full-screen session**: the header shows `FILES ↑1 ↓1` while transfers run,
  and the F1 console shows a TRANSFERS strip (progress bar, rate, ETA per
  transfer) between EVENTS and COMMAND OUTPUT; start and end notices go to the
  command output (✔ done, ✘ failed, ⊘ cancelled). Quitting while transfers run
  asks once more.
- **Safety**: received files never leave `downlink_dir` (names with `..` are
  refused), never overwrite a file (`name(1).ext`), and appear only after the
  checksum is good (they are written to `downlink_dir/.part/` first).
- Each transfer adds a `cfdp=start` and a `cfdp=end` line to the packet log;
  directive PDUs (MD, EOF, ACK, NAK, FIN) are logged like commands, file data
  is counted.

Downlink speed is set by CF: one wakeup per second, 4 engine cycles, one
4000-byte PDU per cycle, so about 16 KB/s and one file at a time. If a `file get`
never starts, check CF's events and `cf.hk err_counter` (missing file, queue
full), and that TO sends telemetry (`fsw ci enable_to`).

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

A target's optional `"cfdp"` block turns on file transfer (it needs the
`telemetry` block: CF's PDUs arrive as telemetry). Every key is optional; the
defaults are CF 2.2.1 on the SIL: `local_entity` `"0.23"`, `remote_entity`
`"0.24"`, `uplink_mid` `"0x192D"`, `downlink_mid` `"0x092D"`, `chunk_size` 512
(file bytes per uplink PDU, limited by CI's datagram buffer), `pdus_per_second`
100, `downlink_dir` `"downlink"`, class 2 timers `ack_timeout` 10, `ack_limit` 2,
`nak_timeout` 5, `nak_limit` 3, `inactivity_timeout` 30 (seconds),
`save_incomplete` false.

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
external/       third-party submodules (do not edit; cli changes go in its fork, other fixes in external/patches/)
external/cfdp-gsfc  the GSFC CFDP library of CF 2.2.1, vendored unmodified except config/cfdp_config.h
src/core        bytes, byte order, hex, plug-in options, errors
src/catalog     app/command/field definitions + validation
src/encode      arguments -> payload bytes
src/protocol    IPacketFormat, IFramingLayer, CcsdsV1Format
src/transport   ITransport, UdpTransport, DryRunTransport; IReceiver, UdpReceiver
src/registry    plug-in names -> factories (builtin_plugins.cpp)
src/pipeline    format -> layers -> transport
src/config      JSON files -> the objects above (only JSON-aware module)
src/log         packet audit log
src/service     CommandService: one command end to end; FileTransferService: file put/get
src/telemetry   telemetry catalog, CCSDS telemetry parser, decoder, TlmStore, receiver thread
src/cfdp        CFDP engine interface, adapter to the GSFC library (gsfc_engine, C bridge), PDU inspector
src/ui          menu tree, full-screen session (only modules using the cli and FTXUI libraries)
src/app         main, command-line options
tests/unit        GoogleTest per module
tests/oracle      mcs packets vs. cFE 6.7 ccsds.h/ccsds.c
tests/integration real UDP on loopback
tests/smoke       the executable, checked for output and exit codes
tests/e2e         file put/get with the executable against tests/tools/cfdp_peer (a CF stand-in)
```

## Testing against cFS

With cFS running and `ci_lab` listening on 127.0.0.1:1234:

1. `mcs -c "fsw ds noop"` → DS no-op event message; DS command counter +1.
2. `mcs -c "fsw ds set_app_state disable"` then `enable` → DS state changes in events / housekeeping.
3. `mcs -c "fsw ds set_app_state maybe"` → rejected locally, nothing sent (exit 1).
4. In `mcs` (full screen): `fsw to enable_output 127.0.0.1 5011`, then `fsw ds noop` →
   the DS no-op event appears in the console's event pane; after `fsw ds hk`, the
   DS housekeeping card (F2, tick `ds.hk`) shows the command counter +1.

5. File transfer, with TO sending telemetry (step 4):
   - `mcs -c "file get /cf/<some file> class1"` → CF events "Outgoing trans started"
     and "…success"; the file in `downlink/cf/...` matches the FSW's copy (`cmp`).
   - `mcs -c "file put test.bin /cf/test.bin class1"`, then with `class2` → CF events
     "Incoming trans started" and "…success"; `cf.hk up_success_counter` +1;
     `cmp test.bin` with the SIL's `/cf/test.bin`.
   - In the full-screen session, start a `file put` and a `file get` together and
     watch both in the TRANSFERS strip.

UDP is fire-and-forget: "sent" means the datagram left this machine. Confirm
reception in the cFS event log.
