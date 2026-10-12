#pragma once

#include "catalog/field_def.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mcs {

// cFE 6.7 telemetry header: 6-byte CCSDS primary header + 6-byte time
// (CFE_SB_TLM_HDR_SIZE). Field offsets in a TlmPacketDef are relative to the
// first byte after it.
inline constexpr std::size_t kTlmHeaderSize = 12;

// One field of a telemetry payload.
struct TlmFieldDef {
    std::string name;                    // empty for Padding
    FieldType type = FieldType::U8;
    std::size_t offset = 0;              // from the start of the payload
    std::size_t size = 0;                // bytes on the wire
    std::vector<EnumValue> enumValues;   // integer fields only
    std::string units;                   // shown after the value, e.g. "B", "s"
    bool hex = false;                    // integer shown as 0x..
    std::string help;

    bool visible() const { return type != FieldType::Padding; }
};

// One telemetry packet (one MID) of an app.
//
// Event packets (EVS long/short event messages) are also listed in the event
// log; they must define the fields app_name, event_id and event_type, and may
// define message.
struct TlmPacketDef {
    std::string app;                     // "ds"
    std::string name;                    // "hk"
    std::uint16_t mid = 0;               // telemetry message ID (CCSDS stream ID)
    std::string help;
    bool isEvent = false;
    std::vector<TlmFieldDef> fields;     // in payload order, offsets filled in

    std::string fullName() const { return app + "." + name; }

    // Bytes after the telemetry header that the definition covers.
    std::size_t payloadSize() const;

    const TlmFieldDef* findField(const std::string& fieldName) const;
};

} // namespace mcs
