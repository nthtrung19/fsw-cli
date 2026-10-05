#include "core/errors.hpp"
#include "core/hex.hpp"
#include "pipeline/pipeline.hpp"
#include "protocol/ccsds_v1_format.hpp"
#include "registry/plugin_registry.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

using namespace fswcli;

// ---- registry -------------------------------------------------------------------

TEST(Registry, BuiltinsAreRegistered)
{
    const PluginRegistry r = builtinPlugins();
    EXPECT_EQ(r.formatNames(), std::vector<std::string>{"ccsds_v1"});
    EXPECT_EQ(r.transportNames(), (std::vector<std::string>{"dryrun", "udp"}));
    EXPECT_TRUE(r.layerNames().empty());
}

TEST(Registry, CreatesBuiltins)
{
    const PluginRegistry r = builtinPlugins();
    EXPECT_EQ(r.createFormat("ccsds_v1", Endian::Little, {})->describe(), "ccsds_v1 (little-endian)");
    EXPECT_EQ(r.createTransport("udp", {{"port", "1234"}})->describe(), "udp://127.0.0.1:1234");
    EXPECT_EQ(r.createTransport("udp", {{"host", "localhost"}, {"port", "5"}})->describe(),
              "udp://localhost:5");
    EXPECT_TRUE(r.createTransport("dryrun", {})->isDryRun());
}

TEST(Registry, UnknownNamesListAlternatives)
{
    const PluginRegistry r = builtinPlugins();
    try {
        r.createTransport("tcp", {});
        FAIL();
    } catch (const ConfigError& e) {
        EXPECT_STREQ(e.what(), "unknown transport 'tcp' (known: dryrun, udp)");
    }
    EXPECT_THROW(r.createFormat("ccsds_v9", Endian::Little, {}), ConfigError);
    EXPECT_THROW(r.createLayer("csp", Endian::Little, {}), ConfigError);
}

TEST(Registry, BuiltinOptionsAreValidated)
{
    const PluginRegistry r = builtinPlugins();
    EXPECT_THROW(r.createTransport("udp", {}), ConfigError);                       // port required
    EXPECT_THROW(r.createTransport("udp", {{"port", "0"}}), ConfigError);          // out of range
    EXPECT_THROW(r.createTransport("udp", {{"port", "1"}, {"hots", "x"}}), ConfigError);
    EXPECT_THROW(r.createTransport("dryrun", {{"x", "1"}}), ConfigError);
    EXPECT_THROW(r.createFormat("ccsds_v1", Endian::Little, {{"checksum", "maybe"}}), ConfigError);
    EXPECT_THROW(r.createTransport("udp", {{"host", "no.such.host.invalid"}, {"port", "1"}}),
                 TransportError);
}

TEST(Registry, NewPluginsNeedOnlyARegistration)
{
    // Demonstrates the extension point: a new layer type is usable by name.
    PluginRegistry r = builtinPlugins();
    r.addLayer("tag", [](Endian, const Options& opts) {
        return std::make_unique<test::TagLayer>(
            static_cast<std::uint8_t>(optUInt(opts, "value", 0, 255, 0xAA, "layer 'tag'")));
    });
    EXPECT_EQ(r.createLayer("tag", Endian::Little, {{"value", "0x7E"}})->wrap({1}), (Bytes{0x7E, 1}));
    EXPECT_THROW(r.addLayer("tag", [](Endian, const Options&) { return nullptr; }), ConfigError);
}

// ---- pipeline ---------------------------------------------------------------------

TEST(Pipeline, FormatThenTransport)
{
    std::vector<Bytes> sent;
    Pipeline p(std::make_unique<CcsdsV1Format>(Endian::Little), {},
               std::make_unique<test::FakeTransport>(&sent));
    const auto result = p.send({0x194B, 0, {}});
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(toHex(sent[0]), "19 4B C0 00 00 01 6C 00");
    EXPECT_EQ(result.packet, result.wire);
    EXPECT_EQ(result.note, "apid=0x14B seq=0");
    EXPECT_EQ(p.describe(), "ccsds_v1 (little-endian) -> fake://");
    EXPECT_EQ(p.destination(), "fake://");
}

TEST(Pipeline, LayersApplyInOrder)
{
    std::vector<Bytes> sent;
    std::vector<std::unique_ptr<IFramingLayer>> layers;
    layers.push_back(std::make_unique<test::TagLayer>(0x01));
    layers.push_back(std::make_unique<test::TagLayer>(0x02));
    Pipeline p(std::make_unique<CcsdsV1Format>(Endian::Little), std::move(layers),
               std::make_unique<test::FakeTransport>(&sent));

    const auto result = p.send({0x194B, 0, {}});
    EXPECT_EQ(toHex(result.packet), "19 4B C0 00 00 01 6C 00");
    EXPECT_EQ(toHex(sent[0]), "02 01 19 4B C0 00 00 01 6C 00");   // outer layer last
    EXPECT_EQ(result.wire, sent[0]);
    EXPECT_TRUE(p.hasLayers());
    EXPECT_EQ(p.describe(), "ccsds_v1 (little-endian) -> tag -> tag -> fake://");
}

TEST(Pipeline, RawSkipsFormatButKeepsLayers)
{
    std::vector<Bytes> sent;
    std::vector<std::unique_ptr<IFramingLayer>> layers;
    layers.push_back(std::make_unique<test::TagLayer>(0xEE));
    Pipeline p(std::make_unique<CcsdsV1Format>(Endian::Little), std::move(layers),
               std::make_unique<test::FakeTransport>(&sent));
    p.sendRaw({0xAA, 0xBB});
    EXPECT_EQ(sent[0], (Bytes{0xEE, 0xAA, 0xBB}));
}

TEST(Pipeline, RequiresFormatAndTransport)
{
    std::vector<Bytes> sent;
    EXPECT_THROW(Pipeline(nullptr, {}, std::make_unique<test::FakeTransport>(&sent)), ConfigError);
    EXPECT_THROW(Pipeline(std::make_unique<CcsdsV1Format>(Endian::Little), {}, nullptr), ConfigError);
}
