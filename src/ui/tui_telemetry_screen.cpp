// Full-screen session, F2 telemetry: packet checklist and cards.

#include "ui/tui_app.hpp"

namespace mcs::ui::tui {

// ---- F2 telemetry -----------------------------------------------------------
//
//   ╭ PACKETS 2/5 ─────╮ ╭ DS  hk 0x08B8 ── seq 7 · ×8 · 0.3 s ╮ ╭ ? 0x0899 ─────╮
//   │ [x] ds.hk      ● │ │ cmd_accepted ·········· 9 ▲      │ │ 0000  08 99 … │
//   │ [ ] to.hk      ○ │ ╰──────────────────────────────────╯ ╰───────────────╯
//   ╰──────────────────╯
//
// Left: every non-event packet of the catalog (● received, ○ not yet), then
// MIDs received without a definition. Right: the ticked ones as cards, in
// checklist order, wrapped into rows; PgUp/PgDn scroll them.

Element Tui::telemetryScreen(int rows)
{
    if (tlm_.store == nullptr) {
        return vbox({text(""), text("Telemetry is not configured for target '" + service_.targetName() + "'.")
                                   | hcenter,
                     text("Add a \"telemetry\" block in targets.json. F1 returns to the console.") | dim
                         | hcenter});
    }
    buildChecklist();
    displayRows_ = rows;
    return hbox({checklist(rows), text(" "),
                 displayArea(rows, width_ - kListWidth - 2) | flex});   // - 1 more for the scrollbar
}

void Tui::buildChecklist()
{
    items_.clear();
    for (const auto& def : tlm_.store->catalog().packets()) {
        if (def.isEvent) {
            continue;   // shown in the console's event pane
        }
        ChecklistItem item;
        item.mid = def.mid;
        item.app = def.app;
        item.label = def.fullName();
        item.def = &def;
        for (const auto& p : snapshot_.packets) {
            if (p.def == &def) {
                item.packet = &p;
            }
        }
        items_.push_back(item);
    }
    for (const auto& u : snapshot_.unknown) {
        ChecklistItem item;
        item.mid = u.mid;
        item.label = "? " + toHexString(u.mid, 4);
        item.unknown = &u;
        items_.push_back(item);
    }
    // Ticked with `tlm show 0x....` before anything arrived on that MID.
    for (const std::uint16_t mid : tlm_.view.shown) {
        const bool listed = std::any_of(items_.begin(), items_.end(),
                                        [mid](const ChecklistItem& i) { return i.mid == mid; });
        if (!listed) {
            ChecklistItem item;
            item.mid = mid;
            item.label = "? " + toHexString(mid, 4);
            items_.push_back(item);
        }
    }
    listCursor_ = std::clamp(listCursor_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
}

void Tui::toggleCursorItem()
{
    if (items_.empty()) {
        return;
    }
    const std::uint16_t mid = items_[static_cast<std::size_t>(listCursor_)].mid;
    if (!tlm_.view.shown.erase(mid)) {
        tlm_.view.shown.insert(mid);
    }
}

Element Tui::checklist(int rows)
{
    const int inner = std::max(1, rows - 2);
    const int count = static_cast<int>(items_.size());
    const bool overflow = count > inner;
    const int visible = overflow ? std::max(1, inner - 2) : inner;   // two lines for ▲/▼
    if (listCursor_ < listScroll_) {
        listScroll_ = listCursor_;
    } else if (listCursor_ >= listScroll_ + visible) {
        listScroll_ = listCursor_ - visible + 1;
    }
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, count - visible));

    const TlmTime now = displayNow();
    constexpr std::size_t kLabelWidth = static_cast<std::size_t>(kListWidth) - 11;

    // Each line records where it is drawn, for clicks (onMouse). Sized first:
    // reflect() keeps a reference to its Box.
    const int last = std::min(count, listScroll_ + visible);
    listRowItem_.clear();
    if (overflow) {
        listRowItem_.push_back(kMoreAbove);
    }
    for (int i = listScroll_; i < last; ++i) {
        listRowItem_.push_back(i);
    }
    if (overflow) {
        listRowItem_.push_back(kMoreBelow);
    }
    listBoxes_.assign(listRowItem_.size(), Box{});
    listPage_ = visible;
    std::size_t k = 0;   // next line

    Elements lines;
    if (overflow) {
        lines.push_back(text(listScroll_ > 0 ? "  ▲ " + std::to_string(listScroll_) + " more" : "") | dim
                        | reflect(listBoxes_[k++]));
    }
    for (int i = listScroll_; i < last; ++i) {
        const ChecklistItem& item = items_[static_cast<std::size_t>(i)];
        const bool ticked = tlm_.view.shown.count(item.mid) != 0;

        // ● fresh / ● older (yellow) / ● stale (grey) / ○ never received
        std::optional<TlmTime> lastRx;
        if (item.packet != nullptr) {
            lastRx = item.packet->lastRx;
        } else if (item.unknown != nullptr) {
            lastRx = item.unknown->lastRx;
        }
        Element dot = text("○") | color(Color::GrayDark);
        if (lastRx) {
            const double age = secondsBetween(*lastRx, now);
            dot = text("●") | color(age < kFreshSeconds ? Color::Green
                                    : age < kStaleSeconds ? Color::Yellow : Color::GrayDark);
        }

        std::string label = item.label;
        if (label.size() > kLabelWidth) {
            label = label.substr(0, kLabelWidth - 1) + "…";
        }
        Element name;
        if (item.def != nullptr) {
            const std::string rest = label.size() > item.app.size() ? label.substr(item.app.size()) : "";
            name = hbox({text(label.substr(0, std::min(label.size(), item.app.size())))
                             | color(appColor(item.app)),
                         text(rest)});
        } else {
            name = text(label) | color(Color::Magenta);
        }
        if (ticked) {
            name = name | bold;
        }
        Element line = hbox({text(ticked ? " ◉ " : " ○ ") | color(pick(ticked, Color::Cyan, Color::GrayDark)),
                             name, filler(), dot, text(" ")});
        if (i == listCursor_) {
            line = line | inverted;
        }
        lines.push_back(line | reflect(listBoxes_[k++]));
    }
    if (overflow) {
        const int below = count - listScroll_ - visible;
        lines.push_back(text(below > 0 ? "  ▼ " + std::to_string(below) + " more" : "") | dim
                        | reflect(listBoxes_[k++]));
    }
    if (items_.empty()) {
        lines.push_back(text(" no packets defined") | dim);
    }
    const std::string counts = std::to_string(tlm_.view.shown.size()) + "/" + std::to_string(count);
    return window(paneTitle("PACKETS", counts), vbox(std::move(lines)) | yframe | flex)
         | size(WIDTH, EQUAL, kListWidth);
}

// The title of a card: badge, name, MID, then seq / count / age and warnings.
Element Tui::cardTitle(Element badge, const std::string& name, std::uint16_t mid, const std::string& facts,
                  double age, std::size_t gaps, std::size_t lengthErrors)
{
    Elements parts{badge, text(" " + name + " ") | bold | color(Color::Default),
                   text(toHexString(mid, 4) + " ") | dim, filler()};
    if (gaps != 0) {
        parts.push_back(text(" ⚠ " + std::to_string(gaps) + " gap" + (gaps > 1 ? "s" : "")) | color(Color::Yellow));
    }
    if (lengthErrors != 0) {
        parts.push_back(text(" ⚠ len") | color(Color::Yellow));
    }
    parts.push_back(text(" " + facts + " · ") | dim);
    parts.push_back(text(ageText(age) + " ") | color(ageColor(age)));
    return hbox(std::move(parts));
}

// A decoded packet: "name ······· value" rows, in as many columns as needed
// to fit `maxRows` (within `maxWidth`). Columns are as wide as the longest
// name and value. Border and badge in the app's colour, grey when stale.
Element Tui::packetCard(const PacketState& p, int maxRows, int maxWidth, int& width, int& height)
{
    const TlmTime now = displayNow();
    const double age = secondsBetween(p.lastRx, now);
    const bool stale = age >= kStaleSeconds;
    const Color accent = stale ? Color::GrayDark : appColor(p.def->app);

    std::vector<const TlmFieldDef*> fields;
    for (const auto& f : p.def->fields) {
        if (f.visible()) {
            fields.push_back(&f);
        }
    }

    std::size_t nameWidth = 4;
    std::size_t valueWidth = 4;
    for (const auto& v : p.values) {
        nameWidth = std::max(nameWidth, v.name.size());
        valueWidth = std::max(valueWidth, v.text.size());
    }
    // " name ··· value ▲ ": 1 + name + 1 + leader(>=3) + 1 + value + 3
    const auto fixed = valueWidth + 9;
    const auto widest = static_cast<std::size_t>(std::max(24, maxWidth - 2));
    if (nameWidth + fixed > widest) {
        nameWidth = widest > fixed + 4 ? widest - fixed : 4;
    }
    const int columnWidth = static_cast<int>(nameWidth + fixed);

    Elements rows;
    for (std::size_t i = 0; i < p.values.size(); ++i) {
        const DecodedValue& v = p.values[i];
        const TlmFieldDef* f = i < fields.size() ? fields[i] : nullptr;
        const bool recent = p.changedAt[i] != TlmTime{} && secondsBetween(p.changedAt[i], now) < 1.0;
        const char* arrow = p.trend[i] > 0 ? "▲" : p.trend[i] < 0 ? "▼" : "•";

        const bool cut = v.name.size() > nameWidth;
        const std::string name = cut ? v.name.substr(0, nameWidth - 1) + "…" : v.name;
        const int nameColumns = cut ? static_cast<int>(nameWidth) : static_cast<int>(name.size());
        // Units dimmed: "2048 B" -> "2048" + " B"
        std::string number = v.text;
        std::string units;
        if (f != nullptr && !f->units.empty() && number.size() > f->units.size() + 1
            && number.compare(number.size() - f->units.size(), f->units.size(), f->units) == 0) {
            units = number.substr(number.size() - f->units.size() - 1);
            number.resize(number.size() - units.size());
        }
        Element value = text(number);
        if (v.missing) {
            value = text("—") | dim;
        } else if (recent) {
            value = value | bold | color(Color::Yellow);
        } else if (f != nullptr && !f->enumValues.empty()) {
            if (const auto c = enumColor(v.text)) {
                value = value | color(*c);
            }
        }
        const int leader = columnWidth - nameColumns - static_cast<int>(v.text.size()) - 6;
        rows.push_back(hbox({text(" " + name + " ") | dim, text(repeat("·", std::max(1, leader))) | color(Color::GrayDark),
                             text(" "), value, text(units) | dim,
                             text(std::string(" ") + (recent ? arrow : " ") + " ") | color(Color::Yellow)})
                       | size(WIDTH, EQUAL, columnWidth));
    }
    if (rows.empty()) {
        rows.push_back(text(" (no fields)") | dim | size(WIDTH, EQUAL, columnWidth));
    }

    const int n = static_cast<int>(rows.size());
    const int fitRows = std::max(3, maxRows - 2);
    const int maxColumns = std::max(1, (maxWidth - 2 + 1) / (columnWidth + 1));
    const int columns = std::clamp((n + fitRows - 1) / fitRows, 1, maxColumns);
    const int perColumn = (n + columns - 1) / columns;
    Elements cols;
    for (int c = 0; c < columns; ++c) {
        Elements col;
        for (int r = c * perColumn; r < std::min(n, (c + 1) * perColumn); ++r) {
            col.push_back(rows[static_cast<std::size_t>(r)]);
        }
        if (c > 0) {
            cols.push_back(separatorLight() | color(Color::GrayDark));
        }
        cols.push_back(vbox(std::move(col)));
    }
    const std::string facts = "seq " + std::to_string(p.sequence) + " · ×" + std::to_string(p.count);
    Element badge = text(" " + upper(p.def->app) + " ") | bold | inverted;
    Element title = cardTitle(badge, p.def->name, p.def->mid, facts, age, p.gaps, p.lengthErrors);
    width = std::max(columns * columnWidth + (columns - 1) + 2, 48);
    height = perColumn + 2;
    return window(title, hbox(std::move(cols)) | color(Color::Default)) | color(accent)
         | size(WIDTH, EQUAL, width);
}

// A MID without a definition: the last packet as hex, header bytes (0-11)
// in cyan, with an ASCII column.
Element Tui::hexCard(const UnknownMid& u, int& width, int& height)
{
    const double age = secondsBetween(u.lastRx, displayNow());
    const Bytes& b = u.lastPacket;
    Elements lines;
    for (std::size_t row = 0; row < b.size(); row += 16) {
        Elements cells{text(" " + toHexString(row, 4).substr(2) + "  ") | color(Color::GrayDark)};
        std::string ascii;
        for (std::size_t i = row; i < row + 16; ++i) {
            if (i >= b.size()) {
                cells.push_back(text("   "));
                continue;
            }
            Element byte = text(toHexString(b[i], 2).substr(2) + " ");
            cells.push_back(i < kTlmHeaderSize ? byte | color(Color::Cyan) | dim : byte);
            ascii += (b[i] >= 0x20 && b[i] < 0x7F) ? static_cast<char>(b[i]) : '.';
        }
        cells.push_back(text(" " + ascii + " ") | dim);
        lines.push_back(hbox(std::move(cells)));
    }
    lines.push_back(text(" no definition · cyan: CCSDS header (12 bytes)") | dim);
    width = 6 + 16 * 3 + 18 + 2;
    height = static_cast<int>(lines.size()) + 2;
    const std::string facts = "seq " + std::to_string(u.sequence) + " · " + std::to_string(u.lastLength)
                            + " B · ×" + std::to_string(u.count);
    Element badge = text(" ? ") | bold | inverted;
    return window(cardTitle(badge, "unknown", u.mid, facts, age, u.gaps, 0),
                  vbox(std::move(lines)) | color(Color::Default))
         | color(Color::Magenta) | size(WIDTH, EQUAL, width);
}

Element Tui::waitingCard(const ChecklistItem& item, int& width, int& height)
{
    width = std::max(40, static_cast<int>(item.label.size()) + 24);
    height = 3;
    return window(text(" " + item.label + " " + toHexString(item.mid, 4) + " "),
                  text(" waiting for the first packet…") | dim)
         | color(Color::GrayDark) | size(WIDTH, EQUAL, width);
}

Element Tui::emptyState()
{
    Elements lines{text("No packets selected") | bold | hcenter, text(""),
                   text("↑↓ choose a packet, Space shows it") | hcenter,
                   text("or type  tlm show all") | dim | hcenter};
    if (snapshot_.counters.received == 0) {
        lines.push_back(text(""));
        lines.push_back(text("No telemetry yet. Start it with") | color(Color::Yellow) | hcenter);
        lines.push_back(text("fsw to enable_output 127.0.0.1 5011") | color(Color::Yellow) | hcenter);
    }
    return hbox({text("   "), vbox(std::move(lines)), text("   ")}) | borderStyled(ROUNDED, Color::GrayDark)
         | center;
}

Element Tui::displayArea(int rows, int areaWidth)
{
    struct Placed {
        Element element;
        int width;
        int height;
    };
    std::vector<Placed> cards;
    for (const auto& item : items_) {
        if (tlm_.view.shown.count(item.mid) == 0) {
            continue;
        }
        Placed p{nullptr, 0, 0};
        if (item.packet != nullptr) {
            p.element = packetCard(*item.packet, rows, areaWidth, p.width, p.height);
        } else if (item.unknown != nullptr) {
            p.element = hexCard(*item.unknown, p.width, p.height);
        } else {
            p.element = waitingCard(item, p.width, p.height);
        }
        cards.push_back(std::move(p));
    }
    if (cards.empty()) {
        displayScroll_ = 0;
        return emptyState();
    }

    // Flow layout: left to right, wrapping when the next card does not fit.
    Elements lines;
    Elements line;
    int lineWidth = 0;
    int lineHeight = 0;
    int total = 0;
    for (auto& c : cards) {
        if (!line.empty() && lineWidth + c.width > areaWidth) {
            lines.push_back(hbox(std::move(line)));
            line = Elements{};
            total += lineHeight;
            lineWidth = 0;
            lineHeight = 0;
        }
        line.push_back(c.element);
        lineWidth += c.width;
        lineHeight = std::max(lineHeight, c.height);
    }
    lines.push_back(hbox(std::move(line)));
    total += lineHeight;

    displayScroll_ = std::clamp(displayScroll_, 0, std::max(0, total - rows));
    const int below = total - rows - displayScroll_;
    Element area = vbox(std::move(lines)) | focusPosition(0, displayScroll_ + (rows - 1) / 2) | yframe;
    if (displayScroll_ > 0 || below > 0) {
        area = vbox({area | flex,
                     hbox({filler(), text(" ▲ " + std::to_string(displayScroll_) + "  ▼ "
                                          + std::to_string(std::max(0, below)) + " lines · PgUp/PgDn ")
                                         | inverted | dim})});
    }
    return hbox({area | flex, scrollBar(Bar::Cards, total, rows, displayScroll_)});
}

} // namespace mcs::ui::tui
