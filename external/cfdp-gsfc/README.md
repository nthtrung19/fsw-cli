# GSFC CFDP library (vendored)

The CCSDS File Delivery Protocol engine that the cFS **CF application 2.2.1**
is built on, written at NASA Goddard (Tim Ray, 2007–2014). mcs runs the same
engine on the ground (entity `0.23`) so that its PDUs match CF's exactly.

| Path | Content |
|---|---|
| `src/` | `fsw/src/PRI/*.c`, `*.h` of CF 2.2.1, **unmodified** (CRLF line endings kept) |
| `include/` | `fsw/src/PUB/` headers of CF 2.2.1, **unmodified**, except `cfdp_config.h` |
| `config/cfdp_config.h` | Our replacement for `PUB/cfdp_config.h` (the library allows the user to edit this one file) |
| `NOSA.pdf` | The NASA Open Source Agreement that governs this code |

Source: `~/SDE/fsw/apps/cf/fsw/src` (CF 2.2.1, `cf_version.h`).

## Why `config/cfdp_config.h` differs

- The flight version includes cFS headers (`osconfig.h`, `cf_platform_cfg.h`,
  `cf_defs.h`); the values that came from them are written out.
- `u_int_4`/`s_int_4` are 32-bit fixed-width types. The flight version uses
  `unsigned long`, which is only 4 bytes in the `-m32` flight build; on a
  64-bit host it would be 8 bytes and the file checksum (`aaa.c`, read through
  `u_int_4*`) would be wrong. All types keep the sizes they have in flight.
- Ground limits: 255-character file names, 32 concurrent transactions,
  4096-byte chunks (CF sends 4000), 8192-byte PDUs.
- Message classes off by default; assertions are reported but never abort.

## Using it from C++

The public headers use C++ keywords as parameter names (`new` in
`register_rename`, `class` in `cfdp_enable_message_class`), so C++ code must
not include `cfdp.h`. mcs calls the library through a small C bridge
(`src/cfdp/gsfc_bridge.c` / `.h`), which includes the real headers so the C
compiler checks every signature. `cfdp_data_structures.h` is C++-safe and is
used directly.

The library keeps all state in globals and is not thread-safe: one engine per
process, called from one thread only (`src/service/file_transfer_service.*`).
