#pragma once

#include "transport/transport.hpp"

namespace mcs {

// Sends nothing. The service prints the packet instead.
class DryRunTransport : public ITransport {
public:
    void send(const Bytes& data) override;
    std::string describe() const override { return "dryrun (nothing sent)"; }
    bool isDryRun() const override { return true; }

    // Number of packets "sent" (useful in tests).
    std::size_t count() const { return count_; }

private:
    std::size_t count_ = 0;
};

} // namespace mcs
