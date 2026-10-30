# mcs

Command-line tool for sending commands to cFS flight software and watching
its telemetry.

```
$ mcs
mcs 0.2.0  target 'fsw': ccsds_v1 (little-endian) -> udp://127.0.0.1:1234
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
ctest --test-dir build                                  # 343 tests
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
./build/src/mcs -f scripts/ds_smoke.mcs           # test script: commands and checks, PASS/FAIL
./build/src/mcs --check -f scripts/ds_smoke.mcs   # check a script, send nothing
./build/src/mcs --target fsw-dry                  # pick a target profile
./build/src/mcs --list-targets
./build/src/mcs --help
```

Exit codes: `0` ok, `1` a command or check failed, `2` bad options, `3` configuration error.

### Full-screen session

On a terminal, the interactive session fills the screen and has two screens.
A header line (target, screen tabs, telemetry link status) and a key-hint line
are always visible.

**F1 Console**: the **FSW CONSOLE** on top (the flight software's own console
text, see below), the output of your commands below, then the `mcs>` input line
and the Tab-completion list. F3 swaps the console text for the flight software
**EVENTS** (and back).

```
 mcs   F1 Console   F2 Telemetry          TLM udp://0.0.0.0:5011 ● 1532 pkts · last 0.2 s ⠙
╭ EVENTS 37 ─────────────────────────────────────────────────────────────────────────────────────╮
│ time          type   app         id    message                                                 │
│ 09:00:19.414  INFO   TO          3     ENABLE OUTPUT CMD Succesful for Routes:0x0001           │
│ 09:00:20.002  ERROR  DS          7     Invalid command length: expected 8, got 12              │
╰────────────────────────────────────────────────────────────────────────────────────────────────╯
╭ COMMAND HISTORY ────────────────────────────────────────────────────────────────────────────────╮
│ > fsw ds noop                                                                                  │
│ sent fsw ds noop -> udp://127.0.0.1:1234  (8 bytes, apid=0x14B seq=0)                          │
╰────────────────────────────────────────────────────────────────────────────────────────────────╯
 mcs> █
 Enter run · Tab complete · ↑↓ history · PgUp/PgDn output · Ctrl+↑↓ events · F3 events/console · F2 telemetry · F5 pause
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
| F3 | Top pane of the console: EVENTS ⇄ FSW CONSOLE (from F2 too) |
| F5 | Pause/resume the display (receiving continues) |
| Ctrl-C | Clear the input line; on an empty line, quit (as do Ctrl-D and `exit`) |
| Mouse: drag a scrollbar (┃) | Scroll that panel: the log pane and COMMAND HISTORY on the console, the cards on the telemetry screen. Shift+drag selects text for copying |
| **Console** | |
| PgUp/PgDn | Scroll the command output |
| Ctrl+↑/↓ (or Alt+↑/↓) | Scroll the events or the FSW console, whichever is shown |
| **Telemetry** | |
| ↑/↓, Home/End | Move in the checklist |
| Space, Enter or a click | Tick/untick the packet under the cursor (a click also moves the cursor there; clicking "▲/▼ N more" pages the list) |
| PgUp/PgDn, Ctrl+↑/↓ | Scroll the cards |
| Esc, or typing any character | Back to the console (the character is typed into the input line) |

Telemetry is configured per target (`"telemetry"` in `config/targets.json`): a
receiver (UDP port 5011 by default, TO's default destination) and telemetry
definition files in `config/telemetry/`. Start the stream from the session, e.g.
`fsw to enable_output 127.0.0.1 5011`. Packets without a definition still show
up in the checklist and the header counters. When stdin or stdout is not a terminal,
or with `--plain`, the session is the line-mode prompt, without telemetry.

**FSW console.** What the flight software prints on its console (`OS_printf`,
`printf`, `EVS Port1` event echoes, boot output) never goes through TO. On the
bench the BBB's UART is forwarded as UDP by a serial-to-Ethernet bridge; give the
target a `"console"` block with that port and mcs shows the text at the top of F1
(the default; F3 switches to the events and back):

```
╭ FSW CONSOLE ───────────────────────────────────────────────────────   udp://0.0.0.0:8898 ● last 0.4 s ╮
│ 10:41:07.112  EVS Port1 66/1/MAIN 1: MAIN App Initialized.                                       │
│ 10:41:07.120  IO_Lib: -12 - UartRcvTimeout Error: Timedout                                       │
╰──────────────────────────────────────────────────────────────────────────────────────────────────╯
```

Lines are rebuilt from the datagrams (a line may span several; `\r\n` or `\n`
ends it; an unfinished line, e.g. a shell prompt, shows after 0.3 s), escape
sequences are removed, and the last 2000 lines are kept. Only one program can
listen on a UDP port: close `nc -ul ... 8898` before starting mcs, otherwise the
pane shows the bind error (mcs keeps working). Console text is not saved.

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
| `! <command line>` | Send a command marked `critical` (refused without `!`): the same line with `!` in front, in any menu: `fm> ! delete_dir /cf/x`, `fsw> ! fm delete_dir /cf/x`, `mcs> ! fsw fm delete_dir /cf/x`. Covers that one command only. Not offered by TAB or `help`: the error on a critical command tells you |
| `tlm view console\|telemetry` | Switch screen (same as F1/F2) |
| `tlm pane events\|console` | Top pane of the console screen (same as F3) |
| `tlm show <packet>\|<mid>\|all`, `tlm hide ...` | Tick/untick packets in the telemetry checklist, e.g. `tlm show ds.hk`, `tlm show 0x0899` |
| `tlm pause [on\|off]`, `tlm clear`, `tlm status` | Freeze the display; forget values, events and counters; show receiver and counters |
| `file put <local> <remote> [class1\|class2]` | Send a ground file to the FSW, e.g. `file put test.bin /cf/test.bin class2` |
| `file get <remote> [local] [class1\|class2]` | Ask CF to send a file and receive it, e.g. `file get /cf/log/hk_0001.dat` |
| `file list`, `file status <id>` | Active and recent transfers; details of one (`0.24_7`) |
| `file cancel\|abandon <id>\|all`, `file suspend\|resume <id>` | Control a transfer |
| `run <script>`, `run stop`, `run` | Run a test script in the background (see [Testing with scripts](#testing-with-scripts)); stop it; show its progress |
| `history`, `exit` | Library built-ins (recall earlier commands with ↑/↓) |

Session commands (`target`, `verbose`, `raw`, `tlm ...`, `file ...`, `run ...`) are typed at the
top level (`mcs>`); `!` works in `mcs>`, `fsw>` and every app menu. The `tlm` and `run` commands exist in the full-screen session only; the
`file` commands exist when the target has a `cfdp` block.

**cFE ES files.** Some ES commands (`config/telecommand/es.json`) write a file on
the FSW's RAM disk instead of sending telemetry; fetch it with `file get` before a
reset clears `/ram`. Omit the file name to use ES's default:

| Command | File | Content (after a 64-byte cFE file header) |
|---|---|---|
| `fsw es write_syslog` | `/ram/cfe_es_syslog.log` | the system log, as text |
| `fsw es write_er_log` | `/ram/cfe_erlog.log` | the 20 Exception and Reset log entries (binary) |
| `fsw es query_all` | `/ram/cfe_es_app_info.log` | one record per app and library (binary) |
| `fsw es query_all_tasks` | `/ram/cfe_es_task_info.log` | one record per task (binary) |
| `fsw es dump_cds_registry` | `/ram/cfe_cds_reg.log` | the critical data store registry (binary) |
| `fsw es stop_perf` | `/ram/cfe_es_perf.dat` | perf data (cFS perf tool); written in the background: wait for ES event 68 |

e.g. `fsw es write_syslog`, then `file get /ram/cfe_es_syslog.log` →
`downlink/ram/cfe_es_syslog.log`. On the PC build `/ram` is the host directory
`./ram` next to `core-ntpm` (the PSP's `cfe_psp_voltab.c` maps `/ramdev0` to it) and
must exist: `mkdir ~/SDE/fsw/build/exe/ntpm/ram`, else every file command fails
with `RC = 0xFFFFFFFF`. `fsw es hk` asks for `es.hk` at once (the
scheduler sends it every 20 s). The ES commands that stop or restart software
(`restart`, `start_app`, `stop_app`, `restart_app`, `reload_app`, `delete_cds`,
`set_max_pr_count`, `rtems_shell`) are critical; `restart` also stops telemetry until
`fsw to enable_output` is sent again.

**cFE tables.** Tables are changed with TBL (`config/telecommand/tbl.json`); names are
`APP.TABLE` (`fsw tbl dump_registry` lists them):

```
file put ds_filter_tbl.tbl /cf/ds_filter_tbl.tbl   # the new table image
fsw tbl load /cf/ds_filter_tbl.tbl                 # -> inactive buffer (event 12)
fsw tbl validate inactive DS.FILTER_TBL            # the owner app checks it (event 36, or 96)
! fsw tbl activate DS.FILTER_TBL                   # critical: the app switches to it (event 37)
```

`fsw tbl abort_load <table>` drops a loaded table instead; loading again replaces it and
it must be validated again. `fsw tbl dump active <table> <file>` writes a table to a file
(same format as a load file) for `file get`. `fsw tbl send_registry <table>` sends
`tbl.registry` with the next `tbl.hk`, so follow it with `fsw tbl hk`.

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
  transfer) between EVENTS and COMMAND HISTORY; start and end notices go to the
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

### Testing with scripts

A script (`-f`) can check what the FSW does, not only send commands, so it
can serve as an automated test. Example ([scripts/ds_smoke.mcs](scripts/ds_smoke.mcs)):

```
fsw to enable_output 127.0.0.1 5011
wait 2

test DS-001 "NOOP is accepted and reported" req DS-NOOP
  save n ds.hk cmd_accepted_counter
  fsw ds noop
  expect-event DS id 31 type INFO text "NOOP command"
  expect ds.hk cmd_accepted_counter == $n+1
  expect-no-event DS type ERROR for 2
end
```

```
=== test DS-001  NOOP is accepted and reported  [DS-NOOP]
n = 4  (ds.hk cmd_accepted_counter)
sent fsw ds noop -> udp://127.0.0.1:1234  (8 bytes, apid=0x14B seq=0)
PASS expect-event DS id 31 type INFO text "NOOP command"  (after 12 ms: "NOOP command, Version 2.5.2.0")
PASS expect ds.hk cmd_accepted_counter == 5  (got 5 after 640 ms)
PASS expect-no-event DS type ERROR for 2 s
=== DS-001 PASS  (3 check(s))
result: PASS  tests: 1 passed, 0 failed; checks: 3 passed, 0 failed; commands sent: 2; errors: 0
```

| Line | Meaning |
|---|---|
| `wait <s>` | Pause (`2`, `0.5`, `500ms`). |
| `timeout <s>` | Time limit of the following checks (5 s at the start). |
| `expect <packet> [within <s>]` | A packet arrives. |
| `expect <packet> <field> <op> <value> [within <s>]` | A packet arrives whose field matches: `==` `!=` `<` `<=` `>` `>=`; the value is a number, one of the field's enum names (`enable`) or, for text fields, a string (`==`/`!=` only). |
| `expect-event <APP> [id <n>] [type <T>] [text "<part>"] [within <s>]` | An event matching all the filters (type: `DEBUG`, `INFO`, `ERROR`, `CRIT`; text: part of the message, case-insensitive). |
| `expect-no-event <APP> [id] [type] [text] [for <s>]` | Waits the whole time; fails if a matching event arrives. |
| `save <name> <packet> <field> [within <s>]` | Stores the field's latest value; later lines use `$name`, `$name+1`, `$name-1` (`$$` is a `$`). |
| `test <ID> ["title"] [req <R1,R2>]` … `end` | A test: it passes if every line inside passes. Lines outside tests still count for the result. |

`<packet>` is a telemetry packet name (`ds.hk`) or MID (`0x0948`); fields
are the names in `config/telemetry/<app>.json` (arrays as `name_0`, ...).

- **What a check sees.** Telemetry and events received *since the last
  command was sent*: an event that arrives during a `wait` after the command
  counts, older telemetry does not. `expect` passes on the first matching
  packet, so the counter check above passes on the first housekeeping packet
  after the NOOP.
- **Checked before it runs.** `-f` reads the whole script first: unknown
  commands, wrong arguments, unknown packets or fields, bad values, undefined
  variables and `test`/`end` mistakes are all reported with their line, and
  then nothing is sent. `--check` stops after this. Lines with `$name` are
  checked without the value, which is only known when the line runs.
- **Results.** One `PASS`/`FAIL` line per check (failures show the last value
  seen), one line per test, and a `result:` line. `FAIL` and `error:` lines
  carry the script location (`ds_smoke.mcs:17: FAIL ...`). Any failed check or
  line makes the exit code 1, so the script can gate a CI job.
- The checks need the target's `telemetry` block and a free receiver port.
  They work in `-f`, `-c` and `run` (below); typed one by one in a session
  they are not available, because a check blocks while it waits.

**Shipped scripts** (`scripts/`):

| Script | What it tests | Runs |
|---|---|---|
| `ds_smoke.mcs` | DS NOOP and an invalid command code | any time |
| `ds_full.mcs` | every DS command: nominal (each with its event and counters, file 15 and filter slot 2 of 0x0806 are changed and put back) and ~20 rejections, 12 of them as `raw` packets DS's own checks must catch | any time (repeatable) |
| `ds_add_mid.mcs` | adding a message ID to DS's filter table | once per FSW start (DS cannot remove it) |
| `evs_full.mcs` | cFE EVS: noop, reset, event types per app, ports (console echo off/on), log mode, event log and app data files (fetched into `downlink/ram/`), a filter on DS's noop event (first only, all, reset, delete), and 6 rejections (4 as `raw` packets); global type/app off, short format and clearing the log are left to manual tests | any time (repeatable; needs CF for `file get`) |
| `tbl_full.mcs` | cFE TBL on DS.FILTER_TBL: noop, reset, registry (packet and file), dump, a full load cycle with the dump as load file (load, validate, re-load drops the validation, activate), abort_load, validate active, and 6 rejections; delete_cds and new table content are left to manual tests | any time (repeatable; the table keeps its content; needs CF for `file get`) |
| `es_full.mcs` | cFE ES: noop, reset, query_one, the five file commands (each fetched with `file get` into `downlink/ram/`), syslog mode, perf masks and perf start/stop, shell, and 6 rejections (4 as `raw` packets); restart, app control, CDS and log clearing are left to manual tests | any time (repeatable; needs CF for `file get`) |

The FSW turns DEBUG events off for every app (`CFE_PLATFORM_EVS_DEFAULT_TYPE_FLAG
0x1E`), and most apps report a successful command only with a DEBUG event: a script
that wants to see them turns them on for that app and off at the end, e.g.
`fsw evs enable_app_event_type DS debug` ... `fsw evs disable_app_event_type DS debug`
(`config/telecommand/evs.json`; EVS accepts the types debug, info, error, critical).

Other EVS commands useful in operations:
- **Console echo**: `fsw evs disable_ports port1` stops the `EVS Port1 ...` lines on
  the FSW console (the FSW CONSOLE pane), `fsw evs enable_ports port1` brings them back.
- **Onboard event log**: EVS keeps the last 20 events, also while TO is off.
  `fsw evs write_log_data_file` writes them to `/ram/cfe_evs.log` (fetch with `file get`);
  `fsw evs set_log_mode overwrite|discard` chooses what happens when it is full.
- **Filters**: `fsw evs add_event_filter <APP> <event id> 0xFFFF` lets only the first
  occurrence of a flooding event through (an event passes while `(count & mask) == 0`);
  `set_filter`, `reset_filter`, `reset_all_filters`, `delete_event_filter` change it.
- `fsw evs hk` asks for `evs.hk` at once (ports, log state, per-app counters; the
  scheduler sends it every 50 s). `set_event_format_mode short` is critical: TO only
  forwards long event packets, so events would stop reaching the ground.

**From the full-screen session: `run <script>`.** `mcs -f` and the
full-screen session cannot run at the same time (both receive telemetry on
UDP 5011, and TO sends to one place), so to watch the events and telemetry
cards while a test runs, start it from the session:

```
mcs> run scripts/ds_smoke.mcs
running scripts/ds_smoke.mcs: 15 line(s); 'run stop' ends it
```

The script is checked first, as with `-f` (errors are listed and nothing is
sent). It then runs in the background: its lines stream into COMMAND HISTORY
(PASS green, FAIL red), the header shows ` SCRIPT ds_smoke.mcs 9/15 `, and
the result is the same as `mcs -f` would print. `run` shows where it is,
`run stop` ends it within a tenth of a second (a running `file put/get`
finishes first). While it runs only `run`, `tlm ...`, `help`, `target` and
`file list|status|cancel|abandon|suspend|resume` are accepted, so typed
commands never mix with the script's; quitting asks once, then stops it.
Tab completes `.mcs` files after `run `.

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
defaults are CF 2.2.1 as built for the `fsw` target: `local_entity` `"0.23"`, `remote_entity`
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
scripts/        example test scripts (ds_smoke.mcs)
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
src/verify      script checks: conditions, $variables, recent-telemetry history, VerifyService
src/ui          menu tree, full-screen session, test scripts (only modules using the cli and FTXUI libraries)
src/app         main, command-line options
tests/unit        GoogleTest per module
tests/oracle      mcs packets vs. cFE 6.7 ccsds.h/ccsds.c
tests/integration real UDP on loopback
tests/smoke       the executable, checked for output and exit codes
tests/e2e         the executable against a stand-in over UDP: file put/get with tests/tools/cfdp_peer
                  (CF)
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
     `cmp test.bin` with the FSW's `/cf/test.bin`.
   - In the full-screen session, start a `file put` and a `file get` together and
     watch both in the TRANSFERS strip.

6. `mcs -f scripts/ds_smoke.mcs` → both tests PASS, `result: PASS`, exit 0.

UDP is fire-and-forget: "sent" means the datagram left this machine. Confirm
reception in the cFS event log, or with a test script.
