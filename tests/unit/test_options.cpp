#include "core/errors.hpp"
#include "core/options.hpp"

#include <gtest/gtest.h>

using namespace mcs;

TEST(PluginOptions, KnownKeys)
{
    EXPECT_NO_THROW(requireKnownKeys({{"host", "x"}, {"port", "1"}}, {"host", "port"}, "udp"));
    try {
        requireKnownKeys({{"hots", "x"}}, {"host", "port"}, "transport 'udp'");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& e) {
        EXPECT_STREQ(e.what(), "transport 'udp': unknown option 'hots' (known: host, port)");
    }
    EXPECT_THROW(requireKnownKeys({{"x", "1"}}, {}, "dryrun"), ConfigError);
}

TEST(PluginOptions, Strings)
{
    const Options opts{{"host", "10.0.0.1"}};
    EXPECT_EQ(requireString(opts, "host", "o"), "10.0.0.1");
    EXPECT_EQ(optString(opts, "missing", "def"), "def");
    EXPECT_THROW(requireString(opts, "missing", "o"), ConfigError);
}

TEST(PluginOptions, UnsignedRange)
{
    const Options opts{{"port", "1234"}, {"hex", "0x10"}, {"bad", "70000"}};
    EXPECT_EQ(requireUInt(opts, "port", 1, 65535, "o"), 1234U);
    EXPECT_EQ(optUInt(opts, "hex", 0, 255, 0, "o"), 16U);
    EXPECT_EQ(optUInt(opts, "none", 0, 255, 7, "o"), 7U);
    EXPECT_THROW(requireUInt(opts, "bad", 1, 65535, "o"), ConfigError);
    EXPECT_THROW(requireUInt(opts, "none", 1, 65535, "o"), ConfigError);
}

TEST(PluginOptions, Booleans)
{
    const Options opts{{"a", "true"}, {"b", "OFF"}, {"c", "1"}, {"d", "maybe"}};
    EXPECT_TRUE(optBool(opts, "a", false, "o"));
    EXPECT_FALSE(optBool(opts, "b", true, "o"));
    EXPECT_TRUE(optBool(opts, "c", false, "o"));
    EXPECT_TRUE(optBool(opts, "none", true, "o"));
    EXPECT_THROW(optBool(opts, "d", true, "o"), ConfigError);
}
