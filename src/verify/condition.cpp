#include "verify/condition.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

namespace mcs {

namespace {

template <typename T>
bool compare(CompareOp op, const T& actual, const T& expected)
{
    switch (op) {
        case CompareOp::Eq: return actual == expected;
        case CompareOp::Ne: return actual != expected;
        case CompareOp::Lt: return actual < expected;
        case CompareOp::Le: return actual <= expected;
        case CompareOp::Gt: return actual > expected;
        case CompareOp::Ge: return actual >= expected;
    }
    return false;
}

const EnumValue* findEnum(const TlmFieldDef& field, std::string_view name)
{
    for (const auto& e : field.enumValues) {
        if (iequals(e.name, name)) {
            return &e;
        }
    }
    return nullptr;
}

[[noreturn]] void invalid(const TlmFieldDef& field, std::string_view value, const std::string& reason)
{
    throw ParseError("invalid value '" + std::string(value) + "' for " + field.name + " ("
                     + toString(field.type) + "): " + reason);
}

} // namespace

std::optional<CompareOp> parseCompareOp(std::string_view text)
{
    if (text == "==") return CompareOp::Eq;
    if (text == "!=") return CompareOp::Ne;
    if (text == "<") return CompareOp::Lt;
    if (text == "<=") return CompareOp::Le;
    if (text == ">") return CompareOp::Gt;
    if (text == ">=") return CompareOp::Ge;
    return std::nullopt;
}

std::string toString(CompareOp op)
{
    switch (op) {
        case CompareOp::Eq: return "==";
        case CompareOp::Ne: return "!=";
        case CompareOp::Lt: return "<";
        case CompareOp::Le: return "<=";
        case CompareOp::Gt: return ">";
        case CompareOp::Ge: return ">=";
    }
    return "?";
}

Condition Condition::parse(const TlmFieldDef& field, std::string_view op, std::string_view value,
                           bool checkValue)
{
    Condition c;
    const auto parsedOp = parseCompareOp(op);
    if (!parsedOp) {
        throw ParseError("unknown operator '" + std::string(op) + "' (use == != < <= > >=)");
    }
    c.op_ = *parsedOp;
    c.text_ = std::string(value);

    if (field.type == FieldType::String) {
        c.kind_ = Kind::String;
        if (c.op_ != CompareOp::Eq && c.op_ != CompareOp::Ne) {
            throw ParseError(field.name + " is a string: only == and != compare it");
        }
        return c;
    }
    if (isFloat(field.type)) {
        c.kind_ = Kind::Float;
        if (!checkValue) {
            return c;
        }
        const auto v = parseDouble(value);
        if (!v) {
            invalid(field, value, "not a number");
        }
        // The FSW sent an f32: compare at that precision.
        c.float_ = field.type == FieldType::F32 ? static_cast<double>(static_cast<float>(*v)) : *v;
        return c;
    }
    if (!isInteger(field.type)) {
        throw ParseError(field.name + " cannot be compared");
    }

    c.kind_ = isUnsignedInt(field.type) ? Kind::Unsigned : Kind::Signed;
    if (!checkValue) {
        return c;
    }
    if (const EnumValue* e = findEnum(field, value)) {
        c.signed_ = e->value;
        c.unsigned_ = static_cast<std::uint64_t>(e->value);
        c.text_ = e->name;
        return c;
    }
    if (c.kind_ == Kind::Unsigned) {
        const auto v = parseUnsigned(value);
        if (!v) {
            invalid(field, value, field.enumValues.empty() ? "not an unsigned integer"
                                                           : "not a number or one of the field's names");
        }
        c.unsigned_ = *v;
    } else {
        const auto v = parseSigned(value);
        if (!v) {
            invalid(field, value, field.enumValues.empty() ? "not an integer"
                                                           : "not a number or one of the field's names");
        }
        c.signed_ = *v;
    }
    return c;
}

bool Condition::matches(const DecodedValue& actual) const
{
    if (actual.missing) {
        return false;
    }
    switch (kind_) {
        case Kind::String:
            return compare(op_, actual.text, text_);
        case Kind::Float:
            return actual.number && compare(op_, *actual.number, float_);
        case Kind::Unsigned:
            return actual.raw && compare(op_, static_cast<std::uint64_t>(*actual.raw), unsigned_);
        case Kind::Signed:
            return actual.raw && compare(op_, *actual.raw, signed_);
    }
    return false;
}

std::string Condition::describe() const
{
    return toString(op_) + " " + (kind_ == Kind::String ? "\"" + text_ + "\"" : text_);
}

} // namespace mcs
