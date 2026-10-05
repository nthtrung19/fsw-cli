#include "catalog/catalog.hpp"
#include "core/errors.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

using namespace fswcli;

namespace {

// Expects add() to throw ConfigError containing `fragment`.
void expectRejected(AppDef app, const std::string& fragment)
{
    CommandCatalog catalog;
    try {
        catalog.add(std::move(app));
        FAIL() << "expected ConfigError containing: " << fragment;
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos) << e.what();
    }
}

} // namespace

TEST(Catalog, AddAndFind)
{
    CommandCatalog catalog;
    catalog.add(test::dsApp());
    ASSERT_NE(catalog.findApp("ds"), nullptr);
    EXPECT_EQ(catalog.findApp("ds")->mid, 0x194B);
    ASSERT_NE(catalog.findCommand("ds", "set_app_state"), nullptr);
    EXPECT_EQ(catalog.findCommand("ds", "set_app_state")->cc, 2);
    EXPECT_EQ(catalog.findCommand("ds", "nope"), nullptr);
    EXPECT_EQ(catalog.findCommand("fm", "noop"), nullptr);
    EXPECT_EQ(catalog.commandCount(), 3U);
}

TEST(Catalog, CommandHelpers)
{
    const AppDef app = test::dsApp();
    const CommandDef& set = app.commands[2];
    EXPECT_EQ(set.payloadSize(), 4U);
    EXPECT_EQ(set.argCount(), 1U);
    EXPECT_EQ(set.usage(), "set_app_state <state: disable|enable>");
    EXPECT_EQ(app.commands[0].usage(), "noop");
}

TEST(Catalog, FieldDescriptions)
{
    FieldDef ranged = FieldDef::number("count", FieldType::U32);
    ranged.min = 1;
    ranged.max = 100;
    EXPECT_EQ(ranged.describe(), "count: u32 1..100");
    EXPECT_EQ(FieldDef::string("path", 64).describe(), "path: string[64]");
    EXPECT_EQ(FieldDef::number("gain", FieldType::F32).describe(), "gain: f32");
}

TEST(Catalog, RejectsDuplicateApp)
{
    CommandCatalog catalog;
    catalog.add(test::dsApp());
    EXPECT_THROW(catalog.add(test::dsApp()), ConfigError);
}

TEST(Catalog, RejectsBadNames)
{
    AppDef app = test::dsApp();
    app.name = "DS";
    expectRejected(app, "name must match");

    app = test::dsApp();
    app.commands[0].name = "no-op";
    expectRejected(app, "command 'no-op'");
}

TEST(Catalog, RejectsDuplicateCommandNameOrCode)
{
    AppDef app = test::dsApp();
    app.commands[1].name = "noop";
    expectRejected(app, "defined more than once");

    app = test::dsApp();
    app.commands[1].cc = 0;
    expectRejected(app, "command code 0 is already used");
}

TEST(Catalog, RejectsCommandCodeAbove127)
{
    AppDef app = test::dsApp();
    app.commands[0].cc = 128;
    expectRejected(app, "out of range 0..127");
}

TEST(Catalog, RejectsInconsistentFields)
{
    AppDef app = test::dsApp();
    app.commands[2].fields[0].size = 4;   // u16 with size 4
    expectRejected(app, "does not match type u16");

    app = test::dsApp();
    app.commands[2].fields[0].enumValues.push_back({"huge", 70000});
    expectRejected(app, "does not fit u16");

    app = test::dsApp();
    app.commands[2].fields[1].name = "pad";
    expectRejected(app, "padding fields must not have a name");

    app = test::dsApp();
    app.commands[2].fields[0].enumValues.push_back({"5", 5});
    expectRejected(app, "must not look like a number");

    app = test::dsApp();
    app.commands[2].fields.push_back(FieldDef::number("state", FieldType::U8));
    expectRejected(app, "field 'state' defined more than once");

    app = test::dsApp();
    FieldDef f = FieldDef::number("x", FieldType::U8);
    f.min = 10;
    f.max = 5;
    app.commands[0].fields.push_back(f);
    expectRejected(app, "min is greater than max");

    app = test::dsApp();
    FieldDef s = FieldDef::string("s", 8);
    s.max = 3;
    app.commands[0].fields.push_back(s);
    expectRejected(app, "only allowed on integer fields");
}
