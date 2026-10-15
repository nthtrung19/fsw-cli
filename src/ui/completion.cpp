#include "ui/completion.hpp"

#include <algorithm>

namespace mcs::ui {

namespace {

std::string commonPrefix(const std::vector<std::string>& v)
{
    std::string prefix = v.front();
    for (const auto& s : v) {
        const auto mismatch = std::mismatch(prefix.begin(), prefix.end(), s.begin(), s.end());
        prefix.erase(mismatch.first, prefix.end());
    }
    return prefix;
}

} // namespace

CompletionResult applyCompletion(const std::string& line, const std::vector<std::string>& completions)
{
    CompletionResult result;
    std::vector<std::string> lines;
    std::vector<std::string> descriptions;
    for (const auto& c : completions) {
        const auto hint = c.find(kCompletionHintMarker);
        if (hint != std::string::npos) {
            result.hints.push_back(c.substr(hint + 1));
            continue;
        }
        const auto sep = c.find(kCompletionHelpSeparator);
        lines.push_back(c.substr(0, sep));
        descriptions.push_back(sep == std::string::npos ? std::string() : c.substr(sep + 1));
    }

    if (lines.empty()) {
        return result;
    }
    result.hints.clear();   // hints only matter when nothing can be completed
    if (lines.size() == 1) {
        result.newLine = lines.front() + ' ';
        return result;
    }
    const std::string prefix = commonPrefix(lines);
    if (prefix.size() > line.size()) {
        result.newLine = prefix;
        return result;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        result.items.push_back({lines[i].substr(lines[i].find_last_of(' ') + 1), descriptions[i]});
    }
    return result;
}

std::vector<std::string> gridRows(const std::vector<std::string>& words, int width)
{
    if (words.empty()) {
        return {};
    }
    std::size_t longest = 0;
    for (const auto& w : words) {
        longest = std::max(longest, w.size());
    }
    const std::size_t column = longest + 3;
    const std::size_t room = width > 3 ? static_cast<std::size_t>(width) - 3 : 0;
    const std::size_t columns = std::max<std::size_t>(1, (room + 3) / column);   // the last one needs no gap
    const std::size_t rows = (words.size() + columns - 1) / columns;

    std::vector<std::string> out;
    for (std::size_t r = 0; r < rows; ++r) {
        std::string line = "   ";
        for (std::size_t c = 0; c < columns; ++c) {
            const std::size_t i = c * rows + r;
            if (i >= words.size()) {
                break;
            }
            line += words[i];
            line.append(column - words[i].size(), ' ');
        }
        line.erase(line.find_last_not_of(' ') + 1);
        out.push_back(line);
    }
    return out;
}

} // namespace mcs::ui
