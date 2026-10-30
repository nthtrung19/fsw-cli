// file: CFDP transfers with CF (put, get, list, status, cancel...).

#include "ui/menu_parts.hpp"

#include "core/text.hpp"
#include "ui/transfer_format.hpp"

#include <cli/cli.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace mcs::ui::menus {

namespace {

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

} // namespace

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

} // namespace mcs::ui::menus
