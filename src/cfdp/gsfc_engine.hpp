#pragma once

#include "cfdp/cfdp_config.hpp"
#include "cfdp/engine.hpp"

#include <memory>

namespace mcs::cfdp {

// ICfdpEngine on top of the GSFC CFDP library that CF 2.2.1 runs
// (external/cfdp-gsfc), so mcs and CF speak exactly the same CFDP.
//
// The library keeps its state in globals: only one GsfcEngine may exist at a
// time (the constructor throws otherwise), and it must be used from one
// thread (checked in debug builds).
//
// Files:
//   uplink    the PUT names the file by a short alias (its base name), so the
//             Metadata sent to CF never exceeds CF's 63-character limit; the
//             file callbacks map the alias back to the real path.
//   downlink  data goes to downlinkDir/.part/<n>.tmp; on success it is moved
//             to downlinkDir/<destination name from the Metadata>, never
//             overwriting a file ("name(1).ext") and never leaving downlinkDir.
class GsfcEngine final : public ICfdpEngine {
public:
    // Throws ConfigError for bad entity IDs, std::logic_error if another
    // engine exists.
    GsfcEngine(Config config, PduOut out, Notify notify, Log log);
    ~GsfcEngine() override;   // abandons active transfers

    GsfcEngine(const GsfcEngine&) = delete;
    GsfcEngine& operator=(const GsfcEngine&) = delete;

    std::optional<TransactionId> put(const PutRequest& request, std::string& error) override;
    bool givePdu(const Bytes& pdu) override;
    void cycle() override;
    bool request(RequestKind kind, const std::string& idOrAll, std::string& error) override;
    std::vector<TransferStatus> transfers() const override;

    // Checks a destination path on the FSW: leading '/', no spaces or control
    // characters, at most 63 characters (CF's OS_MAX_PATH_LEN). "" if fine.
    static std::string checkRemotePath(const std::string& remote);

    // Where a received file named `destination` (Metadata) is stored under
    // `downlinkDir`; nullopt if the name is empty or leaves the directory.
    static std::optional<std::filesystem::path> downlinkPath(const std::filesystem::path& downlinkDir,
                                                             const std::string& destination);

    struct Impl;   // public so the C callbacks in the .cpp can reach it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mcs::cfdp
