#include "cfdp/transfer.hpp"

#include "core/text.hpp"

#include <cstdio>

namespace mcs::cfdp {

std::string TransactionId::toString() const
{
    return source + "_" + std::to_string(seq);
}

std::optional<TransactionId> parseTransactionId(const std::string& text)
{
    const auto at = text.rfind('_');
    if (at == std::string::npos || at == 0 || at + 1 >= text.size()) {
        return std::nullopt;
    }
    const auto seq = parseUnsigned(std::string_view(text).substr(at + 1));
    if (!seq || *seq > 0xFFFFFFFFu) {
        return std::nullopt;
    }
    const std::string source = text.substr(0, at);
    for (const char c : source) {
        if ((c < '0' || c > '9') && c != '.') {
            return std::nullopt;
        }
    }
    return TransactionId{source, static_cast<std::uint32_t>(*seq)};
}

const char* toString(State state)
{
    switch (state) {
    case State::Waiting: return "WAITING";
    case State::Active: return "ACTIVE";
    case State::Suspended: return "SUSPENDED";
    case State::Done: return "DONE";
    case State::Failed: return "FAILED";
    case State::Cancelled: return "CANCELLED";
    case State::Abandoned: return "ABANDONED";
    }
    return "?";
}

bool isFinished(State state)
{
    return state == State::Done || state == State::Failed || state == State::Cancelled
        || state == State::Abandoned;
}

double TransferStatus::percent() const
{
    if (!sizeKnown || fileSize == 0) {
        return (sizeKnown && state == State::Done) ? 100.0 : 0.0;
    }
    const double p = 100.0 * static_cast<double>(bytesDone) / static_cast<double>(fileSize);
    return p > 100.0 ? 100.0 : p;
}

std::string formatBytes(std::uint64_t bytes)
{
    char text[32];
    if (bytes < 1024) {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(text, sizeof text, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        std::snprintf(text, sizeof text, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(text, sizeof text, "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return text;
}

std::string formatRate(double bytesPerSecond)
{
    char text[32];
    if (bytesPerSecond < 1000) {
        std::snprintf(text, sizeof text, "%.0f B/s", bytesPerSecond);
    } else if (bytesPerSecond < 1e6) {
        std::snprintf(text, sizeof text, "%.1f KB/s", bytesPerSecond / 1e3);
    } else {
        std::snprintf(text, sizeof text, "%.1f MB/s", bytesPerSecond / 1e6);
    }
    return text;
}

std::string formatSeconds(double seconds)
{
    char text[32];
    if (seconds < 60) {
        std::snprintf(text, sizeof text, "%.1f s", seconds < 0 ? 0.0 : seconds);
    } else {
        const auto s = static_cast<long>(seconds + 0.5);
        std::snprintf(text, sizeof text, "%ld min %02ld s", s / 60, s % 60);
    }
    return text;
}

} // namespace mcs::cfdp
 