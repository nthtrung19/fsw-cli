#include "config/catalog_loader.hpp"
#include "config/config_paths.hpp"
#include "config/pipeline_factory.hpp"
#include "config/target_config.hpp"
#include "core/errors.hpp"
#include "core/hex.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace mcs;

namespace {

const std::string kSourceDir = mcs_SOURCE_DIR;

std::string configErrorOf(const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "<no error>";
}

bool contains(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

const char* kMinimalApp = R"({ "app": "ds", "mid": "0x194B", "commands": [ { "name": "noop", "cc": 0 } ] })";

} // namespace

// ---- the shipped files -------------------------------------------------------------

TEST(ShippedConfig, DsCatalogMatchesTheDesign)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/catalog/ds.json"});
    const AppDef* ds = catalog.findApp("ds");
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->mids(), (std::vector<std::uint16_t>{0x194B, 0x194C}));   // ds_msgids.h
    ASSERT_EQ(ds->commands.size(), 19U);
    EXPECT_EQ(catalog.findCommand("ds", "noop")->mid, 0x194B);              // DS_CMD_MID
    const CommandDef* hk = catalog.findCommand("ds", "hk");                  // DS_SEND_HK_MID
    ASSERT_NE(hk, nullptr);
    EXPECT_EQ(hk->mid, 0x194C);
    EXPECT_EQ(hk->cc, 0);
    EXPECT_EQ(hk->payloadSize(), 0U);
    EXPECT_EQ(catalog.findCommand("ds", "noop")->cc, 0);
    EXPECT_EQ(catalog.findCommand("ds", "reset")->cc, 1);
    const CommandDef* set = catalog.findCommand("ds", "set_app_state");
    EXPECT_EQ(set->cc, 2);
    EXPECT_EQ(set->payloadSize(), 4U);   // DS_AppStateCmd_t = 8 header + 4
    EXPECT_EQ(set->usage(), "set_app_state <state: disable|enable>");   // file order kept
}

TEST(ShippedConfig, DsCatalogMatchesDsMsgH)
{
    // Command code (ds_msgdefs.h) and payload size = sizeof(DS_*Cmd_t) - 8 (ds_msg.h).
    struct Expected {
        const char* name;
        int cc;
        std::size_t payload;
    };
    const Expected expected[] = {
        {"noop", 0, 0},             {"reset", 1, 0},           {"set_app_state", 2, 4},
        {"set_filter_file", 3, 8},  {"set_filter_type", 4, 8}, {"set_filter_parms", 5, 12},
        {"set_dest_type", 6, 4},    {"set_dest_state", 7, 4},  {"set_dest_path", 8, 68},
        {"set_dest_base", 9, 68},   {"set_dest_ext", 10, 12},  {"set_dest_size", 11, 8},
        {"set_dest_age", 12, 8},    {"set_dest_count", 13, 8}, {"close_file", 14, 4},
        {"get_file_info", 15, 0},   {"add_mid", 16, 4},        {"close_all", 17, 0},
    };
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/catalog/ds.json"});
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("ds", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
    }
}

TEST(ShippedConfig, CiCatalogMatchesCiHeaders)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/catalog/ci.json"});
    const AppDef* ci = catalog.findApp("ci");
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->mids(), (std::vector<std::uint16_t>{0x1934, 0x1935}));   // ci_msgids.h
    ASSERT_EQ(ci->commands.size(), 4U);

    struct Expected {
        const char* name;
        std::uint16_t mid;
        int cc;
        std::size_t payload;   // sizeof(command struct) - 8
    };
    const Expected expected[] = {
        {"noop", 0x1934, 0, 0},        // CI_NoArgCmd_t
        {"reset", 0x1934, 1, 0},       // CI_NoArgCmd_t
        {"enable_to", 0x1934, 2, 18},  // CI_EnableTOCmd_t: cDestIp[16] + usDestPort
        {"hk", 0x1935, 0, 0},          // CI_SEND_HK_MID
    };
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("ci", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, e.mid) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
    }
    const CommandDef* enable = catalog.findCommand("ci", "enable_to");
    EXPECT_EQ(enable->requiredArgCount(), 0U);
    EXPECT_EQ(enable->usage(),
              "enable_to <dest_ip: string[16] = 127.0.0.1> <dest_port: u16 = 5011>");
}

TEST(ShippedConfig, TargetsFileLoadsAndBuilds)
{
    const TargetsFile targets = loadTargetsFile(kSourceDir + "/config/targets.json");
    EXPECT_EQ(targets.defaultTarget, "sil");
    const TargetConfig sil = selectTarget(targets, std::nullopt);   // copy: avoids a GCC false positive
    EXPECT_EQ(sil.name, "sil");
    EXPECT_EQ(sil.endian, Endian::Little);
    EXPECT_EQ(sil.format.type, "ccsds_v1");
    EXPECT_EQ(sil.transport.type, "udp");
    EXPECT_EQ(sil.transport.options.at("port"), "1234");   // JSON number -> option text
    ASSERT_EQ(sil.catalogFiles.size(), 2U);
    EXPECT_TRUE(std::filesystem::exists(sil.catalogFiles[0]));

    for (const auto& t : targets.targets) {
        EXPECT_NO_THROW(loadCatalog(t.catalogFiles)) << t.name;
        EXPECT_NO_THROW(buildPipeline(t, builtinPlugins(), /*forceDryRun=*/false)) << t.name;
    }
}

// ---- catalog loader errors --------------------------------------------------------------

TEST(CatalogLoader, NumbersAsHexStringsOrIntegers)
{
    test::TempDir dir;
    const auto file = dir.write("x.json", R"({
      "app": "x", "mid": 6475,
      "commands": [ { "name": "go", "cc": "0x05",
        "fields": [ { "name": "v", "type": "i16", "min": "-0x10", "max": 16 } ] } ] })");
    const AppDef app = loadAppFile(file);
    EXPECT_EQ(app.commands[0].mid, 0x194B);   // single-MID shorthand
    EXPECT_EQ(app.commands[0].cc, 5);
    EXPECT_EQ(app.commands[0].fields[0].min, -16);
    EXPECT_EQ(app.commands[0].fields[0].size, 2U);
}

TEST(CatalogLoader, ReportsFileAndLocation)
{
    test::TempDir dir;
    const auto bad = dir.write("bad.json", R"({ "app": "ds", "mid": "0x194B", "commands": [
        { "name": "noop", "cc": 0 },
        { "name": "set", "cc": 2, "fields": [ { "name": "s", "type": "u17" } ] } ] })");
    const std::string msg = configErrorOf([&] { loadAppFile(bad); });
    EXPECT_TRUE(contains(msg, "bad.json")) << msg;
    EXPECT_TRUE(contains(msg, "commands[1] 'set' fields[0]")) << msg;
    EXPECT_TRUE(contains(msg, "unknown type 'u17'")) << msg;
}

TEST(CatalogLoader, RejectsTyposAndMissingKeys)
{
    test::TempDir dir;
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("a.json", R"({ "app": "ds", "mid": 1, "comands": [] })"));
    }), "unknown key 'comands'"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("b.json", R"({ "app": "ds", "commands": [] })"));
    }), "missing required key 'mid'"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("c.json", R"({ "app": "ds", "mid": 1, "commands": [ { "name": "x", "cc": 200 } ] })"));
    }), "expected an integer in [0, 127]"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("d.json", R"({ "app": "ds", "mid": 1, "commands": [
            { "name": "x", "cc": 1, "fields": [ { "name": "s", "type": "string" } ] } ] })"));
    }), "'string' fields need a 'size'"));
}

TEST(CatalogLoader, CommandsGroupedByMid)
{
    test::TempDir dir;
    const AppDef app = loadAppFile(dir.write("x.json", R"({
      "app": "x",
      "mids": [
        { "mid": "0x194B", "commands": [ { "name": "a", "cc": 0 }, { "name": "b", "cc": 1 } ] },
        { "mid": 6476, "commands": [ { "name": "c", "cc": 0 } ] } ] })"));
    ASSERT_EQ(app.commands.size(), 3U);
    EXPECT_EQ(app.commands[0].mid, 0x194B);
    EXPECT_EQ(app.commands[1].mid, 0x194B);
    EXPECT_EQ(app.commands[2].mid, 0x194C);
    EXPECT_EQ(app.mids(), (std::vector<std::uint16_t>{0x194B, 0x194C}));
}

TEST(CatalogLoader, MidGroupErrors)
{
    test::TempDir dir;
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("a.json", R"({ "app": "x", "mid": 1, "commands": [ { "name": "a", "cc": 0 } ],
            "mids": [ { "mid": 2, "commands": [ { "name": "b", "cc": 0 } ] } ] })"));
    }), "use either 'mids' or 'mid' + 'commands', not both"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("b.json", R"({ "app": "x" })"));
    }), "missing required key 'mid'"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("c.json", R"({ "app": "x", "mids": [] })"));
    }), "'mids' must be a non-empty array"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("d.json", R"({ "app": "x", "mids": [ { "mid": 1, "commands": [] } ] })"));
    }), "d.json: mids[0]: 'commands' must be a non-empty array"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("e.json", R"({ "app": "x", "mids": [ { "mid": 70000, "commands": [ { "name": "a", "cc": 0 } ] } ] })"));
    }), "e.json: mids[0]: 'mid': expected an integer in [0, 65535]"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("f.json", R"({ "app": "x", "mids": [ { "mid": 1, "comands": [] } ] })"));
    }), "mids[0]: unknown key 'comands'"));
    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("g.json", R"({ "app": "x", "mids": [ { "mid": 1, "commands": [ { "name": "a" } ] } ] })"));
    }), "g.json: mids[0] commands[0] 'a': missing required key 'cc'"));
}

TEST(CatalogLoader, DefaultsAreKeptAsArgumentText)
{
    test::TempDir dir;
    const AppDef app = loadAppFile(dir.write("x.json", R"({
      "app": "x", "mid": 6475,
      "commands": [ { "name": "go", "cc": 1, "fields": [
        { "name": "ip", "type": "string", "size": 16, "default": "127.0.0.1" },
        { "name": "port", "type": "u16", "default": 5011 } ] } ] })"));
    EXPECT_EQ(app.commands[0].fields[0].defaultValue, "127.0.0.1");
    EXPECT_EQ(app.commands[0].fields[1].defaultValue, "5011");

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadAppFile(dir.write("b.json", R"({ "app": "x", "mid": 1, "commands": [
            { "name": "go", "cc": 1, "fields": [ { "name": "p", "type": "u16", "default": true } ] } ] })"));
    }), "'default' must be a string or a number"));
}

TEST(CatalogLoader, InvalidDefaultIsAConfigErrorNamingTheFile)
{
    test::TempDir dir;
    const auto file = dir.write("bad.json", R"({ "app": "x", "mid": "0x1934", "commands": [
        { "name": "go", "cc": 1, "fields": [ { "name": "port", "type": "u16", "default": 70000 } ] } ] })");
    const std::string error = configErrorOf([&] { loadCatalog({file}); });
    EXPECT_TRUE(contains(error, "bad.json")) << error;
    EXPECT_TRUE(contains(error, "app 'x' default: go: invalid value '70000'")) << error;
    EXPECT_TRUE(contains(error, "out of range for u16")) << error;
}

TEST(CatalogLoader, SyntaxErrorsIncludeLine)
{
    test::TempDir dir;
    const auto file = dir.write("broken.json", "{\n  \"app\": \"ds\",\n  \"mid\": ,\n}");
    const std::string msg = configErrorOf([&] { loadAppFile(file); });
    EXPECT_TRUE(contains(msg, "broken.json: invalid JSON")) << msg;
    EXPECT_TRUE(contains(msg, "line 3")) << msg;
}

TEST(CatalogLoader, CatalogValidationErrorsNameTheFile)
{
    test::TempDir dir;
    const auto a = dir.write("a.json", kMinimalApp);
    const auto b = dir.write("b.json", kMinimalApp);   // same app name twice
    const std::string msg = configErrorOf([&] { loadCatalog({a, b}); });
    EXPECT_TRUE(contains(msg, "b.json")) << msg;
    EXPECT_TRUE(contains(msg, "defined more than once")) << msg;
}

TEST(CatalogLoader, CommentsAreAllowed)
{
    test::TempDir dir;
    const auto file = dir.write("c.json", std::string("// comment\n") + kMinimalApp);
    EXPECT_NO_THROW(loadAppFile(file));
}

// ---- targets file -----------------------------------------------------------------------

TEST(TargetsFile, CatalogPathsAreRelativeToTheFile)
{
    test::TempDir dir;
    dir.write("cat/ds.json", kMinimalApp);
    const auto file = dir.write("conf/targets.json", R"({ "targets": { "t1": {
        "endian": "big", "catalog": ["../cat/ds.json"],
        "format": { "type": "ccsds_v1", "checksum": false },
        "transport": { "type": "dryrun" } } } })");
    const TargetsFile targets = loadTargetsFile(file);
    EXPECT_EQ(targets.defaultTarget, "t1");   // first target when not given
    const TargetConfig& t = targets.targets[0];
    EXPECT_EQ(t.endian, Endian::Big);
    EXPECT_EQ(t.format.options.at("checksum"), "false");
    EXPECT_TRUE(std::filesystem::exists(t.catalogFiles[0]));

    auto pipeline = buildPipeline(t, builtinPlugins(), false);
    EXPECT_TRUE(pipeline->isDryRun());
    EXPECT_EQ(toHex(pipeline->send({0x194B, 0, {}}).packet), "19 4B C0 00 00 01 00 00");
}

TEST(TargetsFile, Errors)
{
    test::TempDir dir;
    const auto target = [](const std::string& body) {
        return R"({ "targets": { "t": { "endian": "little", "catalog": ["x.json"], )" + body + " } } }";
    };
    const std::string ok = R"("format": { "type": "ccsds_v1" }, "transport": { "type": "dryrun" })";

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("1.json", R"({ "default_target": "zz", "targets": { "t": {
            "endian": "little", "catalog": ["x"], "format": {"type":"ccsds_v1"},
            "transport": {"type":"dryrun"} } } })"));
    }), "default_target 'zz' is not defined"));

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("2.json", R"({ "targets": { "t": { "endian": "middle", "catalog": ["x"],
            "format": {"type":"ccsds_v1"}, "transport": {"type":"dryrun"} } } })"));
    }), "invalid endian 'middle'"));

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("3.json", target(R"("transport": { "type": "dryrun" })")));
    }), "missing required key 'format'"));

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("4.json", target(ok + R"(, "transprot": {})")));
    }), "unknown key 'transprot'"));

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("5.json", target(R"("format": {"type":"ccsds_v1"}, "transport": {"port": 1})")));
    }), "transport: missing required key 'type'"));
}

TEST(TargetsFile, SelectTarget)
{
    const TargetsFile targets = loadTargetsFile(kSourceDir + "/config/targets.json");
    EXPECT_EQ(selectTarget(targets, std::string("sil-dry")).name, "sil-dry");
    const std::string msg = configErrorOf([&] { selectTarget(targets, std::string("nope")); });
    EXPECT_TRUE(contains(msg, "unknown target 'nope'")) << msg;
    EXPECT_TRUE(contains(msg, "available: sil, sil-dry")) << msg;
}

TEST(PipelineFactory, ErrorsNameTheTargetAndForceDryRun)
{
    TargetConfig t;
    t.name = "lab";
    t.format = {"ccsds_v1", {}};
    t.transport = {"udp", {{"port", "99999"}}};
    const std::string msg = configErrorOf([&] { buildPipeline(t, builtinPlugins(), false); });
    EXPECT_TRUE(contains(msg, "target 'lab': transport 'udp': option 'port'")) << msg;

    // --dry-run replaces the transport, so a broken transport is not even created.
    EXPECT_TRUE(buildPipeline(t, builtinPlugins(), true)->isDryRun());

    t.layers.push_back({"csp", {}});
    EXPECT_TRUE(contains(configErrorOf([&] { buildPipeline(t, builtinPlugins(), true); }),
                         "unknown layer 'csp'"));
}

// ---- config file lookup -----------------------------------------------------------------

TEST(ConfigPaths, ExplicitPathWinsAndMustExist)
{
    EXPECT_EQ(targetsFileCandidates(std::string("my.json")).size(), 1U);
    EXPECT_TRUE(contains(configErrorOf([] { findTargetsFile(std::string("/no/such/file.json")); }),
                         "config file not found"));
    EXPECT_EQ(findTargetsFile(kSourceDir + "/config/targets.json"),
              std::filesystem::path(kSourceDir + "/config/targets.json"));
}

TEST(ConfigPaths, EnvironmentVariableIsSearchedFirst)
{
    ::setenv("mcs_CONFIG", "/tmp/from-env.json", 1);
    const auto candidates = targetsFileCandidates(std::nullopt);
    ::unsetenv("mcs_CONFIG");
    ASSERT_GE(candidates.size(), 2U);
    EXPECT_EQ(candidates[0], std::filesystem::path("/tmp/from-env.json"));
    EXPECT_EQ(candidates[1], std::filesystem::path("config/targets.json"));
}
