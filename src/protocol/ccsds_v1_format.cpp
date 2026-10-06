#include "protocol/ccsds_v1_format.hpp"

#include "core/byte_writer.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"

namespace mcs {

namespace {

constexpr std::uint16_t kVersionMask   = 0xE000;
constexpr std::uint16_t kTypeCommand   = 0x1000;
constexpr std::uint16_t kSecHdrPresent = 0x0800;
constexpr std::uint16_t kApidMask      = 0x07FF;
constexpr std::uint16_t kSeqFlagsComplete = 0xC000;   // segmentation flags = 3

} // namespace

CcsdsV1Format::CcsdsV1Format(Endian target, bool checksum)
    : target_(target), checksum_(checksum)
{
}

std::uint8_t CcsdsV1Format::computeChecksum(const Bytes& packet)
{
    std::uint8_t cs = 0xFF;
    for (const auto b : packet) {
        cs = static_cast<std::uint8_t>(cs ^ b);
    }
    return cs;
}

BuiltPacket CcsdsV1Format::build(const CommandMessage& message)
{
    const std::uint16_t mid = message.mid;
    if ((mid & kVersionMask) != 0) {
        throw EncodeError("MID " + toHexString(mid, 4) + " is not a CCSDS v1 stream ID (version bits set)");
    }
    if ((mid & kTypeCommand) == 0) {
        throw EncodeError("MID " + toHexString(mid, 4) + " is not a command MID (packet type bit is 0)");
    }
    if ((mid & kSecHdrPresent) == 0) {
        throw EncodeError("MID " + toHexString(mid, 4) + " has no secondary header flag");
    }
    if (message.cc > 0x7F) {
        throw EncodeError("function code " + std::to_string(message.cc) + " exceeds 127");
    }
    const std::size_t total = kCommandHeaderSize + message.payload.size();
    if (total > kMaxPacketSize) {
        throw EncodeError("packet of " + std::to_string(total) + " bytes exceeds the CCSDS maximum of "
                          + std::to_string(kMaxPacketSize));
    }

    const std::uint16_t apid = mid & kApidMask;
    std::uint16_t& next = nextSequence_[apid];
    const std::uint16_t sequence = next;
    next = static_cast<std::uint16_t>((next + 1) & kMaxSequence);

    // Primary header: always big-endian.
    ByteWriter header(Endian::Big);
    header.u16(mid);
    header.u16(static_cast<std::uint16_t>(kSeqFlagsComplete | sequence));
    header.u16(static_cast<std::uint16_t>(total - 7));

    // Command secondary header: a native uint16 on the target, checksum 0 for now.
    ByteWriter secondary(target_);
    secondary.u16(static_cast<std::uint16_t>(message.cc << 8U));

    Bytes packet = header.take();
    const Bytes sec = secondary.take();
    packet.insert(packet.end(), sec.begin(), sec.end());
    packet.insert(packet.end(), message.payload.begin(), message.payload.end());

    if (checksum_) {
        // The checksum is the low byte of the uint16 word: byte 6 on
        // little-endian targets, byte 7 on big-endian targets.
        const std::size_t csOffset = (target_ == Endian::Little) ? 6 : 7;
        packet[csOffset] = computeChecksum(packet);
    }

    return {std::move(packet),
            "apid=" + toHexString(apid, 3) + " seq=" + std::to_string(sequence)};
}

std::string CcsdsV1Format::describe() const
{
    return "ccsds_v1 (" + toString(target_) + "-endian" + (checksum_ ? "" : ", no checksum") + ")";
}

} // namespace mcs
