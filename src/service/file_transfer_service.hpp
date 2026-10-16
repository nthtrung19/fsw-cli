#pragma once

#include "cfdp/cfdp_config.hpp"
#include "cfdp/engine.hpp"
#include "log/packet_log.hpp"
#include "pipeline/pipeline.hpp"
#include "service/command_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace mcs {

// File transfer with CF (CFDP), for the "file" commands.
//
//   file put   PDUs from the engine -> CommandMessage{uplinkMid, cc 0, pdu} -> Pipeline -> CI -> CF
//   file get   "fsw cf playback_file ..." through the CommandService, then CF's PDUs arrive
//              as telemetry (downlinkMid) -> onDatagram() -> engine -> file in downlinkDir
//
// Threads: one worker owns the CFDP engine (the GSFC library is global and
// not thread-safe) and is the only thread that calls it. Every public method
// is thread-safe: requests and received PDUs are queued for the worker; the
// state is read from snapshots the worker publishes.
class FileTransferService {
public:
    using EngineFactory = std::function<std::unique_ptr<cfdp::ICfdpEngine>(
        const cfdp::Config&, cfdp::ICfdpEngine::PduOut, cfdp::ICfdpEngine::Notify, cfdp::ICfdpEngine::Log)>;

    struct Counts {
        std::size_t up = 0;
        std::size_t down = 0;
        std::size_t waiting = 0;   // file get sent, CF has not started yet
        std::size_t total() const { return up + down + waiting; }
    };

    struct WaitResult {
        std::optional<cfdp::TransferStatus> status;   // the finished transfer
        std::string error;                            // why waiting stopped early
    };
    using Progress = std::function<void(const cfdp::TransferStatus&)>;

    // `factory` defaults to the GSFC engine.
    FileTransferService(cfdp::Config config, Pipeline& pipeline, CommandService& commands, PacketLog* log,
                        std::string targetName, EngineFactory factory = {});
    ~FileTransferService();   // stop()

    FileTransferService(const FileTransferService&) = delete;
    FileTransferService& operator=(const FileTransferService&) = delete;

    // Starts the worker and creates the engine there. Throws ConfigError if
    // the engine cannot be created.
    void start();
    // Abandons active transfers (temp files removed) and joins the worker.
    void stop();
    bool running() const { return running_; }

    // Whether CF's PDUs can arrive (the telemetry receiver is open). Without
    // it, file get and class 2 put are refused with `why`.
    void setReceiving(bool receiving, std::string why = {});

    // Telemetry sink (receiver thread): keeps the downlinkMid packets.
    void onDatagram(const Bytes& datagram);

    // Starts an uplink. nullopt + `error` if refused (nothing is sent then).
    std::optional<cfdp::TransactionId> put(const std::filesystem::path& local, const std::string& remote,
                                           int cls, std::string& error);

    // Asks CF to send `remote` (playback_file on channel 0) and waits for it
    // in the background. Prints the command line (or the error) to `out`.
    // Returns a ticket for waitForGet(), or nullopt if nothing was sent.
    std::optional<std::size_t> get(const std::string& remote, const std::optional<std::filesystem::path>& local,
                                   int cls, std::ostream& out);

    bool control(cfdp::RequestKind kind, const std::string& idOrAll, std::string& error);

    // Batch mode: blocks until the transfer finishes. Gives up after
    // inactivity_timeout + 10 s without progress.
    WaitResult waitFor(const cfdp::TransactionId& id, const Progress& progress = {});
    WaitResult waitForGet(std::size_t ticket, const Progress& progress = {});

    // Waiting gets, active transfers (oldest first), then finished ones (newest first).
    std::vector<cfdp::TransferStatus> transfers() const;
    Counts activeCounts() const;

    // Lines for the operator ("started …", "✔ put … done", …), oldest first.
    std::vector<std::string> takeNotices();

    // Called (on the worker) after each change; must be quick and thread-safe.
    void setOnChange(std::function<void()> onChange);

    // How long a file get may wait for CF to start sending (default 30 s);
    // the clock pauses while another downlink from CF is running.
    void setStartTimeout(std::chrono::seconds timeout);

    const cfdp::Config& config() const { return config_; }
    std::size_t droppedPdus() const { return dropped_; }
    std::size_t sendErrors() const { return sendErrors_; }
    std::vector<std::string> diagnostics() const;   // last engine messages

private:
    struct Request;
    struct Expectation {
        std::size_t ticket = 0;
        std::string remote;
        std::optional<std::filesystem::path> local;   // move the file there when done
        std::filesystem::path predicted;              // where it will land in downlinkDir
        int cls = 1;
        cfdp::TransferTime created{};
        std::chrono::steady_clock::duration waited{};   // excluding time CF was busy
        std::size_t datagramsAtStart = 0;              // to tell "no telemetry at all"
        std::optional<cfdp::TransactionId> id;        // once CF started it
        std::optional<cfdp::TransferStatus> failed;   // CF never started it
    };

    void run();
    void handle(Request& request);
    void publish();
    cfdp::TransferStatus waitingStatus(const Expectation& e) const;
    void checkExpectations(std::chrono::steady_clock::duration elapsed);
    void onPdu(const Bytes& pdu, const cfdp::PduInfo& info);
    void onEvent(cfdp::EngineEvent event, const cfdp::TransferStatus& status);
    void onLog(int level, const std::string& text);
    void notice(std::string line);
    cfdp::TransferStatus overlay(cfdp::TransferStatus status) const;   // with service-level facts
    bool localInUse(const std::filesystem::path& local) const;
    std::optional<cfdp::TransferStatus> findLocked(const cfdp::TransactionId& id) const;

    cfdp::Config config_;
    Pipeline& pipeline_;
    CommandService& commands_;
    PacketLog* log_;
    std::string target_;
    EngineFactory factory_;
    std::filesystem::path downlinkDir_;   // absolute
    cfdp::ICfdpEngine* engine_ = nullptr; // worker thread only
    bool sendFailing_ = false;            // worker thread only

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> sendErrors_{0};
    std::atomic<std::size_t> datagrams_{0};

    mutable std::mutex mutex_;
    std::condition_variable workCv_;    // wakes the worker
    std::condition_variable doneCv_;    // wakes waiters after each publish
    std::deque<std::shared_ptr<Request>> requests_;
    std::deque<Bytes> pdus_;
    std::vector<cfdp::TransferStatus> snapshot_;
    std::vector<cfdp::TransferStatus> failedGets_;   // gets CF never started, newest first
    std::map<std::size_t, Expectation> expectations_;
    std::map<cfdp::TransactionId, std::size_t> linked_;       // downlink id -> ticket
    std::map<cfdp::TransactionId, std::string> movedTo_;      // downlink id -> requested local
    std::deque<std::string> notices_;
    std::deque<std::string> diagnostics_;
    std::size_t nextTicket_ = 1;
    bool receiving_ = true;
    std::string receivingWhy_;
    std::chrono::seconds startTimeout_{30};
    std::function<void()> onChange_;
    std::string startError_;
    bool started_ = false;
};

} // namespace mcs
