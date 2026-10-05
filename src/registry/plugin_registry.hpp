#pragma once

#include "core/bytes.hpp"
#include "core/options.hpp"
#include "protocol/framing_layer.hpp"
#include "protocol/packet_format.hpp"
#include "transport/transport.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fswcli {

// Maps plug-in type names ("ccsds_v1", "udp", ...) to factories.
// The configuration refers to plug-ins only by these names, so adding a
// protocol or transport never changes existing code: write the class, add one
// registration in builtin_plugins.cpp.
class PluginRegistry {
public:
    using FormatFactory    = std::function<std::unique_ptr<IPacketFormat>(Endian, const Options&)>;
    using LayerFactory     = std::function<std::unique_ptr<IFramingLayer>(Endian, const Options&)>;
    using TransportFactory = std::function<std::unique_ptr<ITransport>(const Options&)>;

    // Throws ConfigError if the name is already registered.
    void addFormat(const std::string& type, FormatFactory factory);
    void addLayer(const std::string& type, LayerFactory factory);
    void addTransport(const std::string& type, TransportFactory factory);

    // Throw ConfigError("unknown format 'x' (known: a, b)") for unknown names;
    // factories throw ConfigError for bad options.
    std::unique_ptr<IPacketFormat> createFormat(const std::string& type, Endian endian,
                                                const Options& opts) const;
    std::unique_ptr<IFramingLayer> createLayer(const std::string& type, Endian endian,
                                               const Options& opts) const;
    std::unique_ptr<ITransport> createTransport(const std::string& type, const Options& opts) const;

    std::vector<std::string> formatNames() const;
    std::vector<std::string> layerNames() const;
    std::vector<std::string> transportNames() const;

private:
    std::map<std::string, FormatFactory> formats_;
    std::map<std::string, LayerFactory> layers_;
    std::map<std::string, TransportFactory> transports_;
};

// Registry with every plug-in shipped in fswcli (ccsds_v1, udp, dryrun).
PluginRegistry builtinPlugins();

} // namespace fswcli
