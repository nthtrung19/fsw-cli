#include "ui/menu_builder.hpp"

#include "core/text.hpp"
#include "ui/fsw_command.hpp"
#include "ui/help_format.hpp"
#include "ui/script_runner.hpp"
#include "ui/transfer_format.hpp"

#include <cli/cli.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mcs::ui {

namespace {

// `checkWith`: check-only session (the commands are checked, not sent).
std::unique_ptr<cli::Menu> buildFswMenu(CommandService& service, const VerifyService* checkWith)
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
                [&service, checkWith, appName = app.name, cmdName = cmd.name](
                    std::ostream& out, const std::vector<std::string>& args) {
                    if (checkWith != nullptr) {
                        service.check(appName, cmdName, args, checkWith->valuesKnown(), out);
                    } else {
                        service.execute(appName, cmdName, args, out);
                    }
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

void insertSessionCommands(cli::Menu& root, CommandService& service, std::size_t& sessionErrors,
                           const VerifyService* checkWith)
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
        [&service, checkWith](std::ostream& out, const std::vector<std::string>& args) {
            if (checkWith != nullptr) {
                service.checkRaw(args, checkWith->valuesKnown(), out);
            } else {
                service.executeRaw(args, out);
            }
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


// ---- file transfer ------------------------------------------------------------------

std::optional<int> parseClass(const std::string& word)
{
    if (iequals(word, "class1") || word == "1") {
        return 1;
    }
    if (iequals(word, "class2") || word == "2") {
        return 2;
    }
    return std::nullopt;
}

void printNotices(FileTransferService& files, std::ostream& out)
{
    for (const auto& line : files.takeNotices()) {
        out << line << '\n';
    }
}

// Batch mode: one line per 10 % of progress, after any pending notice.
FileTransferService::Progress progressPrinter(FileTransferService& files, std::ostream& out)
{
    auto lastStep = std::make_shared<int>(-1);
    return [&files, &out, lastStep](const cfdp::TransferStatus& s) {
        printNotices(files, out);
        if (s.state == cfdp::State::Waiting || !s.sizeKnown || s.fileSize == 0) {
            return;
        }
        const int step = static_cast<int>(s.percent()) / 10;
        if (step <= *lastStep) {
            return;
        }
        *lastStep = step;
        const double secs = std::chrono::duration<double>(cfdp::TransferClock::now() - s.start).count();
        char line[96];
        std::snprintf(line, sizeof line, "  %3d%%  %10s  %s", step * 10, cfdp::formatBytes(s.bytesDone).c_str(),
                      cfdp::formatSeconds(secs).c_str());
        out << line << '\n';
    };
}

FswCommand::ArgCompleter transferIdCompleter(FileTransferService& files, bool activeOnly, bool withAll)
{
    return [&files, activeOnly, withAll](std::size_t index, const std::string& partial) {
        ArgCompletion c;
        if (index > 0) {
            c.hint = "(no more arguments: press Enter)";
            return c;
        }
        if (withAll && std::string("all").rfind(toLower(partial), 0) == 0) {
            c.values.emplace_back("all", "every active transfer");
        }
        for (const auto& s : files.transfers()) {
            if (s.state == cfdp::State::Waiting || (activeOnly && cfdp::isFinished(s.state))) {
                continue;
            }
            const std::string id = s.id.toString();
            if (id.rfind(partial, 0) == 0) {
                c.values.emplace_back(id, std::string(toString(s.state)) + "  " + transferFiles(s));
            }
        }
        if (c.values.empty()) {
            c.hint = activeOnly ? "<transfer id, e.g. 0.24_7>   (no active transfer: 'file list')"
                                : "<transfer id, e.g. 0.24_7>   ('file list' shows them)";
        }
        return c;
    };
}

std::unique_ptr<cli::Menu> buildFileMenu(FileTransferService& files, std::size_t& sessionErrors, FileMenuMode mode)
{
    auto menu = std::make_unique<cli::Menu>("file", "File transfer with CF (CFDP): put, get, list");
    const auto failed = [&sessionErrors](std::ostream& out, const std::string& text) {
        ++sessionErrors;
        out << "error: " << text << '\n';
    };
    const bool batch = mode == FileMenuMode::Batch;
    const bool plain = mode == FileMenuMode::Plain;
    const bool check = mode == FileMenuMode::Check;


    menu->Insert(std::make_unique<FswCommand>(
        "put", "Send a ground file to the FSW, e.g. file put test.bin /cf/test.bin class2",
        std::vector<std::string>{"local", "remote", "class1|class2"},
        [&files, failed, batch, plain, check, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            if (plain) {
                printNotices(files, out);
            }
            if (args.size() < 2 || args.size() > 3) {
                return failed(out, "usage: file put <local file> <remote path> [class1|class2]");
            }
            int cls = 1;
            if (args.size() == 3) {
                const auto c = parseClass(args[2]);
                if (!c) {
                    return failed(out, "file put: '" + args[2] + "' is not class1 or class2");
                }
                cls = *c;
            }
            if (check) {
                return;
            }
            std::string error;
            const auto id = files.put(args[0], args[1], cls, error);
            if (!id) {
                return failed(out, "file put: " + error);
            }
            if (!batch) {
                if (plain) {
                    printNotices(files, out);
                }
                return;
            }
            printNotices(files, out);
            const auto r = files.waitFor(*id, progressPrinter(files, out));
            printNotices(files, out);
            if (!r.error.empty()) {
                return failed(out, "file put " + id->toString() + ": " + r.error);
            }
            if (!r.status || r.status->state != cfdp::State::Done) {
                ++sessionErrors;
            }
        },
        [](std::size_t index, const std::string& partial) {
            ArgCompletion c;
            if (index == 0) {
                c.hint = "<local file>   the ground file to send";
            } else if (index == 1) {
                c.hint = "<remote path>   where CF stores it, e.g. /cf/test.bin (max 63 characters)";
            } else if (index == 2) {
                for (const char* w : {"class1", "class2"}) {
                    if (std::string(w).rfind(toLower(partial), 0) == 0) {
                        c.values.emplace_back(w, std::string(w) == "class1" ? "unacknowledged (default)"
                                                                            : "reliable: CF acknowledges, lost data is resent");
                    }
                }
            } else {
                c.hint = "(no more arguments: press Enter)";
            }
            return c;
        }));

    menu->Insert(std::make_unique<FswCommand>(
        "get", "Ask CF to send a file and receive it, e.g. file get /cf/log/hk.dat",
        std::vector<std::string>{"remote", "local", "class1|class2"},
        [&files, failed, batch, plain, check, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            if (plain) {
                printNotices(files, out);
            }
            if (args.empty() || args.size() > 3) {
                return failed(out, "usage: file get <remote path> [local path] [class1|class2]");
            }
            int cls = 1;
            std::optional<std::filesystem::path> local;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (const auto c = parseClass(args[i])) {
                    cls = *c;
                } else if (!local) {
                    local = args[i];
                } else {
                    return failed(out, "usage: file get <remote path> [local path] [class1|class2]");
                }
            }
            if (check) {
                return;
            }
            const auto ticket = files.get(args[0], local, cls, out);
            if (!ticket) {
                ++sessionErrors;
                return;
            }
            if (!batch) {
                if (plain) {
                    printNotices(files, out);
                }
                return;
            }
            const auto r = files.waitForGet(*ticket, progressPrinter(files, out));
            printNotices(files, out);
            if (!r.error.empty()) {
                return failed(out, "file get " + args[0] + ": " + r.error);
            }
            if (!r.status || r.status->state != cfdp::State::Done) {
                ++sessionErrors;
            }
        },
        [](std::size_t index, const std::string& partial) {
            ArgCompletion c;
            if (index == 0) {
                c.hint = "<remote path>   the file on the FSW, e.g. /cf/log/hk_0001.dat";
            } else if (index <= 2) {
                for (const char* w : {"class1", "class2"}) {
                    if (std::string(w).rfind(toLower(partial), 0) == 0) {
                        c.values.emplace_back(w, std::string(w) == "class1" ? "unacknowledged (default)"
                                                                            : "reliable: lost data is asked again");
                    }
                }
                if (c.values.empty()) {
                    c.hint = index == 1 ? "[local path]   default: the download directory, or class1|class2"
                                        : "[class1|class2]";
                }
            } else {
                c.hint = "(no more arguments: press Enter)";
            }
            return c;
        }));

    menu->Insert(std::make_unique<FswCommand>(
        "list", "List active and recent transfers", std::vector<std::string>{},
        [&files, check](std::ostream& out, const std::vector<std::string>&) {
            if (check) {
                return;
            }
            printNotices(files, out);
            for (const auto& line : listLines(files.transfers())) {
                out << line << '\n';
            }
        }));

    menu->Insert(std::make_unique<FswCommand>(
        "status", "Details of one transfer", std::vector<std::string>{"id"},
        [&files, failed, check](std::ostream& out, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                return failed(out, "usage: file status <id>   (e.g. 0.24_7; 'file list' shows them)");
            }
            if (check) {
                return;
            }
            const auto id = cfdp::parseTransactionId(args[0]);
            for (const auto& s : files.transfers()) {
                if (id && s.id == *id) {
                    for (const auto& line : statusLines(s)) {
                        out << line << '\n';
                    }
                    return;
                }
            }
            failed(out, "file status: no transfer '" + args[0] + "' ('file list' shows them)");
        },
        transferIdCompleter(files, false, false)));

    struct Control {
        const char* name;
        cfdp::RequestKind kind;
        const char* help;
        bool all;
    };
    for (const Control& c : {Control{"cancel", cfdp::RequestKind::Cancel, "Cancel a transfer (the partner is told)", true},
                             Control{"abandon", cfdp::RequestKind::Abandon,
                                     "Stop a transfer here without telling the partner", true},
                             Control{"suspend", cfdp::RequestKind::Suspend, "Pause a transfer", false},
                             Control{"resume", cfdp::RequestKind::Resume, "Continue a suspended transfer", false}}) {
        menu->Insert(std::make_unique<FswCommand>(
            c.name, c.help, std::vector<std::string>{c.all ? "id|all" : "id"},
            [&files, failed, check, c](std::ostream& out, const std::vector<std::string>& args) {
                if (args.size() != 1) {
                    return failed(out, std::string("usage: file ") + c.name + (c.all ? " <id>|all" : " <id>"));
                }
                if (!c.all && iequals(args[0], "all")) {
                    return failed(out, std::string("file ") + c.name + ": give one transfer id");
                }
                if (check) {
                    return;
                }
                std::string error;
                if (!files.control(c.kind, args[0], error)) {
                    return failed(out, std::string("file ") + c.name + ": " + error);
                }
                out << c.name << " requested for " << args[0] << '\n';
            },
            transferIdCompleter(files, true, c.all)));
    }
    return menu;
}

// ---- script checks ----------------------------------------------------------------------

// Batch sessions only: the checks block while they wait.
void insertVerifyCommands(cli::Menu& root, VerifyService& verify)
{
    using Method = bool (VerifyService::*)(const VerifyService::Args&, std::ostream&);
    struct Entry {
        const char* name;
        Method method;
        const char* help;
        std::vector<std::string> params;
    };
    const Entry entries[] = {
        {"wait", &VerifyService::wait, "Pause the script, e.g. wait 2 (seconds; 500ms)", {"seconds"}},
        {"timeout", &VerifyService::setTimeout, "Time limit of the following checks (5 s at the start)",
         {"seconds"}},
        {"expect", &VerifyService::expect,
         "Check telemetry, e.g. expect ds.hk cmd_accepted_counter == $n+1 within 5",
         {"packet", "field op value", "within seconds"}},
        {"expect-event", &VerifyService::expectEvent, "Check for an event, e.g. expect-event DS id 1 type INFO",
         {"APP", "id n", "type T", "text \"...\"", "within seconds"}},
        {"expect-no-event", &VerifyService::expectNoEvent,
         "Fail if an event arrives, e.g. expect-no-event DS type ERROR for 2",
         {"APP", "id n", "type T", "text \"...\"", "for seconds"}},
        {"save", &VerifyService::save, "Save a telemetry value for $name, e.g. save n ds.hk cmd_accepted_counter",
         {"name", "packet", "field", "within seconds"}},
        {"test", &VerifyService::beginTest, "Start a test, e.g. test DS-001 \"NOOP works\" req DS-R1",
         {"ID", "title", "req R1,R2"}},
        {"end", &VerifyService::endTest, "End the current test", {}},
    };
    for (const Entry& e : entries) {
        root.Insert(std::make_unique<FswCommand>(
            e.name, e.help, e.params,
            [&verify, method = e.method](std::ostream& out, const std::vector<std::string>& args) {
                (verify.*method)(args, out);
            }));
    }
}

// ---- run <script> (full-screen session) --------------------------------------------------

// "stop", then directories and *.mcs files that match what was typed of a path.
ArgCompletion scriptCompletion(const std::string& partial)
{
    namespace fs = std::filesystem;
    ArgCompletion c;
    if (std::string("stop").rfind(toLower(partial), 0) == 0) {
        c.values.emplace_back("stop", "stop the running script");
    }
    const auto slash = partial.find_last_of('/');
    const std::string dirText = slash == std::string::npos ? "" : partial.substr(0, slash + 1);
    const std::string prefix = slash == std::string::npos ? partial : partial.substr(slash + 1);
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> found;
    for (const auto& entry : fs::directory_iterator(dirText.empty() ? "." : dirText, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || (name.front() == '.' && prefix.empty())) {
            continue;
        }
        if (entry.is_directory(ec)) {
            found.emplace_back(dirText + name + "/", "directory");
        } else if (entry.path().extension() == ".mcs") {
            found.emplace_back(dirText + name, "test script");
        }
    }
    std::sort(found.begin(), found.end());
    c.values.insert(c.values.end(), found.begin(), found.end());
    if (c.values.empty()) {
        c.hint = "<script file>   a .mcs test script, e.g. scripts/ds_smoke.mcs, or 'stop'";
    }
    return c;
}

std::unique_ptr<FswCommand> buildRunCommand(ScriptRunner& runner, std::size_t& sessionErrors)
{
    return std::make_unique<FswCommand>(
        "run", "Run a test script in the background, e.g. run scripts/ds_smoke.mcs ('run stop' ends it)",
        std::vector<std::string>{"script|stop"},
        [&runner, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            const ScriptRunner::Status s = runner.status();
            if (args.empty()) {
                if (s.running) {
                    out << s.name << ": line " << s.line << " of " << s.lines << " ('run stop' ends it)\n";
                } else if (s.result) {
                    out << s.name << ": finished, " << (*s.result == 0 ? "PASS" : "FAIL") << '\n';
                } else {
                    out << "no script has run ('run <script>' starts one)\n";
                }
            } else if (args.size() == 1 && iequals(args[0], "stop")) {
                if (!s.running) {
                    ++sessionErrors;
                    out << "error: run stop: no script is running\n";
                    return;
                }
                runner.stop();
                out << "stopping " << s.name << '\n';
            } else if (args.size() == 1) {
                if (!runner.start(args[0], out)) {
                    ++sessionErrors;
                }
            } else {
                ++sessionErrors;
                out << "error: usage: run <script> | run stop | run\n";
            }
        },
        [](std::size_t index, const std::string& partial) {
            if (index > 0) {
                ArgCompletion c;
                c.hint = "(no more arguments: press Enter)";
                return c;
            }
            return scriptCompletion(partial);
        });
}

} // namespace

int terminalWidth()
{
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 100;
}

const char* toString(TlmView view)
{
    switch (view) {
    case TlmView::Console: return "console";
    case TlmView::Telemetry: return "telemetry";
    }
    return "?";
}

const char* toString(F1Pane pane)
{
    switch (pane) {
    case F1Pane::Events: return "events";
    case F1Pane::Console: return "console";
    }
    return "?";
}

std::unique_ptr<cli::Menu> buildRootMenu(CommandService& service, std::size_t& sessionErrors,
                                         TlmContext* tlm, FileTransferService* files, FileMenuMode fileMode,
                                         VerifyService* verify)
{
    const VerifyService* checkWith = verify != nullptr && verify->checkOnly() ? verify : nullptr;
    auto root = std::make_unique<cli::Menu>("mcs");
    root->Insert(buildFswMenu(service, checkWith));
    insertSessionCommands(*root, service, sessionErrors, checkWith);
    if (tlm != nullptr && tlm->store != nullptr) {
        root->Insert(buildTlmMenu(*tlm, sessionErrors));
    }
    if (tlm != nullptr && tlm->script != nullptr) {
        root->Insert(buildRunCommand(*tlm->script, sessionErrors));
    }
    if (files != nullptr) {
        root->Insert(buildFileMenu(*files, sessionErrors, checkWith != nullptr ? FileMenuMode::Check : fileMode));
    }
    if (verify != nullptr) {
        insertVerifyCommands(*root, *verify);
    }
    return root;
}

void installHelp(cli::Cli& cli, HelpPrinter print)
{
    cli.HelpHandler([print = std::move(print)](std::ostream& out, const std::vector<std::string>&,
                                               const std::vector<cli::HelpEntry>& entries) {
        // Name and description only: parameters show up in the Tab hints.
        std::vector<HelpItem> items;
        for (const auto& e : entries) {
            items.push_back({e.usage.substr(0, e.usage.find(' ')), e.description});
        }
        print(out, items);
    });
}

HelpPrinter terminalHelp(std::function<int()> width, bool color)
{
    return [width = std::move(width), color](std::ostream& out, const std::vector<HelpItem>& items) {
        for (const auto& line : formatHelp(items, width())) {
            out << (color ? toAnsi(line) : line.text) << '\n';
        }
    };
}

} // namespace mcs::ui
