#pragma once

// Test scripts: the check pass and the run of a script (mcs -f / -c), and
// ScriptRunner, which runs one on its own thread for the full-screen session
// ("run <script>"). In ui/ because the lines go through the cli menu tree.

#include "catalog/catalog.hpp"
#include "core/bytes.hpp"
#include "log/packet_log.hpp"
#include "pipeline/pipeline.hpp"
#include "service/file_transfer_service.hpp"
#include "telemetry/tlm_catalog.hpp"
#include "verify/tlm_history.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace mcs::ui {

// A script line and where it came from ("demo.mcs:7"; empty for -c), for messages.
struct ScriptLine {
    std::string text;
    std::string location;
};

// The lines of a script file, without blank lines and '#' comments. Throws
// std::runtime_error if the file cannot be read.
std::vector<ScriptLine> readScript(const std::string& path);

// -c commands as script lines (no location).
std::vector<ScriptLine> commandLines(const std::vector<std::string>& commands);

// What a script runs against. Everything here outlives the run and may be
// used from the script's thread: Pipeline, PacketLog, FileTransferService and
// TlmHistory are thread-safe.
struct ScriptTarget {
    const CommandCatalog& catalog;
    Endian endian;
    std::string targetName;
    Pipeline& pipeline;
    PacketLog* log;                       // null: no packet log
    FileTransferService* files;           // null: no "cfdp" block
    const TelemetryCatalog& tlmCatalog;   // empty: no "telemetry" block
    const TlmHistory* history;            // null: telemetry is not received (see noTelemetry)
    std::string noTelemetry;              // why checks cannot run
    bool verbose = false;
};

// The pass over a script before it runs: every line goes through a check-only
// menu tree (own CommandService, VerifyService and variables), so nothing is
// sent and nothing waits. Lines using $variables are checked without their
// values. Prints each error with its location (`name` for errors of the whole
// script); returns how many there were.
std::size_t checkScript(const ScriptTarget& target, const std::vector<ScriptLine>& lines,
                        const std::string& name, std::ostream& out);

// Runs the lines through their own CommandService, VerifyService, variables
// and menu tree: $variables are replaced first, error and FAIL lines carry the
// line's location, and a run with checks ends with a summary. Returns 0 if
// every line and check passed, 1 otherwise.
//
// `stop` (may be null) is checked before each line and inside waits; a stop
// ends the run with "stopped at <location>" (result 1). `onLine(i)` is called
// before line i runs.
int runScript(const ScriptTarget& target, const std::vector<ScriptLine>& lines, std::ostream& out,
              const std::atomic<bool>* stop = nullptr, const std::function<void(std::size_t)>& onLine = {});

// One script at a time on its own thread, for the full-screen session.
//
// start() checks the script on the calling (UI) thread and prints the errors
// there; the run's output is collected as lines for the UI thread
// (takeOutput), and onChange is called on the script's thread after each line.
//
// The cli library registers every session's stream in a global list without a
// lock (Cli::cout(), which mcs never uses). That is safe here because only one
// script runs at a time and the full-screen session's own session is created
// before the first run and destroyed after join().
class ScriptRunner {
public:
    struct Status {
        bool running = false;
        std::string name;               // file name of the script
        std::size_t line = 0;           // 1-based line being run
        std::size_t lines = 0;
        std::optional<int> result;      // 0 passed, 1 failed (when finished)
    };

    explicit ScriptRunner(ScriptTarget target);
    ~ScriptRunner();   // stop() and join()

    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    // Reads and checks the script, printing errors to `out`; starts it if
    // clean. False if it did not start (also while another script runs).
    bool start(const std::string& path, std::ostream& out);

    // Asks the running script to stop; returns at once.
    void stop();

    // Waits for the script's thread to end.
    void join();

    Status status() const;

    // Complete output lines since the last call, oldest first.
    std::vector<std::string> takeOutput();

    void setOnChange(std::function<void()> onChange);

private:
    void addLine(std::string line);

    ScriptTarget target_;
    std::thread worker_;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    Status status_;
    std::vector<std::string> output_;
    std::function<void()> onChange_;
};

} // namespace mcs::ui
