// tlm: telemetry and console display settings.

#include "ui/menu_parts.hpp"

#include "core/text.hpp"

#include <cli/cli.h>

#include <optional>

namespace mcs::ui::menus {

namespace {

// Completes argument 0 from a fixed list of words.
FswCommand::ArgCompleter wordsCompleter(std::vector<std::pair<std::string, std::string>> words)
{
    return [words = std::move(words)](std::size_t index, const std::string& partial) {
        ArgCompletion c;
        if (index > 0) {
            c.hint = "(no more arguments: press Enter)";
            return c;
        }
        for (const auto& w : words) {
            if (w.first.rfind(toLower(partial), 0) == 0) {
                c.values.push_back(w);
            }
        }
        return c;
    };
}

// Packets that can be ticked: "all", then every non-event packet of the catalog.
std::vector<std::pair<std::string, std::string>> tlmPacketWords(const TlmStore& store, const char* all)
{
    std::vector<std::pair<std::string, std::string>> words{{"all", all}};
    for (const auto& p : store.catalog().packets()) {
        if (!p.isEvent) {
            words.emplace_back(p.fullName(), toHexString(p.mid, 4) + (p.help.empty() ? "" : "  " + p.help));
        }
    }
    return words;
}

// "ds.hk" (a catalog packet) or "0x08B8" (any MID, also unknown ones).
std::optional<std::uint16_t> resolvePacket(const TlmStore& store, const std::string& arg)
{
    if (arg.rfind("0x", 0) == 0) {
        const auto mid = parseUnsigned(arg);
        if (mid && *mid <= 0xFFFF) {
            return static_cast<std::uint16_t>(*mid);
        }
        return std::nullopt;
    }
    for (const auto& p : store.catalog().packets()) {
        if (!p.isEvent && p.fullName() == arg) {
            return p.mid;
        }
    }
    return std::nullopt;
}

void printTlmStatus(const TlmContext& tlm, std::ostream& out)
{
    const TlmCounters c = tlm.store->counters();
    if (tlm.service != nullptr) {
        out << "receiver:  " << tlm.service->describe() << '\n';
        if (const auto error = tlm.service->lastError(); !error.empty()) {
            out << "error:     " << error << '\n';
        }
    } else {
        out << "receiver:  not open: " << tlm.receiverError << '\n';
    }
    out << "catalog:   " << tlm.store->catalog().packets().size() << " packet definition(s)\n"
        << "received:  " << c.received << " (unknown " << c.unknown << ", malformed " << c.malformed
        << ", seq gaps " << c.gaps << ", length errors " << c.lengthErrors << ")\n"
        << "screen:    " << toString(tlm.view.view) << (tlm.view.paused ? " (paused)" : "")
        << ", " << tlm.view.shown.size() << " packet(s) shown on the telemetry screen, F1 shows "
        << toString(tlm.view.pane) << '\n';
    if (tlm.consoleService != nullptr) {
        const ConsoleSnapshot s = tlm.console->snapshot();
        out << "console:   " << tlm.consoleService->describe() << ", " << s.lines.size() << " line(s), "
            << s.datagrams << " datagram(s)\n";
        if (const auto error = tlm.consoleService->lastError(); !error.empty()) {
            out << "error:     " << error << '\n';
        }
    } else if (tlm.console != nullptr) {
        out << "console:   not open: " << tlm.consoleError << '\n';
    }
}

} // namespace

std::unique_ptr<cli::Menu> buildTlmMenu(TlmContext& tlm, std::size_t& sessionErrors)
{
    auto menu = std::make_unique<cli::Menu>("tlm", "Telemetry display (F1 console, F2 telemetry)");
    const auto usage = [&sessionErrors](std::ostream& out, const std::string& text) {
        ++sessionErrors;
        out << "error: usage: tlm " << text << '\n';
    };

    menu->Insert(std::make_unique<FswCommand>(
        "view", "Switch screen: console (F1) or telemetry (F2)",
        std::vector<std::string>{"console|telemetry"},
        [&tlm, usage](std::ostream& out, const std::vector<std::string>& args) {
            const std::string v = args.size() == 1 ? toLower(args[0]) : "";
            if (v == "console") {
                tlm.view.view = TlmView::Console;
            } else if (v == "telemetry" || v == "tlm") {
                tlm.view.view = TlmView::Telemetry;
            } else {
                return usage(out, "view console|telemetry");
            }
            out << "screen: " << toString(tlm.view.view) << '\n';
        },
        wordsCompleter({{"console", "commands and events (F1)"},
                        {"telemetry", "packet checklist and cards (F2)"}})));

    menu->Insert(std::make_unique<FswCommand>(
        "pane", "Top pane of F1: EVS events or FSW console text (same as F3)",
        std::vector<std::string>{"events|console"},
        [&tlm, usage](std::ostream& out, const std::vector<std::string>& args) {
            const std::string v = args.size() == 1 ? toLower(args[0]) : "";
            if (v == "events") {
                tlm.view.pane = F1Pane::Events;
            } else if (v == "console") {
                tlm.view.pane = F1Pane::Console;
            } else {
                return usage(out, "pane events|console");
            }
            out << "F1 pane: " << toString(tlm.view.pane) << '\n';
        },
        wordsCompleter({{"events", "EVS events received through TO"},
                        {"console", "FSW console text (\"console\" block in targets.json)"}})));

    // show/hide: the same as ticking/unticking in the telemetry screen's checklist (F2).
    for (const bool show : {true, false}) {
        const std::string name = show ? "show" : "hide";
        menu->Insert(std::make_unique<FswCommand>(
            name,
            show ? "Show a packet on the telemetry screen (tick it in the F2 checklist)"
                 : "Remove a packet from the telemetry screen (untick it)",
            std::vector<std::string>{"packet|mid|all"},
            [&tlm, usage, show, name](std::ostream& out, const std::vector<std::string>& args) {
                if (args.size() != 1) {
                    return usage(out, name + " <packet>|<mid>|all  (e.g. ds.hk or 0x08B8)");
                }
                const std::string arg = toLower(args[0]);
                if (arg == "all") {
                    tlm.view.shown.clear();
                    if (show) {
                        for (const auto& p : tlm.store->catalog().packets()) {
                            if (!p.isEvent) {
                                tlm.view.shown.insert(p.mid);
                            }
                        }
                        for (const auto& u : tlm.store->snapshot().unknown) {
                            tlm.view.shown.insert(u.mid);
                        }
                    }
                } else if (const auto mid = resolvePacket(*tlm.store, arg)) {
                    if (show) {
                        tlm.view.shown.insert(*mid);
                    } else {
                        tlm.view.shown.erase(*mid);
                    }
                } else {
                    return usage(out, name + " <packet>|<mid>|all  ('" + arg
                                          + "' is not a telemetry packet; Tab lists them)");
                }
                out << "telemetry screen (F2): " << tlm.view.shown.size() << " packet(s) shown\n";
            },
            wordsCompleter(tlmPacketWords(*tlm.store, show ? "every packet in the checklist"
                                                           : "clear the telemetry screen"))));
    }

    menu->Insert(std::make_unique<FswCommand>(
        "pause", "Freeze or resume the telemetry and event display (F5); receiving continues",
        std::vector<std::string>{"on|off"},
        [&tlm, usage](std::ostream& out, const std::vector<std::string>& args) {
            if (args.empty()) {
                tlm.view.paused = !tlm.view.paused;
            } else if (args.size() == 1 && (iequals(args[0], "on") || iequals(args[0], "off"))) {
                tlm.view.paused = iequals(args[0], "on");
            } else {
                return usage(out, "pause [on|off]");
            }
            out << "display " << (tlm.view.paused ? "paused" : "live") << '\n';
        },
        wordsCompleter({{"on", "freeze the display"}, {"off", "back to live"}})));

    menu->Insert(std::make_unique<FswCommand>(
        "clear", "Forget telemetry values, events and counters",
        std::vector<std::string>{},
        [&tlm](std::ostream& out, const std::vector<std::string>&) {
            tlm.store->clear();
            out << "telemetry cleared\n";
        }));

    menu->Insert(std::make_unique<FswCommand>(
        "status", "Show the telemetry receiver and counters", std::vector<std::string>{},
        [&tlm](std::ostream& out, const std::vector<std::string>&) { printTlmStatus(tlm, out); }));

    return menu;
}

} // namespace mcs::ui::menus
