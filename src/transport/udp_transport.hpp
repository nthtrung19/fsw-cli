#pragma once

#include "transport/transport.hpp"

#include <cstdint>
#include <string>
#include <sys/socket.h>

namespace mcs {

// Sends each packet as one UDP datagram (unconnected socket, fire-and-forget,
// as cFS ci_lab expects). The host name is resolved once, at construction.
class UdpTransport : public ITransport {
public:
    // Throws TransportError if the host cannot be resolved or no socket can be opened.
    UdpTransport(std::string host, std::uint16_t port);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void send(const Bytes& data) override;
    std::string describe() const override;

private:
    std::string host_;
    std::uint16_t port_;
    int fd_ = -1;
    sockaddr_storage address_{};
    socklen_t addressLength_ = 0;
};

} // namespace mcs
