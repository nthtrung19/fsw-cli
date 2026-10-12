#include "config/tlm_catalog_loader.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"

namespace mcs {

using jsonutil::Json;

namespace {

constexpr std::uint64_t kMaxCount = 4096;

// "count": N or [N, M, ...] (row-major); absent = a single item.
std::vector<std::size_t> parseCount(const Json& j, const std::string& ctx)
{
    const auto it = j.find("count");
    if (it == j.end()) {
        return {};
    }
    std::vector<std::size_t> dims;
    if (it->is_array()) {
        if (it->empty()) {
            throw ConfigError(ctx + ": 'count' must not be an empty array");
        }
        for (const auto& d : *it) {
            dims.push_back(static_cast<std::size_t>(jsonutil::toUInt(d, ctx + ": 'count'", kMaxCount)));
        }
    } else {
        dims.push_back(static_cast<std::size_t>(jsonutil::toUInt(*it, ctx + ": 'count'", kMaxCount)));
    }
    std::size_t total = 1;
    for (const std::size_t d : dims) {
        if (d == 0) {
            throw ConfigError(ctx + ": 'count' must be at least 1");
        }
        total *= d;
    }
    if (total > kMaxCount) {
        throw ConfigError(ctx + ": 'count' gives " + std::to_string(total) + " elements (max "
                          + std::to_string(kMaxCount) + ")");
    }
    return dims;
}

// Name suffixes of every element of `dims`, in row-major order: "_0", "_1", ...
// or "_0_0", "_0_1", ...; a single "" when there is no count.
std::vector<std::string> elementSuffixes(const std::vector<std::size_t>& dims)
{
    std::vector<std::string> out{""};
    for (const std::size_t d : dims) {
        std::vector<std::string> next;
        for (const auto& prefix : out) {
            for (std::size_t i = 0; i < d; ++i) {
                next.push_back(prefix + "_" + std::to_string(i));
            }
        }
        out = std::move(next);
    }
    return out;
}

TlmFieldDef parseScalar(const Json& j, const std::string& ctx)
{
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

void parseFieldList(const Json& fields, const std::string& prefix, const std::string& ctx,
                    std::vector<TlmFieldDef>& out);

// Appends the fields of one entry to `out`. An entry is a scalar field, an
// array of them ("count"), or a group of fields ("fields", a nested struct)
// that may also be repeated. Expanded names: name_0, name_0_1, group_member,
// group_0_member. `prefix` is the enclosing group's name plus "_".
void parseField(const Json& j, const std::string& prefix, const std::string& ctx,
                std::vector<TlmFieldDef>& out)
{
    const std::vector<std::size_t> dims = parseCount(j, ctx);
    std::size_t total = 1;
    for (const std::size_t d : dims) {
        total *= d;
    }

    if (j.contains("fields")) {
        jsonutil::checkKeys(j, {"name", "count", "fields", "help"}, ctx);
        const std::string name = jsonutil::getString(j, "name", ctx);
        const Json& members = j.at("fields");
        for (const auto& suffix : elementSuffixes(dims)) {
            parseFieldList(members, prefix + name + suffix + "_", ctx + " '" + name + "'", out);
        }
        return;
    }

    jsonutil::checkKeys(j, {"name", "type", "size", "count", "enum", "units", "hex", "help"}, ctx);
    TlmFieldDef f = parseScalar(j, ctx);
    if (f.type == FieldType::Padding) {
        f.size *= total;
        out.push_back(std::move(f));
        return;
    }
    const std::string name = f.name;
    for (const auto& suffix : elementSuffixes(dims)) {
        f.name = prefix + name + suffix;
        out.push_back(f);
    }
}

void parseFieldList(const Json& fields, const std::string& prefix, const std::string& ctx,
                    std::vector<TlmFieldDef>& out)
{
    if (!fields.is_array()) {
        throw ConfigError(ctx + ": 'fields' must be an array");
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        parseField(fields[i], prefix, ctx + " fields[" + std::to_string(i) + "]", out);
    }
}

TlmPacketDef parsePacket(const Json& j, const std::string& app, const std::string& ctx)
{
    jsonutil::checkKeys(j, {"name", "mid", "help", "event", "variable", "fields"}, ctx);

    TlmPacketDef p;
    p.app = app;
    p.name = jsonutil::getString(j, "name", ctx);
    const std::string where = ctx + " '" + p.name + "'";
    p.mid = static_cast<std::uint16_t>(jsonutil::getUInt(j, "mid", where, 0xFFFF));
    p.help = jsonutil::optString(j, "help", where);
    p.isEvent = jsonutil::optBool(j, "event", where, false);
    p.variableLength = jsonutil::optBool(j, "variable", where, false);

    parseFieldList(jsonutil::require(j, "fields", where), "", where, p.fields);
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
