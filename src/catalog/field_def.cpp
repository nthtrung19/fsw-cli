#include "catalog/field_def.hpp"

#include "core/text.hpp"

#include <array>
#include <utility>

namespace mcs {

namespace {

struct TypeInfo {
    FieldType type;
    const char* name;
    std::size_t size;
};

constexpr std::array<TypeInfo, 12> kTypes{{
    {FieldType::U8, "u8", 1},   {FieldType::U16, "u16", 2}, {FieldType::U32, "u32", 4},
    {FieldType::U64, "u64", 8}, {FieldType::I8, "i8", 1},   {FieldType::I16, "i16", 2},
    {FieldType::I32, "i32", 4}, {FieldType::I64, "i64", 8}, {FieldType::F32, "f32", 4},
    {FieldType::F64, "f64", 8}, {FieldType::String, "string", 0},
    {FieldType::Padding, "padding", 0},
}};

const TypeInfo& info(FieldType type)
{
    for (const auto& t : kTypes) {
        if (t.type == type) {
            return t;
        }
    }
    return kTypes.back();   // unreachable: every enumerator is in the table
}

} // namespace

std::optional<FieldType> parseFieldType(std::string_view text)
{
    const std::string lower = toLower(text);
    for (const auto& t : kTypes) {
        if (lower == t.name) {
            return t.type;
        }
    }
    return std::nullopt;
}

std::string toString(FieldType type) { return info(type).name; }

std::size_t naturalSize(FieldType type) { return info(type).size; }

bool isUnsignedInt(FieldType type)
{
    return type == FieldType::U8 || type == FieldType::U16 || type == FieldType::U32
        || type == FieldType::U64;
}

bool isSignedInt(FieldType type)
{
    return type == FieldType::I8 || type == FieldType::I16 || type == FieldType::I32
        || type == FieldType::I64;
}

bool isInteger(FieldType type) { return isUnsignedInt(type) || isSignedInt(type); }

bool isFloat(FieldType type) { return type == FieldType::F32 || type == FieldType::F64; }

std::string FieldDef::describe() const
{
    std::string out = name + ": ";
    if (!enumValues.empty()) {
        std::vector<std::string> names;
        for (const auto& e : enumValues) {
            names.push_back(e.name);
        }
        return out + join(names, "|");
    }
    if (type == FieldType::String) {
        return out + "string[" + std::to_string(size) + "]";
    }
    out += toString(type);
    if (min || max) {
        out += ' ';
        out += min ? std::to_string(*min) : "";
        out += "..";
        out += max ? std::to_string(*max) : "";
    }
    return out;
}

FieldDef FieldDef::number(std::string fieldName, FieldType fieldType)
{
    FieldDef f;
    f.name = std::move(fieldName);
    f.type = fieldType;
    f.size = naturalSize(fieldType);
    return f;
}

FieldDef FieldDef::enumeration(std::string fieldName, FieldType fieldType,
                               std::vector<EnumValue> values)
{
    FieldDef f = number(std::move(fieldName), fieldType);
    f.enumValues = std::move(values);
    return f;
}

FieldDef FieldDef::string(std::string fieldName, std::size_t fieldSize)
{
    FieldDef f;
    f.name = std::move(fieldName);
    f.type = FieldType::String;
    f.size = fieldSize;
    return f;
}

FieldDef FieldDef::padding(std::size_t fieldSize)
{
    FieldDef f;
    f.type = FieldType::Padding;
    f.size = fieldSize;
    return f;
}

} // namespace mcs
