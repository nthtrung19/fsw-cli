#pragma once

#include "protocol/packet_format.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace mcs {

// CCSDS Space Packet v1 command, as produced by cFE 6.7 (ccsds.h / ccsds.c).
//
//  offset size field
//   0      2   stream ID = MID                         big-endian
//   2      2   seq flags (=3) | 14-bit sequence count  big-endian
//   4      2   total length - 7                        big-endian
//   6      2   (FC << 8) | checksum                    TARGET byte order
//   8      n   payload                                 (already encoded)
//
// On a little-endian target byte 6 is the checksum and byte 7 the function code.
// Checksum: field set to 0, XOR of all bytes seeded with 0xFF.
// Sequence counter: one per APID, starting at 0, wrapping after 0x3FFF.
class CcsdsV1Format : public IPacketFormat {
public:
    static constexpr std::size_t kPrimaryHeaderSize = 6;
    static constexpr std::size_t kCommandHeaderSize = 8;
    static constexpr std::size_t kMaxPacketSize = 0xFFFF + 7;
    static constexpr std::uint16_t kMaxSequence = 0x3FFF;

    explicit CcsdsV1Format(Endian target, bool checksum = true);

    BuiltPacket build(const CommandMessage& message) override;
    std::string describe() const override;

    // XOR of all bytes seeded with 0xFF (CCSDS_ComputeCheckSum).
    // A packet with a correct checksum yields 0.
    static std::uint8_t computeChecksum(const Bytes& packet);

private:
    Endian target_;
    bool checksum_;
    std::unordered_map<std::uint16_t, std::uint16_t> nextSequence_;   // per APID
};

} // namespace mcs
