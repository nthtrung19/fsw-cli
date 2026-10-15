#include "ui/help_format.hpp"

#include <algorithm>

namespace mcs::ui {

namespace {

constexpr std::size_t kMinColumn = 38;    // narrower than this: one column
constexpr std::size_t kMinDescription = 12;
constexpr const char* kIndent = "  ";
constexpr const char* kGap = "    ";   // between the two columns

bool isContinuationByte(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// The first `columns` display columns of `text` (never splits a code point).
std::string takeColumns(const std::string& text, std::size_t columns)
{
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!isContinuationByte(text[i])) {
            if (seen == columns) {
                return text.substr(0, i);
            }
            ++seen;
        }
    }
    return text;
}

std::string dropColumns(const std::string& text, std::size_t columns)
{
    return text.substr(takeColumns(text, columns).size());
}

std::string padTo(std::string text, std::size_t columns)
{
    const std::size_t w = displayWidth(text);
    if (w < columns) {
        text.append(columns - w, ' ');
    }
    return text;
}

std::string rtrim(std::string text)
{
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

// Words of `text` in lines of at most `width` columns.
std::vector<std::string> wrapWords(const std::string& text, std::size_t width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t start = text.find_first_not_of(' ', pos);
        if (start == std::string::npos) {
            break;
        }
        const std::size_t end = std::min(text.find(' ', start), text.size());
        std::string word = text.substr(start, end - start);
        pos = end;

        while (!word.empty()) {
            const std::size_t room = line.empty() ? width : width - std::min(width, displayWidth(line) + 1);
            if (displayWidth(word) <= room) {
                line += (line.empty() ? "" : " ") + word;
                word.clear();
            } else if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            } else {
                // A word longer than a whole line: split it.
                lines.push_back(takeColumns(word, width));
                word = dropColumns(word, width);
            }
        }
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

// One entry as lines of exactly `column` columns; `names` marks the usage.
std::vector<HelpLine> entryLines(const HelpItem& item, std::size_t column, std::size_t usageWidth)
{
    const std::size_t descWidth = column - usageWidth - 2;
    const std::string under(usageWidth + 2, ' ');
    std::vector<HelpLine> out;

    const std::vector<std::string> desc = item.description.empty()
                                              ? std::vector<std::string>{}
                                              : wrapWords(item.description, descWidth);
    std::size_t next = 0;
    if (displayWidth(item.usage) <= usageWidth) {
        out.push_back({padTo(item.usage, usageWidth) + "  " + (desc.empty() ? "" : desc[next++]),
                       {{0, item.usage.size()}}});
    } else {
        // A long usage gets lines of its own; the description starts below it.
        for (const auto& l : wrapWords(item.usage, column)) {
            out.push_back({l, {{0, l.size()}}});
        }
    }
    for (; next < desc.size(); ++next) {
        out.push_back({under + desc[next], {}});
    }
    for (auto& l : out) {
        l.text = padTo(l.text, column);
    }
    return out;
}

// `line` moved `by` bytes to the right.
HelpLine shifted(HelpLine line, std::size_t by)
{
    for (auto& span : line.names) {
        span.offset += by;
    }
    return line;
}

} // namespace

std::size_t displayWidth(const std::string& text)
{
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(),
                                                  [](char c) { return !isContinuationByte(c); }));
}

std::vector<std::string> wrapLine(const std::string& line, int width)
{
    const auto w = static_cast<std::size_t>(std::max(width, 8));
    if (displayWidth(line) <= w) {
        return {line};
    }
    const std::size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {line.substr(0, w)};   // only spaces
    }
    const std::string indent(std::min(first, w / 2), ' ');
    std::vector<std::string> out;
    for (const auto& l : wrapWords(line, w - indent.size())) {
        out.push_back(indent + l);
    }
    return out;
}

std::vector<HelpLine> formatHelp(const std::vector<HelpItem>& items, int width)
{
    const auto total = static_cast<std::size_t>(std::max(width, 30));
    const std::size_t indent = std::char_traits<char>::length(kIndent);
    const std::size_t gap = std::char_traits<char>::length(kGap);
    const bool twoColumns = items.size() > 1 && total >= indent + 2 * kMinColumn + gap;
    const std::size_t column = twoColumns ? (total - indent - gap) / 2 : total - indent;

    // Name column: as wide as the longest usage, at most ~45% of the column,
    // leaving at least kMinDescription for the description.
    std::size_t usageWidth = 0;
    for (const auto& item : items) {
        usageWidth = std::max(usageWidth, displayWidth(item.usage));
    }
    usageWidth = std::min({usageWidth, std::max<std::size_t>(8, column * 45 / 100),
                           column > kMinDescription + 2 ? column - kMinDescription - 2 : 4});

    std::vector<std::vector<HelpLine>> blocks;
    std::size_t lineCount = 0;
    for (const auto& item : items) {
        blocks.push_back(entryLines(item, column, usageWidth));
        lineCount += blocks.back().size();
    }

    std::vector<HelpLine> out;
    const auto finish = [&out](HelpLine line) {
        line.text = rtrim(std::move(line.text));
        out.push_back(std::move(line));
    };
    if (!twoColumns) {
        for (const auto& block : blocks) {
            for (const auto& l : block) {
                HelpLine line = shifted(l, indent);
                line.text = kIndent + line.text;
                finish(std::move(line));
            }
        }
        return out;
    }

    // Split so the left column is the longer one by as little as possible.
    std::size_t split = 0;
    std::size_t leftLines = 0;
    while (split < blocks.size() && leftLines * 2 < lineCount) {
        leftLines += blocks[split++].size();
    }
    std::vector<HelpLine> left;
    std::vector<HelpLine> right;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto& side = i < split ? left : right;
        side.insert(side.end(), blocks[i].begin(), blocks[i].end());
    }
    const HelpLine blank{std::string(column, ' '), {}};
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        HelpLine line = shifted(i < left.size() ? left[i] : blank, indent);
        line.text = kIndent + line.text + kGap;
        if (i < right.size()) {
            const HelpLine r = shifted(right[i], line.text.size());
            line.text += r.text;
            line.names.insert(line.names.end(), r.names.begin(), r.names.end());
        }
        finish(std::move(line));
    }
    return out;
}

std::string toAnsi(const HelpLine& line)
{
    std::string out;
    std::size_t at = 0;
    for (const auto& span : line.names) {
        out += line.text.substr(at, span.offset - at);
        out += "\x1b[36m" + line.text.substr(span.offset, span.length) + "\x1b[0m";
        at = span.offset + span.length;
    }
    return out + line.text.substr(std::min(at, line.text.size()));
}

} // namespace mcs::ui
