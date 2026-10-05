#include "config/catalog_loader.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"

#include <limits>

namespace fswcli {

using jsonutil::Json;

namespace {

FieldDef parseField(const Json& j, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "type", "size", "enum", "min", "max", "help"}, ctx);

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

} // namespace

AppDef loadAppFile(const std::filesystem::path& file)
{
    const Json root = jsonutil::readFile(file);
    const std::string ctx = file.string();
    jsonutil::checkKeys(root, {"app", "mid", "help", "commands"}, ctx);

    AppDef app;
    app.name = jsonutil::getString(root, "app", ctx);
    app.mid = static_cast<std::uint16_t>(jsonutil::getUInt(root, "mid", ctx, 0xFFFF));
    app.help = jsonutil::optString(root, "help", ctx);

    const Json& commands = jsonutil::require(root, "commands", ctx);
    if (!commands.is_array()) {
        throw ConfigError(ctx + ": 'commands' must be an array");
    }
    for (std::size_t i = 0; i < commands.size(); ++i) {
        app.commands.push_back(parseCommand(commands[i], ctx + ": commands[" + std::to_string(i) + "]"));
    }
    return app;
}

CommandCatalog loadCatalog(const std::vector<std::filesystem::path>& files)
{
    CommandCatalog catalog;
    for (const auto& file : files) {
        try {
            catalog.add(loadAppFile(file));
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

} // namespace fswcli
