#pragma once

#include "catalog/command_def.hpp"
#include "core/bytes.hpp"
#include "core/errors.hpp"
#include "protocol/framing_layer.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace mcs::test {

// The DS app as defined in config/telecommand/ds.json, built in code.
inline AppDef dsApp()
{
    AppDef app;
    app.name = "ds";
    app.help = "Data Storage application";
    app.commands.push_back({"noop", 0, "No-op", {}, false, 0x194B});
    app.commands.push_back({"reset", 1, "Reset counters", {}, false, 0x194B});
    app.commands.push_back({"set_app_state", 2, "Enable/disable",
                            {FieldDef::enumeration("state", FieldType::U16,
                                                   {{"disable", 0}, {"enable", 1}}),
                             FieldDef::padding(2)},
                            false, 0x194B});
    return app;
}

// Records everything sent; can be told to fail.
class FakeTransport : public ITransport {
public:
    explicit FakeTransport(std::vector<Bytes>* sink, bool dryRun = false)
        : sink_(sink), dryRun_(dryRun) {}
    void send(const Bytes& data) override
    {
        if (failNext) {
            failNext = false;
            throw TransportError("fake: link down");
        }
        sink_->push_back(data);
    }
    std::string describe() const override { return "fake://"; }
    bool isDryRun() const override { return dryRun_; }
    bool failNext = false;

private:
    std::vector<Bytes>* sink_;
    bool dryRun_;
};

// Prepends a fixed tag, standing in for a CSP-style wrapping layer.
class TagLayer : public IFramingLayer {
public:
    explicit TagLayer(std::uint8_t tag) : tag_(tag) {}
    Bytes wrap(const Bytes& data) override
    {
        Bytes out{tag_};
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }
    std::string describe() const override { return "tag"; }

private:
    std::uint8_t tag_;
};

// A cFE 6.7 telemetry packet: CCSDS primary header (big-endian, secondary
// header flag set), 6-byte big-endian time, then `payload`.
// `timeOrder` is the byte order of the secondary-header time (cFE 6.7 writes
// it in the CPU's order).
inline Bytes tlmPacket(std::uint16_t mid, std::uint16_t sequence, const Bytes& payload,
                       std::uint32_t seconds = 0, std::uint16_t subseconds = 0,
                       Endian timeOrder = Endian::Big)
{
    const std::size_t length = 12 + payload.size();
    const auto lengthField = static_cast<std::uint16_t>(length - 7);
    const auto seqField = static_cast<std::uint16_t>(0xC000 | (sequence & 0x3FFF));
    Bytes p{static_cast<std::uint8_t>(mid >> 8),        static_cast<std::uint8_t>(mid),
            static_cast<std::uint8_t>(seqField >> 8),   static_cast<std::uint8_t>(seqField),
            static_cast<std::uint8_t>(lengthField >> 8), static_cast<std::uint8_t>(lengthField),
            static_cast<std::uint8_t>(seconds >> 24),   static_cast<std::uint8_t>(seconds >> 16),
            static_cast<std::uint8_t>(seconds >> 8),    static_cast<std::uint8_t>(seconds),
            static_cast<std::uint8_t>(subseconds >> 8), static_cast<std::uint8_t>(subseconds)};
    if (timeOrder == Endian::Little) {
        std::reverse(p.begin() + 6, p.begin() + 10);
        std::reverse(p.begin() + 10, p.begin() + 12);
    }
    for (const std::uint8_t b : payload) {   // not insert(): GCC 11 -O2 -Wstringop-overread false positive
        p.push_back(b);
    }
    return p;
}

// EVS long event payload (little-endian target), as in config/telemetry/evs.json.
inline Bytes evsLongEventPayload(const std::string& app, std::uint16_t id, std::uint16_t type,
                                 const std::string& message)
{
    Bytes p(20, 0);
    std::copy(app.begin(), app.end(), p.begin());
    p.push_back(static_cast<std::uint8_t>(id));
    p.push_back(static_cast<std::uint8_t>(id >> 8));
    p.push_back(static_cast<std::uint8_t>(type));
    p.push_back(static_cast<std::uint8_t>(type >> 8));
    p.insert(p.end(), {0x42, 0, 0, 0, 0x01, 0, 0, 0});   // spacecraft 0x42, processor 1
    Bytes text(122, 0);
    std::copy(message.begin(), message.end(), text.begin());
    p.insert(p.end(), text.begin(), text.end());
    p.insert(p.end(), {0, 0});
    return p;
}

// A temporary directory removed at scope exit.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path()
              / ("mcs-test-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path write(const std::string& name, const std::string& content) const
    {
        const auto file = path_ / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << content;
        return file;
    }

private:
    std::filesystem::path path_;
};

} // namespace mcs::test
