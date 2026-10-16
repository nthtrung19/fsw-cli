// Two CFDP entities talking to each other: mcs's ground engine (0.23) in the
// test process and a second GsfcEngine playing CF (0.24) in a forked child
// (the library allows one engine per process). They exchange PDUs over a
// SOCK_SEQPACKET socket pair; either side can drop PDUs to exercise class 2
// retransmission (NAK, ACK timer).

#include "cfdp/gsfc_engine.hpp"
#include "cfdp/pdu_info.hpp"

#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace mcs;
using namespace mcs::cfdp;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string pattern(std::size_t size, unsigned seed)
{
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>((i * 13 + seed) & 0xFF);
    }
    return s;
}

fs::path writeBinary(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

Config entityConfig(const std::string& self, const std::string& peer, const fs::path& downlinkDir)
{
    Config c;
    c.localEntity = self;
    c.remoteEntity = peer;
    c.downlinkDir = downlinkDir;
    c.chunkSize = 512;
    c.pdusPerSecond = 2000;
    c.ackTimeout = 1;
    c.ackLimit = 4;
    c.nakTimeout = 1;
    c.nakLimit = 6;
    c.inactivityTimeout = 6;
    return c;
}

// Drops FileData PDUs the first time each offset is seen, every `nth` one;
// also the first EOF if asked. Retransmissions always get through.
struct Dropper {
    explicit Dropper(unsigned everyNth = 0, bool dropFirstEof = false) : nth(everyNth), firstEof(dropFirstEof) {}

    unsigned nth;
    bool firstEof;
    unsigned seen = 0;
    std::set<std::uint32_t> dropped;
    bool eofDropped = false;

    bool operator()(const PduInfo& info)
    {
        if (info.kind == PduKind::Eof && firstEof && !eofDropped) {
            eofDropped = true;
            return true;
        }
        if (info.kind != PduKind::FileData || nth == 0 || dropped.count(info.offset) != 0) {
            return false;
        }
        if (++seen % nth == 0) {
            dropped.insert(info.offset);
            return true;
        }
        return false;
    }
};

// One entity: an engine plus its end of the socket pair.
class Entity {
public:
    Entity(int fd, Config config, Dropper drop = Dropper())
        : fd_(fd), drop_(drop),
          engine_(std::move(config),
                  [this](const Bytes& pdu, const PduInfo&) {
                      ::send(fd_, pdu.data(), pdu.size(), MSG_NOSIGNAL);
                  },
                  [this](EngineEvent e, const TransferStatus& s) {
                      if (e == EngineEvent::Finished) {
                          finished_.push_back(s);
                      }
                  },
                  {})
    {
    }

    GsfcEngine& engine() { return engine_; }
    const std::vector<TransferStatus>& finished() const { return finished_; }

    // Receives what is there (waiting up to `wait`), then cycles the engine.
    // False once the other side has closed its end.
    bool pump(std::chrono::milliseconds wait = 5ms)
    {
        pollfd p{fd_, POLLIN, 0};
        bool open = true;
        if (::poll(&p, 1, static_cast<int>(wait.count())) > 0) {
            for (;;) {
                std::uint8_t buffer[9000];
                const ssize_t n = ::recv(fd_, buffer, sizeof buffer, MSG_DONTWAIT);
                if (n == 0) {
                    open = false;
                    break;
                }
                if (n < 0) {
                    break;
                }
                Bytes pdu(buffer, buffer + n);
                const auto info = inspectPdu(pdu);
                if (info && drop_(*info)) {
                    continue;
                }
                engine_.givePdu(pdu);
            }
        }
        engine_.cycle();
        return open;
    }

    // Pumps until `done` or `timeout`.
    bool pumpUntil(const std::function<bool()>& done, std::chrono::seconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (done()) {
                return true;
            }
            if (!pump()) {
                return done();
            }
        }
        return done();
    }

    // Keeps answering for `time` (lets the last ACK/FIN reach the other side).
    void linger(std::chrono::milliseconds time)
    {
        const auto end = Clock::now() + time;
        while (Clock::now() < end && pump()) {
        }
    }

private:
    int fd_;
    Dropper drop_;
    std::vector<TransferStatus> finished_;
    GsfcEngine engine_;
};

// Runs `child(fd)` in a forked process; its return value is the exit code.
class Child {
public:
    explicit Child(const std::function<int(int fd)>& body)
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
            throw std::runtime_error("socketpair failed");
        }
        pid_ = ::fork();
        if (pid_ == 0) {
            ::close(sv[0]);
            int code = 90;
            try {
                code = body(sv[1]);
            } catch (...) {
                code = 91;
            }
            ::_exit(code);
        }
        ::close(sv[1]);
        fd_ = sv[0];
    }
    ~Child()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }
    int fd() const { return fd_; }

    // Closes our end (the child sees it and finishes), then waits for its code.
    int finish(std::chrono::seconds timeout = 15s)
    {
        ::close(fd_);
        fd_ = -1;
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
            }
            std::this_thread::sleep_for(10ms);
        }
        return -1;
    }

private:
    pid_t pid_ = -1;
    int fd_ = -1;
};

// Child side, playing CF: runs until the parent closes the socket, then
// returns 0 if exactly one transfer finished in `expected` and, for a
// receive, the stored file equals `content`.
int flightUntilClosed(Entity& flight, State expected, const std::optional<fs::path>& storedFile,
                      const std::string& content)
{
    const auto deadline = Clock::now() + 30s;
    while (Clock::now() < deadline && flight.pump()) {
    }
    if (flight.finished().size() != 1) {
        return 10 + static_cast<int>(flight.finished().size());
    }
    if (flight.finished().front().state != expected) {
        return 20 + static_cast<int>(flight.finished().front().state);
    }
    if (storedFile && readFile(*storedFile) != content) {
        return 30;
    }
    return 0;
}

class TwoEntities : public ::testing::Test {
protected:
    test::TempDir tmp;
};

} // namespace

TEST_F(TwoEntities, Class2PutRecoversLostFileData)
{
    const std::string content = pattern(40000, 1);   // 79 FileData PDUs
    const fs::path src = writeBinary(tmp.path() / "src" / "test.bin", content);
    const fs::path flightDir = tmp.path() / "flight";

    Child child([&](int fd) {
        Entity flight(fd, entityConfig("0.24", "0.23", flightDir), Dropper{7});
        return flightUntilClosed(flight, State::Done, flightDir / "cf" / "test.bin", content);
    });
    Entity ground(child.fd(), entityConfig("0.23", "0.24", tmp.path() / "ground"));
    std::string error;
    ASSERT_TRUE(ground.engine().put({src, "/cf/test.bin", 2}, error)) << error;
    ASSERT_TRUE(ground.pumpUntil([&] { return !ground.finished().empty(); }, 30s));
    ground.linger(300ms);

    const TransferStatus& up = ground.finished().front();
    EXPECT_EQ(up.state, State::Done) << up.condition;
    EXPECT_EQ(up.cls, 2);
    EXPECT_GE(up.received.nak, 1u);          // CF (child) asked for the dropped data
    EXPECT_GT(up.bytesResent, 0u);
    EXPECT_EQ(up.received.fin, 1u);
    EXPECT_EQ(up.sent.ack, 1u);              // ACK(FIN)
    EXPECT_EQ(child.finish(), 0);
}

TEST_F(TwoEntities, Class2GetRecoversLostFileDataAndEof)
{
    const std::string content = pattern(30000, 2);
    const fs::path flightSrc = writeBinary(tmp.path() / "flight-src" / "hk.dat", content);
    const fs::path groundDir = tmp.path() / "downlink";

    Child child([&](int fd) {
        Entity flight(fd, entityConfig("0.24", "0.23", tmp.path() / "flight"));
        std::string error;
        if (!flight.engine().put({flightSrc, "/cf/log/hk.dat", 2}, error)) {
            return 5;
        }
        return flightUntilClosed(flight, State::Done, std::nullopt, content);
    });
    Entity ground(child.fd(), entityConfig("0.23", "0.24", groundDir), Dropper{5, true});
    ASSERT_TRUE(ground.pumpUntil([&] { return !ground.finished().empty(); }, 30s));
    ground.linger(300ms);

    const TransferStatus& down = ground.finished().front();
    EXPECT_EQ(down.state, State::Done) << down.condition;
    EXPECT_EQ(down.direction, Direction::Downlink);
    EXPECT_GE(down.sent.nak, 1u);
    EXPECT_EQ(down.sent.fin, 1u);
    EXPECT_EQ(fs::path(down.local), groundDir / "cf" / "log" / "hk.dat");
    EXPECT_EQ(readFile(groundDir / "cf" / "log" / "hk.dat"), content);
    EXPECT_EQ(child.finish(), 0);
}

TEST_F(TwoEntities, Class2PutAndGetAtTheSameTime)
{
    const std::string upContent = pattern(25000, 3);
    const std::string downContent = pattern(20000, 4);
    const fs::path src = writeBinary(tmp.path() / "src" / "up.bin", upContent);
    const fs::path flightSrc = writeBinary(tmp.path() / "flight-src" / "down.bin", downContent);
    const fs::path flightDir = tmp.path() / "flight";
    const fs::path groundDir = tmp.path() / "downlink";

    Child child([&](int fd) {
        Entity flight(fd, entityConfig("0.24", "0.23", flightDir), Dropper{9});
        std::string error;
        if (!flight.engine().put({flightSrc, "/cf/down.bin", 2}, error)) {
            return 5;
        }
        const auto deadline = Clock::now() + 30s;
        while (Clock::now() < deadline && flight.pump()) {
        }
        if (flight.finished().size() != 2) {
            return 10 + static_cast<int>(flight.finished().size());
        }
        for (const auto& s : flight.finished()) {
            if (s.state != State::Done) {
                return 20;
            }
        }
        return readFile(flightDir / "cf" / "up.bin") == upContent ? 0 : 30;
    });
    Entity ground(child.fd(), entityConfig("0.23", "0.24", groundDir), Dropper{6});
    std::string error;
    ASSERT_TRUE(ground.engine().put({src, "/cf/up.bin", 2}, error)) << error;
    ASSERT_TRUE(ground.pumpUntil([&] { return ground.finished().size() == 2; }, 30s));
    ground.linger(300ms);
    for (const auto& s : ground.finished()) {
        EXPECT_EQ(s.state, State::Done) << s.id.toString() << " " << s.condition;
    }
    EXPECT_EQ(readFile(groundDir / "cf" / "down.bin"), downContent);
    EXPECT_EQ(child.finish(), 0);
}

TEST_F(TwoEntities, Class1PutNeedsNoAnswer)
{
    const std::string content = pattern(5000, 5);
    const fs::path src = writeBinary(tmp.path() / "src" / "a.bin", content);
    const fs::path flightDir = tmp.path() / "flight";
    Child child([&](int fd) {
        Entity flight(fd, entityConfig("0.24", "0.23", flightDir));
        return flightUntilClosed(flight, State::Done, flightDir / "cf" / "a.bin", content);
    });
    Entity ground(child.fd(), entityConfig("0.23", "0.24", tmp.path() / "ground"));
    std::string error;
    ASSERT_TRUE(ground.engine().put({src, "/cf/a.bin", 1}, error)) << error;
    ASSERT_TRUE(ground.pumpUntil([&] { return !ground.finished().empty(); }, 10s));
    ground.linger(300ms);
    EXPECT_EQ(ground.finished().front().state, State::Done);
    EXPECT_EQ(ground.finished().front().received.fin, 0u);   // class 1: nothing comes back
    EXPECT_EQ(child.finish(), 0);
}

TEST_F(TwoEntities, CancelledClass2PutEndsCancelledOnBothSides)
{
    const std::string content = pattern(512 * 200, 6);
    const fs::path src = writeBinary(tmp.path() / "src" / "big.bin", content);
    const fs::path flightDir = tmp.path() / "flight";
    Child child([&](int fd) {
        Entity flight(fd, entityConfig("0.24", "0.23", flightDir));
        const int code = flightUntilClosed(flight, State::Cancelled, std::nullopt, content);
        std::error_code ec;
        return code != 0 ? code : (fs::exists(flightDir / "cf" / "big.bin", ec) ? 40 : 0);
    });
    Config groundCfg = entityConfig("0.23", "0.24", tmp.path() / "ground");
    groundCfg.pdusPerSecond = 50;   // slow, so it is still running when cancelled
    Entity ground(child.fd(), groundCfg);
    std::string error;
    const auto id = ground.engine().put({src, "/cf/big.bin", 2}, error);
    ASSERT_TRUE(id) << error;
    ground.pumpUntil([] { return false; }, 1s);
    ASSERT_TRUE(ground.engine().request(RequestKind::Cancel, id->toString(), error)) << error;
    ASSERT_TRUE(ground.pumpUntil([&] { return !ground.finished().empty(); }, 20s));
    ground.linger(300ms);
    EXPECT_EQ(ground.finished().front().state, State::Cancelled) << ground.finished().front().condition;
    EXPECT_EQ(child.finish(), 0);
}
