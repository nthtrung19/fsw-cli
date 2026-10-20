#pragma once

#include "telemetry/tlm_decoder.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcs {

// A value saved by "save <var> <packet> <field>".
struct VariableValue {
    std::string text;                     // what replaces $var when it is not a number
    std::optional<std::int64_t> integer;  // integer and enum fields (the raw value)
    std::optional<double> number;         // float fields
};

// The value of a decoded telemetry field as a variable.
VariableValue variableValue(const DecodedValue& value);

// Script variables. In any script line, before it runs:
//   $name      the value (integers and enums as their number)
//   $name+N    the value plus N, $name-N minus N (N an integer, or a decimal for floats)
//   $$         a literal '$'
// Names are letters, digits and '_' (not starting with a digit); case matters.
class VariableTable {
public:
    void set(const std::string& name, VariableValue value);
    const VariableValue* find(const std::string& name) const;

    // Throws ParseError for an undefined variable, a bad reference ("$", "$1"),
    // or arithmetic on a text value.
    std::string expand(std::string_view line) const;

    // Names referenced in `line`, in order (no lookup). Throws ParseError for a
    // bad reference.
    static std::vector<std::string> referenced(std::string_view line);

    static bool isName(std::string_view name);

private:
    std::map<std::string, VariableValue> values_;
};

} // namespace mcs
