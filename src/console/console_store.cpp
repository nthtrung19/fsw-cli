#include "console/console_store.hpp"

namespace mcs {

namespace {

constexpr const char* kReplacement = "·";   // '·'
constexpr std::size_t kTabWidth = 8;

// Length of the UTF-8 sequence starting at raw[i], 0 if it is not a valid one.
std::size_t utf8Length(const std::string& raw, std::size_t i)
{
    const auto c = static_cast<unsigned char>(raw[i]);
    std::size_t n = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
    } else {
        return 0;
    }
    if (i + n > raw.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

// Where to cut a too-long line: not inside a UTF-8 sequence.
std::size_t cutPoint(const std::string& raw, std::size_t max)
{
    std::size_t cut = max;
    for (std::size_t back = 0; back < 3 && cut > 1; ++back) {
        if ((static_cast<unsigned char>(raw[cut]) & 0xC0) != 0x80) {
            break;
        }
        --cut;
    }
    return cut;
}

} // namespace

std::string sanitizeConsoleText(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    std::size_t column = 0;
    for (std::size_t i = 0; i < raw.size();) {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1B) {
            // ESC [ params final: colours and cursor moves from the RTEMS shell.
            if (i + 1 < raw.size() && raw[i + 1] == '[') {
                std::size_t j = i + 2;
                while (j < raw.size() && (static_cast<unsigned char>(raw[j]) < 0x40
                                          || static_cast<unsigned char>(raw[j]) > 0x7E)) {
                    ++j;
                }
                i = j < raw.size() ? j + 1 : j;
            } else {
                i += 2;   // ESC + one character
            }
            continue;
        }
        if (c == '\t') {
            const std::size_t spaces = kTabWidth - column % kTabWidth;
            out.append(spaces, ' ');
            column += spaces;
            ++i;
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            ++column;
            ++i;
            continue;
        }
        if (c >= 0x80) {
            if (const std::size_t n = utf8Length(raw, i); n != 0) {
                out.append(raw, i, n);
                ++column;
                i += n;
                continue;
            }
        }
        out += kReplacement;   // other control character or invalid UTF-8
        ++column;
        ++i;
    }
    return out;
}

void ConsoleStore::ingest(const Bytes& datagram, ConsoleTime now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++version_;
    ++datagrams_;
    bytes_ += datagram.size();
    lastRx_ = now;
    if (datagram.empty()) {
        return;
    }
    lastData_ = now;
    for (const std::uint8_t byte : datagram) {
        if (pending_.empty()) {
            pendingSince_ = now;
        }
        if (byte == '\n') {
            if (!pending_.empty() && pending_.back() == '\r') {
                pending_.pop_back();
            }
            emit(std::move(pending_), pendingSince_);
            pending_.clear();
            continue;
        }
        pending_.push_back(static_cast<char>(byte));
        if (pending_.size() > kMaxLineLength) {
            const std::size_t cut = cutPoint(pending_, kMaxLineLength);
            emit(pending_.substr(0, cut), pendingSince_);
            pending_.erase(0, cut);
        }
    }
}

void ConsoleStore::flushIdle(ConsoleTime now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.empty() || now - lastData_ < kPartialFlush) {
        return;
    }
    if (pending_.back() == '\r') {
        pending_.pop_back();   // its '\n' is late: the line is complete
    }
    if (!pending_.empty()) {
        emit(std::move(pending_), pendingSince_);
        ++version_;
    }
    pending_.clear();
}

void ConsoleStore::emit(std::string raw, ConsoleTime rxTime)
{
    lines_.push_back({rxTime, sanitizeConsoleText(raw)});
    if (lines_.size() > kMaxLines) {
        lines_.pop_front();
        ++dropped_;
    }
}

void ConsoleStore::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    pending_.clear();
    datagrams_ = 0;
    bytes_ = 0;
    dropped_ = 0;
    lastRx_.reset();
    ++version_;
}

ConsoleSnapshot ConsoleStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    ConsoleSnapshot s;
    s.lines.assign(lines_.begin(), lines_.end());
    s.datagrams = datagrams_;
    s.bytes = bytes_;
    s.dropped = dropped_;
    s.lastRx = lastRx_;
    return s;
}

} // namespace mcs
