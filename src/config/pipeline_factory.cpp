#include "config/pipeline_factory.hpp"

#include "core/errors.hpp"

namespace mcs {

std::unique_ptr<Pipeline> buildPipeline(const TargetConfig& target, const PluginRegistry& registry,
                                        bool forceDryRun)
{
    try {
        auto format = registry.createFormat(target.format.type, target.endian, target.format.options);

        std::vector<std::unique_ptr<IFramingLayer>> layers;
        for (const auto& spec : target.layers) {
            layers.push_back(registry.createLayer(spec.type, target.endian, spec.options));
        }

        auto transport = forceDryRun
                             ? registry.createTransport("dryrun", {})
                             : registry.createTransport(target.transport.type, target.transport.options);

        return std::make_unique<Pipeline>(std::move(format), std::move(layers), std::move(transport));
    } catch (const ConfigError& e) {
        throw ConfigError("target '" + target.name + "': " + e.what());
    } catch (const TransportError& e) {
        throw TransportError("target '" + target.name + "': " + e.what());
    }
}

std::unique_ptr<IReceiver> buildReceiver(const TargetConfig& target, const PluginRegistry& registry,
                                         std::optional<std::uint16_t> portOverride)
{
    if (!target.telemetry) {
        throw ConfigError("target '" + target.name + "' has no telemetry block");
    }
    Options options = target.telemetry->receiver.options;
    if (portOverride) {
        options["port"] = std::to_string(*portOverride);
    }
    try {
        return registry.createReceiver(target.telemetry->receiver.type, options);
    } catch (const ConfigError& e) {
        throw ConfigError("target '" + target.name + "': telemetry: " + e.what());
    } catch (const TransportError& e) {
        throw TransportError("target '" + target.name + "': telemetry: " + e.what());
    }
}

} // namespace mcs
