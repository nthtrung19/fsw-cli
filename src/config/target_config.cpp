#include "config/target_config.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"

#include <cctype>
#include <type_traits>

namespace mcs {

using jsonutil::Json;

namespace {

bool isTargetName(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

// Array of file paths, resolved relative to the targets file.
std::vector<std::filesystem::path> parsePaths(const Json& array, const std::filesystem::path& baseDir,
                                              const std::string& ctx)
{
    std::vector<std::filesystem::path> out;
    for (const auto& entry : array) {
        if (!entry.is_string()) {
            throw ConfigError(ctx + " entries must be strings");
        }
        const std::filesystem::path p(entry.get<std::string>());
        out.push_back(p.is_absolute() ? p : (baseDir / p).lexically_normal());
    }
    return out;
}

// "0.24": two decimal bytes (CF accepts only 2-byte entity IDs).
bool isEntityId(const std::string& text)
{
    const auto dot = text.find('.');
    if (dot == std::string::npos || text.find('.', dot + 1) != std::string::npos) {
        return false;
    }
    for (const std::string& part : {text.substr(0, dot), text.substr(dot + 1)}) {
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos
            || std::stoul(part) > 255) {
            return false;
        }
    }
    return true;
}

// Optional "cfdp" block of a target: file transfer with CF.
cfdp::Config parseCfdp(const Json& j, const std::string& ctx)
{
    jsonutil::requireObject(j, ctx);
    jsonutil::checkKeys(j, {"local_entity", "remote_entity", "uplink_mid", "downlink_mid", "chunk_size",
                            "pdus_per_second", "downlink_dir", "ack_timeout", "ack_limit", "nak_timeout",
                            "nak_limit", "inactivity_timeout", "save_incomplete"}, ctx);
    cfdp::Config c;
    const auto entity = [&](const char* key, std::string& field) {
        field = jsonutil::optString(j, key, ctx, field);
        if (!isEntityId(field)) {
            throw ConfigError(ctx + ": '" + key + "' must be a 2-byte dotted-decimal entity ID such as 0.23, got '"
                              + field + "'");
        }
    };
    entity("local_entity", c.localEntity);
    entity("remote_entity", c.remoteEntity);
    if (c.localEntity == c.remoteEntity) {
        throw ConfigError(ctx + ": 'local_entity' and 'remote_entity' must differ");
    }
    // CCSDS v1 type bit 0x1000: PDUs go up as commands and come down as telemetry.
    c.uplinkMid = static_cast<std::uint16_t>(jsonutil::optUInt(j, "uplink_mid", ctx, 0xFFFF).value_or(c.uplinkMid));
    c.downlinkMid =
        static_cast<std::uint16_t>(jsonutil::optUInt(j, "downlink_mid", ctx, 0xFFFF).value_or(c.downlinkMid));
    if ((c.uplinkMid & 0x1000) == 0) {
        throw ConfigError(ctx + ": 'uplink_mid' " + toHexString(c.uplinkMid, 4)
                          + " is not a command MID (CF receives PDUs as commands, e.g. 0x192D)");
    }
    if ((c.downlinkMid & 0x1000) != 0) {
        throw ConfigError(ctx + ": 'downlink_mid' " + toHexString(c.downlinkMid, 4)
                          + " is not a telemetry MID (CF sends PDUs as telemetry, e.g. 0x092D)");
    }
    const auto number = [&](const char* key, auto& field, std::uint64_t min, std::uint64_t max) {
        if (const auto v = jsonutil::optUInt(j, key, ctx, max)) {
            if (*v < min) {
                throw ConfigError(ctx + ": '" + key + "' must be at least " + std::to_string(min));
            }
            field = static_cast<std::remove_reference_t<decltype(field)>>(*v);
        }
    };
    number("chunk_size", c.chunkSize, 1, 4000);   // CF's incoming buffer; CI limits it further
    number("pdus_per_second", c.pdusPerSecond, 1, 100000);
    number("ack_timeout", c.ackTimeout, 1, 3600);
    number("ack_limit", c.ackLimit, 1, 100);
    number("nak_timeout", c.nakTimeout, 1, 3600);
    number("nak_limit", c.nakLimit, 1, 100);
    number("inactivity_timeout", c.inactivityTimeout, 1, 86400);
    c.downlinkDir = jsonutil::optString(j, "downlink_dir", ctx, c.downlinkDir.string());
    if (c.downlinkDir.empty()) {
        throw ConfigError(ctx + ": 'downlink_dir' must not be empty");
    }
    c.saveIncomplete = jsonutil::optBool(j, "save_incomplete", ctx, c.saveIncomplete);
    return c;
}

PluginSpec parsePlugin(const Json& j, const std::string& ctx)
{
    jsonutil::requireObject(j, ctx);
    PluginSpec spec;
    spec.type = jsonutil::getString(j, "type", ctx);
    spec.options = jsonutil::toOptions(j, ctx);
    return spec;
}

TargetConfig parseTarget(const std::string& name, const Json& j, const std::filesystem::path& baseDir,
                         const std::string& fileCtx)
{
    const std::string ctx = fileCtx + ": target '" + name + "'";
    if (!isTargetName(name)) {
        throw ConfigError(ctx + ": target names may contain letters, digits, '_', '-' and '.'");
    }
    jsonutil::checkKeys(j, {"description", "endian", "catalog", "format", "layers", "transport",
                            "telemetry", "console", "cfdp"}, ctx);

    TargetConfig t;
    t.name = name;
    t.description = jsonutil::optString(j, "description", ctx);

    try {
        t.endian = parseEndian(jsonutil::getString(j, "endian", ctx));
    } catch (const ParseError& e) {
        throw ConfigError(ctx + ": " + e.what());
    }

    const Json& catalog = jsonutil::require(j, "catalog", ctx);
    if (!catalog.is_array() || catalog.empty()) {
        throw ConfigError(ctx + ": 'catalog' must be a non-empty array of file paths");
    }
    t.catalogFiles = parsePaths(catalog, baseDir, ctx + ": 'catalog'");

    t.format = parsePlugin(jsonutil::require(j, "format", ctx), ctx + ": format");
    t.transport = parsePlugin(jsonutil::require(j, "transport", ctx), ctx + ": transport");

    if (const auto it = j.find("layers"); it != j.end()) {
        if (!it->is_array()) {
            throw ConfigError(ctx + ": 'layers' must be an array");
        }
        for (std::size_t i = 0; i < it->size(); ++i) {
            t.layers.push_back(parsePlugin((*it)[i], ctx + ": layers[" + std::to_string(i) + "]"));
        }
    }
    if (const auto it = j.find("telemetry"); it != j.end()) {
        const std::string where = ctx + ": telemetry";
        jsonutil::requireObject(*it, where);
        jsonutil::checkKeys(*it, {"receiver", "catalog"}, where);
        TelemetryConfig tlm;
        tlm.receiver = parsePlugin(jsonutil::require(*it, "receiver", where), where + ": receiver");
        if (const auto files = it->find("catalog"); files != it->end()) {
            if (!files->is_array()) {
                throw ConfigError(where + ": 'catalog' must be an array of file paths");
            }
            tlm.catalogFiles = parsePaths(*files, baseDir, where + ": 'catalog'");
        }
        t.telemetry = std::move(tlm);
    }
    if (const auto it = j.find("console"); it != j.end()) {
        const std::string where = ctx + ": console";
        jsonutil::requireObject(*it, where);
        jsonutil::checkKeys(*it, {"receiver"}, where);
        ConsoleConfig console;
        console.receiver = parsePlugin(jsonutil::require(*it, "receiver", where), where + ": receiver");
        // The udp receiver's default port is telemetry's (5011): make it explicit.
        if (console.receiver.options.count("port") == 0) {
            throw ConfigError(where + ": receiver needs a 'port'");
        }
        if (t.telemetry && t.telemetry->receiver.type == console.receiver.type) {
            const Options& tlm = t.telemetry->receiver.options;
            const std::string tlmPort = tlm.count("port") != 0 ? tlm.at("port") : "5011";
            if (tlmPort == console.receiver.options.at("port")) {
                throw ConfigError(where + ": receiver uses the telemetry port " + tlmPort);
            }
        }
        t.console = std::move(console);
    }
    if (const auto it = j.find("cfdp"); it != j.end()) {
        if (!t.telemetry) {
            throw ConfigError(ctx + ": 'cfdp' needs a 'telemetry' block (CF's PDUs arrive as telemetry)");
        }
        t.cfdp = parseCfdp(*it, ctx + ": cfdp");
    }
    return t;
}

} // namespace

const TargetConfig* TargetsFile::find(const std::string& name) const
{
    for (const auto& t : targets) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

TargetsFile loadTargetsFile(const std::filesystem::path& file)
{
    const Json root = jsonutil::readFile(file);
    const std::string ctx = file.string();
    jsonutil::checkKeys(root, {"default_target", "targets"}, ctx);

    TargetsFile out;
    out.file = file;
    const std::filesystem::path baseDir = std::filesystem::absolute(file).parent_path();

    const Json& targets = jsonutil::require(root, "targets", ctx);
    if (!targets.is_object() || targets.empty()) {
        throw ConfigError(ctx + ": 'targets' must be a non-empty object");
    }
    for (const auto& item : targets.items()) {
        out.targets.push_back(parseTarget(item.key(), item.value(), baseDir, ctx));
    }

    out.defaultTarget = jsonutil::optString(root, "default_target", ctx, out.targets.front().name);
    if (out.find(out.defaultTarget) == nullptr) {
        throw ConfigError(ctx + ": default_target '" + out.defaultTarget + "' is not defined");
    }
    return out;
}

const TargetConfig& selectTarget(const TargetsFile& targets, const std::optional<std::string>& name)
{
    const std::string wanted = name.value_or(targets.defaultTarget);
    if (const TargetConfig* t = targets.find(wanted)) {
        return *t;
    }
    std::vector<std::string> names;
    for (const auto& t : targets.targets) {
        names.push_back(t.name);
    }
    throw ConfigError("unknown target '" + wanted + "' in " + targets.file.string()
                      + " (available: " + join(names, ", ") + ")");
}

} // namespace mcs
