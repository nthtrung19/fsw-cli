#pragma once

#include "telemetry/tlm_decoder.hpp"
#include "telemetry/tlm_def.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcs {

// The comparison of an "expect" check: "<op> <value>" against one telemetry field.
enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

// "==", "!=", "<", "<=", ">", ">=".
std::optional<CompareOp> parseCompareOp(std::string_view text);
std::string toString(CompareOp op);

// The expected value is parsed against the field when the check is read, so a
// mistake is reported before anything waits:
//   integer fields  a number (decimal or 0x hex) or one of the field's enum names
//                   (case-insensitive); compared with the raw value
//   float fields    a decimal number; f32 fields compare at f32 precision, so
//                   "== 90.01" matches the 90.01f the FSW sent
//   string fields   any text; only == and != (the decoded text, cut at the first NUL)
class Condition {
public:
    // Throws ParseError for an unknown operator, a value the field cannot hold,
    // or an ordering operator on a string field. With `checkValue` false only
    // the operator is checked (the value comes from a variable that is not
    // known yet) and the condition must not be evaluated.
    static Condition parse(const TlmFieldDef& field, std::string_view op, std::string_view value,
                           bool checkValue = true);

    // False for a value missing from a short packet.
    bool matches(const DecodedValue& actual) const;

    // "== 4", "== enable", "< 2.5", "== \"ABC\"".
    std::string describe() const;

private:
    enum class Kind { Unsigned, Signed, Float, String };

    CompareOp op_ = CompareOp::Eq;
    Kind kind_ = Kind::Signed;
    std::uint64_t unsigned_ = 0;
    std::int64_t signed_ = 0;
    double float_ = 0;
    std::string text_;     // String: the expected text; otherwise the value as written
};

} // namespace mcs
