// The one place where plug-in implementations are tied to their names.
//
// To add a plug-in:
//   1. implement IPacketFormat, IFramingLayer or ITransport;
//   2. add one registration below, validating its options;
//   3. refer to it by name in config/targets.json.
// Nothing else in the code base changes.

#include "registry/plugin_registry.hpp"

#include "protocol/ccsds_v1_format.hpp"
#include "transport/dry_run_transport.hpp"
#include "transport/udp_transport.hpp"

#include <memory>

namespace mcs {

PluginRegistry builtinPlugins()
{
    PluginRegistry registry;

    // ---- packet formats ----------------------------------------------------
    // ccsds_v1: options  checksum = true|false (default true)
    registry.addFormat("ccsds_v1", [](Endian endian, const Options& opts) {
        const std::string owner = "format 'ccsds_v1'";
        requireKnownKeys(opts, {"checksum"}, owner);
        return std::make_unique<CcsdsV1Format>(endian, optBool(opts, "checksum", true, owner));
    });

    // ---- framing layers ----------------------------------------------------
    // (none yet; e.g. registry.addLayer("csp", ...))

    // ---- transports ----------------------------------------------------------
    // udp: options  host (default 127.0.0.1), port (required, 1..65535)
    registry.addTransport("udp", [](const Options& opts) {
        const std::string owner = "transport 'udp'";
        requireKnownKeys(opts, {"host", "port"}, owner);
        const auto port = requireUInt(opts, "port", 1, 65535, owner);
        return std::make_unique<UdpTransport>(optString(opts, "host", "127.0.0.1"),
                                              static_cast<std::uint16_t>(port));
    });

    // dryrun: no options
    registry.addTransport("dryrun", [](const Options& opts) {
        requireKnownKeys(opts, {}, "transport 'dryrun'");
        return std::make_unique<DryRunTransport>();
    });

    return registry;
}

} // namespace mcs
