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
#include "config/tlm_catalog_loader.hpp"
#include "console/console_service.hpp"
#include "service/command_service.hpp"
#include "telemetry/telemetry_service.hpp"
#include "transport/udp_receiver.hpp"
#include "unit/test_helpers.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <sstream>
#include <thread>

using namespace mcs;

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
    const CommandCatalog catalog = loadCatalog({std::string(mcs_SOURCE_DIR) + "/config/telecommand/ds.json"});
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

// ---- telemetry: UdpReceiver + TelemetryService --------------------------------

TEST(UdpTelemetry, ReceiverThreadDecodesEvents)
{
    const TelemetryCatalog catalog =
        loadTelemetryCatalog({std::string(mcs_SOURCE_DIR) + "/config/telemetry/evs.json"});
    TlmStore store(catalog, Endian::Little);

    auto receiver = std::make_unique<UdpReceiver>("127.0.0.1", 0);
    const std::uint16_t port = receiver->port();
    ASSERT_NE(port, 0);
    EXPECT_EQ(receiver->describe(), "udp://127.0.0.1:" + std::to_string(port));

    TelemetryService service(std::move(receiver), store);
    std::atomic<int> updates{0};
    service.start([&updates](bool arrived) {
        if (arrived) {
            ++updates;
        }
    });

    auto sender = builtinPlugins().createTransport(
        "udp", {{"host", "127.0.0.1"}, {"port", std::to_string(port)}});
    sender->send(test::tlmPacket(0x0808, 0, test::evsLongEventPayload("DS", 12, 2, "No-op")));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (updates == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    service.stop();

    const TlmSnapshot s = store.snapshot();
    ASSERT_EQ(s.events.size(), 1U);
    EXPECT_EQ(s.events[0].app, "DS");
    EXPECT_EQ(s.events[0].message, "No-op");
    EXPECT_TRUE(service.lastError().empty());
}

TEST(UdpTelemetry, RegistryReceiverAndStopIsPrompt)
{
    TargetConfig t = udpTarget(1);
    t.telemetry = TelemetryConfig{{"udp", {{"host", "127.0.0.1"}}}, {}};
    const std::uint16_t freePort = UdpReceiver("127.0.0.1", 0).port();   // released again here
    auto receiver = buildReceiver(t, builtinPlugins(), freePort);
    EXPECT_EQ(receiver->describe(), "udp://127.0.0.1:" + std::to_string(freePort));

    const TelemetryCatalog catalog;
    TlmStore store(catalog, Endian::Little);
    TelemetryService service(std::move(receiver), store);
    service.start(nullptr);
    const auto begin = std::chrono::steady_clock::now();
    service.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500));
}

TEST(UdpConsole, ServiceRebuildsLinesFromDatagrams)
{
    TargetConfig t = udpTarget(1);
    const std::uint16_t freePort = UdpReceiver("127.0.0.1", 0).port();   // released again here
    t.console = ConsoleConfig{{"udp", {{"host", "127.0.0.1"}, {"port", std::to_string(freePort)}}}};
    auto receiver = buildConsoleReceiver(t, builtinPlugins());
    EXPECT_EQ(receiver->describe(), "udp://127.0.0.1:" + std::to_string(freePort));

    ConsoleStore store;
    ConsoleService service(std::move(receiver), store);
    std::atomic<int> datagrams{0};
    service.start([&datagrams](bool arrived) {
        if (arrived) {
            ++datagrams;
        }
    });

    auto sender = builtinPlugins().createTransport(
        "udp", {{"host", "127.0.0.1"}, {"port", std::to_string(freePort)}});
    for (const std::string part : {"hel", "lo\r\nwor", "ld\nSHLL # "}) {
        sender->send(Bytes(part.begin(), part.end()));
    }

    // The prompt has no line end: it shows after the pause.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (store.snapshot().lines.size() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto begin = std::chrono::steady_clock::now();
    service.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500));

    const ConsoleSnapshot s = store.snapshot();
    ASSERT_EQ(s.lines.size(), 3U);
    EXPECT_EQ(s.lines[0].text, "hello");
    EXPECT_EQ(s.lines[1].text, "world");
    EXPECT_EQ(s.lines[2].text, "SHLL # ");
    EXPECT_EQ(datagrams, 3);
    EXPECT_TRUE(service.lastError().empty());
}
