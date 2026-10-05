#pragma once

#include "config/target_config.hpp"
#include "pipeline/pipeline.hpp"
#include "registry/plugin_registry.hpp"

#include <memory>

namespace fswcli {

// Creates the target's format, layers and transport through the registry.
// With forceDryRun the configured transport is replaced by "dryrun".
// Throws ConfigError (prefixed with the target name) or TransportError.
std::unique_ptr<Pipeline> buildPipeline(const TargetConfig& target, const PluginRegistry& registry,
                                        bool forceDryRun);

} // namespace fswcli
