#pragma once

// Stateless pieces of the full-screen session (tui_*.cpp): text formatting,
// colours and layout constants. Internal to ui/.

#include "telemetry/tlm_store.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace mcs::ui::tui {

std::string clockText(TlmTime t);                // "14:03:20.125" (local time)
double secondsBetween(TlmTime from, TlmTime to);
std::string ageText(double seconds);             // "0.4 s", "12 s", "3 min", "2 h"

inline constexpr double kFreshSeconds = 5;
inline constexpr double kStaleSeconds = 30;
ftxui::Color ageColor(double seconds);

std::string upper(std::string s);
std::string repeat(const char* glyph, int count);
// Color constants are of different enum types; this picks between any two.
ftxui::Color pick(bool condition, ftxui::Color yes, ftxui::Color no);
ftxui::Element cell(const std::string& s, int width);
ftxui::Color eventColor(std::int64_t type);
// A stable colour per app, so a card's border and badge identify its app at a glance.
ftxui::Color appColor(const std::string& app);
// Colour of an enum value by what its name says: good states green, off
// states grey, errors and unknown values red; anything else unchanged.
std::optional<ftxui::Color> enumColor(const std::string& value);

inline const char* const kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
inline constexpr int kListWidth = 30;   // telemetry screen: packet checklist

} // namespace mcs::ui::tui
