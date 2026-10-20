#pragma once

// Text layout for the session's "help" output, independent of any UI library.

#include <string>
#include <vector>

namespace mcs::ui {

struct HelpItem {
    std::string usage;         // "verbose <on|off>"
    std::string description;   // "Show or set hexdump of every sent packet"
};

// A run of bytes in a line of text.
struct TextSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
};

// One line of help text; `names` marks the command names in it, so a UI can
// colour them and leave the descriptions plain.
struct HelpLine {
    std::string text;
    std::vector<TextSpan> names;   // in order, not overlapping
};

// Lays out the commands of "help" in lines of at most `width` columns:
//
//     help     This help message                  verbose  Show or set hexdump of every
//     fsw      Send commands to flight software            sent packet
//              (target 'fsw')                     arm      Allow the next critical ...
//
// Two columns when there is room for both (width >= 82), else one. Entries
// fill the left column, then the right, balancing the line counts. Inside a
// column, a long description wraps under the description column; a usage
// too long for its column gets a line of its own.
std::vector<HelpLine> formatHelp(const std::vector<HelpItem>& items, int width);

// The line with its names in cyan (ANSI escape codes), for a terminal.
std::string toAnsi(const HelpLine& line);

// Word-wraps one line to `width` columns. Continuation lines keep the line's
// leading whitespace (hanging indent). Words longer than a line are split.
std::vector<std::string> wrapLine(const std::string& line, int width);

// Display width of UTF-8 text, counting each code point as one column.
std::size_t displayWidth(const std::string& text);

} // namespace mcs::ui
