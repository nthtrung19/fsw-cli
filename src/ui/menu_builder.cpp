#include "ui/menu_builder.hpp"

#include "core/text.hpp"
#include "ui/fsw_command.hpp"

#include <cli/cli.h>

#include <algorithm>

namespace mcs::ui {

namespace {

std::unique_ptr<cli::Menu> buildFswMenu(CommandService& service)
{
    auto fsw = std::make_unique<cli::Menu>("fsw", "Send commands to flight software (target '"
                                                      + service.targetName() + "')");

    for (const auto& app : service.catalog().apps()) {
        auto appMenu = std::make_unique<cli::Menu>(
            app.name, app.help.empty() ? "Commands for " + app.name : app.help);

        const bool severalMids = app.mids().size() > 1;
        for (const auto& cmd : app.commands) {
            std::string help = cmd.help;
            if (severalMids) {
                help += " [mid " + toHexString(cmd.mid, 4) + "]";
            }
            if (cmd.critical) {
                help += " [critical: 'arm' first]";
            }
            appMenu->Insert(std::make_unique<FswCommand>(
                cmd.name, help, cmd.paramDescriptions(),
                [&service, appName = app.name, cmdName = cmd.name](
                    std::ostream& out, const std::vector<std::string>& args) {
                    service.execute(appName, cmdName, args, out);
                },
                // The catalog outlives the menu, so the definition can be held by pointer.
                [def = &cmd](std::size_t index, const std::string& partial) {
                    return completeArgument(*def, index, partial);
                }));
        }
        fsw->Insert(std::move(appMenu));
    }
    return fsw;
}

void insertSessionCommands(cli::Menu& root, CommandService& service, std::size_t& sessionErrors)
{
    root.Insert(std::make_unique<FswCommand>(
        "target", "Show the active target, its protocol stack and catalog", std::vector<std::string>{},
        [&service](std::ostream& out, const std::vector<std::string>&) {
            out << "target:   " << service.targetName() << '\n'
                << "pipeline: " << service.pipeline().describe() << '\n'
                << "catalog:  " << service.catalog().apps().size() << " app(s), "
                << service.catalog().commandCount() << " command(s)\n";
            for (const auto& app : service.catalog().apps()) {
                std::string mids;
                for (const std::uint16_t mid : app.mids()) {
                    mids += (mids.empty() ? "" : ",") + toHexString(mid, 4);
                }
                out << "          " << app.name << "  mid=" << mids << "  "
                    << app.commands.size() << " command(s)\n";
            }
        }));

    root.Insert(std::make_unique<FswCommand>(
        "verbose", "Show or set hexdump of every sent packet", std::vector<std::string>{"on|off"},
        [&service, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            if (args.size() == 1 && (iequals(args[0], "on") || iequals(args[0], "off"))) {
                service.setVerbose(iequals(args[0], "on"));
            } else if (!args.empty()) {
                ++sessionErrors;
                out << "error: usage: verbose [on|off]\n";
                return;
            }
            out << "verbose is " << (service.verbose() ? "on" : "off") << '\n';
        },
        [](std::size_t index, const std::string& partial) {
            ArgCompletion c;
            if (index > 0) {
                c.hint = "(no more arguments: press Enter)";
                return c;
            }
            for (const char* v : {"on", "off"}) {
                if (std::string(v).rfind(toLower(partial), 0) == 0) {
                    c.values.emplace_back(v, std::string("hexdump ") + v);
                }
            }
            if (c.values.empty()) {
                c.hint = "<on|off>   or nothing to show the current setting";
            }
            return c;
        }));

    root.Insert(std::make_unique<FswCommand>(
        "raw", "Send pre-built packet bytes, e.g. raw 19 4B C0 00 00 01 6C 00",
        std::vector<std::string>{"hex bytes..."},
        [&service](std::ostream& out, const std::vector<std::string>& args) {
            service.executeRaw(args, out);
        }));

    root.Insert(std::make_unique<FswCommand>(
        "arm", "Allow the next critical command to be sent", std::vector<std::string>{},
        [&service](std::ostream& out, const std::vector<std::string>&) {
            service.arm();
            out << "armed: the next critical command will be sent\n";
        }));
}

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

std::vector<std::pair<std::string, std::string>> tlmAppWords(const TlmStore& store, const char* what)
{
    std::vector<std::pair<std::string, std::string>> words{{"all", std::string("all ") + what}};
    for (const auto& app : store.catalog().apps()) {
        words.emplace_back(app, std::string("only ") + app);
    }
    return words;
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
        << ", seq gaps " << c.gaps << ")\n"
        << "sent:      " << c.sent << '\n'
        << "view:      " << toString(tlm.view.view) << (tlm.view.paused ? " (paused)" : "")
        << ", " << tlm.view.shown.size() << " packet(s) shown"
        << ", monitor " << tlm.view.monitorFilter.describe() << '\n';
}

std::unique_ptr<cli::Menu> buildTlmMenu(TlmContext& tlm, std::size_t& sessionErrors)
{
    auto menu = std::make_unique<cli::Menu>("tlm", "Telemetry display (F1 dashboard, F2 monitor, F4 stats)");
    const auto usage = [&sessionErrors](std::ostream& out, const std::string& text) {
        ++sessionErrors;
        out << "error: usage: tlm " << text << '\n';
    };

    menu->Insert(std::make_unique<FswCommand>(
        "view", "Switch the telemetry view", std::vector<std::string>{"dash|monitor|stats"},
        [&tlm, usage](std::ostream& out, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                return usage(out, "view dash|monitor|stats");
            }
            const std::string v = toLower(args[0]);
            if (v == "dash" || v == "dashboard") {
                tlm.view.view = TlmView::Dashboard;
            } else if (v == "monitor") {
                tlm.view.view = TlmView::Monitor;
            } else if (v == "stats") {
                tlm.view.view = TlmView::Stats;
            } else {
                return usage(out, "view dash|monitor|stats");
            }
            out << "view: " << toString(tlm.view.view) << '\n';
        },
        wordsCompleter({{"dash", "packet panels and events (F1)"},
                        {"monitor", "every sent/received packet (F2)"},
                        {"stats", "per-MID link statistics (F4)"}})));

    // show/hide: the same as ticking/unticking in the dashboard checklist (F6).
    for (const bool show : {true, false}) {
        const std::string name = show ? "show" : "hide";
        menu->Insert(std::make_unique<FswCommand>(
            name,
            show ? "Show a packet on the dashboard (tick it in the checklist)"
                 : "Remove a packet from the dashboard (untick it)",
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
                tlm.view.view = TlmView::Dashboard;
                out << "dashboard: " << tlm.view.shown.size() << " packet(s) shown\n";
            },
            wordsCompleter(tlmPacketWords(*tlm.store, show ? "every packet in the checklist"
                                                           : "clear the dashboard"))));
    }

    menu->Insert(std::make_unique<FswCommand>(
        "monitor", "Filter the packet monitor by app (sent and received) or MID",
        std::vector<std::string>{"app|mid|all"},
        [&tlm, usage](std::ostream& out, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                return usage(out, "monitor <app>|<mid>|all");
            }
            MonitorFilter f;
            const std::string arg = toLower(args[0]);
            if (arg.rfind("0x", 0) == 0) {
                const auto mid = parseUnsigned(arg);
                if (!mid || *mid > 0xFFFF) {
                    return usage(out, "monitor <app>|<mid>|all  (mid like 0x0808)");
                }
                f.mid = static_cast<std::uint16_t>(*mid);
            } else if (arg != "all") {
                f.app = arg;
            }
            tlm.view.monitorFilter = f;
            tlm.view.view = TlmView::Monitor;
            out << "monitor: " << f.describe() << '\n';
        },
        wordsCompleter(tlmAppWords(*tlm.store, "packets"))));

    menu->Insert(std::make_unique<FswCommand>(
        "pause", "Freeze or resume the telemetry display (F5); receiving continues",
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
        "clear", "Forget telemetry values, events, statistics and the packet monitor",
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

} // namespace

const char* toString(TlmView view)
{
    switch (view) {
    case TlmView::Dashboard: return "dashboard";
    case TlmView::Monitor: return "monitor";
    case TlmView::Stats: return "stats";
    }
    return "?";
}

std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors,
                                         TlmContext* tlm)
{
    auto root = std::make_unique<cli::Menu>("mcs");
    root->Insert(buildFswMenu(service));
    insertSessionCommands(*root, service, sessionErrors);
    if (tlm != nullptr && tlm->store != nullptr) {
        root->Insert(buildTlmMenu(*tlm, sessionErrors));
    }
    return root;
}

} // namespace mcs::ui
