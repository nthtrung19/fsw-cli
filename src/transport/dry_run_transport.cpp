#include "transport/dry_run_transport.hpp"

namespace fswcli {

void DryRunTransport::send(const Bytes& /*data*/)
{
    ++count_;
}

} // namespace fswcli
