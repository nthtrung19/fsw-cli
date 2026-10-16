#pragma once

#include "core/bytes.hpp"
#include "cfdp/transfer.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace mcs::cfdp {

// What kind of PDU it is: the directive code, or FileData.
enum class PduKind { FileData, Eof, Fin, Ack, Metadata, Nak, Prompt, KeepAlive, Unknown };

const char* toString(PduKind kind);   // "FD", "EOF", "FIN", "ACK", "MD", "NAK", ...

// The fields of a CFDP PDU (CCSDS 727.0-B-3) that mcs looks at. Read-only;
// the engine does the real encoding and decoding.
//
//  byte 0   version(3) | type(1: 1 = FileData) | direction(1: 1 = toward sender)
//           | mode(1: 1 = unacknowledged, class 1) | CRC(1) | reserved(1)
//  1..2     data field length
//  3        reserved(1) | entity ID length - 1 (3) | reserved(1) | seq length - 1 (3)
//  then     source ID, sequence number, destination ID; then the data field
struct PduInfo {
    PduKind kind = PduKind::Unknown;
    bool towardSender = false;
    int cls = 1;                      // 1 unacknowledged, 2 acknowledged
    std::size_t headerLength = 0;     // 12 for CF
    std::size_t dataLength = 0;
    TransactionId id;                 // source entity + sequence number
    std::string destination;          // "0.24"
    std::uint32_t offset = 0;         // FileData: offset of the data
    std::size_t fileDataLength = 0;   // FileData: number of data bytes
    PduKind acked = PduKind::Unknown; // ACK: the directive acknowledged (EOF or FIN)
    std::uint8_t condition = 0;       // EOF/FIN: condition code (4 bits)
};

// Never throws. nullopt if the bytes are not a complete PDU header + data field.
std::optional<PduInfo> inspectPdu(const Bytes& pdu);

// "0.24" from the ID bytes {0x00, 0x18}.
std::string entityToString(const std::uint8_t* bytes, std::size_t length);

// Short text for logs and diagnostics: "MD 0.23_42 -> 0.24", "FD 0.24_7 off=4000 len=4000".
std::string describePdu(const PduInfo& info);

} // namespace mcs::cfdp
