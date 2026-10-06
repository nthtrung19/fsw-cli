#include "log/packet_log.hpp"

#include "core/hex.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace mcs {

namespace {

std::tm localNow(std::chrono::system_clock::time_point now)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    return tm;
}

std::string escapeQuotes(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

PacketLog::PacketLog(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path PacketLog::currentFile() const
{
    const std::tm tm = localNow(std::chrono::system_clock::now());
    char name[32];
    std::strftime(name, sizeof name, "mcs-%Y%m%d.log", &tm);
    return directory_ / name;
}

std::string PacketLog::timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::tm tm = localNow(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    char base[32];
    std::strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    char zone[8];
    std::strftime(zone, sizeof zone, "%z", &tm);   // "+0700"
    const std::string z(zone);

    char millis[8];
    std::snprintf(millis, sizeof millis, ".%03d", static_cast<int>(ms));
    return std::string(base) + millis + z.substr(0, 3) + ":" + z.substr(3);
}

std::optional<std::string> PacketLog::record(const LogEntry& entry)
{
    const auto file = currentFile();
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    std::ofstream out(file, std::ios::app);
    if (out) {
        out << timestamp() << " target=" << entry.target << " cmd=\"" << escapeQuotes(entry.commandLine)
            << "\" " << entry.note << " bytes=" << entry.wire.size()
            << (entry.dryRun ? " dryrun=true" : "") << " wire=" << toHex(entry.wire) << '\n';
        out.flush();
    }
    if (out) {
        return std::nullopt;
    }
    if (warned_) {
        return std::nullopt;
    }
    warned_ = true;
    return "cannot write packet log " + file.string() + " (logging continues to be attempted)";
}

} // namespace mcs
