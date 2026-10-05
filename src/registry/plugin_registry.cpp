#include "registry/plugin_registry.hpp"

#include "core/errors.hpp"
#include "core/text.hpp"

namespace fswcli {

namespace {

template <typename Map>
std::vector<std::string> keysOf(const Map& map)
{
    std::vector<std::string> out;
    for (const auto& entry : map) {
        out.push_back(entry.first);
    }
    return out;
}

template <typename Map, typename Factory>
void addUnique(Map& map, const std::string& kind, const std::string& type, Factory factory)
{
    if (type.empty() || !factory) {
        throw ConfigError("cannot register an empty " + kind + " plug-in");
    }
    if (!map.emplace(type, std::move(factory)).second) {
        throw ConfigError(kind + " '" + type + "' is registered twice");
    }
}

template <typename Map>
const typename Map::mapped_type& lookup(const Map& map, const std::string& kind,
                                        const std::string& type)
{
    const auto it = map.find(type);
    if (it == map.end()) {
        throw ConfigError("unknown " + kind + " '" + type + "' (known: " + join(keysOf(map), ", ")
                          + ")");
    }
    return it->second;
}

} // namespace

void PluginRegistry::addFormat(const std::string& type, FormatFactory factory)
{
    addUnique(formats_, "format", type, std::move(factory));
}

void PluginRegistry::addLayer(const std::string& type, LayerFactory factory)
{
    addUnique(layers_, "layer", type, std::move(factory));
}

void PluginRegistry::addTransport(const std::string& type, TransportFactory factory)
{
    addUnique(transports_, "transport", type, std::move(factory));
}

std::unique_ptr<IPacketFormat> PluginRegistry::createFormat(const std::string& type, Endian endian,
                                                            const Options& opts) const
{
    return lookup(formats_, "format", type)(endian, opts);
}

std::unique_ptr<IFramingLayer> PluginRegistry::createLayer(const std::string& type, Endian endian,
                                                           const Options& opts) const
{
    return lookup(layers_, "layer", type)(endian, opts);
}

std::unique_ptr<ITransport> PluginRegistry::createTransport(const std::string& type,
                                                            const Options& opts) const
{
    return lookup(transports_, "transport", type)(opts);
}

std::vector<std::string> PluginRegistry::formatNames() const { return keysOf(formats_); }
std::vector<std::string> PluginRegistry::layerNames() const { return keysOf(layers_); }
std::vector<std::string> PluginRegistry::transportNames() const { return keysOf(transports_); }

} // namespace fswcli
