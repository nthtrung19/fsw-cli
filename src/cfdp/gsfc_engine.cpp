#include "cfdp/gsfc_engine.hpp"

#include "cfdp/gsfc_bridge.h"
#include "core/errors.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace mcs::cfdp {

namespace fs = std::filesystem;
using Steady = std::chrono::steady_clock;

namespace {

constexpr std::size_t kMaxRemoteLength = 63;   // CF: OS_MAX_PATH_LEN 64, NUL included
constexpr std::size_t kMaxAliasLength = 40;
constexpr std::size_t kMaxHistory = 100;
constexpr int kMaxCyclesPerCall = 200;
constexpr auto kRateWindow = std::chrono::seconds(5);

// Same names as CF uses in its events (CONDITION_CODE, cfdp_data_structures.h).
const char* conditionName(CONDITION_CODE code)
{
    static const char* const names[] = {
        "NO_ERROR", "POSITIVE_ACK_LIMIT_REACHED", "KEEP_ALIVE_LIMIT_REACHED",
        "INVALID_TRANSMISSION_MODE", "FILESTORE_REJECTION", "FILE_CHECKSUM_FAILURE",
        "FILE_SIZE_ERROR", "NAK_LIMIT_REACHED", "INACTIVITY_DETECTED", "INVALID_FILE_STRUCTURE",
        "RESERVED_10", "RESERVED_11", "RESERVED_12", "RESERVED_13", "SUSPEND_REQUEST_RECEIVED",
        "CANCEL_REQUEST_RECEIVED"};
    const auto i = static_cast<std::size_t>(code);
    return i < sizeof names / sizeof names[0] ? names[i] : "?";
}

std::string idToString(const ID& id)
{
    return entityToString(id.value, id.length);
}

TransactionId toId(const TRANSACTION& trans)
{
    return TransactionId{idToString(trans.source_id), trans.number};
}

bool isSender(ROLE role)
{
    return role == CLASS_1_SENDER || role == CLASS_2_SENDER;
}

int roleClass(ROLE role)
{
    return role == CLASS_2_SENDER || role == CLASS_2_RECEIVER ? 2 : 1;
}

// "a b.bin" -> "a_b.bin": the PUT request is split on spaces.
std::string sanitize(const std::string& name)
{
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                     || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    if (out.empty() || out == "." || out == "..") {
        out = "file";
    }
    if (out.size() > kMaxAliasLength) {
        out = out.substr(out.size() - kMaxAliasLength);   // keep the extension
    }
    return out;
}

// "dir/a.bin" -> "dir/a(1).bin", "dir/a(2).bin", ... (first one that does not exist).
fs::path uniquePath(const fs::path& wanted)
{
    std::error_code ec;
    if (!fs::exists(wanted, ec)) {
        return wanted;
    }
    const std::string stem = wanted.stem().string();
    const std::string ext = wanted.extension().string();
    for (int n = 1; n < 10000; ++n) {
        fs::path candidate = wanted.parent_path() / (stem + "(" + std::to_string(n) + ")" + ext);
        if (!fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return wanted.parent_path() / (stem + "." + std::to_string(std::time(nullptr)) + ext);
}

} // namespace

const char* toString(RequestKind kind)
{
    switch (kind) {
    case RequestKind::Cancel: return "CANCEL";
    case RequestKind::Abandon: return "ABANDON";
    case RequestKind::Suspend: return "SUSPEND";
    case RequestKind::Resume: return "RESUME";
    }
    return "?";
}

// ---------------------------------------------------------------------------

struct GsfcEngine::Impl {
    struct Entry {
        TransferStatus status;
        TRANSACTION trans{};
        bool started = false;            // Started notified
        std::string alias;               // uplink: name used in the PUT and the Metadata
        std::string destName;            // downlink: destination name from the Metadata
        std::optional<fs::path> storedAs; // downlink: where the finished file was moved
        std::uint64_t highestSent = 0;   // uplink: end of the furthest FileData sent
        std::deque<std::pair<Steady::time_point, std::uint64_t>> samples;   // rate
    };
    struct PendingPut {
        std::string alias;
    };

    Config config;
    PduOut out;
    Notify notify;
    Log log;
    fs::path downlinkDir;
    fs::path partDir;

    std::map<TransactionId, Entry> active;
    std::deque<TransferStatus> history;      // newest first
    std::map<std::string, fs::path> aliases; // alias -> real ground file

    std::optional<PendingPut> pendingPut;
    std::optional<TransactionId> putStarted;
    std::string lastError;

    double tokens = 0;
    double capacity = 1;
    Steady::time_point lastRefill{};
    int emitted = 0;        // PDUs sent during the current library cycle
    bool refused = false;   // FileData held back by pacing during the current cycle
    unsigned tempCounter = 0;
    std::thread::id owner;

    void checkThread()
    {
        if (owner == std::thread::id{}) {
            owner = std::this_thread::get_id();
        }
        assert(owner == std::this_thread::get_id() && "CFDP engine used from two threads");
    }

    void message(int level, const std::string& text)
    {
        if (level >= MCS_CFDP_ERROR) {
            lastError = text;
        }
        if (log) {
            log(level, text);
        }
    }

    fs::path resolve(const char* name) const
    {
        const auto it = aliases.find(name);
        return it != aliases.end() ? it->second : fs::path(name);
    }

    std::string makeAlias(const fs::path& local) const
    {
        const std::string base = sanitize(local.filename().string());
        if (aliases.count(base) == 0) {
            return base;
        }
        const fs::path p(base);
        for (int n = 2;; ++n) {
            std::string candidate = p.stem().string() + "~" + std::to_string(n) + p.extension().string();
            if (aliases.count(candidate) == 0) {
                return candidate;
            }
        }
    }

    // ---- pacing -------------------------------------------------------------

    void refill()
    {
        const auto now = Steady::now();
        const double seconds = std::chrono::duration<double>(now - lastRefill).count();
        lastRefill = now;
        tokens = std::min(capacity, tokens + seconds * static_cast<double>(config.pdusPerSecond));
    }

    boolean outputReady(PDU_TYPE type)
    {
        if (type == FILE_DIR_PDU) {
            return YES;   // directives are never delayed (ACK/NAK/FIN of other transfers)
        }
        refill();
        if (tokens >= 1.0) {
            tokens -= 1.0;
            return YES;
        }
        refused = true;
        return NO;
    }

    // ---- PDUs -----------------------------------------------------------------

    static void count(PduCounts& c, PduKind kind)
    {
        switch (kind) {
        case PduKind::Metadata: ++c.metadata; break;
        case PduKind::FileData: ++c.fileData; break;
        case PduKind::Eof: ++c.eof; break;
        case PduKind::Ack: ++c.ack; break;
        case PduKind::Nak: ++c.nak; break;
        case PduKind::Fin: ++c.fin; break;
        default: break;
        }
    }

    void outputSend(const TRANSACTION& trans, const CFDP_DATA* pdu)
    {
        ++emitted;
        Bytes bytes(pdu->content, pdu->content + pdu->length);
        PduInfo info = inspectPdu(bytes).value_or(PduInfo{});
        if (const auto it = active.find(toId(trans)); it != active.end()) {
            Entry& e = it->second;
            count(e.status.sent, info.kind);
            if (info.kind == PduKind::FileData && e.status.direction == Direction::Uplink) {
                const std::uint64_t end = std::uint64_t{info.offset} + info.fileDataLength;
                if (end <= e.highestSent) {
                    e.status.bytesResent += info.fileDataLength;
                } else {
                    e.highestSent = end;
                }
            }
            e.status.lastProgress = TransferClock::now();
        }
        if (out) {
            out(bytes, info);
        }
    }

    // ---- indications ----------------------------------------------------------

    void applyMetadata(Entry& e, const TRANS_STATUS& s)
    {
        TransferStatus& st = e.status;
        st.remote = s.md.source_file_name;
        const auto path = downlinkPath(downlinkDir, s.md.dest_file_name);
        st.local = path ? path->string() : std::string("(refused name '") + s.md.dest_file_name + "')";
        st.fileSize = s.md.file_size;
        st.sizeKnown = true;
        e.destName = s.md.dest_file_name;
        if (!e.started) {
            e.started = true;
            if (notify) {
                notify(EngineEvent::Started, st);
            }
        }
    }

    Entry makeEntry(const TransactionId& id, const TRANS_STATUS& s)
    {
        Entry e;
        TransferStatus& st = e.status;
        st.id = id;
        st.cls = roleClass(s.role);
        st.start = TransferClock::now();
        st.lastProgress = st.start;
        st.state = State::Active;
        if (isSender(s.role)) {
            st.direction = Direction::Uplink;
            st.requested = true;
            e.alias = s.md.source_file_name;
            const auto it = aliases.find(e.alias);
            st.local = it != aliases.end() ? it->second.string() : e.alias;
            st.remote = s.md.dest_file_name;
            st.fileSize = s.md.file_size;
            st.sizeKnown = true;
            st.note = "sending metadata";
            if (pendingPut && pendingPut->alias == e.alias) {
                putStarted = id;
            }
        } else {
            st.direction = Direction::Downlink;
            st.requested = false;
            st.note = "receiving";
        }
        return e;
    }

    void finish(Entry& e, const TRANS_STATUS& s)
    {
        TransferStatus& st = e.status;
        const CONDITION_CODE cond = s.condition_code;
        st.condition = conditionName(cond);
        switch (s.final_status) {
        case FINAL_STATUS_SUCCESSFUL:
            st.state = State::Done;
            break;
        case FINAL_STATUS_CANCELLED:
            st.state = cond == CANCEL_REQUEST_RECEIVED ? State::Cancelled : State::Failed;
            break;
        case FINAL_STATUS_ABANDONED:
            st.state = cond == NO_ERROR ? State::Abandoned : State::Failed;
            break;
        case FINAL_STATUS_NO_METADATA:
            st.state = State::Failed;
            st.condition = "NO_METADATA";
            break;
        default:
            st.state = State::Failed;
            break;
        }
        st.end = TransferClock::now();
        st.note.clear();
        st.etaSeconds.reset();

        if (st.direction == Direction::Downlink) {
            // Note: TRANS_STATUS.temp_file_name is only meaningful once the
            // temp file was opened; a reused slot keeps the previous name.
            std::error_code ec;
            if (e.storedAs && st.state == State::Done) {
                st.local = e.storedAs->string();
            } else if (st.state != State::Done && config.saveIncomplete && s.has_md_been_received
                       && fs::exists(s.temp_file_name, ec)) {
                st.note = std::string("partial file kept: ") + s.temp_file_name;
            } else if (st.state != State::Done && s.has_md_been_received) {
                st.note = "partial file removed";
            }
        } else if (!e.alias.empty()) {
            aliases.erase(e.alias);
        }
        if (st.state == State::Done && st.sizeKnown) {
            st.bytesDone = st.fileSize;
        }
        const double seconds = std::chrono::duration<double>(st.end - st.start).count();
        st.rateBps = seconds > 0 ? static_cast<double>(st.bytesDone) / seconds : 0;

        history.push_front(st);
        if (history.size() > kMaxHistory) {
            history.pop_back();
        }
        if (notify) {
            notify(EngineEvent::Finished, st);
        }
    }

    void indication(INDICATION_TYPE type, const TRANS_STATUS& s)
    {
        const TransactionId id = toId(s.trans);
        if (s.is_this_trans_solely_for_ack_fin) {
            // The library re-creates a finished class 2 sender just to answer a late FIN.
            message(MCS_CFDP_DEBUG, "ACK(FIN) for finished transaction " + id.toString());
            return;
        }
        auto it = active.find(id);
        if (it == active.end()) {
            if (type == IND_MACHINE_DEALLOCATED) {
                return;
            }
            it = active.emplace(id, makeEntry(id, s)).first;
            Entry& fresh = it->second;
            if (fresh.status.direction == Direction::Uplink && !fresh.started) {
                fresh.started = true;
                if (notify) {
                    notify(EngineEvent::Started, fresh.status);
                }
            }
        }
        Entry& e = it->second;
        e.trans = s.trans;
        TransferStatus& st = e.status;
        if (st.direction == Direction::Downlink && s.has_md_been_received && !e.started) {
            applyMetadata(e, s);
        }

        switch (type) {
        case IND_METADATA_RECV:
            if (!e.started) {
                applyMetadata(e, s);
            }
            break;
        case IND_METADATA_SENT:
            st.note = "sending file data";
            break;
        case IND_EOF_SENT:
            st.note = st.cls == 2 ? "EOF sent, waiting for FIN" : "EOF sent";
            break;
        case IND_EOF_RECV:
            st.note = "EOF received";
            break;
        case IND_SUSPENDED:
            st.state = State::Suspended;
            st.note = "suspended";
            break;
        case IND_RESUMED:
            st.state = State::Active;
            st.note.clear();
            break;
        case IND_FAULT:
            st.condition = conditionName(s.condition_code);
            st.note = "fault: " + st.condition;
            break;
        case IND_ACK_TIMER_EXPIRED:
            st.note = "ACK timer expired, retrying";
            break;
        case IND_NAK_TIMER_EXPIRED:
            st.note = "NAK timer expired, retrying";
            break;
        case IND_INACTIVITY_TIMER_EXPIRED:
            st.note = "no PDU for " + std::to_string(config.inactivityTimeout) + " s";
            break;
        case IND_ABANDONED:
            st.note = "abandoned";
            break;
        case IND_MACHINE_DEALLOCATED:
            finish(e, s);
            active.erase(it);
            break;
        default:
            break;
        }
    }

    // ---- files ------------------------------------------------------------------

    CFDP_FILE* openFile(const char* name, const char* mode)
    {
        return std::fopen(resolve(name).c_str(), mode);
    }

    u_int_4 fileSize(const char* name)
    {
        std::error_code ec;
        const auto size = fs::file_size(resolve(name), ec);
        return ec ? 0u : static_cast<u_int_4>(std::min<std::uintmax_t>(size, 0xFFFFFFFFu));
    }

    int renameFile(const char* from, const char* to)
    {
        const auto target = downlinkPath(downlinkDir, to);
        if (!target) {
            message(MCS_CFDP_ERROR, std::string("refusing to store a received file as '") + to
                                        + "' (outside " + downlinkDir.string() + ")");
            return -1;
        }
        std::error_code ec;
        fs::create_directories(target->parent_path(), ec);
        const fs::path finalPath = uniquePath(*target);
        fs::rename(from, finalPath, ec);
        if (ec) {   // e.g. another file system: copy, then remove
            ec.clear();
            fs::copy_file(from, finalPath, ec);
            if (ec) {
                message(MCS_CFDP_ERROR, "cannot store " + finalPath.string() + ": " + ec.message());
                return -1;
            }
            fs::remove(from, ec);
        }
        // The library renames only at the end of a successful receive; tell
        // that transfer where its file went.
        for (auto& [id, e] : active) {
            if (e.status.direction == Direction::Downlink && !e.storedAs && e.destName == to) {
                e.storedAs = finalPath;
                break;
            }
        }
        return 0;
    }

    int removeFile(const char* name)
    {
        const fs::path p = fs::path(name).lexically_normal();
        if (p.parent_path() != partDir) {
            message(MCS_CFDP_WARNING, std::string("refusing to remove '") + name + "' (not a temp file)");
            return -1;
        }
        return std::remove(name);
    }

    char* tempName(char* buffer)
    {
        std::error_code ec;
        fs::create_directories(partDir, ec);
        const std::string name = (partDir / (std::to_string(::getpid()) + "-" + std::to_string(++tempCounter)
                                             + ".tmp")).string();
        std::snprintf(buffer, 1024, "%s", name.c_str());
        return buffer;
    }

    // ---- progress -----------------------------------------------------------------

    void refreshProgress()
    {
        const auto now = Steady::now();
        for (auto& [id, e] : active) {
            u_int_4 bytes = 0;
            u_int_4 size = 0;
            if (!mcs_cfdp_progress(e.trans, &bytes, &size)) {
                continue;
            }
            TransferStatus& st = e.status;
            if (bytes != st.bytesDone) {
                st.lastProgress = TransferClock::now();
            }
            st.bytesDone = bytes;
            if (size != 0) {
                st.fileSize = size;
                st.sizeKnown = true;
            }
            e.samples.emplace_back(now, st.bytesDone);
            while (!e.samples.empty() && now - e.samples.front().first > kRateWindow) {
                e.samples.pop_front();
            }
            const double span = std::chrono::duration<double>(now - e.samples.front().first).count();
            if (span >= 0.5) {
                st.rateBps = static_cast<double>(st.bytesDone - e.samples.front().second) / span;
            }
            if (st.rateBps > 0 && st.sizeKnown && st.fileSize >= st.bytesDone) {
                st.etaSeconds = static_cast<double>(st.fileSize - st.bytesDone) / st.rateBps;
            } else {
                st.etaSeconds.reset();
            }
        }
    }
};

// ---- C callbacks: the library calls these, they forward to the one engine ----------

namespace {

GsfcEngine::Impl* g_active = nullptr;

extern "C" {

static boolean cbOutputReady(PDU_TYPE type, TRANSACTION, ID)
{
    return g_active != nullptr ? g_active->outputReady(type) : NO;
}

static void cbOutputSend(TRANSACTION trans, ID, CFDP_DATA* pdu)
{
    if (g_active != nullptr) {
        g_active->outputSend(trans, pdu);
    }
}

static void cbIndication(INDICATION_TYPE type, TRANS_STATUS status)
{
    if (g_active != nullptr) {
        g_active->indication(type, status);
    }
}

static void cbMessage(int level, const char* text)
{
    if (g_active != nullptr) {
        g_active->message(level, text);
    }
}

static CFDP_FILE* cbOpenFile(const char* name, const char* mode)
{
    return g_active != nullptr ? g_active->openFile(name, mode) : nullptr;
}

static u_int_4 cbFileSize(const char* name)
{
    return g_active != nullptr ? g_active->fileSize(name) : 0u;
}

static int cbRename(const char* from, const char* to)
{
    return g_active != nullptr ? g_active->renameFile(from, to) : -1;
}

static int cbRemove(const char* name)
{
    return g_active != nullptr ? g_active->removeFile(name) : -1;
}

static char* cbTempName(char* buffer)
{
    return g_active != nullptr ? g_active->tempName(buffer) : buffer;
}

} // extern "C"

} // namespace

// ---------------------------------------------------------------------------

GsfcEngine::GsfcEngine(Config config, PduOut out, Notify notify, Log log)
    : impl_(std::make_unique<Impl>())
{
    if (g_active != nullptr) {
        throw std::logic_error("only one CFDP engine can exist in a process");
    }
    Impl& m = *impl_;
    m.checkThread();
    m.config = std::move(config);
    m.out = std::move(out);
    m.notify = std::move(notify);
    m.log = std::move(log);

    for (const auto& [what, value] : {std::pair<const char*, const std::string*>{"local", &m.config.localEntity},
                                      {"remote", &m.config.remoteEntity}}) {
        ID id{};
        if (!mcs_cfdp_id_from_string(value->c_str(), &id) || id.length != 2) {
            throw ConfigError(std::string("cfdp: ") + what + " entity '" + *value
                              + "' must be a 2-byte dotted-decimal ID such as 0.23");
        }
    }
    std::error_code ec;
    m.downlinkDir = fs::absolute(m.config.downlinkDir, ec).lexically_normal();
    m.partDir = m.downlinkDir / ".part";
    m.capacity = std::max(2.0, static_cast<double>(m.config.pdusPerSecond) / 20.0);
    m.tokens = m.capacity;
    m.lastRefill = Steady::now();

    g_active = &m;
    const mcs_cfdp_callbacks callbacks{cbOutputReady, cbOutputSend, cbIndication, cbMessage, cbOpenFile,
                                       cbFileSize,    cbRename,     cbRemove,     cbTempName};
    mcs_cfdp_register(&callbacks);

    try {
        const auto set = [&m](const char* param, const std::string& value) {
            if (!mcs_cfdp_set_mib(param, value.c_str())) {
                throw ConfigError(std::string("cfdp: cannot set ") + param + " to '" + value + "'"
                                  + (m.lastError.empty() ? "" : ": " + m.lastError));
            }
        };
        set("MY_ID", m.config.localEntity);
        set("ACK_TIMEOUT", std::to_string(m.config.ackTimeout));
        set("ACK_LIMIT", std::to_string(m.config.ackLimit));
        set("NAK_TIMEOUT", std::to_string(m.config.nakTimeout));
        set("NAK_LIMIT", std::to_string(m.config.nakLimit));
        set("INACTIVITY_TIMEOUT", std::to_string(m.config.inactivityTimeout));
        set("OUTGOING_FILE_CHUNK_SIZE", std::to_string(m.config.chunkSize));
        set("SAVE_INCOMPLETE_FILES", m.config.saveIncomplete ? "YES" : "NO");
    } catch (...) {
        g_active = nullptr;
        throw;
    }
    // Sequence numbers start at 1 in every process; start from the clock so
    // a restarted mcs does not reuse transaction IDs that CF has seen.
    mcs_cfdp_set_trans_seq_num(static_cast<u_int_4>(std::time(nullptr)));
}

GsfcEngine::~GsfcEngine()
{
    if (!impl_->active.empty()) {
        mcs_cfdp_request("ABANDON all");
    }
    g_active = nullptr;
}

std::string GsfcEngine::checkRemotePath(const std::string& remote)
{
    if (remote.empty() || remote[0] != '/') {
        return "remote path must start with '/' (e.g. /cf/file.bin)";
    }
    if (remote.size() > kMaxRemoteLength) {
        return "remote path is longer than " + std::to_string(kMaxRemoteLength) + " characters (CF limit)";
    }
    if (remote.back() == '/') {
        return "remote path must name a file, not a directory";
    }
    for (const char c : remote) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= ' ' || u == 0x7F) {
            return "remote path must not contain spaces or control characters";
        }
    }
    return "";
}

std::optional<fs::path> GsfcEngine::downlinkPath(const fs::path& downlinkDir, const std::string& destination)
{
    std::size_t first = destination.find_first_not_of('/');
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const fs::path raw(destination.substr(first));
    for (const auto& part : raw) {   // any "..", even one that would cancel out, is refused
        if (part == "..") {
            return std::nullopt;
        }
    }
    const fs::path relative = raw.lexically_normal();
    if (relative.empty() || relative.is_absolute()) {
        return std::nullopt;
    }
    if (relative.filename().empty() || relative.filename() == ".") {
        return std::nullopt;
    }
    return downlinkDir / relative;
}

std::optional<TransactionId> GsfcEngine::put(const PutRequest& request, std::string& error)
{
    Impl& m = *impl_;
    m.checkThread();
    if (request.cls != 1 && request.cls != 2) {
        error = "class must be 1 or 2";
        return std::nullopt;
    }
    if (std::string e = checkRemotePath(request.remote); !e.empty()) {
        error = e;
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::is_regular_file(request.local, ec)) {
        error = "no such file: " + request.local.string();
        return std::nullopt;
    }
    const auto size = fs::file_size(request.local, ec);
    if (ec || size > 0xFFFFFFFFu) {
        error = ec ? "cannot read " + request.local.string() : "file is larger than 4 GiB (CFDP limit)";
        return std::nullopt;
    }
    if (std::FILE* f = std::fopen(request.local.c_str(), "rb"); f != nullptr) {
        std::fclose(f);
    } else {
        error = "cannot read " + request.local.string() + ": " + std::strerror(errno);
        return std::nullopt;
    }

    std::string alias = m.makeAlias(request.local);
    if (!request.sourceName.empty()) {
        if (request.sourceName.find_first_of(" \t\r\n") != std::string::npos
            || request.sourceName.size() > kMaxRemoteLength) {
            error = "source name must have no spaces and at most " + std::to_string(kMaxRemoteLength) + " characters";
            return std::nullopt;
        }
        if (m.aliases.count(request.sourceName) != 0) {
            error = request.sourceName + " is already being sent";
            return std::nullopt;
        }
        alias = request.sourceName;
    }
    m.aliases[alias] = fs::absolute(request.local, ec);
    std::string text = "PUT ";
    if (request.cls == 1) {
        text += "-class1 ";
    }
    text += alias + " " + m.config.remoteEntity + " " + request.remote;

    m.pendingPut = Impl::PendingPut{alias};
    m.putStarted.reset();
    m.lastError.clear();
    const bool ok = mcs_cfdp_request(text.c_str());
    m.pendingPut.reset();
    if (!ok || !m.putStarted) {
        m.aliases.erase(alias);
        error = "the CFDP engine refused the transfer" + (m.lastError.empty() ? std::string() : ": " + m.lastError);
        return std::nullopt;
    }
    return m.putStarted;
}

bool GsfcEngine::givePdu(const Bytes& pdu)
{
    Impl& m = *impl_;
    m.checkThread();
    if (pdu.empty() || pdu.size() > MAX_DATA_LENGTH) {
        return false;
    }
    const auto info = inspectPdu(pdu);
    // Count before handing it over: the PDU may finish (and remove) the transaction.
    bool counted = false;
    if (info) {
        if (const auto it = m.active.find(info->id); it != m.active.end()) {
            Impl::count(it->second.status.received, info->kind);
            it->second.status.lastProgress = TransferClock::now();
            counted = true;
        }
    }
    const bool ok = mcs_cfdp_give_pdu(pdu.data(), static_cast<u_int_4>(pdu.size())) != NO;
    if (info && !counted) {   // the PDU started a new transaction
        if (const auto it = m.active.find(info->id); it != m.active.end()) {
            Impl::count(it->second.status.received, info->kind);
        }
    }
    return ok;
}

void GsfcEngine::cycle()
{
    Impl& m = *impl_;
    m.checkThread();
    // One library cycle sends at most one FileData PDU per transaction, so
    // cycle until nothing more goes out or pacing holds data back.
    for (int i = 0; i < kMaxCyclesPerCall; ++i) {
        m.emitted = 0;
        m.refused = false;
        mcs_cfdp_cycle();
        if (m.emitted == 0 || m.refused) {
            break;
        }
    }
    m.refreshProgress();
}

bool GsfcEngine::request(RequestKind kind, const std::string& idOrAll, std::string& error)
{
    Impl& m = *impl_;
    m.checkThread();
    std::string target = "all";
    if (!iequals(idOrAll, "all")) {
        const auto id = parseTransactionId(idOrAll);
        if (!id) {
            error = "'" + idOrAll + "' is not a transaction ID (e.g. 0.24_7)";
            return false;
        }
        if (m.active.count(*id) == 0) {
            error = "no active transfer " + id->toString();
            return false;
        }
        target = id->toString();
    }
    const std::string text = std::string(toString(kind)) + " " + target;
    m.lastError.clear();
    if (!mcs_cfdp_request(text.c_str())) {
        error = "the CFDP engine refused '" + text + "'" + (m.lastError.empty() ? "" : ": " + m.lastError);
        return false;
    }
    return true;
}

std::vector<TransferStatus> GsfcEngine::transfers() const
{
    std::vector<TransferStatus> list;
    for (const auto& [id, e] : impl_->active) {
        list.push_back(e.status);
    }
    std::sort(list.begin(), list.end(),
              [](const TransferStatus& a, const TransferStatus& b) { return a.start < b.start; });
    list.insert(list.end(), impl_->history.begin(), impl_->history.end());
    return list;
}

} // namespace mcs::cfdp
