#include "telemetry/decoded_packet.hpp"

namespace mcs {

DecodedPacket decodePacket(const TelemetryCatalog& catalog, const Bytes& datagram, Endian endian)
{
    DecodedPacket packet;
    packet.datagram = &datagram;
    packet.parsed = parseTelemetry(datagram, endian);
    if (packet.parsed.frame) {
        packet.def = catalog.find(packet.parsed.frame->mid);
        if (packet.def != nullptr) {
            packet.values = decodeTelemetry(*packet.def, packet.parsed.frame->payload, endian);
        }
    }
    return packet;
}

} // namespace mcs
