// Script checks (wait, expect, save, test...) and `run <script>`.

#include "ui/menu_parts.hpp"

#include "core/text.hpp"
#include "ui/script_runner.hpp"

#include <cli/cli.h>

#include <algorithm>
#include <filesystem>

namespace mcs::ui::menus {

namespace {

// "stop", then directories and *.mcs files that match what was typed of a path.
ArgCompletion scriptCompletion(const std::string& partial)
{
    namespace fs = std::filesystem;
    ArgCompletion c;
    if (std::string("stop").rfind(toLower(partial), 0) == 0) {
        c.values.emplace_back("stop", "stop the running script");
    }
    const auto slash = partial.find_last_of('/');
    const std::string dirText = slash == std::string::npos ? "" : partial.substr(0, slash + 1);
    const std::string prefix = slash == std::string::npos ? partial : partial.substr(slash + 1);
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> found;
    for (const auto& entry : fs::directory_iterator(dirText.empty() ? "." : dirText, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || (name.front() == '.' && prefix.empty())) {
            continue;
        }
        if (entry.is_directory(ec)) {
            found.emplace_back(dirText + name + "/", "directory");
        } else if (entry.path().extension() == ".mcs") {
            found.emplace_back(dirText + name, "test script");
        }
    }
    std::sort(found.begin(), found.end());
    c.values.insert(c.values.end(), found.begin(), found.end());
    if (c.values.empty()) {
        c.hint = "<script file>   a .mcs test script, e.g. scripts/ds_smoke.mcs, or 'stop'";
    }
    return c;
}

} // namespace

// Batch sessions only: the checks block while they wait.
void insertVerifyCommands(cli::Menu& root, VerifyService& verify)
{
    using Method = bool (VerifyService::*)(const VerifyService::Args&, std::ostream&);
    struct Entry {
        const char* name;
        Method method;
        const char* help;
        std::vector<std::string> params;
    };
    const Entry entries[] = {
        {"wait", &VerifyService::wait, "Pause the script, e.g. wait 2 (seconds; 500ms)", {"seconds"}},
        {"timeout", &VerifyService::setTimeout, "Time limit of the following checks (5 s at the start)",
         {"seconds"}},
        {"expect", &VerifyService::expect,
         "Check telemetry, e.g. expect ds.hk cmd_accepted_counter == $n+1 within 5",
         {"packet", "field op value", "within seconds"}},
        {"expect-event", &VerifyService::expectEvent, "Check for an event, e.g. expect-event DS id 1 type INFO",
         {"APP", "id n", "type T", "text \"...\"", "within seconds"}},
        {"expect-no-event", &VerifyService::expectNoEvent,
         "Fail if an event arrives, e.g. expect-no-event DS type ERROR for 2",
         {"APP", "id n", "type T", "text \"...\"", "for seconds"}},
        {"save", &VerifyService::save, "Save a telemetry value for $name, e.g. save n ds.hk cmd_accepted_counter",
         {"name", "packet", "field", "within seconds"}},
        {"test", &VerifyService::beginTest, "Start a test, e.g. test DS-001 \"NOOP works\" req DS-R1",
         {"ID", "title", "req R1,R2"}},
        {"end", &VerifyService::endTest, "End the current test", {}},
    };
    for (const Entry& e : entries) {
        root.Insert(std::make_unique<FswCommand>(
            e.name, e.help, e.params,
            [&verify, method = e.method](std::ostream& out, const std::vector<std::string>& args) {
                (verify.*method)(args, out);
            }));
    }
}

std::unique_ptr<FswCommand> buildRunCommand(ScriptRunner& runner, std::size_t& sessionErrors)
{
    return std::make_unique<FswCommand>(
        "run", "Run a test script in the background, e.g. run scripts/ds_smoke.mcs ('run stop' ends it)",
        std::vector<std::string>{"script|stop"},
        [&runner, &sessionErrors](std::ostream& out, const std::vector<std::string>& args) {
            const ScriptRunner::Status s = runner.status();
            if (args.empty()) {
                if (s.running) {
                    out << s.name << ": line " << s.line << " of " << s.lines << " ('run stop' ends it)\n";
                } else if (s.result) {
                    out << s.name << ": finished, " << (*s.result == 0 ? "PASS" : "FAIL") << '\n';
                } else {
                    out << "no script has run ('run <script>' starts one)\n";
                }
            } else if (args.size() == 1 && iequals(args[0], "stop")) {
                if (!s.running) {
                    ++sessionErrors;
                    out << "error: run stop: no script is running\n";
                    return;
                }
                runner.stop();
                out << "stopping " << s.name << '\n';
            } else if (args.size() == 1) {
                if (!runner.start(args[0], out)) {
                    ++sessionErrors;
                }
            } else {
                ++sessionErrors;
                out << "error: usage: run <script> | run stop | run\n";
            }
        },
        [](std::size_t index, const std::string& partial) {
            if (index > 0) {
                ArgCompletion c;
                c.hint = "(no more arguments: press Enter)";
                return c;
            }
            return scriptCompletion(partial);
        });
}

} // namespace mcs::ui::menus
