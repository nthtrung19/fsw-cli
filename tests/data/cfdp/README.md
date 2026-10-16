# Golden CFDP data from the real CF

Captured on the SIL (cFE 6.7, CF 2.2.1, 32-bit build) on 2026-10-16:

- `cf_class1_playback.bin`: the UDP datagrams TO sent on port 5011 for
  `fsw cf playback_file class1 0 0 keep 0.23 /cf/lc_def_adt.tbl /golden/lc_def_adt.tbl`,
  only MID 0x092D (CF_SPACE_TO_GND_PDU_MID). Each record: u32 big-endian
  length, then the whole datagram (12-byte telemetry header + CFDP PDU).
  6 packets: Metadata, 4 x FileData (4000 B chunks), EOF.
- `cf_class1_playback.src`: the file CF sent (`build/exe/ntpm/cf/lc_def_adt.tbl`).

Used by `GsfcEngine.RebuildsAFileFromRealCfPackets` (tests/unit/test_gsfc_engine.cpp).
