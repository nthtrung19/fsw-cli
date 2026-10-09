#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcs {

enum class FieldType { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, String, Padding };

// "u8".."u64", "i8".."i64", "f32", "f64", "string", "padding" (case-insensitive).
std::optional<FieldType> parseFieldType(std::string_view text);
std::string toString(FieldType type);

// Size in bytes for numeric types; 0 for String and Padding (size given explicitly).
std::size_t naturalSize(FieldType type);

bool isUnsignedInt(FieldType type);
bool isSignedInt(FieldType type);
bool isInteger(FieldType type);
bool isFloat(FieldType type);

struct EnumValue {
    std::string name;
    std::int64_t value;
};

// One field of a command payload, in payload order.
struct FieldDef {
    std::string name;                      // empty for Padding
    FieldType type = FieldType::U8;
    std::size_t size = 0;                  // bytes on the wire
    std::vector<EnumValue> enumValues;     // integer fields only; order kept for help
    std::optional<std::int64_t> min;       // integer fields only, inclusive
    std::optional<std::int64_t> max;
    std::string help;
    std::optional<std::string> defaultValue;   // argument text used when it is omitted

    bool userVisible() const { return type != FieldType::Padding; }

    // Text shown in help, e.g. "state: disable|enable", "count: u32 1..100",
    // "path: string[64]", "port: u16 = 5011".
    std::string describe() const;

    // Convenience constructors (mostly for code and tests).
    static FieldDef number(std::string name, FieldType type);
    static FieldDef enumeration(std::string name, FieldType type, std::vector<EnumValue> values);
    static FieldDef string(std::string name, std::size_t size);
    static FieldDef padding(std::size_t size);
};

} // namespace mcs
