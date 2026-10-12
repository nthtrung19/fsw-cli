#include "telemetry/ccsds_tlm_parser.hpp"

#include "telemetry/tlm_def.hpp"

namespace mcs {

namespace {

std::uint16_t be16(const Bytes& b, std::size_t at)
{
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

std::uint32_t be32(const Bytes& b, std::size_t at)
{
    return (static_cast<std::uint32_t>(b[at]) << 24) | (static_cast<std::uint32_t>(b[at + 1]) << 16)
         | (static_cast<std::uint32_t>(b[at + 2]) << 8) | static_cast<std::uint32_t>(b[at + 3]);
}

std::uint16_t le16(const Bytes& b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t le32(const Bytes& b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8)
         | (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

constexpr std::uint16_t kTypeBit = 0x1000;     // 1 = command
constexpr std::uint16_t kApidMask = 0x07FF;
constexpr std::uint16_t kSequenceMask = 0x3FFF;

} // namespace

std::optional<PrimaryHeader> readPrimaryHeader(const Bytes& packet)
{
    if (packet.size() < 6) {
        return std::nullopt;
    }
    return PrimaryHeader{be16(packet, 0), static_cast<std::uint16_t>(be16(packet, 2) & kSequenceMask)};
}

TlmParseResult parseTelemetry(const Bytes& packet, Endian timeOrder)
{
    TlmParseResult result;
    if (packet.size() < kTlmHeaderSize) {
        result.error = "too short (" + std::to_string(packet.size()) + " bytes, header is "
                     + std::to_string(kTlmHeaderSize) + ")";
        return result;
    }

    const std::uint16_t streamId = be16(packet, 0);
    if ((streamId & kTypeBit) != 0) {
        result.error = "command packet, not telemetry";
        return result;
    }
    const std::size_t declared = static_cast<std::size_t>(be16(packet, 4)) + 7;
    if (declared != packet.size()) {
        result.error = "length field says " + std::to_string(declared) + " bytes, received "
                     + std::to_string(packet.size());
        return result;
    }

    TlmFrame f;
    f.mid = streamId;
    f.apid = static_cast<std::uint16_t>(streamId & kApidMask);
    f.sequence = static_cast<std::uint16_t>(be16(packet, 2) & kSequenceMask);
    f.length = packet.size();
    const bool big = timeOrder == Endian::Big;
    f.seconds = big ? be32(packet, 6) : le32(packet, 6);
    f.subseconds = big ? be16(packet, 10) : le16(packet, 10);
    f.payload.assign(packet.begin() + static_cast<std::ptrdiff_t>(kTlmHeaderSize), packet.end());
    result.frame = std::move(f);
    return result;
}

} // namespace mcs
