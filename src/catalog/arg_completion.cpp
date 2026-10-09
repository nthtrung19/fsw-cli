#include "catalog/arg_completion.hpp"

#include "core/text.hpp"

namespace mcs {

ArgCompletion completeArgument(const CommandDef& command, std::size_t index, const std::string& partial)
{
    ArgCompletion out;
    const std::size_t total = command.argCount();
    if (index >= total) {
        out.hint = "(no more arguments: press Enter to send)";
        return out;
    }

    const FieldDef* field = nullptr;
    std::size_t visible = 0;
    for (const auto& f : command.fields) {
        if (f.userVisible() && visible++ == index) {
            field = &f;
            break;
        }
    }

    // Enum names complete like command names; the encoder matches them
    // case-insensitively, so the prefix match does too.
    const std::string typed = toLower(partial);
    for (const auto& e : field->enumValues) {
        if (toLower(e.name).rfind(typed, 0) == 0) {
            // "state = 0": the field it sets and the value sent. The field help
            // ("EnableState") is left out: next to "disable" it reads as a description.
            out.values.emplace_back(e.name, field->name + " = " + std::to_string(e.value));
        }
    }
    if (!out.values.empty()) {
        return out;
    }

    out.hint = "<" + field->describe() + ">";
    if (!field->help.empty()) {
        out.hint += "   " + field->help;
    }
    out.hint += "  (argument " + std::to_string(index + 1) + " of " + std::to_string(total);
    if (field->type == FieldType::String) {
        out.hint += ", at most " + std::to_string(field->size - 1) + " characters";
    }
    if (field->defaultValue) {
        out.hint += ", optional";   // describe() already shows "= <default>"
    }
    out.hint += ")";
    return out;
}

} // namespace mcs
