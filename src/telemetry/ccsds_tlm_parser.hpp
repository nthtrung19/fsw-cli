#pragma once

#include "core/bytes.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace mcs {

// A CCSDS Space Packet v1 telemetry packet as sent by cFE 6.7 (TO / TO_LAB).
//
//  offset size field
//   0      2   stream ID = MID                       big-endian
//   2      2   seq flags | 14-bit sequence count     big-endian
//   4      2   total length - 7                      big-endian
//   6      4   time: seconds                         see below
//  10      2   time: subseconds (upper 16 bits)      see below
//  12      n   payload                               TARGET byte order
//
// cFE 6.7 CFE_SB_SetMsgTime copies the time with memcpy, so it is in the
// CPU's byte order, not big-endian as CCSDS specifies (little-endian on the
// x86-64 PC build).
struct TlmFrame {
    std::uint16_t mid = 0;
    std::uint16_t apid = 0;          // low 11 bits of the stream ID
    std::uint16_t sequence = 0;      // 0..0x3FFF
    std::size_t length = 0;          // total packet length in bytes
    std::uint32_t seconds = 0;
    std::uint16_t subseconds = 0;    // 1/65536 s
    Bytes payload;                   // bytes after the 12-byte header
};

struct TlmParseResult {
    std::optional<TlmFrame> frame;
    std::string error;               // set when frame is empty
};

// Never throws. Rejects: fewer than 12 bytes, a command packet (type bit
// set), a length field that disagrees with the datagram size.
// `timeOrder` is the byte order of the secondary-header time.
TlmParseResult parseTelemetry(const Bytes& packet, Endian timeOrder = Endian::Big);

// Reads the CCSDS primary header only (MID and sequence), for any packet
// with at least 6 bytes; used for sent commands in the packet monitor.
struct PrimaryHeader {
    std::uint16_t mid = 0;
    std::uint16_t sequence = 0;
};
std::optional<PrimaryHeader> readPrimaryHeader(const Bytes& packet);

} // namespace mcs
