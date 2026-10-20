#include "ui/script_runner.hpp"

#include "core/errors.hpp"
#include "service/command_service.hpp"
#include "ui/menu_builder.hpp"
#include "verify/variables.hpp"
#include "verify/verify_service.hpp"

#include <cli/cli.h>
#include <cli/clifilesession.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <unistd.h>

namespace mcs::ui {

namespace {

// Writes to `out`, putting the location of the script line being run in
// front of its error and FAIL lines ("demo.mcs:7: FAIL expect ...").
class LocationPrefixBuf : public std::streambuf {
public:
    explicit LocationPrefixBuf(std::ostream& out) : out_(out) {}
    ~LocationPrefixBuf() override { flushLine(); }

    void setLocation(std::string location) { location_ = std::move(location); }

protected:
    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) {
            return traits_type::not_eof(c);
        }
        line_ += traits_type::to_char_type(c);
        if (line_.back() == '\n') {
            flushLine();
        }
        return c;
    }

    int sync() override
    {
        flushLine();
        out_.flush();
        return 0;
    }

private:
    void flushLine()
    {
        if (line_.empty()) {
            return;
        }
        if (!location_.empty() && (line_.rfind("error:", 0) == 0 || line_.rfind("FAIL ", 0) == 0)) {
            out_ << location_ << ": ";
        }
        out_ << line_;
        line_.clear();
    }

    std::ostream& out_;
    std::string location_;
    std::string line_;
};

// Hands every complete line to `onLine` (the script's output for the UI thread).
class LineQueueBuf : public std::streambuf {
public:
    explicit LineQueueBuf(std::function<void(std::string)> onLine) : onLine_(std::move(onLine)) {}
    ~LineQueueBuf() override
    {
        if (!line_.empty()) {
            onLine_(std::move(line_));
        }
    }

protected:
    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) {
            return traits_type::not_eof(c);
        }
        const char ch = traits_type::to_char_type(c);
        if (ch == '\n') {
            onLine_(std::move(line_));
            line_.clear();
        } else {
            line_ += ch;
        }
        return c;
    }

private:
    std::function<void(std::string)> onLine_;
    std::string line_;
};

// Unknown commands and exceptions are counted in `cliErrors`.
void installBatchHandlers(cli::Cli& cli, std::size_t& cliErrors)
{
    cli.WrongCommandHandler([&cliErrors](std::ostream& out, const std::string& cmd) {
        ++cliErrors;
        out << "error: unknown command: " << cmd << '\n';
    });
    cli.StdExceptionHandler([&cliErrors](std::ostream& out, const std::string& cmd, const std::exception& e) {
        ++cliErrors;
        out << "error: " << cmd << ": " << e.what() << '\n';
    });
    installHelp(cli, terminalHelp(terminalWidth, ::isatty(STDOUT_FILENO) != 0));
}

} // namespace

std::vector<ScriptLine> readScript(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open script file: " + path);
    }
    std::vector<ScriptLine> lines;
    std::string line;
    for (std::size_t number = 1; std::getline(file, line); ++number) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();   // tolerate Windows line endings
        }
        lines.push_back({line, path + ":" + std::to_string(number)});
    }
    return lines;
}

std::vector<ScriptLine> commandLines(const std::vector<std::string>& commands)
{
    std::vector<ScriptLine> lines;
    for (const auto& c : commands) {
        lines.push_back({c, ""});
    }
    return lines;
}

std::size_t checkScript(const ScriptTarget& target, const std::vector<ScriptLine>& lines,
                        const std::string& name, std::ostream& out)
{
    CommandService service(target.catalog, PayloadEncoder(target.endian), target.pipeline, nullptr,
                           target.targetName);
    VariableTable variables;
    VerifyService verify(target.tlmCatalog, nullptr, target.noTelemetry, variables, [] { return std::nullopt; },
                         true);
    std::size_t sessionErrors = 0;
    std::size_t cliErrors = 0;
    cli::Cli cli(buildRootMenu(service, sessionErrors, nullptr, target.files, FileMenuMode::Batch, &verify));
    installBatchHandlers(cli, cliErrors);
    const auto errorCount = [&] { return cliErrors + sessionErrors + service.failures() + verify.failures(); };

    std::istringstream unusedInput;   // the session requires a valid istream
    std::ostringstream lineOut;
    cli::CliFileSession session(cli, unusedInput, lineOut);
    std::size_t errors = 0;
    // The error lines of `lineOut` (with their indented continuation lines).
    const auto report = [&](const std::string& location) {
        std::istringstream text(lineOut.str());
        bool inError = false;
        for (std::string l; std::getline(text, l);) {
            if (l.rfind("error:", 0) == 0) {
                out << (location.empty() ? "" : location + ": ") << l << '\n';
                inError = true;
            } else if (inError && !l.empty() && l.front() == ' ') {
                out << l << '\n';
            } else {
                inError = false;
            }
        }
        lineOut.str("");
    };

    for (const auto& line : lines) {
        std::string text;
        bool fromVariables = false;
        try {
            fromVariables = !VariableTable::referenced(line.text).empty();
            text = variables.expand(line.text);
        } catch (const ParseError& e) {
            out << (line.location.empty() ? "" : line.location + ": ") << "error: " << e.what() << '\n';
            ++errors;
            continue;
        }
        verify.setValuesKnown(!fromVariables);
        const std::size_t before = errorCount();
        session.Feed(text);
        errors += errorCount() - before;
        report(line.location);
    }
    const std::size_t before = errorCount();
    verify.finish(lineOut);
    errors += errorCount() - before;
    report(name);
    return errors;
}

int runScript(const ScriptTarget& target, const std::vector<ScriptLine>& lines, std::ostream& sink,
              const std::atomic<bool>* stop, const std::function<void(std::size_t)>& onLine)
{
    CommandService service(target.catalog, PayloadEncoder(target.endian), target.pipeline, target.log,
                           target.targetName);
    service.setVerbose(target.verbose);
    VariableTable variables;
    VerifyService verify(target.tlmCatalog, target.history, target.noTelemetry, variables,
                         [&service] { return service.lastSentAt(); }, false);
    verify.setStop(stop);
    std::size_t sessionErrors = 0;
    std::size_t cliErrors = 0;
    cli::Cli cli(buildRootMenu(service, sessionErrors, nullptr, target.files, FileMenuMode::Batch, &verify));
    installBatchHandlers(cli, cliErrors);

    LocationPrefixBuf buffer(sink);
    std::ostream out(&buffer);
    std::istringstream unusedInput;   // the session requires a valid istream
    cli::CliFileSession session(cli, unusedInput, out);

    std::size_t variableErrors = 0;
    bool stopped = false;
    const auto otherErrors = [&] { return cliErrors + sessionErrors + service.failures() + variableErrors; };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const ScriptLine& line = lines[i];
        if (stop != nullptr && stop->load()) {
            buffer.setLocation("");
            out << "stopped at " << (line.location.empty() ? "command " + std::to_string(i + 1) : line.location)
                << '\n';
            stopped = true;
            break;
        }
        if (onLine) {
            onLine(i);
        }
        buffer.setLocation(line.location);
        const std::size_t before = otherErrors();
        try {
            session.Feed(variables.expand(line.text));
        } catch (const ParseError& e) {
            ++variableErrors;
            out << "error: " << e.what() << '\n';
        }
        if (otherErrors() > before) {
            verify.noteFailure();   // a failed command fails its test
        }
        out.flush();
    }
    if (!stopped && stop != nullptr && stop->load()) {   // stopped during the last line
        buffer.setLocation("");
        out << "stopped\n";
        stopped = true;
    }
    buffer.setLocation("");
    verify.finish(out, stopped);
    if (verify.anyRun()) {
        verify.printSummary(out, service.packetsSent(), otherErrors());
    }
    out.flush();
    const bool ok = !stopped && otherErrors() == 0 && verify.failures() == 0;
    return ok ? 0 : 1;
}

// ---- ScriptRunner ---------------------------------------------------------------------

ScriptRunner::ScriptRunner(ScriptTarget target) : target_(std::move(target)) {}

ScriptRunner::~ScriptRunner()
{
    stop();
    join();
}

bool ScriptRunner::start(const std::string& path, std::ostream& out)
{
    if (const Status s = status(); s.running) {
        out << "error: run: " << s.name << " is still running ('run stop' ends it)\n";
        return false;
    }
    join();   // the previous run's thread has ended

    std::vector<ScriptLine> lines;
    try {
        lines = readScript(path);
    } catch (const std::exception& e) {
        out << "error: run: " << e.what() << '\n';
        return false;
    }
    if (lines.empty()) {
        out << "error: run: " << path << " has no commands\n";
        return false;
    }
    if (const std::size_t errors = checkScript(target_, lines, path, out); errors != 0) {
        out << path << ": not run: " << errors << " error(s)\n";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status{true, std::filesystem::path(path).filename().string(), 0, lines.size(), std::nullopt};
    }
    stop_ = false;
    out << "running " << path << ": " << lines.size() << " line(s); 'run stop' ends it\n";
    worker_ = std::thread([this, lines = std::move(lines)] {
        int result = 1;
        {
            LineQueueBuf buffer([this](std::string line) { addLine(std::move(line)); });
            std::ostream scriptOut(&buffer);
            try {
                result = runScript(target_, lines, scriptOut, &stop_, [this](std::size_t i) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.line = i + 1;
                });
            } catch (const std::exception& e) {
                scriptOut << "error: run: " << e.what() << '\n';
            }
        }
        std::function<void()> onChange;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.running = false;
            status_.result = result;
            onChange = onChange_;
        }
        if (onChange) {
            onChange();
        }
    });
    return true;
}

void ScriptRunner::stop()
{
    stop_ = true;
}

void ScriptRunner::join()
{
    if (worker_.joinable()) {
        worker_.join();
    }
}

ScriptRunner::Status ScriptRunner::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<std::string> ScriptRunner::takeOutput()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines;
    lines.swap(output_);
    return lines;
}

void ScriptRunner::setOnChange(std::function<void()> onChange)
{
    std::lock_guard<std::mutex> lock(mutex_);
    onChange_ = std::move(onChange);
}

void ScriptRunner::addLine(std::string line)
{
    std::function<void()> onChange;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_.push_back(std::move(line));
        onChange = onChange_;
    }
    if (onChange) {
        onChange();
    }
}

} // namespace mcs::ui
