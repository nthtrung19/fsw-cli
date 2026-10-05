#include "pipeline/pipeline.hpp"

#include "core/errors.hpp"

namespace fswcli {

Pipeline::Pipeline(std::unique_ptr<IPacketFormat> format,
                   std::vector<std::unique_ptr<IFramingLayer>> layers,
                   std::unique_ptr<ITransport> transport)
    : format_(std::move(format)), layers_(std::move(layers)), transport_(std::move(transport))
{
    if (!format_ || !transport_) {
        throw ConfigError("pipeline needs a packet format and a transport");
    }
    for (const auto& layer : layers_) {
        if (!layer) {
            throw ConfigError("pipeline layer is null");
        }
    }
}

Pipeline::Result Pipeline::send(const CommandMessage& message)
{
    BuiltPacket built = format_->build(message);
    return wrapAndSend(std::move(built.bytes), std::move(built.note));
}

Pipeline::Result Pipeline::sendRaw(const Bytes& packet)
{
    return wrapAndSend(packet, "raw");
}

Pipeline::Result Pipeline::wrapAndSend(Bytes packet, std::string note)
{
    Bytes wire = packet;
    for (const auto& layer : layers_) {
        wire = layer->wrap(wire);
    }
    transport_->send(wire);
    return {std::move(packet), std::move(wire), std::move(note)};
}

std::string Pipeline::describe() const
{
    std::string out = format_->describe();
    for (const auto& layer : layers_) {
        out += " -> " + layer->describe();
    }
    return out + " -> " + transport_->describe();
}

} // namespace fswcli
