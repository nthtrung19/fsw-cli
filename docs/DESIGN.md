# mcs — Design Specification

Status: **v1.1, implemented in mcs 0.2.0 (prototype)**. All decisions in §10 confirmed.
Scope of this document: the complete design of the prototype, plus the extension
points that later apps, targets, protocols and transports plug into.

---

## 1. Purpose and scope

`mcs` is a ground-side command-line tool that sends commands to cFS flight
software (FSW). An operator types

```
fsw ds set_app_state enable
```

and the tool validates the arguments, builds the exact packet the FSW expects,
and sends it to the configured target.

### 1.1 Prototype scope (this delivery)

| In scope | Out of scope (later) |
|---|---|
| DS commands `noop`, `reset`, `set_app_state` | DS filter commands (need `ds_msg.h` structs) |
| CCSDS Space Packet v1 command format | CCSDS v2, CSP (designed for, not implemented) |
| UDP transport, dry-run transport | TCP, serial, KISS |
| Target profiles from a config file | Switching target at runtime |
| Command catalog from data files | EDS (XML) catalog import |
| Interactive, one-shot (`-c`) and script (`-f`) modes | Telemetry inspector, RX log, replay |
| Argument validation, help, tab-completion of commands | GUI, remote (telnet) sessions |
| Packet log (audit trail), hexdump, verbose mode | Continuous checks (`watch`), result files, JUnit |
| Full-screen session: telemetry receive/decode (F1 console with EVS events, F2 telemetry checklist and packet cards) | |
| Script checks in batch mode: `wait`, `expect`, `expect-event`, `expect-no-event`, `save`/`$var`, `test` … `end`; the whole script checked first (§4.13) | Script `include`, procedures, starting/stopping the FSW |

### 1.2 Requirements

**Functional**

- F1. Commands are entered as `fsw <app> <command> [args...]`, in one line or by
  navigating menus.
- F2. Arguments are validated (count, type, range, enum names) before anything is
  sent. Invalid input never produces a packet.
- F3. The packet is byte-exact with what cFE 6.7 expects (header, length,
  checksum, byte order).
- F4. The destination, byte order and protocol stack come from a named target
  profile, not from code.
- F5. Dry-run mode prints the packet instead of sending it.
- F6. Every sent packet is appended to a log file with timestamp, target,
  command line and bytes.
- F7. Batch modes return a non-zero exit code if any command fails.

**Extensibility (the main design driver)**

- E1. Adding an **app or command** = adding/editing a data file. No code change,
  no recompilation.
- E2. Adding a **target** = adding an entry to the targets file.
- E3. Adding a **protocol** (packet format or wrapping layer) = one new class +
  one registration line. No existing logic changes.
- E4. Adding a **transport** = one new class + one registration line.
- E5. The core logic does not depend on the CLI library, so it can be reused
  (scripts, tests, a future GUI).

**Non-functional**

- N1. Linux, C++17, CMake ≥ 3.16, GCC ≥ 9 (tested with GCC 13).
- N2. Zero compiler warnings with strict flags on project code.
- N3. All third-party code pinned to fixed versions; builds work offline once
  submodules are checked out.

---

## 2. Fixed facts about the current target

| Item | Value |
|---|---|
| FSW | cFE 6.7 on a Linux PC, **little-endian** |
| Message format | CCSDS v1 (`MESSAGE_FORMAT_IS_CCSDS_VER_2` undefined) |
| Command header | 6 B primary + 2 B command secondary header = 8 B (`CFE_SB_CMD_HDR_SIZE`) |
| Command ingest | `ci_lab`, UDP `127.0.0.1:1234`, one datagram = one packet |
| DS command MID | `0x194B` (`DS_CMD_MID`) |
| DS command codes | NOOP 0, RESET 1, SET_APP_STATE 2, SET_FILTER_FILE 3, SET_FILTER_TYPE 4, SET_FILTER_PARMS 5 |
| `DS_AppStateCmd_t` | header + `uint16 EnableState` + `uint16 Padding` = 12 B |

### 2.1 CCSDS v1 command packet layout

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0 | 2 | Stream ID (= MID) | big-endian |
| 2 | 2 | Sequence: flags (2 bits, = 3) + count (14 bits) | big-endian |
| 4 | 2 | Length = total bytes − 7 | big-endian |
| 6 | 2 | Command secondary header `uint16 (FC << 8) \| checksum` | **target byte order** |
| 8 | n | Payload | **target byte order** (per field) |

On a little-endian target, byte 6 = checksum and byte 7 = function code.

**Checksum** (`ccsds.c`): set field to 0; `cs = 0xFF; for each byte in the
packet: cs ^= byte`; store `cs`. A valid packet then XORs (seed `0xFF`) to 0.

### 2.2 Golden vectors (sequence count 0)

```
ds noop                  19 4B C0 00 00 01 6C 00
ds reset                 19 4B C0 00 00 01 6D 01
ds set_app_state enable  19 4B C0 00 00 05 6B 02 01 00 00 00
ds set_app_state disable 19 4B C0 00 00 05 6A 02 00 00 00 00
```

---

## 3. Architecture

### 3.1 Layers

```
            ┌──────────────────────────────────────────────┐
 operator ─▶│ ui/        menu tree from catalog (cli lib)  │  only module using <cli/...>
            └──────────────────────┬───────────────────────┘
                                   ▼
            ┌──────────────────────────────────────────────┐
            │ service/   CommandService                    │  one entry point per command
            └───┬──────────────┬──────────────┬────────────┘
                ▼              ▼              ▼
          catalog/        encode/        pipeline/  ──────────────────────────┐
          what exists     args → bytes   format → layers… → transport         │
                                          │            │           │          │
                                    protocol/     protocol/    transport/     │
                                    formats       layers       udp, dryrun    │
                                                                              │
            config/  loads targets.json + catalog files, builds the pipeline ◀┘
            core/    Bytes, Endian, ByteWriter, hexdump, errors (used by all)
            log/     packet log (audit trail)
```

### 3.2 Dependency rules

1. `core/` depends on nothing.
2. `catalog/`, `encode/`, `protocol/`, `transport/` depend only on `core/`.
   They never know about each other's internals, about JSON, or about the CLI.
3. `config/` is the **only** module that parses JSON. It turns files into plain
   C++ structs and uses the registries to create objects.
4. `service/` wires catalog + encoder + pipeline + log together;
   `FileTransferService` also wires `cfdp/` and the telemetry parser.
5. `cfdp/` depends only on `core/` and the GSFC CFDP library, which only
   `cfdp/gsfc_bridge.c` and `cfdp/gsfc_engine.cpp` see (§4.12).
5a. `verify/` depends only on `core/` and `telemetry/` (§4.13); the menu and
   `app/` connect it to `CommandService` (when the last command was sent).
6. `ui/` and `app/` are the only modules that include `<cli/...>`.
7. Everything except `ui/` and `app/` is the static library `mcs_core`; `ui/`
   without the full-screen session (menus, help, test scripts) is `mcs_menu`.

Breaking any of these rules is a design defect, not a style issue.

### 3.3 Command data flow

```
"fsw ds set_app_state enable"
   │  ui: tokens → (app="ds", cmd="set_app_state", args=["enable"])
   ▼
CommandService::execute
   │  catalog.find("ds","set_app_state")  → CommandDef {cc=2, fields=[state:u16 enum, pad:2]}
   │  encoder.encode(def, args)           → payload 01 00 00 00        (target endian)
   │  CommandMessage {mid=0x194B, cc=2, payload}
   ▼
Pipeline::send
   │  format.build(msg)                   → 19 4B C0 00 00 05 6B 02 01 00 00 00
   │  for layer in layers: bytes = layer.wrap(bytes)      (none for this target)
   │  transport.send(bytes)               → UDP datagram to 127.0.0.1:1234
   ▼
PacketLog::record(...), output "sent 12 bytes to udp://127.0.0.1:1234 (seq 0)"
```

---

## 4. Module specifications

### 4.1 `core/`

| Item | Purpose |
|---|---|
| `Bytes` | `std::vector<std::uint8_t>` |
| `Endian` | `enum class Endian { Little, Big }`, parse from `"little"`/`"big"` |
| `ByteWriter` | Append `u8/u16/u32/u64`, `i8..i64`, `f32/f64`, raw bytes, fixed-length strings, zero padding, in a chosen byte order. Also `patchU16BE(offset, v)` for headers. |
| `hexdump()` | `"19 4B C0 …"` single-line and offset-prefixed multi-line forms |
| `parseHex()` | `"19 4b c0"` / `"194BC0"` → `Bytes` (for the `raw` command) |
| errors | `mcsError` base; `ParseError` (user input), `ConfigError`, `EncodeError`, `TransportError` |

### 4.2 `catalog/` — what commands exist

Plain data, no I/O:

```cpp
enum class FieldType { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, String, Padding };

struct FieldDef {
    std::string name;                         // empty for Padding
    FieldType   type;
    std::size_t size;                         // bytes; derived for numeric types
    std::vector<std::pair<std::string, std::int64_t>> enumValues;  // ordered
    std::optional<std::int64_t> min, max;     // numeric range (inclusive)
    std::string help;
    bool userVisible() const { return type != FieldType::Padding; }
};

struct CommandDef {
    std::string name;      // "set_app_state"
    std::uint8_t cc;       // 0..127
    std::string help;
    std::vector<FieldDef> fields;   // in payload order
    bool critical = false;          // ask for confirmation
};

struct AppDef {
    std::string name;      // "ds"
    std::uint16_t mid;     // command MID
    std::string help;
    std::vector<CommandDef> commands;
};

class CommandCatalog {
public:
    void add(AppDef app);                        // validates: unique names, unique cc, sizes
    const AppDef*     findApp(std::string_view) const;
    const CommandDef* findCommand(std::string_view app, std::string_view cmd) const;
    const std::vector<AppDef>& apps() const;
};
```

`CommandCatalog::add` rejects duplicates and inconsistent definitions with a
`ConfigError` naming the file and command, so a bad data file fails at startup,
not when an operator sends a command.

### 4.3 `encode/` — arguments to payload

```cpp
class PayloadEncoder {
public:
    explicit PayloadEncoder(Endian target);
    Bytes encode(const CommandDef&, const std::vector<std::string>& args) const;
};
```

Rules:

- Number of args must equal the number of user-visible fields.
- Integers accept decimal, `0x` hex, and enum names (case-insensitive) when the
  field has `enumValues`. Values are range-checked against the type and
  `min`/`max`. A field with `enumValues` accepts only its listed values, by name
  or by number (e.g. `set_app_state 7` is refused although 7 fits a u16).
- Floats accept decimal notation.
- Strings must fit `size - 1` bytes (NUL-terminated, zero-filled), matching the
  cFS `char[N]` convention.
- Padding fields are written as zeros and never consume an argument.
- Errors are `ParseError` with a usage line, e.g.
  `set_app_state: expected 1 argument <state: enable|disable>, got 0`.

### 4.4 `protocol/` — packet formats and wrapping layers

Two plug-in kinds. This split is what lets new protocols be added without
touching existing code.

```cpp
struct CommandMessage { std::uint16_t mid; std::uint8_t cc; Bytes payload; };

// Builds a packet from a command. Owns per-packet state (e.g. sequence counters).
class IPacketFormat {
public:
    virtual ~IPacketFormat() = default;
    virtual Bytes build(const CommandMessage&) = 0;
    virtual std::string describe() const = 0;          // "ccsds_v1 (little-endian)"
};

// Wraps an already-built packet (CSP header, KISS framing, CRC, crypto, ...).
class IFramingLayer {
public:
    virtual ~IFramingLayer() = default;
    virtual Bytes wrap(const Bytes&) = 0;
    virtual std::string describe() const = 0;
};
```

**`CcsdsV1Format`** (implemented): layout and checksum as in §2.1.
- Sequence counter per APID (`mid & 0x07FF`), starts at 0, wraps after `0x3FFF`.
- Validation: MID must have version 0, type bit (`0x1000`) = 1, secondary header
  bit (`0x0800`) = 1; `cc <= 0x7F`; total length ≤ 65542 bytes.
- Option `checksum: true|false` (default true).

**`CcsdsV2Format`** (later): same plus 4-byte APID qualifiers from options.
**`CspLayer`** (later): prepends a CSP header built from options
(priority, src, dst, dport, sport, flags, optional CRC32). Whether CSP wraps a
CCSDS packet (layer) or replaces it (format) is still open; both shapes fit.

### 4.5 `transport/` — where bytes go

```cpp
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void send(const Bytes&) = 0;               // throws TransportError
    virtual std::string describe() const = 0;          // "udp://127.0.0.1:1234"
};
```

- **`UdpTransport`**: POSIX socket, opened once at startup, one datagram per
  packet, resolves hostnames, reports send errors clearly.
- **`DryRunTransport`**: sends nothing; the service prints the hexdump.

Telemetry is received through a separate interface so senders stay simple:

```cpp
class IReceiver {
public:
    virtual std::optional<Bytes> receive(std::chrono::milliseconds timeout) = 0;   // nullopt: timeout
    virtual std::string describe() const = 0;                                      // "udp://0.0.0.0:5011"
};
```

- **`UdpReceiver`**: binds a UDP port (default 5011, TO's default destination);
  `poll()` with a timeout so the receiver thread can stop promptly.

### 4.5a `telemetry/` — receive and decode (full-screen session)

```
UdpReceiver -> TelemetryService (thread) -> TlmStore::ingest -> parse -> decode -> latest values,
                                                                                    events, link counters
UI thread (FTXUI) --snapshot()--> render
```

- Two threads: the UI thread runs commands and renders; the receiver thread
  waits up to 100 ms per packet, fills `TlmStore` (mutex) and asks for a
  redraw. The 100 ms timeout also drives the animation.
- `TelemetryCatalog` (`config/telemetry/<app>.json`): one packet definition
  per MID; fields in payload order after the 12-byte cFE 6.7 telemetry header,
  in the target byte order. `"event": true` packets go to the event log.
  The loader expands `"count": N` / `[N, M]` arrays to `name_0 ..` and
  `"fields": [...]` groups (nested structs) to `group_member` /
  `group_0_member`, so the decoder only sees flat fields.
- The secondary-header time is read in the target byte order: cFE 6.7
  `CFE_SB_SetMsgTime` copies it with `memcpy` (little-endian on the PC build).
- Checks per packet: CCSDS length field, sequence continuity per MID (gap),
  payload size vs. definition (at least the definition for `"variable": true`
  packets such as CF PDUs), unknown MID. All are counted and shown; none
  stops decoding.
- Memory is bounded: latest values per MID, 500 events.

### 4.6 `registry/` — names to factories

```cpp
using Options = std::map<std::string, std::string>;   // flat key/value plug-in options

class PluginRegistry {
public:
    using FormatFactory    = std::function<std::unique_ptr<IPacketFormat>(Endian, const Options&)>;
    using LayerFactory     = std::function<std::unique_ptr<IFramingLayer>(Endian, const Options&)>;
    using TransportFactory = std::function<std::unique_ptr<ITransport>(const Options&)>;

    void addFormat(std::string type, FormatFactory);
    void addLayer(std::string type, LayerFactory);
    void addTransport(std::string type, TransportFactory);
    // create*(type, ...) throw ConfigError("unknown format 'xyz'; known: ccsds_v1")
};

PluginRegistry builtinPlugins();   // registers ccsds_v1, udp, dryrun
```

Options are flat strings so plug-in code never sees JSON; each factory parses
and validates its own options (`port` must be 1..65535, etc.).

### 4.7 `pipeline/`

```cpp
class Pipeline {
public:
    Pipeline(std::unique_ptr<IPacketFormat>, std::vector<std::unique_ptr<IFramingLayer>>,
             std::unique_ptr<ITransport>);
    struct Result { Bytes packet; Bytes wire; };   // before layers / as sent
    Result send(const CommandMessage&);
    Result sendRaw(const Bytes& packet);           // skip format, still apply layers
    std::string describe() const;                  // "ccsds_v1(le) → udp://127.0.0.1:1234"
};
```

### 4.8 `config/` — files to objects

The only JSON-aware module (nlohmann/json).

**`targets.json`**

```json
{
  "default_target": "fsw",
  "targets": {
    "fsw": {
      "description": "cFS on this PC (ci_lab)",
      "endian": "little",
      "catalog": ["telecommand/ds.json"],
      "format":    { "type": "ccsds_v1" },
      "layers":    [],
      "transport": { "type": "udp", "host": "127.0.0.1", "port": "1234" }
    },
    "fsw-dry": {
      "description": "Same as fsw, nothing sent",
      "endian": "little",
      "catalog": ["telecommand/ds.json"],
      "format":    { "type": "ccsds_v1" },
      "transport": { "type": "dryrun" }
    }
  }
}
```

- `catalog` paths are relative to the targets file. Different targets can load
  different catalogs (e.g. another vehicle with different MIDs).
- Inside `format`, each `layers[]` entry and `transport`, every key except
  `type` is passed to the factory as an option.

**Catalog file** (`telecommand/ds.json`, one file per app)

```json
{
  "app": "ds",
  "mid": "0x194B",
  "help": "Data Storage application",
  "commands": [
    { "name": "noop",  "cc": 0, "help": "No-op; increments command counter" },
    { "name": "reset", "cc": 1, "help": "Reset housekeeping counters" },
    { "name": "set_app_state", "cc": 2, "help": "Enable or disable DS",
      "fields": [
        { "name": "state", "type": "u16", "enum": { "disable": 0, "enable": 1 },
          "help": "Application state" },
        { "type": "padding", "size": 2 }
      ] }
  ]
}
```

An app that receives commands on several MIDs (one FSW processing routine
each) groups its commands by MID; the single `"mid"` + `"commands"` form above
is shorthand for one group:

```json
{
  "app": "ds",
  "help": "Data Storage application",
  "mids": [
    { "mid": "0x194B", "commands": [ { "name": "noop", "cc": 0 }, ... ] },
    { "mid": "0x194C", "commands": [ { "name": "hk", "cc": 0 } ] }
  ]
}
```

Each command carries the MID of its group (`CommandDef::mid`). Command codes
are unique per MID, command names per app (the menu is `fsw <app> <name>`),
and a MID belongs to one app.

Loader responsibilities: schema checks with file/line-level messages, numbers
as decimal or `"0x…"` strings, then `CommandCatalog::add`.

**File lookup order**: `--config <file>` → `$mcs_CONFIG` →
`./config/targets.json` → `<exe dir>/../share/mcs/targets.json`.

### 4.9 `log/`

`PacketLog` appends one line per sent packet:

```
2026-10-05T22:30:01.123+07:00 target=fsw cmd="fsw ds noop" seq=0 bytes=8 wire=19 4B C0 00 00 01 6C 00
```

Default file `mcs-YYYYMMDD.log` in `--log-dir` (default: current dir);
`--no-log` disables it. Logging failure warns once and never blocks sending.

### 4.10 `service/`

```cpp
class CommandService {
public:
    CommandService(const CommandCatalog&, PayloadEncoder, Pipeline&, PacketLog*);
    void execute(const std::string& app, const std::string& cmd,
                 const std::vector<std::string>& args, std::ostream& out);
    void executeRaw(const std::string& hex, std::ostream& out);
    void setVerbose(bool);
    // The script check pass: what execute() checks, without sending.
    bool check(const std::string& app, const std::string& cmd,
               const std::vector<std::string>& args, bool checkValues, std::ostream& out);
    // When the last packet was sent: the start of every script check's window.
    std::optional<std::chrono::system_clock::time_point> lastSentAt() const;
};
```

Behavior per command: look up → encode → build → (confirm if `critical`) →
send → log → print one status line (+ hexdump if verbose or dry-run).
All errors are caught here and reported as one line; nothing propagates as an
exception into the CLI library. Return/flag used for batch exit codes.

### 4.11 `ui/` and `app/`

**Menu tree** (generated from the loaded catalog):

```
mcs>
├── fsw
│   └── <app>                         one submenu per catalog app
│       └── <cmd> <param>...          FswCommand node; help shows params and enum values
├── target                            show the active target (name, pipeline, catalog files)
├── verbose on|off                    hexdump every packet
├── raw <hex bytes>                   send pre-built packet bytes (still through layers + transport)
├── wait, timeout, expect, expect-event, expect-no-event, save, test, end
│                                     script checks, batch mode only (§4.13)
├── run [<script>|stop]               full-screen session: a test script in the background (§4.13)
└── (library built-ins) help, exit, history
```

Session commands live at the top level (`mcs>`); inside an app menu only
that app's commands (plus `..`, `help`, `exit`) are available.

**Command-line options**

```
mcs [--config FILE] [-t|--target NAME] [-n|--dry-run] [-v|--verbose]
       [--log-dir DIR | --no-log] [-c CMD]... [-f FILE] [--check] [--list-targets]
       [-h] [--version]
```

`--dry-run` replaces the target's transport with `dryrun` without editing the file.

**Exit codes**: 0 success; 1 a command or check failed; 2 bad command-line options;
3 configuration error (bad targets/catalog file).


### 4.12 `cfdp/` and `FileTransferService` — file transfer with CF

mcs is the ground CFDP entity `0.23` for CF (`0.24`). It runs the CFDP engine
that CF 2.2.1 is built on (the GSFC library, `external/cfdp-gsfc`, unmodified
except `config/cfdp_config.h`), so both ends produce and accept exactly the
same PDUs.

```
 file put/get (ui, any thread)          telemetry receiver thread
        │ queue                                 │ sink: MID 0x092D -> PDU (12-byte tlm header stripped)
        ▼                                       ▼
 FileTransferService ── worker thread: the ONLY caller of the engine (every 20 ms)
        │  requests, PDUs ──► ICfdpEngine (GsfcEngine) ──► gsfc_bridge.c ──► GSFC library
        │  ◄── PduOut: CommandMessage{0x192D, cc 0, pdu} ──► Pipeline (mutex) ──► CI ──► CF
        │  ◄── Notify: started/finished ──► notices, packet log, expectation matching
        ▼
 snapshots (transfers, counts, notices) ──► file list, F1 TRANSFERS strip, batch waitFor
```

- **One owner thread.** The library keeps its state in globals and is not
  thread-safe: one `GsfcEngine` per process (the constructor refuses a second),
  created and used only on the worker. Callers queue requests and read
  snapshots under one mutex; nothing holds that mutex while the engine runs.
- **C bridge.** The library's headers use C++ keywords as parameter names, so
  C++ includes only `cfdp_data_structures.h` and calls through
  `gsfc_bridge.c`, which the C compiler checks against the real prototypes.
- **Types.** `config/cfdp_config.h` gives every library type its size in the
  32-bit flight build (`u_int_4` is `uint32_t`, not `unsigned long`); the file
  checksum reads data through `u_int_4*`.
- **Uplink.** The PUT names the file by a short alias (its sanitized base
  name), so the Metadata stays within CF's 63-character limit; the file
  callbacks map the alias back. FileData is paced by a token bucket in
  `pdu_output_ready`; directives (MD, EOF, ACK, NAK, FIN) are never held back.
  One library cycle sends at most one FileData PDU per transaction, so the
  worker cycles until pacing holds data back.
- **Downlink.** `file get` sends `cf playback_file` through `CommandService`
  and waits for a Metadata whose source name is the requested path
  (an *expectation*; 30 s start timeout, paused while another downlink from
  CF runs). Data goes to `downlink_dir/.part/`; on success the library's rename
  callback moves it to `downlink_dir/<destination name>`, refusing `..` and
  never overwriting (`name(1).ext`). Unrequested downlinks are stored too.
- **Library quirks found while integrating**: `pdu_output_open` has no default
  (a NULL call when a transfer starts); a reused transaction slot keeps the
  previous `temp_file_name` until a new temp file is opened; sequence numbers
  start at 1 in every process (mcs starts them at the Unix time).
- **Batch and sessions.** In `-c`/`-f`, `file put/get` block in `waitFor`
  (gives up after `inactivity_timeout + 10 s` without progress) and failures
  count for the exit code. In sessions they return at once; the full-screen
  session drains notices on the UI thread and shows the TRANSFERS strip.

### 4.13 `verify/` — script checks

Lets a batch script check what the FSW does (the role of CTF's verification
instructions), with the console syntax:

```
save n ds.hk cmd_accepted_counter
fsw ds noop
expect-event DS id 31 type INFO
expect ds.hk cmd_accepted_counter == $n+1 within 5
```

| Class | Role |
|---|---|
| `Condition` | `<op> <value>` parsed against a `TlmFieldDef` (integers by raw value or enum name, f32 at f32 precision, strings `==`/`!=`); `matches(DecodedValue)` |
| `VariableTable` | `$name`, `$name±N`, `$$` in a line; values from `save` |
| `TlmHistory` | Fed by a `TelemetryService` sink on the receiver thread: the last 64 decoded samples per catalog MID and the last 1000 EVS events, with receive times. `waitUntil(deadline, check)` re-runs `check` (under the lock) on every arrival |
| `VerifyService` | One method per script command; PASS/FAIL lines, per-test and total counts, summary. Check-only mode for the pass before a script runs |

- **Window.** A check counts what arrived since `CommandService::lastSentAt()`
  (the session start before the first command). Events that arrive during a
  `wait` after the command count; telemetry from before the command does not.
- **Every packet is seen.** The history is fed by its own sink, so checks do
  not depend on the display's `TlmStore`, which keeps only latest values, and
  a check wakes on the packet that satisfies it (no polling period).
- **Check pass.** `-f` (and `--check`) first feed every line to a second menu
  tree built from a check-only `VerifyService`, its own `CommandService`
  (`check()`, arming tracked) and the file menu in `FileMenuMode::Check`; any
  error stops the run before anything is sent. Lines with `$name` are checked
  with `setValuesKnown(false)`: names and argument counts, not values.
- **Output and exit code.** `ui/script_runner` (`checkScript`, `runScript`)
  puts the script location in front of `FAIL` and `error:` lines
  (`file.mcs:7: FAIL ...`) and prints the summary; failed checks count for
  exit code 1. A script run gets its own `CommandService`, `VerifyService`,
  variables and menu tree; it shares only the thread-safe `Pipeline`,
  `PacketLog`, `FileTransferService` and `TlmHistory`.
- **Not typed in sessions.** A check blocks while it waits, so the
  interactive sessions do not have these commands. The full-screen session
  runs whole scripts instead: `run <script>` checks it on the UI thread, then
  `ScriptRunner` runs it on its own thread. Output lines are queued for the
  UI thread (as `FileTransferService` notices are); `run stop` sets a flag
  that `runScript` checks between lines and `VerifyService` checks every
  100 ms while waiting. One script at a time: the cli library registers each
  session's stream in an unlocked global list, so sessions are only created
  and destroyed while no other thread does so (the TUI joins the script
  before its own session goes away).
- **Thread checking.** The threaded tests run clean under clang's
  ThreadSanitizer. GCC 11's does not intercept `pthread_cond_clockwait`
  (used by `condition_variable::wait_until` with `steady_clock`) and reports
  false "double lock" and data races on `TlmHistory`.

---

## 5. Directory layout

```
mcs/
├── CMakeLists.txt
├── README.md
├── docs/DESIGN.md                    this document
├── config/
│   ├── targets.json
│   └── telecommand/ds.json
├── external/                         git submodules, pinned
│   ├── cli/                          daniele77/cli v2.2.0
│   ├── json/                         nlohmann/json (latest release tag)
│   ├── googletest/                   googletest (latest release tag)
│   └── cfdp-gsfc/                    GSFC CFDP library of CF 2.2.1 (vendored)
├── src/
│   ├── core/       bytes.*, byte_writer.*, hex.*, text.*, options.*, errors.hpp
│   ├── catalog/    field_def.hpp, command_def.hpp, catalog.*
│   ├── encode/     payload_encoder.*
│   ├── protocol/   command_message.hpp, packet_format.hpp, framing_layer.hpp, ccsds_v1_format.*
│   ├── transport/  transport.hpp, udp_transport.*, dry_run_transport.*
│   ├── registry/   plugin_registry.*, builtin_plugins.cpp
│   ├── pipeline/   pipeline.*
│   ├── config/     json_util.*, target_config.*, catalog_loader.*, config_paths.*, pipeline_factory.*
│   ├── log/        packet_log.*
│   ├── service/    command_service.*, file_transfer_service.*
│   ├── cfdp/       engine.hpp, gsfc_engine.*, gsfc_bridge.*, pdu_info.*, transfer.*, cfdp_config.hpp
│   ├── verify/     condition.*, variables.*, tlm_history.*, verify_service.*
│   ├── ui/         menu_builder.*, fsw_command.*, tui_session.*, transfer_format.*, script_runner.*
│   └── app/        main.cpp, options.*
└── tests/
    ├── unit/       one test file per module
    ├── oracle/     cFE ccsds.h/ccsds.c + stubs, compares against our packets
    ├── integration/ UDP loopback: real socket receives what mcs sends
    ├── smoke/      CTest runs of the executable (-c, -f, exit codes)
    ├── tools/      cfdp_peer.cpp: a CF stand-in (same CFDP engine) for file transfer tests;
    │               fake_fsw.cpp: a DS stand-in (housekeeping, events) for script checks
    └── e2e/        file_transfer.sh, verify.sh: the executable against the stand-ins over UDP
```

---

## 6. Extension recipes

| To add… | Do this | Code changed |
|---|---|---|
| A command to DS | Add an entry in `config/telecommand/ds.json` | None |
| A new app (e.g. `fm`) | Create `config/telecommand/fm.json`, list it in the target's `catalog` | None |
| A new target | Add an entry in `targets.json` | None |
| A new packet format (e.g. `ccsds_v2`) | Implement `IPacketFormat`; one `addFormat` line in `builtin_plugins.cpp` | 1 new class + 1 line |
| A new wrapping layer (e.g. `csp`) | Implement `IFramingLayer`; one `addLayer` line | 1 new class + 1 line |
| A new transport (e.g. `tcp`, `serial`) | Implement `ITransport`; one `addTransport` line | 1 new class + 1 line |

Example: CSP over UDP later is just a new class plus this target entry:

```json
"lab-csp": {
  "endian": "little",
  "catalog": ["telecommand/ds.json"],
  "format": { "type": "ccsds_v1" },
  "layers": [ { "type": "csp", "src": "10", "dst": "1", "dport": "10", "crc32": "true" } ],
  "transport": { "type": "udp", "host": "192.168.1.50", "port": "9600" }
}
```

---

## 7. Testing strategy

| Level | What | Tool |
|---|---|---|
| Unit | ByteWriter, hex, catalog validation, encoder rules, CCSDS v1 (golden vectors, sequence wrap, length, checksum, BE variant, invalid input), registry, config loader (good + bad files) | GoogleTest |
| Oracle | Build the same packets with cFE's own `ccsds.h`/`ccsds.c` macros on the host; must equal ours byte for byte | GoogleTest + C stubs |
| Integration | Bind a UDP socket on loopback, run the pipeline with `udp` transport, compare received datagram | GoogleTest |
| Smoke | Run the executable: `-c`, `-f`, `--dry-run`, wrong command / wrong args → exit codes | CTest |
| CFDP | Engine: PDU layout, checksum (independent implementation), round trip, path rules, pacing; two engines in two processes (class 1/2, loss, cancel, put and get at once); the service (CF command packets, playback, expectations); formatter | GoogleTest |
| Script checks | Conditions, variables, history (ring limits, wake-up), every check (pass, late pass, timeout with last value, window), tests, check-only mode; `runScript`/`checkScript`, `ScriptRunner` (background run, output order, stop during a wait and a check, one at a time) | GoogleTest |
| End to end | The executable's `file put/get` against `mcs_cfdp_peer` over UDP, class 1/2, with loss; test scripts against `mcs_fake_fsw` (pass, a failing check, `--check` with a typo) | CTest + bash |
| Threads | Service, pipeline, log, telemetry sinks, engine, script runner under ThreadSanitizer (clang; see §4.13) | manual build |
| Manual acceptance | Against real cFS (§8) | operator |

## 8. Prototype acceptance criteria

1. Build from a clean clone with zero warnings; all automated tests pass.
2. `mcs --dry-run -c "fsw ds noop"` prints `19 4B C0 00 00 01 6C 00`.
3. With cFS running and `ci_lab` listening on 127.0.0.1:1234:
   - `fsw ds noop` → DS no-op event message appears; DS command counter +1.
   - `fsw ds set_app_state disable` / `enable` → DS state changes in housekeeping/events.
   - `fsw ds set_app_state maybe` → rejected locally, nothing sent.
4. Adding a dummy command to `ds.json` makes it appear in `help` without rebuilding.

## 9. Future work (designed for, not built)

CCSDS v2 format · CSP layer · TCP/serial transports · telemetry inspector,
RX log and replay · script `watch` (continuous checks), `include` and
procedures, result files and JUnit output, starting/stopping the FSW from a
script · runtime `target use` · EDS import ·
`critical` confirmation for dangerous commands in other apps · remaining DS
filter commands once `ds_msg.h` is provided.

## 10. Decisions (confirmed)

| # | Decision | Chosen |
|---|---|---|
| D1 | Catalog format | JSON files, loaded at startup (instead of C++ tables) |
| D2 | JSON library | nlohmann/json v3.12.0, git submodule (shallow) |
| D3 | Unit test framework | GoogleTest v1.17.0, git submodule (shallow) |
| D4 | Oracle test against original `ccsds.c` | Include |
| D5 | Target selection | At startup (`--target`), not switchable at runtime |
| D6 | Packet log | On by default, daily file in current directory |
| D7 | Third-party fetching | Git submodules (offline-friendly), not CMake FetchContent |
| D8 | CFDP engine | The GSFC library of CF 2.2.1, vendored, behind `ICfdpEngine` (same engine on both ends) |

## 11. Implementation notes

- Config files accept `//` and `/* */` comments.
- Plug-in options may be JSON strings, numbers or booleans; they reach the
  plug-in as text (`"port": 1234` and `"port": "1234"` are equivalent).
- Critical commands use an `arm` → command sequence instead of a y/N prompt,
  so the same rule works in interactive, `-c` and `-f` modes. An invalid
  critical command does not consume the arming.
- The oracle test compiles the unmodified cFE 6.7 `ccsds.h`/`ccsds.c` (Apache
  2.0) with small stand-in headers; it is skipped on big-endian hosts.
- `UdpTransport` uses an unconnected socket (`sendto`), so a missing listener
  is never reported as an error; success means the datagram was handed to the
  OS.
