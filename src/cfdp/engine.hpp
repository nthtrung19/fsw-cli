#pragma once

#include "core/bytes.hpp"
#include "cfdp/pdu_info.hpp"
#include "cfdp/transfer.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mcs::cfdp {

enum class RequestKind { Cancel, Abandon, Suspend, Resume };

const char* toString(RequestKind kind);   // "CANCEL", ...

struct PutRequest {
    PutRequest(std::filesystem::path localFile, std::string remotePath, int serviceClass = 1,
               std::string metadataSourceName = {})
        : local(std::move(localFile)), remote(std::move(remotePath)), cls(serviceClass),
          sourceName(std::move(metadataSourceName))
    {
    }

    std::filesystem::path local;   // file to send
    std::string remote;            // destination path at the partner ("/cf/a.bin")
    int cls = 1;                   // 1 or 2
    // Source name announced in the Metadata. Empty: a short alias of the
    // local file name (mcs). A CF stand-in sets the FSW path ("/cf/a.bin").
    std::string sourceName;
};

enum class EngineEvent { Started, Finished };

// A CFDP entity (sender and receiver) without any I/O of its own: PDUs come
// in through givePdu() and go out through the PduOut callback.
//
// NOT thread-safe: every call, and every callback, happens on the thread
// that owns the engine (FileTransferService's worker).
class ICfdpEngine {
public:
    using PduOut = std::function<void(const Bytes& pdu, const PduInfo& info)>;
    using Notify = std::function<void(EngineEvent event, const TransferStatus& status)>;
    using Log = std::function<void(int level, const std::string& text)>;   // 0 debug .. 3 error

    virtual ~ICfdpEngine() = default;

    // Starts an uplink. The request is checked first; on failure nothing is
    // sent, `error` says why and nullopt is returned.
    virtual std::optional<TransactionId> put(const PutRequest& request, std::string& error) = 0;

    // One received PDU (CFDP bytes, no CCSDS header). False if it was rejected.
    virtual bool givePdu(const Bytes& pdu) = 0;

    // Lets every transaction act: timers, directives, and FileData up to the
    // pacing budget. Call often (every 20 ms).
    virtual void cycle() = 0;

    // Cancel/abandon/suspend/resume one transaction ("0.24_7") or "all".
    virtual bool request(RequestKind kind, const std::string& idOrAll, std::string& error) = 0;

    // Active transfers (oldest first), then finished ones (newest first).
    virtual std::vector<TransferStatus> transfers() const = 0;
};

} // namespace mcs::cfdp
