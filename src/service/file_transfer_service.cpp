#include "service/file_transfer_service.hpp"

#include "cfdp/gsfc_engine.hpp"
#include "core/errors.hpp"
#include "core/hex.hpp"
#include "core/text.hpp"
#include "telemetry/ccsds_tlm_parser.hpp"

#include <algorithm>
#include <system_error>

namespace mcs {

namespace fs = std::filesystem;
using namespace cfdp;
using Steady = std::chrono::steady_clock;

namespace {

constexpr std::size_t kMaxQueuedPdus = 4096;
constexpr std::size_t kMaxNotices = 500;
constexpr std::size_t kMaxDiagnostics = 200;
constexpr std::size_t kMaxFailedGets = 50;
constexpr auto kWorkerPeriod = std::chrono::milliseconds(20);
constexpr auto kRequestTimeout = std::chrono::seconds(5);

std::string normal(const fs::path& p)
{
    std::error_code ec;
    return fs::absolute(p, ec).lexically_normal().string();
}

std::string kindWord(const TransferStatus& s)
{
    return s.direction == Direction::Uplink ? "put" : (s.requested ? "get" : "in");
}

std::string sizeText(const TransferStatus& s)
{
    return s.sizeKnown ? formatBytes(s.fileSize) : "size unknown";
}

std::string quoted(const std::string& text)
{
    return "\"" + text + "\"";
}

std::string startedNotice(const TransferStatus& s)
{
    const std::string cls = ", class " + std::to_string(s.cls);
    if (s.direction == Direction::Uplink) {
        return "started " + s.id.toString() + "  ↑ " + s.local + " (" + sizeText(s) + ") → " + s.remote + cls;
    }
    if (s.requested) {
        return "started " + s.id.toString() + "  ↓ " + s.remote + " (" + sizeText(s) + ") → " + s.local + cls;
    }
    return "↓ " + s.id.toString() + " incoming " + s.remote + " (" + sizeText(s) + ") → " + s.local + cls
         + " (not requested by 'file get')";
}

std::string finishedNotice(const TransferStatus& s)
{
    const std::string head = kindWord(s) + " " + s.id.toString();
    const double seconds = std::chrono::duration<double>(s.end - s.start).count();
    switch (s.state) {
    case State::Done: {
        std::string line = "✔ " + head + " done: " + (s.direction == Direction::Uplink ? s.remote : s.local) + " "
                         + sizeText(s) + " in " + formatSeconds(seconds) + " (" + formatRate(s.rateBps) + ")";
        if (s.cls == 2) {
            const auto naks = s.direction == Direction::Uplink ? s.received.nak : s.sent.nak;
            line += ", " + std::to_string(naks) + (naks == 1 ? " NAK" : " NAKs") + ", FIN ok";
        }
        return line;
    }
    case State::Cancelled:
        return "⊘ " + head + " cancelled" + (s.note.empty() ? "" : " (" + s.note + ")");
    case State::Abandoned:
        return "⊘ " + head + " abandoned" + (s.note.empty() ? "" : " (" + s.note + ")");
    default:
        return "✘ " + head + " failed: " + (s.condition.empty() ? "?" : s.condition)
             + (s.note.empty() ? "" : " (" + s.note + ")");
    }
}

std::string startFields(const TransferStatus& s)
{
    return "cfdp=start id=" + s.id.toString() + " dir=" + (s.direction == Direction::Uplink ? "up" : "down")
         + " class=" + std::to_string(s.cls) + " local=" + quoted(s.local) + " remote=" + quoted(s.remote)
         + " size=" + (s.sizeKnown ? std::to_string(s.fileSize) : "?");
}

std::string endFields(const TransferStatus& s)
{
    const double seconds = std::chrono::duration<double>(s.end - s.start).count();
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", seconds);
    return "cfdp=end id=" + (s.id.source.empty() ? std::string("-") : s.id.toString()) + " result="
         + toString(s.state) + " condition=" + (s.condition.empty() ? "-" : s.condition) + " bytes="
         + std::to_string(s.bytesDone) + " secs=" + secs + " fd_sent=" + std::to_string(s.sent.fileData)
         + " fd_received=" + std::to_string(s.received.fileData) + " naks="
         + std::to_string(s.sent.nak + s.received.nak) + " resent=" + std::to_string(s.bytesResent)
         + " local=" + quoted(s.local) + " remote=" + quoted(s.remote);
}

} // namespace

struct FileTransferService::Request {
    enum class Kind { Put, Control };
    Kind kind = Kind::Put;
    fs::path local;
    std::string remote;
    int cls = 1;
    RequestKind control = RequestKind::Cancel;
    std::string target;

    bool done = false;
    bool ok = false;
    std::optional<TransactionId> id;
    std::string error;
};

FileTransferService::FileTransferService(Config config, Pipeline& pipeline, CommandService& commands,
                                         PacketLog* log, std::string targetName, EngineFactory factory)
    : config_(std::move(config)), pipeline_(pipeline), commands_(commands), log_(log),
      target_(std::move(targetName)), factory_(std::move(factory))
{
    if (!factory_) {
        factory_ = [](const Config& c, ICfdpEngine::PduOut out, ICfdpEngine::Notify notify, ICfdpEngine::Log l) {
            return std::make_unique<GsfcEngine>(c, std::move(out), std::move(notify), std::move(l));
        };
    }
    downlinkDir_ = normal(config_.downlinkDir);
}

FileTransferService::~FileTransferService()
{
    stop();
}

// ---- lifecycle ----------------------------------------------------------------------

void FileTransferService::start()
{
    if (worker_.joinable()) {
        return;
    }
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        startError_.clear();
    }
    worker_ = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    doneCv_.wait(lock, [this] { return started_; });
    if (!startError_.empty()) {
        const std::string error = startError_;
        lock.unlock();
        worker_.join();
        throw ConfigError(error);
    }
    running_ = true;
}

void FileTransferService::stop()
{
    if (!worker_.joinable()) {
        return;
    }
    stopping_ = true;
    workCv_.notify_all();
    worker_.join();
    running_ = false;
    doneCv_.notify_all();
}

void FileTransferService::run()
{
    std::unique_ptr<ICfdpEngine> engine;
    try {
        engine = factory_(
            config_, [this](const Bytes& pdu, const PduInfo& info) { onPdu(pdu, info); },
            [this](EngineEvent event, const TransferStatus& status) { onEvent(event, status); },
            [this](int level, const std::string& text) { onLog(level, text); });
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        startError_ = std::string("file transfer: ") + e.what();
        started_ = true;
        doneCv_.notify_all();
        return;
    }
    engine_ = engine.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
    }
    doneCv_.notify_all();

    auto last = Steady::now();
    while (!stopping_) {
        std::deque<std::shared_ptr<Request>> requests;
        std::deque<Bytes> pdus;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workCv_.wait_for(lock, kWorkerPeriod,
                             [this] { return stopping_ || !requests_.empty() || !pdus_.empty(); });
            requests.swap(requests_);
            pdus.swap(pdus_);
        }
        for (auto& r : requests) {
            handle(*r);
        }
        for (const auto& pdu : pdus) {
            engine_->givePdu(pdu);
        }
        engine_->cycle();
        const auto now = Steady::now();
        checkExpectations(now - last);
        last = now;
        publish();
    }

    // Abandon what is still running: the partner times out, temp files go.
    std::string ignored;
    engine_->request(RequestKind::Abandon, "all", ignored);
    engine_->cycle();
    publish();
    engine.reset();
    engine_ = nullptr;
}

// ---- worker side ------------------------------------------------------------------------

void FileTransferService::handle(Request& r)
{
    bool ok = false;
    std::optional<TransactionId> id;
    std::string error;
    if (r.kind == Request::Kind::Put) {
        id = engine_->put({r.local, r.remote, r.cls}, error);
        ok = id.has_value();
    } else {
        ok = engine_->request(r.control, r.target, error);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        r.done = true;
        r.ok = ok;
        r.id = id;
        r.error = error;
    }
    doneCv_.notify_all();
}

TransferStatus FileTransferService::waitingStatus(const Expectation& e) const
{
    TransferStatus s;
    s.direction = Direction::Downlink;
    s.cls = e.cls;
    s.requested = true;
    s.remote = e.remote;
    s.local = (e.local ? *e.local : e.predicted).string();
    s.state = State::Waiting;
    s.start = e.created;
    s.lastProgress = e.created;
    const double waited = std::chrono::duration<double>(e.waited).count();
    s.note = "waiting for CF to start · " + std::to_string(static_cast<long>(waited)) + " s";
    return s;
}

TransferStatus FileTransferService::overlay(TransferStatus status) const
{
    if (status.direction == Direction::Downlink) {
        status.requested = linked_.count(status.id) != 0;
        if (const auto it = movedTo_.find(status.id); it != movedTo_.end()) {
            status.local = it->second;
        }
    }
    return status;
}

namespace {

// Whether the UI has something new to show: always while a transfer runs or
// waits (progress, rate, ETA move), otherwise only when the list changed.
bool listChanged(const std::vector<TransferStatus>& before, const std::vector<TransferStatus>& after)
{
    const auto running = [](const std::vector<TransferStatus>& list) {
        return std::any_of(list.begin(), list.end(), [](const TransferStatus& s) { return !isFinished(s.state); });
    };
    if (running(before) || running(after) || before.size() != after.size()) {
        return true;
    }
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (!(before[i].id == after[i].id) || before[i].state != after[i].state) {
            return true;
        }
    }
    return false;
}

} // namespace

void FileTransferService::publish()
{
    const std::vector<TransferStatus> list = engine_ != nullptr ? engine_->transfers()
                                                                 : std::vector<TransferStatus>{};
    std::function<void()> onChange;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TransferStatus> active;
        std::vector<TransferStatus> finished;
        for (const auto& [ticket, e] : expectations_) {
            if (!e.id && !e.failed) {
                active.push_back(waitingStatus(e));
            }
        }
        for (const auto& s : list) {
            (isFinished(s.state) ? finished : active).push_back(overlay(s));
        }
        finished.insert(finished.end(), failedGets_.begin(), failedGets_.end());
        std::stable_sort(finished.begin(), finished.end(),
                         [](const TransferStatus& a, const TransferStatus& b) { return a.end > b.end; });
        active.insert(active.end(), finished.begin(), finished.end());
        if (listChanged(snapshot_, active)) {
            onChange = onChange_;   // the worker publishes every cycle: idle cycles redraw nothing
        }
        snapshot_ = std::move(active);
    }
    doneCv_.notify_all();
    if (onChange) {
        onChange();
    }
}

void FileTransferService::checkExpectations(Steady::duration elapsed)
{
    // CF plays back one file at a time per channel: while a downlink from CF
    // runs, a waiting get is queued on CF, not lost.
    bool cfBusy = false;
    if (engine_ != nullptr) {
        for (const auto& s : engine_->transfers()) {
            if (s.direction == Direction::Downlink && !isFinished(s.state)) {
                cfBusy = true;
            }
        }
    }
    std::vector<TransferStatus> failed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [ticket, e] : expectations_) {
            if (e.id || e.failed) {
                continue;
            }
            if (!cfBusy) {
                e.waited += elapsed;
            }
            if (e.waited < startTimeout_) {
                continue;
            }
            TransferStatus s = waitingStatus(e);
            s.state = State::Failed;
            s.condition = "NOT_STARTED";
            s.end = TransferClock::now();
            s.note = datagrams_ == e.datagramsAtStart
                       ? "no telemetry received; is TO sending? (fsw ci enable_to)"
                       : "CF did not start it within " + std::to_string(startTimeout_.count())
                             + " s; file missing on the FSW? check CF events and cf.hk err_counter";
            e.failed = s;
            failedGets_.insert(failedGets_.begin(), s);
            if (failedGets_.size() > kMaxFailedGets) {
                failedGets_.pop_back();
            }
            failed.push_back(s);
        }
    }
    for (const auto& s : failed) {
        notice("✘ get " + s.remote + " failed: " + s.note);
        if (log_ != nullptr) {
            log_->recordEvent(target_, endFields(s));
        }
    }
}

void FileTransferService::onPdu(const Bytes& pdu, const PduInfo& info)
{
    try {
        const Pipeline::Result result = pipeline_.send(CommandMessage{config_.uplinkMid, 0, pdu});
        sendFailing_ = false;
        if (info.kind != PduKind::FileData) {
            const std::string line = "cfdp " + describePdu(info);
            if (log_ != nullptr) {
                log_->record({target_, line, result.note, result.wire, pipeline_.isDryRun()});
            }
            if (pipeline_.isDryRun()) {
                notice("[dry-run] " + line + "  " + toHex(result.wire));
            }
        }
    } catch (const std::exception& e) {
        ++sendErrors_;
        if (!sendFailing_) {   // report the first failure of a run of them
            sendFailing_ = true;
            notice(std::string("error: cfdp: cannot send PDU: ") + e.what());
        }
    }
}

void FileTransferService::onEvent(EngineEvent event, const TransferStatus& raw)
{
    std::optional<TransferStatus> status;
    if (event == EngineEvent::Started) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (raw.direction == Direction::Downlink) {
                for (auto& [ticket, e] : expectations_) {
                    if (!e.id && !e.failed && e.remote == raw.remote) {
                        e.id = raw.id;
                        linked_[raw.id] = ticket;
                        break;
                    }
                }
            }
            status = overlay(raw);
        }
        notice(startedNotice(*status));
        if (log_ != nullptr) {
            log_->recordEvent(target_, startFields(*status));
        }
        return;
    }

    // Finished: a get with its own local path moves the file there.
    std::optional<fs::path> wanted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = linked_.find(raw.id); it != linked_.end()) {
            if (const auto e = expectations_.find(it->second); e != expectations_.end()) {
                wanted = e->second.local;
            }
        }
    }
    std::string moveNote;
    if (wanted && raw.state == State::Done && raw.direction == Direction::Downlink) {
        std::error_code ec;
        if (fs::exists(*wanted, ec)) {
            moveNote = "not moved: " + wanted->string() + " exists";
        } else {
            fs::create_directories(wanted->parent_path(), ec);
            ec.clear();
            fs::rename(raw.local, *wanted, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(raw.local, *wanted, ec);
                if (!ec) {
                    fs::remove(raw.local, ec);
                }
            }
            if (ec) {
                moveNote = "not moved to " + wanted->string() + ": " + ec.message();
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                movedTo_[raw.id] = wanted->string();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = overlay(raw);
    }
    if (!moveNote.empty()) {
        status->note = moveNote;
    }
    notice(finishedNotice(*status));
    if (log_ != nullptr) {
        log_->recordEvent(target_, endFields(*status));
    }
}

void FileTransferService::onLog(int level, const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.push_back(text);
        if (diagnostics_.size() > kMaxDiagnostics) {
            diagnostics_.pop_front();
        }
    }
    if (level >= 3) {
        notice("warning: cfdp engine: " + text);
    }
}

void FileTransferService::notice(std::string line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    notices_.push_back(std::move(line));
    if (notices_.size() > kMaxNotices) {
        notices_.pop_front();
    }
}

// ---- caller side ----------------------------------------------------------------------------

void FileTransferService::setReceiving(bool receiving, std::string why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    receiving_ = receiving;
    receivingWhy_ = std::move(why);
}

void FileTransferService::setOnChange(std::function<void()> onChange)
{
    std::lock_guard<std::mutex> lock(mutex_);
    onChange_ = std::move(onChange);
}

void FileTransferService::setStartTimeout(std::chrono::seconds timeout)
{
    std::lock_guard<std::mutex> lock(mutex_);
    startTimeout_ = timeout;
}

void FileTransferService::onDatagram(const Bytes& datagram)
{
    ++datagrams_;
    if (datagram.size() < 12
        || static_cast<std::uint16_t>((datagram[0] << 8) | datagram[1]) != config_.downlinkMid) {
        return;
    }
    const TlmParseResult parsed = parseTelemetry(datagram);
    if (!parsed.frame || parsed.frame->payload.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pdus_.size() >= kMaxQueuedPdus) {
            ++dropped_;
            return;
        }
        pdus_.push_back(parsed.frame->payload);
    }
    workCv_.notify_one();
}

bool FileTransferService::localInUse(const fs::path& local) const
{
    const std::string wanted = normal(local);
    for (const auto& s : snapshot_) {
        if (!isFinished(s.state) && normal(s.local) == wanted) {
            return true;
        }
    }
    for (const auto& [ticket, e] : expectations_) {
        if (!e.failed && !e.id && normal(e.local ? *e.local : e.predicted) == wanted) {
            return true;
        }
    }
    return false;
}

std::optional<TransactionId> FileTransferService::put(const fs::path& local, const std::string& remote, int cls,
                                                      std::string& error)
{
    if (!running_) {
        error = "file transfer is not running";
        return std::nullopt;
    }
    auto request = std::make_shared<Request>();
    request->kind = Request::Kind::Put;
    request->local = local;
    request->remote = remote;
    request->cls = cls;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cls == 2 && pipeline_.isDryRun()) {
            error = "class 2 needs a live target (dry-run: no answer would come back)";
            return std::nullopt;
        }
        if (cls == 2 && !receiving_) {
            error = "class 2 needs CF's answers, but telemetry is not received" +
                    (receivingWhy_.empty() ? std::string() : " (" + receivingWhy_ + ")");
            return std::nullopt;
        }
        if (localInUse(local)) {
            error = local.string() + " is already being transferred";
            return std::nullopt;
        }
        requests_.push_back(request);
        workCv_.notify_one();
        if (!doneCv_.wait_for(lock, kRequestTimeout, [&] { return request->done; })) {
            error = "file transfer did not answer";
            return std::nullopt;
        }
    }
    if (!request->ok) {
        error = request->error;
        return std::nullopt;
    }
    return request->id;
}

std::optional<std::size_t> FileTransferService::get(const std::string& remote,
                                                    const std::optional<fs::path>& local, int cls,
                                                    std::ostream& out)
{
    const auto refuse = [&out](const std::string& why) -> std::optional<std::size_t> {
        out << "error: file get: " << why << '\n';
        return std::nullopt;
    };
    if (!running_) {
        return refuse("file transfer is not running");
    }
    if (cls != 1 && cls != 2) {
        return refuse("class must be 1 or 2");
    }
    if (const std::string why = GsfcEngine::checkRemotePath(remote); !why.empty()) {
        return refuse(why);
    }
    if (pipeline_.isDryRun()) {
        return refuse("needs a live target (dry-run: nothing would arrive)");
    }
    const auto predicted = GsfcEngine::downlinkPath(downlinkDir_, remote);
    if (!predicted) {
        return refuse("cannot store '" + remote + "' under " + downlinkDir_.string());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!receiving_) {
            return refuse("telemetry is not received, so the file could not arrive"
                          + (receivingWhy_.empty() ? std::string() : " (" + receivingWhy_ + ")"));
        }
        if (localInUse(local ? *local : *predicted)) {
            return refuse((local ? *local : *predicted).string() + " is already being transferred");
        }
        for (const auto& s : snapshot_) {
            if (s.direction == Direction::Downlink && !isFinished(s.state) && s.remote == remote) {
                return refuse(remote + " is already being received");
            }
        }
    }
    const std::vector<std::string> args{cls == 2 ? "class2" : "class1", "0", "0", "keep",
                                        config_.localEntity, remote, remote};
    if (!commands_.execute("cf", "playback_file", args, out)) {
        return std::nullopt;   // the command service printed why
    }
    std::size_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ticket = nextTicket_++;
        Expectation e;
        e.ticket = ticket;
        e.remote = remote;
        if (local) {
            e.local = fs::path(normal(*local));
        }
        e.predicted = *predicted;
        e.cls = cls;
        e.created = TransferClock::now();
        e.datagramsAtStart = datagrams_;
        expectations_.emplace(ticket, std::move(e));
    }
    out << "waiting for CF to start " << remote << " …\n";
    workCv_.notify_one();
    return ticket;
}

bool FileTransferService::control(RequestKind kind, const std::string& idOrAll, std::string& error)
{
    if (!running_) {
        error = "file transfer is not running";
        return false;
    }
    if (iequals(idOrAll, "all") && (kind == RequestKind::Cancel || kind == RequestKind::Abandon)) {
        std::lock_guard<std::mutex> lock(mutex_);   // forget gets CF has not started
        for (auto it = expectations_.begin(); it != expectations_.end();) {
            it = !it->second.id && !it->second.failed ? expectations_.erase(it) : std::next(it);
        }
    }
    auto request = std::make_shared<Request>();
    request->kind = Request::Kind::Control;
    request->control = kind;
    request->target = idOrAll;
    std::unique_lock<std::mutex> lock(mutex_);
    requests_.push_back(request);
    workCv_.notify_one();
    if (!doneCv_.wait_for(lock, kRequestTimeout, [&] { return request->done; })) {
        error = "file transfer did not answer";
        return false;
    }
    error = request->error;
    return request->ok;
}

std::optional<TransferStatus> FileTransferService::findLocked(const TransactionId& id) const
{
    for (const auto& s : snapshot_) {
        if (s.id == id && s.state != State::Waiting) {
            return s;
        }
    }
    return std::nullopt;
}

FileTransferService::WaitResult FileTransferService::waitFor(const TransactionId& id, const Progress& progress)
{
    const auto noProgressLimit = std::chrono::seconds(config_.inactivityTimeout + 10);
    const auto begin = Steady::now();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!running_) {
            return {findLocked(id), "file transfer stopped"};
        }
        const auto s = findLocked(id);
        if (!s) {
            if (Steady::now() - begin > std::chrono::seconds(5)) {
                return {std::nullopt, "unknown transfer " + id.toString()};
            }
        } else {
            if (isFinished(s->state)) {
                return {s, ""};
            }
            if (s->state != State::Suspended && TransferClock::now() - s->lastProgress > noProgressLimit) {
                return {s, "no progress for " + std::to_string(noProgressLimit.count()) + " s"};
            }
            if (progress) {
                lock.unlock();
                progress(*s);
                lock.lock();
            }
        }
        doneCv_.wait_for(lock, std::chrono::milliseconds(200));
    }
}

FileTransferService::WaitResult FileTransferService::waitForGet(std::size_t ticket, const Progress& progress)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        const auto it = expectations_.find(ticket);
        if (it == expectations_.end()) {
            return {std::nullopt, "the request was cancelled"};
        }
        if (it->second.failed) {
            return {it->second.failed, ""};
        }
        if (it->second.id) {
            const TransactionId id = *it->second.id;
            lock.unlock();
            return waitFor(id, progress);
        }
        if (!running_) {
            return {std::nullopt, "file transfer stopped"};
        }
        if (progress) {
            const TransferStatus waiting = waitingStatus(it->second);
            lock.unlock();
            progress(waiting);
            lock.lock();
        }
        doneCv_.wait_for(lock, std::chrono::milliseconds(200));
    }
}

std::vector<TransferStatus> FileTransferService::transfers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

FileTransferService::Counts FileTransferService::activeCounts() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Counts c;
    for (const auto& s : snapshot_) {
        if (s.state == State::Waiting) {
            ++c.waiting;
        } else if (!isFinished(s.state)) {
            ++(s.direction == Direction::Uplink ? c.up : c.down);
        }
    }
    return c;
}

std::vector<std::string> FileTransferService::takeNotices()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out(notices_.begin(), notices_.end());
    notices_.clear();
    return out;
}

std::vector<std::string> FileTransferService::diagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {diagnostics_.begin(), diagnostics_.end()};
}

} // namespace mcs
