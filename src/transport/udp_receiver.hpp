#pragma once

#include "transport/receiver.hpp"

#include <cstdint>
#include <string>

namespace mcs {

// Receives telemetry datagrams on a bound UDP port (one datagram = one
// packet, as sent by cFS TO / TO_LAB).
class UdpReceiver : public IReceiver {
public:
    // Port 0 binds an ephemeral port (tests); see port().
    // Throws TransportError if the address cannot be resolved or bound.
    UdpReceiver(std::string host, std::uint16_t port);
    ~UdpReceiver() override;

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // One thread at a time (the receiver thread of its service): the
    // datagram is read into a buffer allocated once.
    std::optional<Bytes> receive(std::chrono::milliseconds timeout) override;
    std::string describe() const override;

    // The bound port (useful when constructed with port 0).
    std::uint16_t port() const { return port_; }

private:
    std::string host_;
    std::uint16_t port_;
    int fd_ = -1;
    Bytes buffer_;   // kMaxDatagram bytes, reused by receive()
};

} // namespace mcs
