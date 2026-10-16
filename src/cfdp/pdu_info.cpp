#include "cfdp/pdu_info.hpp"

namespace mcs::cfdp {

namespace {

std::uint64_t readBigEndian(const Bytes& b, std::size_t at, std::size_t length)
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < length; ++i) {
        v = (v << 8) | b[at + i];
    }
    return v;
}

PduKind directiveKind(std::uint8_t code)
{
    switch (code) {
    case 0x04: return PduKind::Eof;
    case 0x05: return PduKind::Fin;
    case 0x06: return PduKind::Ack;
    case 0x07: return PduKind::Metadata;
    case 0x08: return PduKind::Nak;
    case 0x09: return PduKind::Prompt;
    case 0x0C: return PduKind::KeepAlive;
    default: return PduKind::Unknown;
    }
}

} // namespace

const char* toString(PduKind kind)
{
    switch (kind) {
    case PduKind::FileData: return "FD";
    case PduKind::Eof: return "EOF";
    case PduKind::Fin: return "FIN";
    case PduKind::Ack: return "ACK";
    case PduKind::Metadata: return "MD";
    case PduKind::Nak: return "NAK";
    case PduKind::Prompt: return "PROMPT";
    case PduKind::KeepAlive: return "KEEPALIVE";
    case PduKind::Unknown: return "?";
    }
    return "?";
}

std::string entityToString(const std::uint8_t* bytes, std::size_t length)
{
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        if (i != 0) {
            out += '.';
        }
        out += std::to_string(bytes[i]);
    }
    return out;
}

std::optional<PduInfo> inspectPdu(const Bytes& pdu)
{
    if (pdu.size() < 4) {
        return std::nullopt;
    }
    PduInfo info;
    const bool fileData = (pdu[0] & 0x10) != 0;
    info.towardSender = (pdu[0] & 0x08) != 0;
    info.cls = (pdu[0] & 0x04) != 0 ? 1 : 2;
    info.dataLength = static_cast<std::size_t>(readBigEndian(pdu, 1, 2));
    const std::size_t idLength = static_cast<std::size_t>(((pdu[3] >> 4) & 0x07) + 1);
    const std::size_t seqLength = static_cast<std::size_t>((pdu[3] & 0x07) + 1);
    info.headerLength = 4 + 2 * idLength + seqLength;
    if (seqLength > 4 || pdu.size() < info.headerLength + info.dataLength) {
        return std::nullopt;
    }
    info.id.source = entityToString(&pdu[4], idLength);
    info.id.seq = static_cast<std::uint32_t>(readBigEndian(pdu, 4 + idLength, seqLength));
    info.destination = entityToString(&pdu[4 + idLength + seqLength], idLength);

    const std::size_t data = info.headerLength;
    if (fileData) {
        info.kind = PduKind::FileData;
        if (info.dataLength < 4) {
            return std::nullopt;
        }
        info.offset = static_cast<std::uint32_t>(readBigEndian(pdu, data, 4));
        info.fileDataLength = info.dataLength - 4;
        return info;
    }
    if (info.dataLength < 1) {
        return std::nullopt;
    }
    info.kind = directiveKind(pdu[data]);
    if ((info.kind == PduKind::Eof || info.kind == PduKind::Fin) && info.dataLength >= 2) {
        info.condition = static_cast<std::uint8_t>(pdu[data + 1] >> 4);
    }
    if (info.kind == PduKind::Ack && info.dataLength >= 2) {
        info.acked = directiveKind(static_cast<std::uint8_t>(pdu[data + 1] >> 4));
    }
    return info;
}

std::string describePdu(const PduInfo& info)
{
    std::string text = toString(info.kind);
    if (info.kind == PduKind::Ack) {
        text += std::string("(") + toString(info.acked) + ")";
    }
    text += " " + info.id.toString() + " -> " + info.destination;
    if (info.kind == PduKind::FileData) {
        text += " off=" + std::to_string(info.offset) + " len=" + std::to_string(info.fileDataLength);
    }
    if ((info.kind == PduKind::Eof || info.kind == PduKind::Fin) && info.condition != 0) {
        text += " cond=" + std::to_string(info.condition);
    }
    return text;
}

} // namespace mcs::cfdp
