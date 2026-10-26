#include "transport/udp_receiver.hpp"

#include "core/errors.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace mcs {

namespace {

constexpr std::size_t kMaxDatagram = 65536;

std::uint16_t boundPort(int fd)
{
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
}

} // namespace

UdpReceiver::UdpReceiver(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port_);
    const int rc = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &results);
    if (rc != 0) {
        throw TransportError("udp receiver: cannot resolve '" + host_ + "': " + ::gai_strerror(rc));
    }

    std::string lastError = "no usable address";
    for (const addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        // No SO_REUSEADDR: for UDP it only lets a second socket (another mcs,
        // `nc -ul`) bind the same port, and then one of them silently gets
        // nothing. Without it the second bind fails with "Address already in use".
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            lastError = std::strerror(errno);
            ::close(fd);
            continue;
        }
        fd_ = fd;
        break;
    }
    ::freeaddrinfo(results);

    if (fd_ < 0) {
        throw TransportError("udp receiver: cannot bind " + describe() + ": " + lastError);
    }
    port_ = boundPort(fd_);
}

UdpReceiver::~UdpReceiver()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::optional<Bytes> UdpReceiver::receive(std::chrono::milliseconds timeout)
{
    pollfd p{};
    p.fd = fd_;
    p.events = POLLIN;
    const int ready = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (ready < 0) {
        if (errno == EINTR) {
            return std::nullopt;
        }
        throw TransportError("udp receiver: poll failed: " + std::string(std::strerror(errno)));
    }
    if (ready == 0) {
        return std::nullopt;
    }

    Bytes buffer(kMaxDatagram);
    const auto n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return std::nullopt;
        }
        throw TransportError("udp receiver: receive on " + describe() + " failed: "
                             + std::strerror(errno));
    }
    buffer.resize(static_cast<std::size_t>(n));
    return buffer;
}

std::string UdpReceiver::describe() const
{
    const bool ipv6Literal = host_.find(':') != std::string::npos;
    return "udp://" + (ipv6Literal ? "[" + host_ + "]" : host_) + ":" + std::to_string(port_);
}

} // namespace mcs
