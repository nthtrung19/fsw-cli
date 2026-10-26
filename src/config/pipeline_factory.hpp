#pragma once

#include "config/target_config.hpp"
#include "pipeline/pipeline.hpp"
#include "registry/plugin_registry.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace mcs {

// Creates the target's format, layers and transport through the registry.
// With forceDryRun the configured transport is replaced by "dryrun".
// Throws ConfigError (prefixed with the target name) or TransportError.
std::unique_ptr<Pipeline> buildPipeline(const TargetConfig& target, const PluginRegistry& registry,
                                        bool forceDryRun);

// Creates the receiver of the target's telemetry block; `portOverride`
// replaces its "port" option. Throws ConfigError if the target has no
// telemetry block or the options are bad, TransportError if it cannot bind.
std::unique_ptr<IReceiver> buildReceiver(const TargetConfig& target, const PluginRegistry& registry,
                                         std::optional<std::uint16_t> portOverride);

// Creates the receiver of the target's console block. Throws ConfigError if
// the target has no console block or the options are bad, TransportError if
// it cannot bind.
std::unique_ptr<IReceiver> buildConsoleReceiver(const TargetConfig& target, const PluginRegistry& registry);

} // namespace mcs
