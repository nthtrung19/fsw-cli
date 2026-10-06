#include "config/target_config.hpp"

#include "config/json_util.hpp"
#include "core/errors.hpp"
#include "core/text.hpp"

#include <cctype>

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
    jsonutil::checkKeys(j, {"description", "endian", "catalog", "format", "layers", "transport"}, ctx);

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
    for (const auto& entry : catalog) {
        if (!entry.is_string()) {
            throw ConfigError(ctx + ": 'catalog' entries must be strings");
        }
        const std::filesystem::path p(entry.get<std::string>());
        t.catalogFiles.push_back(p.is_absolute() ? p : (baseDir / p).lexically_normal());
    }

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
