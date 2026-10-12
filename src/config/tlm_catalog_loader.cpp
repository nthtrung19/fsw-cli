#include "config/tlm_catalog_loader.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"

namespace mcs {

using jsonutil::Json;

namespace {

TlmFieldDef parseField(const Json& j, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "type", "size", "enum", "units", "hex", "help"}, ctx);

    const std::string typeText = jsonutil::getString(j, "type", ctx);
    const auto type = parseFieldType(typeText);
    if (!type) {
        throw ConfigError(ctx + ": unknown type '" + typeText
                          + "' (u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 string padding)");
    }

    TlmFieldDef f;
    f.type = *type;
    f.name = jsonutil::optString(j, "name", ctx);
    f.units = jsonutil::optString(j, "units", ctx);
    f.hex = jsonutil::optBool(j, "hex", ctx, false);
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
    return f;
}

TlmPacketDef parsePacket(const Json& j, const std::string& app, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "mid", "help", "event", "fields"}, ctx);

    TlmPacketDef p;
    p.app = app;
    p.name = jsonutil::getString(j, "name", ctx);
    const std::string where = ctx + " '" + p.name + "'";
    p.mid = static_cast<std::uint16_t>(jsonutil::getUInt(j, "mid", where, 0xFFFF));
    p.help = jsonutil::optString(j, "help", where);
    p.isEvent = jsonutil::optBool(j, "event", where, false);

    const Json& fields = jsonutil::require(j, "fields", where);
    if (!fields.is_array()) {
        throw ConfigError(where + ": 'fields' must be an array");
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        p.fields.push_back(parseField(fields[i], where + " fields[" + std::to_string(i) + "]"));
    }
    return p;
}

} // namespace

std::vector<TlmPacketDef> loadTlmFile(const std::filesystem::path& file)
{
    const Json root = jsonutil::readFile(file);
    const std::string ctx = file.string();
    jsonutil::checkKeys(root, {"app", "help", "packets"}, ctx);

    const std::string app = jsonutil::getString(root, "app", ctx);
    const Json& packets = jsonutil::require(root, "packets", ctx);
    if (!packets.is_array() || packets.empty()) {
        throw ConfigError(ctx + ": 'packets' must be a non-empty array");
    }
    std::vector<TlmPacketDef> out;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        out.push_back(parsePacket(packets[i], app, ctx + ": packets[" + std::to_string(i) + "]"));
    }
    return out;
}

TelemetryCatalog loadTelemetryCatalog(const std::vector<std::filesystem::path>& files)
{
    TelemetryCatalog catalog;
    for (const auto& file : files) {
        try {
            for (auto& packet : loadTlmFile(file)) {
                catalog.add(std::move(packet));
            }
        } catch (const ConfigError& e) {
            const std::string msg = e.what();
            // loadTlmFile errors already start with the file name; catalog.add ones don't.
            if (msg.rfind(file.string(), 0) == 0) {
                throw;
            }
            throw ConfigError(file.string() + ": " + msg);
        }
    }
    return catalog;
}

} // namespace mcs
