#include "config/catalog_loader.hpp"
#include "config/config_paths.hpp"
#include "config/pipeline_factory.hpp"
#include "config/tlm_catalog_loader.hpp"
#include "config/target_config.hpp"
#include "core/errors.hpp"
#include "core/hex.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/ds.json"});
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
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/ds.json"});
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("ds", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
    }
}

TEST(ShippedConfig, CiCatalogMatchesCiHeaders)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/ci.json"});
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

TEST(ShippedConfig, AocsCatalogMatchesAocsCmdsH)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/aocs.json"});
    const AppDef* aocs = catalog.findApp("aocs");
    ASSERT_NE(aocs, nullptr);
    EXPECT_EQ(aocs->mids(), (std::vector<std::uint16_t>{0x1911, 0x1912}));   // aocs_app_msgids.h

    // Command code (aocs_cmds.h) and payload = sizeof(command struct) - 8: i386 PC build
    // (labsat layout for inertial_pointing, change_control_gain and change_test_solution).
    struct Expected {
        const char* name;
        std::uint16_t mid;
        int cc;
        std::size_t payload;
    };
    const Expected expected[] = {
        {"noop", 0x1911, 0, 0},                    {"reset", 0x1911, 1, 0},
        {"process", 0x1911, 2, 0},                 {"detumble_start", 0x1911, 3, 0},
        {"stop_control", 0x1911, 4, 0},            {"target_spot_pointing", 0x1911, 5, 32},
        {"nadir_pointing", 0x1911, 6, 4},          {"coarse_sun_pointing", 0x1911, 7, 4},
        {"xband_pointing", 0x1911, 8, 32},         {"sband_pointing", 0x1911, 9, 32},
        {"thruster_pointing", 0x1911, 10, 64},     {"request_thruster_ready", 0x1911, 15, 0},
        {"active_propul_system", 0x1911, 16, 0},   {"deactive_propul_system", 0x1911, 17, 0},
        {"target_strip_pointing", 0x1911, 18, 32}, {"fine_sun_pointing", 0x1911, 19, 4},
        {"inertial_pointing", 0x1911, 20, 56},     {"change_control_gain", 0x1911, 26, 80},
        {"change_test_solution", 0x1911, 27, 3},   {"orbit_state_from_tle", 0x1911, 28, 68},
        {"hk", 0x1912, 0, 0},
    };
    ASSERT_EQ(aocs->commands.size(), std::size(expected));
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("aocs", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, e.mid) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
        EXPECT_FALSE(def->critical) << e.name;
    }
}

TEST(ShippedConfig, ScCatalogMatchesScMsgH)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/sc.json"});
    const AppDef* sc = catalog.findApp("sc");
    ASSERT_NE(sc, nullptr);
    EXPECT_EQ(sc->mids(), (std::vector<std::uint16_t>{0x1989, 0x198A}));   // sc_msgids.h

    // Command code (sc_msgdefs.h) and payload = sizeof(command struct) - 8 (sc_msg.h).
    struct Expected {
        const char* name;
        std::uint16_t mid;
        int cc;
        std::size_t payload;
    };
    const Expected expected[] = {
        {"noop", 0x1989, 0, 0},                       {"reset", 0x1989, 1, 0},
        {"start_ats", 0x1989, 2, 2},                  {"stop_ats", 0x1989, 3, 0},
        {"start_rts", 0x1989, 4, 2},                  {"stop_rts", 0x1989, 5, 2},
        {"disable_rts", 0x1989, 6, 2},                {"enable_rts", 0x1989, 7, 2},
        {"switch_ats", 0x1989, 8, 0},                 {"jump_ats", 0x1989, 9, 4},
        {"continue_ats_on_failure", 0x1989, 10, 2},   {"append_ats", 0x1989, 11, 2},
        {"start_rts_group", 0x1989, 13, 4},           {"stop_rts_group", 0x1989, 14, 4},
        {"disable_rts_group", 0x1989, 15, 4},         {"enable_rts_group", 0x1989, 16, 4},
        {"hk", 0x198A, 0, 0},
    };
    ASSERT_EQ(sc->commands.size(), std::size(expected));
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("sc", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, e.mid) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
        EXPECT_FALSE(def->critical) << e.name;
    }
}

TEST(ShippedConfig, FdirCatalogMatchesFdirPlatformMsgH)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/fdir.json"});
    const AppDef* fdir = catalog.findApp("fdir");
    ASSERT_NE(fdir, nullptr);
    EXPECT_EQ(fdir->mids(),
              (std::vector<std::uint16_t>{0x1882, 0x1883, 0x1884, 0x1885}));   // fdir_platform_msgids.h

    // Command code (FDIR_AppCmdCode_t) and payload = sizeof(command struct) - 8 (fdir_platform_msg.h).
    struct Expected {
        const char* name;
        std::uint16_t mid;
        int cc;
        std::size_t payload;
    };
    const Expected expected[] = {
        {"noop", 0x1882, 0, 0},                        {"reset", 0x1882, 1, 0},
        {"trigger_event_watchpoint", 0x1882, 2, 24},   {"enable_event_tracking", 0x1882, 3, 0},
        {"disable_event_tracking", 0x1882, 4, 0},      {"enable_hqm_idx", 0x1882, 5, 1},
        {"disable_hqm_idx", 0x1882, 6, 1},             {"activate_hqm_overrequested", 0x1882, 7, 0},
        {"update_etd", 0x1883, 0, 0},                  {"update_hqm", 0x1883, 1, 0},
        {"update_fud", 0x1883, 2, 0},                  {"dump_etd", 0x1883, 3, 0},
        {"dump_hqm", 0x1883, 4, 0},                    {"dump_fud", 0x1883, 5, 0},
        {"manage_etd", 0x1883, 6, 0},                  {"manage_hqm", 0x1883, 7, 0},
        {"manage_fud", 0x1883, 8, 0},                  {"update_io_timeout", 0x1883, 9, 4},
        {"hk", 0x1884, 0, 0},                          {"hw_hk", 0x1884, 1, 0},
        {"read_obc_temperature", 0x1884, 2, 0},        {"read_obc_edac", 0x1884, 3, 0},
        {"release_hardware", 0x1885, 0, 1},
    };
    ASSERT_EQ(fdir->commands.size(), std::size(expected));
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("fdir", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, e.mid) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
        EXPECT_FALSE(def->critical) << e.name;
    }
}

TEST(ShippedConfig, EvsCatalogMatchesCfeEvsMsgH)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/evs.json"});
    const AppDef* evs = catalog.findApp("evs");
    ASSERT_NE(evs, nullptr);
    EXPECT_EQ(evs->mids(), (std::vector<std::uint16_t>{0x1801}));   // CFE_EVS_CMD_MID

    // Command code (cfe_evs_msg.h) and payload = sizeof(command struct) - 8 on the i386 build:
    // NoArgsCmd 8, BitMaskCmd 10, AppNameCmd 28, AppNameBitMaskCmd 30.
    struct Expected {
        const char* name;
        int cc;
        std::size_t payload;
        bool critical;
    };
    const Expected expected[] = {
        {"noop", 0, 0, false},
        {"reset", 1, 0, false},
        {"enable_event_type", 2, 2, false},
        {"disable_event_type", 3, 2, true},
        {"enable_app_event_type", 5, 22, false},
        {"disable_app_event_type", 6, 22, false},
        {"enable_app_events", 7, 20, false},
        {"disable_app_events", 8, 20, true},
        {"reset_app_counter", 9, 20, false},
        {"clear_log", 20, 0, false},
    };
    ASSERT_EQ(evs->commands.size(), std::size(expected));
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("evs", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, 0x1801) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
        EXPECT_EQ(def->critical, e.critical) << e.name;
    }
    // EVS accepts masks 0x01..0x0F only (cfe_evs_task.c): "all" is 0x0F, not 0x1F.
    const CommandDef* type = catalog.findCommand("evs", "enable_app_event_type");
    ASSERT_NE(type, nullptr);
    std::int64_t largest = 0;
    for (const auto& v : type->fields[1].enumValues) {
        largest = std::max(largest, v.value);
    }
    EXPECT_EQ(largest, 0x0F);
}

TEST(ShippedConfig, MainCatalogMatchesMainMsgdefsH)
{
    const CommandCatalog catalog = loadCatalog({kSourceDir + "/config/telecommand/main.json"});
    const AppDef* mainApp = catalog.findApp("main");
    ASSERT_NE(mainApp, nullptr);
    EXPECT_EQ(mainApp->mids(), (std::vector<std::uint16_t>{0x1900, 0x1901, 0x1903, 0x1904, 0x1905,
                                                           0x1906, 0x1907}));   // main_msgids.h

    // Command code and payload = sizeof(command struct) - 8 (main_msgdefs.h; explicit padding,
    // so the same on the i386 PC build and the labsat build).
    struct Expected {
        const char* name;
        std::uint16_t mid;
        int cc;
        std::size_t payload;
    };
    const Expected expected[] = {
        {"set_switch_state", 0x1900, 0, 4},         {"set_charging_state", 0x1900, 1, 2},
        {"to_hibernate", 0x1901, 0, 0},             {"to_detumble", 0x1901, 1, 0},
        {"to_coarse_sun_pointing", 0x1901, 3, 0},   {"to_fine_nadir_pointing", 0x1901, 4, 0},
        {"to_fine_sun_pointing", 0x1901, 5, 0},     {"to_capture_spot", 0x1901, 6, 24},
        {"to_downlink", 0x1901, 7, 32},             {"to_orbit_transfer", 0x1901, 8, 56},
        {"to_capture_strip", 0x1901, 9, 24},        {"to_inertial_pointing", 0x1901, 10, 48},
        {"noop", 0x1903, 0, 0},                     {"prop_init", 0x1904, 0, 0},
        {"prop_activate", 0x1904, 1, 0},            {"prop_deactivate", 0x1904, 2, 0},
        {"dump_devmap", 0x1905, 0, 0},              {"update_devmap", 0x1905, 1, 0},
        {"manage_devmap", 0x1905, 2, 0},            {"dump_modecfg", 0x1905, 3, 0},
        {"update_modecfg", 0x1905, 4, 0},           {"manage_modecfg", 0x1905, 5, 0},
        {"dump_epscfg", 0x1905, 6, 0},              {"update_epscfg", 0x1905, 7, 0},
        {"manage_epscfg", 0x1905, 8, 0},            {"dump_dtbcfg", 0x1905, 9, 0},
        {"update_dtbcfg", 0x1905, 10, 0},           {"manage_dtbcfg", 0x1905, 11, 0},
        {"dump_tevcfg", 0x1905, 12, 0},             {"update_tevcfg", 0x1905, 13, 0},
        {"manage_tevcfg", 0x1905, 14, 0},           {"hk", 0x1906, 0, 0},
        {"capture", 0x1907, 0, 0},
    };
    ASSERT_EQ(mainApp->commands.size(), std::size(expected));
    for (const auto& e : expected) {
        const CommandDef* def = catalog.findCommand("main", e.name);
        ASSERT_NE(def, nullptr) << e.name;
        EXPECT_EQ(def->mid, e.mid) << e.name;
        EXPECT_EQ(def->cc, e.cc) << e.name;
        EXPECT_EQ(def->payloadSize(), e.payload) << e.name;
        EXPECT_FALSE(def->critical) << e.name;
    }
}

TEST(ShippedConfig, TargetsFileLoadsAndBuilds)
{
    const TargetsFile targets = loadTargetsFile(kSourceDir + "/config/targets.json");
    EXPECT_EQ(targets.defaultTarget, "fsw");
    const TargetConfig fsw = selectTarget(targets, std::nullopt);   // copy: avoids a GCC false positive
    EXPECT_EQ(fsw.name, "fsw");
    EXPECT_EQ(fsw.endian, Endian::Little);
    EXPECT_EQ(fsw.format.type, "ccsds_v1");
    EXPECT_EQ(fsw.transport.type, "udp");
    EXPECT_EQ(fsw.transport.options.at("port"), "1234");   // JSON number -> option text
    ASSERT_EQ(fsw.catalogFiles.size(), 17U);
    EXPECT_TRUE(std::filesystem::exists(fsw.catalogFiles[0]));

    for (const auto& t : targets.targets) {
        EXPECT_NO_THROW(loadCatalog(t.catalogFiles)) << t.name;
        EXPECT_NO_THROW(buildPipeline(t, builtinPlugins(), /*forceDryRun=*/false)) << t.name;
        if (t.telemetry) {
            EXPECT_NO_THROW(loadTelemetryCatalog(t.telemetry->catalogFiles)) << t.name;
        }
    }

    ASSERT_TRUE(fsw.telemetry);
    EXPECT_EQ(fsw.telemetry->receiver.type, "udp");
    EXPECT_EQ(fsw.telemetry->receiver.options.at("port"), "5011");
    ASSERT_FALSE(fsw.telemetry->catalogFiles.empty());
    EXPECT_TRUE(std::filesystem::exists(fsw.telemetry->catalogFiles[0]));

    // File transfer with CF 2.2.1 (cf_msgids.h, cf_cfgtable.c).
    ASSERT_TRUE(fsw.cfdp);
    EXPECT_EQ(fsw.cfdp->localEntity, "0.23");
    EXPECT_EQ(fsw.cfdp->remoteEntity, "0.24");
    EXPECT_EQ(fsw.cfdp->uplinkMid, 0x192D);
    EXPECT_EQ(fsw.cfdp->downlinkMid, 0x092D);
    EXPECT_EQ(fsw.cfdp->chunkSize, 512U);
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

TEST(TargetsFile, TelemetryBlock)
{
    test::TempDir dir;
    const auto target = [](const std::string& tlm) {
        return R"({ "targets": { "t": { "endian": "little", "catalog": ["x.json"],
            "format": { "type": "ccsds_v1" }, "transport": { "type": "dryrun" }, "telemetry": )"
               + tlm + " } } }";
    };

    const TargetsFile ok = loadTargetsFile(dir.write("conf/ok.json",
        target(R"({ "receiver": { "type": "udp", "port": 6000 }, "catalog": ["tlm/a.json"] })")));
    ASSERT_TRUE(ok.targets[0].telemetry);
    EXPECT_EQ(ok.targets[0].telemetry->receiver.options.at("port"), "6000");
    EXPECT_EQ(ok.targets[0].telemetry->catalogFiles[0],
              (std::filesystem::absolute(dir.path()) / "conf/tlm/a.json").lexically_normal());

    const TargetsFile noCatalog = loadTargetsFile(dir.write("nc.json",
        target(R"({ "receiver": { "type": "udp" } })")));
    EXPECT_TRUE(noCatalog.targets[0].telemetry->catalogFiles.empty());

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("e1.json", target(R"({ "catalog": [] })")));
    }), "telemetry: missing required key 'receiver'"));

    EXPECT_TRUE(contains(configErrorOf([&] {
        loadTargetsFile(dir.write("e2.json", target(R"({ "receiver": { "type": "udp" }, "catalgo": [] })")));
    }), "unknown key 'catalgo'"));

    // Bad receiver options are reported when the receiver is built.
    const TargetsFile badPort = loadTargetsFile(dir.write("e3.json",
        target(R"({ "receiver": { "type": "udp", "port": 0 } })")));
    EXPECT_TRUE(contains(configErrorOf([&] {
        buildReceiver(badPort.targets[0], builtinPlugins(), std::nullopt);
    }), "target 't': telemetry: "));

    EXPECT_TRUE(contains(configErrorOf([&] {
        TargetConfig none = ok.targets[0];
        none.telemetry.reset();
        buildReceiver(none, builtinPlugins(), std::nullopt);
    }), "has no telemetry block"));
}

TEST(TargetsFile, CfdpBlock)
{
    test::TempDir dir;
    const auto target = [](const std::string& cfdp, bool withTelemetry = true) {
        return R"({ "targets": { "t": { "endian": "little", "catalog": ["x.json"],
            "format": { "type": "ccsds_v1" }, "transport": { "type": "dryrun" })"
               + std::string(withTelemetry ? R"(, "telemetry": { "receiver": { "type": "udp" } })" : "")
               + R"(, "cfdp": )" + cfdp + " } } }";
    };

    // Every key is optional: the defaults are CF as built for the fsw target.
    const TargetsFile defaults = loadTargetsFile(dir.write("d.json", target("{}")));
    ASSERT_TRUE(defaults.targets[0].cfdp);
    const cfdp::Config& d = *defaults.targets[0].cfdp;
    EXPECT_EQ(d.localEntity, "0.23");
    EXPECT_EQ(d.uplinkMid, 0x192D);
    EXPECT_EQ(d.pdusPerSecond, 100U);
    EXPECT_EQ(d.downlinkDir, "downlink");
    EXPECT_FALSE(d.saveIncomplete);

    const TargetsFile custom = loadTargetsFile(dir.write("c.json", target(R"({
        "local_entity": "0.21", "remote_entity": "0.42", "uplink_mid": 6445, "downlink_mid": "0x0930",
        "chunk_size": 256, "pdus_per_second": 20, "downlink_dir": "files/in",
        "ack_timeout": 3, "ack_limit": 4, "nak_timeout": 2, "nak_limit": 5, "inactivity_timeout": 60,
        "save_incomplete": true })")));
    const cfdp::Config& c = *custom.targets[0].cfdp;
    EXPECT_EQ(c.localEntity, "0.21");
    EXPECT_EQ(c.remoteEntity, "0.42");
    EXPECT_EQ(c.uplinkMid, 6445);
    EXPECT_EQ(c.downlinkMid, 0x0930);
    EXPECT_EQ(c.chunkSize, 256U);
    EXPECT_EQ(c.pdusPerSecond, 20U);
    EXPECT_EQ(c.downlinkDir, "files/in");
    EXPECT_EQ(c.ackTimeout, 3U);
    EXPECT_EQ(c.ackLimit, 4U);
    EXPECT_EQ(c.nakTimeout, 2U);
    EXPECT_EQ(c.nakLimit, 5U);
    EXPECT_EQ(c.inactivityTimeout, 60U);
    EXPECT_TRUE(c.saveIncomplete);

    const auto error = [&](const std::string& name, const std::string& cfdp, bool withTelemetry = true) {
        return configErrorOf([&] { loadTargetsFile(dir.write(name, target(cfdp, withTelemetry))); });
    };
    EXPECT_TRUE(contains(error("e1.json", "{}", false), "'cfdp' needs a 'telemetry' block"));
    EXPECT_TRUE(contains(error("e2.json", R"({ "local_entity": "23" })"), "2-byte dotted-decimal"));
    EXPECT_TRUE(contains(error("e3.json", R"({ "remote_entity": "0.256" })"), "2-byte dotted-decimal"));
    EXPECT_TRUE(contains(error("e4.json", R"({ "remote_entity": "0.23" })"), "must differ"));
    EXPECT_TRUE(contains(error("e5.json", R"({ "uplink_mid": "0x092D" })"), "not a command MID"));
    EXPECT_TRUE(contains(error("e6.json", R"({ "downlink_mid": "0x192D" })"), "not a telemetry MID"));
    EXPECT_TRUE(contains(error("e7.json", R"({ "chunk_size": 5000 })"), "chunk_size"));
    EXPECT_TRUE(contains(error("e8.json", R"({ "chunk_size": 0 })"), "at least 1"));
    EXPECT_TRUE(contains(error("e9.json", R"({ "pdus_per_second": 0 })"), "at least 1"));
    EXPECT_TRUE(contains(error("e10.json", R"({ "chunk": 512 })"), "unknown key 'chunk'"));
    EXPECT_TRUE(contains(error("e11.json", R"({ "downlink_dir": "" })"), "must not be empty"));
}

TEST(TargetsFile, SelectTarget)
{
    const TargetsFile targets = loadTargetsFile(kSourceDir + "/config/targets.json");
    EXPECT_EQ(selectTarget(targets, std::string("fsw-dry")).name, "fsw-dry");
    const std::string msg = configErrorOf([&] { selectTarget(targets, std::string("nope")); });
    EXPECT_TRUE(contains(msg, "unknown target 'nope'")) << msg;
    EXPECT_TRUE(contains(msg, "available: fsw, fsw-dry")) << msg;
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
