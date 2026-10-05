#include "config/config_paths.hpp"

#include "core/errors.hpp"

#include <cstdlib>
#include <system_error>

namespace fswcli {

namespace {

std::optional<std::filesystem::path> executableDir()
{
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::nullopt;
    }
    return exe.parent_path();
}

} // namespace

std::vector<std::filesystem::path> targetsFileCandidates(const std::optional<std::string>& explicitPath)
{
    if (explicitPath) {
        return {std::filesystem::path(*explicitPath)};
    }

    std::vector<std::filesystem::path> out;
    if (const char* env = std::getenv("FSWCLI_CONFIG"); env != nullptr && *env != '\0') {
        out.emplace_back(env);
    }
    out.emplace_back(std::filesystem::path("config") / "targets.json");
    if (const auto dir = executableDir()) {
        out.push_back((*dir / ".." / "config" / "targets.json").lexically_normal());
        out.push_back((*dir / ".." / ".." / "config" / "targets.json").lexically_normal());
        out.push_back((*dir / ".." / "share" / "fswcli" / "targets.json").lexically_normal());
    }
    return out;
}

std::filesystem::path findTargetsFile(const std::optional<std::string>& explicitPath)
{
    const auto candidates = targetsFileCandidates(explicitPath);
    std::string searched;
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) {
            return c;
        }
        searched += "\n  " + c.string();
    }
    if (explicitPath) {
        throw ConfigError("config file not found: " + *explicitPath);
    }
    throw ConfigError("no targets.json found; searched:" + searched
                      + "\n  (use --config FILE or set FSWCLI_CONFIG)");
}

} // namespace fswcli
