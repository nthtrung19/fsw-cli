#include "transport/udp_transport.hpp"

#include "core/errors.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace mcs {

UdpTransport::UdpTransport(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port_);
    const int rc = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &results);
    if (rc != 0) {
        throw TransportError("udp: cannot resolve host '" + host_ + "': " + ::gai_strerror(rc));
    }

    std::string lastError = "no usable address";
    for (const addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        fd_ = fd;
        std::memcpy(&address_, ai->ai_addr, ai->ai_addrlen);
        addressLength_ = ai->ai_addrlen;
        break;
    }
    ::freeaddrinfo(results);

    if (fd_ < 0) {
        throw TransportError("udp: cannot open socket for " + describe() + ": " + lastError);
    }
}

UdpTransport::~UdpTransport()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void UdpTransport::send(const Bytes& data)
{
    const auto sent = ::sendto(fd_, data.data(), data.size(), 0,
                               reinterpret_cast<const sockaddr*>(&address_), addressLength_);
    if (sent < 0) {
        throw TransportError("udp: send to " + describe() + " failed: " + std::strerror(errno));
    }
    if (static_cast<std::size_t>(sent) != data.size()) {
        throw TransportError("udp: short send to " + describe() + " (" + std::to_string(sent)
                             + " of " + std::to_string(data.size()) + " bytes)");
    }
}

std::string UdpTransport::describe() const
{
    const bool ipv6Literal = host_.find(':') != std::string::npos;
    return "udp://" + (ipv6Literal ? "[" + host_ + "]" : host_) + ":" + std::to_string(port_);
}

} // namespace mcs
