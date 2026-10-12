#pragma once

// Tab completion for the full-screen session, independent of any UI library.
//
// The patched cli library (external/patches/cli-tab-completion.patch) returns
// completions as "<line>[\x1f<description>]", and hints (what an argument
// expects) as "<line>\x1e<text>". The rules mirror the patched
// CommandProcessor (cli/detail/commandprocessor.h) used by the line-mode REPL.

#include <optional>
#include <string>
#include <vector>

namespace mcs::ui {

// Same values as cli::CompletionHelpSeparator / cli::CompletionHintMarker
// (checked with static_assert where both are visible, in tui_session.cpp).
inline constexpr char kCompletionHelpSeparator = '\x1f';
inline constexpr char kCompletionHintMarker = '\x1e';

struct CompletionItem {
    std::string word;          // the word being completed: "noop", not "fsw ds noop"
    std::string description;   // may be empty
};

struct CompletionResult {
    std::optional<std::string> newLine;   // replace the input with this
    std::vector<CompletionItem> items;    // list to show (several matches, no longer prefix)
    std::vector<std::string> hints;       // shown when nothing can be completed
};

// One Tab press on `line`, given the session's completions for it:
//   - one match            -> newLine = match + ' '
//   - longer common prefix -> newLine = prefix
//   - otherwise            -> items (several matches) or hints (none)
CompletionResult applyCompletion(const std::string& line, const std::vector<std::string>& completions);

} // namespace mcs::ui
