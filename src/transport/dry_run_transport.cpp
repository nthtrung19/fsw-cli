#include "transport/dry_run_transport.hpp"

namespace mcs {

void DryRunTransport::send(const Bytes& /*data*/)
{
    ++count_;
}

} // namespace mcs
