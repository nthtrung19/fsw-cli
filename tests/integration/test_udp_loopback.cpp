// Integration: real UDP sockets on the loopback interface. A receiver socket
// stands in for cFS ci_lab; packets go through the same registry, pipeline
// and service code the executable uses.

#include "catalog/catalog.hpp"
#include "config/catalog_loader.hpp"
#include "config/pipeline_factory.hpp"
#include "core/errors.hpp"
#include "core/hex.hpp"
#include "protocol/ccsds_v1_format.hpp"
#include "registry/plugin_registry.hpp"
#include "service/command_service.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>

using namespace fswcli;

namespace {

// Bound UDP socket on 127.0.0.1 with an ephemeral port.
class Receiver {
public:
    Receiver()
    {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            throw std::runtime_error("cannot bind receiver");
        }
        socklen_t len = sizeof addr;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        timeval timeout{2, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    }
    ~Receiver() { ::close(fd_); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::uint16_t port() const { return port_; }

    Bytes receive()
    {
        Bytes buffer(70000);
        const auto n = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (n < 0) {
            throw std::runtime_error("no datagram received within 2 s");
        }
        buffer.resize(static_cast<std::size_t>(n));
        return buffer;
    }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

TargetConfig udpTarget(std::uint16_t port)
{
    TargetConfig t;
    t.name = "loopback";
    t.endian = Endian::Little;
    t.format = {"ccsds_v1", {}};
    t.transport = {"udp", {{"host", "127.0.0.1"}, {"port", std::to_string(port)}}};
    return t;
}

} // namespace

TEST(UdpLoopback, OneDatagramPerPacket)
{
    Receiver rx;
    auto pipeline = buildPipeline(udpTarget(rx.port()), builtinPlugins(), false);
    EXPECT_EQ(pipeline->destination(), "udp://127.0.0.1:" + std::to_string(rx.port()));

    pipeline->send({0x194B, 0, {}});
    pipeline->send({0x194B, 2, {0x01, 0x00, 0x00, 0x00}});

    EXPECT_EQ(toHex(rx.receive()), "19 4B C0 00 00 01 6C 00");
    const Bytes second = rx.receive();
    EXPECT_EQ(second.size(), 12U);
    EXPECT_EQ(CcsdsV1Format::computeChecksum(second), 0);
}

TEST(UdpLoopback, FullServiceWithShippedCatalog)
{
    Receiver rx;
    const CommandCatalog catalog = loadCatalog({std::string(FSWCLI_SOURCE_DIR) + "/config/catalog/ds.json"});
    auto pipeline = buildPipeline(udpTarget(rx.port()), builtinPlugins(), false);
    CommandService service(catalog, PayloadEncoder(Endian::Little), *pipeline, nullptr, "loopback");

    std::ostringstream out;
    ASSERT_TRUE(service.execute("ds", "set_app_state", {"enable"}, out)) << out.str();
    EXPECT_EQ(toHex(rx.receive()), "19 4B C0 00 00 05 6B 02 01 00 00 00");

    // An invalid command must not put anything on the wire.
    EXPECT_FALSE(service.execute("ds", "set_app_state", {"maybe"}, out));
    ASSERT_TRUE(service.execute("ds", "noop", {}, out));
    const Bytes next = rx.receive();
    EXPECT_EQ(next[7], 0);   // the next datagram is the noop, not the rejected command
}

TEST(UdpLoopback, LocalhostNameResolves)
{
    Receiver rx;
    auto transport = builtinPlugins().createTransport(
        "udp", {{"host", "localhost"}, {"port", std::to_string(rx.port())}});
    // "localhost" may resolve to ::1 first on some systems; only check it does not throw
    // and that sending works without error.
    EXPECT_NO_THROW(transport->send({0x01}));
}
