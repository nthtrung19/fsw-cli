#include "ui/tui_widgets.hpp"

#include "core/text.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace mcs::ui::tui {

using namespace ftxui;

std::string clockText(TlmTime t)
{
    const std::time_t secs = TlmClock::to_time_t(t);
    std::tm local{};
    localtime_r(&secs, &local);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms));
    return buf;
}

double secondsBetween(TlmTime from, TlmTime to)
{
    return std::chrono::duration<double>(to - from).count();
}

std::string ageText(double seconds)
{
    char buf[32];
    if (seconds < 10) {
        std::snprintf(buf, sizeof buf, "%.1f s", seconds);
    } else if (seconds < 120) {
        std::snprintf(buf, sizeof buf, "%d s", static_cast<int>(seconds));
    } else if (seconds < 7200) {
        std::snprintf(buf, sizeof buf, "%d min", static_cast<int>(seconds / 60));
    } else {
        std::snprintf(buf, sizeof buf, "%d h", static_cast<int>(seconds / 3600));
    }
    return buf;
}

Color ageColor(double seconds)
{
    if (seconds < kFreshSeconds) {
        return Color::Default;
    }
    return seconds < kStaleSeconds ? Color::Yellow : Color::Red;
}

std::string upper(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string repeat(const char* glyph, int count)
{
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += glyph;
    }
    return out;
}

Color pick(bool condition, Color yes, Color no)
{
    return condition ? yes : no;
}

Element cell(const std::string& s, int width)
{
    return text(s) | size(WIDTH, EQUAL, width);
}

Color eventColor(std::int64_t type)
{
    switch (type) {
    case 1: return Color::GrayDark;   // DEBUG
    case 2: return Color::Green;      // INFO
    case 3: return Color::Red;        // ERROR
    case 4: return Color::RedLight;   // CRITICAL
    case 5: return Color::Yellow;     // WARNING
    default: return Color::Default;
    }
}

Color appColor(const std::string& app)
{
    static const Color kPalette[] = {Color::Cyan,      Color::Green,     Color::Yellow,
                                     Color::BlueLight, Color::MagentaLight, Color::GreenLight,
                                     Color::CyanLight, Color::YellowLight};
    unsigned hash = 0;
    for (const char c : app) {
        hash = hash * 31U + static_cast<unsigned char>(c);
    }
    return kPalette[hash % (sizeof kPalette / sizeof kPalette[0])];
}

std::optional<Color> enumColor(const std::string& value)
{
    const std::string v = toLower(value);
    const auto has = [&v](const char* word) { return v.find(word) != std::string::npos; };
    if (v.rfind("?", 0) == 0 || has("err") || has("fail") || has("fault") || has("bad") || has("crit")) {
        return Color::Red;
    }
    if (has("disab") || has("inactive") || v == "off" || v == "false" || v == "none" || v == "idle") {
        return Color::GrayDark;
    }
    if (has("enab") || has("active") || v == "on" || v == "true" || v == "ok" || has("good")) {
        return Color::Green;
    }
    return std::nullopt;
}

} // namespace mcs::ui::tui
