#pragma once

#include "protocol/framing_layer.hpp"
#include "protocol/packet_format.hpp"
#include "transport/transport.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fswcli {

// The protocol stack of one target:  format -> layer 1 -> ... -> layer N -> transport.
class Pipeline {
public:
    struct Result {
        Bytes packet;      // output of the format (before layers)
        Bytes wire;        // bytes handed to the transport
        std::string note;  // format note, e.g. "apid=0x14B seq=0"
    };

    // Throws ConfigError if format or transport is null.
    Pipeline(std::unique_ptr<IPacketFormat> format,
             std::vector<std::unique_ptr<IFramingLayer>> layers,
             std::unique_ptr<ITransport> transport);

    // Build, wrap and send. Throws EncodeError / TransportError.
    Result send(const CommandMessage& message);

    // Send pre-built packet bytes: skips the format, still applies layers.
    Result sendRaw(const Bytes& packet);

    // "ccsds_v1 (little-endian) -> udp://127.0.0.1:1234"
    std::string describe() const;

    // Transport description only, e.g. "udp://127.0.0.1:1234".
    std::string destination() const { return transport_->describe(); }

    bool isDryRun() const { return transport_->isDryRun(); }
    bool hasLayers() const { return !layers_.empty(); }

private:
    Result wrapAndSend(Bytes packet, std::string note);

    std::unique_ptr<IPacketFormat> format_;
    std::vector<std::unique_ptr<IFramingLayer>> layers_;
    std::unique_ptr<ITransport> transport_;
};

} // namespace fswcli
