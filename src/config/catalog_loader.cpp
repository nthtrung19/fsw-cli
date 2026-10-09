#include "config/catalog_loader.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"
#include "encode/payload_encoder.hpp"

#include <limits>

namespace mcs {

using jsonutil::Json;

namespace {

FieldDef parseField(const Json& j, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "type", "size", "enum", "min", "max", "help", "default"}, ctx);

    const std::string typeText = jsonutil::getString(j, "type", ctx);
    const auto type = parseFieldType(typeText);
    if (!type) {
        throw ConfigError(ctx + ": unknown type '" + typeText
                          + "' (u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 string padding)");
    }

    FieldDef f;
    f.type = *type;
    f.name = jsonutil::optString(j, "name", ctx);
    f.help = jsonutil::optString(j, "help", ctx);

    const auto size = jsonutil::optUInt(j, "size", ctx, 0xFFFF);
    if (naturalSize(f.type) != 0) {
        f.size = size ? static_cast<std::size_t>(*size) : naturalSize(f.type);
    } else if (!size) {
        throw ConfigError(ctx + ": '" + typeText + "' fields need a 'size'");
    } else {
        f.size = static_cast<std::size_t>(*size);
    }

    if (const auto it = j.find("enum"); it != j.end()) {
        jsonutil::requireObject(*it, ctx + ": 'enum'");
        for (const auto& item : it->items()) {
            f.enumValues.push_back({item.key(), jsonutil::toInt(item.value(),
                                                                ctx + ": enum '" + item.key() + "'")});
        }
    }
    f.min = jsonutil::optInt(j, "min", ctx);
    f.max = jsonutil::optInt(j, "max", ctx);

    // Kept as argument text: it is encoded exactly as if the user had typed it.
    if (const auto it = j.find("default"); it != j.end()) {
        if (it->is_string()) {
            f.defaultValue = it->get<std::string>();
        } else if (it->is_number()) {
            f.defaultValue = it->dump();
        } else {
            throw ConfigError(ctx + ": 'default' must be a string or a number, got " + it->type_name());
        }
    }
    return f;
}

CommandDef parseCommand(const Json& j, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "cc", "help", "critical", "fields"}, ctx);

    CommandDef c;
    c.name = jsonutil::getString(j, "name", ctx);
    const std::string where = ctx + " '" + c.name + "'";
    c.cc = static_cast<std::uint8_t>(jsonutil::getUInt(j, "cc", where, 0x7F));
    c.help = jsonutil::optString(j, "help", where);
    c.critical = jsonutil::optBool(j, "critical", where, false);

    if (const auto it = j.find("fields"); it != j.end()) {
        if (!it->is_array()) {
            throw ConfigError(where + ": 'fields' must be an array");
        }
        for (std::size_t i = 0; i < it->size(); ++i) {
            c.fields.push_back(parseField((*it)[i], where + " fields[" + std::to_string(i) + "]"));
        }
    }
    return c;
}

// One MID and the commands sent on it: { "mid": ..., "commands": [...] }.
// `ctx` locates the group ("file.json" or "file.json: mids[1]").
void parseMidGroup(const Json& group, const std::string& ctx, AppDef& app)
{
    const auto mid = static_cast<std::uint16_t>(jsonutil::getUInt(group, "mid", ctx, 0xFFFF));
    const Json& commands = jsonutil::require(group, "commands", ctx);
    if (!commands.is_array() || commands.empty()) {
        throw ConfigError(ctx + ": 'commands' must be a non-empty array");
    }
    const std::string prefix = ctx.find(": ") == std::string::npos ? ctx + ": " : ctx + " ";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        CommandDef cmd = parseCommand(commands[i], prefix + "commands[" + std::to_string(i) + "]");
        cmd.mid = mid;
        app.commands.push_back(std::move(cmd));
    }
}

} // namespace

AppDef loadAppFile(const std::filesystem::path& file)
{
    const Json root = jsonutil::readFile(file);
    const std::string ctx = file.string();
    jsonutil::checkKeys(root, {"app", "help", "mids", "mid", "commands"}, ctx);

    AppDef app;
    app.name = jsonutil::getString(root, "app", ctx);
    app.help = jsonutil::optString(root, "help", ctx);

    // Either "mids": [ { "mid", "commands" }, ... ] for an app with several
    // MIDs, or "mid" + "commands" at the top level as shorthand for one.
    const bool grouped = root.contains("mids");
    const bool single = root.contains("mid") || root.contains("commands");
    if (grouped && single) {
        throw ConfigError(ctx + ": use either 'mids' or 'mid' + 'commands', not both");
    }
    if (!grouped) {
        parseMidGroup(root, ctx, app);
        return app;
    }

    const Json& groups = root.at("mids");
    if (!groups.is_array() || groups.empty()) {
        throw ConfigError(ctx + ": 'mids' must be a non-empty array");
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::string where = ctx + ": mids[" + std::to_string(i) + "]";
        jsonutil::checkKeys(groups[i], {"mid", "commands"}, where);
        parseMidGroup(groups[i], where, app);
    }
    return app;
}

CommandCatalog loadCatalog(const std::vector<std::filesystem::path>& files)
{
    CommandCatalog catalog;
    for (const auto& file : files) {
        try {
            catalog.add(loadAppFile(file));
            // Defaults are encoded like typed arguments; reject bad ones at start-up.
            // Byte order does not affect validation.
            const PayloadEncoder checker(Endian::Little);
            const AppDef& app = catalog.apps().back();
            for (const auto& cmd : app.commands) {
                try {
                    checker.checkDefaults(cmd);
                } catch (const ParseError& e) {
                    throw ConfigError("app '" + app.name + "' default: " + e.what());
                }
            }
        } catch (const ConfigError& e) {
            const std::string msg = e.what();
            // loadAppFile errors already start with the file name; catalog.add ones don't.
            if (msg.rfind(file.string(), 0) == 0) {
                throw;
            }
            throw ConfigError(file.string() + ": " + msg);
        }
    }
    return catalog;
}

} // namespace mcs
