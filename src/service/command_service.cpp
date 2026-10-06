#include "service/command_service.hpp"

#include "core/errors.hpp"
#include "core/hex.hpp"
#include "core/text.hpp"

namespace mcs {

namespace {

std::string quoteIfNeeded(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"'") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (const char c : arg) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

std::string commandLineOf(const std::string& app, const std::string& command,
                          const std::vector<std::string>& args)
{
    std::string line = "fsw " + app + " " + command;
    for (const auto& a : args) {
        line += " " + quoteIfNeeded(a);
    }
    return line;
}

} // namespace

CommandService::CommandService(const CommandCatalog& catalog, PayloadEncoder encoder,
                               Pipeline& pipeline, PacketLog* log, std::string targetName)
    : catalog_(catalog), encoder_(encoder), pipeline_(pipeline), log_(log),
      targetName_(std::move(targetName))
{
}

bool CommandService::fail(const std::string& message, std::ostream& out)
{
    ++failures_;
    out << "error: " << message << '\n';
    return false;
}

bool CommandService::execute(const std::string& app, const std::string& command,
                             const std::vector<std::string>& args, std::ostream& out)
{
    const std::string line = commandLineOf(app, command, args);
    try {
        const AppDef* appDef = catalog_.findApp(app);
        const CommandDef* def = appDef != nullptr ? appDef->findCommand(command) : nullptr;
        if (def == nullptr) {
            return fail("unknown command '" + app + " " + command + "' for target '" + targetName_ + "'", out);
        }

        CommandMessage message{appDef->mid, def->cc, encoder_.encode(*def, args)};

        if (def->critical && !armed_) {
            return fail("'" + command + "' is a critical command: type 'arm', then repeat it", out);
        }

        const Pipeline::Result result = pipeline_.send(message);
        if (def->critical) {
            armed_ = false;   // arming covers one critical command
        }
        report(line, result, out);
        return true;
    } catch (const mcsError& e) {
        return fail(e.what(), out);
    } catch (const std::exception& e) {
        return fail(std::string("unexpected: ") + e.what(), out);
    }
}

bool CommandService::executeRaw(const std::vector<std::string>& hexTokens, std::ostream& out)
{
    const std::string joined = join(hexTokens, " ");
    try {
        const Bytes packet = parseHex(joined);
        const Pipeline::Result result = pipeline_.sendRaw(packet);
        report("raw " + toHex(packet), result, out);
        return true;
    } catch (const mcsError& e) {
        return fail(std::string("raw: ") + e.what(), out);
    } catch (const std::exception& e) {
        return fail(std::string("raw: unexpected: ") + e.what(), out);
    }
}

void CommandService::report(const std::string& commandLine, const Pipeline::Result& result,
                            std::ostream& out)
{
    ++sent_;
    const bool dryRun = pipeline_.isDryRun();
    const std::string details = "(" + std::to_string(result.wire.size()) + " bytes, " + result.note + ")";

    if (dryRun) {
        out << "[dry-run] " << commandLine << "  " << details << '\n';
    } else {
        out << "sent " << commandLine << " -> " << pipeline_.destination() << "  " << details << '\n';
    }

    if (dryRun || verbose_) {
        if (pipeline_.hasLayers()) {
            out << "  packet: " << toHex(result.packet) << '\n';
            out << "  wire:   " << toHex(result.wire) << '\n';
        } else {
            out << "  " << toHex(result.wire) << '\n';
        }
    }

    if (log_ != nullptr) {
        LogEntry entry{targetName_, commandLine, result.note, result.wire, dryRun};
        if (const auto warning = log_->record(entry)) {
            out << "warning: " << *warning << '\n';
        }
    }
}

} // namespace mcs
