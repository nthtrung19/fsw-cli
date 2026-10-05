#include "encode/payload_encoder.hpp"

#include "core/byte_writer.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"

#include <cmath>
#include <limits>

namespace fswcli {

namespace {

std::uint64_t unsignedMax(FieldType type)
{
    switch (type) {
        case FieldType::U8:  return std::numeric_limits<std::uint8_t>::max();
        case FieldType::U16: return std::numeric_limits<std::uint16_t>::max();
        case FieldType::U32: return std::numeric_limits<std::uint32_t>::max();
        default:             return std::numeric_limits<std::uint64_t>::max();
    }
}

std::pair<std::int64_t, std::int64_t> signedLimits(FieldType type)
{
    switch (type) {
        case FieldType::I8:  return {std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
        case FieldType::I16: return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
        case FieldType::I32: return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
        default:             return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    }
}

// Builds "<cmd>: invalid value 'x' for <state: disable|enable>: reason".
[[noreturn]] void invalid(const CommandDef& cmd, const FieldDef& field, const std::string& arg,
                          const std::string& reason)
{
    throw ParseError(cmd.name + ": invalid value '" + arg + "' for <" + field.describe()
                     + ">: " + reason);
}

const EnumValue* findEnum(const FieldDef& field, const std::string& arg)
{
    for (const auto& e : field.enumValues) {
        if (iequals(e.name, arg)) {
            return &e;
        }
    }
    return nullptr;
}

// An enum field accepts only its listed values (by name or by number).
void requireEnumMember(const CommandDef& cmd, const FieldDef& f, const std::string& arg,
                       std::int64_t value)
{
    if (f.enumValues.empty()) {
        return;
    }
    for (const auto& e : f.enumValues) {
        if (e.value == value) {
            return;
        }
    }
    invalid(cmd, f, arg, "not one of the allowed values");
}

void writeUnsigned(ByteWriter& w, FieldType type, std::uint64_t v)
{
    switch (type) {
        case FieldType::U8:  w.u8(static_cast<std::uint8_t>(v)); break;
        case FieldType::U16: w.u16(static_cast<std::uint16_t>(v)); break;
        case FieldType::U32: w.u32(static_cast<std::uint32_t>(v)); break;
        default:             w.u64(v); break;
    }
}

void writeSigned(ByteWriter& w, FieldType type, std::int64_t v)
{
    switch (type) {
        case FieldType::I8:  w.i8(static_cast<std::int8_t>(v)); break;
        case FieldType::I16: w.i16(static_cast<std::int16_t>(v)); break;
        case FieldType::I32: w.i32(static_cast<std::int32_t>(v)); break;
        default:             w.i64(v); break;
    }
}

void encodeUnsigned(ByteWriter& w, const CommandDef& cmd, const FieldDef& f, const std::string& arg)
{
    std::uint64_t value = 0;
    if (const EnumValue* e = findEnum(f, arg)) {
        value = static_cast<std::uint64_t>(e->value);   // validated non-negative by the catalog
    } else {
        const auto parsed = parseUnsigned(arg);
        if (!parsed) {
            invalid(cmd, f, arg, f.enumValues.empty() ? "not an unsigned integer"
                                                      : "not one of the allowed names or a number");
        }
        value = *parsed;
    }
    if (value > unsignedMax(f.type)) {
        invalid(cmd, f, arg, "out of range for " + toString(f.type));
    }
    // Enum values were validated to fit the (unsigned) type, so any value
    // above INT64_MAX cannot be a member.
    if (!f.enumValues.empty()
        && value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid(cmd, f, arg, "not one of the allowed values");
    }
    requireEnumMember(cmd, f, arg, static_cast<std::int64_t>(value));
    // min/max were validated to fit the (unsigned) type, so they are >= 0.
    if (f.min && value < static_cast<std::uint64_t>(*f.min)) {
        invalid(cmd, f, arg, "below minimum " + std::to_string(*f.min));
    }
    if (f.max && value > static_cast<std::uint64_t>(*f.max)) {
        invalid(cmd, f, arg, "above maximum " + std::to_string(*f.max));
    }
    writeUnsigned(w, f.type, value);
}

void encodeSigned(ByteWriter& w, const CommandDef& cmd, const FieldDef& f, const std::string& arg)
{
    std::int64_t value = 0;
    if (const EnumValue* e = findEnum(f, arg)) {
        value = e->value;
    } else {
        const auto parsed = parseSigned(arg);
        if (!parsed) {
            invalid(cmd, f, arg, f.enumValues.empty() ? "not an integer"
                                                      : "not one of the allowed names or a number");
        }
        value = *parsed;
    }
    const auto [lo, hi] = signedLimits(f.type);
    if (value < lo || value > hi) {
        invalid(cmd, f, arg, "out of range for " + toString(f.type));
    }
    requireEnumMember(cmd, f, arg, value);
    if (f.min && value < *f.min) {
        invalid(cmd, f, arg, "below minimum " + std::to_string(*f.min));
    }
    if (f.max && value > *f.max) {
        invalid(cmd, f, arg, "above maximum " + std::to_string(*f.max));
    }
    writeSigned(w, f.type, value);
}

void encodeFloat(ByteWriter& w, const CommandDef& cmd, const FieldDef& f, const std::string& arg)
{
    const auto parsed = parseDouble(arg);
    if (!parsed) {
        invalid(cmd, f, arg, "not a finite number");
    }
    if (f.type == FieldType::F32) {
        if (std::fabs(*parsed) > static_cast<double>(std::numeric_limits<float>::max())) {
            invalid(cmd, f, arg, "out of range for f32");
        }
        w.f32(static_cast<float>(*parsed));
    } else {
        w.f64(*parsed);
    }
}

} // namespace

Bytes PayloadEncoder::encode(const CommandDef& command, const std::vector<std::string>& args) const
{
    const std::size_t expected = command.argCount();
    if (args.size() != expected) {
        throw ParseError(command.name + ": expected " + std::to_string(expected) + " argument"
                         + (expected == 1 ? "" : "s") + ", got " + std::to_string(args.size())
                         + "\n  usage: " + command.usage());
    }

    ByteWriter w(endian_);
    std::size_t next = 0;
    for (const auto& field : command.fields) {
        if (field.type == FieldType::Padding) {
            w.zeros(field.size);
            continue;
        }
        const std::string& arg = args[next++];

        if (isUnsignedInt(field.type)) {
            encodeUnsigned(w, command, field, arg);
        } else if (isSignedInt(field.type)) {
            encodeSigned(w, command, field, arg);
        } else if (isFloat(field.type)) {
            encodeFloat(w, command, field, arg);
        } else {   // String
            if (arg.size() >= field.size) {
                invalid(command, field, arg,
                        "too long (at most " + std::to_string(field.size - 1) + " characters)");
            }
            w.fixedString(arg, field.size);
        }
    }
    return w.take();
}

} // namespace fswcli
