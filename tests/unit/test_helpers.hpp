#pragma once

#include "catalog/command_def.hpp"
#include "core/bytes.hpp"
#include "core/errors.hpp"
#include "protocol/framing_layer.hpp"
#include "transport/transport.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fswcli::test {

// The DS app as defined in config/catalog/ds.json, built in code.
inline AppDef dsApp()
{
    AppDef app;
    app.name = "ds";
    app.mid = 0x194B;
    app.help = "Data Storage application";
    app.commands.push_back({"noop", 0, "No-op", {}, false});
    app.commands.push_back({"reset", 1, "Reset counters", {}, false});
    app.commands.push_back({"set_app_state", 2, "Enable/disable",
                            {FieldDef::enumeration("state", FieldType::U16,
                                                   {{"disable", 0}, {"enable", 1}}),
                             FieldDef::padding(2)},
                            false});
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

// A temporary directory removed at scope exit.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path()
              / ("fswcli-test-" + std::to_string(rd()) + "-" + std::to_string(counter++));
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

} // namespace fswcli::test
