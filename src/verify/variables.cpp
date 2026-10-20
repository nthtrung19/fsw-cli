#include "verify/variables.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace mcs {

namespace {

bool isNameStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isNameChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// One "$name[+-N]" starting at `at` (the '$').
struct Reference {
    std::string name;
    std::string offset;    // "+1", "-2.5" or empty
    std::size_t end = 0;   // one past the reference
};

Reference readReference(std::string_view line, std::size_t at)
{
    std::size_t i = at + 1;
    if (i >= line.size() || !isNameStart(line[i])) {
        throw ParseError("'$' must be followed by a variable name (use $$ for a '$')");
    }
    Reference r;
    while (i < line.size() && isNameChar(line[i])) {
        r.name += line[i++];
    }
    if (i + 1 < line.size() && (line[i] == '+' || line[i] == '-')
        && (std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0)) {
        const std::size_t start = i++;
        while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i])) != 0 || line[i] == '.')) {
            ++i;
        }
        r.offset = std::string(line.substr(start, i - start));
    }
    r.end = i;
    return r;
}

std::string formatNumber(double v)
{
    std::ostringstream out;
    out << std::setprecision(17) << v;
    return out.str();
}

std::string quoteIfNeeded(const std::string& text)
{
    if (!text.empty() && text.find_first_of(" \t\"'") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

std::string valueText(const Reference& r, const VariableValue& v)
{
    if (r.offset.empty()) {
        if (v.integer) {
            return std::to_string(*v.integer);
        }
        if (v.number) {
            return formatNumber(*v.number);
        }
        return quoteIfNeeded(v.text);
    }
    if (v.integer) {
        const auto n = parseSigned(r.offset);
        if (!n) {
            throw ParseError("$" + r.name + r.offset + ": " + r.name + " is an integer: add an integer");
        }
        return std::to_string(*v.integer + *n);
    }
    if (v.number) {
        const auto n = parseDouble(r.offset);
        if (!n) {
            throw ParseError("$" + r.name + r.offset + ": not a number");
        }
        return formatNumber(*v.number + *n);
    }
    throw ParseError("$" + r.name + r.offset + ": " + r.name + " is text ('" + v.text + "'), not a number");
}

} // namespace

VariableValue variableValue(const DecodedValue& value)
{
    VariableValue v;
    v.text = value.text;
    if (value.raw) {
        v.integer = value.raw;
    } else if (value.number) {
        v.number = value.number;
    }
    return v;
}

void VariableTable::set(const std::string& name, VariableValue value)
{
    values_[name] = std::move(value);
}

const VariableValue* VariableTable::find(const std::string& name) const
{
    const auto it = values_.find(name);
    return it != values_.end() ? &it->second : nullptr;
}

std::string VariableTable::expand(std::string_view line) const
{
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] != '$') {
            out += line[i++];
            continue;
        }
        if (i + 1 < line.size() && line[i + 1] == '$') {
            out += '$';
            i += 2;
            continue;
        }
        const Reference r = readReference(line, i);
        const VariableValue* v = find(r.name);
        if (v == nullptr) {
            throw ParseError("undefined variable $" + r.name + " (set it with 'save " + r.name
                             + " <packet> <field>' before this line)");
        }
        out += valueText(r, *v);
        i = r.end;
    }
    return out;
}

std::vector<std::string> VariableTable::referenced(std::string_view line)
{
    std::vector<std::string> names;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] != '$') {
            ++i;
        } else if (i + 1 < line.size() && line[i + 1] == '$') {
            i += 2;
        } else {
            const Reference r = readReference(line, i);
            names.push_back(r.name);
            i = r.end;
        }
    }
    return names;
}

bool VariableTable::isName(std::string_view name)
{
    if (name.empty() || !isNameStart(name.front())) {
        return false;
    }
    for (const char c : name) {
        if (!isNameChar(c)) {
            return false;
        }
    }
    return true;
}

} // namespace mcs
